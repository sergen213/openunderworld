// ou-game --self-test (src/app/game/SelfTest.*, docs/SELFTEST.md): the condition evaluator, the per-step measurements
// and the scenario checks, without game data. The scenarios themselves run in ou-game (ctest game_selftest_*).
#include "app/game/SelfTest.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>

using namespace ou;
using app::SelfTest;
using app::selftest::Condition;
using app::selftest::Measurements;
using app::selftest::Tracker;
using app::selftest::Value;
using nlohmann::json;

namespace {
Condition cond(const char* text) { return Condition::parse(json::parse(text), "test"); }
using List = std::vector<std::string>;
} // namespace

TEST(SelfTest, ConditionOperators) {
    EXPECT_TRUE(cond(R"({"probe":"player.state","eq":"hang"})").holds(Value(std::string("hang"))));
    EXPECT_FALSE(cond(R"({"probe":"player.state","eq":"hang"})").holds(Value(std::string("fall"))));
    EXPECT_TRUE(cond(R"({"probe":"player.state","ne":"hang"})").holds(Value(std::string("fall"))));
    EXPECT_FALSE(cond(R"({"probe":"player.state","ne":"hang"})").holds(Value(std::string("hang"))));
    EXPECT_TRUE(cond(R"({"probe":"player.state","in":["hang","shimmy"]})").holds(Value(std::string("shimmy"))));
    EXPECT_FALSE(cond(R"({"probe":"player.state","in":["hang","shimmy"]})").holds(Value(std::string("run"))));
    EXPECT_TRUE(cond(R"({"probe":"player.grounded","eq":true})").holds(Value(true)));
    EXPECT_FALSE(cond(R"({"probe":"player.grounded","eq":true})").holds(Value(false)));
    EXPECT_FALSE(cond(R"({"probe":"player.grounded","eq":true})").holds(Value(1.0))); // a number is not a bool

    const auto range = cond(R"({"probe":"player.speed","min":1200,"max":1350})");
    EXPECT_TRUE(range.holds(Value(1274.0)));
    EXPECT_TRUE(range.holds(Value(1200.0)));
    EXPECT_FALSE(range.holds(Value(1199.9)));
    EXPECT_FALSE(range.holds(Value(1350.1)));
    EXPECT_FALSE(range.holds(Value(std::string("1274")))); // a string is not a number
    const auto near = cond(R"({"probe":"step.peak_height","near":201.6,"tol":5})");
    EXPECT_TRUE(near.holds(Value(197.0)));
    EXPECT_TRUE(near.holds(Value(206.6)));
    EXPECT_FALSE(near.holds(Value(207.0)));
    EXPECT_TRUE(cond(R"({"probe":"world.chapter","eq":4})").holds(Value(4.0)));
    EXPECT_FALSE(cond(R"({"probe":"world.chapter","eq":4})").holds(Value(5.0)));

    // an unavailable value (object not loaded, no bike) never satisfies anything, not even "ne"
    EXPECT_FALSE(cond(R"({"probe":"bike.speed","ne":3})").holds(Value()));
    EXPECT_FALSE(cond(R"({"probe":"bike.speed","max":1e9})").holds(Value()));

    // lists: seq = an ordered subsequence, has = contains all
    const auto visits = cond(R"({"visits":["run","jump","hang","shimmy","climbup","run"]})");
    EXPECT_EQ(visits.probe, "step.states");
    EXPECT_TRUE(visits.holds(Value(List{"idle", "run", "jumpprep", "jump", "hang", "shimmy", "hang", "climbup", "idle", "run"})));
    EXPECT_FALSE(visits.holds(Value(List{"idle", "run", "jump", "fall", "land", "run"})));         // never hung
    EXPECT_FALSE(visits.holds(Value(List{"run", "hang", "jump", "shimmy", "climbup", "run"})));    // wrong order
    EXPECT_FALSE(visits.holds(Value(List{"run", "jump", "hang", "shimmy", "climbup"})));           // no run at the end
    EXPECT_FALSE(visits.holds(Value(std::string("run"))));
    const auto calls = cond(R"({"calls":["6505:54","6502:20"]})");
    EXPECT_EQ(calls.probe, "step.calls");
    EXPECT_TRUE(calls.holds(Value(List{"1:2", "6502:20", "6505:54"})));
    EXPECT_FALSE(calls.holds(Value(List{"6505:54"})));
    EXPECT_TRUE(cond(R"({"probe":"player.clip","has":["ng_run","sprint_loop"]})").holds(Value(std::string("sprint_loop"))));
    EXPECT_FALSE(cond(R"({"probe":"player.clip","has":["ng_run","sprint_loop"]})").holds(Value(std::string("ng_idle"))));
}

