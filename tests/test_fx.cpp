// Original particle effects: definition parser, deterministic simulation, sprite batches (docs/formats/EFFECTS.md).
#include "formats/Formats.hpp"
#include "fx/Fx.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <algorithm>
#include <memory>

#include <glm/gtc/type_ptr.hpp>

using namespace ou;

namespace {
template <class T>
void put(std::vector<uint8_t>& d, size_t off, T v) { std::memcpy(d.data() + off, &v, sizeof v); }

// A torch-flame-like definition: rises, 4 keys, additive, 25-frame life, one particle per frame.
std::vector<uint8_t> flameDef() {
    std::vector<uint8_t> d(fx::kParticleDefSize, 0);
    put<float>(d, 0x00, 63.2f);           // key 0 size
    put<int16_t>(d, 0x0C, fx::kNoSegment);
    put<float>(d, 0x14 + 8, 9.5f);        // velocity z
    put<float>(d, 0x2C + 8, 0.79f);       // accel z
    put<uint32_t>(d, 0x38, 0x327C2502);   // key 0 colour
    put<uint32_t>(d, 0x3C, 0x000F1A80);   // last colour
    put<int16_t>(d, 0x44, -1);
    put<uint16_t>(d, 0x46, 25);
    put<float>(d, 0x48, -1.0f);           // one per 30 Hz frame
    put<float>(d, 0x80, 31.6f);           // last size
    for (size_t o : {0x94, 0x96, 0xAA}) put<int16_t>(d, o, fx::kNoSegment);
    put<uint32_t>(d, 0xB8, 0);            // key 1: no colour (interpolated) but a size
    put<float>(d, 0xBC, 110.6f);
    put<int16_t>(d, 0xCE, 33);
    put<uint32_t>(d, 0xC4, 0x80123E80);   // key 2
    put<float>(d, 0xC8, 79.0f);
    put<int16_t>(d, 0xD0, 66);
    for (int k = 0; k < 4; ++k) put<float>(d, 0xE0 + 4 * k, 1.0f);
    d[0x11C] = 2;                         // additive
    d[4] = 1;                             // starts at birth (0x543F70: +4 & 1)
    put<uint32_t>(d, 0x110, 766);
    return d;
}
std::shared_ptr<fx::ObjectFx> flameFx() {
    auto o = std::make_shared<fx::ObjectFx>();
    o->particles.push_back(fx::parseParticleDef(flameDef()));
    o->events.push_back({0x2001, 0, 0, {3, 0, 0, 0}});
    o->events.push_back({0x2005, 0, 0, {3, 0, 0, 0}}); // disabled (info & 4)
    return o;
}
} // namespace

TEST(Fx, ParsesKeysAndInterpolatesMiddleColour) {
    const auto p = fx::parseParticleDef(flameDef());
    ASSERT_EQ(p.keys.size(), 4u);
    EXPECT_FLOAT_EQ(p.keys[0].size, 63.0f); // ftol
    EXPECT_FLOAT_EQ(p.keys[1].size, 110.0f);
    EXPECT_FLOAT_EQ(p.keys[1].time, 0.33f);
    EXPECT_FLOAT_EQ(p.keys[2].time, 0.66f);
    EXPECT_FLOAT_EQ(p.keys[3].time, 1.0f);
    EXPECT_EQ(p.keys[0].rgba, 0x327C2502u);
    EXPECT_NE(p.keys[1].rgba, 0u); // filled between key 0 and key 2
    EXPECT_EQ(p.keys[1].rgba & 0xFF, 65u); // red halfway between 0x02 and 0x80 (33 / 66)
    EXPECT_EQ(p.life, 25);
    EXPECT_EQ(fx::blendFor(p.keys[0].blend), fx::Blend::Additive);
    EXPECT_EQ(fx::blendFor(1), fx::Blend::Alpha);
    EXPECT_EQ(fx::blendFor(4), fx::Blend::Subtract);
}

TEST(Fx, BirthSkipsDisabledEventsAndSimulatesDeterministically) {
    const auto o = flameFx();
    auto run = [&] {
        fx::System s;
        EXPECT_EQ(s.startBirth(o, glm::mat4(1), 7), 1);
        for (int t = 0; t < 300; ++t) s.tick({1000, 0, 0});
        return std::pair{s.checksum(), s.particleCount()};
    };
    const auto a = run(), b = run();
    EXPECT_EQ(a.first, b.first);
    // One spawn per 2 ticks, each lives 50 ticks: 25 alive in steady state.
    EXPECT_EQ(a.second, 25u);
}

TEST(Fx, RisesAndDrawsBatchedSprites) {
    fx::System s;
    ASSERT_TRUE(s.start(flameFx(), 0, glm::translate(glm::mat4(1), glm::vec3(0, 0, 100)), 1));
    for (int t = 0; t < 60; ++t) s.tick({0, 0, 0});
    render::Camera cam;
    cam.position = {-2000, 0, 200};
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    s.build(cam, 16.0f / 9, v, b);
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(v.size() % 6, 0u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].textureId, 766u);
    EXPECT_EQ(b[0].blend, uint8_t(fx::Blend::Additive));
    float top = -1e9f;
    for (const auto& x : v) top = std::max(top, x.pos[2]);
    EXPECT_GT(top, 200.0f); // flames rose above the emitter
    s.stopOwner(1);
    for (int t = 0; t < 60; ++t) s.tick({0, 0, 0});
    EXPECT_EQ(s.particleCount(), 0u);
    EXPECT_EQ(s.emitterCount(), 0u);
}

