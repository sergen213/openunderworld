// Updater security tests. No network: a file-backed transport is injected (tests only); it maps
// https://updates.test/<rel> to file://<server dir>/<rel>, so the https-only URL rules stay in force.
#include "config/Config.hpp"
#include "update/Update.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
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

// An AppImage named like the user's (an integrator-made desktop entry included), updated in place.
class AppImageUpdateTest : public UpdateTest {
protected:
    fs::path apps, dir, desktop;
    fs::path named(const std::string& v) { return dir / ("OpenUnderworld-" + v + "-x86_64.AppImage"); }
    void SetUp() override {
        UpdateTest::SetUp();
        dir = tmp / "bin";
        apps = tmp / "share/applications";
        writeAll(named("0.1.0"), bytes("appimage 0.1.0"));
        fs::permissions(named("0.1.0"), fs::perms(0755));
        desktop = apps / "openunderworld-0.1.0-x86_64.desktop";
        writeAll(desktop, bytes(entry(named("0.1.0"), "0.1.0")));
        writeAll(apps / "other.desktop", bytes("[Desktop Entry]\nExec=/opt/other/OpenUnderworld-0.1.0-x86_64.AppImage\n"));
        writeAll(apps / "bak.desktop", bytes("[Desktop Entry]\nExec=" + named("0.1.0").string() + ".bak\n"));
    }
    static std::string entry(const fs::path& app, const std::string& v) {
        return "[Desktop Entry]\nType=Application\nName=OpenUnderworld\nExec=\"" + app.string() + "\" %U\nIcon=openunderworld-0.1.0-x86_64\n"
               "X-AppImage-Version=" + v + "\n\nTryExec=" + app.string() + "\n";
    }
    Options appOpts(const std::string& v, const std::string& running, const fs::path& app) {
        Options o = publish(v, release(v, {{"OpenUnderworld-" + v + "-x86_64.AppImage", "appimage " + v}}));
        o.appImage = app;
        o.runningVersion = running;
        o.applicationsDir = apps;
        return o;
    }
    std::vector<std::string> files(const fs::path& d) {
        std::vector<std::string> out;
        for (const auto& e : fs::directory_iterator(d)) out.push_back(e.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    }
};

TEST_F(AppImageUpdateTest, RefusalsLeaveTheAppImageAlone) {
    const fs::path app = named("0.1.0");
    EXPECT_EQ(check(appOpts("0.1.0", "0.1.0", app)).status, Status::UpToDate);
    EXPECT_THROW(check(appOpts("0.0.9", "0.1.0", app)), Error); // signed but older: rollback refused
    // Hash mismatch: the running AppImage stays, no staging file is left.
    Options bad = appOpts("0.2.0", "0.1.0", app);
    writeAll(srv / "0.2.0/OpenUnderworld-0.2.0-x86_64.AppImage", bytes("APPIMAGE 0.2.0"));
    EXPECT_THROW(update(bad), Error);
    // A manifest without an AppImage entry is refused in AppImage mode.
    Options noApp = publish("0.4.0", release("0.4.0", {{"bin/ou-viewer", "viewer"}}));
    noApp.appImage = app;
    noApp.runningVersion = "0.1.0";
    EXPECT_THROW(update(noApp), Error);
    EXPECT_EQ(files(dir), std::vector<std::string>{"OpenUnderworld-0.1.0-x86_64.AppImage"});
    EXPECT_EQ(text(app), "appimage 0.1.0");
    EXPECT_EQ(text(desktop), entry(app, "0.1.0"));
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(AppImageUpdateTest, UpdateRenamesAndRetargetsDesktopEntry) {
    const fs::path old = named("0.1.0"), next = named("0.2.0");
    writeAll(next.string() + ".new", bytes("stale partial download"));
    writeAll(dir / "OpenUnderworld-0.0.5-x86_64.AppImage.bak", bytes("ancient backup"));
    writeAll(dir / "notes.bak", bytes("not ours"));
    Result r = update(appOpts("0.2.0", "0.1.0", old));
    EXPECT_EQ(r.status, Status::Installed);
    EXPECT_EQ(r.installedVersion, "0.1.0");
    EXPECT_EQ(r.appImage, next);
    EXPECT_EQ(text(next), "appimage 0.2.0");
    EXPECT_NE(fs::status(next).permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_EQ(text(old.string() + ".bak"), "appimage 0.1.0");
    EXPECT_EQ(files(dir), (std::vector<std::string>{"OpenUnderworld-0.1.0-x86_64.AppImage.bak", "OpenUnderworld-0.2.0-x86_64.AppImage", "notes.bak"}));
    EXPECT_FALSE(fs::exists(root));

    // The entry is rewritten and renamed; Icon and unrelated entries stay.
    const fs::path moved = apps / "openunderworld-0.2.0-x86_64.desktop";
    EXPECT_FALSE(fs::exists(desktop));
    EXPECT_EQ(text(moved), "[Desktop Entry]\nType=Application\nName=OpenUnderworld\nExec=\"" + next.string() +
                               "\" %U\nIcon=openunderworld-0.1.0-x86_64\nX-AppImage-Version=0.2.0\n\nTryExec=" + next.string() + "\n");
    EXPECT_EQ(text(apps / "other.desktop"), "[Desktop Entry]\nExec=/opt/other/OpenUnderworld-0.1.0-x86_64.AppImage\n");
    EXPECT_EQ(text(apps / "bak.desktop"), "[Desktop Entry]\nExec=" + old.string() + ".bak\n");

    // Next update from the new path: one backup only (the 0.2.0 file).
    EXPECT_EQ(update(appOpts("0.3.0", "0.2.0", next)).appImage, named("0.3.0"));
    EXPECT_EQ(files(dir), (std::vector<std::string>{"OpenUnderworld-0.2.0-x86_64.AppImage.bak", "OpenUnderworld-0.3.0-x86_64.AppImage", "notes.bak"}));
    EXPECT_TRUE(fs::exists(apps / "openunderworld-0.3.0-x86_64.desktop"));
}

TEST_F(AppImageUpdateTest, RollbackRestoresOldNameAndDesktopEntry) {
    const fs::path old = named("0.1.0"), next = named("0.2.0");
    EXPECT_EQ(update(appOpts("0.2.0", "0.1.0", old)).appImage, next);
    // The launcher was not restarted: it rolls back from the path update() returned.
    EXPECT_EQ(rollbackAppImage(next, apps), old);
    EXPECT_EQ(files(dir), (std::vector<std::string>{"OpenUnderworld-0.1.0-x86_64.AppImage", "OpenUnderworld-0.2.0-x86_64.AppImage.bak"}));
    EXPECT_EQ(text(old), "appimage 0.1.0");
    EXPECT_EQ(text(next.string() + ".bak"), "appimage 0.2.0");
    EXPECT_EQ(text(desktop), entry(old, "0.1.0"));
    EXPECT_FALSE(fs::exists(apps / "openunderworld-0.2.0-x86_64.desktop"));
    // Again: re-applies the newer one.
    EXPECT_EQ(rollbackAppImage(old, apps), next);
    EXPECT_EQ(text(next), "appimage 0.2.0");
    EXPECT_TRUE(fs::exists(apps / "openunderworld-0.2.0-x86_64.desktop"));
    EXPECT_EQ(files(dir), (std::vector<std::string>{"OpenUnderworld-0.1.0-x86_64.AppImage.bak", "OpenUnderworld-0.2.0-x86_64.AppImage"}));
    EXPECT_THROW(rollbackAppImage(tmp / "missing.AppImage", apps), std::exception);
}

TEST_F(AppImageUpdateTest, CustomNameAndSymlink) {
    // A user-named file is renamed too; its backup keeps the user's name.
    const fs::path custom = dir / "openunderworld.AppImage";
    fs::rename(named("0.1.0"), custom);
    EXPECT_EQ(update(appOpts("0.2.0", "0.1.0", custom)).appImage, named("0.2.0"));
    EXPECT_EQ(files(dir), (std::vector<std::string>{"OpenUnderworld-0.2.0-x86_64.AppImage", "openunderworld.AppImage.bak"}));
    EXPECT_EQ(rollbackAppImage(named("0.2.0"), apps), custom);
    EXPECT_EQ(text(custom), "appimage 0.1.0");
    // A symlinked $APPIMAGE is repointed and stays the path to run; legacy same-name backups still roll back.
    fs::create_symlink(custom, tmp / "link.AppImage");
    EXPECT_EQ(update(appOpts("0.3.0", "0.1.0", tmp / "link.AppImage")).appImage, tmp / "link.AppImage");
    EXPECT_TRUE(fs::is_symlink(tmp / "link.AppImage"));
    EXPECT_EQ(fs::read_symlink(tmp / "link.AppImage"), named("0.3.0"));
    EXPECT_EQ(text(tmp / "link.AppImage"), "appimage 0.3.0");
    EXPECT_EQ(rollbackAppImage(tmp / "link.AppImage", apps), tmp / "link.AppImage");
    EXPECT_EQ(text(tmp / "link.AppImage"), "appimage 0.1.0");
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
    EXPECT_EQ(ou::config::Config{}.renderer, "auto");
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
    std::ofstream(p) << R"({"renderer":"auto"})";
    EXPECT_EQ(ou::config::load(p).renderer, "auto");
    fs::remove_all(p.parent_path());
}

TEST(Config, ControlsOverridePerActionAndRoundTrip) {
    fs::path p = fs::temp_directory_path() / ("ou_config_ctl_" + std::to_string(::getpid()) + "/config.json");
    fs::create_directories(p.parent_path());
    const ou::config::InputConfig defaults;
    // A file binding only `jump` keeps every other action's defaults.
    std::ofstream(p) << R"({"controls":{"invert_y":true,"mouse_sensitivity":2.5,"bindings":{"jump":["J","Pad y"]}}})";
    auto c = ou::config::load(p);
    EXPECT_EQ(c.controls.bindings.at("jump"), (std::vector<std::string>{"J", "Pad y"}));
    EXPECT_EQ(c.controls.bindings.at("drop"), defaults.bindings.at("drop"));
    EXPECT_TRUE(c.controls.invertY);
    EXPECT_FLOAT_EQ(c.controls.mouseSensitivity, 2.5f);
    c.controls.bindings["save"] = {}; // unbound survives a save/load
    ou::config::save(p, c);
    const auto d = ou::config::load(p);
    EXPECT_EQ(d.controls.bindings, c.controls.bindings);
    EXPECT_TRUE(d.controls.bindings.at("save").empty());
    std::ofstream(p) << R"({"controls":{"bindings":{"fly":["Space"]}}})";
    EXPECT_THROW(ou::config::load(p), std::runtime_error);
    std::ofstream(p) << R"({"controls":{"mouse_sensitivity":0}})";
    EXPECT_THROW(ou::config::load(p), std::runtime_error);
    fs::remove_all(p.parent_path());
}

// Pre-0.12 configs saved every binding; an action still on its old default (Shift walk, Alt sprint) takes today's
// default (tru.exe's), a rebound one is kept.
TEST(Config, ControlsMigrateOldDefaults) {
    fs::path p = fs::temp_directory_path() / ("ou_config_mig_" + std::to_string(::getpid()) + "/config.json");
    fs::create_directories(p.parent_path());
    const ou::config::InputConfig defaults;
    std::ofstream(p) << R"({"controls":{"bindings":{"walk":["Left Shift","Right Shift","Pad leftshoulder"],
        "sprint":["Left Alt","Pad leftstick"],"pda":["1"],"light":["L","Pad dpleft"],"grapple":["G"]}}})";
    const auto c = ou::config::load(p);
    EXPECT_EQ(c.controls.bindings.at("walk"), std::vector<std::string>{"Left Alt"});
    EXPECT_EQ(c.controls.bindings.at("sprint"), (std::vector<std::string>{"Left Shift", "Pad leftshoulder"}));
    EXPECT_EQ(c.controls.bindings.at("pda"), defaults.bindings.at("pda"));
    EXPECT_EQ(c.controls.bindings.at("light"), (std::vector<std::string>{"F", "Pad dpleft"}));
    EXPECT_EQ(c.controls.bindings.at("grapple"), std::vector<std::string>{"G"}); // the user's own
    fs::remove_all(p.parent_path());
}

