// Shadow mode: Lara's chart beside the controller (Chart.hpp Shadow; docs/re/PLAYER_CHART.md "Shadow mode").
#include "game/Chart.hpp"
#include "cdc/Cdc.hpp"
#include "game/Game.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace ou::game::chart {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& ch : o) ch = char(std::tolower(uint8_t(ch)));
    return o;
}

// The support-foot events (commands 0x34 / 0x35 / 0x36, handler 0x716F30) of every clip of lara.drm's animation list
// (object DTP +0x54 count, +0x58 -> 12-byte entries {s16 anim, u32 event DTP id, -> name}); an event DTP is a run of
// records {u16 info, u16 frame, ...} of 8 + (info >> 12) x 4 bytes, info & 3 == 2 a command with its id at +8,
// ended by info 0xFFFF [O, openunderworld-work/gameplaygoal/melee/ev.cpp]. Frames are 30 Hz keys.
struct FootClip {
    float frames = 0; // clip length in 30 Hz frames
    std::vector<std::pair<float, int>> events; // (frame, command), by frame
};
std::unordered_map<std::string, FootClip> footEvents(const cdc::BigfileSet& bf) {
    std::unordered_map<std::string, FootClip> out;
    auto bytes = bf.read("pc-w\\lara.drm");
    if (!bytes) return out;
    const auto d = cdc::parseDrm(std::move(*bytes));
    const auto rd16 = [](std::span<const uint8_t> v, size_t o) { uint16_t x = 0; if (o + 2 <= v.size()) std::memcpy(&x, v.data() + o, 2); return x; };
    const auto rd32 = [](std::span<const uint8_t> v, size_t o) { uint32_t x = 0; if (o + 4 <= v.size()) std::memcpy(&x, v.data() + o, 4); return x; };
    uint32_t base = 0;
    for (const auto& s : d.sections)
        if (s.type == cdc::SectionType::Object) { base = rd32(s.data, 0); break; }
    for (uint32_t i = 0; i < d.sections.size(); ++i) {
        const auto& s = d.sections[i];
        if (s.type != cdc::SectionType::DtpData || s.id != base) continue;
        const auto ptrs = cdc::sectionPointers(d, i);
        const auto list = ptrs.find(0x58);
        if (list == ptrs.end()) break;
        const auto& ls = d.sections[list->second.section];
        const auto lptrs = cdc::sectionPointers(d, list->second.section);
        const uint32_t n = rd32(s.data, 0x54);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t e = list->second.offset + 12 * k;
            const auto name = lptrs.find(e + 8);
            if (name == lptrs.end()) continue;
            std::string nm;
            const auto& nd = d.sections[name->second.section].data;
            for (size_t c = name->second.offset; c < nd.size() && nd[c]; ++c) nm += char(nd[c]);
            const int16_t animId = int16_t(rd16(ls.data, e));
            const uint32_t evId = rd32(ls.data, e + 4);
            FootClip fc;
            for (const auto& x : d.sections)
                if (x.type == cdc::SectionType::Animation && x.id == uint32_t(animId)) {
                    fc.frames = float(anim::decodeClip(x.data).durationSeconds() * 30.0);
                    break;
                }
            for (const auto& x : d.sections) {
                if (x.type != cdc::SectionType::DtpData || x.id != evId) continue;
                for (size_t at = 4; at + 8 <= x.data.size();) {
                    const uint16_t info = rd16(x.data, at);
                    if (info == 0xFFFF) break;
                    if ((info & 3) == 2)
                        if (const int cmd = int16_t(rd16(x.data, at + 8)); cmd >= 0x34 && cmd <= 0x36) fc.events.emplace_back(float(rd16(x.data, at + 2)), cmd);
                    at += 8 + size_t(info >> 12) * 4;
                }
                break;
            }
            std::ranges::stable_sort(fc.events, {}, &std::pair<float, int>::first);
            out.emplace(lower(nm), std::move(fc));
        }
        break;
    }
    return out;
}


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
        // 136 (context 8) and 115 (context 9) share control 0 with 52, 135 (context 12) and 72 (context 19) control 1
        // with 50 (am.txt A0 / A1 / A86 / A126) [O]; contexts not modelled [I].
        case 52: case 47: case 136: case 115: return Jump;
        case 50: case 51: case 101: case 135: case 72: return Crouch;
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
    // Player +0xF3C from the support-foot events of the clip our controller plays (fired as its time passes them).
    std::unordered_map<std::string, FootClip> footClips;
    std::string footClip;
    float footFrame = 0;
    int foot = 0;
    std::optional<int> supportFoot() const override { return footClips.empty() ? std::nullopt : std::optional<int>(foot); }
    void updateFoot(const PlayerState& s) {
        const auto it = footClips.find(lower(s.clip));
        if (it == footClips.end()) return;
        const FootClip& fc = it->second;
        float f = float(s.clipTime * 30.0);
        if (s.loop && fc.frames > 0) f = std::fmod(f, fc.frames);
        const auto fire = [&](float lo, bool loInclusive, float hi) {
            for (const auto& [at, cmd] : fc.events)
                if ((loInclusive ? at >= lo : at > lo) && at <= hi)
                    foot = cmd == 0x35 ? 0 : ((cmd == 0x34) != s.mirror ? 1 : 2); // 0x716F30 with 0x6EDD00
        };
        const std::string key = it->first;
        if (key != footClip) fire(0, true, f);              // a new clip: its events up to now
        else if (f >= footFrame) fire(footFrame, false, f); // passed since the last tick
        else fire(footFrame, false, fc.frames), fire(0, true, f); // looped
        footClip = key, footFrame = f;
    }
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
    std::optional<bool> wallJumpAnticipate(float frames) const override {
        return w->player().state().state == game::State::Jump && w->player().wallJumpAnticipate(frames);
    }
    // 0x710380 over ours' markup: ledges and hurdles (MUD 0x40010000) whose nearest point to P = her root + forward x
    // player data float +0x61C lies (vertically) within (short +0xC44, short +0xC46) of her z, nearest two by horizontal
    // distance under short +0xC48 [O: decomp; the nearest point per polyline is ours, 0x709500's second point not ported].
    std::optional<Candidates> jumpCandidates() const override {
        const Charts& d = chart().charts();
        const PlayerState& s = w->player().state();
        const glm::vec2 fwd(std::cos(s.yaw), std::sin(s.yaw));
        const glm::vec3 p = s.pos + glm::vec3(fwd * d.data<float>(0x61C), 0);
        const float lo = float(d.data<int16_t>(0xC44)), hi = float(d.data<int16_t>(0xC46));
        float best[2] = {float(d.data<int16_t>(0xC48)), float(d.data<int16_t>(0xC48))};
        Candidates c;
        const auto& col = w->collision();
        for (size_t m = 0; m < col.markup.size(); ++m) {
            const auto& mk = col.markup[m];
            if (!(collision::markupFlags(mk.type) & 0x40010000u) || mk.points.size() < 2) continue;
            float bd = 1e30f, bt = 0;
            int bs = 0;
            glm::vec3 bp{0};
            for (size_t i = 0; i + 1 < mk.points.size(); ++i) {
                const glm::vec3 a = mk.points[i], b = mk.points[i + 1];
                const glm::vec2 ab(b - a);
                const float l2 = glm::dot(ab, ab);
                const float t = l2 > 0 ? std::clamp(glm::dot(glm::vec2(p - a), ab) / l2, 0.0f, 1.0f) : 0.0f;
                const glm::vec3 q = a + (b - a) * t;
                const float dd = glm::length(glm::vec2(q - p));
                if (dd < bd) bd = dd, bt = t * std::sqrt(l2), bs = int(i), bp = q;
            }
            const float dz = std::abs(s.pos.z - bp.z);
            if (!(dz > lo && dz < hi)) continue;
            MarkupSlot slot = this->slot(int(m), bs, bt);
            const glm::vec3 seg = mk.points[size_t(bs) + 1] - mk.points[size_t(bs)];
            const glm::vec2 n = glm::length(glm::vec2(seg)) > 0 ? glm::normalize(glm::vec2(-seg.y, seg.x)) : glm::vec2(0);
            slot.normal = glm::vec3(n, 0);
            if (bd < best[0]) best[1] = best[0], c.b = c.a, best[0] = bd, c.a = slot;
            else if (bd < best[1]) best[1] = bd, c.b = slot;
        }
        return c;
    }
    std::optional<Aim> takeoffAim(int kind) const override {
        const auto a = w->player().groundJumpAim(kind);
        if (a.target.markup < 0) return Aim{};
        return Aim{slot(a.target.markup, a.target.seg, a.target.t), a.flags, a.cls, a.target.frames};
    }
    // 0x7594C0 (Player::wallJumpProbe), asked in her free flights only (ours keeps no launch velocity elsewhere) [I].
    std::optional<int> wallJumpProbe() const override {
        return w->player().state().state == game::State::Jump ? w->player().wallJumpProbe() : 0;
    }
    // The aimed jump's +0xAC0 / +0xA80 (PlayerState::aimFlags / aimClass, set by Player::aimGroundJump) while a jump
    // target is held.
    std::optional<uint32_t> jumpQueryFlags() const override { return jumpTarget() ? w->player().state().aimFlags : 0u; }
    std::optional<int> jumpTargetType() const override { return jumpTarget() ? int(w->player().state().aimClass) : 0; }
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
    bool wasAir = false; // the last tick's noFloor, for the landing event
    void effect(JEffect e, float) override { ++effects[size_t(e)]; }
    // ---- end group J ----
};

