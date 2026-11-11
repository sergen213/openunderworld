// Unit streaming (portal neighbours relocated into one world frame), portal crossing, checkpoints,
// respawn and the state hooks saves use. Evidence: docs/formats/LEVELS.md "Streaming", docs/SAVES.md.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"
#include "script/Script.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
// [I] The game crosses portals when the instance's collision hits the unit's signal faces (0x5D2CA0 ->
// 0x5D2AA0); we test Lara's path at mid-body height against the portal quad's plane and bounds.
constexpr float kPortalProbeHeight = 250.0f, kPortalMargin = 50.0f;
// Checkpoints activate by script (checkpointbase 3 -> tru.exe message 0x196 / 1, World::touchCheckpoints). [I] Beside
// that, touching one activates it: feet within this cylinder around the checkpoint intro.
constexpr float kCheckpointRadius = 250.0f, kCheckpointHeight = 400.0f;

vec3 v3(const float* f) { return {f[0], f[1], f[2]}; }

// Does intro script type `id` derive from native "uberobjectbase"? Types live in the unit or the object's DRM
// (`objectDrm`, null when missing).
// [O] the movable grapple targets (levers, shiva arms, the grapple block, the snake head) and the swivelling grapple
// handles (`sa_ei_grapple_wall*`, below) are script type 3565's subclasses under uberobjectbase; the fixed anchors
// derive from type 3941.
bool uberScript(const cdc::Drm& unit, const cdc::Drm* objectDrm, uint32_t id) {
    for (int depth = 0; id && depth < 16; ++depth) {
        bool found = false;
        for (const cdc::Drm* d : std::initializer_list<const cdc::Drm*>{&unit, objectDrm}) {
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
// ponytail: process-wide caches keyed by name, not per bigfile (one install per process); one lock for both, held
// over the read (the background streamer and the main thread load units).
std::mutex cacheMutex;
bool isMusic(const cdc::BigfileSet& bigfile, const std::string& name) {
    const std::lock_guard lock(cacheMutex);
    static std::map<std::string, bool> cache;
    const auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    return cache[name] = bigfile.read("pc-w\\music\\" + name + "\\" + name + ".mus").has_value();
}

// Bend data of object `name` (nullptr: none). Cached as isMusic.
std::shared_ptr<const BendType> bendType(const cdc::BigfileSet& bigfile, const std::string& name) {
    const std::lock_guard lock(cacheMutex);
    static std::map<std::string, std::shared_ptr<const BendType>> cache;
    if (const auto it = cache.find(name); it != cache.end()) return it->second;
    std::shared_ptr<const BendType> t;
    if (auto b = bigfile.read("pc-w\\" + name + ".drm")) try { t = loadBendType(cdc::parseDrm(std::move(*b))); } catch (const cdc::FormatError&) {}
    return cache[name] = t;
}

// Grapple point of object `name`: the first model feature with mask bit physics::kFeatureGrapple (tru.exe's grapple
// candidates are these features, 0x7131A0 -> 0x6ECCA0; docs/GAMEPLAY.md "Grapple"), in the object's model space.
// `drm` is kept for objects that have one (their script types). Cached as isMusic.
struct GrappleObject {
    std::shared_ptr<const cdc::Drm> drm;
    vec3 point{0};
    int model = 0;
    bool pullable = false; // an uber object whose state machine watches a joint (trigger 14 / 15), or a class-34 rigid body
    bool anchor = false;   // an uber object that watches none: something to swing on
};
std::map<std::string, int> objectClasses; // physClass by object name, filled by grappleObject (under cacheMutex)
std::shared_ptr<const GrappleObject> grappleObject(const cdc::BigfileSet& bigfile, const std::string& name) {
    const std::lock_guard lock(cacheMutex);
    static std::map<std::string, std::shared_ptr<const GrappleObject>> cache;
    if (const auto it = cache.find(name); it != cache.end()) return it->second;
    std::shared_ptr<GrappleObject> g;
    if (auto b = bigfile.read("pc-w\\" + name + ".drm")) try {
            auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*b)));
            const size_t models = formats::objectRenderModels(*drm).size();
            objectClasses[name] = physics::loadObjectPhysics(*drm, 0).physClass;
            for (uint32_t m = 0; m < models && !g; ++m) {
                const physics::ObjectPhysics p = physics::loadObjectPhysics(*drm, m);
                for (const auto& f : p.features) {
                    if (!(f.mask & physics::kFeatureGrapple)) continue;
                    g = std::make_shared<GrappleObject>();
                    const glm::mat4 seg = size_t(f.segment) < p.segments.size() ? p.segments[size_t(f.segment)] : glm::mat4(1);
                    g->point = vec3(seg * glm::vec4(f.pos, 1)), g->model = int(m);
                    g->pullable = p.physClass == physics::kClassRigidBody;
                    break;
                }
            }
            if (g) {
                std::optional<formats::UberObjectData> uber;
                try { uber = formats::loadUberObject(*drm); } catch (const cdc::FormatError&) {}
                if (uber) {
                    g->anchor = std::ranges::none_of(uber->sections, [](const auto& s) {
                        return std::ranges::any_of(s.transitions, [](const auto& t) { return t.trigger == 14 || t.trigger == 15; });
                    });
                    g->pullable = !g->anchor;
                }
                g->drm = std::move(drm);
            }
        } catch (const cdc::FormatError&) { g.reset(); }
    return cache[name] = g;
}
// Is object `name` a class-30 breakable (game/Breakables.hpp)? After grappleObject(name).
bool breakableObject(const std::string& name) {
    const std::lock_guard lock(cacheMutex);
    const auto it = objectClasses.find(name);
    return it != objectClasses.end() && it->second == kClassBreakable;
}
} // namespace

