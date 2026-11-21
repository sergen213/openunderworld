// Shadow mode: Lara's chart beside the controller (Chart.hpp Shadow; docs/re/PLAYER_CHART.md "Shadow mode").
#include "game/Chart.hpp"
#include "game/Game.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace ou::game::chart {

namespace {

bool sameClip(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

// The contract's input bitmask: 1 Fwd, 2 Back, 4 Left, 8 Right, 0x10 Jump, 0x20 Interact, 0x40 Crouch, 0x80 Sprint,
// 0x100 Walk, 0x200 Grapple (the stick against the camera, 0.3 dead zone).
unsigned inputMask(const Actions& a) {
    unsigned m = 0;
    if (a.move.y > 0.3f) m |= 1;
    if (a.move.y < -0.3f) m |= 2;
    if (a.move.x < -0.3f) m |= 4;
    if (a.move.x > 0.3f) m |= 8;
    if (a.jump) m |= 0x10;
    if (a.interact) m |= 0x20;
    if (a.drop) m |= 0x40;
    if (a.sprint) m |= 0x80;
    if (a.walk) m |= 0x100;
    if (a.grapple) m |= 0x200;
    return m;
}

} // namespace

// The chart's view of World / Player: input actions timed here from the same Actions the world got.
struct Shadow::Impl : ChartContext {
    enum Button { Jump, Crouch, Sprint, Interact, Grapple, ManualAim, Grenade, ReAim, kButtons };
    std::array<ButtonTime, kButtons> buttons;
    // Seconds since each button's last press (never pressed: as long ago as tru.exe's zeroed press time can be).
    std::array<float, kButtons> sincePressed = [] { std::array<float, kButtons> t; t.fill(1e9f); return t; }();
    const World* w = nullptr;
    glm::vec2 stickWorld{0};
    float stickLen = 0;
    std::ofstream trace;
    bool log = false, started = false, lastMatch = true;
    uint64_t ticks = 0, matched = 0, diverged = 0, divergences = 0, lastChanges = 0;
    int lastState = -1, stateTicks = 0;