TEST(SelfTest, ConditionsOverAStep) {
    const auto run = [](const char* text, std::initializer_list<Value> samples, Value end) {
        Tracker t(cond(text));
        for (const auto& v : samples) t.sample(v);
        return t.finish(end);
    };
    // end: only the final value counts
    EXPECT_TRUE(run(R"({"probe":"player.state","eq":"idle"})", {Value(std::string("run"))}, Value(std::string("idle"))).passed);
    EXPECT_FALSE(run(R"({"probe":"player.state","eq":"run"})", {Value(std::string("run"))}, Value(std::string("idle"))).passed);
    // max / min: the extreme of the step, reported as the actual
    auto r = run(R"({"probe":"player.speed","over":"max","min":1200,"max":1350})", {Value(0.0), Value(900.0), Value(1274.0), Value(300.0)}, Value(0.0));
    EXPECT_TRUE(r.passed);
    EXPECT_EQ(std::get<double>(r.actual), 1274.0);
    r = run(R"({"probe":"player.speed","over":"max","min":1200})", {Value(0.0), Value(900.0)}, Value(5000.0)); // the end value is no sample
    EXPECT_FALSE(r.passed);
    EXPECT_EQ(std::get<double>(r.actual), 900.0);
    r = run(R"({"probe":"player.air","over":"min","max":0.99})", {Value(1.0), Value(0.97), Value(1.0)}, Value(1.0));
    EXPECT_TRUE(r.passed);
    EXPECT_EQ(std::get<double>(r.actual), 0.97);
    // any: one tick is enough; all: every tick
    EXPECT_TRUE(run(R"({"probe":"player.state","over":"any","eq":"hang"})", {Value(std::string("jump")), Value(std::string("hang")), Value(std::string("fall"))}, Value(std::string("fall"))).passed);
    EXPECT_FALSE(run(R"({"probe":"player.state","over":"any","eq":"hang"})", {Value(std::string("jump")), Value(std::string("fall"))}, Value(std::string("hang"))).passed);
    EXPECT_TRUE(run(R"({"probe":"bike.wheels","over":"all","min":1})", {Value(2.0), Value(1.0), Value(2.0)}, Value(0.0)).passed);
    r = run(R"({"probe":"bike.wheels","over":"all","min":1})", {Value(2.0), Value(0.0), Value(2.0)}, Value(2.0));
    EXPECT_FALSE(r.passed);
    EXPECT_EQ(std::get<double>(r.actual), 0.0); // the first tick that broke it
    // nothing sampled: nothing proven
    EXPECT_FALSE(run(R"({"probe":"bike.wheels","over":"all","min":1})", {}, Value(2.0)).passed);
    EXPECT_FALSE(run(R"({"probe":"player.speed","over":"max","max":1e9})", {}, Value(2.0)).passed);
}

