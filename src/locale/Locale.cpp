#include "locale/Locale.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstring>

#include "cdc/Cdc.hpp"

namespace ou::locale {

using cdc::FormatError;

namespace {

constexpr std::array<std::string_view, kLanguageCount> kNames = {
    "lang_english", "lang_french",  "lang_german",    "lang_italian", "lang_spanish",
    "lang_japanese", "lang_portugese", "lang_polish", "lang_ukenglish", "lang_russian",
    "lang_czech",   "lang_dutch",   "lang_hungarian",
};
constexpr std::array<std::string_view, kLanguageCount> kCodes = {
    "en", "fr", "de", "it", "es", "ja", "pt", "pl", "en-gb", "ru", "cs", "nl", "hu"};

uint32_t rd32(std::span<const uint8_t> b, size_t o) {
    uint32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}

bool iequalsPrefix(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

} // namespace

std::string_view languageName(Language l) { return kNames.at(static_cast<size_t>(l)); }

std::optional<Language> parseLanguage(std::string_view s) {
    for (int i = 0; i < kLanguageCount; ++i) {
        std::string_view name = kNames[i];
        if (s == kCodes[i] || s == name || s == name.substr(5)) return Language(i);
    }
    int n = -1;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec == std::errc() && p == s.data() + s.size() && n >= 0 && n < kLanguageCount) return Language(n);
    return std::nullopt;
}

// ---- locals.bin -------------------------------------------------------------------------

bool exeUtf8Valid(std::string_view s) {
    auto forbidden = [](uint8_t c) { return c == 0xC0 || c == 0xC1 || c >= 0xF5; };
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = static_cast<uint8_t>(s[i++]);
        if (forbidden(b)) return false;
        int len = (b & 0xF8) == 0xF0 ? 4 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xE0) == 0xC0 ? 2 : 1;
        if (len == 1 && (b & 0x80)) return false;
        for (int k = 1; k < len; ++k, ++i) {
            if (i >= s.size()) return false; // the exe reads the NUL here, which fails the top-bit test
            const uint8_t c = static_cast<uint8_t>(s[i]);
            if (forbidden(c) || !(c & 0x80)) return false;
        }
    }
    return true;
}

Strings::Strings(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
    if (bytes_.size() < 8) throw FormatError("locals.bin: truncated header");
    headerLanguage_ = rd32(bytes_, 0);
    const uint32_t n = rd32(bytes_, 4);
    if (n > (bytes_.size() - 8) / 4) throw FormatError("locals.bin: offset table truncated");
    offsets_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t o = rd32(bytes_, 8 + 4 * size_t(i));
        if (o && (o >= bytes_.size() || !std::memchr(bytes_.data() + o, 0, bytes_.size() - o)))
            throw FormatError("locals.bin: string " + std::to_string(i) + " truncated");
        offsets_[i] = o;
    }
}

bool Strings::valid(int id) const {
    if (id < 0 || size_t(id) >= offsets_.size() || !offsets_[id]) return false;
    return exeUtf8Valid(reinterpret_cast<const char*>(bytes_.data() + offsets_[id]));
}

std::string_view Strings::get(int id) const {
    if (id < 0 || size_t(id) >= offsets_.size()) return "ERROR OUT OF TABLE RANGE";
    if (!offsets_[id]) return "";
    std::string_view s(reinterpret_cast<const char*>(bytes_.data() + offsets_[id]));
    return exeUtf8Valid(s) ? s : "Invalid UTF8 encoding";
}

std::optional<Strings> loadStrings(const std::filesystem::path& gameDir, Language lang) {
    auto bytes = cdc::BigfileSet(gameDir, specMask(lang)).read("pc-w\\local\\locals.bin");
    if (!bytes) return std::nullopt;
    return Strings(std::move(*bytes));
}

// ---- Movie subtitles ----------------------------------------------------------------------

std::string movieSchemaPath(std::string_view movieStem) {
    return "\\tr8\\local\\localization\\movies\\" + std::string(movieStem) + ".sch";
}

