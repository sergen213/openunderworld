// Drives every ou-launcher page through its widgets, offscreen: import scan (version detection + validation),
// graphics settings, controls (capture dialog), save backup / restore, diagnostics (refresh, export, logs folder),
// Play / Viewer, and the update buttons (check, a streamed download that drops and resumes, install, rollback,
// disable) in AppImage and install-root mode against a local signed release. No network; the real game
// directory is scanned when OU_GAME_DIR is set. OU_LAUNCHER_SHOTS=<dir> saves a PNG of each page.
#define OU_LAUNCHER_NO_MAIN
#include "launcher/main.cpp"

#include <gtest/gtest.h>
#include <sodium.h>

#include <QElapsedTimer>
#include <QThread>

inline void PrintTo(const QString& t, std::ostream* os) { *os << '"' << t.toStdString() << '"'; } // readable gtest failures

namespace {

constexpr std::string_view kBase = "https://updates.test/";

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void writeText(const fs::path& p, const std::string& t, bool exec = false) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
    if (exec) fs::permissions(p, fs::perms(0755));
}

bool waitFor(const std::function<bool()>& done, int ms = 30000) {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

// Runs `act` on the next modal dialog; the click that opens it blocks in exec() meanwhile.
void onModal(std::function<void(QWidget*)> act) {
    auto* t = new QTimer;
    t->setInterval(10);
    QObject::connect(t, &QTimer::timeout, [t, act] {
        if (QWidget* m = QApplication::activeModalWidget()) {
            t->stop();
            t->deleteLater();
            act(m);
        }
    });
    t->start();
}

// Records QDesktopServices::openUrl("file://…") instead of starting the desktop's file manager.
class UrlSink : public QObject {
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void open(const QUrl& u) { opened << u; }
};

class LauncherGui : public ::testing::Test {
protected:
    fs::path tmp, srv;
    Paths paths;
    std::unique_ptr<Launcher> w;
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    std::optional<uint64_t> cutAfter; // the transport drops the connection once after this many bytes
    std::vector<uint64_t> offsets;    // payload request start bytes
    UrlSink urls;

    void SetUp() override {
        tmp = fs::temp_directory_path() / ("ou_launcher_gui_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(tmp);
        srv = tmp / "srv";
        for (const char* v : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"})
            ::setenv(v, (tmp / v).c_str(), 1);
        ::unsetenv("APPIMAGE");
        ::unsetenv("OU_DISABLE_UPDATES");
        paths = Paths{};
        paths.config = tmp / "config.json";
        ASSERT_GE(sodium_init(), 0);
        crypto_sign_keypair(pk, sk);
        ou::update::PublicKey key;
        std::copy(std::begin(pk), std::end(pk), key.begin());
        paths.key = key;
        paths.fetch = [this](const std::string& url, uint64_t offset, uint64_t max, const ou::update::Sink& sink) {
            if (!url.starts_with(kBase)) throw ou::update::Error("unexpected url " + url);
            const std::string body = readText(srv / url.substr(kBase.size()));
            if (body.size() > max) throw ou::update::Error("too large");
            if (!url.ends_with("manifest.json") && !url.ends_with(".sig")) offsets.push_back(offset);
            for (size_t at = offset; at < body.size(); at += 4096) {
                if (cutAfter && at - offset >= *cutAfter) {
                    cutAfter.reset();
                    throw ou::update::Error("connection reset");
                }
                sink({reinterpret_cast<const uint8_t*>(body.data()) + at, std::min<size_t>(4096, body.size() - at)});
            }
        };
        QDesktopServices::setUrlHandler("file", &urls, "open");
    }
    void TearDown() override {
        w.reset();
        QDesktopServices::unsetUrlHandler("file");
        ::unsetenv("APPIMAGE");
        fs::remove_all(tmp);
    }

    Launcher& open(int tab = 0) {
        w = std::make_unique<Launcher>(paths, QString());
        w->tabs()->setCurrentIndex(tab);
        w->show();
        QCoreApplication::processEvents();
        return *w;
    }
    void shot(const char* name) {
        QCoreApplication::processEvents();
        if (const char* dir = std::getenv("OU_LAUNCHER_SHOTS")) {
            EXPECT_TRUE(w->grab().save(QString("%1/%2.png").arg(dir, name)));
        }
    }
    QLineEdit* gameDir() {
        for (auto* e : w->findChildren<QLineEdit*>())
            if (e->placeholderText().startsWith("Tomb Raider: Underworld install directory")) return e;
        ADD_FAILURE() << "no game directory field";
        return nullptr;
    }
    template <class T>
    T* named(const char* name) {
        T* c = w->findChild<T*>(name);
        EXPECT_TRUE(c) << name;
        return c;
    }
    QPushButton* button(const QString& text, int tab = -1) { // tab: only that page (both Controllers and Diagnostics have "Refresh")
        for (auto* b : (tab < 0 ? static_cast<QWidget*>(w.get()) : w->tabs()->widget(tab))->findChildren<QPushButton*>())
            if (b->text() == text) return b;
        ADD_FAILURE() << "no button " << s(text);
        return nullptr;
    }
    template <class T>
    T* field(const QString& label) { // QFormLayout::addRow(text, field) makes the field the label's buddy
        for (auto* l : w->findChildren<QLabel*>())
            if (l->text() == label && l->buddy()) return qobject_cast<T*>(l->buddy());
        ADD_FAILURE() << "no field " << s(label);
        return nullptr;
    }
    QCheckBox* box(const QString& text) {
        for (auto* c : w->findChildren<QCheckBox*>())
            if (c->text() == text) return c;
        ADD_FAILURE() << "no checkbox " << s(text);
        return nullptr;
    }
    static void pick(QComboBox* c, const QString& data) {
        ASSERT_GE(c->findData(data), 0) << s(data);
        c->setCurrentIndex(c->findData(data));
    }
    QString status() { return named<QLabel>("status")->text(); }
    QString updateStatus() { return named<QLabel>("updateStatus")->text(); }
    void clickAndWait(const QString& text) { // an update button: disabled until its worker finishes
        QPushButton* b = button(text);
        b->click();
        EXPECT_TRUE(waitFor([b] { return b->isEnabled(); }));
    }

    // A launchable stand-in for ou-game / ou-viewer / an AppImage: appends "<itself> <args>" to launches.txt.
    std::string fakeTool() const { return "#!/bin/sh\necho \"$0 $*\" >> '" + (tmp / "launches.txt").string() + "'\n"; }
    std::string launches() {
        waitFor([this] { return fs::exists(tmp / "launches.txt"); }, 5000);
        QThread::msleep(50);
        return readText(tmp / "launches.txt");
    }

    // Publishes a signed release under srv/<version>/ and returns its manifest URL.
    std::string publish(const std::string& version, const std::map<std::string, std::string>& files) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [path, body] : files) {
            writeText(srv / version / path, body);
            ou::update::Bytes b(body.begin(), body.end());
            list.push_back({{"path", path}, {"size", body.size()}, {"sha256", ou::update::sha256Hex(b)},
                            {"url", std::string(kBase) + version + "/" + path}, {"executable", true}});
        }
        const std::string m = nlohmann::json{{"version", version}, {"channel", "stable"}, {"changelog", "release " + version},
                                             {"min_version", ""}, {"files", list}}.dump();
        unsigned char sig[crypto_sign_BYTES];
        crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(m.data()), m.size(), sk);
        writeText(srv / version / "manifest.json", m);
        writeText(srv / version / "manifest.json.sig", std::string(reinterpret_cast<char*>(sig), sizeof sig));
        return std::string(kBase) + version + "/manifest.json";
    }
};