TEST(SelfTest, MeasurementsOfAJump) {
    Measurements m;
    m.begin({100, 200, 50}, "idle", "ng_idle", 1000);
    glm::vec3 p{100, 200, 50};
    for (int i = 0; i < 6; ++i) m.tick(p, "jumpprep", "ng_standing_jump", false, 1000); // on the ground
    // a ballistic arc at 60 Hz: 1441.2 up, 600 along +x, gravity 5151.99 (docs/GAMEPLAY.md "Numbers")
    const float vz = 1441.2f, g = 5151.99f, dt = 1 / 60.0f;
    int air = 0;
    for (float t = dt;; t += dt) {
        const float z = vz * t - 0.5f * g * t * t;
        if (z <= 0) break;
        p = {100 + 600 * t, 200, 50 + z};
        m.tick(p, vz - g * t > 0 ? "jump" : "fall", "jump_loop", true, 1000);
        ++air;
    }
    p.z = 50;
    m.tick(p, "land", "ng_land", false, 880);
    m.tick(p, "idle", "ng_idle", false, 880);
    EXPECT_NEAR(std::get<double>(m.get("peak_height")), 201.6, 2.0); // vz^2 / 2g
    EXPECT_NEAR(std::get<double>(m.get("air_time")), 0.56, 0.02);
    EXPECT_EQ(std::get<double>(m.get("air_ticks")), air);
    EXPECT_NEAR(std::get<double>(m.get("height_gain")), 0.0, 1e-3);
    EXPECT_NEAR(std::get<double>(m.get("horizontal")), 600 * air * dt, 1.0);
    EXPECT_NEAR(std::get<double>(m.get("peak_speed")), 600.0, 1.0);
    EXPECT_NEAR(std::get<double>(m.get("peak_rise_speed")), vz - g * dt / 2, 2.0);
    EXPECT_GT(std::get<double>(m.get("moved")), std::get<double>(m.get("horizontal")) + 150); // the path goes up and down
    EXPECT_EQ(std::get<double>(m.get("health_lost")), 120.0);
    EXPECT_EQ(std::get<double>(m.get("ticks")), 6 + air + 2);
    EXPECT_EQ(std::get<List>(m.get("states")), (List{"idle", "jumpprep", "jump", "fall", "land", "idle"}));
    EXPECT_EQ(std::get<List>(m.get("clips")), (List{"ng_idle", "ng_standing_jump", "jump_loop", "ng_land", "ng_idle"}));
    EXPECT_TRUE(cond(R"({"visits":["jumpprep","jump","fall","land"]})").holds(m.get("states")));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(m.get("no_such_measurement")));

    // a respawn / placement is a teleport, not motion
    m.tick(p + glm::vec3(20000, 0, 3000), "idle", "ng_idle", false, 1000);
    EXPECT_EQ(std::get<double>(m.get("teleports")), 1.0);
    EXPECT_NEAR(std::get<double>(m.get("peak_speed")), 600.0, 1.0);
    m.note(m.calls, "5:54"), m.note(m.calls, "7:20"), m.note(m.calls, "5:54");
    EXPECT_EQ(std::get<List>(m.get("calls")), (List{"5:54", "7:20"}));
    const json j = m.json();
    EXPECT_EQ(j.at("start"), (json{100.0, 200.0, 50.0}));
    EXPECT_EQ(j.at("air_ticks"), air);
    EXPECT_EQ(j.at("states").size(), 6u);
}

