// ou-launcher: Qt 6 Widgets front end. Writes $XDG_CONFIG_HOME/openunderworld/config.json and starts
// ou-game --config <that file> (Play: main menu, New Game = chapter 0) or ou-viewer --config <that file> (Viewer).
// Reads the game directory only (ou_importer); never writes to it.
// No Q_OBJECT classes (lambdas only), so no moc. Notes: docs/LAUNCHER.md.
#include "config/Config.hpp"
#include "importer/Importer.hpp"
#include "platform/Input.hpp"
#include "update/Update.hpp"

#include <SDL3/SDL.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QSysInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include <functional>

namespace fs = std::filesystem;
using nlohmann::json;

#ifndef OU_VERSION
#define OU_VERSION "0.0.0"
#endif

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }
QString q(const fs::path& p) { return QString::fromStdString(p.string()); }
QString q(const char* t) { return QString::fromUtf8(t); }
std::string s(const QString& t) { return t.toStdString(); }

// "Preset default" / "On" / "Off" for an optional<bool> override.
QComboBox* overrideCombo(std::optional<bool> v) {
    auto* c = new QComboBox;
    c->addItem("Preset default", QVariant());
    c->addItem("On", true);
    c->addItem("Off", false);
    c->setCurrentIndex(!v ? 0 : *v ? 1 : 2);
    return c;
}
std::optional<bool> overrideValue(const QComboBox* c) {
    QVariant d = c->currentData();
    return d.isValid() ? std::optional(d.toBool()) : std::nullopt;
}

struct Paths {
    fs::path config = ou::config::defaultConfigPath();
    fs::path saves = ou::config::dataHome() / "openunderworld" / "saves";
    fs::path backups = ou::config::dataHome() / "openunderworld" / "save-backups";
    fs::path logs = ou::config::stateHome() / "openunderworld" / "logs";
    fs::path installRoot = ou::config::dataHome() / "openunderworld" / "install"; // updater-managed copy
    fs::path appImage = [] { const char* a = std::getenv("APPIMAGE"); return fs::path(a ? a : ""); }(); // set by the AppImage runtime
};

// Update options from config; AppImage mode replaces $APPIMAGE itself instead of the install root.
ou::update::Options updateOptions(const Paths& paths, const ou::config::Config& c) {
    auto key = ou::update::releasePublicKey();
    if (!key) throw std::runtime_error("this build has no release key compiled in; updates are unavailable");
    ou::update::Options o;
    o.root = paths.installRoot;
    o.appImage = paths.appImage;
    o.runningVersion = OU_VERSION;
    o.manifestUrl = c.updateManifestUrl;
    o.channel = c.updateChannel;
    o.key = *key;
    o.fetch = ou::update::httpsFetch();
    o.enabled = c.updatesEnabled;
    if (!c.gameDir.empty()) o.forbiddenRoots = {c.gameDir};
    return o;
}

QString describe(const ou::update::Result& r, const Paths& paths) {
    using S = ou::update::Status;
    if (r.status == S::Disabled) return "Updates are disabled.";
    if (r.status == S::UpToDate) return "Up to date (" + q(r.manifest.version) + ").";
    if (r.status == S::Available) return "Signed update " + q(r.manifest.version) + " available.";
    if (!paths.appImage.empty())
        return "Installed " + q(r.manifest.version) + " into " + q(paths.appImage) + " (old one kept as .bak); restart to use it.";
    return "Installed " + q(r.manifest.version) + "; previous version kept for rollback.";
}

// Human summary + verdict of an ou_importer scan.
QString summarize(const json& m, bool& ok) {
    QStringList out, problems, warnings;
    const json& ex = m.value("exe", json::object());
    out << "Game directory: " + q(m.value("game_dir", ""));
    if (ex.contains("error")) problems << "tru.exe: " + q(ex["error"].get<std::string>());
    else {
        out << "Detected build: " + q(ex.value("build", "unknown"));
        out << "tru.exe sha256: " + q(ex.value("sha256", ""));
        if (ex.contains("version_resource") && ex["version_resource"].is_object())
            out << "File version: " + q(ex["version_resource"].value("fixed_file_version", "?"));
        if (ex.contains("pe")) out << "PE timestamp: " + q(ex["pe"].value("timestamp_utc", "?"));
        if (ex.value("build", "unknown") == "unknown") problems << "tru.exe is not a known build (unsupported edition?)";
    }
    out << QString("Files: %1").arg(m.value("files", json::array()).size());
    for (const auto& a : m.value("archives", json::array())) {
        QString name = q(a.value("name", "?"));
        if (a.contains("error")) { problems << name + ": " + q(a["error"].get<std::string>()); continue; }
        out << QString("%1: %2 records in %3 parts, %4 record errors, names %5%")
                   .arg(name).arg(a.value("record_count", 0)).arg(a.value("parts", json::array()).size())
                   .arg(a.value("record_errors", 0)).arg(a.value("name_coverage_pct", 0.0), 0, 'f', 1);
        if (!a.value("missing_parts", json::array()).empty()) problems << name + ": missing parts " + q(a["missing_parts"].dump());
        if (a.value("record_errors", 0) > 0) problems << name + ": record errors";
    }
    for (const auto& h : m.value("gog_hashdb", json::array())) {
        if (h.contains("error")) { problems << q(h.value("file", "")) + ": " + q(h["error"].get<std::string>()); continue; }
        const auto& sm = h["summary"];
        out << QString("GOG hashdb %1: %2 ok, %3 mismatch, %4 missing")
                   .arg(q(h.value("file", ""))).arg(sm.value("ok", 0)).arg(sm.value("mismatch", 0)).arg(sm.value("missing", 0));
        if (sm.value("mismatch", 0)) problems << "GOG hashdb: files differ from the GOG release";
        for (const auto& e : h.value("entries", json::array()))
            if (e.value("status", "") == "missing") warnings << "GOG hashdb lists a file that is absent: " + q(e.value("path", ""));
        if (!sm.value("ok", 0) && !sm.value("mismatch", 0)) out << "  (file hashes not checked; enable \"Hash every file\")";
    }
    if (m.contains("runtime_seconds")) out << QString("Scan time: %1 s").arg(m["runtime_seconds"].value("total", 0.0), 0, 'f', 1);
    ok = problems.empty();
    QString head = ok ? "VALIDATION: OK" : "VALIDATION: PROBLEMS\n  - " + problems.join("\n  - ");
    if (!warnings.empty()) head += "\nWarnings:\n  - " + warnings.join("\n  - ");
    out.prepend(head);
    return out.join('\n');
}