TEST_F(LauncherGui, ImportScanDetectsAndValidates) {
    open(0);
    auto* dir = gameDir();
    auto* out = named<QPlainTextEdit>("scanOut");
    button("Play")->click();
    EXPECT_EQ(status(), "Pick the game directory on the Game tab first.");
    button("Scan / validate")->click();
    EXPECT_EQ(out->toPlainText(), "Pick the game directory first.");
    // Not a game install: problems, and the directory is not adopted.
    writeText(tmp / "notgame/readme.txt", "hi");
    dir->setText(q(tmp / "notgame"));
    button("Scan / validate")->click();
    ASSERT_TRUE(waitFor([&] { return button("Scan / validate")->isEnabled(); }));
    EXPECT_TRUE(out->toPlainText().startsWith("VALIDATION: PROBLEMS") || out->toPlainText().startsWith("Scan failed")) << s(out->toPlainText());
    EXPECT_EQ(status(), out->toPlainText().startsWith("Scan failed") ? status() : QString("Install has problems; see the Game tab."));
    const char* game = std::getenv("OU_GAME_DIR");
    if (!game) GTEST_SKIP() << "OU_GAME_DIR not set: real install not scanned";
    dir->setText(game);
    button("Scan / validate")->click();
    ASSERT_TRUE(waitFor([&] { return button("Scan / validate")->isEnabled(); }, 300000));
    const QString t = out->toPlainText();
    EXPECT_TRUE(t.startsWith("VALIDATION: OK")) << s(t);
    EXPECT_TRUE(t.contains("Detected build: ")) << s(t);
    EXPECT_FALSE(t.contains("Detected build: unknown"));
    EXPECT_TRUE(t.contains("Full manifest: "));
    EXPECT_EQ(status(), "Install validated; Save settings to keep this game directory.");
    shot("1-game");
    button("Save settings")->click();
    EXPECT_EQ(ou::config::load(paths.config).gameDir, game);
    // The diagnostic report carries the scan summary.
    w->tabs()->setCurrentIndex(4);
    button("Refresh", 4)->click();
    EXPECT_TRUE(named<QPlainTextEdit>("diagnostics")->toPlainText().contains("Detected build: "));
}

