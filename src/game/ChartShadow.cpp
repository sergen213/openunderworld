// Shadow mode: Lara's chart beside the controller (Chart.hpp Shadow; docs/re/PLAYER_CHART.md "Shadow mode").
#include "game/Chart.hpp"
#include "game/Game.hpp"
#include "game/ObjData.hpp"

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
        // 0x5C3AA0 takes an index past the last segment as the last one (with a debug print)
        if (seg < 0 || p.size() < 2) return {};
        const size_t s = std::min(size_t(seg), p.size() - 2);
        return std::pair{p[s], p[s + 1]};
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
    std::optional<bool> ledgeShelf(int m, float param) const override {
        const auto& col = w->collision();
        if (m < 0 || size_t(m) >= col.markup.size() || col.markup[size_t(m)].points.size() < 2) return {};
        const auto& p = col.markup[size_t(m)].points;
        const int seg = std::clamp(int(param), 0, int(p.size()) - 2);
        const float t = (param - float(seg)) * glm::length(p[size_t(seg) + 1] - p[size_t(seg)]);
        return w->player().ledgeShelf(m, seg, t);
    }
    std::optional<int> hurdlePastClass(glm::vec3 q, glm::vec2 f) const override { return game::hurdlePastClass(w->collision(), q, f); }
    // ---- end group B ----

    // ---- group J ----
    // Ours runs no ground check in flight; tru.exe's 0x5DCE20 clears bit 19 every check and sets it when it finds no
    // floor (posting 0x1CC). [I] her free-flight states are its "no floor".
    std::optional<bool> noFloor() const override {
        const game::State st = w->player().state().state;
        return st == game::State::Jump || st == game::State::Fall || st == game::State::HangJump;
    }
    // Instance velocity +0x2A0 per 30 Hz frame [I: her displacement this 60 Hz tick x 2, root motion included].
    std::optional<glm::vec3> velocity() const override { return (w->player().state().pos - w->previous().pos) * 2.0f; }
    // Bone 0 z now minus previous frame's (+0x90 / +0x94, docs/re/CAMERA.md) [I: bone 0 moves with her root].
    std::optional<float> boneRise() const override { return w->player().state().pos.z - w->previous().pos.z; }
    std::optional<bool> combatFlag(int bit) const override {
        if (bit == 22) return w->combat().headShotReady; // 0x400000 (0x6E6630), Combat.hpp
        if (bit == 27) return w->combat().adrenalineMode; // 0x8000000 (focused fire)
        return {};
    }
    std::optional<bool> carrying() const override { return w->heldPortable() != 0; }
    std::optional<ChartContext::Grapple> grapple() const override {
        const PlayerState& s = w->player().state();
        const game::State st = s.state;
        const bool on = st == game::State::GrappleThrow || st == game::State::GrappleSwing || st == game::State::GrapplePull ||
                        st == game::State::GrappleRappel || s.grappleUid != 0;
        if (!on) return ChartContext::Grapple{};
        return ChartContext::Grapple{true, true, s.grappleAnchor, s.ropeLength, w->player().maxRope()};
    }
    std::optional<Water> water() const override {
        const PlayerState& s = w->player().state();
        const int m = w->player().waterAt(s.pos);
        if (m < 0) return Water{};
        return Water{true, waterSurface(w->collision().markup[size_t(m)], glm::vec2(s.pos), double(w->ticks()) * kTickSeconds) - s.pos.z};
    }
    std::array<uint32_t, size_t(JEffect::kCount)> effects{};
    void effect(JEffect e, float) override { ++effects[size_t(e)]; }
    // ---- end group J ----

    // ---- group CD ----
    // Player +0x8EC: 0x400 outside a water volume (ours Player::waterAt at her feet); inside one 0x5D3860's class from
    // her root (+0x20) and the floor under her (+0x24) against the wave surface, lara.drm +0xAC / +0xAE / +0xB0 / +0xB2.
    // [I] +0x20 / +0x24 = root z / floor z minus the surface; the in-volume test also takes the floor when player +0x8DC
    // (the volume) is set, which it is here. [U] the volume-extent bits 0x800..0x4000 (+0x2C against volume +0x28 /
    // +0xC8 and the bone matrices) are not computed: values 2 / 3 of 0x1F5 read them.
    std::optional<uint32_t> waterFlags() const override {
        const PlayerState& s = w->player().state();
        const int m = w->player().waterAt(s.pos);
        if (m < 0) return 0x400u;
        const float surface = waterSurface(w->collision().markup[size_t(m)], glm::vec2(s.pos), double(w->ticks()) * kTickSeconds);
        const float root = s.pos.z - surface;
        const auto g = w->collision().groundProbe(s.pos + glm::vec3(0, 0, 50), 50 + 4000);
        const float floor = g ? g->point.z - surface : -1e9f;
        const Charts& d = chart().charts();
        const float ac = d.data<int16_t>(0xAC), ae = d.data<int16_t>(0xAE), b0 = d.data<int16_t>(0xB0), b2 = d.data<int16_t>(0xB2);
        if (!(root <= 0 || floor <= 0)) return 0x400u;
        if (!(floor <= -ac)) return 0x200u;
        if (!(floor <= -ae)) return 0x100u;
        if (!(floor <= -b0)) return 0x80u;
        if (-b2 < floor || -b0 < root) return 0x40u;
        return root <= -b2 ? 0x10u : 0x20u;
    }
    // (player +0x500) +0x134: the drawn weapon's slot (weapon data +0xDC: 0 pistols, 2 Thor's hammer, 1 the rest), -1
    // with empty hands; carrying a portable the slot is not known [U: 3?].
    std::optional<int> gearSlot() const override {
        const CombatState& c = w->combat();
        if (c.drawn) return c.weapon == Weapon::Pistols ? 0 : c.weapon == Weapon::ThorHammer ? 2 : 1;
        if (w->heldPortable() != 0) return std::nullopt;
        return -1;
    }
    bool tracksCriticalSection() const override { return true; }
    std::optional<RayHit> rayHit(glm::vec3 a, glm::vec3 b, uint32_t) const override {
        const float d = glm::length(b - a);
        if (d <= 0) return {};
        const auto h = w->collision().playerRay(a, (b - a) / d, d); // [I] one ray mask for every tru.exe flag set
        if (!h) return {};
        RayHit r;
        r.point = h->point, r.normal = h->normal;
        if (h->dynamic >= 0) r.kind = 2; // stillBody / flag8000000 unknown for ours
        return r;
    }
    // The chart clip's command events (lara.drm anim list) and how far its clock had run.
    std::map<std::string, ClipEvents, std::less<>> clipCommands;
    uint64_t clipChanges = ~0ull;
    int clipRequest = -1;
    float clipFrame = 0;
    // ---- end group CD ----
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
    if (const auto o = objData(bigfile, "lara")) impl_->clipCommands = clipEvents(*o); // group CD: critical sections
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
    // ---- group CD ---- 0x768C50 after the chart tick: agent +0x6C = player +0x78. Then the chart clip's events over the
    // frames its clock ran this tick (0x716F30): command 0x13 opens the critical section (+0x78 = 1, agent +0x53C = 1),
    // 0x14 closes it. [I] the anim update runs after the chart tick (the order is not pinned, docs Runtime "Open").
    rt_->field(0x6C) = m.playerInt(0x78);
    if (const Request* p = rt_->pattern()) {
        const float cur = rt_->animFrames(), len = rt_->animLengthFrames();
        const bool fresh = rt_->stats.changes != m.clipChanges || rt_->request() != m.clipRequest;
        const float prev = fresh ? float(std::max(0, int(p->start) - int(p->blend))) - 1e-4f : m.clipFrame;
        const auto ev = m.clipCommands.find(p->clip);
        if (ev != m.clipCommands.end() && cur > prev) {
            auto apply = [&](float lo, float hi) { // events with lo < frame <= hi, in stored order
                for (const auto& e : ev->second.commands)
                    if (float(e[2]) > lo && float(e[2]) <= hi && (e[0] == 0x13 || e[0] == 0x14)) {
                        m.playerInt(0x78) = e[0] == 0x13;
                        rt_->deferFlag7 = e[0] == 0x13;
                    }
            };
            if (rt_->animLoops() && len > 0) {
                for (float k = std::floor(prev / len); k <= std::floor(cur / len); k += 1)
                    apply(std::max(prev - k * len, -1.0f), std::min(cur - k * len, len));
            } else {
                apply(prev, cur);
            }
        }
        m.clipFrame = cur;
    }
    m.clipChanges = rt_->stats.changes, m.clipRequest = rt_->request();
    // ---- end group CD ----

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