TEST(Fx, BurstEmitterSpawnsOnce) {
    auto d = flameDef();
    put<int16_t>(d, 0x44, 0);
    put<float>(d, 0x48, 12.0f);
    auto o = std::make_shared<fx::ObjectFx>();
    o->particles.push_back(fx::parseParticleDef(d));
    fx::System s;
    ASSERT_TRUE(s.start(o, 0, glm::mat4(1), 1));
    s.tick({});
    EXPECT_EQ(s.particleCount(), 12u);
    for (int t = 0; t < 10; ++t) s.tick({});
    EXPECT_EQ(s.particleCount(), 12u);
}

// 0x540B80 re-reads an emitter's segment from its instance every frame: started on a tracked owner, it spawns where the
// owner is now (swim_climbout's drips on Lara's head as she climbs onto the deck), not where it was started.
TEST(Fx, EmitterFollowsItsTrackedOwner) {
    fx::System s;
    ASSERT_TRUE(s.start(flameFx(), 0, glm::mat4(1), 1));
    s.track(1, glm::translate(glm::mat4(1), glm::vec3(5000, 0, 0)), {});
    for (int t = 0; t < 20; ++t) s.tick({});
    ASSERT_GT(s.particleCount(), 0u);
    render::Camera cam;
    cam.position = {3000, -2000, 200};
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    s.build(cam, 16.0f / 9, v, b);
    ASSERT_FALSE(v.empty());
    for (const auto& x : v) EXPECT_GT(x.pos[0], 4000.0f);
}

// def +0x05 & 1 (water rings): the sprite lies on the world XY plane (tru.exe 0x549500), not facing the camera.
TEST(Fx, FlatParticlesLieOnTheGround) {
    auto d = flameDef();
    put<float>(d, 0x14 + 8, 0.0f), put<float>(d, 0x2C + 8, 0.0f); // stays at z = 0
    d[5] = 1;
    auto o = std::make_shared<fx::ObjectFx>();
    o->particles.push_back(fx::parseParticleDef(d));
    ASSERT_TRUE(o->particles[0].flat);
    fx::System s;
    ASSERT_TRUE(s.start(o, 0, glm::mat4(1), 1));
    for (int t = 0; t < 10; ++t) s.tick({});
    render::Camera cam;
    cam.position = {-2000, 0, 200};
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    s.build(cam, 1.0f, v, b);
    ASSERT_FALSE(v.empty());
    for (const auto& x : v) EXPECT_NEAR(x.pos[2], 0.0f, 1e-3f);
}

// Anim-event records: material filter (info & 0x3F0), variant bits (0xC00) skipped, later frames fire as ticks pass.
TEST(Fx, AnimEventsFilterByMaterialAndPlayLater) {
    auto o = std::make_shared<fx::ObjectFx>();
    o->particles.push_back(fx::parseParticleDef(flameDef()));
    auto& ev = o->clips["idle"];
    ev.push_back({0x2001, 0, 0, {3, 0, 0, 0}}); // any material
    ev.push_back({0x2011, 0, 0, {3, 0, 0, 0}}); // material 1 only
    ev.push_back({0x2021, 3, 0, {3, 0, 0, 0}}); // material 2, frame 3
    ev.push_back({0x2801, 0, 0, {3, 0, 0, 0}}); // variant 2: never
    ev.push_back({0x2025, 0, 0, {3, 0, 0, 0}}); // disabled
    fx::System s;
    s.play(o, *o->clip("idle"), glm::mat4(1), 7, 2);
    EXPECT_EQ(s.emitterCount(), 1u); // the unfiltered record now
    for (int t = 0; t < 5; ++t) s.tick({});
    EXPECT_EQ(s.emitterCount(), 1u);
    s.tick({}); // tick 6 = frame 3
    EXPECT_EQ(s.emitterCount(), 2u);
    EXPECT_EQ(s.runEvents(o, ev, glm::mat4(1), 8, 1, 0, 1), 2); // material 1: unfiltered + its own
}