TEST(SelfTest, ScenarioChecks) {
    const auto scenario = [](const char* stepText) {
        json j = {{"version", 1}, {"name", "t"}, {"level", "l4_pool"}, {"steps", json::array({json::parse(stepText)})}};
        return j;
    };
    const char* good = R"({"step":"run","input":["0 forward 1","2 forward 0"],"duration":3,
                           "expect":[{"probe":"step.horizontal","min":1500},{"probe":"player.speed","over":"max","near":1274,"tol":40}]})";
    {
        const SelfTest t = SelfTest::parse(scenario(good));
        EXPECT_EQ(t.entry().level, "l4_pool");
        EXPECT_FALSE(t.entry().spawn);
        EXPECT_FALSE(t.done());
        EXPECT_FALSE(t.passed()); // nothing ran
        const json r = t.report();
        EXPECT_EQ(r.at("game"), "Tomb Raider: Underworld");
        EXPECT_EQ(r.at("platform"), "Linux"); // the brief's report schema: version, game, platform, steps
        EXPECT_EQ(r.at("version"), 1);
        EXPECT_FALSE(t.usesSaves());
        EXPECT_EQ(r.at("passed"), false);
        EXPECT_EQ(r.at("steps").at(0).at("passed"), false);
    }
    const auto rejects = [&](json j, const char* what) {
        try {
            (void)SelfTest::parse(j);
            ADD_FAILURE() << "accepted: " << what;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(what), std::string::npos) << e.what();
        }
    };
    rejects(scenario(R"({"step":"a","input":"0 forward 1","duration":1})"), "no 'expect'");
    rejects(scenario(R"({"step":"a","input":"0 forward 1","duration":1,"expect":[]})"), "'expect' is empty");
    rejects(scenario(R"({"step":"a","duration":1,"expect":[{"probe":"player.state"}]})"), "no operator");
    rejects(scenario(R"({"step":"a","duration":1,"expect":[{"probe":"player.mood","eq":"calm"}]})"), "unknown probe");
    rejects(scenario(R"({"step":"a","duration":1,"expect":[{"probe":"player.state","equals":"idle"}]})"), "unknown key");
    rejects(scenario(R"({"step":"a","duration":1,"expect":[{"probe":"player.pos.z","near":3}]})"), "'tol'");
    rejects(scenario(R"({"step":"a","duration":1,"expect":[{"probe":"step.moved","over":"max","min":3}]})"), "no 'over'");
    rejects(scenario(R"({"step":"a","expect":[{"probe":"player.state","eq":"idle"}]})"), "'duration'");
    rejects(scenario(R"({"step":"a","input":"0 fly 1","duration":1,"expect":[{"probe":"player.state","eq":"idle"}]})"), "input script line 1");
    rejects(scenario(R"({"step":"a","plan":"teleport 1 2","duration":1,"expect":[{"probe":"player.state","eq":"idle"}]})"), "autopilot plan line 1");
    rejects(scenario(R"({"step":"a","duration":1,"until":{"probe":"player.speed","over":"max","min":1},"expect":[{"probe":"player.state","eq":"idle"}]})"),
            "'until'");
    rejects(scenario(R"({"step":"a","duration":1,"fixtures":[{"godmode":true}],"expect":[{"probe":"player.state","eq":"idle"}]})"), "fixture");
    {
        json two = scenario(good);
        two["steps"].push_back(two["steps"][0]);
        rejects(two, "used twice");
        json entry = scenario(good);
        entry["start_chapter"] = 4;
        rejects(entry, "exactly one of");
        entry.erase("level"), entry.erase("start_chapter");
        rejects(entry, "exactly one of");
        json version = scenario(good);
        version["version"] = 2;
        rejects(version, "'version'");
        json key = scenario(good);
        key["levle"] = "x";
        rejects(key, "unknown key");
        json manor = scenario(good); // a unit of two chapters played in the one named
        manor["level"] = "l3_main_hall", manor["level_chapter"] = 0;
        const SelfTest prologue = SelfTest::parse(manor);
        EXPECT_EQ(prologue.entry().levelChapter, 0);
        EXPECT_EQ(prologue.entry().chapter, -1) << "not a chapter start";
        EXPECT_EQ(prologue.report().at("entry").at("level_chapter"), 0);
        manor["level_chapter"] = 99;
        rejects(manor, "'level_chapter' is 0..");
        json alone = scenario(good);
        alone.erase("level"), alone["start_chapter"] = 0, alone["level_chapter"] = 0;
        rejects(alone, "goes with 'level'");
    }
    // probe names: fixed ones, ids, measurements, engine / HUD / audio, inventory, save
    for (const char* p : {"player.state", "player.pos.z", "bike.wheels", "object.6505.joint", "object.6505.uber.2", "object.6505.uberjoint.0",
                          "object.6505.body.z", "enemy.6531.health", "target.0.hits", "portable.7190.in", "step.peak_height", "step.states",
                          "engine.sdl.video", "engine.renderer", "engine.ao_as_requested", "engine.water_surfaces", "hud.health_alpha", "hud.bar_kind",
                          "audio.music_state", "camera.mode_name", "inventory.weapons", "inventory.health_packs", "physics.bodies", "save.restored",
                          "save.differs"})
        EXPECT_TRUE(SelfTest::probeKnown(p)) << p;
    for (const char* p : {"", "player", "player.pos", "player.pos.w", "object.x.joint", "object.6505", "object.6505.uber", "enemy.1.mood", "step.nothing",
                          "target.0", "portable.held.in", "engine", "engine.sdl", "engine.gpu", "hud", "audio.volume", "inventory", "save"})
        EXPECT_FALSE(SelfTest::probeKnown(p)) << p;
    // save / load input lines mark the scenario (ou-game then wants a redirected XDG_DATA_HOME)
    EXPECT_TRUE(SelfTest::parse(scenario(R"({"step":"s","input":["0.2 save 1","0.3 save 0"],"duration":1,"expect":{"probe":"save.saves","eq":1}})"))
                    .usesSaves());
    EXPECT_TRUE(SelfTest::parse(scenario(R"({"step":"l","input":"0.2 load 1 # reload\n0.3 load 0","duration":1,"expect":{"probe":"save.loads","eq":1}})"))
                    .usesSaves());
    EXPECT_FALSE(SelfTest::parse(scenario(R"({"step":"c","input":"0.2 jump 1 # no save here","duration":1,"expect":{"probe":"save.saves","eq":0}})"))
                     .usesSaves());
    // SDL device events (step "events"): EventLog lines with seconds; a malformed one rejects the scenario
    const SelfTest pad = SelfTest::parse(scenario(R"({"step":"p","events":["0 padadd 1","0.1 button 1 0 1  # A","0.2 button 1 0 0","","1 axis 1 1 -32767"],
                                                    "duration":2,"expect":{"probe":"player.state","eq":"run"}})"));
    EXPECT_TRUE(pad.usesEvents());
    EXPECT_FALSE(SelfTest::parse(scenario(good)).usesEvents());
    rejects(scenario(R"({"step":"e","events":["0.1 joystick 1 0 1"],"duration":1,"expect":{"probe":"player.state","eq":"idle"}})"), "unknown event");
    rejects(scenario(R"({"step":"e","events":["key 44 1"],"duration":1,"expect":{"probe":"player.state","eq":"idle"}})"), "'<seconds> <event> <args>'");
    rejects(scenario(R"({"step":"e","events":["0.5 key 44 1","0.2 key 44 0"],"duration":1,"expect":{"probe":"player.state","eq":"idle"}})"),
            "ticks go backwards");
    rejects(scenario(R"({"step":"e","events":["-1 key 44 1"],"duration":1,"expect":{"probe":"player.state","eq":"idle"}})"), "negative time");
    // the fighting autopilot's line, checkpoint position, HUD reticle / toasts, sounds
    EXPECT_NO_THROW((void)SelfTest::parse(scenario(R"({"step":"f","plan":["fight","fight 2500 40"],"duration":45,"expect":{"probe":"enemies.alive","eq":0}})")));
    for (const char* p : {"world.checkpoint_pos.x", "world.checkpoint_pos.z", "world.checkpoint_distance", "hud.reticle", "hud.reticle_slowmo", "hud.toast",
                          "hud.title", "hud.hint", "step.sounds", "step.toasts", "step.titles", "step.hints"})
        EXPECT_TRUE(SelfTest::probeKnown(p)) << p;
    for (const char* p : {"world.checkpoint_pos", "world.checkpoint_pos.w", "step.sound", "hud.reticles"}) EXPECT_FALSE(SelfTest::probeKnown(p)) << p;
}

// The engine.* / hud.* / audio.* probes read ou-game's selftest::Engine: each answers from its own field, and the
// frame-derived ones are unavailable until a frame was drawn (--draw-last), so an engine scenario cannot pass headless.
TEST(SelfTest, EngineProbes) {
    using app::selftest::Engine;
    using app::selftest::engineProbe;
    Engine e;
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "engine.renderer")), nullptr) << "no renderer initialised";
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "engine.ao")), nullptr) << "no frame drawn";
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "engine.water_as_requested")), nullptr);
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "audio.music_state")), "off");
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "audio.voices")), nullptr) << "no mixer: no voice count";
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "engine.nothing")), nullptr);
    for (const auto name : app::selftest::engineProbeNames()) EXPECT_TRUE(SelfTest::probeKnown(name)) << name;

    e.sdlVideo = true, e.renderer = "opengl", e.device = "OpenGL 4.6 | test", e.frames = 3;
    e.water = "enhanced", e.waterSurfaces = 2, e.aoRequested = "cortao", e.ao = "cortao", e.rtRequested = "off";
    EXPECT_TRUE(cond(R"({"probe":"engine.sdl.video","eq":true})").holds(engineProbe(e, "engine.sdl.video")));
    EXPECT_FALSE(cond(R"({"probe":"engine.sdl.audio","eq":true})").holds(engineProbe(e, "engine.sdl.audio")));
    EXPECT_TRUE(cond(R"({"probe":"engine.renderer","in":["opengl","vulkan"]})").holds(engineProbe(e, "engine.renderer")));
    const auto asRequested = [&](const char* probe) { return std::get<bool>(engineProbe(e, probe)); };
    EXPECT_TRUE(asRequested("engine.water_as_requested"));
    EXPECT_TRUE(asRequested("engine.ao_as_requested"));
    EXPECT_TRUE(asRequested("engine.rt_as_requested"));
    e.waterSurfaces = 0; // Enhanced asked, nothing drawn with its shading
    EXPECT_FALSE(asRequested("engine.water_as_requested"));
    e.water = "original";
    EXPECT_TRUE(asRequested("engine.water_as_requested"));
    e.ao = "off"; // CORTAO asked, none drawn
    EXPECT_FALSE(asRequested("engine.ao_as_requested"));
    EXPECT_FALSE(cond(R"({"probe":"engine.ao","eq":"cortao"})").holds(engineProbe(e, "engine.ao")));
    e.aoRequested = "rt", e.ao = "cortao"; // RT AO without rays falls back to CORTAO
    EXPECT_TRUE(asRequested("engine.ao_as_requested"));
    e.rtActive = true;
    EXPECT_FALSE(asRequested("engine.ao_as_requested"));
    EXPECT_FALSE(asRequested("engine.rt_as_requested")) << "tracing although RT is off / unsupported";
    e.rtRequested = "shadows", e.rtSupported = true, e.ao = "rt";
    EXPECT_TRUE(asRequested("engine.rt_as_requested"));
    EXPECT_TRUE(asRequested("engine.ao_as_requested"));
    e.rtActive = false; // supported and asked, not traced
    EXPECT_FALSE(asRequested("engine.rt_as_requested"));

    e.hudAlpha = {1.0f, 0.5f, 0.0f}, e.hudHealth = 0.7f, e.hudBar = 0.4f, e.hudHealthPacks = 2, e.hudFrames = 10;
    EXPECT_TRUE(cond(R"({"probe":"hud.health","near":0.7,"tol":1e-5})").holds(engineProbe(e, "hud.health")));
    EXPECT_TRUE(cond(R"({"probe":"hud.weapons_alpha","near":0.5,"tol":1e-6})").holds(engineProbe(e, "hud.weapons_alpha")));
    EXPECT_TRUE(cond(R"({"probe":"hud.gear_alpha","eq":0})").holds(engineProbe(e, "hud.gear_alpha")));
    EXPECT_TRUE(cond(R"({"probe":"hud.bar_kind","eq":"adrenaline"})").holds(engineProbe(e, "hud.bar_kind")));
    e.hudBreath = true;
    EXPECT_TRUE(cond(R"({"probe":"hud.bar_kind","eq":"breath"})").holds(engineProbe(e, "hud.bar_kind")));
    EXPECT_TRUE(cond(R"({"probe":"hud.health_packs","eq":2})").holds(engineProbe(e, "hud.health_packs")));
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "hud.reticle")), "none");
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "hud.toast")), "") << "nothing shown";
    e.reticle = "locked", e.reticleSlowMo = true, e.toast = "HEALTH PACKS  1", e.title = "CROFT MANOR", e.hint = "Press \u1803 to jump";
    EXPECT_TRUE(cond(R"({"probe":"hud.reticle","eq":"locked"})").holds(engineProbe(e, "hud.reticle")));
    EXPECT_TRUE(cond(R"({"probe":"hud.reticle_slowmo","eq":true})").holds(engineProbe(e, "hud.reticle_slowmo")));
    EXPECT_TRUE(cond(R"({"probe":"hud.toast","eq":"HEALTH PACKS  1"})").holds(engineProbe(e, "hud.toast")));
    EXPECT_TRUE(cond(R"({"probe":"hud.title","eq":"CROFT MANOR"})").holds(engineProbe(e, "hud.title")));
    EXPECT_TRUE(cond(R"({"probe":"hud.hint","has":["Press \u1803 to jump"]})").holds(engineProbe(e, "hud.hint")));

    e.audioOn = true, e.voices = 5;
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "audio.music_state")), "idle");
    e.musicIdle = false;
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "audio.music_state")), "loading");
    e.music = {"l1_seatop_mus_01"};
    EXPECT_EQ(app::selftest::toJson(engineProbe(e, "audio.music_state")), "playing");
    EXPECT_TRUE(cond(R"({"probe":"audio.music","has":["l1_seatop_mus_01"]})").holds(engineProbe(e, "audio.music")));
    EXPECT_TRUE(cond(R"({"probe":"audio.voices","min":1})").holds(engineProbe(e, "audio.voices")));

    EXPECT_EQ(app::selftest::cameraModeName(1), "foot");
    EXPECT_EQ(app::selftest::cameraModeName(11), "hang");
    EXPECT_EQ(app::selftest::cameraModeName(19), "underwater");
    EXPECT_EQ(app::selftest::cameraModeName(-1), "none");
    EXPECT_EQ(app::selftest::cameraModeName(7), "mode 7");
}

