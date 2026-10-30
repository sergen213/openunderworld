#include "update/Update.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>

namespace ou::update {

namespace {

using nlohmann::json;
constexpr std::string_view kMarker = ".openunderworld-install";

void sodium() {
    static const bool ok = sodium_init() >= 0;
    if (!ok) throw Error("libsodium initialisation failed");
}

bool isHex(std::string_view s, size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Refuse anything that looks like original game data; updates only ever carry project files.
bool looksLikeGameData(std::string_view path) {
    std::string p(path);
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string name = p.substr(p.find_last_of('/') + 1);
    std::string ext = name.find('.') == std::string::npos ? "" : name.substr(name.find_last_of('.'));
    static const std::set<std::string> bad = {".drm", ".exe", ".dll", ".mul", ".fsb", ".big"};
    bool numericExt = ext.size() == 4 && std::all_of(ext.begin() + 1, ext.end(), ::isdigit); // bigfile.000 etc.
    return bad.count(ext) || numericExt || name.starts_with("bigfile") || name.starts_with("patch.");
}

void checkRelativePath(const std::string& p) {
    if (p.empty() || p.size() > 255 || p.front() == '/' || p.find('\\') != std::string::npos || p.find('\0') != std::string::npos)
        throw Error("manifest: bad path '" + p + "'");
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string_view c(p.data() + start, end - start);
        if (c.empty() || c == "." || c == "..") throw Error("manifest: bad path '" + p + "'");
        start = end + 1;
    }
    if (p == kManifestName || p == std::string(kManifestName) + ".sig") throw Error("manifest: reserved path '" + p + "'");
    if (looksLikeGameData(p)) throw Error("manifest: refusing game-data-like path '" + p + "'");
}

void checkHttps(const std::string& url) {
    if (!url.starts_with("https://") || url.size() <= 8 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        throw Error("not an https URL: '" + url + "'");
}

Bytes readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Error("cannot read " + p.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

void writeFileSync(const fs::path& p, std::span<const uint8_t> data, mode_t mode) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) throw Error("cannot create " + p.string() + ": " + std::strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); throw Error("write failed: " + p.string()); }
        off += size_t(n);
    }
    bool ok = ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    if (!ok) throw Error("fsync/close failed: " + p.string());
    fs::permissions(p, fs::perms(mode), fs::perm_options::replace);
}

void fsyncDir(const fs::path& d) {
    int fd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
}

void exchange(const fs::path& a, const fs::path& b) {
    if (::renameat2(AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_EXCHANGE) != 0)
        throw Error("renameat2(RENAME_EXCHANGE) " + a.string() + " <-> " + b.string() + ": " + std::strerror(errno));
}

bool overlaps(const fs::path& a, const fs::path& b) {
    auto inside = [](const fs::path& p, const fs::path& dir) {
        auto rel = p.lexically_relative(dir);
        return !rel.empty() && *rel.begin() != "..";
    };
    fs::path ca = fs::weakly_canonical(fs::absolute(a)), cb = fs::weakly_canonical(fs::absolute(b));
    return inside(ca, cb) || inside(cb, ca);
}

// Ensures the root is ours (marker) and does not overlap e.g. the game directory.
void prepareRoot(const fs::path& root, const std::vector<fs::path>& forbidden) {
    if (root.empty()) throw Error("no install root");
    for (const auto& f : forbidden)
        if (!f.empty() && overlaps(root, f)) throw Error("install root " + root.string() + " overlaps " + f.string());
    if (fs::exists(root) && !fs::exists(root / kMarker) && !fs::is_empty(root))
        throw Error(root.string() + " is not empty and lacks " + std::string(kMarker) + "; refusing to touch it");
    fs::create_directories(root);
    if (!fs::exists(root / kMarker)) {
        std::string_view text = "OpenUnderworld install root; managed by the updater.\n";
        writeFileSync(root / kMarker, {reinterpret_cast<const uint8_t*>(text.data()), text.size()}, 0644);
    }
}