// One worker thread running a UnitLoader for queued unit names, FIFO; at most kMaxQueued wait (more are loaded by take
// on the calling thread). Results are handed out once.
class UnitStreamer {
public:
    static constexpr size_t kMaxQueued = 8;
    explicit UnitStreamer(UnitLoader loader) : loader_(std::move(loader)), worker_([this] { run(); }) {}
    ~UnitStreamer() {
        { const std::lock_guard lock(m_); stop_ = true; }
        cv_.notify_one();
        worker_.join();
    }
    void request(const std::string& name) {
        const std::lock_guard lock(m_);
        if (results_.contains(name) || queue_.size() >= kMaxQueued) return;
        queue_.emplace_back(name, std::promise<UnitData>{});
        results_[name] = queue_.back().second.get_future();
        cv_.notify_one();
    }
    void cancel(const std::string& name) { // a load already running finishes unseen
        const std::lock_guard lock(m_);
        results_.erase(name);
        std::erase_if(queue_, [&](const auto& j) { return j.first == name; });
    }
    UnitData take(const std::string& name) { // waits; rethrows what the loader threw
        std::future<UnitData> f;
        {
            const std::lock_guard lock(m_);
            const auto it = results_.find(name);
            if (it == results_.end()) return loader_(name); // never queued (the queue was full)
            f = std::move(it->second);
            results_.erase(it);
        }
        return f.get();
    }

private:
    void run() {
        for (;;) {
            std::pair<std::string, std::promise<UnitData>> job;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                job.second.set_value(loader_(job.first));
            } catch (...) {
                job.second.set_exception(std::current_exception());
            }
        }
    }
    UnitLoader loader_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::promise<UnitData>>> queue_;
    std::map<std::string, std::future<UnitData>> results_;
    bool stop_ = false;
    std::thread worker_; // last: starts after the members it uses
};

// tru.exe 0x5D13E0: (1) a portal P of the new unit to `from` and the portal Q of `from` whose signal is P.toSignal,
// else (2) a portal Q of `from` to the new unit and the new unit's portal P whose signal is Q.toSignal; offset =
// Q.min - P.min (the first match; 0x5D10D0 relocates). Neither step checks the other portal's target [O]: it is what
// pairs l2_cliffhall_b and l2_hubhall, whose portals to each other name signals of portals to l2_junglehub (LEVELS.md
// "Streaming").
std::optional<vec3> relocation(const UnitData& unit, const LoadedUnit& from) {
    for (const auto& p : unit.portals)
        if (p.toUnit == from.data.name)
            for (const auto& q : from.data.portals)
                if (q.signal == p.toSignal) return from.offset + v3(q.min) - v3(p.min);
    for (const auto& q : from.data.portals)
        if (q.toUnit == unit.name)
            for (const auto& p : unit.portals)
                if (p.signal == q.toSignal) return from.offset + v3(q.min) - v3(p.min);
    return std::nullopt;
}

