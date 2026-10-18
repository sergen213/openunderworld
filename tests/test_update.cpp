// Updater security tests. No network: a file-backed transport is injected (tests only); it maps
// https://updates.test/<rel> to file://<server dir>/<rel>, so the https-only URL rules stay in force.
#include "config/Config.hpp"
#include "update/Update.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <cstdlib>
#include <fstream>
#include <map>

using namespace ou::update;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kBase = "https://updates.test/";

Bytes bytes(std::string_view s) { return {s.begin(), s.end()}; }
Bytes readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}
void writeAll(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

struct Keys {
    PublicKey pk{};
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    Keys() { EXPECT_GE(sodium_init(), 0); crypto_sign_keypair(pk.data(), sk); }
    Bytes sign(const Bytes& m) const {
        Bytes s(crypto_sign_BYTES);
        crypto_sign_detached(s.data(), nullptr, m.data(), m.size(), sk);
        return s;
    }
};

class UpdateTest : public ::testing::Test {
protected:
    fs::path tmp, srv, root;
    Keys keys, otherKeys;
    int fetches = 0;

    void SetUp() override {
        tmp = fs::temp_directory_path() / ("ou_update_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(tmp);
        srv = tmp / "srv";
        root = tmp / "install";
        fs::create_directories(srv);
        ::unsetenv("OU_DISABLE_UPDATES");
    }
    void TearDown() override { fs::remove_all(tmp); }

    Fetch fileFetch() {
        return [this](const std::string& url, uint64_t maxBytes) -> Bytes {
            ++fetches;
            if (!url.starts_with(kBase)) throw Error("test transport: unexpected url " + url);
            std::string fileUrl = "file://" + srv.string() + "/" + url.substr(kBase.size());
            fs::path p = fileUrl.substr(7);
            if (!fs::exists(p)) throw Error("404 " + url);
            Bytes b = readAll(p);
            if (b.size() > maxBytes) throw Error("transfer exceeds " + std::to_string(maxBytes) + " bytes");
            return b;
        };
    }

    // Publishes files under srv/<version>/ and returns the manifest JSON (not yet signed).
    nlohmann::json release(const std::string& version, const std::map<std::string, std::string>& files,
                           const std::string& minVersion = "") {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [path, content] : files) {
            writeAll(srv / version / path, bytes(content));
            list.push_back({{"path", path}, {"size", content.size()}, {"sha256", sha256Hex(bytes(content))},
                            {"url", std::string(kBase) + version + "/" + path}, {"executable", path.starts_with("bin/")}});
        }
        return {{"version", version}, {"channel", "stable"}, {"changelog", "release " + version},
                {"min_version", minVersion}, {"files", list}};
    }
    // Writes manifest + signature to the server; returns the update options pointing at them.
    Options publish(const std::string& version, const Bytes& manifest, const Bytes& sig) {
        writeAll(srv / version / "manifest.json", manifest);
        writeAll(srv / version / "manifest.json.sig", sig);
        Options o;
        o.root = root;
        o.manifestUrl = std::string(kBase) + version + "/manifest.json";
        o.key = keys.pk;
        o.fetch = fileFetch();
        return o;
    }
    Options publish(const std::string& version, const nlohmann::json& m) {
        Bytes b = bytes(m.dump());
        return publish(version, b, keys.sign(b));
    }
    Options good(const std::string& version) { return publish(version, release(version, {{"bin/ou-viewer", "viewer " + version}, {"share/readme.txt", "hi"}})); }

    std::string text(const fs::path& p) { Bytes b = readAll(p); return {b.begin(), b.end()}; }
    // A fake installed version directory, as the updater leaves it.
    void fakeVersion(const fs::path& dir, const std::string& v) {
        writeAll(dir / ".ou-manifest.json", bytes(nlohmann::json{{"version", v}}.dump()));
        writeAll(dir / "bin/ou-viewer", bytes("viewer " + v));
    }
    void fakeRoot() { writeAll(root / ".openunderworld-install", bytes("x")); }
};

TEST_F(UpdateTest, GoodSignatureInstalls) {
    Result r = update(good("0.2.0"));
    EXPECT_EQ(r.status, Status::Installed);
    EXPECT_FALSE(r.installedVersion);
    EXPECT_EQ(installedVersion(root), "0.2.0");
    EXPECT_EQ(text(root / "current/bin/ou-viewer"), "viewer 0.2.0");
    EXPECT_NE(fs::status(root / "current/bin/ou-viewer").permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_EQ(fs::status(root / "current/share/readme.txt").permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_TRUE(fs::exists(root / "current/.ou-manifest.json.sig"));
    EXPECT_FALSE(fs::exists(root / ".journal"));
    EXPECT_FALSE(fs::exists(root / ".staging"));
    EXPECT_EQ(r.manifest.changelog, "release 0.2.0");

    // Second update keeps the first as backup.
    EXPECT_EQ(update(good("0.3.0")).status, Status::Installed);
    EXPECT_EQ(installedVersion(root), "0.3.0");
    EXPECT_EQ(versionOf(root / "previous"), "0.2.0");
    // Same version again: up to date, nothing downloaded beyond the manifest.
    EXPECT_EQ(update(good("0.3.0")).status, Status::UpToDate);
}

TEST_F(UpdateTest, BadSignatureRejected) {
    nlohmann::json m = release("0.2.0", {{"bin/ou-viewer", "evil"}});
    Bytes b = bytes(m.dump());
    Options o = publish("0.2.0", b, otherKeys.sign(b)); // signed by an untrusted key
    EXPECT_THROW(update(o), Error);
    Bytes truncated = keys.sign(b);
    truncated.pop_back();
    EXPECT_THROW(update(publish("0.2.0", b, truncated)), Error);
    EXPECT_FALSE(fs::exists(root / "current"));
    EXPECT_EQ(fetches, 4); // manifest + sig twice; no payload was ever fetched
}

TEST_F(UpdateTest, TamperedManifestRejected) {
    nlohmann::json m = release("0.2.0", {{"bin/ou-viewer", "good"}});
    Bytes b = bytes(m.dump());
    Bytes sig = keys.sign(b);
    m["files"][0]["sha256"] = sha256Hex(bytes("evil"));
    m["files"][0]["size"] = 4;
    writeAll(srv / "0.2.0/bin/ou-viewer", bytes("evil"));
    EXPECT_THROW(update(publish("0.2.0", bytes(m.dump()), sig)), Error);
    Bytes flipped = b;
    flipped[flipped.size() / 2] ^= 1;
    EXPECT_THROW(update(publish("0.2.0", flipped, sig)), Error);
    EXPECT_FALSE(fs::exists(root / "current"));
}

TEST_F(UpdateTest, SizeAndHashMismatchLeaveCurrentUntouched) {
    update(good("0.2.0"));
    // Size: the manifest promises fewer bytes than the server sends (transport cap) ...
    nlohmann::json m = release("0.3.0", {{"bin/ou-viewer", "viewer 0.3.0"}});
    m["files"][0]["size"] = 3;
    EXPECT_THROW(update(publish("0.3.0", m)), Error);
    // ... or more bytes than it sends.
    m["files"][0]["size"] = 400;
    EXPECT_THROW(update(publish("0.3.0", m)), Error);
    // Hash: right size, wrong content.
    m = release("0.3.0", {{"bin/ou-viewer", "viewer 0.3.0"}});
    writeAll(srv / "0.3.0/bin/ou-viewer", bytes("VIEWER 0.3.0"));
    EXPECT_THROW(update(publish("0.3.0", m)), Error);
    EXPECT_EQ(installedVersion(root), "0.2.0");
    EXPECT_EQ(text(root / "current/bin/ou-viewer"), "viewer 0.2.0");
    EXPECT_FALSE(fs::exists(root / ".staging"));
}

TEST_F(UpdateTest, RollbackAttemptRefused) {
    update(good("0.3.0"));
    EXPECT_THROW(check(good("0.2.0")), Error);  // properly signed but older
    EXPECT_THROW(update(good("0.2.10")), Error);
    // Direct install of an older version is refused too.
    nlohmann::json m = release("0.1.0", {{"bin/ou-viewer", "old"}});
    Bytes b = bytes(m.dump());
    EXPECT_THROW(install(root, parseManifest(m.dump()), b, keys.sign(b), fileFetch()), Error);
    EXPECT_EQ(installedVersion(root), "0.3.0");
    // min_version: an install older than min_version must not take the shortcut.
    EXPECT_THROW(update(publish("0.5.0", release("0.5.0", {{"bin/ou-viewer", "x"}}, "0.4.0"))), Error);
    EXPECT_EQ(installedVersion(root), "0.3.0");
}

TEST_F(UpdateTest, UserRollbackToPrevious) {
    update(good("0.2.0"));
    update(good("0.3.0"));
    EXPECT_EQ(rollback(root), "0.2.0");
    EXPECT_EQ(text(root / "current/bin/ou-viewer"), "viewer 0.2.0");
    EXPECT_EQ(versionOf(root / "previous"), "0.3.0");
}

TEST_F(UpdateTest, AppImageSelfUpdate) {
    const fs::path app = tmp / "apps/OpenUnderworld.AppImage", bak = app.string() + ".bak", stg = app.string() + ".new";
    writeAll(app, bytes("appimage 0.1.0"));
    fs::permissions(app, fs::perms(0755));
    auto appOpts = [&](const std::string& v, const std::string& running) {
        Options o = publish(v, release(v, {{"OpenUnderworld-" + v + "-x86_64.AppImage", "appimage " + v}}));
        o.appImage = app;
        o.runningVersion = running;
        return o;
    };
    EXPECT_EQ(check(appOpts("0.1.0", "0.1.0")).status, Status::UpToDate);
    EXPECT_THROW(check(appOpts("0.0.9", "0.1.0")), Error); // signed but older: rollback refused

    // Hash mismatch: the running AppImage stays, no staging file is left.
    Options bad = appOpts("0.2.0", "0.1.0");
    writeAll(srv / "0.2.0/OpenUnderworld-0.2.0-x86_64.AppImage", bytes("APPIMAGE 0.2.0"));
    EXPECT_THROW(update(bad), Error);
    EXPECT_EQ(text(app), "appimage 0.1.0");
    EXPECT_FALSE(fs::exists(stg));

    // Good update: atomic replace, old file kept as .bak, install root never created.
    writeAll(stg, bytes("stale partial download"));
    Result r = update(appOpts("0.2.0", "0.1.0"));
    EXPECT_EQ(r.status, Status::Installed);
    EXPECT_EQ(r.installedVersion, "0.1.0");
    EXPECT_EQ(text(app), "appimage 0.2.0");
    EXPECT_NE(fs::status(app).permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_EQ(text(bak), "appimage 0.1.0");
    EXPECT_FALSE(fs::exists(stg));
    EXPECT_FALSE(fs::exists(root));

    // A symlink to the AppImage updates the real file and keeps the link.
    fs::create_symlink(app, tmp / "link.AppImage");
    Options viaLink = appOpts("0.3.0", "0.2.0");
    viaLink.appImage = tmp / "link.AppImage";
    EXPECT_EQ(update(viaLink).status, Status::Installed);
    EXPECT_TRUE(fs::is_symlink(tmp / "link.AppImage"));
    EXPECT_EQ(text(app), "appimage 0.3.0");

    rollbackAppImage(app);
    EXPECT_EQ(text(app), "appimage 0.2.0");
    EXPECT_EQ(text(bak), "appimage 0.3.0");

    // A manifest without an AppImage entry is refused in AppImage mode.
    Options noApp = publish("0.4.0", release("0.4.0", {{"bin/ou-viewer", "viewer"}}));
    noApp.appImage = app;
    noApp.runningVersion = "0.2.0";
    EXPECT_THROW(update(noApp), Error);
    EXPECT_EQ(text(app), "appimage 0.2.0");
}

TEST_F(UpdateTest, InterruptedSwapRecovery) {
    // Crash after staging + journal, before anything moved: roll forward.
    fakeRoot();
    fakeVersion(root / "previous", "0.1.0");
    fakeVersion(root / "current", "0.2.0");
    fakeVersion(root / ".staging", "0.3.0");
    writeAll(root / ".journal", bytes(R"({"version":"0.3.0"})"));
    EXPECT_NE(recover(root), "");
    EXPECT_EQ(installedVersion(root), "0.3.0");
    EXPECT_EQ(versionOf(root / "previous"), "0.2.0");
    EXPECT_FALSE(fs::exists(root / ".staging"));
    EXPECT_FALSE(fs::exists(root / ".journal"));
    EXPECT_EQ(recover(root), ""); // idempotent

    // Crash after the exchange, before the old version became previous.
    fs::remove_all(root / "previous");
    fakeVersion(root / ".staging", "0.2.0");
    writeAll(root / ".journal", bytes(R"({"version":"0.3.0"})"));
    recover(root);
    EXPECT_EQ(installedVersion(root), "0.3.0");
    EXPECT_EQ(versionOf(root / "previous"), "0.2.0");

    // Crash after removing the old backup into .trash.
    fakeVersion(root / ".staging", "0.4.0");
    fs::rename(root / "previous", root / ".trash");
    writeAll(root / ".journal", bytes(R"({"version":"0.4.0"})"));
    recover(root);
    EXPECT_EQ(installedVersion(root), "0.4.0");
    EXPECT_EQ(versionOf(root / "previous"), "0.3.0");
    EXPECT_FALSE(fs::exists(root / ".trash"));

    // Journal but incomplete staging (no manifest): abandon, keep running version.
    fs::create_directories(root / ".staging/bin");
    writeAll(root / ".journal", bytes(R"({"version":"0.5.0"})"));
    EXPECT_NE(recover(root).find("abandoned"), std::string::npos);
    EXPECT_EQ(installedVersion(root), "0.4.0");
    EXPECT_FALSE(fs::exists(root / ".staging"));

    // Crash during download (no journal): staging discarded.
    fakeVersion(root / ".staging", "0.6.0");
    recover(root);
    EXPECT_EQ(installedVersion(root), "0.4.0");
    EXPECT_FALSE(fs::exists(root / ".staging"));

    // An actual update after recovery still works.
    EXPECT_EQ(update(good("0.7.0")).status, Status::Installed);
    EXPECT_EQ(versionOf(root / "previous"), "0.4.0");
}

TEST_F(UpdateTest, DisableSwitch) {
    Options o = good("0.2.0");
    o.enabled = false;
    EXPECT_EQ(update(o).status, Status::Disabled);
    o.enabled = true;
    ::setenv("OU_DISABLE_UPDATES", "1", 1);
    EXPECT_EQ(update(o).status, Status::Disabled);
    EXPECT_EQ(check(o).status, Status::Disabled);
    ::setenv("OU_DISABLE_UPDATES", "0", 1);
    EXPECT_EQ(check(o).status, Status::Available);
    ::unsetenv("OU_DISABLE_UPDATES");
    EXPECT_EQ(fetches, 2); // only the last check touched the transport
    EXPECT_FALSE(fs::exists(root / "current"));
}

TEST_F(UpdateTest, OnlyProjectFilesAndOwnedRoots) {
    auto bad = [&](nlohmann::json f) {
        nlohmann::json m = release("0.2.0", {{"bin/ou-viewer", "x"}});
        m["files"].push_back(f);
        return m.dump();
    };
    std::string h = sha256Hex(bytes("x"));
    for (std::string p : {"../escape", "/abs", "a//b", "a/./b", "bigfile.000", "pc-w/lara.drm", "tru.exe", "x\\y", ".ou-manifest.json"})
        EXPECT_THROW(parseManifest(bad({{"path", p}, {"size", 1}, {"sha256", h}, {"url", "https://updates.test/x"}})), Error) << p;
    EXPECT_THROW(parseManifest(bad({{"path", "y"}, {"size", 1}, {"sha256", h}, {"url", "http://updates.test/x"}})), Error);
    EXPECT_THROW(parseManifest(bad({{"path", "y"}, {"size", 1}, {"sha256", h}, {"url", "file:///etc/passwd"}})), Error);
    EXPECT_THROW(parseManifest(bad({{"path", "bin/ou-viewer"}, {"size", 1}, {"sha256", h}, {"url", "https://updates.test/x"}})), Error); // duplicate
    EXPECT_THROW(parseManifest(bad({{"path", "y"}, {"size", -1}, {"sha256", h}, {"url", "https://updates.test/x"}})), Error);
    EXPECT_THROW(parseManifest(R"({"version":"1.x","channel":"stable","files":[]})"), Error);

    // A non-empty root that isn't ours, or one overlapping the game directory, is refused.
    writeAll(root / "somebody-elses.txt", bytes("x"));
    EXPECT_THROW(update(good("0.2.0")), Error);
    EXPECT_TRUE(fs::exists(root / "somebody-elses.txt"));
    fs::remove_all(root);
    Options o = good("0.2.0");
    o.forbiddenRoots = {tmp};
    EXPECT_THROW(update(o), Error);
    EXPECT_FALSE(fs::exists(root));
}

TEST(UpdateCore, VersionsAndKeys) {
    EXPECT_LT(compareVersions("0.2.9", "0.2.10"), 0);
    EXPECT_EQ(compareVersions("1.0", "1.0.0"), 0);
    EXPECT_GT(compareVersions("2", "1.99.99"), 0);
    EXPECT_THROW(compareVersions("1.-1", "1"), Error);
    EXPECT_THROW(compareVersions("", "1"), Error);
    EXPECT_FALSE(parsePublicKeyHex("abc"));
    EXPECT_TRUE(parsePublicKeyHex(std::string(64, 'a')));
    if (auto k = releasePublicKey()) { EXPECT_NE(*k, PublicKey{}); }
}

TEST(UpdateCore, HttpsTransportRefusesOtherSchemes) {
    Fetch f = httpsFetch(); // rejected before any connection attempt
    EXPECT_THROW(f("http://example.invalid/manifest.json", 10), Error);
    EXPECT_THROW(f("file:///etc/passwd", 10), Error);
    EXPECT_THROW(f("ftp://example.invalid/x", 10), Error);
}

TEST(Config, RoundTripAndDefaults) {
    fs::path p = fs::temp_directory_path() / ("ou_config_" + std::to_string(::getpid()) + "/config.json");
    EXPECT_EQ(ou::config::load(p).width, 1280); // missing -> defaults
    EXPECT_EQ(ou::config::load(p).updateManifestUrl,
              "https://github.com/sergen213/openunderworld/releases/latest/download/manifest.json");
    ou::config::Config c;
    c.gameDir = "/games/tru";
    c.renderer = "vulkan";
    c.width = 2560, c.height = 1440, c.fullscreen = true, c.fov = 75, c.vsync = false;
    c.updatesEnabled = false, c.updateManifestUrl = "https://example.invalid/m.json";
    ou::config::save(p, c);
    auto d = ou::config::load(p);
    EXPECT_EQ(d.gameDir, c.gameDir);
    EXPECT_EQ(d.renderer, "vulkan");
    EXPECT_EQ(d.width, 2560);
    EXPECT_TRUE(d.fullscreen);
    EXPECT_FALSE(d.vsync);
    EXPECT_FALSE(d.updatesEnabled);
    EXPECT_EQ(d.updateManifestUrl, c.updateManifestUrl);
    std::ofstream(p) << R"({"renderer":"d3d9"})";
    EXPECT_THROW(ou::config::load(p), std::runtime_error);
    fs::remove_all(p.parent_path());
}

TEST(Config, GraphicsPresets) {
    using ou::config::GraphicsSettings;
    using ou::config::resolve;
    using Opt = std::optional<bool>;
    auto g = [](std::string preset, int msaa = 0, Opt shadows = {}, Opt fxaa = {}, Opt hdr = {}) {
        return ou::config::GraphicsConfig{std::move(preset), msaa, shadows, fxaa, hdr};
    };
    EXPECT_EQ(resolve({}), (GraphicsSettings{1, false, false, false}));                // original
    EXPECT_EQ(resolve(g("enhanced")), (GraphicsSettings{4, true, false, false}));    // 4x MSAA + shadows
    EXPECT_EQ(resolve(g("enhanced", 1, false, true, true)), (GraphicsSettings{1, false, true, true})); // overrides
    EXPECT_EQ(resolve(g("original", 8, true)), (GraphicsSettings{8, true, false, false}));
    EXPECT_THROW(resolve(g("ultra")), std::runtime_error);
    EXPECT_THROW(resolve(g("original", 3)), std::runtime_error);
    // Ray tracing: enhanced only; quality = shadow rays per pixel.
    auto rt = [&](std::string preset, std::string mode, std::string quality = "medium") {
        auto c = g(std::move(preset));
        c.rt = std::move(mode), c.rtQuality = std::move(quality);
        return c;
    };
    EXPECT_EQ(resolve(rt("enhanced", "shadows")), (GraphicsSettings{4, true, false, false, 1, 2}));
    EXPECT_EQ(resolve(rt("enhanced", "full", "high")), (GraphicsSettings{4, true, false, false, 2, 4}));
    EXPECT_EQ(resolve(rt("enhanced", "off", "low")), (GraphicsSettings{4, true, false, false, 0, 1}));
    EXPECT_THROW(resolve(rt("original", "shadows")), std::runtime_error);
    EXPECT_THROW(resolve(rt("enhanced", "on")), std::runtime_error);
    EXPECT_THROW(resolve(rt("enhanced", "full", "ultra")), std::runtime_error);
    ou::config::GraphicsConfig flags;
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--rt", "full"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--rt-quality", "low"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--shadows", "off"));
    EXPECT_FALSE(ou::config::applyGraphicsFlag(flags, "--level", "x"));
    EXPECT_THROW(ou::config::applyGraphicsFlag(flags, "--fxaa", "yes"), std::runtime_error);
    EXPECT_EQ(flags.rt, "full");
    EXPECT_EQ(flags.rtQuality, "low");
    EXPECT_EQ(flags.shadows, std::optional<bool>(false));

    fs::path p = fs::temp_directory_path() / ("ou_config_gfx_" + std::to_string(::getpid()) + "/config.json");
    ou::config::Config c;
    c.graphics = g("enhanced", 2, std::nullopt, true);
    c.graphics.rt = "shadows", c.graphics.rtQuality = "high";
    ou::config::save(p, c);
    const auto d = ou::config::load(p).graphics;
    EXPECT_EQ(d.preset, "enhanced");
    EXPECT_EQ(d.msaa, 2);
    EXPECT_FALSE(d.shadows.has_value()); // unset stays unset: the preset decides
    EXPECT_EQ(d.fxaa, std::optional<bool>(true));
    EXPECT_EQ(resolve(d), (GraphicsSettings{2, true, true, false, 1, 4}));
    std::ofstream(p) << R"({"graphics":{"preset":"enhanced","msaa":5}})";
    EXPECT_THROW(ou::config::load(p), std::runtime_error);
    fs::remove_all(p.parent_path());
}

} // namespace
