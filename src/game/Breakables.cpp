// Class-30 breakables (falling floors, breaking ledges / beams / poles, braced doors) and explosion records.
// docs/GAMEPLAY.md "Breakables".
#include "game/Breakables.hpp"
#include "game/Game.hpp"

#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>

namespace ou::game {

namespace {

template <class T> T rd(const cdc::Section& s, size_t off) {
    if (off + sizeof(T) > s.data.size()) throw cdc::FormatError("breakable: read past the object data");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof(T));
    return v;
}

// Physics key of piece k (as the enemies' chunk pieces: negative, outside the intro ids).
int32_t pieceKey(int32_t uid, size_t k) { return -(uid * 64 + int32_t(k) + 1); }
// objectMarkup_ item of piece k's markup j (the objects' own model-0 items are 0..).
constexpr int kPieceMarkup = 1000;
int markupItem(size_t piece, size_t j) { return kPieceMarkup + int(piece) * 32 + int(j); }

} // namespace

std::optional<BreakableData> loadBreakable(const cdc::Drm& d) {
    const auto base = formats::objectBaseSection(d);
    if (!base) return std::nullopt;
    const auto& ob = d.sections[*base];
    const auto rec = cdc::internalPointer(ob, 0xC8);
    if (!rec || *rec + 0x60 > ob.data.size() || rd<uint16_t>(ob, *rec + 2) != kClassBreakable || rd<uint16_t>(ob, *rec + 4) != 0xB00B)
        return std::nullopt;
    BreakableData b;
    b.hits = rd<int16_t>(ob, *rec + 0x10);
    const uint32_t pieces = rd<uint32_t>(ob, *rec + 0x1C);
    const auto list = cdc::internalPointer(ob, *rec + 0x18);
    if (pieces > 0x1C || (pieces && !list)) throw cdc::FormatError("breakable: bad piece list"); // 0x6D4030's bound
    for (uint32_t k = 0; k < pieces; ++k) {
        BreakableData::Piece p;
        p.model = rd<uint8_t>(ob, *list + 0x20 * k + 0x0A);
        p.physics = physics::loadObjectPhysics(d, uint32_t(p.model));
        b.pieces.push_back(std::move(p));
    }
    const uint32_t conds = rd<uint32_t>(ob, *rec + 0x28);
    const auto cl = cdc::internalPointer(ob, *rec + 0x34);
    if (conds > 64 || (conds && !cl)) throw cdc::FormatError("breakable: bad condition list");
    std::vector<uint32_t> actionAt;
    for (uint32_t i = 0; i < conds; ++i) {
        const uint32_t c = *cl + 0x1C * i;
        BreakableData::Condition k;
        k.flags = rd<uint32_t>(ob, c), k.speed = rd<float>(ob, c + 4), k.spin = rd<float>(ob, c + 8);
        k.flags2 = rd<uint32_t>(ob, c + 0x14);
        const uint32_t n = rd<uint32_t>(ob, c + 0xC);
        if (const auto ids = cdc::internalPointer(ob, c + 0x10); ids && n <= 32)
            for (uint32_t j = 0; j < n; ++j) k.classes.push_back(rd<int32_t>(ob, *ids + 4 * j));
        if (const auto a = cdc::internalPointer(ob, c + 0x18)) {
            auto at = std::find(actionAt.begin(), actionAt.end(), *a);
            if (at == actionAt.end()) {
                BreakableData::Action act;
                act.id = rd<int16_t>(ob, *a), act.type = rd<int16_t>(ob, *a + 2), act.flags = rd<uint32_t>(ob, *a + 8);
                act.delay = float(rd<int32_t>(ob, *a + 0x1C));
                act.shake = {rd<float>(ob, *a + 0x20), rd<float>(ob, *a + 0x24), rd<float>(ob, *a + 0x28)};
                act.grace = float(rd<int32_t>(ob, *a + 0x38));
                b.actions.push_back(act);
                at = actionAt.insert(actionAt.end(), *a);
            }
            k.action = int(at - actionAt.begin());
        }
        b.conditions.push_back(std::move(k));
    }
    return b;
}