void copyTree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

// Modal "press an input" capture: a key, mouse button or gamepad button/trigger. Esc cancels. result = binding name.
class CaptureDialog : public QDialog {
public:
    std::string result;
    explicit CaptureDialog(QWidget* parent, const QString& action) : QDialog(parent) {
        setWindowTitle("Bind " + action);
        auto* lay = new QVBoxLayout(this);
        lay->addWidget(new QLabel("Press a key, mouse button or gamepad button for <b>" + action + "</b>.<br>Esc cancels."));
        lay->addWidget(status_);
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            for (int i = 0; i < n; ++i)
                if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) pads_.push_back(g);
            SDL_free(ids);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {} // drop stale presses
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                using K = ou::platform::InputRef::Kind;
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) return take({K::PadButton, e.gbutton.button});
                if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 16384 &&
                    (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                    return take({K::PadTrigger, e.gaxis.axis});
            }
        });
        poll->start(16);
    }
    ~CaptureDialog() override {
        for (SDL_Gamepad* g : pads_) SDL_CloseGamepad(g);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape) return reject();
        // Left/right modifiers by XKB keycode (evdev + 8); Qt reports both sides as one key.
        static const std::pair<quint32, const char*> kSided[] = {{50, "Left Shift"}, {62, "Right Shift"}, {37, "Left Ctrl"},
                                                                 {105, "Right Ctrl"}, {64, "Left Alt"}, {108, "Right Alt"}};
        for (auto [code, name] : kSided)
            if (e->nativeScanCode() == code) return take(*ou::platform::parseInput(name));
        static const std::pair<const char*, const char*> kQtToSdl[] = {{"Del", "Delete"}, {"Ins", "Insert"}, {"PgUp", "PageUp"},
                                                                       {"PgDown", "PageDown"}, {"Enter", "Keypad Enter"}};
        QString name = QKeySequence(e->key()).toString();
        for (auto [qt, sdl] : kQtToSdl)
            if (name == qt) name = sdl;
        if (const auto r = ou::platform::parseInput(s(name))) return take(*r);
        status_->setText("'" + name + "' has no SDL key name; try another key.");
    }
    void mousePressEvent(QMouseEvent* e) override {
        using K = ou::platform::InputRef::Kind;
        if (e->button() == Qt::LeftButton) take({K::Mouse, SDL_BUTTON_LEFT});
        else if (e->button() == Qt::RightButton) take({K::Mouse, SDL_BUTTON_RIGHT});
        else if (e->button() == Qt::MiddleButton) take({K::Mouse, SDL_BUTTON_MIDDLE});
    }

private:
    std::vector<SDL_Gamepad*> pads_;
    QLabel* status_ = new QLabel(this);
    void take(const ou::platform::InputRef& r) {
        result = ou::platform::inputName(r);
        accept();
    }
};

class Launcher : public QWidget {
public:
    Launcher(Paths paths, const QString& gameDirOverride) : paths_(std::move(paths)) {
        setWindowTitle("OpenUnderworld Launcher");
        setWindowIcon(QIcon::fromTheme("openunderworld"));
        try { cfg_ = ou::config::load(paths_.config); } catch (const std::exception& e) { startupNote_ = "config ignored: " + q(e.what()); }
        if (!gameDirOverride.isEmpty()) cfg_.gameDir = s(gameDirOverride);

        tabs_ = new QTabWidget;
        tabs_->addTab(gamePage(), "Game");
        tabs_->addTab(graphicsPage(), "Graphics");
        tabs_->addTab(controllersPage(), "Controllers");
        tabs_->addTab(savesPage(), "Saves");
        tabs_->addTab(diagnosticsPage(), "Diagnostics");
        tabs_->addTab(updatesPage(), "Version && Updates");

        auto* save = new QPushButton("Save settings");
        auto* viewer = new QPushButton("Viewer");
        viewer->setToolTip("Start ou-viewer (debug fly camera over the start level / DRM)");
        auto* play = new QPushButton("Play");
        play->setToolTip("Start ou-game at the main menu (New Game starts chapter 0)");
        play->setDefault(true);
        status_ = new QLabel;
        status_->setWordWrap(true);
        connect(save, &QPushButton::clicked, this, [this] { saveConfig(); });
        connect(viewer, &QPushButton::clicked, this, [this] { launch("ou-viewer"); });
        connect(play, &QPushButton::clicked, this, [this] {
            collect();
            if (cfg_.gameDir.empty()) { status_->setText("Pick the game directory on the Game tab first."); return; }
            launch("ou-game");
        });
        auto* bar = new QHBoxLayout;
        bar->addWidget(status_, 1);
        bar->addWidget(save);
        bar->addWidget(viewer);
        bar->addWidget(play);
        auto* lay = new QVBoxLayout(this);
        lay->addWidget(tabs_);
        lay->addLayout(bar);
        resize(900, 640);

        try {
            if (auto r = ou::update::recover(paths_.installRoot); !r.empty()) startupNote_ += " update recovery: " + q(r);
        } catch (const std::exception& e) { startupNote_ += " update recovery failed: " + q(e.what()); }
        status_->setText(startupNote_.isEmpty() ? "Config: " + q(paths_.config) : startupNote_);
    }

