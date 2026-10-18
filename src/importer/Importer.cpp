#include "importer/Importer.hpp"

#include "cdc/Cdc.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef OU_VERSION
#define OU_VERSION "0.0.0"
#endif

namespace ou::importer {
namespace {

using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

template <class T>
T le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T>
std::optional<T> at(std::span<const uint8_t> d, size_t off) {
    if (off > d.size() || d.size() - off < sizeof(T)) return std::nullopt;
    return le<T>(d.data() + off);
}

std::string hex32(uint32_t v) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%08x", v);
    return b;
}
std::string hexBytes(std::span<const uint8_t> d) {
    static constexpr char k[] = "0123456789abcdef";
    std::string s;
    for (uint8_t c : d) s += {k[c >> 4], k[c & 15]};
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

template <class F>
void parallelFor(size_t n, unsigned threads, F&& f) {
    std::atomic<size_t> next{0};
    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < std::max(1u, threads); ++t)
        pool.emplace_back([&, t] {
            for (size_t i; (i = next++) < n;) f(i, t);
        });
}

// ---- file hashing ------------------------------------------------------------------------
struct Digests { std::string sha256, md5; };
Digests hashFile(const fs::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + p.string());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> s(EVP_MD_CTX_new(), EVP_MD_CTX_free),
        m(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(s.get(), EVP_sha256(), nullptr);
    EVP_DigestInit_ex(m.get(), EVP_md5(), nullptr);
    std::vector<uint8_t> buf(4 << 20);
    for (ssize_t n; (n = ::read(fd, buf.data(), buf.size())) != 0;) {
        if (n < 0) { ::close(fd); throw std::runtime_error("read error " + p.string()); }
        EVP_DigestUpdate(s.get(), buf.data(), size_t(n));
        EVP_DigestUpdate(m.get(), buf.data(), size_t(n));
    }
    ::close(fd);
    uint8_t out[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    Digests d;
    EVP_DigestFinal_ex(s.get(), out, &len); d.sha256 = hexBytes({out, len});
    EVP_DigestFinal_ex(m.get(), out, &len); d.md5 = hexBytes({out, len});
    return d;
}

std::vector<uint8_t> readFile(const fs::path& p, size_t limit = SIZE_MAX) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::vector<uint8_t> v(std::min<uint64_t>(fs::file_size(p), limit));
    f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size()));
    return v;
}

std::string detectFileType(const fs::path& rel, std::span<const uint8_t> head) {
    auto ext = lower(rel.extension().string());
    auto starts = [&](std::string_view m) { return head.size() >= m.size() && std::memcmp(head.data(), m.data(), m.size()) == 0; };
    if (ext.size() == 4 && ext[1] >= '0' && ext[1] <= '9' && ext[2] >= '0' && ext[3] >= '0' && ext[2] <= '9' && ext[3] <= '9')
        return lower(rel.stem().string()) == "patch" ? "bigfile-patch" : ext == ".000" ? "bigfile-index+data" : "bigfile-data";
    if (starts("MZ")) return ext == ".exe" ? "pe-exe" : "pe-dll";
    if (starts("BIK")) return "bink-video";
    if (starts("{\\rtf")) return "rtf";
    if (starts("PK\x03\x04")) return ext == ".hashdb" ? "gog-hashdb(zip)" : "zip";
    if (head.size() >= 4 && head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0) return "ico";
    if (starts("Inno Setup Unins")) return "inno-uninstall-log";
    if (ext == ".lnk") return "windows-shortcut";
    if (ext == ".ini" || ext == ".txt" || ext == ".log" || ext == ".info" || ext == ".script" || ext == ".msg") return "text";
    return "unknown";
}

// ---- bigfile part access via pread ---------------------------------------------------------
struct Parts {
    std::vector<int> fds;
    std::vector<uint64_t> sizes;
    uint64_t align;
    Parts(const cdc::Bigfile& bf, uint32_t count, uint64_t alignment) : align(alignment) {
        for (uint32_t i = 0; i < count; ++i) {
            auto p = bf.partPath(i);
            int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
            fds.push_back(fd);
            sizes.push_back(fd >= 0 ? fs::file_size(p) : 0);
        }
    }
    ~Parts() { for (int fd : fds) if (fd >= 0) ::close(fd); }
    Parts(const Parts&) = delete;
    Parts& operator=(const Parts&) = delete;

    struct Loc { uint32_t part; uint64_t offset; };
    Loc locate(const cdc::BigfileRecord& r) const {
        uint64_t pos = uint64_t(r.sector) << 11;
        return {uint32_t(pos / align), pos % align};
    }
    // Empty string on success, else the reason the record can't be read.
    std::string check(const cdc::BigfileRecord& r) const {
        auto [part, off] = locate(r);
        if (part >= fds.size() || fds[part] < 0) return "part " + std::to_string(part) + " missing";
        if (off + r.size > sizes[part])
            return "record extends past end of part " + std::to_string(part) + " (ends at " + std::to_string(off + r.size) +
                   ", part size " + std::to_string(sizes[part]) + ")";
        return {};
    }
    std::vector<uint8_t> read(const cdc::BigfileRecord& r, uint64_t from, uint64_t len) const {
        auto [part, off] = locate(r);
        len = std::min<uint64_t>(len, r.size > from ? r.size - from : 0);
        std::vector<uint8_t> v(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::pread(fds[part], v.data() + got, len - got, off_t(off + from + got));
            if (n <= 0) throw cdc::FormatError("read error in part " + std::to_string(part));
            got += size_t(n);
        }
        return v;
    }
};

// ---- record analysis ---------------------------------------------------------------------
struct DrmSummary {
    uint32_t version = 0, sections = 0, objDepBytes = 0, unitDepBytes = 0;
    uint64_t decompressedSize = 0;
    std::map<std::string, uint32_t> types;
    std::vector<std::string> objDeps, unitDeps;
};

struct RecInfo {
    std::string kind = "unknown", magic, error;
    std::optional<DrmSummary> drm;
    std::vector<std::string> music; // music names referenced by Mus! scripts
};

std::vector<std::string> splitNul(std::span<const uint8_t> b) {
    std::vector<std::string> out;
    size_t s = 0;
    for (size_t i = 0; i <= b.size(); ++i)
        if (i == b.size() || b[i] == 0) {
            if (i > s) out.emplace_back(reinterpret_cast<const char*>(b.data() + s), i - s);
            s = i + 1;
        }
    return out;
}