std::optional<ExplosionRecord> loadExplosion(const cdc::Drm& d) {
    const auto base = formats::objectBaseSection(d);
    if (!base) return std::nullopt;
    const auto& ob = d.sections[*base];
    const auto rec = cdc::internalPointer(ob, 0xC8);
    if (!rec || *rec + 0x84 > ob.data.size() || rd<uint16_t>(ob, *rec + 0x70) != 0x2775) return std::nullopt;
    const uint32_t e = *rec + 0x70;
    ExplosionRecord x;
    x.damage = float(rd<int16_t>(ob, e + 4)), x.edgeDamage = float(rd<uint16_t>(ob, e + 6));
    x.radius = float(rd<int16_t>(ob, e + 0xC));
    x.knock = float(rd<int16_t>(ob, e + 0xE)), x.knockEdge = float(rd<int16_t>(ob, e + 0x10));
    x.angle = float(rd<uint16_t>(ob, e + 0x12));
    if (!(x.damage > 0) || !(x.radius > 0)) return std::nullopt; // 0x69F1E0 does nothing without damage
    return x;
}

// ---- World: breakables ---------------------------------------------------------------------------------

// The loaded units' class-30 intros (UnitData::breakables); their state lives in breakableStates_ (saved) so a unit
// streaming out and back finds its floors as they were.
void World::bindBreakables() {
    breakables_.clear();
    for (const auto& u : units_)
        for (const auto& in : u.data.breakables) {
            auto data = breakableData_.find(in.object);
            if (data == breakableData_.end()) {
                std::shared_ptr<const BreakableData> loaded;
                if (const auto* bf = script_.bigfile())
                    if (auto bytes = bf->read("pc-w\\" + in.object + ".drm")) try {
                            if (auto b = loadBreakable(cdc::parseDrm(std::move(*bytes)))) loaded = std::make_shared<const BreakableData>(std::move(*b));
                        } catch (const cdc::FormatError& e) { warnings.push_back(in.object + ": " + e.what()); }
                data = breakableData_.emplace(in.object, std::move(loaded)).first;
            }
            if (!data->second) continue;
            breakables_[in.uid] = {u.data.name, in.object, glm::translate(glm::mat4(1), u.offset) * in.frame, data->second};
            breakableStates_.try_emplace(in.uid, BreakableState{BreakPhase::Whole, data->second->hits});
        }
    breakableUnits_ = unitsVersion_;
    syncedObjects_ = ~size_t(0);
}

// Whole breakables collide as their pieces' models (appended to the object colliders by syncObjects).
void World::breakableColliders() {
    for (const auto& [uid, b] : breakables_) {
        if (breakableStates_.at(uid).phase == BreakPhase::Broken) continue;
        for (const auto& piece : b.data->pieces)
            for (int seg = 0; seg < int(piece.physics.segments.size()); ++seg)
                if (auto shape = physics::solidShape(piece.physics, seg)) {
                    col_.dynamics.push_back({uid, std::move(shape), b.frame * piece.physics.segments[size_t(seg)]});
                    colliders_.push_back({uid, seg, -1, b.frame, piece.physics.segments[size_t(seg)]});
                }
    }
}

// Their pieces' markup (ledges to hang from, the pole, the beam) while they are whole.
void World::tickBreakableMarkup() {
    for (const auto& [uid, b] : breakables_) {
        const bool whole = breakableStates_.at(uid).phase != BreakPhase::Broken;
        for (size_t k = 0; k < b.data->pieces.size(); ++k) {
            const auto& markup = b.data->pieces[k].physics.markup;
            for (size_t j = 0; j < markup.size() && j < 32; ++j) {
                auto [it, added] = objectMarkup_.try_emplace({uid, markupItem(k, j)}, int(col_.markup.size()));
                if (added) {
                    collision::Markup m = markup[j];
                    for (vec3& q : m.points) q = vec3(b.frame * glm::vec4(q, 1));
                    col_.markup.push_back(std::move(m));
                }
                if (size_t(it->second) < col_.markup.size()) col_.markup[size_t(it->second)].type = whole ? markup[j].type : 0;
            }
        }
    }
    for (const auto& [key, index] : objectMarkup_) // its unit streamed out
        if (key.second >= kPieceMarkup && !breakables_.contains(key.first) && size_t(index) < col_.markup.size())
            col_.markup[size_t(index)].type = 0;
}