// Class 2 glows: growth (end / start - 1) / life per frame, fade-out over the last frames, kGlowTexture without a
// texture, additive unless flags >> 6 == 1.
TEST(Fx, GlowsGrowFadeAndDraw) {
    auto o = std::make_shared<fx::ObjectFx>();
    fx::GlowDef g;
    g.flags = 0x01, g.width = g.height = 10, g.endWidth = g.endHeight = 30, g.life = 4, g.fadeOut = 2;
    g.colours = {0x80808080};
    o->glows.push_back(g);
    o->events.push_back({0x2001, 0, 0, {6, 0, 0, 0}});
    fx::System s;
    EXPECT_EQ(s.startBirth(o, glm::mat4(1), 1), 1);
    render::Camera cam;
    cam.position = {-500, 0, 0};
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    s.build(cam, 1.0f, v, b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].textureId, fx::kGlowTexture);
    EXPECT_EQ(b[0].blend, uint8_t(fx::Blend::Additive));
    auto width = [&] { return glm::distance(glm::make_vec3(v[0].pos), glm::make_vec3(v[1].pos)); };
    const float w0 = width();
    for (int t = 0; t < 5; ++t) s.tick({-500, 0, 0});
    s.build(cam, 1.0f, v, b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_NEAR(width() / w0, 2.25f, 0.05f);        // 5 of 8 ticks: 10 -> 22.5 of 30
    EXPECT_LT(v[0].rgba & 0xFF, 0x80u);            // fading (RGB) in its last 2 frames
    for (int t = 0; t < 3; ++t) s.tick({-500, 0, 0});
    EXPECT_EQ(s.glowCount(), 0u);
}

// ---- Real data (skipped without OU_GAME_DIR) ---------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
std::optional<fx::ObjectFx> load(const cdc::BigfileSet& bf, const std::string& name) {
    auto b = bf.read("pc-w\\" + name + ".drm");
    if (!b) return std::nullopt;
    return fx::parseObjectFx(cdc::parseDrm(std::move(*b)));
}
} // namespace

TEST(FxReal, TorchDefinitionsAndBirthEffects) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto t = load(bf, "fx_torch_med_baldrs_tomb_ist");
    ASSERT_TRUE(t);
    ASSERT_EQ(t->particles.size(), 16u);
    const auto& flame = t->particles[4];
    EXPECT_EQ(flame.textures, (std::vector<uint32_t>{766, 767, 768, 769, 770, 771}));
    EXPECT_EQ(flame.life, 25);
    EXPECT_FLOAT_EQ(flame.rate, -1.0f);
    EXPECT_EQ(flame.keys.size(), 4u);
    EXPECT_EQ(t->particles[1].texture, 160u);
    int birth = 0;
    for (const auto& e : t->events) birth += e.effect() && e.event == 0 && e.cmd.type == 3;
    EXPECT_EQ(birth, 5); // particles 7, 2, 3, 6, 4; particle 0's record is disabled
    fx::System s;
    EXPECT_EQ(s.startBirth(std::make_shared<fx::ObjectFx>(*t), glm::mat4(1), 1), 5);
    for (int k = 0; k < 120; ++k) s.tick({500, 0, 0});
    EXPECT_GT(s.particleCount(), 20u);
}

// Splash-edge emitters run between two HInfo markers (segment fields -7 / -8, shape 7) with velocity in marker 5's frame.
TEST(FxReal, MarkerSegmentsResolve) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto t = load(bf, "fx_splash_edge_lrg_tha_junglehub_ist");
    ASSERT_TRUE(t);
    ASSERT_GE(t->particles.size(), 4u);
    EXPECT_EQ(t->particles[0].segment, -7);
    EXPECT_EQ(t->particles[0].segment2, -8);
    for (const int id : {5, 7, 8}) EXPECT_TRUE(t->markers.contains(id)) << id;
    const glm::vec3 a = t->frame(-7)[3], b = t->frame(-8)[3];
    EXPECT_GT(glm::length(a - b), 50.0f); // a line, not a point
}

TEST(FxReal, EveryFxObjectParses) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
    ASSERT_TRUE(list);
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    int objects = 0, defs = 0, textured = 0, birth = 0;
    for (const auto& [id, name] : names) {
        if (!name.starts_with("fx")) continue;
        std::vector<std::string> w;
        auto b = bf.read("pc-w\\" + name + ".drm");
        if (!b) continue;
        const auto drm = cdc::parseDrm(std::move(*b));
        const auto f = fx::parseObjectFx(drm, &w);
        EXPECT_TRUE(w.empty()) << name;
        if (!f) continue;
        ++objects, defs += int(f->particles.size());
        for (const auto& e : f->events) birth += e.effect() && e.event == 0;
        std::set<uint32_t> have;
        for (const auto& s : drm.sections) if (s.type == cdc::SectionType::Texture) have.insert(s.id);
        for (const auto id : f->textureIds()) {
            EXPECT_TRUE(have.contains(id)) << name << " texture " << id;
            ++textured;
        }
    }
    EXPECT_EQ(objects, 205);  // fx* objects with an event list or GenericFXObject
    EXPECT_EQ(defs, 2011);    // particle definitions
    EXPECT_EQ(birth, 389);    // enabled event-0 effect records
    EXPECT_EQ(textured, 2266); // per-object distinct particle (incl. material) + glow / ribbon / lightning textures, all in the DRM
}