Shadow::Shadow(const cdc::BigfileSet& bigfile, std::optional<std::filesystem::path> trace, bool log) : impl_(std::make_unique<Impl>()) {
    auto c = loadCharts(bigfile);
    if (!c) throw std::runtime_error("chart shadow: lara.drm player data not found");
    charts_ = std::make_unique<Charts>(std::move(*c));
    rt_ = std::make_unique<ChartRuntime>(*charts_, charts_->player, Registry::player());
    impl_->log = log;
    impl_->footClips = footEvents(bigfile);
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
    m.updateFoot(world.player().state());
    const float yaw = world.stickYaw();
    const glm::vec2 fwd(std::cos(yaw), std::sin(yaw)), right(std::sin(yaw), -std::cos(yaw));
    m.stickWorld = right * a.move.x + fwd * a.move.y;
    m.stickLen = std::min(1.0f, glm::length(a.move));
    if (!m.started) rt_->start(m), m.started = true;
    else rt_->tick(m, dt * 30.0f);
    // ---- group J ----
    // Chart event 455 (0x1C7): her landing, posted when the ground check finds a floor under her again after a flight
    // (the handlers in S288 / S1004 / S1292: T16 -> S1275 the landing group) [I: the message that posts it is not
    // decoded; ours posts it on the tick the controller leaves its free-flight states].
    const bool air = m.noFloor().value_or(false);
    if (m.wasAir && !air) rt_->post(m, 455);
    m.wasAir = air;
    // ---- end group J ----

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
