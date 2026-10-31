// Unit streaming (portal neighbours relocated into one world frame), portal crossing, checkpoints,
// respawn and the state hooks saves use. Evidence: docs/formats/LEVELS.md "Streaming", docs/SAVES.md.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"
#include "script/Script.hpp"

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

// Does intro script type `id` derive from native "uberobjectbase"? Types live in the unit or the object's DRM.
// [O] the movable grapple targets (levers, the naga statue, shiva arms, the grapple block, the snake head, the uber
// wall slabs) are script type 3565's subclasses under uberobjectbase; the fixed anchors derive from type 3941.
bool uberScript(const cdc::BigfileSet& bigfile, const cdc::Drm& unit, const std::string& object, uint32_t id) {
    std::optional<cdc::Drm> objectDrm;
    if (auto b = bigfile.read("pc-w\\" + object + ".drm")) objectDrm = cdc::parseDrm(std::move(*b));
    for (int depth = 0; id && depth < 16; ++depth) {
        bool found = false;
        for (const cdc::Drm* d : std::initializer_list<const cdc::Drm*>{&unit, objectDrm ? &*objectDrm : nullptr}) {
            if (!d) continue;
            for (uint32_t i = 0; i < d->sections.size() && !found; ++i)
                if (d->sections[i].type == cdc::SectionType::Script && d->sections[i].id == id) {
                    script::ScriptType t;
                    try { t = script::parseScriptType(*d, i); } catch (const cdc::FormatError&) { return false; }
                    if (t.nativeName == "uberobjectbase") return true;
                    id = t.superId, found = true;
                }
            if (found) break;
        }
        if (!found) return false;
    }
    return false;
}

// Does pc-w\music\<name>\<name>.mus exist? Cached: the same names recur in every unit of a chapter.
// ponytail: process-wide cache keyed by name, not per bigfile (one install per process).
bool isMusic(const cdc::BigfileSet& bigfile, const std::string& name) {
    static std::map<std::string, bool> cache;
    const auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    return cache[name] = bigfile.read("pc-w\\music\\" + name + "\\" + name + ".mus").has_value();
}

