// Unit streaming (portal neighbours relocated into one world frame), portal crossing, checkpoints,
// respawn and the state hooks saves use. Evidence: docs/formats/LEVELS.md "Streaming", docs/SAVES.md.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
// [I] The game crosses portals when the instance's collision hits the unit's signal faces (0x5D2CA0 ->
// 0x5D2AA0); we test Lara's path at mid-body height against the portal quad's plane and bounds.
constexpr float kPortalProbeHeight = 250.0f, kPortalMargin = 50.0f;
// [I] Checkpoint activation is scripted (checkpoint.drm scripts, not decoded): touching = feet within
// this cylinder around the checkpoint intro.
constexpr float kCheckpointRadius = 250.0f, kCheckpointHeight = 400.0f;

vec3 v3(const float* f) { return {f[0], f[1], f[2]}; }

// Does pc-w\music\<name>\<name>.mus exist? Cached: the same names recur in every unit of a chapter.
// ponytail: process-wide cache keyed by name, not per bigfile (one install per process).
bool isMusic(const cdc::BigfileSet& bigfile, const std::string& name) {
    static std::map<std::string, bool> cache;
    const auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    return cache[name] = bigfile.read("pc-w\\music\\" + name + "\\" + name + ".mus").has_value();
}
} // namespace

UnitData loadUnitData(const cdc::BigfileSet& bigfile, const std::string& name) {
    const std::string path = "pc-w\\" + name + ".drm";
    auto bytes = bigfile.read(path);
    if (!bytes) throw cdc::FormatError(path + " not in bigfile");
    auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)));
    UnitData u;
    u.name = name;
    u.col = collision::loadLevelCollision(*drm);
    const auto water = findWater(bigfile, *drm);
    u.col.markup.insert(u.col.markup.end(), water.begin(), water.end());
    auto unit = formats::parseUnit(*drm);
    u.portals = std::move(unit.portals);
    // tru.exe 0x5D40C0 picks the respawn intro among objects "checkpoint" / "invis_checkpoint".
    const auto list = bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    const auto names = list ? formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()})
                            : std::map<uint16_t, std::string>{};
    // tru.exe 0x5DE2B0: the level loads Lara as the object named by its settings DTP (GAMEPLAY.md "Outfits").
    if (const auto p = names.find(unit.playerObjectId); unit.playerObjectId && p != names.end()) u.playerObject = p->second;
    for (const auto& in : unit.intros) {
        const auto n = names.find(in.objectId);
        if (n == names.end()) continue;
        if (n->second == "checkpoint" || n->second == "invis_checkpoint")
            u.checkpoints.push_back({in.uniqueId, v3(in.position), in.rotation[2] - kPi / 2}); // heading as findSpawn
        if (const auto k = pickupKind(n->second)) u.pickups.push_back({in.uniqueId, *k, v3(in.position)});
        if (n->second == "motorcycle") u.vehicles.push_back({in.uniqueId, v3(in.position), in.rotation[2] - kPi / 2});
        if (n->second.find("_grapple") != std::string::npos) {
            const GrappleKind k = n->second.find("wall") != std::string::npos   ? GrappleKind::Wall
                                  : n->second.find("post") != std::string::npos ? GrappleKind::Post
                                  : n->second.find("ceiling") != std::string::npos ? GrappleKind::Ceiling
                                                                                : GrappleKind::Mounted;
            u.grapples.push_back({in.uniqueId, v3(in.position), k});
        }
        if (!in.scriptId) continue;
        const auto strings = scriptStrings(*drm, in.scriptId);
        std::vector<std::string> starts;
        for (const auto& s : strings)
            if (chapterIndex(s) >= 0) starts.push_back(s);
        if (!starts.empty() && n->second == "cinematic_object") u.chapterLinks[in.uniqueId] = std::move(starts);
        if (n->second == "cinematic_object")
            if (auto c = cine::introCinematic(*drm, in.scriptId); !c.empty()) u.cinematics[in.uniqueId] = std::move(c);
        for (const auto& s : strings)
            if (isMusic(bigfile, s)) {
                u.musicByIntro[in.uniqueId] = s;
                break;
            }
    }
    if (isMusic(bigfile, name + "_mus_01")) u.music = name + "_mus_01";
    u.spawn = findSpawn(bigfile, *drm);
    u.drm = std::move(drm);
    return u;
}