float portalBlendWeight(const formats::Portal& p, vec3 offset, vec3 pos) {
    const float r = p.radius;
    if (r <= 0) return 0;
    for (int i = 0; i < 3; ++i)
        if (!(p.min[i] + offset[i] - r < pos[i] && pos[i] < p.max[i] + offset[i] + r)) return 0;
    const vec3 n = v3(p.normal);
    vec3 q[4];
    for (int c = 0; c < 4; ++c) q[c] = v3(p.quad[c]) + offset;
    const vec3 on = pos - (glm::dot(n, pos) - glm::dot(n, q[0])) * n; // 0x49F290: the plane point under pos
    float d = glm::distance(pos, on);
    if (d >= r) return 0;
    // 0x5C7690 (the quad as two triangles, the normal's largest axis dropped); outside: the nearest edge (0x4A03B0).
    const vec3 a = glm::abs(n);
    const int u = a.x > a.y && a.x > a.z ? 1 : 0, v = a.z >= a.x && a.z >= a.y ? 1 : 2;
    const auto cross = [&](vec3 o, vec3 b, vec3 c) { return (b[u] - o[u]) * (c[v] - o[v]) - (b[v] - o[v]) * (c[u] - o[u]); };
    const auto inTriangle = [&](vec3 x, vec3 y, vec3 z) {
        const float s0 = cross(x, y, on), s1 = cross(y, z, on), s2 = cross(z, x, on);
        return (s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0);
    };
    if (!inTriangle(q[0], q[1], q[2]) && !inTriangle(q[0], q[2], q[3])) {
        d = r + 1;
        for (int c = 0; c < 4; ++c) {
            const vec3 e = q[(c + 1) % 4] - q[c];
            const float len2 = glm::dot(e, e), t = len2 > 0 ? std::clamp(glm::dot(pos - q[c], e) / len2, 0.0f, 1.0f) : 0.0f;
            d = std::min(d, glm::distance(pos, q[c] + t * e));
        }
    }
    return d < r ? (r - d) / r : 0.0f;
}

void blendUnitLook(scene::Environment& out, const scene::Environment& own,
                   std::span<const std::pair<const scene::Environment*, float>> others) {
    const auto fields = [](scene::Environment& e, const scene::Environment& s, auto&& f) {
        f(e.fogColor, s.fogColor, 3), f(&e.fogNear, &s.fogNear, 1), f(&e.fogFar, &s.fogFar, 1);
        f(e.waterFogColor, s.waterFogColor, 3), f(&e.waterFogNear, &s.waterFogNear, 1), f(&e.waterFogFar, &s.waterFogFar, 1);
        f(&e.farPlane, &s.farPlane, 1), f(e.ambient, s.ambient, 3), f(e.pssmSplits, s.pssmSplits, 4);
    };
    out.valid = own.valid;
    fields(out, own, [](float* d, const float* s, int n) { std::copy(s, s + n, d); });
    float sum = 1;
    for (const auto& [o, w] : others) {
        if (!o || !o->valid || !(w > 0)) continue;
        sum += w;
        fields(out, *o, [w](float* d, const float* s, int n) { for (int k = 0; k < n; ++k) d[k] += w * s[k]; });
    }
    fields(out, own, [sum](float* d, const float*, int n) { for (int k = 0; k < n; ++k) d[k] /= sum; });
}

