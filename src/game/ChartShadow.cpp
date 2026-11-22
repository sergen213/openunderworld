// Shadow mode: Lara's chart beside the controller (Chart.hpp Shadow; docs/re/PLAYER_CHART.md "Shadow mode").
#include "game/Chart.hpp"
#include "cdc/Cdc.hpp"
#include "game/Game.hpp"
#include "game/ObjData.hpp"
#include "anim/Anim.hpp"

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
struct Shadow::Impl : ChartContext, PlayerDriver {
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
    // The walk modifier (input 0x220 / the device's walk button, which tru.exe folds into the stick magnitude it hands
    // the chart, 0xFADFB0 -> +0xB0 -> +0x18 [U: the fold]) holds a full stick in the walk zone: the middle of the
    // state's 0x20A..0x20B band (SCCTest_MoveSpeed's walk zone) [I].
    float stickLevel() const override {
        if (!walkHeld || !rt) return stickLen;
        const auto t0 = rt->propFloat(0x20A), t1 = rt->propFloat(0x20B);
        return t0 && t1 ? std::min(stickLen, 0.5f * (*t0 + *t1)) : stickLen;
    }
    bool walkHeld = false;
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
        // t is Player's ledgeT: a distance in the ground plane (Player.cpp segLen), so the fraction is over that length
        // (a 3D length here shrank the grip every tick the drive fed +0xC98 back, d.t = param x the plane length)
        const float len = glm::length(glm::vec2(b - a));
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
        if (driving) return chartAttached ? *chartAttached : MarkupSlot{};
        if (!chartAttached) return gameAttached();
        const MarkupSlot& c = *chartAttached;
        if (!c) return {};
        const MarkupSlot g = gameAttached();
        return g.markup == c.markup ? g : c;
    }
    MarkupSlot jumpTarget() const override {
        if (driving) return {}; // +0xA64 is the chart's own (chartTarget, chartJumpTarget)
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
        const float t = (param - float(seg)) * glm::length(glm::vec2(p[size_t(seg) + 1] - p[size_t(seg)]));
        return w->player().ledgeShelf(m, seg, t);
    }
    std::optional<int> hurdlePastClass(glm::vec3 q, glm::vec2 f) const override { return game::hurdlePastClass(w->collision(), q, f); }
    // ---- end group B ----

    // ---- group J ----
    // 0x5DCE20 clears bit 19 every check and sets it when it finds no floor (posting 0x1CC): the shadow's ground check.
    std::optional<bool> noFloor() const override { return !ground.floor; }
    // Instance velocity +0x2A0 per 30 Hz frame [I: her displacement this 60 Hz tick x 2, root motion included].
    std::optional<glm::vec3> velocity() const override {
        if (driving) return w->player().state().vel / 30.0f; // instance +0x2A0 per 30 Hz frame: the drive's own velocity
        return (w->player().state().pos - w->previous().pos) * 2.0f;
    }
    // Bone 0 z now minus previous frame's (+0x90 / +0x94, docs/re/CAMERA.md) [I: bone 0 moves with her root].
    std::optional<float> boneRise() const override {
        if (driving) return lastRise;
        return w->player().state().pos.z - w->previous().pos.z;
    }
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
    void stateEntered(int from, int to) override;
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
    std::string clipName; // the clip whose events ran last tick, its length (30 Hz frames), loop flag and rate
    float clipLen = 0, clipRate = 1;
    bool clipLoops = false;
    // --chart-state N@T: the chart enters state N at world tick T (Shadow::enterAt); -1 none.
    int enterState = -1;
    uint64_t enterTick = 0;
    // ---- end group CD ----

    // ---- drive (--traversal chart; docs/re/PLAYER_CHART.md "Drive mode") ----
    // The chart decides; its effects are applied to Lara (Player::drive). The reads above that came from our controller
    // (the input-sources table) come from the drive's own state instead: velocity = the integrated instance velocity
    // (PlayerState::vel), bone rise = the last move's, attach = the chart's own +0xC94 only, jump target = the chart's own
    // +0xA64 only (the controller's aim is gone).
    bool driving = false;
    Player* pm = nullptr;
    uint32_t placements = ~0u;
    bool parked = false;        // the controller holds her (a boundary state): the chart restarts when she comes back
    float lastRise = 0;         // the last drive move's root z rise (bone 0 [I])
    int attachedBefore = -1;    // +0xC94's markup as the tick began (an attach made by this tick's change starts a blend)
    bool launched = false;      // this flight was launched by a take-off (Jump) rather than walked off (Fall)
    int takeoffState = -1;      // the take-off state (action 504) the chart left last, and on which world tick
    uint64_t takeoffTick = ~0ull;
    struct Blend { bool on = false; int state = -1; glm::vec3 from{0}, to{0}; float yawFrom = 0, yawTo = 0, frames = 1, t = 0; } blend;
    ChartRuntime* rt = nullptr;   // Lara's chart (agent 0) and the upper body's (agent 1), Shadow's
    ChartRuntime* up = nullptr;
    const Charts* ch = nullptr;
    // ---- end drive ----

    // The tick's input timing (the chart's action records), stick and ground check: shadow and drive alike.
    void inputs(const World& world, const Actions& a, glm::vec2 stickW, float stickL) {
        w = &world;
        const float dt = float(kTickSeconds); // [I] real time: the chart's timeMult ignores the combat time scale here
        const bool down[kButtons] = {a.jump, a.drop, a.sprint, a.interact, a.grapple, a.manualAim, a.grenade, a.recenter || a.manualAim};
        for (size_t i = 0; i < kButtons; ++i) {
            buttons[i].update(down[i], dt, 0.0f);
            sincePressed[i] = buttons[i].pressed() ? 0.0f : sincePressed[i] + dt;
        }
        aimHeld = a.aim;
        nowMs = uint32_t(double(world.ticks()) * kTickSeconds * 1000.0);
        groundCheck(*ch);
        if (ground.steep) lastSteepMs = std::max<uint32_t>(nowMs, 1);
        stickWorld = stickW, stickLen = stickL, walkHeld = a.walk;
    }
    // 0x768C50: agent 0 (Lara), then agent 1 (the upper body); --chart-state's entry.
    void chartTick() {
        const float dt = float(kTickSeconds);
        if (!started) {
            playerFloat(0x6DC) = w->player().state().pos.z; // placement: message 0x2A -> 0x700060
            rt->start(*this), up->start(*this), started = true;
        } else {
            rt->tick(*this, dt * 30.0f);
            up->tick(*this, dt * 30.0f);
        }
        // Wine corpus cases that begin in the previous case's state (case.json start.state): put the chart there at the
        // given world tick, as a change would (ChartRuntime::enter), without a transition.
        if (enterState >= 0 && w->ticks() >= enterTick) {
            // Drive mode: a hold state (its chain runs action 420) needs +0xC94; the Wine case began attached (a hold
            // survives the corpus' teleport), so the markup nearest her hands is attached as the state's actions would
            // have [I: harness only, W/chart/wine/README.md "Harness limits"].
            if (driving && rt->chainHasAction(enterState, 420)) seedAttach();
            rt->enter(*this, enterState), enterState = -1;
        }
    }
    // ---- group B ---- A kind-2 request (an anim-graph node, 0x443720 -> 0x43E250) that ChartRuntime::resolveRequest
    // cannot resolve has no clip on the chart's clock (docs Runtime "Anim request"). [I] Its end (0x768CF0 from the anim
    // controller) is then taken from our controller: the one-shot clips it plays from the chart's state on ending in a
    // loop (the controller settles: ledgeb_attach_M -> ledgeb_idle). A resolved node ends on the chart's own clip clock.
    // Shadow mode only: in drive mode the clip playing is the chart's, so there is no controller to bridge from.
    void kind2Bridge() {
        const PlayerState& ps = w->player().state();
        const int st = rt->state();
        if (st != kind2State) kind2State = st, kind2Since = w->ticks();
        if (ps.clip != ctlClip) {
            const State* cs = rt->data().state(st);
            if (started && cs && cs->requestKind == 2 && !rt->resolved() && !ctlClip.empty() && !ctlLoop && ps.loop && ctlClipSince + 1 >= kind2Since)
                rt->animEnded(*this);
            ctlClip = ps.clip, ctlLoop = ps.loop, ctlClipSince = w->ticks();
        }
    }
    // ---- group CD ---- 0x768C50 after the chart tick: agent +0x6C = player +0x78. Then the chart clip's events over the
    // frames its clock ran this tick (0x716F30): command 0x13 opens the critical section (+0x78 = 1, agent +0x53C = 1),
    // 0x14 closes it. [I] the anim update runs after the chart tick (the order is not pinned, docs Runtime "Open").
    void clipCommandsRun() {
        rt->field(0x6C) = playerInt(0x78);
        // Commands 0x13 / 0x14 of `clip` with lo < frame <= hi, in frame order.
        auto apply = [&](const std::string& clip, float lo, float hi) {
            const auto ev = clipCommands.find(clip);
            if (ev == clipCommands.end()) return;
            std::vector<std::array<int, 3>> hits;
            for (const auto& e : ev->second.commands)
                if ((e[0] == 0x13 || e[0] == 0x14) && float(e[2]) > lo && float(e[2]) <= hi) hits.push_back(e);
            std::ranges::stable_sort(hits, {}, [](const std::array<int, 3>& e) { return e[2]; });
            for (const auto& e : hits) playerInt(0x78) = e[0] == 0x13, rt->deferFlag7 = e[0] == 0x13;
        };
        const bool fresh = rt->clipStarts() != clipChanges;
        // The clip a change replaced played to its end (its clock ran out this tick, or the transition taken tests
        // AnimationEnded 0xC9 v1): its remaining events run first, as the anim update reaches them; events lie inside
        // the clip, so every one past the playhead counts. wall_jump_attach_air's 0x14 sits on frame 6, its last, while
        // the chart's clock leaves S1291 at 2.5 frames [core: clip clock]; without the 0x14 agent +0x53C stays set and
        // every flag-7 exit of S1294 waits forever (Wine jh_stick_jump_rebound). [I] a change mid-clip runs none.
        if (fresh && !clipName.empty() && !clipLoops) {
            bool ended = clipLen > 0 && clipFrame + timeMult * clipRate >= clipLen;
            const int t = rt->lastTransition();
            if (t >= 0 && size_t(t) < rt->data().transitions.size())
                for (const Cond& c : rt->data().transitions[size_t(t)].conds) ended |= c.id == 0xC9 && c.value == 1 && !c.negate();
            if (ended) apply(clipName, clipFrame, 1e9f);
        }
        clipName.clear();
        if (const Request* p = rt->pattern()) {
            const float cur = rt->animFrames(), len = rt->animLengthFrames();
            const float prev = fresh ? rt->clipStartFrame() - 1e-4f : clipFrame;
            if (cur > prev) {
                if (rt->animLoops() && len > 0) {
                    for (float k = std::floor(prev / len); k <= std::floor(cur / len); k += 1)
                        apply(p->clip, std::max(prev - k * len, -1.0f), std::min(cur - k * len, len));
                } else {
                    apply(p->clip, prev, cur);
                }
            }
            clipFrame = cur, clipName = p->clip, clipLen = len, clipLoops = rt->animLoops(), clipRate = p->rate;
        }
        clipChanges = rt->clipStarts();
    }
    // ---- group J ---- Message 0x1C7 = chart event 455, posted by every ground check that finds a floor (0x5DCE20 with
    // flag 2; the player's message handler 0x76FC30 zeroes agent +0x8C and passes it to the chart, 0x787CD0). Only
    // flight / landing states handle it (S288, S1004, S1292, ...), so on the ground it does nothing. [I] the player call's
    // flag 2 and the check's place in the frame against the chart tick (ours after it).
    void postFloor() {
        if (!ground.floor) return;
        rt->field(0x8C) = 0;
        rt->post(*this, 455);
    }
    void traceRow(const Actions& a) {
        if (rt->state() != lastState || rt->stats.changes != lastChanges) stateTicks = 0;
        else ++stateTicks;
        lastState = rt->state(), lastChanges = rt->stats.changes;
        const PlayerState& s = w->player().state();
        const int r = rt->request();
        const std::string& chartClip = r >= 0 && size_t(r) < ch->requests.size() ? ch->requests[size_t(r)].clip : std::string();
        const bool match = sameClip(chartClip, s.clip);
        (match ? matched : diverged)++;
        if (match != lastMatch || ticks == 0) {
            if (!match) ++divergences;
            if (log) // tick = the frame's index (the input script's tick, tru.exe's frame counter during the frame)
                std::fprintf(stderr, "ou-game: chart tick %llu %s S%d (parent %d, req %d %s, T%d) | ours %s clip %s\n",
                             (unsigned long long)(w->ticks() - 1), match ? "matches" : "DIVERGES", rt->state(), rt->parent(), r, chartClip.c_str(),
                             rt->lastTransition(), std::string(stateName(s.state)).c_str(), s.clip.c_str());
        }
        lastMatch = match;
        if (trace) {
            char row[512];
            std::snprintf(row, sizeof row, "%llu\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%x\tt30=%.3f\tvar_anim=%.3f\n",
                          (unsigned long long)(w->ticks() - 1), rt->state(), rt->parent(), r, stateTicks, s.pos.x, s.pos.y, s.pos.z, s.vel.x,
                          s.vel.y, s.vel.z, glm::degrees(s.yaw), inputMask(a), rt->stateTime(), rt->animFrames());
            trace << row;
        }
        ++ticks;
    }

    // ---- drive ----
    bool isBoundary(int st) const;
    void restartChart();
    bool driveTick(World& world, Player& player, glm::vec2 move, const Actions& a, float dt);
    void launch(int from);
    void aimAt(const MarkupSlot& target, float frames);
    float steerRate() const;
    game::State coarse(Player::DriveStep::Motion motion) const;
    float hangSide(int m, int seg, glm::vec3 point) const;
    void seedAttach();
    uint64_t driveClipStarts = ~0ull; // the chart's clip starts the drive last applied
    // PlayerDriver
    bool drive(World& world, Player& player, glm::vec2 move, const Actions& a, float dt) override;
    nlohmann::json save() const override;
    void load(const nlohmann::json& j) override;
    void applyLoad(const nlohmann::json& j);
    nlohmann::json pendingLoad;
    bool loadDue = false;
};