void World::registerCinematicUnits() {
    for (const char* name : {"l1_cine_ship_sinking", "l3_mansion_exterior_afterburn", "l6_ship_natla_room"}) {
        auto u = loader_(name);
        cinematicRefs_.push_back({u.name, u.drm, vec3(0)});
        for (const auto& [uid, stream] : u.cinematics) cinematicEnds_[uid] = {stream, u.chapterLinks[uid]};
    }
}

World::World(UnitLoader loader, const std::string& unit, AnimLibrary anims, std::optional<Spawn> spawn, Tuning tuning)
    : World(collision::CollisionMesh{}, std::move(anims), Spawn{}, tuning) {
    loader_ = std::move(loader);
    registerCinematicUnits();
    startUnit_ = unit;
    startSpawn_ = spawn;
    enterUnit(unit, spawn);
}

LoadedUnit* World::findUnit(const std::string& name) {
    for (auto& u : units_)
        if (u.data.name == name) return &u;
    return nullptr;
}

void World::enterUnit(const std::string& unit, std::optional<Spawn> spawn, int32_t checkpointId) {
    LoadedUnit base{loader_(unit), vec3(0), 0}; // may throw: keep the current stream until it succeeds
    if (!spawn)
        for (const auto& c : base.data.checkpoints)
            if (c.id == checkpointId) spawn = Spawn{c.pos, c.yaw, true};
    if (!spawn) spawn = base.data.spawn;
    if (!spawn) throw cdc::FormatError(unit + ": no intro to spawn at");
    // Level load (also respawns, saves, chapters): Lara becomes the unit's outfit object; streaming keeps hers.
    if (!base.data.playerObject.empty()) outfit = base.data.playerObject;
    units_.clear();
    units_.push_back(std::move(base));
    current_ = 0;
    player_.restore(PlayerState{}); // the old ledge index means nothing in the new stream
    streamAround(0);
    player_.spawn(spawn->pos, spawn->yaw);
    prev_ = player_.state();
    cam_.yaw = spawn->yaw;
    cam_.pitch = CameraState{}.pitch;
    updateCamera({}, 0);
    prevCam_ = cam_;
    water_.reset(); // no stale ripples across a teleport
    air_ = 1.0f, submerged_ = false;
    updatePose(0, true);
}

void World::respawn() {
    if (units_.empty()) { // single-mesh world: back to its spawn
        player_.spawn(startSpawn_->pos, startSpawn_->yaw);
        prev_ = player_.state();
        updatePose(0, true);
    } else if (checkpoint_) {
        enterUnit(checkpoint_->unit, std::nullopt, checkpoint_->id);
    } else {
        enterUnit(startUnit_, startSpawn_);
    }
    health_.reset(); // tru.exe 0x700060 on the restored player
    air_ = 1.0f, submerged_ = false;
    combat_ = {};
    enemies_.reset();
}

