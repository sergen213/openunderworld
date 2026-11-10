// ou-game --self-test (src/app/game/SelfTest.*, docs/SELFTEST.md): the condition evaluator, the per-step measurements
// and the scenario checks, without game data. The scenarios themselves run in ou-game (ctest game_selftest_*).
#include "app/game/SelfTest.hpp"

#include <gtest/gtest.h>

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
    }
    // probe names: fixed ones, ids, measurements
    for (const char* p : {"player.state", "player.pos.z", "bike.wheels", "object.6505.joint", "object.6505.uber.2", "object.6505.uberjoint.0",
                          "object.6505.body.z", "enemy.6531.health", "target.0.hits", "portable.7190.in", "step.peak_height", "step.states"})
        EXPECT_TRUE(SelfTest::probeKnown(p)) << p;
    for (const char* p : {"", "player", "player.pos", "player.pos.w", "object.x.joint", "object.6505", "object.6505.uber", "enemy.1.mood", "step.nothing",
                          "target.0", "portable.held.in"})
        EXPECT_FALSE(SelfTest::probeKnown(p)) << p;
}