// Hitscan impacts: the ammo object names its hit objects (data +0x3C above water, +0xA8 under water; tru.exe 0x6C64D0
// -> 0x6B7030), whose "idle" clip events pick the particles by the hit surface's material (info & 0x3F0).
TEST(FxReal, BulletHitPicksParticlesBySurfaceMaterial) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    EXPECT_EQ(lib.dataU16("pr_handgunbullet", 0x3C), 53);  // bullet_hit
    EXPECT_EQ(lib.dataU16("pr_handgunbullet", 0xA8), 61);  // bullet_hit_underwater
    EXPECT_EQ(lib.dataU16("pr_shotgunbullet", 0x3C), 68);  // bullet_hit_shotgun
    const auto hit = lib.get("bullet_hit");
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->particles.size(), 161u);
    const auto* idle = hit->clip("idle");
    ASSERT_TRUE(idle);
    auto started = [&](uint8_t material) {
        std::set<int> defs;
        for (const auto& e : *idle)
            if (e.runsOn(material) && e.cmd.type == 3) defs.insert(e.cmd.a1);
        return defs;
    };
    EXPECT_EQ(started(1), (std::set<int>{61, 62, 63, 87, 88, 89, 90}));      // stone dust + unfiltered sparks
    EXPECT_TRUE(started(23).contains(23));                                   // flesh: dark-red blood
    EXPECT_EQ(hit->particles[23].keys[0].rgba & 0xFFFFFF, 0x07076Eu);
    EXPECT_TRUE(started(2).contains(109));                                   // water spray
    fx::System s;
    s.play(hit, *idle, glm::mat4(1), 1, 1);
    for (int t = 0; t < 4; ++t) s.tick({0, -500, 0});
    EXPECT_GT(s.particleCount(), 10u);
}

// Weapon "fire" clip events start the muzzle flash at the gun's HInfo markers (-10 muzzle, -20..-23 flash points).
TEST(FxReal, WeaponFireEventsUseMuzzleMarkers) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto gun = load(bf, "wp_handgun");
    ASSERT_TRUE(gun);
    const auto* fire = gun->clip("fire");
    ASSERT_TRUE(fire);
    std::set<int> defs;
    for (const auto& e : *fire)
        if (e.runsOn(1) && e.cmd.type == 3 && e.frame == 0) defs.insert(e.cmd.a1);
    for (const int d : {0, 1, 2, 3, 4, 5, 6, 7}) EXPECT_TRUE(defs.contains(d)) << d;
    for (const int id : {10, 20, 23}) EXPECT_TRUE(gun->markers.contains(id)) << id;
    EXPECT_GT(glm::length(glm::vec3(gun->frame(-10)[3]) - glm::vec3(gun->frame(-13)[3])), 5.0f); // muzzle != ejection port
    for (const auto* name : {"wp_shotgun", "wp_uzi", "wp_speargun"}) {
        const auto w = load(bf, name);
        ASSERT_TRUE(w) << name;
        EXPECT_TRUE(w->clip("fire")) << name;
    }
}

// Weapon fire: glow 1 at the flash marker (-23) cycling 3 colours, the muzzle light (marker -10, 300 units, 2 frames);
// fx_light_chapel_fill: an endless birth light with its light DTP (fixture).
TEST(FxReal, GlowsAndEffectLights) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto gun = lib.get("wp_handgun");
    ASSERT_TRUE(gun);
    ASSERT_EQ(gun->glows.size(), 3u);
    const auto& flash = gun->glows[1];
    EXPECT_EQ(flash.segment, -23);
    EXPECT_EQ(flash.colours.size(), 3u);
    EXPECT_EQ(flash.life, 2);
    EXPECT_FLOAT_EQ(flash.endWidth, 142.0f);
    EXPECT_EQ(flash.texture, 558u);
    ASSERT_EQ(gun->lights.size(), 1u);
    EXPECT_EQ(gun->lights[0].segment, -10);
    EXPECT_FLOAT_EQ(gun->lights[0].radius, 300.0f);
    EXPECT_EQ(gun->lights[0].duration, 2);
    EXPECT_NEAR(gun->lights[0].colour.g, 246 / 255.0f, 1e-6f);
    fx::System s;
    s.play(gun, *gun->clip("fire"), glm::mat4(1), -1, 0);
    std::vector<fx::System::LiveLight> lights;
    s.lights(lights);
    EXPECT_EQ(s.glowCount(), 1u);
    ASSERT_EQ(lights.size(), 1u);
    EXPECT_FALSE(lights[0].forever);
    for (int t = 0; t < 6; ++t) s.tick({0, -500, 0});
    s.lights(lights);
    EXPECT_EQ(s.glowCount(), 0u);
    EXPECT_TRUE(lights.empty());
    const auto fill = lib.get("fx_light_chapel_fill");
    ASSERT_TRUE(fill);
    fx::System f;
    EXPECT_EQ(f.startBirth(fill, glm::mat4(1), 7), 0); // its birth record is disabled: scripts play "on" / "off"
    ASSERT_TRUE(fill->clip("on")); // its records are variant 1 (info & 0xC00): run its light command directly
    EXPECT_TRUE(f.runCommand(fill, {0x11, 1, 0, 0}, glm::mat4(1), 7));
    f.lights(lights);
    ASSERT_FALSE(lights.empty());
    EXPECT_TRUE(lights[0].forever);
    ASSERT_TRUE(lights[0].dtp);
    EXPECT_EQ(lights[0].dtp->falloff.size(), 256u);
    EXPECT_FLOAT_EQ(lights[0].radius, 3792.0f);
    const auto& tex = lib.textures();
    EXPECT_TRUE(std::any_of(tex.begin(), tex.end(), [](const auto& t) { return t.id == 558; }));
}