// tru.exe STREAM_LevelLoadAdjacentUnits (0x5D2940) keeps the portal neighbours of the player's unit loaded;
// a newly loaded unit is relocated by 0x5D13E0: for its portal P back to a loaded unit L, the portal Q of L
// with Q.signal == P.toSignal gives offset = Q.min - P.min (applied by RelocateUnit 0x5C6210 via 0x5D10D0).
void World::streamAround(size_t center) {
    // Markup indices into the merged mesh change below; keep (unit, local index) and map back.
    const auto ledgeOwner = markupOwner(player_.state().ledge), waterOwner = markupOwner(player_.state().water);
    const std::string centerName = units_[center].data.name;
    std::vector<std::string> keep{centerName};
    for (const auto& p : units_[center].data.portals) keep.push_back(p.toUnit); // copy: units_ grows below
    for (size_t k = 1; k < keep.size(); ++k) {
        const std::string& name = keep[k];
        if (findUnit(name)) continue;
        UnitData d;
        try {
            d = loader_(name);
        } catch (const std::exception& e) {
            warnings.push_back("stream " + name + ": " + e.what());
            continue;
        }
        const LoadedUnit& from = *findUnit(centerName);
        std::optional<vec3> offset;
        for (const auto& p : d.portals)
            for (const auto& q : from.data.portals)
                if (!offset && p.toUnit == centerName && q.toUnit == name && q.signal == p.toSignal)
                    offset = from.offset + v3(q.min) - v3(p.min);
        if (!offset) {
            warnings.push_back("stream " + name + ": no portal pair with " + centerName);
            continue;
        }
        units_.push_back({std::move(d), *offset, 0});
    }
    std::erase_if(units_, [&](const LoadedUnit& u) { return std::find(keep.begin(), keep.end(), u.data.name) == keep.end(); });
    for (size_t i = 0; i < units_.size(); ++i)
        if (units_[i].data.name == centerName) current_ = i;
    rebuildCollision();
    for (const auto& u : units_) {
        for (const auto& [uid, chapters] : u.data.chapterLinks) cinematicEnds_[uid].chapters = chapters;
        for (const auto& [uid, stream] : u.data.cinematics) cinematicEnds_[uid].stream = stream;
    }
    PlayerState s = player_.state();
    s.ledge = ledgeOwner ? markupIndex(ledgeOwner->first, ledgeOwner->second) : -1;
    s.water = waterOwner ? markupIndex(waterOwner->first, waterOwner->second) : -1; // its unit is Lara's or a neighbour: kept
    if (s.ledge < 0 && (s.state == State::Hang || s.state == State::Shimmy || s.state == State::ClimbUp)) s.state = State::Fall;
    player_.restore(s);
    syncVehicles();
    ++unitsVersion_;
}

void World::rebuildCollision() {
    col_.vertices.clear(), col_.faces.clear(), col_.markup.clear();
    col_.clearTerrainGroups();
    for (auto& u : units_) {
        const auto& c = u.data.col;
        const auto vb = uint32_t(col_.vertices.size());
        const auto groupBase = col_.appendTerrainGroups(c);
        u.markupBase = col_.markup.size();
        for (const vec3& v : c.vertices) col_.vertices.push_back(v + u.offset);
        for (collision::Face f : c.faces) {
            for (auto& i : f.v) i += vb;
            f.group = uint16_t(f.group + groupBase);
            col_.faces.push_back(f);
        }
        for (collision::Markup m : c.markup) {
            for (auto& p : m.points) p += u.offset;
            if (disabledMarkup_.contains({u.data.name, m.id})) m.type = kMarkupDisabled;
            col_.markup.push_back(std::move(m));
        }
    }
    col_.build();
}

void World::crossPortals(vec3 from, vec3 to) {
    const vec3 a = from + vec3(0, 0, kPortalProbeHeight), b = to + vec3(0, 0, kPortalProbeHeight);
    const LoadedUnit& cur = units_[current_];
    for (const auto& p : cur.data.portals) {
        const vec3 n = v3(p.normal), q = v3(p.quad[0]) + cur.offset;
        const float d0 = glm::dot(a - q, n), d1 = glm::dot(b - q, n);
        if ((d0 > 0) == (d1 > 0) || d0 == d1) continue;
        const vec3 x = a + (b - a) * (d0 / (d0 - d1)) - cur.offset;
        if (glm::any(glm::lessThan(x, v3(p.min) - kPortalMargin)) || glm::any(glm::greaterThan(x, v3(p.max) + kPortalMargin))) continue;
        const std::string target = p.toUnit; // `p` dies with the unit list below
        if (!findUnit(target)) streamAround(current_); // a neighbour that failed before: retry (tru.exe blocks here)
        for (size_t i = 0; i < units_.size(); ++i)
            if (units_[i].data.name == target) {
                streamAround(i);
                return;
            }
        return;
    }
}