void unitLook(const World& w, const std::function<const scene::Environment*(const std::string&)>& environment,
              scene::Environment& out) {
    const LoadedUnit& cur = w.currentUnit();
    const scene::Environment* own = environment(cur.data.name);
    if (!own) return void(out.valid = false);
    std::pair<const scene::Environment*, float> others[16];
    size_t n = 0;
    const vec3 at = w.player().state().pos;
    for (const auto& p : cur.data.portals) {
        if (n == std::size(others)) break;
        const float k = portalBlendWeight(p, cur.offset, at);
        if (k <= 0) continue;
        const bool loaded = std::any_of(w.units().begin(), w.units().end(), [&](const LoadedUnit& u) { return u.data.name == p.toUnit; });
        if (loaded) others[n++] = {environment(p.toUnit), k};
    }
    blendUnitLook(out, *own, std::span(others, n));
}

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
    u.rainMap = std::move(unit.rainMap);
    u.levelFlags = unit.flags;
    // tru.exe 0x5D40C0 picks the respawn intro among objects "checkpoint" / "invis_checkpoint".
    const auto list = bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    const auto names = list ? formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()})
                            : std::map<uint16_t, std::string>{};
    // tru.exe 0x5DE2B0: the level loads Lara as the object named by its settings DTP (GAMEPLAY.md "Outfits").
    if (const auto p = names.find(unit.playerObjectId); unit.playerObjectId && p != names.end()) u.playerObject = p->second;
    if (const auto p = names.find(unit.weatherObjectId); unit.weatherObjectId && p != names.end()) u.weatherObject = p->second;
    // The reward table index of a reward intro (treasure, urn, relic): intro data +0x30 [O: every one of the 185 has
    // its own, 1..205, and the table lists it in a treasure or relic group; health packs carry no data]. The born
    // treasure records it through its parent (0x4D3B50 hands the urn's intro to the birth, message 0x1E8).
    const auto rewardIndex = [&](const formats::Intro& in) -> uint16_t {
        if (!in.data || in.data->section >= drm->sections.size()) return 0;
        const auto& d = drm->sections[in.data->section].data;
        uint32_t v = 0;
        if (size_t(in.data->offset) + 0x34 <= d.size()) std::memcpy(&v, d.data() + in.data->offset + 0x30, 4);
        return v < 2048 ? uint16_t(v) : uint16_t(0);
    };
    // What the reward urns give birth to when they break (formats::birthIntros): a treasure at the urn's marker.
    std::map<int32_t, const formats::Intro*> byUid;
    for (const auto& in : unit.intros) byUid[in.uniqueId] = &in;
    for (auto& b : formats::birthIntros(bigfile, unit, names)) {
        const auto n = names.find(b.intro.objectId);
        const auto parent = byUid.find(b.parent);
        if (const auto k = n != names.end() ? pickupKind(n->second) : std::nullopt; k && parent != byUid.end())
            u.pickups.push_back({b.intro.uniqueId, *k, v3(b.intro.position), b.parent, rewardIndex(*parent->second)});
        u.births.push_back(std::move(b));
    }
    for (const auto& in : unit.intros) {
        u.markers.push_back({in.uniqueId, in.marker, v3(in.position)});
        const auto n = names.find(in.objectId);
        if (n == names.end()) continue;
        if (n->second == "checkpoint" || n->second == "invis_checkpoint")
            u.checkpoints.push_back({in.uniqueId, v3(in.position), in.rotation[2] - kPi / 2}); // heading as findSpawn
        if (const auto k = pickupKind(n->second)) u.pickups.push_back({in.uniqueId, *k, v3(in.position), 0, rewardIndex(in)});
        if (ammoWeapon(n->second)) u.weaponPickups.push_back({in.uniqueId, n->second, v3(in.position)});
        if (n->second == "motorcycle") u.vehicles.push_back({in.uniqueId, v3(in.position), in.rotation[2] - kPi / 2});
        if (n->second.starts_with("bend_"))
            if (auto t = bendType(bigfile, n->second))
                u.bends.push_back({in.uniqueId, v3(in.position), v3(in.rotation), std::move(t)});
        if (const auto g = n->second.starts_with("fx_") ? nullptr : grappleObject(bigfile, n->second)) {
            // Pull targets [O data]: their uber state machines react to the pulled joint (a trigger 14 / 15 range watch
            // transition: levers, shiva arms, the snake heads, the grapple block). The class-79 grapple handles
            // (`*_ei_grapple_wall*`, `sa_ei_grapple_mounted`) only swivel on a free hinge (section 0 = model 1 on
            // section 1, +-50 degrees; the naga statue's 0..120): anchors to swing / wall-run on, whatever their script.
            // Objects that are not `*_grapple*` anchors by name (ma_uo_bedroom_door, ms_rb_tomb_gate, the tentacle
            // gears, ...) are pull targets when they watch a joint or are rigid bodies, else anchors.
            const bool named = n->second.find("_grapple") != std::string::npos;
            const bool anchor = g->anchor;
            const GrappleKind k = !named ? (g->pullable ? GrappleKind::Mounted : GrappleKind::Ceiling)
                                  : !anchor && in.scriptId && uberScript(*drm, g->drm.get(), in.scriptId)
                                      ? GrappleKind::Mounted
                                  : n->second.find("wall") != std::string::npos ? GrappleKind::Wall
                                  : n->second.find("post") != std::string::npos ? GrappleKind::Post
                                  : n->second.find("ceiling") != std::string::npos || anchor ? GrappleKind::Ceiling
                                                                                             : GrappleKind::Mounted;
            // The target point: the feature (0x6FF040: the instance's origin + its query-0xDB offset). The named
            // anchors keep their intro origin [I: every rope / swing replay was recorded on it; their features lie
            // 118..261 along the anchor's axis, the handles' 214 out on the hinged handle].
            float im[16];
            formats::introMatrix(in, im);
            const vec3 at = named ? v3(in.position) : vec3(glm::make_mat4(im) * glm::vec4(g->point, 1));
            GrappleTarget t{in.uniqueId, at, k};
            t.model = g->model;
            // its intro data's 0x404 block: flag +4, own throw range +8 and longest rope +12 (0x698A70) [O]
            if (in.data && in.data->section < drm->sections.size()) {
                const auto& d = drm->sections[in.data->section].data;
                uint32_t tag = 0;
                if (const size_t o = in.data->offset; o + 16 <= d.size() && (std::memcpy(&tag, &d[o], 4), tag == 0x404) && d[o + 4])
                    std::memcpy(&t.range, &d[o + 8], 4), std::memcpy(&t.rope, &d[o + 12], 4);
            }
            u.grapples.push_back(t);
        }
        if (!n->second.starts_with("fx_") && !formats::dormant(in) && breakableObject(n->second)) {
            float im[16];
            formats::introMatrix(in, im);
            u.breakables.push_back({in.uniqueId, n->second, glm::make_mat4(im)});
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
        cinematicRefs_.push_back({u.name, u.drm, vec3(0), true}); // resident: their unit scripts wait for a real load
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

void World::setAsyncStreaming(UnitLoader background) {
    joinStreams(true); // the old streamer's queue
    streamer_ = background ? std::make_shared<UnitStreamer>(std::move(background)) : nullptr;
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
    // Level load (also respawns, saves, chapters): Lara becomes the unit's outfit object, or the chosen costume;
    // streaming keeps hers.
    if (!base.data.playerObject.empty()) levelOutfit = base.data.playerObject;
    if (!costume.empty() || !base.data.playerObject.empty()) wear(costume);
    for (const auto& p : pending_) streamer_->cancel(p.name);
    pending_.clear();
    units_.clear();
    units_.push_back(std::move(base));
    current_ = 0;
    playerUnit_.clear(); // a level load: the unit scripts get vt 13 (World::tick)
    bikeEntryDue_ = true; // ... and the chapter's motorcycle is found there (World::bikeEntry, once the scripts follow)
    PlayerState fresh; // the old ledge index means nothing in the new stream
    fresh.buttons.hold(); // a key still down gives no press edge on the first tick
    player_.restore(fresh);
    streamAround(0, true);
    joinStreams(true); // a level load waits for the neighbours
    player_.spawn(spawn->pos, spawn->yaw);
    exteriorOrigin_ = spawn->pos;
    prev_ = player_.state();
    cam_.yaw = spawn->yaw;
    cam_.pitch = CameraState{}.pitch;
    updateCamera({}, 0);
    prevCam_ = cam_;
    water_.reset(); // no stale ripples across a teleport
    air_ = 1.0f, submerged_ = false;
    updatePose(0, true);
}

// tru.exe [O]: DeathState's wipe done -> 0x6F55A0 sets game flag 0x80000 -> the game loop (0x5DAB40 case 6) calls
// "Restore Game" 0x557130: the committed save buffer is copied back over the working one, the script variable
// tables (0x5558A0) and the game state (0x556E90: clock, inventory, outfit, reverb) come back and the level loads
// again, every instance born from its saved block; 0x5D40C0 puts Lara on the checkpoint's intro and message 0x2A
// (0x700060) gives her full health.
void World::respawn() {
    // The secondary, clips, reserves and the meter stay; the hands are empty (a born player holds nothing).
    const auto holster = [&] {
        CombatState c;
        c.secondary = combat_.secondary, c.adrenaline = combat_.adrenaline, c.rng = combat_.rng;
        c.healthPacksUsed = combat_.healthPacksUsed;
        std::memcpy(c.clip, combat_.clip, sizeof c.clip);
        std::memcpy(c.reserve, combat_.reserve, sizeof c.reserve);
        c.spread = anims_.weapons[0].spreadDeg;
        combat_ = c;
    };
    if (units_.empty()) { // single-mesh world: back to its spawn, nothing to restore
        player_.spawn(startSpawn_->pos, startSpawn_->yaw);
        prev_ = player_.state();
        updatePose(0, true);
        holster();
        enemies_.reset();
    } else {
        const nlohmann::json save = checkpointSave_; // a copy: the restore below must not read what it replaces
        if (save.is_object()) setTicks(save.at("ticks").get<uint64_t>()); // before enterUnit, as a load (save::apply)
        if (checkpoint_) enterUnit(checkpoint_->unit, std::nullopt, checkpoint_->id);
        else enterUnit(startUnit_, startSpawn_);
        if (save.is_object()) {
            restoreCheckpointSave(save);
        } else { // she died within the first tick: nothing was saved yet
            holster();
            enemies_.reset();
        }
    }
    health_.reset(); // tru.exe 0x700060 on the born player
    air_ = 1.0f, submerged_ = false;
    checkpointDue_ = false;
    ++respawns_;
}

nlohmann::json World::serializeCheckpoint() const {
    nlohmann::json runtime = serializeRuntime();
    for (const char* lara : {"camera", "hair", "air", "submerged", "anim", "aimTime", "bones"}) runtime.erase(lara);
    return {{"ticks", ticks_},           {"collected", collected},       {"born", born},
            {"found", found},            {"grenades", grenades},         {"flashlight", flashlight},
            {"script", script_.serialize()}, {"enemies", enemies_.serialize()}, {"vehicles", serializeVehicles()},
            {"combat", serializeCombat()},   {"objects", serializeObjects()},   {"runtime", std::move(runtime)}};
}

void World::restoreCheckpointSave(const nlohmann::json& save) {
    collected = save.at("collected").get<std::set<int32_t>>();
    born = save.at("born").get<std::set<int32_t>>();
    found = save.at("found").get<std::array<int, 3>>();
    grenades = save.at("grenades").get<int>();
    flashlight = save.at("flashlight").get<bool>();
    script_.restore(save.at("script"));
    script_.playerJumped(); // she is at the intro now: no plane crossings across the jump
    restoreEnemies(save.at("enemies"));
    restoreVehicles(save.at("vehicles"));
    // Weapons, clips, reserves and the meter as saved; what her hands were doing then (draw, lock, reload, cooldowns)
    // and the health clocks are not: she is born again (restoreCombat without them holsters).
    nlohmann::json combat = save.at("combat");
    const int packsUsed = combat_.healthPacksUsed; // a count for logs: not rolled back
    combat.erase("state"), combat.erase("healthClocks");
    combat["weapon"] = weaponName(Weapon::Pistols);
    restoreCombat(combat);
    combat_.healthPacksUsed = packsUsed;
    restoreObjects(save.at("objects"));
    const nlohmann::json& runtime = save.at("runtime");
    restoreWorldRuntime(runtime);
    // The scripts were saved knowing her unit: only another unit is news to them (World::tick), not a level load.
    playerUnit_ = runtime.at("playerUnit").get<std::string>();
    syncMaxHealth(); // the relics among `collected`
}

// tru.exe STREAM_LevelLoadAdjacentUnits (0x5D2940) keeps the portal neighbours of the player's unit loaded; a newly
// loaded unit is relocated against it (relocation). With setAsyncStreaming the neighbours are queued: joinStreams.
// A crossing that adds and drops nothing changes only the current unit (no collision rebuild, no new render scene).
void World::streamAround(size_t center, bool fresh) {
    // Markup indices into the merged mesh change below; keep (unit, local index) and map back.
    const auto ledgeOwner = markupOwner(player_.state().ledge), waterOwner = markupOwner(player_.state().water);
    const std::string centerName = units_[center].data.name;
    std::vector<std::string> keep{centerName};
    for (const auto& p : units_[center].data.portals) keep.push_back(p.toUnit); // copy: units_ grows below
    const auto kept = [&](const std::string& name) { return std::find(keep.begin(), keep.end(), name) != keep.end(); };
    std::erase_if(pending_, [&](const PendingUnit& p) {
        if (kept(p.name)) return false;
        streamer_->cancel(p.name);
        return true;
    });
    for (size_t k = 1; k < keep.size(); ++k) {
        const std::string& name = keep[k];
        if (findUnit(name) || std::any_of(pending_.begin(), pending_.end(), [&](const PendingUnit& p) { return p.name == name; }))
            continue;
        if (streamer_) {
            streamer_->request(name);
            pending_.push_back({name, centerName, ticks_ + kStreamTicks});
            continue;
        }
        try {
            fresh |= attach(loader_(name), centerName);
        } catch (const std::exception& e) {
            warnings.push_back("stream " + name + ": " + e.what());
        }
    }
    fresh |= std::erase_if(units_, [&](const LoadedUnit& u) { return !kept(u.data.name); }) > 0;
    for (size_t i = 0; i < units_.size(); ++i)
        if (units_[i].data.name == centerName) current_ = i;
    if (fresh) unitsChanged(ledgeOwner, waterOwner);
}

// The safe point for background loads (start of World::tick, crossings, level loads): every neighbour due by this tick
// (all with `now`) joins, waiting for a load still running. Deterministic: which ones join depends on ticks only.
void World::joinStreams(bool now) {
    if (pending_.empty()) return;
    const auto ledgeOwner = markupOwner(player_.state().ledge), waterOwner = markupOwner(player_.state().water);
    bool changed = false;
    for (size_t i = 0; i < pending_.size();) {
        if (!now && pending_[i].due > ticks_) {
            ++i;
            continue;
        }
        const PendingUnit p = std::move(pending_[i]);
        pending_.erase(pending_.begin() + std::ptrdiff_t(i));
        try {
            changed |= attach(streamer_->take(p.name), p.from);
        } catch (const std::exception& e) {
            warnings.push_back("stream " + p.name + ": " + e.what());
        }
    }
    if (changed) unitsChanged(ledgeOwner, waterOwner);
}

bool World::attach(UnitData d, const std::string& from) {
    const LoadedUnit* base = findUnit(from);
    if (!base) base = &units_[current_]; // `from` streamed out while the load ran
    const auto offset = relocation(d, *base);
    if (!offset) {
        warnings.push_back("stream " + d.name + ": no portal pair with " + base->data.name);
        return false;
    }
    units_.push_back({std::move(d), *offset, 0});
    return true;
}

void World::unitsChanged(const std::optional<std::pair<std::string, int>>& ledgeOwner,
                         const std::optional<std::pair<std::string, int>>& waterOwner) {
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

// Each unit's collision keeps the BVH it was loaded with (built on the streamer's worker thread): the merge only copies
// and moves, then puts a top level over the units (CollisionMesh::appendPart). l4_staircase_mictlan joining
// pool_to_ragnarok: 8.3 ms for a BVH over every loaded face before.
void World::rebuildCollision() {
    col_.clearParts();
    col_.markup.clear();
    for (auto& [uid, in] : inserted_) in.markup = -1; // tickInserted adds the inserted poles' markups again
    objectMarkup_.clear();                            // tickObjectMarkup below: after every unit's
    for (auto& u : units_) {
        const auto& c = u.data.col;
        col_.appendPart(c, u.offset);
        u.markupBase = col_.markup.size();
        for (collision::Markup m : c.markup) {
            // Water boxes (findWater) carry {min, max, centre, half extents, rotation, waves, sink, source, flow}: the
            // first three and the two markers are positions.
            const bool box = m.type == 10 && m.points.size() >= 5;
            for (size_t i = 0; i < (box ? 3 : m.points.size()); ++i) m.points[i] += u.offset;
            if (box && m.points.size() > kWaterIds) {
                m.points[kWaterSink] += u.offset, m.points[kWaterSource] += u.offset;
                applyWaterState(m, u.offset);
            }
            if (m.camera)
                for (vec3& p : m.camera->pos) p += u.offset;
            if (disabledMarkup_.contains({u.data.name, m.id})) m.type = kMarkupDisabled;
            col_.markup.push_back(std::move(m));
        }
    }
    col_.finishParts();
    tickObjectMarkup(); // the colliders (col_.dynamics) survive the rebuild
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
        if (!findUnit(target)) joinStreams(true); // queued: wait for it (tru.exe blocks here too)
        if (!findUnit(target)) streamAround(current_), joinStreams(true); // a neighbour that failed before: retry
        for (size_t i = 0; i < units_.size(); ++i)
            if (units_[i].data.name == target) {
                streamAround(i);
                return;
            }
        return;
    }
}

void World::touchCheckpoints() {
    if (const auto e = script_.takeExperience()) // a script set a PLAYER TAILORING level (uiexperienceadjustmentmenu 6..10)
        if (const Tailoring t{e->levels[0], e->levels[1], e->levels[2], e->levels[3], e->levels[4]}; t != tailoring_) setTailoring(t);
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
                checkpointDue_ = true; // tru.exe 0x196 / 1: every activation saves (0x5570F0), the active one's too
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
                checkpointDue_ = true;
            }
        }
}

