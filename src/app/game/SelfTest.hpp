// ou-game --self-test FILE.json: a gameplay scenario run in the real game (bigfile, level load / streaming, scripts,
// physics, the Actions path of --input-script) at fixed 60 Hz ticks. Each step sends scripted input and / or an autopilot
// plan, then its expectations are checked against the World's own state and against what was measured while the step
// ran; the result is a JSON report. Contract (schema, probes, operators, report): docs/SELFTEST.md.
#pragma once

#include "app/game/Autopilot.hpp"
#include "game/Game.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ou::app {
namespace selftest {

// A probe's value; monostate = unavailable (the object is not loaded, Lara is not on a bike, ...).
using Value = std::variant<std::monostate, bool, double, std::string, std::vector<std::string>>;
nlohmann::json toJson(const Value& v); // null / bool / number / string / array of strings

// What of the step's ticks a condition is tested on: the state when the step ends, the largest / smallest value seen
// (numbers), or the operators holding at one tick / at every tick.
enum class Over : uint8_t { End, Max, Min, Any, All };

// One condition: a probe and operators; every operator given must hold.
struct Condition {
    std::string probe;       // "player.state", "step.peak_height", ... (docs/SELFTEST.md "Probes")
    Over over = Over::End;
    nlohmann::json expected; // the operators as written, for the report
    std::optional<nlohmann::json> eq, ne;         // a scalar; numbers compare within 1e-6
    std::optional<double> min, max, near;         // numbers; near with tol
    double tol = 0;
    std::optional<std::vector<nlohmann::json>> in; // equal to one of them
    std::optional<std::vector<std::string>> seq;   // lists: these appear in this order (other entries may lie between)
    std::optional<std::vector<std::string>> has;   // lists: every one of these is in it; a string: equal to one
    // Throws std::runtime_error naming `where` for anything but a well-formed condition with at least one operator.
    static Condition parse(const nlohmann::json& j, const std::string& where);
    bool holds(const Value& v) const; // false for an unavailable value or an operator its type cannot answer
};

// A condition followed over a step: sample() after every tick, finish() with the value at the step's end.
class Tracker {
public:
    explicit Tracker(Condition c) : c_(std::move(c)) {}
    void sample(const Value& v);
    struct Result { bool passed = false; Value actual; };
    Result finish(const Value& end) const;
    const Condition& condition() const { return c_; }

private:
    Condition c_;
    Value seen_;        // Max / Min: the extreme; Any: the first value that held (else the last); All: the first that did not
    bool any_ = false, all_ = true, sampled_ = false;
};

// What a step did, recorded for every step whether or not it is expected (the report's "measurements", and the
// `step.*` probes). Speeds are Lara's root displacement per tick x 60; a tick that moves her more than kTeleport, or
// the tick she leaves the dead state, is a relocation (respawn, cinematic placement) and counts as a teleport, not as
// motion.
struct Measurements {
    static constexpr float kTeleport = 600.0f; // units in one tick (36 000 units/s; clip-end root snaps reach ~350)
    glm::vec3 start{0}, end{0};
    double travelled = 0;           // path length
    float peakZ = 0, minZ = 0;      // highest / lowest root z
    float peakSpeed = 0;            // horizontal, units/s
    float peakRise = 0, peakFall = 0; // vertical speed up / down, units/s (both >= 0)
    int ticks = 0, airTicks = 0, teleports = 0;
    float healthStart = 0, healthEnd = 0, healthMin = 0;
    float speed = 0, vspeed = 0;    // the last tick's
    std::vector<std::string> states, clips; // distinct in the order entered (a state re-entered is listed again)
    std::vector<std::string> calls;         // script virtual calls "callee:vt", first-call order
    std::vector<std::string> events;        // script events "kind:source>target", in order (repeats kept once)
    std::vector<std::string> pickups;       // "kind:id" collected
    std::vector<std::string> hazards;       // "object name:damage" of each contact-hazard hit that hurt her, in order

    void begin(glm::vec3 pos, std::string_view state, std::string_view clip, float health);
    void tick(glm::vec3 pos, std::string_view state, std::string_view clip, bool airborne, float health);
    void note(std::vector<std::string>& list, std::string entry); // appended unless already there
    Value get(std::string_view name) const; // "moved", "peak_height", "states", ...; monostate = no such measurement
    nlohmann::json json() const;
};
// The measurement names Measurements::get answers (the probes "step.<name>").
const std::vector<std::string_view>& measurementNames();

} // namespace selftest

class SelfTest {
public:
    // Throws std::runtime_error (malformed scenario: bad JSON, unknown keys / probes / operators, a step without
    // expectations, bad input or plan lines).
    static SelfTest load(const std::filesystem::path& file);
    static SelfTest parse(const nlohmann::json& scenario, std::string file = {});

    // How the scenario enters gameplay (ou-game's --level / --spawn / --chapter / --load).
    struct Entry {
        std::string level;                         // unit name ("l4_pool"), empty with chapter / load
        std::optional<std::array<float, 4>> spawn; // x, y, z (unit frame), yaw degrees
        int chapter = -1;
        std::string load;        // save slot
        bool cinematics = false; // "cinematics": true plays script cutscenes (default: skipped, their events still run)
    };
    const Entry& entry() const { return entry_; }
    const std::string& name() const { return name_; }
    bool trace = false; // --self-test-trace: one stderr line per tick (state, clip, position, speed)

    // Before the first tick: the scenario's fixtures, the script-call hook. `w` must outlive the run.
    void attach(game::World& w);
    game::Actions actions(game::World& w); // this tick's input (starts the next step when one is due)
    void observe(game::World& w);          // after World::tick: measure, test `until` / the timeout, close the step
    void paused();                         // a tick the world did not run (a menu is up): counts toward the timeout
    bool done() const { return cur_ >= steps_.size(); }
    bool passed() const;
    nlohmann::json report() const;
    std::string summary() const; // one line per step + the verdict (stdout)

    // The value of probe `name` now; monostate when it cannot be answered. probeKnown: `name` is a probe this build
    // answers (checked when a scenario loads).
    selftest::Value probe(game::World& w, std::string_view name) const;
    static bool probeKnown(std::string_view name);

private:
    struct Step {
        std::string id, input, plan;
        uint64_t ticks = 0; // duration
        std::vector<selftest::Condition> until, expect;
        std::vector<nlohmann::json> fixtures;
    };
    struct StepResult {
        bool ran = false, passed = false;
        std::string detail, endedBy;
        selftest::Measurements m;
        nlohmann::json expectations = nlohmann::json::array(), end = nlohmann::json::object();
    };
    void applyFixture(game::World& w, const nlohmann::json& f, const std::string& step);
    void begin(game::World& w);
    void close(game::World& w, const std::string& endedBy, bool endedOk, const std::string& why);

    std::string name_, file_, chapterLabel_, walkthrough_;
    Entry entry_;
    bool stopOnFail_ = true;
    std::vector<nlohmann::json> fixtures_;
    std::vector<Step> steps_;
    std::vector<StepResult> results_;
    nlohmann::json fixturesUsed_ = nlohmann::json::array();
    size_t cur_ = 0;
    bool started_ = false;
    uint64_t stepTick_ = 0, totalTicks_ = 0;
    game::InputScript script_;
    std::optional<Autopilot> pilot_;
    std::vector<selftest::Tracker> trackers_;
    selftest::Measurements m_;
    std::vector<std::unique_ptr<game::DummyTarget>> dummies_; // "target" fixtures (World::targets holds the pointers)
    uint64_t simHash_ = 0;
};

} // namespace ou::app