int32_t World::breakableOfMarkup(int markup) const {
    if (markup < 0) return 0;
    for (const auto& [key, index] : objectMarkup_)
        if (index == markup && key.second >= kPieceMarkup && breakables_.contains(key.first)) return key.first;
    return 0;
}

void World::startBreak(int32_t uid, int action) {
    const auto b = breakables_.find(uid);
    auto& st = breakableStates_[uid];
    if (b == breakables_.end() || st.phase != BreakPhase::Whole || action < 0 || size_t(action) >= b->second.data->actions.size()) return;
    const auto& a = b->second.data->actions[size_t(action)];
    st.action = action;
    if (a.type == 2 || a.type == 3) st.phase = BreakPhase::Waiting, st.timer = a.delay, st.away = 0; // 0x6D2C80: now + delay frames
    else breakNow(uid);
}

// 0x6D2960 / 0x6D2A50: the pieces become rigid bodies of the physics world (0x69BC70), its collision and markup go.
void World::breakNow(int32_t uid) {
    auto& st = breakableStates_[uid];
    if (st.phase == BreakPhase::Broken) return;
    st.phase = BreakPhase::Broken, st.timer = 0;
    broken_.push_back(uid);
    syncedObjects_ = ~size_t(0);
    const auto& s = player_.state();
    const bool attached = s.state == State::Hang || s.state == State::Shimmy || s.state == State::ClimbUp || s.state == State::DropToHang ||
                          s.state == State::PoleHang || s.state == State::Beam || s.state == State::HangJump;
    if (attached && breakableOfMarkup(s.ledge) == uid) player_.letGo(); // what she held is gone: she falls
    tickBreakableMarkup();
}

bool World::breakBreakable(int32_t uid, int actionId) {
    const auto b = breakables_.find(uid);
    if (b == breakables_.end()) return false;
    const auto& actions = b->second.data->actions;
    for (size_t i = 0; i < actions.size(); ++i)
        if (actions[i].id == actionId) return startBreak(uid, int(i)), true;
    return false;
}

// Message 0x11C (0x6D3160 -> 0x6D20C0 -> 0x69BFC0): a hit counts when a condition lists its damage class, or by the
// condition's flags (0x400 class 6; 0x800.. classes 0-2 dealt by Lara); the last one starts that condition's action.
bool World::hitBreakable(int32_t uid, int damageClass, bool fromLara) {
    const auto b = breakables_.find(uid);
    if (b == breakables_.end()) return false;
    auto& st = breakableStates_[uid];
    if (st.phase != BreakPhase::Whole) return true;
    for (const auto& c : b->second.data->conditions) {
        const bool listed = std::ranges::find(c.classes, damageClass) != c.classes.end();
        const bool byFlag = ((c.flags & 0x400) && damageClass == 6) ||
                            (fromLara && damageClass >= 0 && damageClass <= 2 && (c.flags & (0x800u << damageClass)));
        if (!listed && !byFlag) continue;
        if (--st.hits < 1) startBreak(uid, c.action);
        break;
    }
    return true;
}