// Traversal chart boundary [I]: states whose request plays a swim / grapple / death / bike / carried-portable clip
// (docs/re/PLAYER_CHART.md "Traversal subgraph": the boundary families) hand Lara to the controller.
bool Shadow::Impl::isBoundary(int st) const {
    static constexpr std::string_view kPrefix[] = {"swim", "grapple", "rope", "death", "die_", "dead", "bike", "moto", "portable", "carry"};
    const auto bad = [](std::string_view clip) {
        const std::string c = lower(clip);
        for (const auto p : kPrefix)
            if (c.starts_with(p)) return true;
        return false;
    };
    if (const Request* p = rt->pattern(); p && st == rt->state() && bad(p->clip)) return true;
    const State* s = rt->data().state(st);
    if (!s || s->requestKind == 2 || s->request < 0 || size_t(s->request) >= ch->requests.size()) return false;
    return bad(ch->requests[size_t(s->request)].clip);
}

// --chart-state into a hold state in drive mode: +0xC94 = the markup nearest her grip, by family: a ledge (type 1),
// cover (12) or beam (14, the hang under it) by the hang hands (lara.drm ledgeb_idle), a horizontal pole (4 / 6) by
// the pole hang's hands (HPole_idleHangPerp), a vertical pole, ladder, perch or pillar (5 / 9 / 13 / 15) by her root;
// within 900 units (the corpus pins her up to 600 over a hang root: its own air grab took the ledge during the hold).
// The corpus spawns her in the held pose (case start.ours), so she stays put unless the grip is a ledge hang more than
// 60 units off: then she goes to its hang pose (attachPose).
void Shadow::Impl::seedAttach() {
    const PlayerState& s = w->player().state();
    const float c = std::cos(s.yaw + 1.5707964f), sn = std::sin(s.yaw + 1.5707964f);
    const auto at = [&](const glm::vec3& h) { return s.pos + glm::vec3(c * h.x - sn * h.y, sn * h.x + c * h.y, h.z); };
    const glm::vec3 hangHands = at(w->anims().hangHandOffset), poleHands = at(w->anims().poleHandOffset);
    const auto& col = w->collision();
    float best = 900;
    MarkupSlot pick;
    bool hangGrip = false;
    for (size_t m = 0; m < col.markup.size(); ++m) {
        const uint8_t type = col.markup[m].type;
        const bool hang = type == 1 || type == 12 || type == 14, pole = type == 4 || type == 6;
        const glm::vec3 probe = hang ? hangHands : pole ? poleHands : s.pos;
        const auto& p = col.markup[m].points;
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const glm::vec3 ab = p[i + 1] - p[i];
            const float l2 = glm::dot(ab, ab);
            const float t = l2 > 0 ? std::clamp(glm::dot(probe - p[i], ab) / l2, 0.0f, 1.0f) : 0.0f;
            const float d = glm::length(probe - (p[i] + ab * t));
            if (d < best) best = d, pick = slot(int(m), int(i), t * glm::length(glm::vec2(ab))), hangGrip = type == 1;
        }
    }
    if (!pick) return;
    chartAttached = pick, attachedBefore = pick.markup;
    if (pm && hangGrip && best > 60) {
        const auto& p = col.markup[size_t(pick.markup)].points;
        const int seg = std::clamp(int(pick.param), 0, int(p.size()) - 2);
        const float t = (pick.param - float(seg)) * glm::length(glm::vec2(p[size_t(seg) + 1] - p[size_t(seg)]));
        const auto [root, yaw] = pm->hangPose(pick.markup, seg, t, hangSide(pick.markup, seg, pick.point));
        PlayerState ps = s;
        ps.pos = root, ps.yaw = yaw, ps.vel = glm::vec3(0);
        pm->restore(ps);
        placements = pm->placements(); // ours, not a placement that restarts the chart
    }
}