TEST_F(LauncherGui, GraphicsSettingsSaveAndAdoptGameChanges) {
    open(1);
    pick(field<QComboBox>("Renderer"), "vulkan");
    pick(field<QComboBox>("Preset"), "original");
    EXPECT_FALSE(field<QComboBox>("Ray tracing")->isEnabled()); // RT needs the enhanced preset
    pick(field<QComboBox>("Preset"), "enhanced");
    EXPECT_TRUE(field<QComboBox>("Ray tracing")->isEnabled());
    pick(field<QComboBox>("Ray tracing"), "full");
    pick(field<QComboBox>("Upscaler"), "off");
    EXPECT_FALSE(field<QComboBox>("FSR quality")->isEnabled());
    EXPECT_FALSE(box("Frame generation")->isEnabled());
    pick(field<QComboBox>("Upscaler"), "fsr");
    EXPECT_TRUE(box("Frame generation")->isEnabled());
    box("Frame generation")->setChecked(true);
    pick(field<QComboBox>("FSR quality"), "balanced");
    pick(field<QComboBox>("Water"), "enhanced");
    pick(field<QComboBox>("Weather"), "snow");
    field<QDoubleSpinBox>("Brightness")->setValue(1.25);
    field<QDoubleSpinBox>("Field of view")->setValue(75);
    box("Fullscreen")->setChecked(true);
    shot("2-graphics");
    button("Save settings")->click();
    EXPECT_TRUE(status().startsWith("Saved ")) << s(status());
    auto c = ou::config::load(paths.config);
    EXPECT_EQ(c.renderer, "vulkan");
    EXPECT_EQ(c.graphics.rt, "full");
    EXPECT_EQ(c.graphics.upscaler, "fsr");
    EXPECT_EQ(c.graphics.fsrQuality, "balanced");
    EXPECT_TRUE(c.graphics.frameGeneration);
    EXPECT_EQ(c.graphics.weather, "snow");
    EXPECT_FLOAT_EQ(c.graphics.brightness, 1.25f);
    EXPECT_FLOAT_EQ(c.fov, 75.f);
    EXPECT_TRUE(c.fullscreen);
    // Frame generation only exists with FSR: turning the upscaler off saves it off.
    pick(field<QComboBox>("Upscaler"), "off");
    button("Save settings")->click();
    EXPECT_FALSE(ou::config::load(paths.config).graphics.frameGeneration);
    // ou-game wrote its menu settings meanwhile: Save adopts them instead of undoing them.
    c = ou::config::load(paths.config);
    c.game.language = "fr";
    c.graphics.weather = "rain";
    ou::config::save(paths.config, c);
    button("Save settings")->click();
    c = ou::config::load(paths.config);
    EXPECT_EQ(c.game.language, "fr");
    EXPECT_EQ(c.graphics.weather, "rain");
    EXPECT_EQ(field<QComboBox>("Weather")->currentData().toString(), "rain");
}