void World::startChapter(int n, std::optional<Weapon> chosen) {
    const auto unit = std::string(kChapters.at(size_t(n)).unit);
    enterUnit(unit, std::nullopt); // throws before anything changes
    chapter = n;
    startUnit_ = unit, startSpawn_.reset();
    checkpoint_.reset();
    checkpointSave_ = nullptr, checkpointDue_ = false; // the level start is saved when its first tick ends
    // tru.exe 0x71FFE0 message 0x23 -> 0x7198D0 [O]: a level start gives Lara the unit's loadout (level settings
    // +0x2B4 -> DTP: weapons and gadgets); no unit's lists g_healthpack, so she starts every level without packs.
    found[size_t(PickupKind::HealthPack)] = 0;
    health_.raiseMax(maxHealth()); // the relics kept (Treasure Hunt) or given up (New Game)
    health_.reset(); // [I] a chapter starts at full health, as a respawn (tru.exe 0x700060)
    script_.setPlayerObjects(chapterGear(n));
    chapterLoadout(n, chosen);
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
        if (formats::isBornUid(id) && !born.contains(id)) return false; // still inside its urn
        bestD = d, bestPos = w;
        return true;
    };
    // 0x6F5E70 takes rounds only while the gun is below its max reserve
    const auto room = [&](const std::string& object) {
        const auto w = ammoWeapon(object);
        return w && combat_.reserve[size_t(*w)] < maxReserve(*w) ? w : std::nullopt;
    };
    for (const auto& u : units_) {
        for (const auto& k : u.data.pickups) {
            // 0x6F5E70: a health pack is taken only while she carries fewer than the most she can
            if (k.kind == PickupKind::HealthPack && found[size_t(PickupKind::HealthPack)] >= kHealthPackMax) continue;
            if (nearer(k.pos + u.offset, k.id)) best = &k, ammoId = 0;
        }
        for (const auto& k : u.data.weaponPickups)
            if (const auto w = room(k.object); w && nearer(k.pos + u.offset, k.id)) best = nullptr, ammoId = k.id, ammo = *w;
    }
    // [I] an armed enemy's body gives its gun's rounds (the poachers' wp_akrifle). ponytail: keyed by the enemy's uid, so
    // a generator's spawns (sharing its uid) give once.
    for (const auto& e : enemies_.all())
        if (const auto w = e->state().mode == ai::Mode::Dead ? room(e->type().weaponObject) : std::nullopt; w && nearer(e->position(), e->uid()))
            best = nullptr, ammoId = e->uid(), ammo = *w;
    if (!ammoId && !best) return;
    // [O] lara.drm requests 49 gear_weapon_pickup, 98 gear_weapon_pickup_crouch. [I] She plays it where she stands,
    // facing what she takes; crouched, the crouch variant. The item is taken at once.
    const auto& s = player_.state();
    player_.use(s.state == State::Crouch ? "gear_weapon_pickup_crouch" : "gear_weapon_pickup", s.pos,
                std::atan2(bestPos.y - s.pos.y, bestPos.x - s.pos.x));
    if (ammoId) {
        collected.insert(ammoId);
        int& r = combat_.reserve[size_t(ammo)];
        r = std::min(maxReserve(ammo), r + anims_.weapons[size_t(ammo)].clip);
        return;
    }
    collected.insert(best->id);
    ++found[size_t(best->kind)];
    pickedUp_.push_back({best->id, best->kind, bestPos, best->parent, best->reward});
    // tru.exe 0x5DB6A0 [O]: a reward's first collection posts the active checkpoint its activation (0x196 / 1) again,
    // which saves the game as it is now: what she has found stays found after a death.
    if (checkpoint_ && best->kind != PickupKind::HealthPack) checkpointDue_ = true;
    if (best->kind == PickupKind::Relic) {
        // tru.exe 0x5DB6A0 [O]: the maximum rises with the count (health as it is), then the relic heals 300, to no more
        // than the new maximum + 50; health over the maximum decays (Health::tick).
        health_.raiseMax(maxHealth());
        health_.heal(std::max(0.0f, std::min(kRelicHeal, health_.max() + kRelicHeadroom - health_.value())), true);
    }
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
            if (onLevelChange) onLevelChange(*next);
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
    for (const auto& [key, i] : objectMarkup_)
        if (i == index) return std::pair{"@" + std::to_string(key.first), key.second};
    return std::nullopt;
}

int World::markupIndex(const std::string& unit, int local) const {
    if (unit.starts_with('@')) {
        const auto it = objectMarkup_.find({std::atoi(unit.c_str() + 1), local});
        return it == objectMarkup_.end() ? -1 : it->second;
    }
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