// The chart (re)starts with her where she is (0x787E00 from the root, which settles in the idle by itself).
void Shadow::Impl::restartChart() {
    started = false, parked = false, launched = false;
    blend = {};
    chartAttached = MarkupSlot{}, chartTarget = {}, handedTarget = -1;
    collisionMask = 0x48;
}


// The side of markup m's segment `seg` she hangs on: the one her root is on (attachPose: root = point - the hands'
// offset, facing the markup) [I: tru.exe takes the markup normal +0xCC0].
float Shadow::Impl::hangSide(int m, int seg, glm::vec3 point) const {
    const auto& p = w->collision().markup[size_t(m)].points;
    const int s = std::clamp(seg, 0, int(p.size()) - 2);
    const glm::vec2 d = glm::vec2(p[size_t(s) + 1] - p[size_t(s)]);
    const glm::vec2 n = glm::length(d) > 0 ? glm::vec2(-d.y, d.x) / glm::length(d) : glm::vec2(0);
    return glm::dot(n, glm::vec2(w->player().state().pos - point)) >= 0 ? 1.0f : -1.0f;
}

// Action 601's turn rate (0x75B330: prop 601 when 0 < it < prop 605, else prop 605) [O]; the props hold degrees per 30 Hz
// frame [I], so rad/s = deg x 30 x pi / 180.
float Shadow::Impl::steerRate() const {
    const auto p601 = rt->propFloat(601), p605 = rt->propFloat(605);
    float deg = p605.value_or(0);
    if (p601 && *p601 > 0 && (!p605 || *p601 < *p605)) deg = *p601;
    return glm::radians(deg) * 30.0f;
}