// Material particles (+0x168 / +0x16C): fx_fire's flames are soft additive flipbooks (material 6603: shaders 651-653,
// DepthBufferTexture, c100 = (2, 0, -0.01)); wp_handgun def 25 (material 2770) refracts the back buffer by c100.z.
// The prologue corridor fire is unit FX point sets (unit data +0x120 / +0x124) running fx_fire's GenericFXObject.
TEST(FxReal, MaterialParticlesAndUnitPointSets) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto fire = load(bf, "fx_fire");
    ASSERT_TRUE(fire);
    ASSERT_EQ(fire->particles[2].materials.size(), 1u);
    const auto& m = fire->particles[2].materials[0];
    EXPECT_EQ(m.id, 6603u);
    EXPECT_EQ(m.texture, 1792u);
    EXPECT_TRUE(m.additive);
    EXPECT_FALSE(m.distort);
    EXPECT_NEAR(m.softScale, 0.01f, 1e-6f);
    const auto gun = load(bf, "wp_handgun");
    ASSERT_TRUE(gun);
    ASSERT_EQ(gun->particles[25].materials.size(), 1u);
    const auto& d = gun->particles[25].materials[0];
    EXPECT_TRUE(d.distort);
    EXPECT_EQ(d.texture, 566u);
    EXPECT_EQ(d.normal, 565u);
    EXPECT_NEAR(d.distortScale, 0.3f, 1e-6f);

    auto b = bf.read("pc-w\\l3_main_bedroom.drm");
    ASSERT_TRUE(b);
    const auto unit = cdc::parseDrm(std::move(*b));
    const auto sets = fx::parsePointSets(unit);
    ASSERT_EQ(sets.size(), 230u);
    const auto it = std::find_if(sets.begin(), sets.end(), [](const fx::PointSet& s) { return s.name == "FXPointSet50069"; });
    ASSERT_NE(it, sets.end());
    ASSERT_EQ(it->segments.size(), 3u);
    EXPECT_NEAR(it->segments[1][3].x, -4282.35f, 0.1f);
    EXPECT_NEAR(it->segments[2][3].z, 952.39f, 0.1f);
    fx::System s;
    EXPECT_GT(s.startPointSet(*it, glm::mat4(1), 1), 0);
    for (int k = 0; k < 60; ++k) s.tick({-4263, 1109, 300});
    render::Camera cam;
    cam.position = {-4258, 2364, 661};
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> batches;
    cam.yaw = -1.5707963f; // looking along -Y at the rubble
    s.build(cam, 16.0f / 9.0f, v, batches);
    EXPECT_TRUE(std::any_of(batches.begin(), batches.end(), [](const auto& x) { return x.softScale > 0 && x.blend == 1; }));
}

// Class-5 screen filters (EFFECTS.md "Screen filters"): weight = intensity x fades x (1 - d^2 / r^2), d from
// mix(camera, player) to the owner; finish block sums clamp at 4 (blur) / 2 (amount, drawn x 0.5); stop fades out.
TEST(Fx, ScreenFilterWeightsFadeAndStop) {
    std::vector<uint8_t> res(0x31C0, 0);
    for (int c = 0; c < 3; ++c)
        for (int x = 0; x < 256; ++x) res[size_t(0x2640 + 4 * c * 256 + x)] = uint8_t(x); // identity finish curves
    res[0x2640 + 0x909] = 1;
    put<float>(res, 0x2640 + 0x900, 4.0f);
    put<float>(res, 0x2640 + 0x904, 2.0f);
    auto o = std::make_shared<fx::ObjectFx>();
    fx::FilterDef f;
    f.id = 10, f.fadeIn = 10, f.fadeOut = 5, f.hold = -1, f.intensity = 0.5f, f.radius = 1000, f.playerMix = 1;
    f.resource = std::make_shared<const fx::FilterResource>(fx::parseFilterResource(res));
    EXPECT_TRUE(f.resource->finish);
    EXPECT_FALSE(f.resource->colorDof);
    o->filters.push_back(f);
    fx::System s;
    const glm::vec3 cam(0, -3000, 0), player(0, 500, 0); // player 500 from the owner: weight x 0.75
    render::ScreenFilter out;
    EXPECT_TRUE(s.runCommand(o, {0xB, 0, 0, 0}, glm::mat4(1), 7));
    EXPECT_FALSE(s.runCommand(o, {0xB, 0, 0, 0}, glm::mat4(1), 7, {}, true)); // birth: needs +1 & 1
    for (int k = 0; k < 10; ++k) s.tick(cam); // halfway through the 20-tick fade-in
    s.screenFilter(cam, player, out);
    EXPECT_NEAR(out.blurC, 4.0f * 0.5f * 0.5f * 0.75f, 1e-4f);
    EXPECT_NEAR(out.amountC, 2.0f * 0.5f * 0.5f * 0.75f * 0.5f, 1e-4f);
    EXPECT_FALSE(out.finishMatrix);
    for (int k = 0; k < 100; ++k) s.tick(cam);
    s.screenFilter(cam, player, out);
    EXPECT_NEAR(out.amountC, 2.0f * 0.5f * 0.75f * 0.5f, 1e-4f);
    s.screenFilter(cam, {0, 2000, 0}, out); // outside the radius
    EXPECT_FALSE(out.active());
    s.runCommand(o, {0x1A, 10, 0, 0}, glm::mat4(1), 7); // fade out over 10 ticks
    EXPECT_FALSE(s.hasOwner(7));
    for (int k = 0; k < 5; ++k) s.tick(cam);
    s.screenFilter(cam, player, out);
    EXPECT_NEAR(out.amountC, 2.0f * 0.5f * 0.75f * 0.5f * 0.5f, 1e-4f);
    for (int k = 0; k < 6; ++k) s.tick(cam);
    EXPECT_EQ(s.filterCount(), 0u);
    // The LUT keeps the identity where no curves run: row 6 (finish matrix, red input) texel 200 = (200, 0, 0).
    s.runCommand(o, {0xB, 0, 0, 0}, glm::mat4(1), 7);
    s.screenFilter(cam, player, out);
    EXPECT_EQ(out.lut[(6 * 256 + 200) * 4], 200);
    EXPECT_EQ(out.lut[(6 * 256 + 200) * 4 + 1], 0);
}