// Lara's contacts (0x6EC510 for the markup she is attached to, the ground contact for what she stands on) meet the
// conditions' flags; a waiting break counts its frames down (0x6D2D70).
void World::tickBreakables(float dt) {
    broken_.clear();
    if (breakableUnits_ != unitsVersion_) bindBreakables();
    if (breakables_.empty()) return;
    const auto& s = player_.state();
    const float frames = dt * 30.0f;
    int32_t stoodOn = 0;
    const bool grounded = s.state == State::Idle || s.state == State::Walk || s.state == State::Run || s.state == State::Stop ||
                          s.state == State::Turn || s.state == State::Land || s.state == State::Crouch || s.state == State::Roll ||
                          s.state == State::JumpPrep || s.state == State::Use;
    if (grounded)
        if (const auto g = col_.groundProbe(s.pos + vec3(0, 0, 20), 60); g && g->dynamic >= 0) stoodOn = col_.dynamics[size_t(g->dynamic)].id;
    const int32_t held = breakableOfMarkup(s.ledge);
    const uint8_t heldType = held && size_t(s.ledge) < col_.markup.size() ? col_.markup[size_t(s.ledge)].type : 0;
    const bool hanging = s.state == State::Hang || s.state == State::Shimmy || s.state == State::ClimbUp;
    std::vector<int32_t> due;
    for (const auto& [uid, b] : breakables_) {
        auto& st = breakableStates_.at(uid);
        if (st.phase == BreakPhase::Broken) continue;
        uint32_t kinds = 0; // the condition flags her contact sets
        bool beam = false;
        if (stoodOn == uid) kinds |= 0x08;
        if (held == uid) {
            if (heldType == 1 && hanging) kinds |= 0x40;
            if ((heldType == 6 || heldType == 4) && s.state == State::PoleHang) kinds |= 0x20;
            if (heldType == 14 && s.state == State::Beam) beam = true;
        }
        int met = -1;
        for (const auto& c : b.data->conditions)
            if ((c.flags & kinds) || (beam && (c.flags2 & 1))) { met = c.action; break; }
        if (st.phase == BreakPhase::Whole) {
            if (met >= 0) startBreak(uid, met);
            continue;
        }
        const auto& a = b.data->actions[size_t(st.action)];
        if (a.flags & 0x40) { // the wait lasts only while she stays on it
            st.away = met >= 0 ? 0 : st.away + frames;
            if (st.away > a.grace) { st.phase = BreakPhase::Whole; continue; }
        }
        if ((st.timer -= frames) <= 0) due.push_back(uid);
    }
    for (const int32_t uid : due) breakNow(uid);
}

void World::breakableLoose(std::vector<physics::Scene::Loose>& out) const {
    for (const auto& [uid, b] : breakables_) {
        if (breakableStates_.at(uid).phase != BreakPhase::Broken) continue;
        for (size_t k = 0; k < b.data->pieces.size(); ++k) out.push_back({pieceKey(uid, k), &b.data->pieces[k].physics, b.frame});
    }
}

std::vector<World::BreakablePiece> World::breakablePieces() const {
    std::vector<BreakablePiece> out;
    for (const auto& [uid, b] : breakables_) {
        const auto& st = breakableStates_.at(uid);
        for (size_t k = 0; k < b.data->pieces.size(); ++k) {
            BreakablePiece p{uid, int(k), b.data->pieces[k].model, b.frame, st.phase, vec3(0)};
            if (st.phase == BreakPhase::Broken) p.world = physics_.loose(pieceKey(uid, k)).value_or(b.frame);
            else if (st.phase == BreakPhase::Waiting) p.shake = b.data->actions[size_t(st.action)].shake;
            p.frame = b.frame;
            out.push_back(p);
        }
    }
    return out;
}

std::optional<World::BreakPhase> World::breakablePhase(int32_t uid) const {
    const auto it = breakables_.find(uid);
    if (it == breakables_.end()) return std::nullopt;
    return breakableStates_.at(uid).phase;
}

nlohmann::json World::serializeBreakables() const {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [uid, st] : breakableStates_)
        if (st.phase != BreakPhase::Whole || breakables_.contains(uid)) out.push_back({uid, int(st.phase), st.hits, st.action, st.timer, st.away});
    return out;
}

void World::restoreBreakables(const nlohmann::json& j) {
    std::map<int32_t, BreakableState> states;
    if (j.is_array())
        for (const auto& e : j) {
            const int phase = e.at(1).get<int>();
            if (phase < 0 || phase > 2) throw std::runtime_error("objects: bad breakable phase");
            states[e.at(0).get<int32_t>()] = {BreakPhase(phase), e.at(2).get<int>(), e.at(3).get<int>(), e.at(4).get<float>(), e.at(5).get<float>()};
        }
    breakableStates_ = std::move(states);
    breakables_.clear();
    breakableUnits_ = ~uint64_t(0); // bound again on the next tick, keeping these states
}