game::State Shadow::Impl::coarse(Player::DriveStep::Motion motion) const {
    using M = Player::DriveStep::Motion;
    using GS = game::State;
    const std::string clip = lower(w->player().state().clip);
    const auto has = [&](std::string_view s) { return clip.find(s) != std::string::npos; };
    // The traversal families by the clip set the chart plays (lara.drm prefixes bbeam_, hpole_, ladder_, perch_,
    // pillar_, DICE_, wall_hug), so World's subsystems (camera, interactions) see the family [I: mapping].
    if (clip.starts_with("bbeam")) return GS::Beam;
    if (clip.starts_with("hpole")) return motion == M::Air ? GS::HangJump : GS::PoleHang;
    if (clip.starts_with("ladder")) return GS::Ladder;
    if (clip.starts_with("perch")) return GS::Perch;
    if (clip.starts_with("pillar")) return GS::Pillar;
    if (clip.starts_with("dice")) return GS::Climb;
    if (has("drop_to_ledgeb")) return GS::DropToHang;
    if (motion == M::Air) return clip.starts_with("ledgeb_jump") ? GS::HangJump : launched ? GS::Jump : GS::Fall;
    if (motion == M::Hang || motion == M::Blend) {
        if (has("climb")) return GS::ClimbUp;
        const int st = rt ? rt->state() : -1; // the shimmy states (the grip moves: S459, S432, S441)
        return has("shimmy") || st == 459 || st == 432 || st == 441 ? GS::Shimmy : GS::Hang;
    }
    if (clip.find("land") != std::string::npos) return GS::Land;
    if (clip.find("crouch") != std::string::npos) return GS::Crouch;
    if (clip.find("roll") != std::string::npos) return GS::Roll;
    if (clip.find("walk") != std::string::npos) return GS::Walk;
    if (clip.find("run") != std::string::npos || clip.find("sprint") != std::string::npos) return GS::Run;
    if (clip.find("turn") != std::string::npos) return GS::Turn;
    if (clip.find("jump") != std::string::npos) return GS::JumpPrep;
    return GS::Idle;
}