// The prologue corridor fire's point sets run fx_fire's class-5 record 0 (filter 4190: finish blur 4, amount 2,
// intensity 0.625, radius 1580 around Lara): close to the rubble the bloom saturates; far up the corridor it is off.
TEST(FxReal, PrologueFireScreenFilter) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto b = bf.read("pc-w\\l3_main_bedroom.drm");
    ASSERT_TRUE(b);
    const auto unit = cdc::parseDrm(std::move(*b));
    const auto sets = fx::parsePointSets(unit);
    const auto it = std::find_if(sets.begin(), sets.end(), [](const fx::PointSet& x) { return x.name == "FXPointSet50069"; });
    ASSERT_NE(it, sets.end());
    const auto& fire = *it->fx;
    ASSERT_EQ(fire.filters.size(), 3u);
    const auto& f = fire.filters[0];
    ASSERT_TRUE(f.resource);
    EXPECT_FLOAT_EQ(f.intensity, 0.625f);
    EXPECT_FLOAT_EQ(f.radius, 1580.0f);
    EXPECT_FLOAT_EQ(f.playerMix, 1.0f);
    EXPECT_EQ(f.hold, -1);
    EXPECT_TRUE(f.resource->finish);
    EXPECT_FALSE(f.resource->colorDof);
    EXPECT_FLOAT_EQ(f.resource->finishBlur, 4.0f);
    EXPECT_FLOAT_EQ(f.resource->finishAmount, 2.0f);
    EXPECT_TRUE(fire.filters[1].resource && fire.filters[1].resource->colorDof && fire.filters[1].resource->finishCurves);
    fx::System s;
    for (size_t i = 0; i < sets.size(); ++i) s.startPointSet(sets[i], glm::mat4(1), 0x40000000 | int32_t(i));
    EXPECT_GT(s.filterCount(), 50u);
    s.tick({0, 0, 0});
    render::ScreenFilter out;
    s.screenFilter({-4258, 2364, 661}, {-4263, 1109, 87}, out); // the fire_rubble_close view
    EXPECT_FLOAT_EQ(out.blurC, 4.0f);
    EXPECT_FLOAT_EQ(out.amountC, 1.0f);
    s.screenFilter({-4257, 7552, 668}, {-4263, 6291, 87}, out); // fire_corridor_far: none
    EXPECT_FALSE(out.active());
}