std::vector<Cue> parseMovieSchema(std::string_view sch, Language lang) {
    // 0x4A0720: first line starting with the key (strnicmp) that has a quote; text runs to the last
    // quote of that line.
    const std::string_view key = languageName(lang);
    std::string_view text;
    for (size_t pos = 0; pos < sch.size();) {
        size_t eol = sch.find('\n', pos);
        if (eol == std::string_view::npos) eol = sch.size();
        std::string_view line = sch.substr(pos, eol - pos);
        pos = eol + 1;
        if (!iequalsPrefix(line, key)) continue;
        const size_t q0 = line.find('"'), q1 = line.rfind('"');
        if (q0 == std::string_view::npos || q1 == q0) continue;
        text = line.substr(q0 + 1, q1 - q0 - 1);
        break;
    }

    // 0x4A0910: "\(seconds)" sets the start of the following text; "\n" is a line break and any
    // other "\c" stands for c. The text before the first marker starts at 0.
    std::vector<Cue> cues;
    Cue cur;
    auto flush = [&] {
        if (!cur.text.empty()) cues.push_back(cur);
        cur.text.clear();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '(') {
            flush();
            const size_t close = text.find(')', i);
            double t = 0;
            std::from_chars(text.data() + i + 2, text.data() + (close == std::string_view::npos ? text.size() : close), t);
            cur.start = t;
            i = close == std::string_view::npos ? text.size() : close + 1;
        } else if (text[i] == '\\' && i + 1 < text.size()) {
            cur.text += text[i + 1] == 'n' ? '\n' : text[i + 1];
            i += 2;
        } else {
            cur.text += text[i++];
        }
    }
    flush();
    return cues;
}

void SubtitleTicker::push(std::string_view line) {
    if (!lines_[1].empty()) lines_[0] = lines_[1];
    lines_[1] = line;
    timer_ = 90; // 0x583DB0 sets max(timer, 90) after zeroing it on a shift; the timer never exceeds 90
}

void SubtitleTicker::advance(double ticks) {
    timer_ -= ticks;
    if (timer_ <= 0) {
        lines_[0] = lines_[1];
        lines_[1].clear();
        timer_ = 90;
    }
}

void MovieSubtitles::update(double t, double dtTicks) {
    if (next_ < cues_.size() && cues_[next_].start <= t) { // at most one cue per frame, as the exe
        std::string_view s = cues_[next_++].text;
        for (size_t p = 0; p <= s.size();) {
            size_t nl = s.find('\n', p);
            if (nl == std::string_view::npos) nl = s.size();
            if (nl > p) ticker_.push(s.substr(p, nl - p));
            p = nl + 1;
        }
    }
    ticker_.advance(dtTicks);
}

std::vector<std::string> cineSubtitleLines(std::string_view text) {
    std::vector<std::string> out;
    for (size_t p = 0; p < text.size();) {
        size_t nl = text.find('\n', p);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(p, nl - p);
        p = nl + 1;
        if (line.empty()) continue; // strtok skips empty tokens
        const size_t colon = line.find(": ");
        if (colon != std::string_view::npos && colon + 2 < line.size()) line.remove_prefix(colon + 2);
        out.emplace_back(line);
    }
    return out;
}

// ---- Cinematic streams ----------------------------------------------------------------------

std::vector<uint8_t> mulCinematicStream(std::span<const uint8_t> mul) {
    // MultiplexStream: 2048-byte header, then packets {u32 type, u32 size, u32 pad[2], size bytes}.
    // Type 0 = sound, 1 = cinematic, 2 = padding.
    if (mul.size() < 2048) throw FormatError("mul: truncated header");
    std::vector<uint8_t> out;
    for (size_t o = 2048; o + 16 <= mul.size();) {
        const uint32_t type = rd32(mul, o), size = rd32(mul, o + 4);
        if (size > mul.size() - o - 16) throw FormatError("mul: truncated packet");
        if (type == 1) out.insert(out.end(), mul.begin() + o + 16, mul.begin() + o + 16 + size);
        o += 16 + size;
    }
    return out;
}