// 0x73EDC0's solve [O: it re-aims the flight at the target with the time left, 0x752410]: the velocity that brings her
// root onto the target's hang root after `frames` 30 Hz frames under gravity. The hang root: attachPose of the target
// point on the side she flies from [I: tru.exe aims the query's arc start for the target type, 0x771830].
void Shadow::Impl::aimAt(const MarkupSlot& target, float frames) {
    if (!target || frames <= 0.5f) return;
    const auto& col = w->collision();
    if (size_t(target.markup) >= col.markup.size() || col.markup[size_t(target.markup)].points.size() < 2) return;
    const int seg = std::clamp(int(target.param), 0, int(col.markup[size_t(target.markup)].points.size()) - 2);
    const auto& p = col.markup[size_t(target.markup)].points;
    const float t = (target.param - float(seg)) * glm::length(glm::vec2(p[size_t(seg) + 1] - p[size_t(seg)]));
    const glm::vec3 root = pm->hangPose(target.markup, seg, t, hangSide(target.markup, seg, target.point)).first;
    const float T = frames / 30.0f, g = pm->tuning().gravity;
    glm::vec3 v = (root - w->player().state().pos) / T;
    v.z += 0.5f * g * T;
    pm->setVelocity(v);
}

// The flight's launch, on the change into a state whose chain runs action 510 from one that did not (stateEntered)
// [I: tru.exe's launch call (0x7726D0 -> 0x76FFC0, the state's jump query prop 541) is not found among the chart's
// actions; Wine shows the take-off velocity on the flight state's first row]. The take-off states (action 504) launch
// by their query: S248 standing q2 (straight up), S245 / S247 with the stick q43, S249 / S280 / S281 running q26 / q1
// (Tuning's launch speeds, measured under Wine); with a jump target the flight is aimed at it over its hit frame
// (player +0x77C). The hang take-offs launch as the controller's free leaps (Tuning::hangLeap*, Wine ledge 973):
// ledgeb_jump_back out from the wall, ledgeb_jump_right(_from_antic) to her right (mirrored: left), ledgeb_jump_up up.
// Anything else (walking off an edge) keeps her velocity.
void Shadow::Impl::launch(int from) {
    const Tuning& t = pm->tuning();
    const PlayerState& s = w->player().state();
    const glm::vec3 f(std::cos(s.yaw), std::sin(s.yaw), 0);
    launched = true;
    if (from >= 0 && rt->chainHasAction(from, 504)) {
        glm::vec3 v(0, 0, t.jumpVz);
        switch (from) {
        case 245: case 247:
            if (stickLen > 0.1f) pm->setYaw(std::atan2(stickWorld.y, stickWorld.x));
            v = glm::vec3(std::cos(w->player().state().yaw), std::sin(w->player().state().yaw), 0) * t.standJumpSpeed + glm::vec3(0, 0, t.standMoveJumpVz);
            break;
        case 249: case 280: case 281: v = f * t.runJumpSpeed + glm::vec3(0, 0, t.runJumpVz); break;
        default: break;
        }
        pm->setVelocity(v);
        if (const MarkupSlot tg = chartJumpTarget(*this); tg && chartTarget.target) aimAt(tg, chartTarget.frames);
        return;
    }
    const std::string clip = lower(s.clip);
    const MarkupSlot at = chartAttached ? *chartAttached : MarkupSlot{};
    glm::vec2 out = -glm::vec2(f);
    if (at) out = glm::length(glm::vec2(s.pos - at.point)) > 1 ? glm::normalize(glm::vec2(s.pos - at.point)) : out;
    if (clip == "ledgeb_jump_back") {
        pm->setVelocity(glm::vec3(out * t.hangLeapBack, t.hangLeapVz));
    } else if (clip.starts_with("ledgeb_jump_right")) {
        const glm::vec2 face = -out, right(face.y, -face.x);
        pm->setVelocity(glm::vec3(right * (s.mirror ? -1.0f : 1.0f) * t.hangLeapSide, t.hangLeapVz));
    } else if (clip == "ledgeb_jump_up") {
        pm->setVelocity(glm::vec3(0, 0, t.hangJumpUpVz));
        if (const MarkupSlot tg = chartJumpTarget(*this); tg && chartTarget.target) aimAt(tg, chartTarget.frames);
    } else {
        launched = false;
    }
}