// Class 1 ribbons follow their tracked owner's two segments: one quad per tick, fading over the face life; class 3
// lightning draws two bands of `segments` quads and ends with its life.
TEST(Fx, RibbonsTrailTrackedSegmentsAndBoltsExpire) {
    auto o = std::make_shared<fx::ObjectFx>();
    o->segments = {glm::mat4(1), glm::translate(glm::mat4(1), glm::vec3(0, 0, 100))};
    fx::RibbonDef r;
    r.segment = 0, r.segment2 = 1, r.life = -1, r.faceLife = 10, r.rgba = r.rgba2 = 0x80808080u;
    o->ribbons.push_back(r);
    fx::LightningDef l;
    l.life = 5, l.segment = 0, l.segment2 = 1, l.segments = 4, l.width = l.width2 = 10, l.glow = l.glow2 = 20;
    l.core = l.core2 = l.outer = l.outer2 = 0x80808080u;
    o->bolts.push_back(l);
    fx::System s;
    auto at = [](float x) { return glm::translate(glm::mat4(1), glm::vec3(x, 2000, 0)); };
    ASSERT_TRUE(s.runCommand(o, {5, 0, 0, 1}, at(0), -2));
    ASSERT_TRUE(s.runCommand(o, {9, 0, 0, 1}, at(0), -2));
    for (int t = 0; t < 8; ++t) s.track(-2, at(float(t) * 10), {}), s.tick({0, 0, 0});
    EXPECT_EQ(s.ribbonCount(), 1u);
    EXPECT_EQ(s.faceCount(), 7u); // a quad per tick after the first pair
    EXPECT_EQ(s.boltCount(), 1u); // life 5 frames = 10 ticks
    render::Camera cam;
    cam.lookAt({0, 2000, 50});
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    s.build(cam, 1.0f, v, b);
    EXPECT_EQ(v.size(), (7u + 2u * 4u) * 6u); // ribbon quads + glow and core bands of 4 steps
    for (int t = 0; t < 30; ++t) s.tick({0, 0, 0}); // owner no longer tracked: the ribbon ends, its quads fade out
    EXPECT_EQ(s.ribbonCount(), 0u);
    EXPECT_EQ(s.faceCount(), 0u);
    EXPECT_EQ(s.boltCount(), 0u);
}

TEST(Fx, InstanceFadeClass7Curve) {
    auto o = std::make_shared<fx::ObjectFx>();
    o->fades = {{0, 1, 35}, {1, 0, 0}}; // med_rw_shard_a's fade out; an instant set
    fx::Event pickup, idle;
    pickup.info = 0x2401, pickup.cmd = {0x22, 0, 0, 0}; // variant 1: run by runVariant, never by frame
    idle.info = 0x2001, idle.cmd = {0x22, 1, 0, 0};
    const std::vector<fx::Event> list{idle, pickup};
    fx::System s;
    EXPECT_EQ(s.runEvents(o, list, glm::mat4(1), 7, 0, 0, 1), 1); // the variant-0 record only: fade set to 0 at once
    EXPECT_FLOAT_EQ(s.fade(7), 0.0f);
    EXPECT_FALSE(s.fading(7));
    EXPECT_EQ(s.runVariant(o, list, glm::mat4(1), 7, 1), 1);
    EXPECT_TRUE(s.fading(7));
    for (int t = 0; t < 35; ++t) s.tick({0, 0, 0}); // 35 ticks = 17.5 frames of 35
    EXPECT_NEAR(s.fade(7), 0.5f, 1e-5f);
    std::vector<std::pair<int32_t, float>> faded;
    s.fadedOwners(faded);
    ASSERT_EQ(faded.size(), 1u);
    EXPECT_NEAR(faded[0].second, 0.5f, 1e-5f);
    for (int t = 0; t < 35; ++t) s.tick({0, 0, 0});
    EXPECT_FLOAT_EQ(s.fade(7), 1.0f); // gone, and stays
    EXPECT_FALSE(s.fading(7));
    EXPECT_TRUE(s.runCommand(o, {0x21, 0, 10, 0}, glm::mat4(1), 7)); // 0x52C890: back to 0 over 10 frames from the current 1
    for (int t = 0; t < 10; ++t) s.tick({0, 0, 0});
    EXPECT_NEAR(s.fade(7), 0.5f, 1e-5f);
    s.stopOwner(7);
    EXPECT_FLOAT_EQ(s.fade(7), 0.0f);
}

TEST(FxReal, RewardPickupFadesOut) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    for (const char* name : {"med_rw_shard_a", "ar_rw_artifact"}) {
        const auto parsed = load(bf, name);
        ASSERT_TRUE(parsed) << name;
        const auto t = std::make_shared<const fx::ObjectFx>(*parsed);
        ASSERT_EQ(t->fades.size(), 1u) << name;
        EXPECT_FLOAT_EQ(t->fades[0].from, 0.0f);
        EXPECT_FLOAT_EQ(t->fades[0].to, 1.0f);
        EXPECT_EQ(t->fades[0].frames, 35);
        const auto* pickup = t->clip("pickup");
        ASSERT_TRUE(pickup) << name;
        fx::System s;
        EXPECT_EQ(s.runVariant(t, *pickup, glm::mat4(1), 1, 1), 1) << name;
        for (int k = 0; k < 70; ++k) s.tick({0, 0, 0});
        EXPECT_FLOAT_EQ(s.fade(1), 1.0f) << name;
    }
}