// ---- World: explosions ---------------------------------------------------------------------------------

// The area hit 0x69F1E0 of an explosion record: every living target and Lara within its radius take the damage
// (class kDamageClassBlast) and are thrown; objects that take damage (other barrels, crates, breakables) are hit; loose
// bodies and ragdolls get the knock.
void World::blast(vec3 at, const ExplosionRecord& x) {
    explosions_.push_back(at);
    const auto fall = [&](float d) { return std::clamp(d / x.radius, 0.0f, 1.0f); };
    const float lift = std::sin(glm::radians(x.angle)), out = std::cos(glm::radians(x.angle));
    for (Damageable* t : std::vector<Damageable*>(targets)) { // onHit may unregister a target
        if (!t || !t->alive()) continue;
        const vec3 p = t->aimPoint();
        const float d = glm::distance(at, p);
        if (d >= x.radius) continue;
        const vec3 dir = d > 1e-3f ? (p - at) / d : vec3(0, 0, 1);
        Hit hit{glm::mix(x.damage, x.edgeDamage, fall(d)), p, dir, Team::Neutral};
        hit.damageClass = kDamageClassBlast;
        vec3 flat(dir.x, dir.y, 0);
        if (glm::dot(flat, flat) > 1e-6f) flat = glm::normalize(flat);
        hit.knockVel = (flat * out + vec3(0, 0, lift)) * glm::mix(x.knock, x.knockEdge, fall(d)); // units/s [I], as the grenade's
        t->onHit(hit);
    }
    const vec3 chest = player_.state().pos + vec3(0, 0, targeting.eyeHeight);
    if (const float d = glm::distance(at, chest); d < x.radius) damageLara(glm::mix(x.damage, x.edgeDamage, fall(d)), at, kDamageHeavy);
    std::vector<int32_t> struck;
    for (const auto& o : script_.objects())
        if (glm::distance(at, o.pos + o.offset) < x.radius && uberRuntime_.contains(o.uid)) struck.push_back(o.uid);
    for (const int32_t uid : struck) damageObject(uid, x.damage, kDamageClassBlast);
    struck.clear();
    for (const auto& [uid, b] : breakables_)
        if (glm::distance(at, vec3(b.frame[3])) < x.radius) struck.push_back(uid);
    for (const int32_t uid : struck) hitBreakable(uid, kDamageClassBlast, false);
    physics_.blast(at, x.radius, x.knock, x.knockEdge, x.angle);
}

// Uber state action 0x13 gave birth to objectlist id `object` at `at`: an effect object with an explosion record goes
// off there on the next tick (its anim graph's first record hands the record to the area hit, 0x6A5C50).
void World::birthBlast(uint16_t object, vec3 at) {
    const std::string* name = script_.objectName(object);
    if (!name) return;
    auto it = explosionData_.find(*name);
    if (it == explosionData_.end()) {
        std::optional<ExplosionRecord> rec;
        if (const auto* bf = script_.bigfile())
            if (auto bytes = bf->read("pc-w\\" + *name + ".drm")) try {
                    rec = loadExplosion(cdc::parseDrm(std::move(*bytes)));
                } catch (const cdc::FormatError& e) { warnings.push_back(*name + ": " + e.what()); }
        it = explosionData_.emplace(*name, rec).first;
    }
    if (it->second) pendingBlasts_.push_back({at, *it->second, 1});
}

// A born squib goes off at the end of the tick after its birth: bodies the same state's actions set free (a door leaf
// off its hinge) are in the physics scene by then and take its knock.
void World::tickBlasts() {
    std::vector<PendingBlast> due;
    for (auto it = pendingBlasts_.begin(); it != pendingBlasts_.end();)
        if (it->ticks-- <= 0) due.push_back(*it), it = pendingBlasts_.erase(it);
        else ++it;
    for (const auto& b : due) blast(b.at, b.record);
}

} // namespace ou::game