TEST(Config, GraphicsPresets) {
    using ou::config::GraphicsSettings;
    using ou::config::resolve;
    using Opt = std::optional<bool>;
    auto g = [](std::string preset, int msaa = 0, Opt shadows = {}, Opt fxaa = {}, Opt hdr = {}) {
        return ou::config::GraphicsConfig{std::move(preset), msaa, shadows, fxaa, hdr};
    };
    // Default = enhanced: 4x MSAA + shadows + rt auto -> shadows, 2 rays, original shaders, neutral brightness.
    EXPECT_EQ(resolve({}), (GraphicsSettings{4, true, false, false, 1, 2, true, 1, 1.0f, true, 2, true, 1}));
    EXPECT_EQ(resolve(g("enhanced")), (GraphicsSettings{4, true, false, false, 1, 2, true, 1, 1.0f, true, 2, true, 1}));
    EXPECT_EQ(resolve(g("enhanced", 1, false, true, true)), (GraphicsSettings{1, false, true, true, 1, 2, true, 1, 1.0f, true, 2, true, 1})); // overrides
    EXPECT_EQ(resolve(g("original")), (GraphicsSettings{1, false, false, false, 0})); // purist: rt auto -> off
    EXPECT_EQ(resolve(g("original", 8, true)), (GraphicsSettings{8, true, false, false, 0}));
    EXPECT_THROW(resolve(g("ultra")), std::runtime_error);
    EXPECT_THROW(resolve(g("original", 3)), std::runtime_error);
    // Ray tracing: enhanced only; quality = shadow rays per pixel.
    auto rt = [&](std::string preset, std::string mode, std::string quality = "medium") {
        auto c = g(std::move(preset));
        c.rt = std::move(mode), c.rtQuality = std::move(quality);
        return c;
    };
    EXPECT_EQ(resolve(rt("enhanced", "shadows")), (GraphicsSettings{4, true, false, false, 1, 2, true, 1, 1.0f, true, 2, true, 1}));
    EXPECT_EQ(resolve(rt("enhanced", "full", "high")), (GraphicsSettings{4, true, false, false, 2, 4, true, 1, 1.0f, true, 2, true, 1}));
    EXPECT_EQ(resolve(rt("enhanced", "off", "low")), (GraphicsSettings{4, true, false, false, 0, 1, true, 1, 1.0f, true, 2, true, 1}));
    EXPECT_THROW(resolve(rt("original", "shadows")), std::runtime_error);
    EXPECT_THROW(resolve(rt("original", "full")), std::runtime_error);
    EXPECT_EQ(resolve(rt("original", "auto")).rt, 0);
    EXPECT_EQ(resolve(rt("enhanced", "auto", "high")), (GraphicsSettings{4, true, false, false, 1, 4, true, 1, 1.0f, true, 2, true, 1}));
    auto bright = g("enhanced");
    bright.brightness = 0.2f;
    EXPECT_THROW(resolve(bright), std::runtime_error);
    bright.brightness = 2.5f;
    EXPECT_THROW(resolve(bright), std::runtime_error);
    bright.brightness = 1.5f;
    EXPECT_FLOAT_EQ(resolve(bright).brightness, 1.5f);
    EXPECT_THROW(resolve(rt("enhanced", "on")), std::runtime_error);
    EXPECT_THROW(resolve(rt("enhanced", "full", "ultra")), std::runtime_error);
    ou::config::GraphicsConfig flags;
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--rt", "full"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--rt-quality", "low"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--shadows", "off"));
    EXPECT_FALSE(ou::config::applyGraphicsFlag(flags, "--level", "x"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--brightness", "1.25"));
    EXPECT_FLOAT_EQ(flags.brightness, 1.25f);
    EXPECT_THROW(ou::config::applyGraphicsFlag(flags, "--fxaa", "yes"), std::runtime_error);
    EXPECT_EQ(flags.rt, "full");
    EXPECT_EQ(flags.rtQuality, "low");
    EXPECT_EQ(flags.shadows, std::optional<bool>(false));

    fs::path p = fs::temp_directory_path() / ("ou_config_gfx_" + std::to_string(::getpid()) + "/config.json");
    ou::config::Config c;
    c.graphics = g("enhanced", 2, std::nullopt, true);
    c.graphics.rt = "shadows", c.graphics.rtQuality = "high", c.graphics.brightness = 0.75f;
    c.graphics.atmosphere = "high", c.graphics.lightShafts = false;
    ou::config::save(p, c);
    const auto d = ou::config::load(p).graphics;
    EXPECT_EQ(d.preset, "enhanced");
    EXPECT_EQ(d.msaa, 2);
    EXPECT_FALSE(d.shadows.has_value()); // unset stays unset: the preset decides
    EXPECT_EQ(d.fxaa, std::optional<bool>(true));
    EXPECT_FLOAT_EQ(d.brightness, 0.75f);
    EXPECT_EQ(d.atmosphere, "high");
    EXPECT_EQ(d.lightShafts, std::optional<bool>(false));
    EXPECT_EQ(resolve(d), (GraphicsSettings{2, true, true, false, 1, 4, true, 1, 0.75f, true, 3, false, 1}));
    std::ofstream(p) << R"({"graphics":{"brightness":0.2}})";
    EXPECT_THROW(ou::config::load(p), std::runtime_error);
    std::ofstream(p) << R"({"graphics":{"preset":"enhanced","msaa":5}})";
    EXPECT_THROW(ou::config::load(p), std::runtime_error);
    fs::remove_all(p.parent_path());
    // Water detail, atmosphere, weather: auto = on for enhanced (medium atmosphere, the unit's weather), off for original.
    auto fx = g("enhanced");
    fx.waterDetail = "standard", fx.atmosphere = "high", fx.lightShafts = false, fx.weather = "snow";
    EXPECT_EQ(resolve(fx), (GraphicsSettings{4, true, false, false, 1, 2, true, 1, 1.0f, false, 3, false, 3}));
    fx.atmosphere = "thick";
    EXPECT_THROW(resolve(fx), std::runtime_error);
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--atmosphere", "low"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--weather", "off"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--water-detail", "high"));
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--shafts", "off"));
    EXPECT_EQ(flags.lightShafts, std::optional<bool>(false));
    EXPECT_TRUE(ou::config::resolve(flags).effects); // the original effects default on
    EXPECT_TRUE(ou::config::applyGraphicsFlag(flags, "--fx", "off"));
    EXPECT_FALSE(ou::config::resolve(flags).effects);
    EXPECT_THROW(ou::config::applyGraphicsFlag(flags, "--fx", "no"), std::runtime_error);
}

TEST(Config, WaterQuality) {
    using ou::config::resolve;
    EXPECT_EQ(resolve({}).water, 1); // best-look default: enhanced water
    auto orig = ou::config::GraphicsConfig{};
    orig.preset = "original";
    EXPECT_EQ(resolve(orig).water, 0); // purist keeps TRU's own water
    auto c = ou::config::GraphicsConfig{};
    c.water = "original";
    EXPECT_EQ(resolve(c).water, 0);
    EXPECT_TRUE(ou::config::applyGraphicsFlag(c, "--water", "enhanced"));
    EXPECT_EQ(resolve(c).water, 1);
    c.water = "bogus";
    EXPECT_THROW(resolve(c), std::runtime_error);

    fs::path p = fs::temp_directory_path() / ("ou_config_water_" + std::to_string(::getpid()) + "/config.json");
    ou::config::Config cfg;
    cfg.graphics.water = "original";
    ou::config::save(p, cfg);
    EXPECT_EQ(ou::config::load(p).graphics.water, "original");
    fs::remove_all(p.parent_path());
}

} // namespace