class Lock {
public:
    explicit Lock(const fs::path& root) {
        fd_ = ::open((root / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) ::close(fd_);
            throw Error("install root is locked by another update/launcher process");
        }
    }
    ~Lock() { ::close(fd_); } // closing releases the flock
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    int fd_;
};

// Idempotent: every crash point between "journal written" and "journal removed" ends in the same state.
std::string finishSwap(const fs::path& root, const std::string& target) {
    const fs::path cur = root / "current", prev = root / "previous", stg = root / ".staging", trash = root / ".trash";
    fs::remove_all(trash);
    std::string what = "completed interrupted update to " + target;
    if (versionOf(cur) != target) {
        if (versionOf(stg) != target) { // staging vanished/corrupt: abandon, keep the running version
            fs::remove_all(stg);
            fs::remove(root / ".journal");
            fsyncDir(root);
            return "abandoned interrupted update to " + target + " (staging incomplete)";
        }
        if (fs::exists(prev)) { fs::rename(prev, trash); fs::remove_all(trash); }
        if (fs::exists(cur)) exchange(stg, cur);
        else fs::rename(stg, cur);
        fsyncDir(root);
    }
    if (fs::exists(stg)) { // stg now holds the old version
        if (!fs::exists(prev)) fs::rename(stg, prev);
        else fs::remove_all(stg);
    }
    fs::remove(root / ".journal");
    fsyncDir(root);
    return what;
}

std::string recoverLocked(const fs::path& root) {
    fs::remove_all(root / ".trash");
    if (fs::exists(root / ".journal")) {
        std::string target;
        try {
            auto j = json::parse(readFile(root / ".journal"));
            target = j.at("version").get<std::string>();
        } catch (const std::exception&) {}
        if (target.empty()) {
            fs::remove_all(root / ".staging");
            fs::remove(root / ".journal");
            return "removed unreadable update journal";
        }
        return finishSwap(root, target);
    }
    if (fs::exists(root / ".staging")) {
        fs::remove_all(root / ".staging");
        return "removed incomplete staging directory";
    }
    return {};
}

void say(const std::function<void(const std::string&)>& p, const std::string& s) {
    if (p) p(s);
}

Result fetchVerified(const Options& opt, Bytes& manifestBytes, Bytes& sig) {
    Result r;
    if (!opt.enabled || disabledByEnvironment()) return r; // Status::Disabled, no network
    if (!opt.fetch) throw Error("no transport");
    checkHttps(opt.manifestUrl);
    say(opt.progress, "fetching " + opt.manifestUrl);
    manifestBytes = opt.fetch(opt.manifestUrl, kMaxManifestBytes);
    sig = opt.fetch(opt.manifestUrl + ".sig", crypto_sign_BYTES);
    r.manifest = verifyAndParse(manifestBytes, sig, opt.key);
    if (r.manifest.channel != opt.channel)
        throw Error("manifest channel '" + r.manifest.channel + "' != configured '" + opt.channel + "'");
    r.installedVersion = opt.appImage.empty() ? installedVersion(opt.root) : std::optional(opt.runningVersion);
    r.status = Status::Available;
    if (r.installedVersion) {
        int c = compareVersions(r.manifest.version, *r.installedVersion);
        if (c < 0) throw Error("rollback refused: offered " + r.manifest.version + " is older than installed " + *r.installedVersion);
        if (c == 0) r.status = Status::UpToDate;
        else if (!r.manifest.minVersion.empty() && compareVersions(*r.installedVersion, r.manifest.minVersion) < 0)
            throw Error("installed " + *r.installedVersion + " is below min_version " + r.manifest.minVersion + "; reinstall manually");
    }
    return r;
}

} // namespace

std::optional<PublicKey> parsePublicKeyHex(std::string_view hex) {
    std::string h(hex);
    while (!h.empty() && std::isspace(static_cast<unsigned char>(h.back()))) h.pop_back();
    if (!isHex(h, 64)) return std::nullopt;
    PublicKey k{};
    for (size_t i = 0; i < 32; ++i) k[i] = uint8_t(std::stoul(h.substr(2 * i, 2), nullptr, 16));
    return k;
}