    int button(int id) const {
        switch (id) { // generalbank action map DTP 47 (docs/GAMEPLAY.md "Button timing", "Rope inputs", Melee.cpp)
        case 52: case 47: return Jump;
        case 50: case 51: case 101: return Crouch;
        case 42: return Sprint;
        // 112 (context 14), 34 (context 17), 79 (context 0): control 2 = virtual button 0, Interact
        // (openunderworld-work/gameplaygoal/melee/am.txt A89 / A103 / A119) [O]; the contexts are not modelled [I].
        case 99: case 32: case 44: case 48: case 75: case 112: case 34: case 79: return Interact;
        case 46: return Grapple;
        case 25: case 37: return ManualAim;
        case 102: return Grenade;
        // 28 (context 0): control 29 = virtual button 8 (ours "recenter", Q / pad left stick click), second control 9 =
        // virtual button 9 (Manual Aim) (am.txt A118) [O].
        case 28: return ReAim;
        default: return -1;
        }
    }
    Action action(int id) const override {
        const int b = button(id);
        if (b < 0) return {};
        const ButtonTime& t = buttons[size_t(b)];
        return {true, t.down, t.edge, t.time};
    }
    float stickLevel() const override { return stickLen; }
    glm::vec2 stick() const override { return stickWorld; }
    // ---- group A ----
    std::optional<float> sincePress(int id) const override {
        const int b = button(id);
        return b < 0 ? std::nullopt : std::optional<float>(sincePressed[size_t(b)]);
    }
    std::optional<bool> lowCeiling() const override { return !w->player().roomToStand(); }
    // 0x6F2800 [I]: ours a weapon in hand (or being drawn) or the target lock held (input 33, Target Lock).
    std::optional<bool> combatMode() const override { return w->combat().drawn || aimHeld; }
    // 0x74FD70 [I]: ours the run's sprint rule (Player.cpp: stamina spent within sprintSeconds - sprintResume).
    std::optional<bool> canSprint() const override {
        const auto& t = w->player().tuning();
        return w->player().state().sprintUsed <= t.sprintSeconds - t.sprintResume;
    }
    std::optional<bool> wallJumpAttach() const override { return w->player().wallJumpWall().has_value(); }
    // Agent +0x90: game ms of the last too-steep ground contact (message 0x1C9; ours Player::steepUnder each tick).
    std::optional<bool> wallSlideRecent() const override { return lastSteepMs > 0 && nowMs - lastSteepMs <= 99; }
    bool aimHeld = false;
    uint32_t nowMs = 0, lastSteepMs = 0;
    // ---- end group A ----
    float fallDistance() const override {
        const PlayerState& s = w->player().state();
        return s.fallStartZ - s.pos.z;
    }
    const PlayerState* player() const override { return &w->player().state(); }
    const World* world() const override { return w; }
};

Shadow::Shadow(const cdc::BigfileSet& bigfile, std::optional<std::filesystem::path> trace, bool log) : impl_(std::make_unique<Impl>()) {
    auto c = loadCharts(bigfile);
    if (!c) throw std::runtime_error("chart shadow: lara.drm player data not found");
    charts_ = std::make_unique<Charts>(std::move(*c));
    rt_ = std::make_unique<ChartRuntime>(*charts_, charts_->player, Registry::player());
    impl_->log = log;
    if (trace) {
        impl_->trace.open(*trace);
        if (!impl_->trace) throw std::runtime_error("chart shadow: cannot write " + trace->string());
        impl_->trace << "tick\tstate\tparent\trequest\tstateTicks\tpx\tpy\tpz\tvx\tvy\tvz\tyaw\tinputs\n";
    }
}
Shadow::~Shadow() = default;

void Shadow::tick(const World& world, const Actions& a) {
    Impl& m = *impl_;
    m.w = &world;
    const float dt = float(kTickSeconds); // [I] real time: the chart's timeMult ignores the combat time scale here
    const bool down[Impl::kButtons] = {a.jump, a.drop, a.sprint, a.interact, a.grapple, a.manualAim, a.grenade, a.recenter || a.manualAim};
    for (size_t i = 0; i < Impl::kButtons; ++i) {
        m.buttons[i].update(down[i], dt, 0.0f);
        m.sincePressed[i] = m.buttons[i].pressed() ? 0.0f : m.sincePressed[i] + dt;
    }
    m.aimHeld = a.aim;
    m.nowMs = uint32_t(double(world.ticks()) * kTickSeconds * 1000.0);
    if (world.player().steepUnder()) m.lastSteepMs = std::max<uint32_t>(m.nowMs, 1);
    const float yaw = world.stickYaw();
    const glm::vec2 fwd(std::cos(yaw), std::sin(yaw)), right(std::sin(yaw), -std::cos(yaw));
    m.stickWorld = right * a.move.x + fwd * a.move.y;
    m.stickLen = std::min(1.0f, glm::length(a.move));
    if (!m.started) rt_->start(m), m.started = true;
    else rt_->tick(m, dt * 30.0f);

    const ChartRuntime& rt = *rt_;
    if (rt.state() != m.lastState || rt.stats.changes != m.lastChanges) m.stateTicks = 0;
    else ++m.stateTicks;
    m.lastState = rt.state(), m.lastChanges = rt.stats.changes;
    const PlayerState& s = world.player().state();
    const int r = rt.request();
    const std::string& chartClip = r >= 0 && size_t(r) < charts_->requests.size() ? charts_->requests[size_t(r)].clip : std::string();
    const bool match = sameClip(chartClip, s.clip);
    (match ? m.matched : m.diverged)++;
    if (match != m.lastMatch || m.ticks == 0) {
        if (!match) ++m.divergences;
        if (m.log)
            std::fprintf(stderr, "ou-game: chart tick %llu %s S%d (parent %d, req %d %s, T%d) | ours %s clip %s\n",
                         (unsigned long long)world.ticks(), match ? "matches" : "DIVERGES", rt.state(), rt.parent(), r, chartClip.c_str(),
                         rt.lastTransition(), std::string(stateName(s.state)).c_str(), s.clip.c_str());
    }
    m.lastMatch = match;
    if (m.trace) {
        char row[512];
        std::snprintf(row, sizeof row, "%llu\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%x\tt30=%.3f\tvar_anim=%.3f\n",
                      (unsigned long long)world.ticks(), rt.state(), rt.parent(), r, m.stateTicks, s.pos.x, s.pos.y, s.pos.z, s.vel.x,
                      s.vel.y, s.vel.z, glm::degrees(s.yaw), inputMask(a), rt.stateTime(), rt.animFrames());
        m.trace << row;
    }
    ++m.ticks;
}

std::string Shadow::summary() const {
    const Impl& m = *impl_;
    std::ostringstream o;
    o << "chart shadow: " << m.ticks << " ticks, clip matches " << m.matched << ", diverges " << m.diverged << " (" << m.divergences
      << " runs), " << rt_->stats.changes << " state changes, final S" << rt_->state() << " | unported conditions:";
    std::vector<std::pair<uint64_t, int16_t>> top;
    for (const auto& [id, n] : rt_->stats.unportedConds) top.emplace_back(n, id);
    std::ranges::sort(top, std::greater<>());
    char b[32];
    for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i) {
        std::snprintf(b, sizeof b, " 0x%x:%llu", unsigned(uint16_t(top[i].second)), (unsigned long long)top[i].first);
        o << b;
    }
    o << " (" << top.size() << " ids), unported actions " << rt_->stats.unportedActions.size();
    return o.str();
}

} // namespace ou::game::chart