namespace {

struct Reader {
    std::span<const uint8_t> b;
    size_t p = 0;
    void need(size_t n) const {
        if (n > b.size() - p) throw FormatError("cine: truncated at " + std::to_string(p));
    }
    uint32_t u32() { need(4); uint32_t v = rd32(b, p); p += 4; return v; }
    void skip(uint64_t n) {
        if (n > b.size() - p) throw FormatError("cine: truncated at " + std::to_string(p));
        p += n;
    }
};

// One subtitle blob (0x4538D0): repeated "<lang>\r<text>\r", text cut at 255 bytes.
void parseSubtitleBlob(std::string_view s, int frame, std::vector<CineSubtitle>& out) {
    auto skipSpace = [&](size_t& i) { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    size_t i = 0;
    skipSpace(i);
    while (i < s.size() && s[i]) {
        int lang = 0;
        std::from_chars(s.data() + i, s.data() + s.size(), lang);
        while (i < s.size() && s[i] != '\r' && s[i]) ++i;
        if (i < s.size() && s[i] == '\r') ++i;
        std::string text;
        for (; i < s.size() && s[i] != '\r' && s[i]; ++i)
            if (text.size() < 255) text += s[i];
        out.push_back({frame, lang, std::move(text)});
        skipSpace(i);
    }
}

} // namespace

CineInfo parseCineStream(std::span<const uint8_t> cine) {
    // Header as read by 0x4537A0 + 0x456580; the channel count is their running total (+0xCC).
    Reader r{cine};
    r.need(12);
    if (std::memcmp(cine.data(), "ENIC", 4) != 0) throw FormatError("cine: bad magic");
    r.p = 8;
    const uint32_t headerSize = r.u32(); // bytes after the magic + version
    CineInfo info;
    r.need(64);
    info.unitName.assign(reinterpret_cast<const char*>(cine.data() + r.p), strnlen(reinterpret_cast<const char*>(cine.data() + r.p), 64));
    r.p += 64;
    r.u32();
    uint64_t channels = 0;
    const uint32_t nA = r.u32();
    r.skip(12ull * nA);
    channels += 9ull * nA;
    const uint32_t nInstances = r.u32();
    for (uint32_t i = 0; i < nInstances; ++i) {
        r.u32(); r.u32();
        const uint32_t bones = r.u32();
        r.u32();
        r.skip(64ull * bones); // 4x4 float matrices (0x453630)
        channels += 14ull * bones + 3;
        const uint32_t nC = r.u32();
        r.u32();
        r.skip(24ull * nC);
        channels += 10ull * nC;
        channels += r.u32();
        r.u32();
    }
    const uint32_t nE = r.u32();
    r.skip(4ull * nE);
    channels += 16ull * nE;
    r.u32();
    const uint32_t nF = r.u32();
    r.skip(8ull * nF);
    channels += nF;
    r.skip(4);
    if (r.p < headerSize) r.skip(28ull * nInstances); // per-instance 7 dwords
    if (r.p != uint64_t(headerSize) + 8) throw FormatError("cine: header size mismatch");
    if (channels > (1u << 20)) throw FormatError("cine: implausible channel count");
    info.channels = int(channels);

    // Frames (0x457100): u32 size; i32 frame (-1 ends); per channel a run-length coded float;
    // u32 subtitle length + bytes; pad to 16.
    struct Run { uint32_t left = 0; bool animated = false; };
    std::vector<Run> runs(channels);
    for (;;) {
        const size_t start = r.p;
        const uint32_t size = r.u32();
        const int32_t frame = int32_t(r.u32());
        if (frame == -1) break;
        if (frame != info.frames) throw FormatError("cine: frame " + std::to_string(frame) + " out of sequence");
        for (Run& run : runs) {
            if (run.left == 0) {
                const uint32_t v = r.u32();
                run.left = v & 0x0FFFFFFF;
                run.animated = (v & 0xF0000000) != 0;
                r.skip(4); // first value of the run
            } else if (run.animated) {
                r.skip(4);
            }
            --run.left; // wraps for an (unobserved) zero-length run, as the exe's counter
        }
        const uint32_t subLen = r.u32();
        r.need(subLen);
        if (subLen && subLen < 0xFFF) // longer blobs: the exe shows "bad subtitle data" (none observed)
            parseSubtitleBlob({reinterpret_cast<const char*>(cine.data() + r.p), subLen}, frame, info.subtitles);
        r.p += subLen;
        r.skip((16 - r.p % 16) % 16);
        if (r.p - start != uint64_t(size) + 4) throw FormatError("cine: frame size mismatch at frame " + std::to_string(frame));
        ++info.frames;
    }
    return info;
}

} // namespace ou::locale