std::optional<PublicKey> releasePublicKey() {
#ifdef OU_UPDATE_PUBLIC_KEY
    return parsePublicKeyHex(OU_UPDATE_PUBLIC_KEY);
#else
    return std::nullopt;
#endif
}

bool verifySignature(std::span<const uint8_t> message, std::span<const uint8_t> signature, const PublicKey& key) {
    sodium();
    return signature.size() == crypto_sign_BYTES &&
           crypto_sign_verify_detached(signature.data(), message.data(), message.size(), key.data()) == 0;
}

std::string sha256Hex(std::span<const uint8_t> data) {
    sodium();
    unsigned char h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, data.data(), data.size());
    char hex[2 * sizeof h + 1];
    sodium_bin2hex(hex, sizeof hex, h, sizeof h);
    return hex;
}

int compareVersions(std::string_view a, std::string_view b) {
    auto parse = [](std::string_view v) {
        std::vector<uint32_t> out;
        size_t start = 0;
        while (true) {
            size_t end = v.find('.', start);
            std::string_view c = v.substr(start, end == std::string_view::npos ? v.npos : end - start);
            if (c.empty() || c.size() > 9 || !std::all_of(c.begin(), c.end(), ::isdigit) || out.size() == 4)
                throw Error("malformed version '" + std::string(v) + "'");
            out.push_back(uint32_t(std::stoul(std::string(c))));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        out.resize(4, 0);
        return out;
    };
    auto x = parse(a), y = parse(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

Manifest parseManifest(std::string_view text) {
    if (text.size() > kMaxManifestBytes) throw Error("manifest too large");
    json j;
    try { j = json::parse(text); } catch (const std::exception& e) { throw Error(std::string("manifest: ") + e.what()); }
    try {
        Manifest m;
        m.version = j.at("version").get<std::string>();
        m.channel = j.at("channel").get<std::string>();
        m.changelog = j.value("changelog", "");
        m.minVersion = j.value("min_version", "");
        compareVersions(m.version, "0");
        if (!m.minVersion.empty()) compareVersions(m.minVersion, "0");
        if (m.channel.empty()) throw Error("manifest: empty channel");
        std::set<std::string> seen;
        for (const auto& f : j.at("files")) {
            FileEntry e;
            e.path = f.at("path").get<std::string>();
            e.size = f.at("size").get<uint64_t>();
            e.sha256 = f.at("sha256").get<std::string>();
            e.url = f.at("url").get<std::string>();
            e.executable = f.value("executable", false);
            checkRelativePath(e.path);
            checkHttps(e.url);
            if (!isHex(e.sha256, 64)) throw Error("manifest: bad sha256 for " + e.path);
            if (!f.at("size").is_number_unsigned()) throw Error("manifest: bad size for " + e.path);
            if (!seen.insert(e.path).second) throw Error("manifest: duplicate path " + e.path);
            m.files.push_back(std::move(e));
        }
        if (m.files.empty()) throw Error("manifest: no files");
        return m;
    } catch (const Error&) {
        throw;
    } catch (const std::exception& e) {
        throw Error(std::string("manifest: ") + e.what());
    }
}

Manifest verifyAndParse(std::span<const uint8_t> manifest, std::span<const uint8_t> signature, const PublicKey& key) {
    if (manifest.size() > kMaxManifestBytes) throw Error("manifest too large");
    if (!verifySignature(manifest, signature, key)) throw Error("manifest signature verification failed");
    return parseManifest({reinterpret_cast<const char*>(manifest.data()), manifest.size()});
}

bool disabledByEnvironment() {
    const char* v = std::getenv("OU_DISABLE_UPDATES");
    return v && *v && std::string_view(v) != "0";
}

std::optional<std::string> versionOf(const fs::path& dir) {
    try {
        if (!fs::is_regular_file(dir / kManifestName)) return std::nullopt;
        auto j = json::parse(readFile(dir / kManifestName));
        return j.at("version").get<std::string>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> installedVersion(const fs::path& root) { return versionOf(root / "current"); }

Fetch httpsFetch() {
    return [](const std::string& url, uint64_t maxBytes) -> Bytes {
        checkHttps(url);
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> c(curl_easy_init(), curl_easy_cleanup);
        if (!c) throw Error("curl_easy_init failed");
        struct Sink { Bytes data; uint64_t max; } sink{{}, maxBytes};
        auto write = +[](char* p, size_t sz, size_t n, void* ud) -> size_t {
            auto* s = static_cast<Sink*>(ud);
            if (s->data.size() + sz * n > s->max) return 0; // aborts the transfer
            s->data.insert(s->data.end(), p, p + sz * n);
            return sz * n;
        };
        char err[CURL_ERROR_SIZE] = {};
        CURL* h = c.get();
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(h, CURLOPT_SSLVERSION, long(CURL_SSLVERSION_TLSv1_2));
        curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, curl_off_t(maxBytes));
        curl_easy_setopt(h, CURLOPT_USERAGENT, "OpenUnderworld-updater");
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, err);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
        CURLcode rc = curl_easy_perform(h);
        if (rc != CURLE_OK) throw Error(url + ": " + (*err ? err : curl_easy_strerror(rc)));
        return std::move(sink.data);
    };
}

Result check(const Options& opt) {
    Bytes mb, sig;
    return fetchVerified(opt, mb, sig);
}

Result update(const Options& opt) {
    Bytes mb, sig;
    Result r = fetchVerified(opt, mb, sig);
    if (r.status != Status::Available) return r;
    if (opt.appImage.empty()) install(opt.root, r.manifest, mb, sig, opt.fetch, opt.forbiddenRoots, opt.progress);
    else r.appImage = installAppImage(opt.appImage, r.manifest, opt.fetch, opt.applicationsDir, opt.progress);
    r.status = Status::Installed;
    return r;
}

void install(const fs::path& root, const Manifest& m, std::span<const uint8_t> manifestBytes,
             std::span<const uint8_t> signature, const Fetch& fetch, const std::vector<fs::path>& forbiddenRoots,
             const std::function<void(const std::string&)>& progress) {
    prepareRoot(root, forbiddenRoots);
    Lock lock(root);
    if (auto r = recoverLocked(root); !r.empty()) say(progress, r);
    if (auto cur = installedVersion(root); cur && compareVersions(m.version, *cur) <= 0)
        throw Error("rollback refused: " + m.version + " is not newer than installed " + *cur);

    const fs::path stg = root / ".staging";
    try {
        fs::create_directories(stg);
        for (const auto& f : m.files) {
            say(progress, "downloading " + f.path);
            Bytes data = fetch(f.url, f.size);
            if (data.size() != f.size)
                throw Error(f.path + ": size mismatch (" + std::to_string(data.size()) + " != " + std::to_string(f.size) + ")");
            if (sha256Hex(data) != f.sha256) throw Error(f.path + ": sha256 mismatch");
            fs::path dst = stg / f.path;
            fs::create_directories(dst.parent_path());
            writeFileSync(dst, data, f.executable ? 0755 : 0644);
        }
        writeFileSync(stg / kManifestName, manifestBytes, 0644);
        writeFileSync(stg / (std::string(kManifestName) + ".sig"), signature, 0644);
        fsyncDir(stg);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(stg, ec); // nothing was swapped; the current version is untouched
        throw;
    }

    std::string journal = json{{"version", m.version}}.dump();
    writeFileSync(root / ".journal", {reinterpret_cast<const uint8_t*>(journal.data()), journal.size()}, 0644);
    fsyncDir(root);
    say(progress, "swapping in " + m.version);
    finishSwap(root, m.version);
}

std::string rollback(const fs::path& root) {
    if (!fs::exists(root / kMarker)) throw Error("no OpenUnderworld install at " + root.string());
    Lock lock(root);
    recoverLocked(root);
    if (!versionOf(root / "previous") || !versionOf(root / "current")) throw Error("no previous version to roll back to");
    exchange(root / "previous", root / "current");
    fsyncDir(root);
    return *versionOf(root / "current");
}

std::string recover(const fs::path& root) {
    if (!fs::exists(root / kMarker)) return {};
    Lock lock(root);
    return recoverLocked(root);
}

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The version in an OpenUnderworld-<version>-x86_64.AppImage name; "" for any other name.
std::string versionInName(const fs::path& p) {
    std::string n = p.filename().string();
    constexpr std::string_view pre = "OpenUnderworld-", post = "-x86_64.AppImage";
    if (!n.starts_with(pre) || !n.ends_with(post) || n.size() <= pre.size() + post.size()) return {};
    std::string v = n.substr(pre.size(), n.size() - pre.size() - post.size());
    try { compareVersions(v, v); } catch (const Error&) { return {}; }
    return v;
}

// <x>.AppImage.bak siblings an earlier update made: the explicit one, else OpenUnderworld*-named ones.
std::vector<fs::path> backups(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        std::string n = lower(e.path().filename().string());
        if (n.starts_with("openunderworld") && n.ends_with(".appimage.bak") && e.is_regular_file()) out.push_back(e.path());
    }
    return out;
}

void linkOrCopy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_hard_link(from, to, ec); // same inode, no copy; falls back to a copy (e.g. FAT)
    if (ec) fs::copy_file(from, to);
}

// Replaces `from` by `to` at a path boundary (not inside e.g. "<from>.bak").
bool replacePath(std::string& line, const std::string& from, const std::string& to) {
    bool changed = false;
    for (size_t at = line.find(from); at != std::string::npos; at = line.find(from, at)) {
        size_t end = at + from.size();
        if (end < line.size() && line[end] != '"' && line[end] != '\'' && line[end] != ' ' && line[end] != '\t') {
            at = end;
            continue;
        }
        line.replace(at, from.size(), to);
        at += to.size();
        changed = true;
    }
    return changed;
}

// Points every .desktop entry in `apps` whose Exec/TryExec runs one of `from` at `to`.
// ponytail: plain substring match; an Exec with desktop-escaped characters (\$, \") in the path is not followed.
void retargetDesktopEntries(const fs::path& apps, const std::vector<fs::path>& from, const fs::path& to,
                            const std::string& version) {
    std::error_code ec;
    if (apps.empty() || !fs::is_directory(apps, ec)) return;
    const std::string oldStem = lower(from.back().stem().string()), newStem = to.stem().string();
    for (const auto& e : fs::directory_iterator(apps)) {
        if (e.path().extension() != ".desktop" || !e.is_regular_file()) continue;
        std::ifstream in(e.path());
        std::vector<std::string> lines;
        bool launches = false;
        for (std::string l; std::getline(in, l);) {
            if (l.starts_with("Exec=") || l.starts_with("TryExec="))
                for (const auto& f : from) launches = replacePath(l, f.string(), to.string()) || launches;
            lines.push_back(std::move(l));
        }
        if (!launches) continue;
        std::string text;
        for (auto& l : lines) {
            if (l.starts_with("X-AppImage-Version=") && !version.empty()) l = "X-AppImage-Version=" + version;
            text += l + '\n';
        }
        // Rename e.g. openunderworld-0.10.0-x86_64.desktop, keeping the name's case style.
        std::string name = e.path().filename().string();
        if (size_t at = lower(name).find(oldStem); at != std::string::npos && !oldStem.empty()) {
            std::string span = name.substr(at, oldStem.size());
            name.replace(at, span.size(), span == lower(span) ? lower(newStem) : newStem);
        }
        const fs::path dst = apps / name, tmp = apps / ("." + name + ".tmp");
        writeFileSync(tmp, {reinterpret_cast<const uint8_t*>(text.data()), text.size()},
                      mode_t(fs::status(e.path()).permissions() & fs::perms::mask));
        fs::rename(tmp, dst);
        if (dst != e.path()) fs::remove(e.path());
    }
    fsyncDir(apps);
}

// Repoints a symlinked $APPIMAGE at `to` (atomically); returns the path the launcher should run.
fs::path follow(const fs::path& appImage, const fs::path& to) {
    if (!fs::is_symlink(appImage)) return to;
    const fs::path tmp = appImage.string() + ".new";
    fs::remove(tmp);
    fs::create_symlink(to, tmp);
    fs::rename(tmp, appImage);
    return appImage;
}

// Entries pointing at the symlink itself still work; only those naming the real file move.
std::vector<fs::path> launchPaths(const fs::path& appImage, const fs::path& target) {
    if (fs::is_symlink(appImage)) return {target};
    return {fs::absolute(appImage).lexically_normal(), target};
}

} // namespace