    QTabWidget* tabs() { return tabs_; }
    void startScan() { scan(); }
    std::function<void()> onScanDone; // used by --grab

private:
    Paths paths_;
    ou::config::Config cfg_;
    QString startupNote_, lastScanSummary_;
    QTabWidget* tabs_;
    QLabel* status_;
    QLineEdit *gameDir_, *drm_, *manifestUrl_;
    QCheckBox *hashFiles_, *fullscreen_, *vsync_, *updatesEnabled_;
    QPushButton* scanBtn_;
    QPlainTextEdit *scanOut_, *diag_, *changelog_;
    QComboBox *renderer_, *channel_, *gfxPreset_, *rt_, *rtQuality_, *msaa_, *shadows_, *shaders_;
    QComboBox *water_, *waterDetail_, *atmosphere_, *shafts_, *weather_, *upscaler_, *fsrQuality_;
    QCheckBox* frameGeneration_;
    QDoubleSpinBox* sharpness_;
    QSpinBox *width_, *height_;
    QDoubleSpinBox *fov_, *brightness_;
    QListWidget *pads_, *backups_;
    QTableWidget* bindings_;
    QDoubleSpinBox* mouseSensitivity_;
    QCheckBox* invertY_;
    QLabel *updateStatus_, *versionLabel_;

    // ---------------------------------------------------------------------------------------
    QWidget* gamePage() {
        auto* w = new QWidget;
        gameDir_ = new QLineEdit(q(cfg_.gameDir));
        gameDir_->setPlaceholderText("Tomb Raider: Underworld install directory (contains tru.exe, bigfile.000)");
        auto* browse = new QPushButton("Browse…");
        connect(browse, &QPushButton::clicked, this, [this] {
            QString d = QFileDialog::getExistingDirectory(this, "Game directory", gameDir_->text());
            if (!d.isEmpty()) gameDir_->setText(d);
        });
        hashFiles_ = new QCheckBox("Hash every file and check the GOG hashdb (slow: reads the whole install)");
        scanBtn_ = new QPushButton("Scan / validate");
        connect(scanBtn_, &QPushButton::clicked, this, [this] { scan(); });
        scanOut_ = new QPlainTextEdit;
        scanOut_->setReadOnly(true);
        scanOut_->setPlaceholderText("Scan the install to detect the build and validate the archives. Read-only: nothing in the game directory is modified.");
        auto* row = new QHBoxLayout;
        row->addWidget(gameDir_, 1);
        row->addWidget(browse);
        auto* lay = new QVBoxLayout(w);
        lay->addWidget(new QLabel("<b>Install / import</b> — OpenUnderworld reads your own legally obtained copy of the game."));
        lay->addLayout(row);
        lay->addWidget(hashFiles_);
        lay->addWidget(scanBtn_);
        lay->addWidget(scanOut_, 1);
        return w;
    }

