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
    enum Button { Jump, Crouch, Sprint, Interact, Grapple, ManualAim, Grenade, kButtons };
    std::array<ButtonTime, kButtons> buttons;
    const World* w = nullptr;
    glm::vec2 stickWorld{0};
    float stickLen = 0;
    std::ofstream trace;
    bool log = false, started = false, lastMatch = true;
    uint64_t ticks = 0, matched = 0, diverged = 0, divergences = 0, lastChanges = 0;
    int lastState = -1, stateTicks = 0;

    Action action(int id) const override {
        int b = -1;
        switch (id) { // generalbank action map (docs/GAMEPLAY.md "Button timing", "Rope inputs", Melee.cpp)
        case 52: case 47: b = Jump; break;
        case 50: case 51: case 101: b = Crouch; break;
        case 42: b = Sprint; break;
        case 99: case 32: case 44: case 48: case 75: b = Interact; break;
        case 46: b = Grapple; break;
        case 25: case 37: b = ManualAim; break;
        case 102: b = Grenade; break;
        default: return {};
        }
        const ButtonTime& t = buttons[size_t(b)];
        return {true, t.down, t.edge, t.time};
    }
    float stickLevel() const override { return stickLen; }
    glm::vec2 stick() const override { return stickWorld; }
    float fallDistance() const override {
        const PlayerState& s = w->player().state();
        return s.fallStartZ - s.pos.z;
    }
    const PlayerState* player() const override { return &w->player().state(); }
    const World* world() const override { return w; }

    // ---- group B ----
    // The live controller's markup state as tru.exe's player slots: the attached markup (+0xC94) while she hangs /
    // shimmies / climbs up / is on a pole, beam, ladder, perch, pillar or hold (Player::attachedMarkup), the jump target
    // (+0xA64) of an aimed ground jump (PlayerState::aimMarkup) or of a hang jump in flight (its ledge).
    MarkupSlot slot(int m, int seg, float t) const {
        const auto& col = w->collision();
        if (m < 0 || size_t(m) >= col.markup.size()) return {};
        const auto& mk = col.markup[size_t(m)];
        MarkupSlot r;
        r.markup = m;
        r.flags = collision::markupFlags(mk.type);
        r.live = !mk.velocity.empty();
        if (mk.points.empty()) return r;
        const int n = int(mk.points.size());
        const int s = std::clamp(seg, 0, std::max(0, n - 2));
        const glm::vec3 a = mk.points[size_t(s)], b = mk.points[size_t(std::min(s + 1, n - 1))];
        const float len = glm::length(b - a);
        const float f = len > 0 ? std::clamp(t / len, 0.0f, 1.0f) : 0.0f;
        r.param = float(s) + f;
        r.point = a + (b - a) * f;
        const float yaw = w->player().state().yaw;
        r.normal = -glm::vec3(std::cos(yaw), std::sin(yaw), 0); // [I] outward: she faces the markup
        return r;
    }
    MarkupSlot attachedMarkup() const override {
        const PlayerState& s = w->player().state();
        const int m = w->player().attachedMarkup();
        if (m < 0 || s.state == game::State::HangJump) return {};
        if (s.state == game::State::Climb) return slot(m, 0, 0);
        return slot(m, s.ledgeSeg, s.ledgeT);
    }
    MarkupSlot jumpTarget() const override {
        const PlayerState& s = w->player().state();
        if (s.aimMarkup >= 0) return slot(s.aimMarkup, s.aimSeg, s.aimT);
        if (s.state == game::State::HangJump) return slot(s.ledge, s.ledgeSeg, s.ledgeT);
        return {};
    }
    std::optional<std::pair<glm::vec3, glm::vec3>> markupSegment(int m, int seg) const override {
        const auto& col = w->collision();
        if (m < 0 || size_t(m) >= col.markup.size()) return {};
        const auto& p = col.markup[size_t(m)].points;
        if (seg < 0 || size_t(seg) + 1 >= p.size()) return {};
        return std::pair{p[size_t(seg)], p[size_t(seg) + 1]};
    }
    int markupSegmentCount(int m) const override {
        const auto& col = w->collision();
        if (m < 0 || size_t(m) >= col.markup.size()) return 0;
        return std::max(0, int(col.markup[size_t(m)].points.size()) - 1);
    }
    std::optional<float> playerRay(glm::vec3 a, glm::vec3 b) const override {
        const float d = glm::length(b - a);
        if (d <= 0) return {};
        if (const auto h = w->collision().playerRay(a, (b - a) / d, d)) return h->t;
        return {};
    }
    bool hasCollision() const override { return w != nullptr; }
    glm::vec3 position() const override { return w->player().state().pos; }
    float heading() const override { return w->player().state().yaw; }
    float controlYaw() const override { return w->stickYaw(); }
    uint32_t gameMs() const override { return uint32_t(double(w->ticks()) * kTickSeconds * 1000.0); }
    int ledgeClearanceClass() const override {
        const PlayerState& s = w->player().state();
        if (s.ledge < 0 || size_t(s.ledge) >= w->collision().markup.size() || w->collision().markup[size_t(s.ledge)].type != 1) return 0;
        return w->player().hangClearanceClass();
    }
    // ---- end group B ----
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
    const bool down[Impl::kButtons] = {a.jump, a.drop, a.sprint, a.interact, a.grapple, a.manualAim, a.grenade};
    for (size_t i = 0; i < Impl::kButtons; ++i) m.buttons[i].update(down[i], dt, 0.0f);
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