// Bend data of object `name` (nullptr: none). ponytail: process-wide cache keyed by name, as isMusic.
std::shared_ptr<const BendType> bendType(const cdc::BigfileSet& bigfile, const std::string& name) {
    static std::map<std::string, std::shared_ptr<const BendType>> cache;
    if (const auto it = cache.find(name); it != cache.end()) return it->second;
    std::shared_ptr<const BendType> t;
    if (auto b = bigfile.read("pc-w\\" + name + ".drm")) try { t = loadBendType(cdc::parseDrm(std::move(*b))); } catch (const cdc::FormatError&) {}
    return cache[name] = t;
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
    u.flashlightSpot = unit.flashlightSpot;
    u.cameraSplines = std::move(unit.cameraSplines);
    u.wind = std::move(unit.wind);
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
        if (ammoWeapon(n->second)) u.weaponPickups.push_back({in.uniqueId, n->second, v3(in.position)});
        if (n->second == "motorcycle") u.vehicles.push_back({in.uniqueId, v3(in.position), in.rotation[2] - kPi / 2});
        if (n->second.starts_with("bend_"))
            if (auto t = bendType(bigfile, n->second))
                u.bends.push_back({in.uniqueId, v3(in.position), v3(in.rotation), std::move(t)});
        if (n->second.find("_grapple") != std::string::npos && !n->second.starts_with("fx_")) {
            const GrappleKind k = in.scriptId && uberScript(bigfile, *drm, n->second, in.scriptId) ? GrappleKind::Mounted
                                  : n->second.find("wall") != std::string::npos ? GrappleKind::Wall
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
    playerUnit_.clear(); // a level load: the unit scripts get vt 13 (World::tick)
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
            // Water boxes (findWater) carry {min, max, centre, half extents, rotation, waves}: only the first three are positions.
            const size_t positions = m.type == 10 && m.points.size() >= 5 ? 3 : m.points.size();
            for (size_t i = 0; i < positions; ++i) m.points[i] += u.offset;
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
    for (const auto& e : script_.events()) {
        if (e.kind == script::EventKind::Autosave) ++autosaveRequests_;
        if (e.kind == script::EventKind::Wetness) e.target ? wetness_.soak() : wetness_.reset();
        if (e.kind == script::EventKind::Checkpoint)
            if (const auto* o = script_.find(e.target)) {
                const CheckpointRef next{o->unit, o->uid};
                if (checkpoint_ != next) {
                    checkpoint_ = next;
                    enemies_.checkpoint();
                }
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
    script_.setPlayerObjects(chapterGear(n));
    chapterLoadout(n);
    announceChapter();
}

void World::recordReveal() {
    const LoadedUnit& u = currentUnit();
    const vec3 p = player_.state().pos - u.offset;
    auto& points = mapReveal[u.data.name];
    for (const vec3& q : points)
        if (glm::length(p - q) < kSonarRevealStep) return;
    points.push_back(p);
}

void World::collectPickups(bool pressed) {
    pickedUp_.clear();
    if (!pressed) return;
    const vec3 p = player_.state().pos;
    const Pickup* best = nullptr;
    vec3 bestPos{0};
    float bestD = kPickupRadius;
    int32_t ammoId = 0; // a weapon intro / armed body is nearest instead
    Weapon ammo = Weapon::Pistols;
    const auto nearer = [&](vec3 w, int32_t id) {
        const float d = glm::length(vec2(p - w));
        if (!(d < bestD && std::abs(p.z - w.z) < kPickupHeight && !collected.contains(id))) return false;
        bestD = d, bestPos = w;
        return true;
    };
    // 0x6F5E70 takes rounds only while the gun is below its max reserve
    const auto room = [&](const std::string& object) {
        const auto w = ammoWeapon(object);
        return w && combat_.reserve[size_t(*w)] < maxReserve(*w) ? w : std::nullopt;
    };
    for (const auto& u : units_) {
        for (const auto& k : u.data.pickups)
            if (nearer(k.pos + u.offset, k.id)) best = &k, ammoId = 0;
        for (const auto& k : u.data.weaponPickups)
            if (const auto w = room(k.object); w && nearer(k.pos + u.offset, k.id)) best = nullptr, ammoId = k.id, ammo = *w;
    }
    // [I] an armed enemy's body gives its gun's rounds (the poachers' wp_akrifle). ponytail: keyed by the enemy's uid, so
    // a generator's spawns (sharing its uid) give once.
    for (const auto& e : enemies_.all())
        if (const auto w = e->state().mode == ai::Mode::Dead ? room(e->type().weaponObject) : std::nullopt; w && nearer(e->position(), e->uid()))
            best = nullptr, ammoId = e->uid(), ammo = *w;
    if (ammoId) {
        collected.insert(ammoId);
        int& r = combat_.reserve[size_t(ammo)];
        r = std::min(maxReserve(ammo), r + anims_.weapons[size_t(ammo)].clip);
        return;
    }
    if (!best) return;
    collected.insert(best->id);
    ++found[size_t(best->kind)];
    pickedUp_.push_back({best->id, best->kind, bestPos});
}

void World::checkLevelEnd() {
    const auto events = script_.events(); // copy: the cinematic events' handlers add events
    std::set<int32_t> done; // two scripts may activate one cinematic in a tick (l1_seatop's 5789 instances -> 997)
    for (const auto& e : events) {
        if (e.kind != script::EventKind::Activate || !done.insert(e.target).second) continue;
        const auto end = cinematicEnds_.find(e.target);
        if (end == cinematicEnds_.end()) continue;
        const auto next = nextChapter(chapter, end->second.chapters);
        const std::string stream = end->second.stream;
        script_.cinematicEvent(e.target, 0);
        if (!stream.empty() && onCinematic) onCinematic(stream, next.has_value());
        if (!stream.empty() && !next) placeAfterCinematic(stream);
        script_.cinematicEvent(e.target, 1);
        for (const int32_t bike : script_.takeMounts()) mountBike(bike);
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

// The stream's header tails (tru.exe 0x455D90 places every instance that has one): position relative to the area
// marker of its end unit, heading from the tail rotation (intro yaw - 90 degrees, as findSpawn). Lara (intro -1) and
// the bikes. Lara's (0, 0, 0) tails (180_mex_intro, 230_val_intro) are none (0x49AFA0). A loaded unit whose water box
// holds her end becomes the current one (275_hel_explosion: under the ice, in l7_arctic_sea), else the marker's [I].
// A tail over no loaded floor or water leaves her where she was [I]: 120_man_vault's (-2704.7, -4321, -7.3) is in
// l3_main_hall, five portals from l3_caverns_start, whose player intro on the cavern stairs is where the original
// stands after loading the unit.
void World::placeAfterCinematic(const std::string& stream) {
    const auto* bigfile = script_.bigfile();
    const auto mul = bigfile ? bigfile->read("pc-w\\cinstream\\" + stream + ".mul") : std::nullopt;
    if (!mul) return;
    cine::Header h;
    try {
        h = cine::parseHeader(cine::demux(*mul));
    } catch (const std::exception& e) {
        warnings.push_back("cinematic " + stream + ": " + e.what());
        return;
    }
    struct Marker { vec3 pos; std::string unit; };
    std::map<uint32_t, Marker> markers; // stream unit id -> its loaded marker intro (world frame)
    for (const auto& u : units_)
        for (const auto& in : formats::parseUnit(*u.data.drm).intros)
            for (const auto& m : h.markers)
                if (m.intro == in.uniqueId && !markers.contains(m.unit)) markers[m.unit] = {v3(in.position) + u.offset, u.data.name};
    for (const auto& inst : h.instances) {
        if (!inst.endPos || !inst.endRot) continue;
        const auto m = markers.find(inst.endUnit);
        if (m == markers.end()) continue;
        const vec3 at = *inst.endPos + m->second.pos;
        const float yaw = inst.endRot->z - kPi / 2;
        if (inst.intro == -1) {
            if (*inst.endPos == vec3(0)) continue;
            // tru.exe under Wine (new game): 001_pro_corridor's tail holds Lara at (-4336, 5847) facing +x for the unit
            // script's 5 s corridor camera, then she stands on l3_main_bedroom's player intro 4456 facing down the corridor
            // (rot z 0), the camera behind her, and play starts. [I] a tail within 500 of the start unit's player intro is
            // that hand-over: she keeps the intro pose (enterUnit put her there).
            if (const auto& s = currentUnit().data.spawn; s && s->fromIntro && currentUnit().data.name == startUnit_ &&
                                                         glm::distance(at, s->pos + currentUnit().offset) < 500.0f)
                continue;
            std::string unit = m->second.unit;
            bool water = false;
            for (const auto& u : units_)
                for (const auto& w : u.data.col.markup)
                    if (w.type == 10 && insideWater(w, at - u.offset)) unit = u.data.name, water = true;
            if (!water && !col_.groundProbe(at + vec3(0, 0, 100), 20000)) continue;
            recentre(unit);
            auto s = player_.state();
            s.pos = at, s.yaw = yaw, s.vel = vec3(0);
            restore(s, yaw, cam_.pitch);
            script_.playerJumped();
        } else if (const auto b = bikes_.find(inst.intro); b != bikes_.end() && bikeMatrix(b->first, 1)) {
            b->second.pos = at - bikeOffset(b->second), b->second.yaw = yaw, b->second.pitch = 0;
            b->second.prevPos = at, b->second.prevYaw = yaw, b->second.prevPitch = 0;
        }
    }
}

void World::recentre(const std::string& unit) {
    for (size_t i = 0; i < units_.size(); ++i)
        if (units_[i].data.name == unit && i != current_) return streamAround(i);
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