    void scan() {
        fs::path dir = s(gameDir_->text().trimmed());
        if (dir.empty()) { scanOut_->setPlainText("Pick the game directory first."); return; }
        ou::importer::ScanOptions opt;
        opt.hashFiles = hashFiles_->isChecked();
        scanBtn_->setEnabled(false);
        scanOut_->setPlainText("Scanning " + q(dir) + " …");
        auto* watcher = new QFutureWatcher<std::pair<json, std::string>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, dir] {
            auto [m, err] = watcher->result();
            watcher->deleteLater();
            scanBtn_->setEnabled(true);
            if (!err.empty()) { scanOut_->setPlainText("Scan failed: " + q(err)); return; }
            bool ok = false;
            QString text = summarize(m, ok);
            if (auto sha = m["exe"].value("sha256", ""); !sha.empty()) {
                fs::path out = ou::importer::defaultManifestPath(sha);
                std::error_code ec;
                fs::create_directories(out.parent_path(), ec);
                std::ofstream(out) << m.dump(1);
                text += "\nFull manifest: " + q(out);
            }
            if (ok) cfg_.gameDir = dir.string();
            lastScanSummary_ = text;
            if (onScanDone) QTimer::singleShot(100, this, onScanDone);
            scanOut_->setPlainText(text);
            status_->setText(ok ? "Install validated; Save settings to keep this game directory." : "Install has problems; see the Game tab.");
        });
        watcher->setFuture(QtConcurrent::run([dir, opt]() -> std::pair<json, std::string> {
            try { return {ou::importer::scan(dir, opt), {}}; } catch (const std::exception& e) { return {json(), e.what()}; }
        }));
    }

    // ---------------------------------------------------------------------------------------
    QWidget* graphicsPage() {
        auto* w = new QWidget;
        renderer_ = new QComboBox;
        renderer_->addItem("Auto (Vulkan, else OpenGL)", "auto");
        renderer_->addItem("Vulkan", "vulkan");
        renderer_->addItem("OpenGL 4.6", "opengl");
        renderer_->setCurrentIndex(std::max(0, renderer_->findData(q(cfg_.renderer))));
        const auto& g = cfg_.graphics;
        gfxPreset_ = new QComboBox;
        gfxPreset_->addItem("Enhanced", "enhanced");
        gfxPreset_->addItem("Original (purist)", "original");
        gfxPreset_->setCurrentIndex(std::max(0, gfxPreset_->findData(q(g.preset))));
        rt_ = new QComboBox;
        rt_->addItem("Auto: shadows on capable GPUs", "auto");
        rt_->addItem("Off", "off");
        rt_->addItem("Shadows", "shadows");
        rt_->addItem("Shadows + water reflections", "full");
        rt_->setCurrentIndex(std::max(0, rt_->findData(q(g.rt))));
        rtQuality_ = new QComboBox;
        rtQuality_->addItem("Low (1 ray/pixel)", "low");
        rtQuality_->addItem("Medium (2 rays/pixel)", "medium");
        rtQuality_->addItem("High (4 rays/pixel)", "high");
        rtQuality_->setCurrentIndex(std::max(0, rtQuality_->findData(q(g.rtQuality))));
        auto rtEnable = [this] { // RT needs the enhanced preset
            bool on = gfxPreset_->currentData().toString() == "enhanced";
            rt_->setEnabled(on);
            rtQuality_->setEnabled(on);
        };
        connect(gfxPreset_, &QComboBox::currentIndexChanged, this, rtEnable);
        rtEnable();
        msaa_ = new QComboBox;
        msaa_->addItem("Preset default", 0);
        for (int n : {1, 2, 4, 8}) msaa_->addItem(QString("%1x").arg(n), n);
        msaa_->setCurrentIndex(std::max(0, msaa_->findData(g.msaa)));
        shadows_ = overrideCombo(g.shadows);
        shaders_ = new QComboBox;
        shaders_->addItem("Original TRU shaders");
        shaders_->addItem("Generic");
        shaders_->setCurrentIndex(g.originalShaders == std::optional(false) ? 1 : 0);
        // Enhanced water / atmosphere / weather (docs/RENDERING.md); "auto" follows the preset.
        auto choice = [](std::initializer_list<std::pair<const char*, const char*>> items, const std::string& v) {
            auto* c = new QComboBox;
            for (auto [label, data] : items) c->addItem(label, data);
            c->setCurrentIndex(std::max(0, c->findData(q(v))));
            return c;
        };
        water_ = choice({{"Preset default", "auto"}, {"Enhanced", "enhanced"}, {"Original", "original"}}, g.water);
        waterDetail_ = choice({{"Preset default (high)", "auto"}, {"High", "high"}, {"Standard", "standard"}}, g.waterDetail);
        atmosphere_ = choice({{"Preset default (medium)", "auto"}, {"Off", "off"}, {"Low: height fog", "low"},
                              {"Medium: + sun shafts", "medium"}, {"High: finer shafts", "high"}}, g.atmosphere);
        shafts_ = overrideCombo(g.lightShafts);
        weather_ = choice({{"Preset default (level's rain / snow)", "auto"}, {"Off", "off"}, {"Always rain", "rain"},
                           {"Always snow", "snow"}}, g.weather);
        brightness_ = new QDoubleSpinBox;
        brightness_->setRange(0.5, 2.0);
        brightness_->setSingleStep(0.05);
        brightness_->setDecimals(2);
        brightness_->setValue(g.brightness);
        // AMD FSR (Vulkan only, docs/RENDERING.md "FSR").
        upscaler_ = choice({{"Off (native)", "off"}, {"AMD FSR 3.1 (Vulkan)", "fsr"}}, g.upscaler);
        fsrQuality_ = choice({{"Native AA", "native"}, {"Quality (1.5x)", "quality"}, {"Balanced (1.7x)", "balanced"},
                              {"Performance (2x)", "performance"}, {"Ultra performance (3x)", "ultra_performance"}}, g.fsrQuality);
        sharpness_ = new QDoubleSpinBox;
        sharpness_->setRange(0.0, 1.0);
        sharpness_->setSingleStep(0.05);
        sharpness_->setValue(g.sharpness);
        frameGeneration_ = new QCheckBox("Frame generation");
        frameGeneration_->setChecked(g.frameGeneration);
        auto fsrEnable = [this] {
            const bool on = upscaler_->currentData().toString() == "fsr";
            fsrQuality_->setEnabled(on), sharpness_->setEnabled(on), frameGeneration_->setEnabled(on);
        };
        connect(upscaler_, &QComboBox::currentIndexChanged, this, fsrEnable);
        fsrEnable();
        width_ = new QSpinBox;
        height_ = new QSpinBox;
        width_->setRange(320, 16384);
        height_->setRange(200, 16384);
        width_->setValue(cfg_.width);
        height_->setValue(cfg_.height);
        auto* preset = new QComboBox;
        preset->addItem("Presets…");
        for (auto [pw, ph] : {std::pair{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}})
            preset->addItem(QString("%1 × %2").arg(pw).arg(ph), QSize(pw, ph));
        connect(preset, &QComboBox::activated, this, [this, preset](int i) {
            if (QSize sz = preset->itemData(i).toSize(); sz.isValid()) { width_->setValue(sz.width()); height_->setValue(sz.height()); }
        });
        fullscreen_ = new QCheckBox("Fullscreen");
        fullscreen_->setChecked(cfg_.fullscreen);
        fov_ = new QDoubleSpinBox;
        fov_->setRange(30, 120);
        fov_->setSuffix("°");
        fov_->setValue(cfg_.fov);
        vsync_ = new QCheckBox("VSync");
        vsync_->setChecked(cfg_.vsync);
        drm_ = new QLineEdit(q(cfg_.drm));
        drm_->setPlaceholderText("e.g. pc-w\\lara.drm or pc-w\\l4_pool.drm (empty: placeholder scene)");
        auto* res = new QHBoxLayout;
        res->addWidget(width_);
        res->addWidget(new QLabel("×"));
        res->addWidget(height_);
        res->addWidget(preset);
        res->addStretch();
        auto* f = new QFormLayout(w);
        f->addRow("Renderer", renderer_);
        f->addRow("Resolution", res);
        f->addRow("", fullscreen_);
        f->addRow("Field of view", fov_);
        f->addRow("", vsync_);
        f->addRow("Start level (DRM)", drm_);
        f->addRow(new QLabel("<b>Graphics</b>"));
        f->addRow("Preset", gfxPreset_);
        f->addRow("Ray tracing", rt_);
        f->addRow("RT quality", rtQuality_);
        f->addRow("MSAA", msaa_);
        f->addRow("Sun shadows", shadows_);
        f->addRow("Shaders", shaders_);
        f->addRow("Water", water_);
        f->addRow("Water detail", waterDetail_);
        f->addRow("Atmosphere", atmosphere_);
        f->addRow("Sun shafts", shafts_);
        f->addRow("Weather", weather_);
        f->addRow("Brightness", brightness_);
        f->addRow("Upscaler", upscaler_);
        f->addRow("FSR quality", fsrQuality_);
        f->addRow("FSR sharpness", sharpness_);
        f->addRow("", frameGeneration_);
        f->addRow("", new QLabel("Like the original game's brightness option; 1.00 = neutral."));
        return w;
    }

    // ---------------------------------------------------------------------------------------
    QWidget* controllersPage() {
        auto* w = new QWidget;
        // Bindings: one row per action, inputs comma-separated (editable; names in docs/GAMEPLAY.md "Controls").
        const auto& actions = ou::config::bindableActions();
        bindings_ = new QTableWidget(int(actions.size()), 1);
        bindings_->setHorizontalHeaderLabels({"Inputs (comma-separated)"});
        bindings_->horizontalHeader()->setStretchLastSection(true);
        bindings_->setSelectionBehavior(QAbstractItemView::SelectRows);
        bindings_->setSelectionMode(QAbstractItemView::SingleSelection);
        for (int i = 0; i < int(actions.size()); ++i) bindings_->setVerticalHeaderItem(i, new QTableWidgetItem(q(actions[i])));
        showBindings(cfg_.controls);
        auto* add = new QPushButton("Add input…");
        auto* clear = new QPushButton("Clear");
        auto* reset = new QPushButton("Reset all to defaults");
        connect(add, &QPushButton::clicked, this, [this] {
            const int row = bindings_->currentRow();
            if (row < 0) { status_->setText("Select an action first."); return; }
            CaptureDialog d(this, bindings_->verticalHeaderItem(row)->text());
            if (d.exec() != QDialog::Accepted) return;
            auto* item = bindings_->item(row, 0);
            const QString cur = item->text().trimmed();
            if (!splitInputs(cur).contains(q(d.result))) item->setText(cur.isEmpty() ? q(d.result) : cur + ", " + q(d.result));
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            if (const int row = bindings_->currentRow(); row >= 0) bindings_->item(row, 0)->setText({});
        });
        connect(reset, &QPushButton::clicked, this, [this] { showBindings({}); });
        mouseSensitivity_ = new QDoubleSpinBox;
        mouseSensitivity_->setRange(0.1, 10.0);
        mouseSensitivity_->setSingleStep(0.1);
        mouseSensitivity_->setValue(cfg_.controls.mouseSensitivity);
        invertY_ = new QCheckBox("Invert look Y (mouse and right stick)");
        invertY_->setChecked(cfg_.controls.invertY);
        pads_ = new QListWidget;
        auto* refresh = new QPushButton("Refresh");
        connect(refresh, &QPushButton::clicked, this, [this] { listPads(); });
        auto* buttons = new QHBoxLayout;
        for (auto* b : {add, clear, reset}) buttons->addWidget(b);
        buttons->addStretch();
        auto* mouse = new QHBoxLayout;
        mouse->addWidget(new QLabel("Mouse sensitivity"));
        mouse->addWidget(mouseSensitivity_);
        mouse->addWidget(invertY_);
        mouse->addStretch();
        auto* lay = new QVBoxLayout(w);
        lay->addWidget(new QLabel("Controls for ou-game (left stick moves and right stick looks on every gamepad):"));
        lay->addWidget(bindings_, 3);
        lay->addLayout(buttons);
        lay->addLayout(mouse);
        lay->addWidget(new QLabel("Gamepads seen by SDL3 (the game uses the first one):"));
        lay->addWidget(pads_, 1);
        lay->addWidget(refresh, 0, Qt::AlignLeft);
        listPads();
        return w;
    }

    static QStringList splitInputs(const QString& text) {
        QStringList r;
        for (const QString& p : text.split(',', Qt::SkipEmptyParts))
            if (!p.trimmed().isEmpty()) r << p.trimmed();
        return r;
    }
    void showBindings(const ou::config::InputConfig& c) {
        const auto& actions = ou::config::bindableActions();
        for (int i = 0; i < int(actions.size()); ++i) {
            QStringList names;
            for (const auto& n : c.bindings.at(actions[i])) names << q(n);
            bindings_->setItem(i, 0, new QTableWidgetItem(names.join(", ")));
        }
    }

    void listPads() {
        pads_->clear();
        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) { pads_->addItem(QString("SDL gamepad subsystem unavailable: ") + SDL_GetError()); return; }
        SDL_UpdateJoysticks();
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        for (int i = 0; i < n; ++i) {
            char guid[64];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, sizeof guid);
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            pads_->addItem(QString("%1  [%2]  guid %3").arg(name ? name : "unnamed")
                               .arg(SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(ids[i]))).arg(guid));
        }
        SDL_free(ids);
        if (n == 0) pads_->addItem("No gamepads detected. Connect one and press Refresh.");
    }

    // ---------------------------------------------------------------------------------------
    QWidget* savesPage() {
        auto* w = new QWidget;
        backups_ = new QListWidget;
        auto* backup = new QPushButton("Back up now");
        auto* restore = new QPushButton("Restore selected…");
        auto* open = new QPushButton("Open save folder");
        connect(backup, &QPushButton::clicked, this, [this] { backupSaves(); });
        connect(restore, &QPushButton::clicked, this, [this] { restoreSaves(); });
        connect(open, &QPushButton::clicked, this, [this] {
            fs::create_directories(paths_.saves);
            QDesktopServices::openUrl(QUrl::fromLocalFile(q(paths_.saves)));
        });
        auto* row = new QHBoxLayout;
        row->addWidget(backup);
        row->addWidget(restore);
        row->addWidget(open);
        row->addStretch();
        auto* lay = new QVBoxLayout(w);
        lay->addWidget(new QLabel("Save directory: " + q(paths_.saves) + "\nBackups: " + q(paths_.backups) +
                                  "\n(OpenUnderworld's own saves; the original game's saves are never touched.)"));
        lay->addWidget(backups_, 1);
        lay->addLayout(row);
        listBackups();
        return w;
    }

    void listBackups() {
        backups_->clear();
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(paths_.backups, ec))
            if (e.is_directory()) names.push_back(e.path().filename().string());
        std::sort(names.rbegin(), names.rend());
        for (const auto& n : names) backups_->addItem(q(n));
        if (names.empty()) backups_->addItem("(no backups yet)");
    }

    void backupSaves(const QString& suffix = {}) {
        std::error_code ec;
        if (!fs::exists(paths_.saves) || fs::is_empty(paths_.saves, ec)) { status_->setText("Nothing to back up: save directory is empty."); return; }
        fs::path dst = paths_.backups / s(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + suffix);
        try {
            copyTree(paths_.saves, dst);
            status_->setText("Saves backed up to " + q(dst));
        } catch (const std::exception& e) { status_->setText("Backup failed: " + q(e.what())); }
        listBackups();
    }

    void restoreSaves() {
        auto* item = backups_->currentItem();
        fs::path src = item ? paths_.backups / s(item->text()) : fs::path();
        if (!item || !fs::is_directory(src)) { status_->setText("Select a backup first."); return; }
        if (QMessageBox::question(this, "Restore saves", "Replace the current saves with backup " + item->text() +
                                  "?\nThe current saves are backed up first.") != QMessageBox::Yes) return;
        try {
            backupSaves("-before-restore");
            fs::remove_all(paths_.saves);
            copyTree(src, paths_.saves);
            status_->setText("Restored saves from " + item->text());
        } catch (const std::exception& e) { status_->setText("Restore failed: " + q(e.what())); }
        listBackups();
    }

    // ---------------------------------------------------------------------------------------
    QWidget* diagnosticsPage() {
        auto* w = new QWidget;
        diag_ = new QPlainTextEdit;
        diag_->setReadOnly(true);
        auto* refresh = new QPushButton("Refresh");
        auto* exportBtn = new QPushButton("Export report…");
        auto* logs = new QPushButton("Open logs folder");
        connect(refresh, &QPushButton::clicked, this, [this] { diag_->setPlainText(report()); });
        connect(exportBtn, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getSaveFileName(this, "Export diagnostic report",
                                                     "openunderworld-report-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt");
            if (f.isEmpty()) return;
            std::ofstream(s(f)) << s(report());
            status_->setText("Report written to " + f);
        });
        connect(logs, &QPushButton::clicked, this, [this] {
            fs::create_directories(paths_.logs);
            QDesktopServices::openUrl(QUrl::fromLocalFile(q(paths_.logs)));
        });
        auto* row = new QHBoxLayout;
        row->addWidget(refresh);
        row->addWidget(exportBtn);
        row->addWidget(logs);
        row->addStretch();
        auto* lay = new QVBoxLayout(w);
        lay->addWidget(diag_, 1);
        lay->addLayout(row);
        QTimer::singleShot(0, this, [this] { diag_->setPlainText(report()); });
        return w;
    }

    QString report() {
        QStringList r;
        r << "OpenUnderworld diagnostic report, " + QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        r << QString("Launcher %1, Qt %2, SDL %3.%4.%5").arg(OU_VERSION).arg(qVersion())
                 .arg(SDL_VERSIONNUM_MAJOR(SDL_GetVersion())).arg(SDL_VERSIONNUM_MINOR(SDL_GetVersion())).arg(SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
        r << "OS: " + QSysInfo::prettyProductName() + ", kernel " + QSysInfo::kernelVersion() + ", " + QSysInfo::currentCpuArchitecture();
        r << "Session: " + qEnvironmentVariable("XDG_SESSION_TYPE") + " / " + QGuiApplication::platformName();
        r << "Game: " + q(toolPath("ou-game"));
        r << "Viewer: " + q(toolPath("ou-viewer"));
        r << "Updater: installed " + q(ou::update::installedVersion(paths_.installRoot).value_or("none")) + " at " + q(paths_.installRoot) +
                 (ou::update::releasePublicKey() ? ", release key compiled in" : ", NO release key compiled in");
        r << "\n--- config (" + q(paths_.config) + ") ---";
        std::ifstream cf(paths_.config);
        r << (cf ? QString::fromStdString(std::string(std::istreambuf_iterator<char>(cf), {})) : QString("(not saved yet)"));
        r << "--- last import scan ---" << (lastScanSummary_.isEmpty() ? "(none this session)" : lastScanSummary_);
        // Newest game/viewer log, last 60 lines.
        fs::path newest;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(paths_.logs, ec))
            if (e.path().extension() == ".log" && (newest.empty() || e.last_write_time(ec) > fs::last_write_time(newest, ec)))
                newest = e.path();
        r << "--- newest log: " + (newest.empty() ? QString("(none)") : q(newest)) + " ---";
        if (!newest.empty()) {
            std::ifstream lf(newest);
            QStringList lines;
            for (std::string l; std::getline(lf, l);) lines << q(l);
            r << lines.mid(std::max<qsizetype>(0, lines.size() - 60));
        }
        return r.join('\n');
    }

    // ---------------------------------------------------------------------------------------
    QWidget* updatesPage() {
        auto* w = new QWidget;
        versionLabel_ = new QLabel;
        changelog_ = new QPlainTextEdit;
        changelog_->setReadOnly(true);
        updatesEnabled_ = new QCheckBox("Enable updates (OU_DISABLE_UPDATES=1 also disables)");
        updatesEnabled_->setChecked(cfg_.updatesEnabled);
        channel_ = new QComboBox;
        channel_->setEditable(true);
        channel_->addItems({"stable", "beta"});
        channel_->setCurrentText(q(cfg_.updateChannel));
        manifestUrl_ = new QLineEdit(q(cfg_.updateManifestUrl));
        manifestUrl_->setPlaceholderText("https://…/manifest.json (signature: manifest.json.sig)");
        updateStatus_ = new QLabel;
        updateStatus_->setWordWrap(true);
        auto* checkBtn = new QPushButton("Check for updates");
        auto* installBtn = new QPushButton("Download && install");
        auto* rollbackBtn = new QPushButton("Roll back to previous");
        connect(checkBtn, &QPushButton::clicked, this, [this] { runUpdate(false); });
        connect(installBtn, &QPushButton::clicked, this, [this] { runUpdate(true); });
        connect(rollbackBtn, &QPushButton::clicked, this, [this] {
            try {
                if (!paths_.appImage.empty()) {
                    ou::update::rollbackAppImage(paths_.appImage);
                    updateStatus_->setText("Swapped " + q(paths_.appImage) + " with its .bak; restart to use it.");
                } else {
                    updateStatus_->setText("Rolled back; active version " + q(ou::update::rollback(paths_.installRoot)));
                }
            } catch (const std::exception& e) { updateStatus_->setText("Rollback failed: " + q(e.what())); }
            refreshVersion();
        });
        auto* f = new QFormLayout;
        f->addRow("", updatesEnabled_);
        f->addRow("Channel", channel_);
        f->addRow("Manifest URL", manifestUrl_);
        auto* row = new QHBoxLayout;
        row->addWidget(checkBtn);
        row->addWidget(installBtn);
        row->addWidget(rollbackBtn);
        row->addStretch();
        auto* lay = new QVBoxLayout(w);
        lay->addWidget(versionLabel_);
        lay->addWidget(new QLabel("Changelog:"));
        lay->addWidget(changelog_, 1);
        lay->addLayout(f);
        lay->addLayout(row);
        lay->addWidget(updateStatus_);
        refreshVersion();
        return w;
    }

    void refreshVersion() {
        auto inst = ou::update::installedVersion(paths_.installRoot);
        QString managed = !paths_.appImage.empty() ? "AppImage " + q(paths_.appImage) + " (updated in place)"
                          : inst                   ? "install " + q(*inst) + " in " + q(paths_.installRoot)
                                                   : QString("none");
        versionLabel_->setText(QString("<b>OpenUnderworld %1</b> (this launcher)<br>Updater-managed: %2<br>%3")
                                   .arg(OU_VERSION).arg(managed)
                                   .arg(ou::update::releasePublicKey() ? "Release signing key compiled in." : "No release key compiled in: updates unavailable."));
        std::ifstream mf(paths_.installRoot / "current" / ou::update::kManifestName);
        try {
            if (mf) changelog_->setPlainText(q(json::parse(mf).value("changelog", "")));
        } catch (const std::exception&) {}
        if (changelog_->toPlainText().isEmpty()) changelog_->setPlaceholderText("No changelog: no updater-managed version installed yet.");
    }

    void runUpdate(bool install) {
        collect();
        ou::update::Options o;
        try { o = updateOptions(paths_, cfg_); } catch (const std::exception& e) { updateStatus_->setText(q(e.what())); return; }
        updateStatus_->setText(install ? "Downloading…" : "Checking…");
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            updateStatus_->setText(watcher->result());
            watcher->deleteLater();
            refreshVersion();
        });
        watcher->setFuture(QtConcurrent::run([o, install, this]() -> QString {
            try {
                auto r = install ? ou::update::update(o) : ou::update::check(o);
                if (r.status != ou::update::Status::Disabled) {
                    QString cl = q(r.manifest.changelog);
                    QMetaObject::invokeMethod(changelog_, [this, cl] { changelog_->setPlainText(cl); });
                }
                return describe(r, paths_);
            } catch (const std::exception& e) { return QString("Update failed: ") + e.what(); }
        }));
    }

    // ---------------------------------------------------------------------------------------
    void collect() {
        cfg_.gameDir = s(gameDir_->text().trimmed());
        cfg_.drm = s(drm_->text().trimmed());
        cfg_.renderer = s(renderer_->currentData().toString());
        cfg_.width = width_->value();
        cfg_.height = height_->value();
        cfg_.fullscreen = fullscreen_->isChecked();
        cfg_.fov = float(fov_->value());
        cfg_.vsync = vsync_->isChecked();
        cfg_.updatesEnabled = updatesEnabled_->isChecked();
        cfg_.updateChannel = s(channel_->currentText().trimmed());
        cfg_.updateManifestUrl = s(manifestUrl_->text().trimmed());
        auto& g = cfg_.graphics;
        g.preset = s(gfxPreset_->currentData().toString());
        // RT needs the enhanced preset: "auto" resolves to off for original, so resolve() never throws.
        g.rt = rt_->isEnabled() ? s(rt_->currentData().toString()) : "auto";
        g.rtQuality = s(rtQuality_->currentData().toString());
        g.msaa = msaa_->currentData().toInt();
        g.shadows = overrideValue(shadows_);
        g.originalShaders = shaders_->currentIndex() == 1 ? std::optional(false) : std::nullopt; // unset = original
        g.brightness = float(brightness_->value());
        g.water = s(water_->currentData().toString());
        g.waterDetail = s(waterDetail_->currentData().toString());
        g.atmosphere = s(atmosphere_->currentData().toString());
        g.lightShafts = overrideValue(shafts_);
        g.weather = s(weather_->currentData().toString());
        g.upscaler = s(upscaler_->currentData().toString());
        g.fsrQuality = s(fsrQuality_->currentData().toString());
        g.sharpness = float(sharpness_->value());
        g.frameGeneration = g.upscaler == "fsr" && frameGeneration_->isChecked();
        const auto& actions = ou::config::bindableActions();
        for (int i = 0; i < int(actions.size()); ++i) {
            auto& names = cfg_.controls.bindings[actions[i]];
            names.clear();
            for (const QString& n : splitInputs(bindings_->item(i, 0)->text())) names.push_back(s(n));
        }
        cfg_.controls.mouseSensitivity = float(mouseSensitivity_->value());
        cfg_.controls.invertY = invertY_->isChecked();
    }

    bool saveConfig() {
        collect();
        try {
            ou::config::resolve(cfg_.graphics); // the apps reject what this rejects
            for (const auto& [action, names] : cfg_.controls.bindings)
                for (const auto& n : names)
                    if (!ou::platform::parseInput(n)) throw std::runtime_error("control " + action + ": unknown input '" + n + "'");
        } catch (const std::exception& e) {
            status_->setText("Not saved: " + q(e.what()));
            return false;
        }
        try {
            ou::config::save(paths_.config, cfg_);
            status_->setText("Saved " + q(paths_.config));
            return true;
        } catch (const std::exception& e) {
            status_->setText("Saving config failed: " + q(e.what()));
            return false;
        }
    }

    // Updater-managed copy first (not when running as an AppImage), then the binary next to this launcher, then $PATH.
    fs::path toolPath(const char* name) const {
        fs::path managed = paths_.appImage.empty() ? paths_.installRoot / "current/bin" / name : fs::path();
        for (fs::path p : {managed, fs::path(s(QCoreApplication::applicationDirPath())) / name})
            if (!p.empty() && ::access(p.c_str(), X_OK) == 0) return p;
        return name;
    }

    // Saves the config, then starts `name` (ou-game / ou-viewer) --config <it>, logging to logs/<game|viewer>-<time>.log.
    void launch(const char* name) {
        if (!saveConfig()) return;
        fs::create_directories(paths_.logs);
        fs::path log = paths_.logs / s(q(name).mid(3) + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".log");
        const fs::path bin = toolPath(name);
        QProcess p;
        p.setProgram(q(bin));
        p.setArguments({"--config", q(paths_.config)});
        p.setStandardOutputFile(q(log));
        p.setStandardErrorFile(q(log));
        qint64 pid = 0;
        if (p.startDetached(&pid)) status_->setText(QString("Started %1 (pid %2), log %3").arg(name).arg(pid).arg(q(log)));
        else status_->setText("Could not start " + q(bin));
    }
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ou-launcher");
    QApplication::setApplicationVersion(OU_VERSION);
    QApplication::setDesktopFileName("openunderworld");
    QCommandLineParser cli;
    cli.setApplicationDescription("OpenUnderworld launcher");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption config("config", "Config file (default $XDG_CONFIG_HOME/openunderworld/config.json).", "file");
    QCommandLineOption gameDir("game-dir", "Game directory for this session (saved only via Save settings).", "dir");
    QCommandLineOption tab("tab", "Open on tab index.", "n", "0");
    QCommandLineOption scan("scan", "Run the import scan on start.");
    QCommandLineOption selfTest("self-test", "Build the UI, process events once and exit (CI smoke).");
    QCommandLineOption grab("grab", "Save a PNG of the window (after --scan finishes) and exit.", "file");
    QCommandLineOption checkUpdate("check-update", "Check the configured signed manifest, print the result and exit.");
    QCommandLineOption doUpdate("update", "Check and install a newer signed release (the AppImage itself when run as one), then exit.");
    cli.addOptions({config, gameDir, tab, scan, selfTest, grab, checkUpdate, doUpdate});
    cli.process(app);

    Paths paths;
    if (cli.isSet(config)) paths.config = s(cli.value(config));
    if (cli.isSet(checkUpdate) || cli.isSet(doUpdate)) {
        try {
            auto o = updateOptions(paths, ou::config::load(paths.config));
            o.progress = [](const std::string& m) { std::fprintf(stderr, "update: %s\n", m.c_str()); };
            auto r = cli.isSet(doUpdate) ? ou::update::update(o) : ou::update::check(o);
            std::printf("%s\n", s(describe(r, paths)).c_str());
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Update failed: %s\n", e.what());
            return 1;
        }
    }
    cli.process(app);

    // Gamepad listing only; the viewer owns its own SDL instance.
    SDL_Init(SDL_INIT_GAMEPAD);
    Launcher w(paths, cli.value(gameDir));
    w.tabs()->setCurrentIndex(cli.value(tab).toInt());
    w.show();
    if (cli.isSet(grab)) {
        auto save = [&w, file = cli.value(grab)] { w.grab().save(file); QApplication::quit(); };
        if (cli.isSet(scan)) w.onScanDone = save;
        else QTimer::singleShot(500, &w, save);
    }
    if (cli.isSet(scan)) w.startScan();
    if (cli.isSet(selfTest)) {
        QApplication::processEvents();
        SDL_Quit();
        return 0;
    }
    int rc = app.exec();
    SDL_Quit();
    return rc;
}