void Shadow::Impl::stateEntered(int from, int to) {
    if (!driving || !pm) return;
    const bool flyFrom = from >= 0 && rt->chainHasAction(from, 510), flyTo = rt->chainHasAction(to, 510);
    // A take-off (action 504's chain) left this tick: the flight may come through a link state in the same frame's 0x65
    // chaining (S248 -> S289 -> S294), so the launch reads the take-off state, not the link.
    if (from >= 0 && rt->chainHasAction(from, 504) && !rt->chainHasAction(to, 504)) takeoffState = from, takeoffTick = w->ticks();
    if (flyTo && !flyFrom) launch(from >= 0 && !rt->chainHasAction(from, 504) && takeoffTick == w->ticks() ? takeoffState : from);
    else if (!flyTo && flyFrom) pm->setVelocity(glm::vec3(0)), launched = false;
    // An attach made by this change's PreAnim actions (230 / 232: +0xC94 = the jump target) starts the attach clip's
    // root alignment onto the hang pose (action 236 PSCAF_AlignRootMotionToTarget, unported: [I] a blend over the clip
    // and what its node queued, ChartRuntime::remainingFrames).
    const int now = chartAttached && *chartAttached ? chartAttached->markup : -1;
    if (now >= 0 && now != attachedBefore) {
        const MarkupSlot& a = *chartAttached;
        const auto& p = w->collision().markup[size_t(a.markup)].points;
        if (p.size() >= 2) {
            const int seg = std::clamp(int(a.param), 0, int(p.size()) - 2);
            const float t = (a.param - float(seg)) * glm::length(glm::vec2(p[size_t(seg) + 1] - p[size_t(seg)]));
            const auto [root, yaw] = pm->hangPose(a.markup, seg, t, hangSide(a.markup, seg, a.point));
            const PlayerState& s = w->player().state();
            blend = {true, to, s.pos, root, s.yaw, yaw, std::max(1.0f, rt->remainingFrames()), 0};
            pm->setVelocity(glm::vec3(0));
        }
    }
    attachedBefore = now;
}