TEST_F(LauncherGui, ControlsCaptureClearResetAndValidate) {
    open(2);
    auto* t = named<QTableWidget>("bindings");
    int jump = -1;
    for (int i = 0; i < t->rowCount(); ++i)
        if (t->verticalHeaderItem(i)->text() == "Jump") jump = i;
    ASSERT_GE(jump, 0);
    button("Add input…")->click();
    EXPECT_EQ(status(), "Select an action first.");
    t->setCurrentCell(jump, 0);
    t->item(jump, 0)->setText("Space");
    onModal([](QWidget* d) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier, "k");
        QApplication::sendEvent(d, &press);
    });
    button("Add input…")->click();
    EXPECT_EQ(t->item(jump, 0)->text(), "Space, K");
    onModal([](QWidget* d) { // Esc cancels
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(d, &press);
    });
    button("Add input…")->click();
    EXPECT_EQ(t->item(jump, 0)->text(), "Space, K");
    // A pad button binds too: a virtual Steam Deck-style pad (SDL's virtual joystick with every gamepad button and axis),
    // pressing a back paddle while the dialog waits.
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD, desc.naxes = SDL_GAMEPAD_AXIS_COUNT, desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1, desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "Virtual Steam Deck";
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    ASSERT_NE(vid, 0u) << SDL_GetError();
    SDL_Joystick* vj = SDL_OpenJoystick(vid);
    ASSERT_TRUE(vj) << SDL_GetError();
    onModal([vj](QWidget*) { SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, true); });
    button("Add input…")->click();
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, false);
    const std::string paddle = ou::platform::inputName({ou::platform::InputRef::Kind::PadButton, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1});
    EXPECT_EQ(t->item(jump, 0)->text(), "Space, K, " + q(paddle));
    auto* pads = w->tabs()->widget(2)->findChild<QListWidget*>();
    button("Refresh", 2)->click();
    EXPECT_FALSE(pads->findItems("Virtual Steam Deck", Qt::MatchStartsWith).isEmpty());
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (auto* sb : w->findChildren<QDoubleSpinBox*>())
        if (sb->maximum() == 10.0 && sb->minimum() == 0.1) sb->setValue(2.5);
    box("Invert look Y (mouse and right stick)")->setChecked(true);
    box("Invert look X")->setChecked(true);
    std::vector<QComboBox*> sticks; // Move with, Look with
    for (auto* cb : w->tabs()->widget(2)->findChildren<QComboBox*>())
        if (cb->findData("none") >= 0) sticks.push_back(cb);
    ASSERT_EQ(sticks.size(), 2u);
    sticks[0]->setCurrentIndex(sticks[0]->findData("right")), sticks[1]->setCurrentIndex(sticks[1]->findData("left"));
    shot("3-controllers");
    button("Save settings")->click();
    auto c = ou::config::load(paths.config);
    EXPECT_EQ(c.controls.bindings.at("jump"), (std::vector<std::string>{"Space", "K", paddle}));
    EXPECT_FLOAT_EQ(c.controls.mouseSensitivity, 2.5f);
    EXPECT_TRUE(c.controls.invertY && c.controls.invertX);
    EXPECT_EQ(std::pair(c.controls.moveStick, c.controls.lookStick), std::pair(std::string("right"), std::string("left")));
    // An unknown input name is refused, not saved.
    t->item(jump, 0)->setText("Bogus");
    button("Save settings")->click();
    EXPECT_TRUE(status().startsWith("Not saved: control jump: unknown input 'Bogus'")) << s(status());
    EXPECT_EQ(ou::config::load(paths.config).controls.bindings.at("jump"), (std::vector<std::string>{"Space", "K", paddle}));
    button("Clear")->click();
    EXPECT_EQ(t->item(jump, 0)->text(), "");
    button("Reset all to defaults")->click();
    button("Save settings")->click();
    EXPECT_EQ(ou::config::load(paths.config).controls.bindings, ou::config::InputConfig{}.bindings);
    pads->clear();
    button("Refresh", 2)->click();
    EXPECT_GT(pads->count(), 0); // a pad or "No gamepads detected…"
}