// Lara's four arm / leg ribbons are on her object list's events 50 / 80 (stopped by 51 / 81), which tru.exe never
// fires (0x5BA000 runs only events 0, 9..0xE, 0x2E / 0x2F, 0x30, 0x77, 0x78 / 0x79): her birth starts none.
// With visuals off (ou-game --fx off) nothing starts at all.
TEST(FxReal, LaraRibbonsAreOnUnfiredEventsAndVisualsOffStartsNothing) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto lara = load(bf, "lara");
    ASSERT_TRUE(lara);
    int ribbons = 0;
    for (const auto& e : lara->events)
        if (e.effect() && e.cmd.type == 5) {
            EXPECT_TRUE(e.event == 50 || e.event == 80) << int(e.event);
            ++ribbons;
        }
    EXPECT_EQ(ribbons, 8);
    const auto fx = std::make_shared<const fx::ObjectFx>(*lara);
    fx::System s;
    s.startBirth(fx, glm::mat4(1), 1);
    s.track(1, glm::mat4(1), {});
    s.tick({0, 0, 0});
    EXPECT_EQ(s.ribbonCount(), 0u);
    EXPECT_GT(s.emitterCount() + s.glowCount(), 0u); // the birth's own particles run

    const auto gun = load(bf, "wp_handgun");
    ASSERT_TRUE(gun);
    const auto g = std::make_shared<const fx::ObjectFx>(*gun);
    fx::System off;
    off.visuals = false;
    int sounds = 0;
    off.sound = [&](const fx::ObjectFx&, const fx::Event&, glm::vec3, uint8_t) { ++sounds; };
    off.startBirth(fx, glm::mat4(1), 1);
    off.play(g, *g->clip("fire"), glm::mat4(1), -1, 0, glm::vec3(0, 1000, 0));
    for (int t = 0; t < 4; ++t) off.tick({0, 0, 0});
    EXPECT_EQ(off.emitterCount() + off.particleCount() + off.glowCount() + off.boltCount() + off.ribbonCount() + off.filterCount(), 0u);
    EXPECT_GT(sounds, 0); // the fire clip's sound record still plays
}

// Under water (docs/RENDERING.md "Under water"): the unit's water fog set, deep / surface screen filters with their fog
// blocks, the full-screen pass material and the enter list (tru.exe 0x5C1F50, 0x5C78A0, 0x6D8C20, 0x5C7CA0).
TEST(FxReal, UnderwaterSettings) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto b = bf.read("pc-w\\l1_seafloor.drm");
    ASSERT_TRUE(b);
    const auto d = cdc::parseDrm(std::move(*b));
    const auto e = formats::parseUnit(d).environment;
    EXPECT_NEAR(e.waterFogNear, 31605.6f, 1.0f);
    EXPECT_NEAR(e.waterFogFar, 632111.0f, 2.0f);
    EXPECT_EQ(e.waterFilter[0], 5292u);
    EXPECT_EQ(e.waterFilter[1], 5557u);
    EXPECT_FLOAT_EQ(e.waterFilterRange[0], 100.0f);
    EXPECT_EQ(e.waterList[0], 4455u);
    EXPECT_NEAR(e.waterPass[0][2], 0.607843f, 1e-5); // surface blue (material 5767 c100)
    EXPECT_NEAR(e.waterPass[0][3], 0.6f, 1e-5);      // alpha at the eye
    EXPECT_NEAR(e.waterPass[1][3], 45000.0f, 1.0f);  // full-fog distance
    const auto& rays = e.waterRays; // the record's light rays: DTP 5386 grid, markers 2 / 3 / 4, material 5379
    EXPECT_EQ(rays.grid, 32);
    EXPECT_EQ(rays.mask.size(), 32u * 32u);
    EXPECT_FLOAT_EQ(rays.halfSize, 11000.0f);
    EXPECT_NEAR(rays.length, 126422.0f, 1.0f);
    EXPECT_NEAR(rays.sun[2], 374807.0f, 1.0f);
    EXPECT_NEAR(rays.depthZ[0], -9022.19f, 0.1f);
    EXPECT_NEAR(rays.depthZ[1], 12299.8f, 0.1f);
    EXPECT_NEAR(rays.vsExtinction, 2.11e-5f, 1e-9f);
    const auto deep = fx::loadFilterResource(d, 5292), surface = fx::loadFilterResource(d, 5557);
    ASSERT_TRUE(deep && surface);
    EXPECT_TRUE(deep->fog && !surface->fog);
    EXPECT_NEAR(deep->fogNear, 379267.0f, 2.0f);
    EXPECT_TRUE(deep->colorDof && surface->feedback);
    const auto enter = fx::parseEffectList(d, e.waterList[0]);
    ASSERT_TRUE(enter && enter->fx);
    EXPECT_FALSE(enter->events.empty());
    fx::System s;
    EXPECT_GT(s.startPointSet(*enter, glm::mat4(1), -3), 0);
    s.moveOwner(-3, {100, 0, 0});
    for (int k = 0; k < 60; ++k) s.tick({0, 0, 0});
    EXPECT_GT(s.particleCount(), 0u);

    auto a = bf.read("pc-w\\l7_arctic_sea.drm");
    ASSERT_TRUE(a);
    const auto arctic = formats::parseUnit(cdc::parseDrm(std::move(*a))).environment;
    EXPECT_NEAR(arctic.waterPass[0][2], 0.662745f, 1e-5); // material 19575
    EXPECT_NEAR(arctic.waterPass[1][3], 60000.0f, 1.0f);
    EXPECT_EQ(arctic.waterFilter[0], 859u);
}