// Parses the DRM header (section table + dependency names). Returns the header byte count needed
// if `d` is too short, 0 on success. Throws FormatError on implausible values.
size_t drmHeader(std::span<const uint8_t> d, DrmSummary& s) {
    if (d.size() < 24) return 24;
    s.version = le<uint32_t>(d.data());
    if (s.version != cdc::kDrmVersion) throw cdc::FormatError("DRM version " + std::to_string(s.version));
    s.objDepBytes = le<uint32_t>(d.data() + 4);
    s.unitDepBytes = le<uint32_t>(d.data() + 8);
    s.sections = le<uint32_t>(d.data() + 20);
    if (s.sections > 100000 || s.objDepBytes > (1u << 24) || s.unitDepBytes > (1u << 24))
        throw cdc::FormatError("implausible DRM header");
    size_t need = 24 + size_t(s.sections) * 20 + s.objDepBytes + s.unitDepBytes;
    if (d.size() < need) return need;
    s.types.clear();
    for (uint32_t i = 0; i < s.sections; ++i)
        ++s.types[std::string(cdc::sectionTypeName(cdc::SectionType(d[24 + i * 20 + 4])))];
    size_t p = 24 + size_t(s.sections) * 20;
    s.objDeps = splitNul(d.subspan(p, s.objDepBytes));
    s.unitDeps = splitNul(d.subspan(p + s.objDepBytes, s.unitDepBytes));
    return 0;
}

// Decompresses only as many CDRM blocks as the DRM header needs.
DrmSummary cdrmHeaderOnly(const Parts& parts, const cdc::BigfileRecord& r) {
    auto head = parts.read(r, 0, 16);
    const uint32_t nb = le<uint32_t>(head.data() + 8);
    if (nb == 0 || nb > (r.size - 16) / 8) throw cdc::FormatError("CDRM block table truncated");
    auto table = parts.read(r, 16, uint64_t(nb) * 8);
    DrmSummary s;
    for (uint32_t i = 0; i < nb; ++i) s.decompressedSize += le<uint32_t>(table.data() + 8 * i) >> 8;
    std::vector<uint8_t> out;
    uint64_t pos = (16 + uint64_t(nb) * 8 + 15) & ~uint64_t(15);
    for (uint32_t i = 0; i < nb; ++i) {
        const uint32_t a = le<uint32_t>(table.data() + 8 * i), cs = le<uint32_t>(table.data() + 8 * i + 4);
        const uint32_t type = a & 0xFF, us = a >> 8;
        if (pos + cs > r.size) throw cdc::FormatError("CDRM block " + std::to_string(i) + " truncated");
        auto c = parts.read(r, pos, cs);
        const size_t dst = out.size();
        out.resize(dst + us);
        if (type == 1) {
            if (cs < us) throw cdc::FormatError("CDRM stored block smaller than declared");
            std::memcpy(out.data() + dst, c.data(), us);
        } else if (type == 2) {
            uLongf len = us;
            if (uncompress(out.data() + dst, &len, c.data(), cs) != Z_OK || len != us)
                throw cdc::FormatError("CDRM block " + std::to_string(i) + " zlib error");
        } else if (type != 0 || us != 0) {
            throw cdc::FormatError("CDRM block type " + std::to_string(type) + " unsupported");
        }
        pos = (pos + cs + 15) & ~uint64_t(15);
        if (drmHeader(out, s) == 0) return s;
    }
    throw cdc::FormatError("DRM header exceeds CDRM payload");
}

DrmSummary summarize(const cdc::Drm& drm) {
    DrmSummary s;
    s.version = drm.version;
    s.sections = uint32_t(drm.sections.size());
    s.decompressedSize = drm.bytes.size();
    drmHeader(drm.bytes, s); // header fields + dependency split (already validated by parseDrm)
    return s;
}

