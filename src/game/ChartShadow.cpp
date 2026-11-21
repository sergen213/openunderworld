// Shadow mode: Lara's chart beside the controller (Chart.hpp Shadow; docs/re/PLAYER_CHART.md "Shadow mode").
#include "game/Chart.hpp"
#include "cdc/Cdc.hpp"
#include "game/Game.hpp"
#include "game/ObjData.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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
    // ---- group B ---- the controller clip bridging kind-2 requests' animation end (Shadow::tick)
    std::string ctlClip;
    bool ctlLoop = true;
    uint64_t ctlClipSince = 0, kind2Since = 0;
    int kind2State = -1;

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
    // Agent +0x90: game ms of the last too-steep ground contact (message 0x1C9 from the ground check 0x5DCE20; ours the
    // shadow's own ground check, `ground.steep`).
    std::optional<bool> wallSlideRecent() const override { return lastSteepMs > 0 && nowMs - lastSteepMs <= 99; }
    bool aimHeld = false;
    uint32_t nowMs = 0, lastSteepMs = 0;
    // Player +0xF3C from the support-foot events of the clip Lara's anim layer plays (0x716F30 handles that layer's
    // events): the chart's clip on the runtime's clip clock (0x783470) [O], for a kind-2 request the pattern its anim
    // graph node plays (ChartRuntime::resolveRequest; stick_jump_open: S39 ng_1foot_idle sets foot 1, S40's node plays its
    // idle, whose 0x36 / 0x35 leave foot 0, and S246 falls to T295 under Wine [O-runtime]). Only while a request resolves
    // to no pattern does ours take our controller's clip and time [I]. Fired as the clock passes them (the anim update's
    // order against the chart tick [U]: ours after it).
    std::unordered_map<std::string, FootClip> footClips;
    uint64_t footChanges = ~0ull;
    std::string footClip;
    float footFrame = 0;
    int foot = 0;
    std::optional<int> supportFoot() const override { return footClips.empty() ? std::nullopt : std::optional<int>(foot); }
    void updateFoot(const ChartRuntime& rt, const PlayerState& s) {
        const Request* p = rt.pattern();
        const float len = rt.animLengthFrames();
        const bool pattern = p && len > 0;
        const std::string name = lower(pattern ? std::string_view(p->clip) : std::string_view(s.clip));
        const auto it = footClips.find(name);
        if (it == footClips.end()) return;
        const FootClip& fc = it->second;
        const float total = pattern ? len : fc.frames;
        float f = pattern ? rt.animFrames() : float(s.clipTime * 30.0);
        if ((pattern ? rt.animLoops() : s.loop) && total > 0) f = std::fmod(f, total);
        const bool mirror = pattern ? rt.mirrored() : s.mirror;
        const auto fire = [&](float lo, bool loInclusive, float hi) {
            for (const auto& [at, cmd] : fc.events)
                if ((loInclusive ? at >= lo : at > lo) && at <= hi)
                    foot = cmd == 0x35 ? 0 : ((cmd == 0x34) != mirror ? 1 : 2); // 0x716F30 with 0x6EDD00
        };
        // A chart clip restarts when the runtime starts it (clipStarts); the controller's clip only when it changes.
        if ((pattern && rt.clipStarts() != footChanges) || name != footClip) {
            fire(pattern ? rt.clipStartFrame() : 0.0f, true, f);
        } else if (f >= footFrame) {
            fire(footFrame, false, f);
        } else {
            fire(footFrame, false, total), fire(0, true, f); // looped
        }
        footChanges = pattern ? rt.clipStarts() : ~0ull, footClip = name, footFrame = f;
    }
    // The upper-body chart (agent 1, player +0xF14), ticked after Lara's as 0x768C50 does: conditions 0x401 / 0x404 /
    // 0x408 read its current state's bool property (0x6EEF00 mode 1, false when absent; ours the value in force [I]).
    const ChartRuntime* upper = nullptr;
    std::optional<bool> upperProp(uint16_t id) const override {
        if (!upper) return std::nullopt;
        return upper->propBool(id).value_or(false);
    }
    // ---- end group A ----
    // Player +0x6DC - z. +0x6DC is the chart's own store (playerFloat): actions 515 Entry (0x72FBF0, Ground.cpp) /
    // 508 Exit (0x75C550) write her root z, and the player mode's message 0x2A (0x71FFE0 -> 0x700060) on placement,
    // which the shadow does at its start.
    float fallDistance() const override { return const_cast<Impl*>(this)->playerFloat(0x6DC) - w->player().state().pos.z; }
    // The ground check (tru.exe 0x71B910 -> 0x5DCE20), run here on her root each tick, independent of the controller's
    // state. The ray starts half her height (player data +0x10 x 0.5) over her feet [O: 0x71B910's first span, at least
    // that]; a face counts as floor only within the second span under her feet: half her height while she stands on an
    // instance (support handle +0x610, set when the last check's hit was an instance, result 2) and 0 on the terrain
    // [O: 0x71B910 / 0x5DCE20]. Ours: an instance = a dynamic body; 1 unit instead of 0 for float error; the +0x11CC /
    // +0x11D8 overrides and move mode 0x14's quarter height are not modelled [I]. So a walk off a step lower than her
    // feet finds no floor at once, as tru.exe's does.
    game::Player::GroundCheck ground;
    bool supportBody = false;
    // 0x71B910 runs the check only while collision mask 0xFADA74 has bit 8 (0x768B60 starts it at 0x48; the flight's
    // actions 503 / 510 set 0x49 / 0x81 / 0xC9 and clear 0x48 while she rises, Jump.cpp's CollisionMask effects); a
    // skipped check drops her support (0x5D76B0: +0x610 = 0), posts nothing and finds no floor.
    uint32_t collisionMask = 0x48;
    void groundCheck(const Charts& c) {
        if (!(collisionMask & 8)) {
            ground = {}, supportBody = false;
            return;
        }
        const float half = c.data<float>(0x10) * 0.5f;
        const glm::vec3 feet = w->player().state().pos;
        ground = w->player().groundCheck(feet, half, supportBody ? half : 1.0f);
        supportBody = ground.floor && ground.onBody;
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
    // The game's attached markup (Player::attachedMarkup) as a slot.
    MarkupSlot gameAttached() const {
        const PlayerState& s = w->player().state();
        const int m = w->player().attachedMarkup();
        if (m < 0 || s.state == game::State::HangJump) return {};
        if (s.state == game::State::Climb) return slot(m, 0, 0);
        return slot(m, s.ledgeSeg, s.ledgeT);
    }
    // +0xC94 is the chart's own (chartAttached, attached by its actions): the controller attaches on its own frame
    // (a caught ledge is hung from the tick the flight meets it), tru.exe only in the attach state's action 230. Its
    // geometry (param, point) follows the controller while both hold the same markup.
    MarkupSlot attachedMarkup() const override {
        if (!chartAttached) return gameAttached();
        const MarkupSlot& c = *chartAttached;
        if (!c) return {};
        const MarkupSlot g = gameAttached();
        return g.markup == c.markup ? g : c;
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
    // 0x5DCE20 clears bit 19 every check and sets it when it finds no floor (posting 0x1CC): the shadow's ground check.
    std::optional<bool> noFloor() const override { return !ground.floor; }
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
        return !ground.floor && w->player().wallJumpAnticipate(frames);
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
    // 0x7594C0 (Player::wallJumpProbe), asked while the ground check finds no floor (ours keeps no launch velocity on
    // the ground) [I].
    std::optional<int> wallJumpProbe() const override { return !ground.floor ? w->player().wallJumpProbe() : 0; }
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
    void effect(JEffect e, float v) override {
        ++effects[size_t(e)];
        if (e == JEffect::CollisionMask) { // +-0x48: or / clear (0x752410); anything else: the new mask
            const int x = int(v);
            collisionMask = x == 0x48 ? collisionMask | 0x48u : x == -0x48 ? collisionMask & ~0x48u : uint32_t(x);
        }
    }
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
    float clipFrame = 0;
    // --chart-state N@T: the chart enters state N at world tick T (Shadow::enterAt); -1 none.
    int enterState = -1;
    uint64_t enterTick = 0;
    // ---- end group CD ----
};

Shadow::Shadow(const cdc::BigfileSet& bigfile, std::optional<std::filesystem::path> trace, bool log) : impl_(std::make_unique<Impl>()) {
    auto c = loadCharts(bigfile);
    if (!c) throw std::runtime_error("chart shadow: lara.drm player data not found");
    charts_ = std::make_unique<Charts>(std::move(*c));
    rt_ = std::make_unique<ChartRuntime>(*charts_, charts_->player, Registry::player());
    upper_ = std::make_unique<ChartRuntime>(*charts_, charts_->upper, Registry::upper());
    impl_->upper = upper_.get();
    impl_->log = log;
    impl_->chartAttached = ChartContext::MarkupSlot{}; // +0xC94 is the chart's own (Impl::attachedMarkup)
    impl_->footClips = footEvents(bigfile);
    if (trace) {
        impl_->trace.open(*trace);
        if (!impl_->trace) throw std::runtime_error("chart shadow: cannot write " + trace->string());
        impl_->trace << "tick\tstate\tparent\trequest\tstateTicks\tpx\tpy\tpz\tvx\tvy\tvz\tyaw\tinputs\n";
    }
    if (const auto o = objData(bigfile, "lara")) impl_->clipCommands = clipEvents(*o); // group CD: critical sections
}
void Shadow::enterAt(int state, uint64_t tick) { impl_->enterState = state, impl_->enterTick = tick; }
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
    m.groundCheck(*charts_);
    if (m.ground.steep) m.lastSteepMs = std::max<uint32_t>(m.nowMs, 1);
    const float yaw = world.stickYaw();
    const glm::vec2 fwd(std::cos(yaw), std::sin(yaw)), right(std::sin(yaw), -std::cos(yaw));
    m.stickWorld = right * a.move.x + fwd * a.move.y;
    m.stickLen = std::min(1.0f, glm::length(a.move));
    if (!m.started) {
        m.playerFloat(0x6DC) = world.player().state().pos.z; // placement: message 0x2A -> 0x700060
        rt_->start(m), upper_->start(m), m.started = true;
    } else {
        rt_->tick(m, dt * 30.0f);   // 0x768C50: agent 0 (Lara) ...
        upper_->tick(m, dt * 30.0f); // ... then agent 1 (upper body)
    }
    // Wine corpus cases that begin in the previous case's state (case.json start.state): put the chart there at the
    // given world tick, as a change would (ChartRuntime::enter), without a transition.
    if (m.enterState >= 0 && world.ticks() >= m.enterTick) rt_->enter(m, m.enterState), m.enterState = -1;
    // ---- group B ---- A kind-2 request (an anim-graph node, 0x443720 -> 0x43E250) has no clip on the chart's clock
    // (docs Runtime "Kind-2 requests"). [I] Its end (0x768CF0 from the anim controller) is taken from our controller: the
    // one-shot clips it plays from the chart's state on ending in a loop (the controller settles: ledgeb_attach_M ->
    // ledgeb_idle). Run_jump S533 (request 22, the hang attach): ledgeb_attach_M runs 31 ticks, Wine's S533 31. The
    // standing jump's S535 (same node, flight + attach) runs 38 in Wine, ours 47: the graph node is not the
    // controller's clips (needs the anim-graph VM).
    {
        const PlayerState& ps = world.player().state();
        const int st = rt_->state();
        if (st != m.kind2State) m.kind2State = st, m.kind2Since = world.ticks();
        if (ps.clip != m.ctlClip) {
            const State* cs = rt_->data().state(st);
            if (m.started && cs && cs->requestKind == 2 && !m.ctlClip.empty() && !m.ctlLoop && ps.loop && m.ctlClipSince + 1 >= m.kind2Since)
                rt_->animEnded(m);
            m.ctlClip = ps.clip, m.ctlLoop = ps.loop, m.ctlClipSince = world.ticks();
        }
    }
    // ---- group CD ---- 0x768C50 after the chart tick: agent +0x6C = player +0x78. Then the chart clip's events over the
    // frames its clock ran this tick (0x716F30): command 0x13 opens the critical section (+0x78 = 1, agent +0x53C = 1),
    // 0x14 closes it. [I] the anim update runs after the chart tick (the order is not pinned, docs Runtime "Open").
    rt_->field(0x6C) = m.playerInt(0x78);
    if (const Request* p = rt_->pattern()) {
        const float cur = rt_->animFrames(), len = rt_->animLengthFrames();
        const bool fresh = rt_->clipStarts() != m.clipChanges;
        const float prev = fresh ? rt_->clipStartFrame() - 1e-4f : m.clipFrame;
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
    m.clipChanges = rt_->clipStarts();
    // ---- end group CD ----
    m.updateFoot(*rt_, world.player().state());
    // ---- group J ----
    // Message 0x1C7 = chart event 455, posted by every ground check that finds a floor (0x5DCE20 with flag 2; the
    // player's message handler 0x76FC30 zeroes agent +0x8C and passes it to the chart, 0x787CD0). Only flight / landing
    // states handle it (S288, S1004, S1292, ...), so on the ground it does nothing. [I] the player call's flag 2 and the
    // check's place in the frame against the chart tick (ours after it).
    if (m.ground.floor) {
        rt_->field(0x8C) = 0;
        rt_->post(m, 455);
    }
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
        if (m.log) // tick = the frame's index (the input script's tick, tru.exe's frame counter during the frame)
            std::fprintf(stderr, "ou-game: chart tick %llu %s S%d (parent %d, req %d %s, T%d) | ours %s clip %s\n",
                         (unsigned long long)(world.ticks() - 1), match ? "matches" : "DIVERGES", rt.state(), rt.parent(), r, chartClip.c_str(),
                         rt.lastTransition(), std::string(stateName(s.state)).c_str(), s.clip.c_str());
    }
    m.lastMatch = match;
    if (m.trace) {
        char row[512];
        std::snprintf(row, sizeof row, "%llu\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%x\tt30=%.3f\tvar_anim=%.3f\n",
                      (unsigned long long)(world.ticks() - 1), rt.state(), rt.parent(), r, m.stateTicks, s.pos.x, s.pos.y, s.pos.z, s.vel.x,
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