void World::touchCheckpoints() {
    for (const auto& e : script_.events())
        if (e.kind == script::EventKind::Checkpoint)
            if (const auto* o = script_.find(e.target)) {
                const CheckpointRef next{o->unit, o->uid};
                if (checkpoint_ != next) {
                    checkpoint_ = next;
                    enemies_.checkpoint();
                }
            }
    const vec3 p = player_.state().pos;
    for (const auto& u : units_)
        for (const auto& c : u.data.checkpoints) {
            const vec3 w = c.pos + u.offset;
            if (glm::length(vec2(p - w)) < kCheckpointRadius && std::abs(p.z - w.z) < kCheckpointHeight &&
                checkpoint_ != CheckpointRef{u.data.name, c.id}) {
                checkpoint_ = CheckpointRef{u.data.name, c.id};
                enemies_.checkpoint();
            }
        }
}

void World::startChapter(int n) {
    const auto unit = std::string(kChapters.at(size_t(n)).unit);
    enterUnit(unit, std::nullopt); // throws before anything changes
    chapter = n;
    startUnit_ = unit, startSpawn_.reset();
    checkpoint_.reset();
    health_.reset(); // [I] a chapter starts at full health, as a respawn (tru.exe 0x700060)
}

void World::collectPickups(bool pressed) {
    pickedUp_.clear();
    if (!pressed) return;
    const vec3 p = player_.state().pos;
    const Pickup* best = nullptr;
    vec3 bestPos{0};
    float bestD = kPickupRadius;
    for (const auto& u : units_)
        for (const auto& k : u.data.pickups) {
            const vec3 w = k.pos + u.offset;
            const float d = glm::length(vec2(p - w));
            if (d < bestD && std::abs(p.z - w.z) < kPickupHeight && !collected.contains(k.id)) best = &k, bestPos = w, bestD = d;
        }
    if (!best) return;
    collected.insert(best->id);
    ++found[size_t(best->kind)];
    pickedUp_.push_back({best->id, best->kind, bestPos});
}

void World::checkLevelEnd() {
    for (const auto& e : script_.events()) {
        if (e.kind != script::EventKind::Activate) continue;
        const auto end = cinematicEnds_.find(e.target);
        if (end == cinematicEnds_.end()) continue;
        const auto next = nextChapter(chapter, end->second.chapters);
        if (!end->second.stream.empty() && onCinematic) onCinematic(end->second.stream, next.has_value());
        script_.endCinematic(e.target);
        if (next) {
            try {
                startChapter(*next);
            } catch (const std::exception& ex) {
                warnings.push_back(std::string("level end -> chapter ") + std::to_string(*next) + ": " + ex.what());
            }
            return;
        }
    }
}

std::optional<std::pair<std::string, int>> World::markupOwner(int index) const {
    for (const auto& u : units_)
        if (index >= int(u.markupBase) && index < int(u.markupBase + u.data.col.markup.size()))
            return std::pair{u.data.name, index - int(u.markupBase)};
    return std::nullopt;
}

int World::markupIndex(const std::string& unit, int local) const {
    for (const auto& u : units_)
        if (u.data.name == unit && local >= 0 && size_t(local) < u.data.col.markup.size()) return int(u.markupBase) + local;
    return -1;
}

void World::restore(const PlayerState& s, float camYaw, float camPitch) {
    player_.restore(s);
    health_.reset(); // full; save::apply then sets the saved health (setHealth)
    prev_ = s;
    cam_.yaw = camYaw;
    cam_.pitch = camPitch;
    updateCamera({}, 0);
    prevCam_ = cam_;
    updatePose(0, true);
}

} // namespace ou::game