fs::path installAppImage(const fs::path& appImage, const Manifest& m, const Fetch& fetch, const fs::path& applicationsDir,
                         const std::function<void(const std::string&)>& progress) {
    const fs::path target = fs::canonical(appImage); // a symlinked AppImage: replace the real file
    if (!fs::is_regular_file(target)) throw Error(target.string() + " is not a regular file");
    auto it = std::find_if(m.files.begin(), m.files.end(),
                           [](const FileEntry& f) { return f.path.ends_with(".AppImage") && f.path.find('/') == std::string::npos; });
    if (it == m.files.end()) throw Error("manifest lists no AppImage");
    // Lock the AppImage itself (no extra files next to it); a second launcher fails fast.
    int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) ::close(fd);
        throw Error(target.string() + " is locked by another update");
    }
    std::unique_ptr<int, void (*)(int*)> unlock(&fd, [](int* f) { ::close(*f); });
    const fs::path dir = target.parent_path(), next = dir / ("OpenUnderworld-" + m.version + "-x86_64.AppImage");
    const fs::path stg = next.string() + ".new", bak = target.string() + ".bak";
    fs::remove(stg); // leftover of an interrupted download
    say(progress, "downloading " + it->path);
    // ponytail: whole AppImage buffered in memory (~100 MB); stream to disk if it grows much larger.
    Bytes data = fetch(it->url, it->size);
    if (data.size() != it->size)
        throw Error(it->path + ": size mismatch (" + std::to_string(data.size()) + " != " + std::to_string(it->size) + ")");
    if (sha256Hex(data) != it->sha256) throw Error(it->path + ": sha256 mismatch");
    try {
        writeFileSync(stg, data, 0755);
        for (const auto& b : backups(dir)) fs::remove(b); // one backup only: the version being replaced
        fs::remove(bak);
        linkOrCopy(target, bak);
        say(progress, "installing " + next.string());
        fs::rename(stg, next); // atomic; until the old name is removed below both names run
    } catch (...) {
        std::error_code ec;
        fs::remove(stg, ec);
        throw;
    }
    fsyncDir(dir);
    if (next == target) return follow(appImage, next);
    retargetDesktopEntries(applicationsDir, launchPaths(appImage, target), next, m.version);
    fs::path run = follow(appImage, next);
    fs::remove(target); // the stale old-named file; its inode lives on as .bak (and for the running process)
    fsyncDir(dir);
    return run;
}

fs::path rollbackAppImage(const fs::path& appImage, const fs::path& applicationsDir) {
    const fs::path cur = fs::canonical(appImage), dir = cur.parent_path();
    fs::path bak = cur.string() + ".bak";
    if (!fs::is_regular_file(bak)) {
        auto found = backups(dir);
        if (found.size() != 1) throw Error("no single OpenUnderworld*.AppImage.bak next to " + cur.string() + " to roll back to");
        bak = found.front();
    }
    const fs::path old = bak.parent_path() / bak.stem(); // X.AppImage.bak -> X.AppImage
    if (old == cur) {
        exchange(bak, cur);
        fsyncDir(dir);
        return follow(appImage, cur);
    }
    const fs::path curBak = cur.string() + ".bak";
    fs::remove(curBak);
    linkOrCopy(cur, curBak);
    fs::rename(bak, old);
    fsyncDir(dir);
    retargetDesktopEntries(applicationsDir, launchPaths(appImage, cur), old, versionInName(old));
    fs::path run = follow(appImage, old);
    fs::remove(cur);
    fsyncDir(dir);
    return run;
}

} // namespace ou::update