bool Shadow::Impl::driveTick(World& world, Player& player, glm::vec2 move, const Actions& a, float dt) {
    using M = Player::DriveStep::Motion;
    w = &world, pm = &player;
    // A ledge hang's root 161 out of the line (Wine, ledges 973 and 1155; REFERENCE_COMPARISON M01) where ledgeb_idle's
    // wrists (bones 55 / 80) reach 141.7: the wrists hang in front of the edge, the fingers over it. [O-runtime; the
    // tru.exe term is not located: 0x76D270's attach frame has the markup point (+0xCA0) as its origin and the normal /
    // edge rows +0xCC0 / +0xCB0, ledgeb_idle has no root translation, the alignment target +0x1260 (239 / 1012, read by
    // 236) is not traced.]
    player.setLedgeGripOut(161.0f - 141.7f);
    const PlayerState& s0 = player.state();
    // The controller keeps what lies outside the traversal chart (swimming, the rope, the bike, death, hit reactions,
    // interactions, close combat); a placement (spawn, load, teleport) restarts the chart.
    switch (s0.state) {
    using GS = game::State;
    case GS::SwimSplash: case GS::SwimTread: case GS::SwimSurface: case GS::SwimDive: case GS::SwimUnder:
    case GS::Surfacing: case GS::SwimClimbOut: case GS::SwimDodge: case GS::Dead: case GS::GrappleThrow:
    case GS::GrappleSwing: case GS::GrapplePull: case GS::GrappleRappel: case GS::Hit: case GS::Vehicle:
    case GS::Use: case GS::Block: case GS::Melee: case GS::Knocked:
        parked = true;
        return false;
    default: break;
    }
    if (player.placements() != placements || parked) restartChart(), placements = player.placements();
    if (loadDue) applyLoad(pendingLoad), loadDue = false, pendingLoad = nullptr;
    inputs(world, a, move, std::min(1.0f, glm::length(move)));
    attachedBefore = chartAttached && *chartAttached ? chartAttached->markup : -1;
    chartTick();
    if (isBoundary(rt->state())) { // the chart hands her over at its boundary edge: the controller decides from here
        player.setVelocity(s0.vel);
        parked = true;
        return false;
    }
    // What the chart decided -> Player::drive.
    Player::DriveStep d;
    if (const Request* p = rt->pattern()) {
        // The clip's mirror: the agent's bit (+0xF4 0x20, transition flags 2 / 5), reflected again by a mirrored request
        // (pattern +0x20 bit 0x80); bool props 201 / 202 of the shimmy states (S432 left 201, S431 right 202) set / clear
        // it [I: their handlers are not decoded; Wine's left shimmy S432 plays ledgeb_right_3 moving to her left].
        bool mirror = rt->mirrored();
        if (rt->propBool(201).value_or(false)) mirror = true;
        else if (rt->propBool(202).value_or(false)) mirror = false;
        d.clip = p->clip, d.loop = rt->animLoops(), d.mirror = mirror != p->mirror;
        d.start = rt->clipStartFrame() / 30.0, d.time = rt->animFrames() / 30.0;
        d.restart = rt->clipStarts() != driveClipStarts;
        // the blend the clip started with: its lead before the pattern's start frame (Request::startFrame = start - blend)
        if (d.restart) d.crossfade = std::max(0.0f, float(p->start) - rt->clipStartFrame()) / 30.0f;
    } else {
        d.rate = animRate;
    }
    driveClipStarts = rt->clipStarts();
    const int st = rt->state();
    const MarkupSlot att = chartAttached ? *chartAttached : MarkupSlot{};
    if (blend.on && (blend.state != st || !att)) blend.on = false;
    if (blend.on) {
        blend.t += timeMult;
        d.motion = M::Blend;
        d.blendFrom = blend.from, d.blendTo = blend.to, d.yawFrom = blend.yawFrom, d.yawTo = blend.yawTo;
        d.blend = blend.t / blend.frames;
    } else if (att && rt->chainHasAction(st, 420)) {
        d.motion = M::Hang;
        const std::string c = lower(d.clip.empty() ? player.state().clip : d.clip);
        d.snap = c.starts_with("ledgeb_") && c.find("jump") == std::string::npos && c.find("climb") == std::string::npos;
        // The grip (+0xC98) moves only with the shimmy (S459 start, S432 shimmy, S441 hustle): the attach and idle
        // clips' sideways root motion leaves it where it is, and the ledge end S448 stops her dead (ChartB, Wine 973:
        // +0xC98 0.907 stays until the stick press, so the end bit 642 holds and S458 -> S450) [O-runtime].
        d.slide = st == 459 || st == 432 || st == 441;
    } else if (rt->chainHasAction(st, 510)) {
        d.motion = M::Air;
        // 0x752410: the flight re-aimed at its target with the time left (+0x77C, action 510's update) [O]
        if (const MarkupSlot tg = chartJumpTarget(*this); tg && chartTarget.target) aimAt(tg, playerFloat(0x77C));
    }
    if (att) {
        const auto& p = world.collision().markup[size_t(att.markup)].points;
        if (p.size() >= 2) {
            d.markup = att.markup, d.seg = std::clamp(int(att.param), 0, int(p.size()) - 2);
            d.t = (att.param - float(d.seg)) * glm::length(glm::vec2(p[size_t(d.seg) + 1] - p[size_t(d.seg)]));
            d.side = hangSide(att.markup, d.seg, att.point);
        }
    }
    // Action 601 (PSCAF_GroundTurnToStick, 0x76AB20 [O]: toward the camera-relative stick heading 0x6EBA90); a clip that
    // turns the root turns her instead (player +0x23C, prop 602) [I: not modelled, both run].
    if (d.motion == M::Ground && rt->chainHasAction(st, 601) && stickLen > 0.1f)
        d.steer = std::atan2(stickWorld.y, stickWorld.x), d.steerRate = steerRate();
    const float z0 = player.state().pos.z;
    d.coarse = coarse(d.motion);
    player.drive(d, dt);
    lastRise = player.state().pos.z - z0;
    // +0xC94's geometry follows her grip (shimmy)
    if (att && player.state().ledge == att.markup) {
        MarkupSlot n = slot(att.markup, player.state().ledgeSeg, player.state().ledgeT);
        n.normal = att.normal;
        chartAttached = n;
    }
    clipCommandsRun();
    updateFoot(*rt, player.state());
    postFloor();
    return true;
}

bool Shadow::Impl::drive(World& world, Player& player, glm::vec2 move, const Actions& a, float dt) {
    return driveTick(world, player, move, a, dt);
}