// NUL-delimited identifier-like strings. Requiring NUL on both sides keeps random byte runs out
// (0 false hits against the reference name list vs. 3 with a plain printable-run regex).
void harvest(std::span<const uint8_t> d, std::unordered_set<std::string>& out) {
    auto ok = [](uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    for (size_t i = 1; i < d.size();) {
        if (d[i - 1] != 0 || !ok(d[i])) { ++i; continue; }
        size_t j = i;
        while (j < d.size() && ok(d[j])) ++j;
        if (j < d.size() && d[j] == 0 && j - i >= 3 && j - i <= 64)
            out.insert(lower(std::string(reinterpret_cast<const char*>(d.data() + i), j - i)));
        i = j + 1;
    }
}

bool plausibleMul(std::span<const uint8_t> h, uint32_t size) {
    // MultiplexStream header (research/Templates/tr7ae_mul.bt): i32 hertz, startLoop, endLoop, channelCount.
    // Observed hertz values cluster around 32000/44100/48000 with small per-file offsets.
    if (h.size() < 16 || size < 2048) return false;
    auto hz = le<uint32_t>(h.data()), ch = le<uint32_t>(h.data() + 12);
    return hz >= 8000 && hz <= 96000 && ch >= 1 && ch <= 12;
}

RecInfo analyze(const Parts& parts, const cdc::BigfileRecord& r, bool deep, std::unordered_set<std::string>* strings) {
    RecInfo ri;
    if (auto e = parts.check(r); !e.empty()) { ri.kind = "error"; ri.error = e; return ri; }
    try {
        auto head = parts.read(r, 0, 64);
        ri.magic = hexBytes(std::span(head).first(std::min<size_t>(4, head.size())));
        auto is = [&](const char* m) { return head.size() >= 4 && std::memcmp(head.data(), m, 4) == 0; };
        if (cdc::isCdrm(head)) {
            ri.kind = "CDRM";
            if (deep) {
                auto drm = cdc::parseDrm(parts.read(r, 0, r.size));
                ri.drm = summarize(drm);
                if (strings) harvest(drm.bytes, *strings);
            } else {
                ri.drm = cdrmHeaderOnly(parts, r);
            }
        } else if (head.size() >= 24 && le<uint32_t>(head.data()) == cdc::kDrmVersion && le<uint32_t>(head.data() + 12) == 0) {
            ri.kind = "DRM";
            auto drm = cdc::parseDrm(parts.read(r, 0, r.size));
            ri.drm = summarize(drm);
            if (deep && strings) harvest(drm.bytes, *strings);
        } else if (is("FSB4")) {
            ri.kind = "FSB4";
        } else if (is("!WAR")) {
            ri.kind = "!WAR";
        } else if (is("\x89PNG")) {
            ri.kind = "PNG";
        } else if (is("Mus!")) {
            ri.kind = "Mus!";
            // Music scripts name their control file "\tr8\audio\music\<name>\<name>ctl.txt".
            auto all = parts.read(r, 0, std::min<uint32_t>(r.size, 1 << 20));
            static constexpr std::string_view key = "\\tr8\\audio\\music\\";
            std::string_view sv(reinterpret_cast<const char*>(all.data()), all.size());
            for (size_t p = 0; (p = sv.find(key, p)) != sv.npos; p += key.size()) {
                auto e = sv.find('\\', p + key.size());
                if (e != sv.npos && e - p - key.size() < 128) ri.music.emplace_back(lower(std::string(sv.substr(p + key.size(), e - p - key.size()))));
            }
        } else if (plausibleMul(head, r.size)) {
            ri.kind = "MUL?";
        }
    } catch (const std::exception& e) {
        ri.error = e.what();
    }
    return ri;
}

// ---- name recovery -----------------------------------------------------------------------
struct NameTable {
    std::unordered_map<uint32_t, std::string> kind;            // hash -> kind of first record
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> resolved; // hash -> (name, source)
    std::map<std::string, uint32_t> bySource;
    uint64_t tried = 0, kindRejected = 0;

    static bool compatible(std::string_view ext, std::string_view k) {
        if (k == "error") return true;
        if (ext == ".drm") return k == "CDRM" || k == "DRM";
        if (ext == ".raw") return k == "!WAR";
        if (ext == ".mus") return k == "Mus!";
        if (ext == ".sam") return k == "FSB4";
        if (ext == ".mul") return k == "MUL?";
        if (ext == ".png") return k == "PNG";
        return true;
    }
    bool add(const std::string& name, const std::string& source) {
        ++tried;
        const uint32_t h = cdc::pathHash(name);
        auto k = kind.find(h);
        if (k == kind.end() || resolved.contains(h)) return false;
        auto dot = name.rfind('.');
        if (!compatible(dot == std::string::npos ? "" : lower(name.substr(dot)), k->second)) { ++kindRejected; return false; }
        resolved.emplace(h, std::pair{name, source});
        ++bySource[source];
        return true;
    }
};

// ---- PE / version resource ---------------------------------------------------------------
std::string utf16le(std::span<const uint8_t> d, size_t off, size_t maxChars, size_t* endOff = nullptr) {
    std::string s;
    size_t i = off;
    for (size_t n = 0; n < maxChars && i + 1 < d.size(); ++n, i += 2) {
        uint16_t c = le<uint16_t>(d.data() + i);
        if (c == 0) { i += 2; break; }
        if (c < 0x80) s += char(c);
        else if (c < 0x800) s += {char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
        else s += {char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
    }
    if (endOff) *endOff = i;
    return s;
}

void walkVersion(std::span<const uint8_t> d, size_t off, size_t end, int depth, bool inStrings, PeInfo& pe) {
    while (off + 6 <= end && depth < 6) {
        const uint16_t len = le<uint16_t>(d.data() + off), vlen = le<uint16_t>(d.data() + off + 2), type = le<uint16_t>(d.data() + off + 4);
        if (len < 6 || off + len > end) return;
        size_t p;
        std::string key = utf16le(d, off + 6, 64, &p);
        p = (p + 3) & ~size_t(3);
        if (depth == 0 && vlen >= 52 && p + 52 <= d.size() && le<uint32_t>(d.data() + p) == 0xFEEF04BD) {
            auto ver = [&](size_t o) {
                uint32_t ms = le<uint32_t>(d.data() + p + o), ls = le<uint32_t>(d.data() + p + o + 4);
                return std::to_string(ms >> 16) + "." + std::to_string(ms & 0xFFFF) + "." + std::to_string(ls >> 16) + "." + std::to_string(ls & 0xFFFF);
            };
            pe.fixedFileVersion = ver(8);
            pe.fixedProductVersion = ver(16);
        }
        if (inStrings && depth == 3) pe.strings.emplace_back(key, type == 1 ? utf16le(d, p, vlen) : std::string());
        const size_t valBytes = type == 1 ? size_t(vlen) * 2 : vlen;
        const size_t child = (p + valBytes + 3) & ~size_t(3);
        walkVersion(d, child, off + len, depth + 1, inStrings || key == "StringFileInfo", pe);
        off = (off + len + 3) & ~size_t(3);
    }
}

// ---- minimal ZIP (single deflated/stored member) -------------------------------------------
std::vector<uint8_t> unzipFirst(const std::vector<uint8_t>& z) {
    if (z.size() < 22) throw std::runtime_error("zip too small");
    size_t eocd = z.size() - 22;
    while (eocd > 0 && le<uint32_t>(z.data() + eocd) != 0x06054b50) --eocd;
    if (le<uint32_t>(z.data() + eocd) != 0x06054b50) throw std::runtime_error("zip: no end of central directory");
    const uint32_t cd = le<uint32_t>(z.data() + eocd + 16);
    if (cd + 46 > z.size() || le<uint32_t>(z.data() + cd) != 0x02014b50) throw std::runtime_error("zip: bad central directory");
    const uint16_t method = le<uint16_t>(z.data() + cd + 10);
    const uint32_t csize = le<uint32_t>(z.data() + cd + 20), usize = le<uint32_t>(z.data() + cd + 24),
                   lho = le<uint32_t>(z.data() + cd + 42);
    if (lho + 30 > z.size() || le<uint32_t>(z.data() + lho) != 0x04034b50) throw std::runtime_error("zip: bad local header");
    const size_t data = lho + 30 + le<uint16_t>(z.data() + lho + 26) + le<uint16_t>(z.data() + lho + 28);
    if (data > z.size() || z.size() - data < csize || usize > (64u << 20)) throw std::runtime_error("zip: member out of range");
    std::vector<uint8_t> out(usize);
    if (method == 0) {
        if (csize != usize) throw std::runtime_error("zip: stored size mismatch");
        std::memcpy(out.data(), z.data() + data, usize);
        return out;
    }
    if (method != 8) throw std::runtime_error("zip: unsupported method " + std::to_string(method));
    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK) throw std::runtime_error("zip: inflateInit failed");
    s.next_in = const_cast<Bytef*>(z.data() + data);
    s.avail_in = csize;
    s.next_out = out.data();
    s.avail_out = usize;
    int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (rc != Z_STREAM_END || s.total_out != usize) throw std::runtime_error("zip: inflate failed");
    return out;
}

// The GOG hashdb stores UTF-8 ("Léeme.rtf" is C3 A9); anything that isn't valid UTF-8 is taken as Latin-1.
std::string toUtf8(std::string_view s) {
    auto valid = [&] {
        for (size_t i = 0; i < s.size();) {
            const auto c = uint8_t(s[i]);
            const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (n == 0 || i + n > s.size()) return false;
            for (size_t k = 1; k < n; ++k)
                if ((uint8_t(s[i + k]) >> 6) != 2) return false;
            i += n;
        }
        return true;
    };
    if (valid()) return std::string(s);
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else o += {char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
    }
    return o;
}

// Known retail executables (sha256 -> label).
struct KnownBuild { std::string_view sha256, label; };
constexpr KnownBuild kKnownBuilds[] = {
    {"1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011", "GOG 1900745791 (buildId 58082205556264096)"},
};

std::string iso8601(uint32_t t) {
    std::time_t tt = t;
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::string machineName(uint16_t m) {
    switch (m) {
    case 0x14c: return "i386";
    case 0x8664: return "amd64";
    case 0x1c0: return "arm";
    case 0xaa64: return "arm64";
    default: return hex32(m);
    }
}

// Install-relative name generation. Templates come from tru.exe strings (verified present in the
// GOG build): "%s%s.drm", "%sStreamGroups\\%s.drm", "%simage\\%s.raw", "%s%s.mul" with "%scinstream\\",
// "%smusic\\%s\\%s", "%slocal\\locals.bin", "%spadshock\\%s" + "padshocklib.tfb",
// "\\tr8\\%s\\objectlist.txt", "\\tr8\\%s\\objlist.dat", "\\tr8\\local\\localization\\movies\\%s.sch".
// The "%s" prefix expands to "pc-w\\" (proven by "pc-w\\lara.drm").
constexpr std::string_view kPlat = "pc-w";

void tryPool(NameTable& nt, const std::vector<std::string>& pool, const std::string& src) {
    const std::string p = std::string(kPlat) + "\\";
    for (const auto& n : pool) {
        nt.add(p + n + ".drm", src);
        nt.add(p + "streamgroups\\" + n + ".drm", src);
        nt.add(p + "image\\" + n + ".raw", src);
        nt.add(p + "cinstream\\" + n + ".mul", src);
    }
}

std::vector<std::string> exeStrings(const std::vector<uint8_t>& exe) {
    std::vector<std::string> out;
    size_t s = 0;
    for (size_t i = 0; i <= exe.size(); ++i) {
        if (i == exe.size() || exe[i] < 0x20 || exe[i] > 0x7E) {
            if (i - s >= 4) out.emplace_back(reinterpret_cast<const char*>(exe.data() + s), i - s);
            s = i + 1;
        }
    }
    return out;
}

struct ArchiveCtx {
    std::string label;
    fs::path first;
    std::optional<cdc::Bigfile> bf;
    std::unique_ptr<Parts> parts;
    std::vector<RecInfo> info;
    std::string error;
    uint32_t partsReferenced = 0;
};

// Reads the whole record (small text/list files) from the first archive that resolves `name`.
std::optional<std::vector<uint8_t>> readNamed(const std::vector<std::unique_ptr<ArchiveCtx>>& archives, std::string_view name) {
    for (auto it = archives.rbegin(); it != archives.rend(); ++it) { // patch first
        const auto& a = **it;
        if (!a.bf) continue;
        for (const auto& r : a.bf->find(cdc::pathHash(name)))
            if (a.parts->check(r).empty()) return a.parts->read(r, 0, r.size);
    }
    return std::nullopt;
}

std::vector<std::string> lines(const std::vector<uint8_t>& d) {
    std::vector<std::string> out;
    std::string cur;
    for (uint8_t c : d) {
        if (c == '\n' || c == '\r') { if (!cur.empty()) out.push_back(std::move(cur)); cur.clear(); }
        else cur += char(c);
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

} // namespace

std::string_view toolVersion() { return OU_VERSION; }

std::optional<PeInfo> parsePe(const std::vector<uint8_t>& img) {
    std::span<const uint8_t> d(img);
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return std::nullopt;
    const uint32_t pe = *at<uint32_t>(d, 0x3C);
    if (at<uint32_t>(d, pe) != 0x00004550u) return std::nullopt;
    PeInfo info;
    info.machine = *at<uint16_t>(d, pe + 4);
    const uint16_t nsec = at<uint16_t>(d, pe + 6).value_or(0), optSize = at<uint16_t>(d, pe + 20).value_or(0);
    info.timestamp = at<uint32_t>(d, pe + 8).value_or(0);
    const size_t opt = pe + 24;
    const auto magic = at<uint16_t>(d, opt);
    if (!magic) return info;
    const size_t dirs = opt + (*magic == 0x20b ? 112 : 96);
    const uint32_t rsrcRva = at<uint32_t>(d, dirs + 2 * 8).value_or(0);
    if (!rsrcRva) return info;
    auto rvaToOff = [&](uint32_t rva) -> std::optional<size_t> {
        for (uint16_t i = 0; i < nsec; ++i) {
            const size_t s = opt + optSize + size_t(i) * 40;
            auto vs = at<uint32_t>(d, s + 8), va = at<uint32_t>(d, s + 12), rs = at<uint32_t>(d, s + 16), rp = at<uint32_t>(d, s + 20);
            if (!vs || !va || !rs || !rp) return std::nullopt;
            if (rva >= *va && rva < *va + std::max(*vs, *rs)) return *rp + size_t(rva - *va);
        }
        return std::nullopt;
    };
    const auto base = rvaToOff(rsrcRva);
    if (!base) return info;
    // Resource tree: type(16 = RT_VERSION) -> first name -> first language -> data entry.
    auto entry = [&](size_t dir, std::optional<uint32_t> id) -> std::optional<uint32_t> {
        auto named = at<uint16_t>(d, dir + 12), ids = at<uint16_t>(d, dir + 14);
        if (!named || !ids) return std::nullopt;
        for (size_t i = 0; i < size_t(*named) + *ids; ++i) {
            auto nm = at<uint32_t>(d, dir + 16 + i * 8), off = at<uint32_t>(d, dir + 20 + i * 8);
            if (!nm || !off) return std::nullopt;
            if (!id || *nm == *id) return *off;
        }
        return std::nullopt;
    };
    auto e1 = entry(*base, 16u);
    if (!e1 || !(*e1 & 0x80000000)) return info;
    auto e2 = entry(*base + (*e1 & 0x7FFFFFFF), std::nullopt);
    if (!e2 || !(*e2 & 0x80000000)) return info;
    auto e3 = entry(*base + (*e2 & 0x7FFFFFFF), std::nullopt);
    if (!e3 || (*e3 & 0x80000000)) return info;
    auto dataRva = at<uint32_t>(d, *base + *e3), dataSize = at<uint32_t>(d, *base + *e3 + 4);
    if (!dataRva || !dataSize) return info;
    auto vo = rvaToOff(*dataRva);
    if (!vo || *vo + *dataSize > d.size()) return info;
    walkVersion(d, *vo, *vo + *dataSize, 0, false, info);
    return info;
}

std::vector<HashDbEntry> parseGogHashdb(const fs::path& file) {
    auto t = unzipFirst(readFile(file));
    constexpr size_t kEntry = 1024 + 32;
    if (t.size() < 12) throw std::runtime_error("hashdb table too small");
    const uint32_t n = le<uint32_t>(t.data() + 8);
    if (12 + size_t(n) * kEntry != t.size()) throw std::runtime_error("hashdb layout mismatch (count " + std::to_string(n) + ", size " + std::to_string(t.size()) + ")");
    std::vector<HashDbEntry> out;
    for (uint32_t i = 0; i < n; ++i) {
        const char* e = reinterpret_cast<const char*>(t.data() + 12 + size_t(i) * kEntry);
        HashDbEntry h{toUtf8({e, strnlen(e, 1024)}), lower(std::string(e + 1024, 32))};
        if (!std::ranges::all_of(h.md5, [](char c) { return std::isxdigit(uint8_t(c)); }))
            throw std::runtime_error("hashdb entry " + std::to_string(i) + " has non-hex md5");
        out.push_back(std::move(h));
    }
    return out;
}

bool isInside(const fs::path& p, const fs::path& dir) {
    auto a = fs::weakly_canonical(fs::absolute(p)), b = fs::weakly_canonical(fs::absolute(dir));
    auto [ie, ia] = std::mismatch(b.begin(), b.end(), a.begin(), a.end());
    return ie == b.end() || (std::next(ie) == b.end() && ie->empty()); // trailing-slash dir
}

fs::path defaultManifestPath(std::string_view sha) {
    fs::path base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = fs::path(h) / ".local/share";
    else throw std::runtime_error("neither XDG_DATA_HOME nor HOME is set");
    return base / "openunderworld" / "manifests" / (std::string(sha.substr(0, 16)) + ".json");
}

json scan(const fs::path& gameDir, const ScanOptions& opt) {
    const auto t0 = Clock::now();
    if (!fs::is_directory(gameDir)) throw std::runtime_error("not a directory: " + gameDir.string());
    const unsigned threads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    json m;
    m["tool"] = {{"name", "ou-import"}, {"version", toolVersion()}};
    m["game_dir"] = fs::absolute(gameDir).string();
    m["mode"] = opt.deep ? "deep" : "header";
    {
        std::time_t now = std::time(nullptr);
        m["scanned_at"] = iso8601(uint32_t(now));
    }

    // ---- files ----
    std::vector<fs::path> rels;
    for (auto& e : fs::recursive_directory_iterator(gameDir))
        if (e.is_regular_file()) rels.push_back(fs::relative(e.path(), gameDir));
    std::ranges::sort(rels);
    std::vector<json> files(rels.size());
    const auto tHash = Clock::now();
    // ponytail: 4 hashing threads is a guess tuned for one HDD; SSD installs could use more.
    parallelFor(rels.size(), std::min(threads, 4u), [&](size_t i, unsigned) {
        auto p = gameDir / rels[i];
        json f{{"path", rels[i].generic_string()}, {"size", fs::file_size(p)}};
        try {
            f["type"] = detectFileType(rels[i], readFile(p, 16));
            if (opt.hashFiles) {
                auto d = hashFile(p);
                f["sha256"] = d.sha256;
                f["md5"] = d.md5;
            }
        } catch (const std::exception& e) {
            f["error"] = e.what();
        }
        files[i] = std::move(f);
    });
    m["files"] = files;
    const double hashSec = secondsSince(tHash);

    // ---- exe ----
    std::vector<uint8_t> exeBytes;
    {
        json ex{{"path", "tru.exe"}};
        const auto p = gameDir / "tru.exe";
        if (!fs::exists(p)) {
            ex["error"] = "tru.exe missing";
        } else {
            exeBytes = readFile(p);
            const auto sha = hashFile(p).sha256;
            ex["sha256"] = sha;
            ex["size"] = exeBytes.size();
            ex["build"] = "unknown";
            for (auto& k : kKnownBuilds)
                if (k.sha256 == sha) ex["build"] = k.label;
            if (auto pe = parsePe(exeBytes)) {
                ex["pe"] = {{"machine", machineName(pe->machine)}, {"timestamp", pe->timestamp}, {"timestamp_utc", iso8601(pe->timestamp)}};
                json ver = json::object();
                if (pe->fixedFileVersion) ver["fixed_file_version"] = *pe->fixedFileVersion;
                if (pe->fixedProductVersion) ver["fixed_product_version"] = *pe->fixedProductVersion;
                for (auto& [k, v] : pe->strings) ver[k] = v;
                ex["version_resource"] = ver.empty() ? json() : ver;
            } else {
                ex["error"] = "not a PE image";
            }
        }
        m["exe"] = ex;
    }

    // ---- GOG hashdb ----
    {
        json hd = json::array();
        for (auto& e : fs::directory_iterator(gameDir)) {
            auto fn = e.path().filename().string();
            if (!fn.starts_with("goggame-") || e.path().extension() != ".hashdb") continue;
            json h{{"file", fn}};
            try {
                json entries = json::array();
                std::map<std::string, std::string> md5ByPath;
                for (auto& f : files) md5ByPath[lower(f["path"].get<std::string>())] = f.value("md5", "");
                uint32_t ok = 0, bad = 0, missing = 0;
                for (auto& en : parseGogHashdb(e.path())) {
                    auto rel = en.path;
                    std::ranges::replace(rel, '\\', '/');
                    auto it = md5ByPath.find(lower(rel));
                    std::string st = it == md5ByPath.end() ? "missing" : !opt.hashFiles ? "unchecked" : it->second == en.md5 ? "ok" : "mismatch";
                    (st == "ok" ? ok : st == "missing" ? missing : bad) += st != "unchecked";
                    entries.push_back({{"path", en.path}, {"md5", en.md5}, {"status", st}});
                }
                h["entries"] = entries;
                h["summary"] = {{"ok", ok}, {"mismatch", bad}, {"missing", missing}};
            } catch (const std::exception& ex) {
                h["error"] = ex.what();
            }
            hd.push_back(h);
        }
        m["gog_hashdb"] = hd;
    }

    // ---- archives ----
    const auto tArc = Clock::now();
    std::vector<std::unique_ptr<ArchiveCtx>> archives;
    for (auto [label, file] : {std::pair{"bigfile", "bigfile.000"}, std::pair{"patch", "patch.000"}}) {
        auto a = std::make_unique<ArchiveCtx>();
        a->label = label;
        a->first = gameDir / file;
        if (!fs::exists(a->first)) { a->error = std::string(file) + " missing"; archives.push_back(std::move(a)); continue; }
        try {
            a->bf.emplace(a->first, opt.partAlignment);
        } catch (const std::exception& e) {
            a->error = e.what();
            archives.push_back(std::move(a));
            continue;
        }
        for (auto& r : a->bf->records())
            a->partsReferenced = std::max(a->partsReferenced, uint32_t((uint64_t(r.sector) << 11) / opt.partAlignment) + 1);
        a->parts = std::make_unique<Parts>(*a->bf, a->partsReferenced, opt.partAlignment);
        archives.push_back(std::move(a));
    }

    std::vector<std::unordered_set<std::string>> harvested(threads);
    for (auto& a : archives) {
        if (!a->bf) continue;
        const auto& recs = a->bf->records();
        a->info.resize(recs.size());
        parallelFor(recs.size(), threads, [&](size_t i, unsigned t) {
            a->info[i] = analyze(*a->parts, recs[i], opt.deep, opt.deep ? &harvested[t] : nullptr);
        });
    }
    const double analyzeSec = secondsSince(tArc);

    // ---- names ----
    const auto tNames = Clock::now();
    NameTable nt;
    for (auto& a : archives)
        if (a->bf)
            for (size_t i = 0; i < a->bf->records().size(); ++i)
                nt.kind.emplace(a->bf->records()[i].hash, a->info[i].kind);
    const std::string P = std::string(kPlat) + "\\";
    nt.add(P + "local\\locals.bin", "exe-template");
    nt.add(P + "padshock\\padshocklib.tfb", "exe-template");
    for (auto f : {"objectlist.txt", "objlist.dat", "unitlist.txt"}) nt.add("\\tr8\\" + std::string(kPlat) + "\\" + f, "exe-template");
    for (auto& s : exeStrings(exeBytes)) { // literal \tr8\... paths, "%d" expanded 0..15
        auto p = s.find("\\tr8\\");
        if (p == std::string::npos || s.find("%s") != std::string::npos) continue;
        s = s.substr(p);
        if (auto d = s.find("%d"); d != std::string::npos)
            for (int i = 0; i < 16; ++i) nt.add(s.substr(0, d) + std::to_string(i) + s.substr(d + 2), "exe-literal");
        else
            nt.add(s, "exe-literal");
    }
    if (fs::is_directory(gameDir / "Movies"))
        for (auto& e : fs::directory_iterator(gameDir / "Movies"))
            if (lower(e.path().extension().string()) == ".bik")
                nt.add("\\tr8\\local\\localization\\movies\\" + e.path().stem().string() + ".sch", "movies-dir");

    json lists;
    {   // objectlist.txt: "<count>\r\n<id>,<name>\r\n..."
        std::vector<std::string> names;
        if (auto d = readNamed(archives, "\\tr8\\pc-w\\objectlist.txt")) {
            auto ls = lines(*d);
            for (size_t i = 1; i < ls.size(); ++i)
                if (auto c = ls[i].find(','); c != std::string::npos) names.push_back(lower(ls[i].substr(c + 1)));
            lists["objectlist_txt"] = {{"declared", ls.empty() ? 0 : std::atoi(ls[0].c_str())}, {"parsed", names.size()}};
        }
        tryPool(nt, names, "objectlist.txt");
    }
    {   // unitlist.txt: "<count>\r\n<dir\\...\\unit>\r\n..."; objlist.dat: u32 count, count x 132-byte records (name[128] first)
        std::vector<std::string> names;
        if (auto d = readNamed(archives, "\\tr8\\pc-w\\unitlist.txt")) {
            auto ls = lines(*d);
            for (size_t i = 1; i < ls.size(); ++i) names.push_back(lower(ls[i].substr(ls[i].rfind('\\') + 1)));
            lists["unitlist_txt"] = {{"declared", ls.empty() ? 0 : std::atoi(ls[0].c_str())}, {"parsed", names.size()}};
        }
        tryPool(nt, names, "unitlist.txt");
        names.clear();
        if (auto d = readNamed(archives, "\\tr8\\pc-w\\objlist.dat"); d && d->size() >= 4) {
            const uint32_t n = le<uint32_t>(d->data());
            for (uint32_t i = 0; i < n && 4 + (i + 1) * 132ull <= d->size(); ++i) {
                const char* s = reinterpret_cast<const char*>(d->data() + 4 + i * 132);
                if (std::string nm(s, strnlen(s, 128)); !nm.empty()) names.push_back(lower(nm));
            }
            lists["objlist_dat"] = {{"declared", n}, {"named_entries", names.size()}};
        }
        tryPool(nt, names, "objlist.dat");
    }
    {
        std::set<std::string> deps, music;
        uint32_t withUnitDeps = 0;
        for (auto& a : archives)
            for (auto& ri : a->info) {
                if (ri.drm) {
                    deps.insert(ri.drm->objDeps.begin(), ri.drm->objDeps.end());
                    deps.insert(ri.drm->unitDeps.begin(), ri.drm->unitDeps.end());
                    withUnitDeps += !ri.drm->unitDeps.empty();
                }
                music.insert(ri.music.begin(), ri.music.end());
            }
        std::vector<std::string> dv;
        for (auto& d : deps) dv.push_back(lower(d));
        tryPool(nt, dv, "drm-dependency");
        for (auto& mu : music)
            for (auto ext : {".mus", ".sam", ".mul"}) nt.add(P + "music\\" + mu + "\\" + mu + ext, "mus-script");
        lists["drm_dependency_names"] = deps.size();
        lists["drms_with_unit_dependencies"] = withUnitDeps;
        lists["music_names"] = music.size();
    }
    if (opt.deep) {
        std::set<std::string> all;
        for (auto& h : harvested) all.insert(h.begin(), h.end());
        lists["drm_harvested_strings"] = all.size();
        tryPool(nt, {all.begin(), all.end()}, "drm-strings");
    }
    const uint64_t derivedTried = nt.tried;
    const size_t derived = nt.resolved.size();
    json external = json::array();
    for (auto& lf : opt.nameLists) {
        std::ifstream f(lf);
        if (!f) throw std::runtime_error("cannot open name list " + lf.string());
        uint32_t agree = 0, conflict = 0, added = 0;
        const std::string src = "namelist:" + lf.filename().string();
        for (std::string line; std::getline(f, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == ';') continue;
            const uint32_t h = cdc::pathHash(line);
            if (auto it = nt.resolved.find(h); it != nt.resolved.end()) {
                if (it->second.second.starts_with("namelist:")) continue;
                (lower(it->second.first) == lower(line) ? agree : conflict)++;
            } else {
                added += nt.add(line, src);
            }
        }
        external.push_back({{"file", lf.string()}, {"added", added}, {"agree_with_derived", agree}, {"conflict_with_derived", conflict}});
    }
    m["name_lists"] = lists;
    const double namesSec = secondsSince(tNames);

    // ---- archive JSON ----
    json arcs = json::array();
    std::map<std::string, uint32_t> totalKinds;
    for (auto& a : archives) {
        json aj{{"name", a->label}, {"first_part", a->first.filename().string()}};
        if (!a->error.empty()) aj["error"] = a->error;
        if (!a->bf) { arcs.push_back(aj); continue; }
        const auto& recs = a->bf->records();
        json parts = json::array(), missing = json::array();
        for (uint32_t p = 0; p < a->partsReferenced; ++p) {
            bool present = a->parts->fds[p] >= 0;
            parts.push_back({{"file", a->bf->partPath(p).filename().string()}, {"present", present}, {"size", a->parts->sizes[p]}});
            if (!present) missing.push_back(a->bf->partPath(p).filename().string());
        }
        json extra = json::array(); // parts on disk that no record references
        for (uint32_t p = a->partsReferenced; p < 1000 && fs::exists(a->bf->partPath(p)); ++p) extra.push_back(a->bf->partPath(p).filename().string());
        aj["parts"] = parts;
        aj["missing_parts"] = missing;
        aj["unreferenced_parts"] = extra;
        aj["record_count"] = recs.size();
        std::set<uint32_t> uniq;
        std::map<std::string, uint32_t> kinds, secTypes;
        uint32_t errors = 0, named = 0;
        uint64_t drmBytes = 0;
        json rj = json::array();
        for (size_t i = 0; i < recs.size(); ++i) {
            const auto& r = recs[i];
            const auto& ri = a->info[i];
            uniq.insert(r.hash);
            ++kinds[ri.kind];
            ++totalKinds[ri.kind];
            auto [part, off] = a->parts->locate(r);
            json j{{"hash", hex32(r.hash)}, {"size", r.size}, {"spec_mask", hex32(r.specMask)}, {"part", part}, {"offset", off}, {"kind", ri.kind}};
            if (r.compressedSize) j["compressed_size"] = r.compressedSize;
            if (auto n = nt.resolved.find(r.hash); n != nt.resolved.end()) { j["name"] = n->second.first; j["name_source"] = n->second.second; }
            if (ri.kind == "unknown") j["magic"] = ri.magic;
            if (!ri.error.empty()) { j["error"] = ri.error; ++errors; }
            if (ri.drm) {
                json types(ri.drm->types);
                j["drm"] = {{"sections", ri.drm->sections}, {"section_types", types}, {"decompressed_size", ri.drm->decompressedSize}};
                if (!ri.drm->objDeps.empty()) j["drm"]["object_deps"] = ri.drm->objDeps;
                if (!ri.drm->unitDeps.empty()) j["drm"]["unit_deps"] = ri.drm->unitDeps;
                for (auto& [k, v] : ri.drm->types) secTypes[k] += v;
                drmBytes += ri.drm->decompressedSize;
            }
            rj.push_back(std::move(j));
        }
        for (uint32_t h : uniq) named += nt.resolved.contains(h);
        aj["unique_hashes"] = uniq.size();
        aj["named_unique_hashes"] = named;
        aj["name_coverage_pct"] = uniq.empty() ? 0.0 : 100.0 * named / double(uniq.size());
        aj["record_errors"] = errors;
        aj["kinds"] = kinds;
        aj["drm_section_types"] = secTypes;
        aj["drm_decompressed_bytes"] = drmBytes;
        aj["records"] = std::move(rj);
        arcs.push_back(std::move(aj));
    }
    m["archives"] = arcs;

    // patch.000 records whose hash also exists in bigfile (the game prefers the patch copy).
    if (archives.size() == 2 && archives[0]->bf && archives[1]->bf) {
        json ov = json::array();
        for (auto& r : archives[1]->bf->records()) {
            auto base = archives[0]->bf->find(r.hash);
            bool sameSpec = std::ranges::any_of(base, [&](auto& b) { return b.specMask == r.specMask; });
            json j{{"hash", hex32(r.hash)}, {"spec_mask", hex32(r.specMask)}, {"in_bigfile", !base.empty()}, {"same_spec_in_bigfile", sameSpec}};
            if (auto n = nt.resolved.find(r.hash); n != nt.resolved.end()) j["name"] = n->second.first;
            ov.push_back(std::move(j));
        }
        m["patch_overrides"] = ov;
    }

    std::set<uint32_t> allUniq;
    for (auto& [h, k] : nt.kind) allUniq.insert(h);
    m["names"] = {{"unique_hashes", allUniq.size()},
                  {"resolved_install_derived", derived},
                  {"resolved_total", nt.resolved.size()},
                  {"coverage_install_derived_pct", allUniq.empty() ? 0.0 : 100.0 * double(derived) / double(allUniq.size())},
                  {"coverage_total_pct", allUniq.empty() ? 0.0 : 100.0 * double(nt.resolved.size()) / double(allUniq.size())},
                  {"candidates_tried_install_derived", derivedTried},
                  {"rejected_kind_mismatch", nt.kindRejected},
                  {"by_source", nt.bySource},
                  {"external_lists", external}};
    m["kinds_total"] = totalKinds;
    m["runtime_seconds"] = {{"file_hashing", hashSec}, {"record_analysis", analyzeSec}, {"name_recovery", namesSec}, {"total", secondsSince(t0)}, {"threads", threads}};
    return m;
}

json diffManifests(const json& a, const json& b) {
    json d;
    auto fileMap = [](const json& m) {
        std::map<std::string, json> r;
        for (auto& f : m.value("files", json::array())) r[f.value("path", "")] = f;
        return r;
    };
    auto fa = fileMap(a), fb = fileMap(b);
    json added = json::array(), removed = json::array(), changed = json::array();
    for (auto& [p, f] : fb)
        if (!fa.contains(p)) added.push_back(p);
    for (auto& [p, f] : fa) {
        auto it = fb.find(p);
        if (it == fb.end()) { removed.push_back(p); continue; }
        const auto& g = it->second;
        if (f.value("size", 0ull) != g.value("size", 0ull) || f.value("sha256", "") != g.value("sha256", ""))
            changed.push_back({{"path", p}, {"a", {{"size", f.value("size", 0ull)}, {"sha256", f.value("sha256", "")}}},
                               {"b", {{"size", g.value("size", 0ull)}, {"sha256", g.value("sha256", "")}}}});
    }
    d["files"] = {{"added", added}, {"removed", removed}, {"changed", changed}};

    auto recMap = [](const json& arc) {
        std::map<std::string, json> r; // key: hash/spec_mask (+ #n for duplicates)
        for (auto& rec : arc.value("records", json::array())) {
            std::string k = rec.value("hash", "") + "/" + rec.value("spec_mask", "");
            for (int n = 1; r.contains(k); ++n) k = rec.value("hash", "") + "/" + rec.value("spec_mask", "") + "#" + std::to_string(n);
            r[k] = rec;
        }
        return r;
    };
    std::map<std::string, std::pair<json, json>> arcs;
    for (auto& x : a.value("archives", json::array())) arcs[x.value("name", "")].first = x;
    for (auto& x : b.value("archives", json::array())) arcs[x.value("name", "")].second = x;
    json ra;
    uint64_t total = 0;
    for (auto& [name, ab] : arcs) {
        auto ma = recMap(ab.first.is_null() ? json::object() : ab.first), mb = recMap(ab.second.is_null() ? json::object() : ab.second);
        json add = json::array(), rem = json::array(), chg = json::array();
        auto label = [](const std::string& k, const json& r) { return r.contains("name") ? k + " " + r["name"].get<std::string>() : k; };
        for (auto& [k, r] : mb)
            if (!ma.contains(k)) add.push_back(label(k, r));
        for (auto& [k, r] : ma) {
            auto it = mb.find(k);
            if (it == mb.end()) { rem.push_back(label(k, r)); continue; }
            json fields = json::array();
            for (auto f : {"size", "part", "offset", "kind", "error"})
                if (r.value(f, json()) != it->second.value(f, json())) fields.push_back(f);
            if (!fields.empty()) chg.push_back({{"record", label(k, r)}, {"fields", fields}});
        }
        total += add.size() + rem.size() + chg.size();
        ra[name] = {{"added", add}, {"removed", rem}, {"changed", chg}};
    }
    d["records"] = ra;
    const auto ea = a.value("exe", json::object()).value("sha256", ""), eb = b.value("exe", json::object()).value("sha256", "");
    if (ea != eb) d["exe"] = {{"a", ea}, {"b", eb}};
    d["identical"] = added.empty() && removed.empty() && changed.empty() && total == 0 && ea == eb;
    return d;
}

Extracted extract(const fs::path& gameDir, std::string_view key, std::optional<uint32_t> spec, bool raw, uint64_t align) {
    uint32_t hash;
    std::string k(key);
    if (k.starts_with("0x") || k.starts_with("0X")) {
        size_t used = 0;
        unsigned long v = std::stoul(k.substr(2), &used, 16);
        if (used != k.size() - 2 || v > 0xFFFFFFFFul) throw std::runtime_error("bad hash " + k);
        hash = uint32_t(v);
    } else {
        hash = cdc::pathHash(k);
    }
    for (auto [label, file] : {std::pair{"patch", "patch.000"}, std::pair{"bigfile", "bigfile.000"}}) {
        if (!fs::exists(gameDir / file)) continue;
        cdc::Bigfile bf(gameDir / file, align);
        auto recs = bf.find(hash);
        auto it = std::ranges::find_if(recs, [&](auto& r) { return !spec || r.specMask == *spec; });
        if (it == recs.end()) continue;
        uint32_t parts = 0;
        for (auto& r : bf.records()) parts = std::max(parts, uint32_t((uint64_t(r.sector) << 11) / align) + 1);
        Parts p(bf, parts, align);
        if (auto e = p.check(*it); !e.empty()) throw std::runtime_error(e);
        Extracted x{p.read(*it, 0, it->size), label, hash, it->specMask, recs.size()};
        if (!raw && cdc::isCdrm(x.bytes)) x.bytes = cdc::decompressCdrm(x.bytes);
        return x;
    }
    throw std::runtime_error("no record for " + hex32(hash) + (spec ? " with spec mask " + hex32(*spec) : ""));
}

} // namespace ou::importer