TEST_F(LauncherGui, SavesBackupAndRestore) {
    writeText(paths.saves / "slot0.sav", "A");
    open(3);
    auto* list = named<QListWidget>("backups");
    EXPECT_EQ(list->item(0)->text(), "(no backups yet)");
    button("Restore selected…")->click();
    EXPECT_EQ(status(), "Select a backup first.");
    button("Back up now")->click();
    button("Back up now")->click(); // same second: a second, separate backup
    ASSERT_EQ(list->count(), 2);
    EXPECT_NE(list->item(0)->text(), list->item(1)->text());
    const QString first = list->item(1)->text();
    writeText(paths.saves / "slot0.sav", "B");
    writeText(paths.saves / "slot1.sav", "new");
    list->setCurrentRow(1);
    onModal([](QWidget* d) { qobject_cast<QMessageBox*>(d)->button(QMessageBox::No)->click(); });
    button("Restore selected…")->click();
    EXPECT_EQ(readText(paths.saves / "slot0.sav"), "B");
    onModal([](QWidget* d) { qobject_cast<QMessageBox*>(d)->button(QMessageBox::Yes)->click(); });
    button("Restore selected…")->click();
    EXPECT_EQ(status(), "Restored saves from " + first);
    EXPECT_EQ(readText(paths.saves / "slot0.sav"), "A");
    EXPECT_FALSE(fs::exists(paths.saves / "slot1.sav"));
    ASSERT_EQ(list->count(), 3);
    fs::path before;
    for (const auto& e : fs::directory_iterator(paths.backups))
        if (e.path().filename().string().ends_with("-before-restore")) before = e.path();
    ASSERT_FALSE(before.empty());
    EXPECT_EQ(readText(before / "slot0.sav"), "B");
    EXPECT_EQ(readText(before / "slot1.sav"), "new");
    shot("4-saves");
    button("Open save folder")->click();
    ASSERT_EQ(urls.opened.size(), 1);
    EXPECT_EQ(urls.opened[0], QUrl::fromLocalFile(q(paths.saves)));
}

TEST_F(LauncherGui, DiagnosticsReportExportAndLogs) {
    writeText(paths.logs / "game-20260101-000000.log", "old log\n");
    writeText(paths.logs / "game-20261106-120000.log", "line one\nVkRenderer: device lost\n");
    fs::last_write_time(paths.logs / "game-20260101-000000.log", fs::file_time_type::clock::now() - std::chrono::hours(48));
    open(4);
    button("Save settings")->click();
    button("Refresh", 4)->click();
    const QString r = named<QPlainTextEdit>("diagnostics")->toPlainText();
    EXPECT_TRUE(r.startsWith("OpenUnderworld diagnostic report, "));
    EXPECT_TRUE(r.contains("Launcher " OU_VERSION ", Qt "));
    EXPECT_TRUE(r.contains("release key compiled in"));
    EXPECT_TRUE(r.contains("\"renderer\"")) << s(r); // the saved config
    EXPECT_TRUE(r.contains("game-20261106-120000.log ---\nline one\nVkRenderer: device lost")) << s(r);
    EXPECT_FALSE(r.contains("old log"));
    shot("5-diagnostics");
    const fs::path report = tmp / "report.txt";
    onModal([report](QWidget* d) {
        auto* f = qobject_cast<QFileDialog*>(d);
        ASSERT_TRUE(f);
        EXPECT_TRUE(f->selectedFiles().value(0).startsWith(QDir::homePath() + "/openunderworld-report-")) << s(f->selectedFiles().value(0));
        f->findChild<QLineEdit*>("fileNameEdit")->setText(q(report)); // as if typed
        QMetaObject::invokeMethod(f, "accept"); // a protected slot
    });
    button("Export report…")->click();
    EXPECT_EQ(status(), "Report written to " + q(report));
    EXPECT_TRUE(readText(report).starts_with("OpenUnderworld diagnostic report, "));
    button("Open logs folder")->click();
    ASSERT_EQ(urls.opened.size(), 1);
    EXPECT_EQ(urls.opened[0], QUrl::fromLocalFile(q(paths.logs)));
}