nlohmann::json Shadow::Impl::save() const {
    if (!started || parked) return nullptr;
    const ChartRuntime::Snapshot s = rt->snapshot(), u = up->snapshot();
    const auto snap = [](const ChartRuntime::Snapshot& x) {
        nlohmann::json f = nlohmann::json::object();
        for (const auto& [k, v] : x.fields) f[std::to_string(k)] = v;
        return nlohmann::json{{"state", x.state}, {"queued", x.queued}, {"last", x.lastTransition}, {"counter", x.animCounter},
                              {"time", x.stateTime}, {"total", x.totalTime}, {"clip", x.animFrames}, {"mirror", x.mirror},
                              {"defer7", x.deferFlag7}, {"fields", f}};
    };
    const auto map = [](const auto& m) {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [k, v] : m) j[std::to_string(k)] = v;
        return j;
    };
    nlohmann::json j{{"version", 1}, {"lara", snap(s)}, {"upper", snap(u)}, {"playerInt", map(storedInts())},
                     {"playerFloat", map(storedFloats())}, {"agentFloat", map(storedAgentFloats())}, {"launched", launched},
                     {"mask", collisionMask}};
    if (chartAttached && *chartAttached) j["attach"] = {{"markup", chartAttached->markup}, {"param", chartAttached->param}};
    return j;
}

// A load restores Lara (Player::restore, a placement) before the next tick: the chart state waits for that tick's drive,
// where the placement's restart would otherwise wipe it.
void Shadow::Impl::load(const nlohmann::json& j) { pendingLoad = j, loadDue = true; }

void Shadow::Impl::applyLoad(const nlohmann::json& j) {
    restartChart();
    if (!j.is_object() || j.value("version", 0) != 1) return; // older saves (no chart state): the chart starts afresh
    const auto snap = [](const nlohmann::json& x) {
        ChartRuntime::Snapshot s;
        s.state = x.value("state", -1), s.queued = x.value("queued", -1), s.lastTransition = x.value("last", -1);
        s.animCounter = x.value("counter", 0), s.stateTime = x.value("time", 0.0f), s.totalTime = x.value("total", 0.0f);
        s.animFrames = x.value("clip", 0.0f), s.mirror = x.value("mirror", false), s.deferFlag7 = x.value("defer7", false);
        if (const auto f = x.find("fields"); f != x.end())
            for (const auto& [k, v] : f->items()) s.fields[std::stoi(k)] = v.get<int>();
        return s;
    };
    for (const auto& [k, v] : j.value("playerInt", nlohmann::json::object()).items()) playerInt(std::stoi(k)) = v.get<int>();
    for (const auto& [k, v] : j.value("playerFloat", nlohmann::json::object()).items()) playerFloat(std::stoi(k)) = v.get<float>();
    for (const auto& [k, v] : j.value("agentFloat", nlohmann::json::object()).items()) agentFloat(std::stoi(k)) = v.get<float>();
    // +0xC94: the markup the save resolved (its ledge reference, unit-relative: PlayerState::ledge), at the saved param
    if (const auto at = j.find("attach"); at != j.end() && pm) {
        const int m = pm->state().ledge;
        const float param = at->value("param", 0.0f);
        if (m >= 0 && size_t(m) < w->collision().markup.size() && w->collision().markup[size_t(m)].points.size() >= 2) {
            const auto& p = w->collision().markup[size_t(m)].points;
            const int seg = std::clamp(int(param), 0, int(p.size()) - 2);
            chartAttached = slot(m, seg, (param - float(seg)) * glm::length(glm::vec2(p[size_t(seg) + 1] - p[size_t(seg)])));
        }
    }
    launched = j.value("launched", false), collisionMask = j.value("mask", 0x48u);
    rt->restore(*this, snap(j.at("lara"))), up->restore(*this, snap(j.at("upper")));
    started = true, placements = pm ? pm->placements() : placements;
}

Shadow::Shadow(const cdc::BigfileSet& bigfile, std::optional<std::filesystem::path> trace, bool log, bool drive)
    : impl_(std::make_unique<Impl>()) {
    auto c = loadCharts(bigfile);
    if (!c) throw std::runtime_error("chart shadow: lara.drm player data not found");
    charts_ = std::make_unique<Charts>(std::move(*c));
    rt_ = std::make_unique<ChartRuntime>(*charts_, charts_->player, Registry::player());
    upper_ = std::make_unique<ChartRuntime>(*charts_, charts_->upper, Registry::upper());
    impl_->rt = rt_.get(), impl_->up = upper_.get(), impl_->ch = charts_.get();
    impl_->upper = upper_.get();
    impl_->log = log;
    impl_->driving = drive;
    rt_->layerRate = drive;
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
PlayerDriver* Shadow::driver() { return impl_->driving ? impl_.get() : nullptr; }

void Shadow::tick(const World& world, const Actions& a) {
    Impl& m = *impl_;
    m.w = &world;
    if (m.driving) { // the chart ran in World::tick's player slot (Impl::driveTick); here only the trace
        if (m.started) m.traceRow(a);
        return;
    }
    const float yaw = world.stickYaw();
    const glm::vec2 fwd(std::cos(yaw), std::sin(yaw)), right(std::sin(yaw), -std::cos(yaw));
    m.inputs(world, a, right * a.move.x + fwd * a.move.y, std::min(1.0f, glm::length(a.move)));
    m.chartTick();
    m.kind2Bridge();
    m.clipCommandsRun();
    m.updateFoot(*rt_, world.player().state());
    m.postFloor();
    m.traceRow(a);
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