// Save / load input runs only into a redirected data directory: never the user's ~/.local/share.
TEST(SelfTest, SaveDirectoryGuard) {
    const auto keep = [](const char* name) { const char* v = std::getenv(name); return v ? std::optional<std::string>(v) : std::nullopt; };
    const auto data = keep("XDG_DATA_HOME"), home = keep("HOME");
    const auto restore = [](const char* name, const std::optional<std::string>& v) { v ? (void)setenv(name, v->c_str(), 1) : (void)unsetenv(name); };
    setenv("HOME", "/tmp/ou-selftest-home", 1);
    unsetenv("XDG_DATA_HOME");
    EXPECT_FALSE(app::selftest::unsafeSaveReason().empty()) << "unset";
    setenv("XDG_DATA_HOME", "", 1);
    EXPECT_FALSE(app::selftest::unsafeSaveReason().empty()) << "empty";
    setenv("XDG_DATA_HOME", "/tmp/ou-selftest-home/.local/share", 1);
    EXPECT_FALSE(app::selftest::unsafeSaveReason().empty()) << "the user's own";
    setenv("XDG_DATA_HOME", "/tmp/ou-selftest-home/.local/share/", 1);
    EXPECT_FALSE(app::selftest::unsafeSaveReason().empty()) << "the user's own, spelt with a slash";
    setenv("XDG_DATA_HOME", "/tmp/ou-selftest-data", 1);
    EXPECT_TRUE(app::selftest::unsafeSaveReason().empty());
    restore("XDG_DATA_HOME", data), restore("HOME", home);
}