// AppImage mode: check, a download that drops and resumes, Play through the new AppImage, rollback, disable.
TEST_F(LauncherGui, AppImageUpdateCheckInstallResumeRollback) {
    const fs::path bin = tmp / "bin", old = bin / ("OpenUnderworld-" OU_VERSION "-x86_64.AppImage");
    writeText(old, fakeTool(), true);
    paths.appImage = old;
    // The new "AppImage" is a launchable script padded past a few transport chunks.
    const std::string next = fakeTool() + "#" + std::string(40000, 'x') + "\n";
    const std::string url = publish("999.0.0", {{"OpenUnderworld-999.0.0-x86_64.AppImage", next}});
    const fs::path nextPath = bin / "OpenUnderworld-999.0.0-x86_64.AppImage";
    open(5);
    EXPECT_TRUE(named<QPlainTextEdit>("changelog")->placeholderText().startsWith("Check for updates"));
    field<QLineEdit>("Manifest URL")->setText(q(url));
    clickAndWait("Check for updates");
    EXPECT_EQ(updateStatus(), "Signed update 999.0.0 available.");
    EXPECT_EQ(named<QPlainTextEdit>("changelog")->toPlainText(), "release 999.0.0");
    EXPECT_FALSE(fs::exists(nextPath));
    cutAfter = 20000;
    clickAndWait("Download && install");
    EXPECT_EQ(updateStatus(), "Update failed: connection reset");
    EXPECT_EQ(fs::file_size(nextPath.string() + ".new"), 20480u); // kept for resuming
    EXPECT_EQ(readText(old), fakeTool());
    clickAndWait("Download && install");
    EXPECT_EQ(offsets, (std::vector<uint64_t>{0, 20480}));
    EXPECT_EQ(updateStatus(), "Installed 999.0.0 as " + q(nextPath) + " (old one kept as .bak); restart to use it.");
    EXPECT_EQ(readText(nextPath), next);
    EXPECT_FALSE(fs::exists(old));
    EXPECT_TRUE(fs::exists(old.string() + ".bak"));
    EXPECT_TRUE(button("Restart")->isVisible());
    EXPECT_EQ(qEnvironmentVariable("APPIMAGE"), q(nextPath));
    shot("6-updates");
    // Play goes through the new AppImage (its game subcommand), with the saved config.
    gameDir()->setText(q(tmp / "game"));
    button("Play")->click();
    EXPECT_EQ(launches(), nextPath.string() + " game --config " + paths.config.string() + "\n");
    EXPECT_TRUE(status().startsWith("Started ou-game (pid ")) << s(status());
    clickAndWait("Roll back to previous");
    EXPECT_EQ(updateStatus(), "Rolled back to " + q(old) + "; restart to use it.");
    EXPECT_EQ(readText(old), fakeTool());
    EXPECT_TRUE(fs::exists(nextPath.string() + ".bak"));
    // Disabled: no network, says so.
    box("Enable updates (OU_DISABLE_UPDATES=1 also disables)")->setChecked(false);
    offsets.clear();
    clickAndWait("Check for updates");
    EXPECT_EQ(updateStatus(), "Updates are disabled.");
    button("Save settings")->click();
    EXPECT_FALSE(ou::config::load(paths.config).updatesEnabled);
}

// Install-root mode (not an AppImage): the updater-managed copy, Viewer runs it, rollback to the previous one.
TEST_F(LauncherGui, InstallRootUpdateViewerAndRollback) {
    const std::string v1 = publish("999.0.0", {{"bin/ou-viewer", fakeTool()}, {"bin/ou-game", fakeTool()}});
    const std::string v2 = publish("999.1.0", {{"bin/ou-viewer", fakeTool() + "# 999.1.0\n"}, {"bin/ou-game", fakeTool()}});
    open(5);
    EXPECT_TRUE(named<QPlainTextEdit>("changelog")->placeholderText().startsWith("No changelog"));
    clickAndWait("Roll back to previous");
    EXPECT_TRUE(updateStatus().startsWith("Rollback failed: ")) << s(updateStatus());
    field<QLineEdit>("Manifest URL")->setText(q(v1));
    clickAndWait("Download && install");
    EXPECT_EQ(updateStatus(), "Installed 999.0.0; previous version kept for rollback.");
    EXPECT_EQ(ou::update::installedVersion(paths.installRoot), "999.0.0");
    EXPECT_FALSE(button("Restart")->isVisible());
    button("Viewer")->click();
    EXPECT_EQ(launches(), (paths.installRoot / "current/bin/ou-viewer").string() + " --config " + paths.config.string() + "\n");
    field<QLineEdit>("Manifest URL")->setText(q(v2));
    clickAndWait("Download && install");
    EXPECT_EQ(ou::update::installedVersion(paths.installRoot), "999.1.0");
    EXPECT_EQ(named<QPlainTextEdit>("changelog")->toPlainText(), "release 999.1.0");
    clickAndWait("Check for updates");
    EXPECT_EQ(updateStatus(), "Up to date (999.1.0).");
    clickAndWait("Roll back to previous");
    EXPECT_EQ(updateStatus(), "Rolled back; active version 999.0.0");
    EXPECT_EQ(named<QPlainTextEdit>("changelog")->toPlainText(), "release 999.0.0");
    bool shown = false;
    for (auto* l : w->findChildren<QLabel*>()) shown |= l->text().contains("Updater-managed: install 999.0.0");
    EXPECT_TRUE(shown);
}

} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    SDL_Init(SDL_INIT_GAMEPAD);
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    SDL_Quit();
    return rc;
}

#include "test_launcher_gui.moc"
