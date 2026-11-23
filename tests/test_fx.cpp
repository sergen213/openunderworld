// Original particle effects: definition parser, deterministic simulation, sprite batches (docs/formats/EFFECTS.md).
#include "formats/Formats.hpp"
#include "fx/Breath.hpp"
#include "fx/Fx.hpp"
#include "fx/Precip.hpp"

#include <gtest/gtest.h>

#include <cmath>
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
    // 0x536280: NDC half-size 2 x 10 / z x 4/3 whatever the fov = 10 x (8/3) tan(fov / 2) world units at our camera.
    EXPECT_NEAR(w0, 2 * 10 * 8.0f / 3.0f * std::tan(glm::radians(cam.verticalFovDeg) / 2), 0.05f);
    for (int t = 0; t < 5; ++t) s.tick({-500, 0, 0});
    s.build(cam, 1.0f, v, b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_NEAR(width() / w0, 2.25f, 0.05f);        // 5 of 8 ticks: 10 -> 22.5 of 30
    EXPECT_LT(v[0].rgba & 0xFF, 0x80u);            // fading (RGB) in its last 2 frames
    for (int t = 0; t < 3; ++t) s.tick({-500, 0, 0});
    EXPECT_EQ(s.glowCount(), 0u);
}

// A faded instance's glows (instance +0x1B4, command 0x21) draw at its opacity instead of their own fade in (0x536280).
TEST(Fx, GlowsFadeWithTheirInstance) {
    auto o = std::make_shared<fx::ObjectFx>();
    fx::GlowDef g;
    g.flags = 0x01, g.width = g.height = 10, g.fadeIn = 10;
    g.colours = {0x80808080};
    o->glows.push_back(g);
    o->events.push_back({0x2001, 0, 0, {6, 0, 0, 0}});
    fx::System s;
    ASSERT_EQ(s.startBirth(o, glm::mat4(1), 1), 1);
    s.runCommand(o, {0x21, 64, 0, 0}, glm::mat4(1), 1); // fade 0.25 at once
    render::Camera cam;
    cam.position = {-500, 0, 0};
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    s.build(cam, 1.0f, v, b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_NEAR(float(v[0].rgba & 0xFF), 0x80 * 0.75f, 1.0f); // age 0: no fade-in, the instance's 0.75
    s.runCommand(o, {0x21, 255, 0, 0}, glm::mat4(1), 1);
    s.build(cam, 1.0f, v, b);
    EXPECT_TRUE(b.empty()); // gone
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

// Lara's idle breath condensation (ng_idle frames 20 / 120: def 6 at the head, event byte 0x2C) needs the snow amount,
// which nothing in the data sets (tru.exe 0x7181A0): her idle starts no emitter.
TEST(FxReal, LaraIdleBreathNeedsSnow) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto lara = lib.get("lara");
    ASSERT_TRUE(lara);
    const auto* idle = lara->clip("ng_idle");
    ASSERT_TRUE(idle);
    EXPECT_EQ(std::count_if(idle->begin(), idle->end(), [](const fx::Event& e) { return e.runsOn(0) && e.event == 0x2C; }), 2);
    fx::System s;
    EXPECT_EQ(s.runEvents(lara, *idle, glm::mat4(1), -2, 0, 0.0f, 1e9f), 0);
    EXPECT_EQ(s.emitterCount(), 0u);
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

// The prologue smoke (material 6946, ShaderLib 655 reads InstanceParams c133 / c135) is a lit sprite: its emitter's light
// record gives light 0 = (75, 44, 14) / 255, light 1 = 0 (rendercap10 bedroom, D3D9 call 19973134), window c100 (0, 1).
TEST(FxReal, PrologueSmokeIsLitByItsLightRecord) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = cdc::parseDrm(std::move(*bf.read("pc-w\\l3_main_bedroom.drm")));
    std::shared_ptr<const fx::ObjectFx> smoke;
    int def = -1;
    for (const auto& set : fx::parsePointSets(unit))
        for (size_t k = 0; set.fx && k < set.fx->particles.size() && def < 0; ++k)
            for (const auto& m : set.fx->particles[k].materials)
                if (m.id == 6946) smoke = set.fx, def = int(k);
    ASSERT_TRUE(smoke) << "no point set runs material 6946";
    const auto& d = smoke->particles[size_t(def)];
    const auto& m = d.materials.front();
    EXPECT_TRUE(m.lit);
    EXPECT_FALSE(m.distort);
    EXPECT_NE(m.normal, 0u);
    EXPECT_FLOAT_EQ(m.litLow, 0.0f);
    EXPECT_FLOAT_EQ(m.litHigh, 1.0f);
    EXPECT_NEAR(d.lights[0].r, 75 / 255.0f, 1e-6f);
    EXPECT_NEAR(d.lights[0].g, 44 / 255.0f, 1e-6f);
    EXPECT_NEAR(d.lights[0].b, 14 / 255.0f, 1e-6f);
    EXPECT_EQ(d.lights[1], glm::vec3(0));
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

// Every unit's own screen filter (render settings +0x0C) carries the level's haze in its fog block. At the l2_junglehub
// start tru.exe's view held (Wine, 2026-11-06): filter slot 0 = DTP 10643 at weight 1, fog +0x200 = (-0, 1/56890),
// colour +0x210 = (0.6431, 0.6745, 0.7059), LUT fog row ending 0x5F..0x63; the unit set (near 889495, far 1105858) adds
// nothing at weight 1.
TEST(FxReal, UnitFilterFogMatchesOriginal) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto drm = cdc::parseDrm(std::move(*bf.read("pc-w\\l2_junglehub.drm")));
    const auto env = formats::parseUnit(drm).environment;
    EXPECT_EQ(env.filter, 10643u);
    const auto f = fx::loadFilterResource(drm, env.filter);
    ASSERT_TRUE(f && f->fog);
    fx::FogSum s;
    s.add(*f, 1.0f);
    render::Fog set;
    set.nearZ = env.fogNear, set.farZ = env.fogFar;
    for (int k = 0; k < 3; ++k) set.color[k] = env.fogColor[k];
    const auto fog = s.resolve(set);
    EXPECT_FLOAT_EQ(fog.nearZ, 0.0f);
    EXPECT_NEAR(1.0f / (fog.farZ - fog.nearZ), 1.758e-5f, 1e-8f);
    EXPECT_NEAR(fog.color[0], 0.6431f, 1e-3f);
    EXPECT_NEAR(fog.color[1], 0.6745f, 1e-3f);
    EXPECT_NEAR(fog.color[2], 0.7059f, 1e-3f);
    EXPECT_EQ(fog.curve[0], 0);
    EXPECT_EQ(fog.curve[255], 0x63);
    EXPECT_EQ(fog.curve[244], 0x5F);
    // Half weight: the rest is the unit set (far clamped to it) and half the identity curve.
    fx::FogSum h;
    h.add(*f, 0.5f);
    const auto half = h.resolve(set);
    EXPECT_NEAR(half.nearZ, env.fogNear * 0.5f, 1.0f);
    EXPECT_EQ(half.curve[255], std::lround(0x63 * 0.5) + 128);
}

// fx::Weather (EFFECTS.md "Weather"): the pool refills to 512 at 32 a tick above the height map, drops fall and end
// at it, the sheets draw as additive quads of the tier textures, lightning starts its event 0x30 and flicker light.
TEST(Fx, WeatherRainFillsFallsAndFlashes) {
    fx::WeatherDef d;
    d.textures = {3711, 757, 3712};
    d.levels = {0, 0.2167f, 0.6067f, 1};
    d.minWidth = 160, d.width = 256, d.length = 10, d.nearZ = 448, d.farZ = 6000, d.wind = 3;
    d.colourA = d.colourB = 0x141414, d.farColour = 0x101010, d.alpha = 0x80000000u, d.flags = 2;
    d.flashColour = 0x806855, d.flashLight = 4214, d.flashRadius = 100000;
    fx::Weather w;
    fx::Weather::World q;
    q.ceiling = [](glm::vec3) { return 0; };          // open ground at z 0
    q.wind = [](glm::vec3) { return glm::vec2(1, 0); };
    q.ray = [](glm::vec3 a, glm::vec3) { return std::optional<std::pair<glm::vec3, uint8_t>>({{a.x, a.y, 0}, 2}); };
    fx::Weather::Camera cam;
    cam.eye = {0, 0, 300};
    w.setScript(40, 1, 1); // ballcourt's rain (DAT_00AE4D84 0x199 under Wine), lightning every second
    EXPECT_EQ(w.rain(), 0x199);
    EXPECT_TRUE(w.started()); // 0x53AB30: event 0x2E on every instance
    w.tick(&d, cam, q);
    EXPECT_EQ(w.dropCount(), 32u);
    EXPECT_EQ(w.musicVar(), std::optional<uint8_t>(0)); // unit flag 2 clear (every retail unit)
    w.setScript(40, 1, 1);
    EXPECT_FALSE(w.started()); // only when it starts from none
    bool splash = false, bolt = false;
    for (int t = 0; t < 120; ++t) {
        w.tick(&d, cam, q);
        for (const auto& e : w.events()) splash |= e.event == 0x2E && e.material == 2, bolt |= e.event == 0x30;
    }
    EXPECT_GT(w.dropCount(), 400u); // drops end at the ground or their life, the pool refills
    EXPECT_TRUE(splash);
    EXPECT_TRUE(bolt);
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    w.build(d, cam, v, b);
    ASSERT_EQ(b.size(), 2u); // 0.4: tier 3712 for the first drops, 757 for the rest
    EXPECT_EQ(b[0].textureId, 3712u);
    EXPECT_EQ(b[1].textureId, 757u);
    EXPECT_EQ(b[0].blend, 1);
    EXPECT_EQ(v.size() % 6, 0u);
    w.setScript(0, 1, 1);
    EXPECT_EQ(w.musicVar(), std::optional<uint8_t>(0));
    for (int t = 0; t < 200; ++t) w.tick(&d, cam, q);
    EXPECT_EQ(w.dropCount(), 0u); // no new drops, the last ones fell
    EXPECT_FALSE(w.flash());
    // Unit flags (Level+0x2C): 2 sets music variable 2 every rain tick; 0x20000 starts drops up to 18000 deep.
    auto farthest = [&](uint32_t flags) {
        fx::Weather x;
        q.unitFlags = flags;
        x.setScript(100, 0, 0);
        float far = 0;
        for (int t = 0; t < 30; ++t) {
            x.tick(&d, cam, q);
            std::vector<render::FxVertex> xv;
            std::vector<render::FxBatch> xb;
            x.build(d, cam, xv, xb);
            for (const auto& p : xv) far = std::max(far, glm::dot(glm::make_vec3(p.pos) - cam.eye, cam.forward));
        }
        EXPECT_EQ(x.musicVar(), std::optional<uint8_t>((flags & 2) ? 1 : 0));
        return far;
    };
    EXPECT_LT(farthest(0), 8192.0f + 200.0f);
    EXPECT_GT(farthest(0x20002), 9000.0f);
}

// fx::Weather snow (tru.exe 0x539EF0 / 0x539CD0 / 0x534DF0): script snow fills the shared pool to snow / 2 flakes at
// <= 20 a still frame, every flake drawn as a glow-texture quad inside the view 1024 .. 12288 deep, under a roof as well
// (tru.exe drops the height map's answer); snow clears the rain; nothing spawns below z -29968 of tru.exe's frame;
// snow off lets the flakes end.
TEST(Fx, WeatherSnowFlakesFillTheView) {
    fx::WeatherDef d;
    d.textures = {3711, 757, 3712};
    fx::Weather w;
    fx::Weather::World q;
    q.ceiling = [](glm::vec3) { return 100000; }; // a roof over everything: the flakes ignore it
    fx::Weather::Camera cam;
    cam.eye = {0, 0, 300};
    w.setScript(40, 0, 0);
    w.setSnow(100);
    EXPECT_EQ(w.snow(), 1024);
    EXPECT_EQ(w.rain(), 0);
    w.tick(&d, cam, q);
    EXPECT_EQ(w.flakeCount(), 20u); // 0x539EF0: 20 + turn / 2 a frame
    for (int t = 0; t < 120; ++t) w.tick(&d, cam, q);
    EXPECT_GT(w.flakeCount(), 480u);
    EXPECT_LE(w.flakeCount(), 512u);
    std::vector<render::FxVertex> v;
    std::vector<render::FxBatch> b;
    w.build(d, cam, v, b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].textureId, fx::kGlowTexture);
    EXPECT_GT(b[0].count, 6u * 400u);
    const float tanY = cam.tanHalfFovY;
    for (const auto& p : v) {
        const glm::vec3 r = glm::make_vec3(p.pos) - cam.eye;
        const float depth = glm::dot(r, cam.forward);
        EXPECT_GT(depth, 1000.0f);
        EXPECT_LT(depth, 12300.0f);
        EXPECT_LT(std::abs(glm::dot(r, cam.down)), depth * tanY * 1.2f); // inside the view (+-1.1 and the quad)
    }
    fx::Weather deep;
    q.exeOrigin = {0, 0, 40000}; // the camera at z -39700 in tru.exe's frame: the whole view below -29968
    deep.setSnow(100);
    for (int t = 0; t < 10; ++t) deep.tick(&d, cam, q);
    EXPECT_EQ(deep.flakeCount(), 0u);
    w.setSnow(0);
    for (int t = 0; t < 320; ++t) w.tick(&d, cam, q);
    EXPECT_EQ(w.flakeCount(), 0u);
}

// The weather objects' data blocks (ObjectBase +0xC8) and the l4 units' weather object / height map (Level+0x70 DTP
// +0x290, Level+0x54), against tru.exe's memory under Wine (weather object +0x5C, 0x546FC0's 0xAE4DA0).
TEST(FxReal, WeatherObjectAndRainMap) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto* d = lib.weather("rainfx_mex");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->textures[0], 3711u);
    EXPECT_EQ(d->textures[2], 3712u);
    EXPECT_NEAR(d->levels[1], 0.2167f, 1e-4f);
    EXPECT_FLOAT_EQ(d->minWidth, 160.0f);
    EXPECT_FLOAT_EQ(d->farZ, 6000.0f);
    EXPECT_FLOAT_EQ(d->wind, 3.0f);
    EXPECT_EQ(d->alpha, 0x80000000u);
    EXPECT_EQ(d->flags, 2);
    EXPECT_EQ(d->flashLight, 4214u);
    EXPECT_EQ(d->flashColour, 0x80806855u);
    std::set<uint32_t> tex;
    for (const auto& t : lib.textures()) tex.insert(t.id);
    EXPECT_TRUE(tex.contains(757));
    ASSERT_TRUE(lib.weather("rainfx"));
    const auto drm = cdc::parseDrm(std::move(*bf.read("pc-w\\l4_ballcourt.drm")));
    const auto u = formats::parseUnit(drm);
    EXPECT_EQ(u.weatherObjectId, 541); // rainfx_mex
    EXPECT_FALSE(u.rainMap.leaves.empty());
    const int32_t h = u.rainMap.height(26968.7f, 23597.2f); // the Wine pair's Lara, open sky
    EXPECT_NE(h, formats::Unit::RainMap::kNone);
    EXPECT_LT(h, 2646 + 600);
    EXPECT_EQ(u.rainMap.height(0x41000, 0), formats::Unit::RainMap::kNone);
    EXPECT_EQ(u.flags, 0u); // Level+0x2C: no unit sets 2 (music variable 2) or 0x20000 (deep drops)
    // Rain starting runs event 0x2E on every instance (0x53AB30): rainfx's splash list, placed once in l6_ship_exterior.
    const auto rain = lib.get("rainfx");
    ASSERT_TRUE(rain);
    EXPECT_EQ(std::count_if(rain->events.begin(), rain->events.end(), [](const auto& e) { return e.event == 0x2E && e.effect(); }), 3);
    const auto ship = formats::parseUnit(cdc::parseDrm(std::move(*bf.read("pc-w\\l6_ship_exterior.drm"))));
    EXPECT_TRUE(std::any_of(ship.intros.begin(), ship.intros.end(), [](const auto& in) { return in.uniqueId == 7719; }));
}

// The snow the original shows in l5_startsnow is its 17 placed fx_snow_blizzard_arc_startsnow_ist emitters (GenericFX
// particles, no level collision: tru.exe's particle update 0x54C300 only clips at a water surface found at birth) plus,
// when a script sets Snow, the flake pool (fx::Weather). Emitter numbers from the data (per 30 Hz frame, units).
TEST(FxReal, WeatherEmittersFromData) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
    ASSERT_TRUE(list);
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    const auto u = formats::parseUnit(cdc::parseDrm(std::move(*bf.read("pc-w\\l5_startsnow.drm"))));
    EXPECT_EQ(std::count_if(u.intros.begin(), u.intros.end(), [&](const auto& in) {
                  const auto it = names.find(in.objectId);
                  return it != names.end() && it->second == "fx_snow_blizzard_arc_startsnow_ist";
              }), 17);
    const auto fx = lib.get("fx_snow_blizzard_arc_startsnow_ist");
    ASSERT_TRUE(fx);
    ASSERT_EQ(fx->particles.size(), 7u);
    const auto& haze = fx->particles[1]; // the drifting veils: 3 a frame within 3793, while the camera is within 15803
    EXPECT_FLOAT_EQ(haze.rate, 3.0f);
    EXPECT_EQ(haze.life, 50);
    EXPECT_NEAR(haze.radiusMax, 3793, 1);
    EXPECT_NEAR(haze.maxDistance, 15803, 1);
    EXPECT_NEAR(haze.velocity.y, 79.0f, 0.1f); // blown along the emitter's +y (its yaw: -2.60 rad for most)
    EXPECT_NEAR(haze.velocity.z, -6.3f, 0.1f);
    const auto& dots = fx->particles[4]; // the small flakes: texture 145, size 63 .. 0, alpha 0x14 at most
    EXPECT_EQ(dots.texture, 145u);
    EXPECT_NEAR(dots.maxDistance, 4741, 1);
}

// Clip variant records (0x43C9C0 -> 0x5B26C0): a fixture's clip start / leave records, and Lara's class-7 model
// crossfades, which nothing in the data reaches (no effect-map class 7, no command 0x22 in her lists).
TEST(FxReal, FixtureClipVariantsAndLaraCrossfades) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto fill = lib.get("fx_light_chapel_fill");
    ASSERT_TRUE(fill && fill->clip("on") && fill->clip("off"));
    for (const char* c : {"on", "off"})
        EXPECT_EQ(std::count_if(fill->clip(c)->begin(), fill->clip(c)->end(), [](const auto& e) { return e.effect() && e.variant() == 1; }), 2) << c;
    fx::System s;
    EXPECT_GT(s.runVariant(fill, *fill->clip("on"), glm::mat4(1), 5, 1), 0);
    std::vector<fx::System::LiveLight> lights;
    s.lights(lights);
    EXPECT_FALSE(lights.empty()); // the fill light burns
    const auto lara = lib.get("lara");
    ASSERT_TRUE(lara);
    ASSERT_EQ(lara->fades.size(), 17u);
    for (const auto& f : lara->fades) {
        EXPECT_EQ(f.flags, 0x80);
        EXPECT_TRUE(f.model >= 10 && f.model <= 12); // Thor's gauntlets / belt draw groups
    }
    EXPECT_TRUE(std::none_of(lara->effectMap.begin(), lara->effectMap.end(), [](const auto& m) { return m.cls == 7; }));
    auto noFadeCommand = [](const std::vector<fx::Event>& ev) {
        return std::none_of(ev.begin(), ev.end(), [](const auto& e) { return e.effect() && (e.cmd.type == 0x22 || e.cmd.type == 0x21); });
    };
    EXPECT_TRUE(noFadeCommand(lara->events));
    for (const auto& [name, ev] : lara->clips) EXPECT_TRUE(noFadeCommand(ev)) << name;
}

// Particle defs' water side (def +0x88 >> 5 & 3, tru.exe 0x540B80 / 0x54AEC0 against the camera under water 0x5EA390):
// a pistol shot above water draws its flash and smoke but not its bubbles, under water only the bubbles; the speargun's
// defs are all bubbles. bullet_hit's event-0x2D records (dust / sparks) skip water hits (0x5BBFD0).
TEST(FxReal, GunFireBubblesOnlyUnderWater) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto gun = lib.get("wp_handgun");
    ASSERT_TRUE(gun);
    ASSERT_EQ(gun->particles.size(), 27u);
    for (int d : {0, 4, 9, 11}) EXPECT_EQ(gun->particles[size_t(d)].water, 2) << d; // flash, smoke
    for (int d : {22, 23, 24, 26}) EXPECT_EQ(gun->particles[size_t(d)].water, 1) << d; // bubbles
    const auto drawn = [&](const char* object, bool underwater) {
        const auto fx = lib.get(object);
        fx::System s;
        s.cameraUnderwater = underwater;
        s.play(fx, *fx->clip(underwater ? "fire_submerged" : "fire"), glm::mat4(1), -1, 0);
        render::Camera cam;
        cam.position = {-500, 0, 0};
        std::set<uint32_t> textures;
        size_t particles = 0;
        for (int t = 0; t < 12; ++t) {
            s.tick(cam.position);
            std::vector<render::FxVertex> v;
            std::vector<render::FxBatch> b;
            s.build(cam, 16.0f / 9, v, b);
            for (const auto& x : b) textures.insert(x.textureId);
            particles = std::max(particles, s.particleCount());
        }
        return std::pair{textures, particles};
    };
    const auto bubbles = [](const std::set<uint32_t>& t) { return t.contains(147) || t.contains(148) || t.contains(149); }; // defs 22 / 23
    const auto [dry, dryCount] = drawn("wp_handgun", false);
    EXPECT_TRUE(dry.contains(162)); // muzzle flash (def 0)
    EXPECT_FALSE(bubbles(dry));
    const auto [wet, wetCount] = drawn("wp_handgun", true);
    EXPECT_TRUE(bubbles(wet));
    EXPECT_FALSE(wet.contains(162));
    EXPECT_GT(dryCount, 0u);
    EXPECT_GT(wetCount, 0u);
    EXPECT_EQ(drawn("wp_speargun", false).second, 0u); // nothing above water
    EXPECT_GT(drawn("wp_speargun", true).second, 0u);
    const auto hit = lib.get("bullet_hit");
    ASSERT_TRUE(hit);
    const auto* idle = hit->clip("idle");
    ASSERT_TRUE(idle);
    int dust = 0;
    for (const auto& e : *idle)
        if (e.event == 0x2D && e.runsOn(2)) {
            ++dust;
            EXPECT_TRUE(e.suppressed(2) && e.suppressed(3) && !e.suppressed(1));
        }
    EXPECT_EQ(dust, 3);
}

// Enhanced water (render/Water.hpp UwParticlePush): System::build hands the under-water-only particles (the handgun's
// bubble defs 22-26) to the bubble list instead of drawing their sprites; the flash and smoke are not under water anyway.
TEST(FxReal, EnhancedWaterBubblesLeaveTheSprites) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    const auto gun = lib.get("wp_handgun");
    ASSERT_TRUE(gun);
    fx::System s;
    s.cameraUnderwater = true;
    s.play(gun, *gun->clip("fire_submerged"), glm::mat4(1), -1, 0);
    render::Camera cam;
    cam.position = {-500, 0, 0};
    size_t most = 0, kept = 0;
    for (int t = 0; t < 12; ++t) {
        s.tick(cam.position);
        std::vector<render::FxVertex> v;
        std::vector<render::FxBatch> b;
        std::vector<render::Bubble> bubbles;
        s.build(cam, 16.0f / 9, v, b, &bubbles);
        for (const auto& x : b) {
            EXPECT_FALSE(x.textureId >= 147 && x.textureId <= 149) << x.textureId; // defs 22 / 23
            if (x.textureId == 161 || x.textureId == 183 || x.textureId == 165 || x.textureId == 184 || x.textureId == 555 ||
                x.textureId == 566)
                ++kept; // def 24's murk, defs 25 / 26's flashes: under-water-only too, but not bubbles
        }
        for (const auto& u : bubbles) {
            EXPECT_GT(u.radius, 0.0f);
            EXPECT_TRUE(u.alpha > 0.0f && u.alpha <= 1.0f);
            EXPECT_TRUE(u.seed >= 0.0f && u.seed < 1.0f);
        }
        most = std::max(most, bubbles.size());
    }
    EXPECT_GT(most, 0u);
    EXPECT_GT(kept, 0u);
}

// Only the bubble particles go to the bubble pass: of the under-water-only defs (water 1), those drawn with bubble
// textures alone (fx::isBubbleTexture) are bubbles; silt / smoke clouds, flashes, the speargun's trail streak are not.
TEST(FxReal, OnlyBubbleTexturedDefsAreBubbles) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    fx::Library lib(bf);
    struct Want { const char* object; size_t def; bool bubble; };
    for (const Want& c : {Want{"wp_handgun", 22, true}, Want{"wp_handgun", 23, true}, Want{"wp_handgun", 24, false},
                          Want{"wp_handgun", 25, false}, Want{"wp_handgun", 26, false}, Want{"wp_speargun", 0, true},
                          Want{"wp_speargun", 1, false}, Want{"pr_grenade", 33, true}, Want{"pr_grenade", 34, false},
                          Want{"pr_grenade", 38, false}, Want{"bullet_hit_underwater", 24, false},
                          Want{"fx_water_bubbles_vent", 0, true}, Want{"fx_bubbles_seafloor_ist", 1, true},
                          Want{"fx_med_krakenroom_current_ist", 0, true}, Want{"fx_med_krakenroom_current_ist", 1, false},
                          Want{"fx_med_krakenroom_current_ist", 11, false}, Want{"lara", 193, true}, Want{"lara", 165, false}}) {
        const auto o = lib.get(c.object);
        ASSERT_TRUE(o) << c.object;
        ASSERT_LT(c.def, o->particles.size()) << c.object;
        EXPECT_EQ(o->particles[c.def].water, 1) << c.object << " " << c.def;
        EXPECT_EQ(o->particles[c.def].bubble(), c.bubble) << c.object << " " << c.def;
    }
}

// Lara's breath (fx/Breath.hpp; puff clock [I], bubbles abyssal-garden's): puffs only while submerged, the same game time
// and mouth path give the same bubbles, they rise straight up from the mouth with the demo's wobble and size and are gone
// before the surface; a jump back in time clears them.
TEST(FxBreath, DeterministicRisingAndGoneAtTheSurface) {
    const glm::vec3 mouth(100, 200, -1000);
    auto run = [&](bool submerged, double until) {
        fx::Breath b;
        for (double t = 0; t <= until; t += 1.0 / 60) b.tick(t, mouth, submerged, false, 0.0f);
        std::vector<render::Bubble> out;
        b.append(until, out);
        return out;
    };
    EXPECT_TRUE(run(false, 8.0).empty());
    const auto a = run(true, 8.0), c = run(true, 8.0);
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), c.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(std::memcmp(&a[i], &c[i], sizeof a[i]), 0) << i;
        EXPECT_GT(a[i].pos[2], mouth.z - 10.0f); // above the mouth (jitter +-2)
        EXPECT_LT(a[i].pos[2] + a[i].radius, 0.0f); // under the surface
        EXPECT_LT(std::hypot(a[i].pos[0] - mouth.x, a[i].pos[1] - mouth.y), 10.0f); // jitter +-4; the sway is the shader's
        EXPECT_TRUE(a[i].wobble >= 0.05f && a[i].wobble <= 0.16f);
        EXPECT_LE(a[i].radius, 0.08f * 1.25f * 290.0f);
        EXPECT_NEAR(a[i].rise, (a[i].pos[2] - mouth.z) / 290.0f, 0.02f);
    }
    // 1000 units (3.25 m to life 1) at >= 0.74 m/s: every bubble older than ~4.4 s has reached the surface and left
    fx::Breath b;
    for (double t = 0; t <= 2.0; t += 1.0 / 60) b.tick(t, mouth, true, true, 0.0f);
    for (double t = 2.0; t <= 20.0; t += 1.0 / 60) b.tick(t, mouth, false, true, 0.0f);
    std::vector<render::Bubble> out;
    b.append(20.0, out);
    EXPECT_TRUE(out.empty());
    for (double t = 0; t <= 4.0; t += 1.0 / 60) b.tick(t, mouth, true, true, 0.0f); // time jumped back: restarts
    b.append(4.0, out);
    EXPECT_FALSE(out.empty());
}

// ---- Enhanced weather (fx::Precip, fx::SnowCoat) ----
namespace {
// Cover query: a flat ground at `groundZ` everywhere, a roof at `roofZ` where x < 0 (none: +inf).
ou::fx::Precip::Query flatCover(float groundZ, float roofZ = INFINITY) {
    return [=](std::span<const glm::vec3> from, std::span<const glm::vec3>, std::span<std::optional<float>> hits) {
        for (size_t i = 0; i < from.size(); ++i) hits[i] = std::isfinite(roofZ) && from[i].x < 0 ? roofZ : groundZ;
    };
}
std::vector<glm::vec3> quadCentres(const std::vector<ou::render::FxVertex>& v, const ou::render::FxBatch& b) {
    std::vector<glm::vec3> out;
    for (uint32_t i = b.first; i + 6 <= b.first + b.count; i += 6) { // two triangles 0 1 2 / 0 2 3: corners 0, 1, 2, 5
        const uint32_t k[4] = {i, i + 1, i + 2, i + 5};
        glm::vec3 c{0};
        for (const uint32_t j : k) c += glm::make_vec3(v[j].pos);
        out.push_back(c / 4.0f);
    }
    return out;
}
ou::fx::Precip::Frame frameAt(glm::vec3 eye, ou::fx::Precip::Kind kind, double t = 10.0) {
    ou::fx::Precip::Frame f;
    f.kind = kind, f.intensity = 1, f.time = t, f.eye = eye, f.forward = {0, 1, 0}, f.viewportHeight = 720;
    return f;
}
} // namespace

// The particles belong to world cells, not to the camera: moving the eye leaves every flake seen from both positions where
// it was (no sliding with the camera), and the same time draws the same flakes however the camera got there.
TEST(FxPrecip, WorldAnchoredNotViewLocked) {
    ou::fx::Precip p;
    for (int i = 0; i < 20; ++i) p.updateCover({0, 0, 0}, flatCover(-5000), 1 << 16);
    auto centres = [&](glm::vec3 eye) {
        std::vector<ou::render::FxVertex> v;
        std::vector<ou::render::FxBatch> b;
        p.build(frameAt(eye, ou::fx::Precip::Kind::Snow), v, b);
        EXPECT_FALSE(b.empty());
        return b.empty() ? std::vector<glm::vec3>{} : quadCentres(v, b[0]);
    };
    const auto a = centres({0, 0, 0}), b = centres({120, 40, 0});
    ASSERT_GT(a.size(), 1000u);
    size_t shared = 0, found = 0;
    for (const glm::vec3& c : a) {
        if (glm::distance(c, glm::vec3(0, 400, 0)) > 250) continue; // well inside both views and radii
        ++shared;
        found += std::any_of(b.begin(), b.end(), [&](const glm::vec3& d) { return glm::distance(c, d) < 0.01f; });
    }
    ASSERT_GT(shared, 50u);
    EXPECT_EQ(found, shared);
    // Time moves them (falling), the camera does not.
    std::vector<ou::render::FxVertex> v;
    std::vector<ou::render::FxBatch> bb;
    p.build(frameAt({0, 0, 0}, ou::fx::Precip::Kind::Snow, 10.5), v, bb);
    const auto later = quadCentres(v, bb[0]);
    size_t same = 0;
    for (size_t i = 0; i < std::min<size_t>(200, a.size()); ++i)
        same += std::any_of(later.begin(), later.end(), [&](const glm::vec3& d) { return glm::distance(a[i], d) < 0.01f; });
    EXPECT_LT(same, 5u);
}

// Rain drops are short velocity-aligned quads (a few cm, not streaks across the view) and drawn near the camera too;
// under a roof (the cover grid's top surface above them) there are none, and drops reaching the ground splash there.
TEST(FxPrecip, DropsCoverAndSplashes) {
    ou::fx::Precip p;
    for (int i = 0; i < 20; ++i) p.updateCover({0, 0, 0}, flatCover(-150, 400), 1 << 16);
    std::vector<ou::render::FxVertex> v;
    std::vector<ou::render::FxBatch> b;
    p.build(frameAt({0, 0, 0}, ou::fx::Precip::Kind::Rain), v, b);
    ASSERT_GE(b.size(), 2u);
    EXPECT_EQ(b[0].textureId, ou::fx::kPrecipDropTexture);
    size_t near = 0;
    for (uint32_t i = b[0].first; i + 6 <= b[0].first + b[0].count; i += 6) {
        const glm::vec3 c0 = glm::make_vec3(v[i].pos), c2 = glm::make_vec3(v[i + 2].pos);
        EXPECT_LT(glm::distance(c0, c2), 25.0f); // at most ~20 units (20 cm) head to tail
        const glm::vec3 c = (c0 + c2) * 0.5f;
        if (c.x < -20.0f) EXPECT_GE(c.z, 380.0f) << "a drop under the roof (x < 0, roof at 400)";
        EXPECT_GE(c.z, -151.0f) << "a drop below the ground";
        near += glm::length(c) < 250.0f;
    }
    EXPECT_GT(near, 10u); // half the space near the eye is under the roof
    const auto ring = std::find_if(b.begin(), b.end(), [](const auto& x) { return x.textureId == ou::fx::kPrecipRingTexture; });
    ASSERT_NE(ring, b.end());
    for (const glm::vec3& c : quadCentres(v, *ring)) // on the ground, or on the roof where it covers (x < 0)
        EXPECT_NEAR(c.z, c.x < 0 ? 400.6f : -149.4f, 0.01f);
    // Columns not queried yet show nothing (no rain inside before the grid knows the roofs).
    ou::fx::Precip empty;
    v.clear(), b.clear();
    empty.build(frameAt({0, 0, 0}, ou::fx::Precip::Kind::Rain), v, b);
    EXPECT_TRUE(b.empty());
}

// PrecipWorker (the off-main-thread build): each collect() returns exactly what Precip would have built for the job the
// previous frame submitted (cover queries then build, in submit order), with the probed column's cover.
TEST(FxPrecip, WorkerMatchesDirectBuildOneFrameBehind) {
    ou::fx::Precip direct;
    ou::fx::PrecipWorker worker;
    EXPECT_FALSE(worker.collect().valid); // nothing submitted yet
    const auto query = flatCover(-150, 400);
    std::vector<ou::render::FxVertex> expectV;
    std::vector<ou::render::FxBatch> expectB;
    for (int frame = 0; frame < 8; ++frame) {
        const auto& r = worker.collect();
        if (frame > 0) {
            ASSERT_TRUE(r.valid);
            ASSERT_EQ(r.vertices.size(), expectV.size()) << frame;
            EXPECT_EQ(0, std::memcmp(r.vertices.data(), expectV.data(), expectV.size() * sizeof(expectV[0]))) << frame;
            ASSERT_EQ(r.batches.size(), expectB.size());
            for (size_t i = 0; i < expectB.size(); ++i) {
                EXPECT_EQ(r.batches[i].first, expectB[i].first);
                EXPECT_EQ(r.batches[i].count, expectB[i].count);
            }
            ASSERT_EQ(r.probes.size(), 1u);
            ASSERT_TRUE(r.probes[0]);
            EXPECT_EQ(*r.probes[0], -150.0f);
        }
        auto f = frameAt({frame * 40.0f, 0, 0}, ou::fx::Precip::Kind::Rain);
        f.time += frame / 60.0;
        direct.updateCover(f.eye, query, 512);
        expectV.clear(), expectB.clear();
        direct.build(f, expectV, expectB);
        worker.submit({f, query, 512, {glm::vec2(f.eye) + glm::vec2(8)}}); // the eye's column: queried first
    }
}

// Snow on Lara builds while she stands in snowfall, fades slowly while she walks, and running / landing sheds clumps
// that fall and settle on the ground the query reports.
TEST(FxSnowCoat, BuildsFadesAndSheds) {
    ou::fx::SnowCoat c;
    const auto ground = [](const glm::vec3&) -> std::optional<float> { return 0.0f; };
    ou::fx::SnowCoat::Input in;
    in.dt = 1.0f / 60, in.snowfall = 1;
    for (int i = 0; i < 60 * 60; ++i) in.time += in.dt, c.tick(in, ground);
    EXPECT_GT(c.coverage()[0], 0.9f);
    EXPECT_GT(c.coverage()[0], c.coverage()[3]); // the head and shoulders collect more than the legs
    const float full = c.coverage()[0];
    in.snowfall = 0, in.velocity = {150, 0, 0}; // walking out of the snow: slow fade, no clumps
    for (int i = 0; i < 60 * 5; ++i) in.time += in.dt, in.position += in.velocity * in.dt, c.tick(in, ground);
    EXPECT_LT(c.coverage()[0], full);
    EXPECT_GT(c.coverage()[0], 0.4f * full);
    EXPECT_EQ(c.clumps(), 0u);
    in.velocity = {600, 0, 0}; // running
    for (int i = 0; i < 30; ++i) in.time += in.dt, in.position += in.velocity * in.dt, c.tick(in, ground);
    EXPECT_GT(c.clumps(), 0u);
    for (int i = 0; i < 90; ++i) in.time += in.dt, in.velocity = {}, c.tick(in, ground); // they fall and settle
    std::vector<ou::render::FxVertex> v;
    std::vector<ou::render::FxBatch> b;
    c.build(frameAt({0, -500, 100}, ou::fx::Precip::Kind::Snow), v, b);
    ASSERT_EQ(b.size(), 1u);
    for (const glm::vec3& q : quadCentres(v, b[0])) EXPECT_NEAR(q.z, 0.4f, 0.5f);
}

// tru.exe 0x43CC10 -> 0x5B26C0: each clip record fires once, when an advance's [time before, time after) reaches its
// frame; the lead before frame 0 fires nothing; a restart counts from its start (no rest of the old pass) [O decomp].
TEST(Fx, ClipEventSpansFireEachFrameOnceFromTheClipStart) {
    const auto fired = [](const std::vector<std::pair<float, float>>& spans, int frame) {
        int n = 0;
        for (const auto& [a, z] : spans) n += float(frame) >= a && float(frame) < z;
        return n;
    };
    const double dt = 1.0 / 60.0, dur = 20.0 / 30.0; // a once clip of 20 frames, 60 Hz ticks
    // A chart request's lead: start - blend = -3 frames; time = start + advance on its first tick, clamped at the end.
    for (const double start : {-3.0 / 30.0, 0.0, 5.0 / 30.0}) {
        std::map<int, int> count;
        double prev = 0, t = start + dt;
        for (int tick = 0; tick < 80; ++tick) {
            const auto s = ou::fx::clipEventSpans(prev, t, dur, tick > 0, tick == 0 ? std::optional<double>(start) : std::nullopt);
            for (int f = 0; f <= 20; ++f) count[f] += fired(s, f);
            prev = t, t = std::min(t + dt, dur);
        }
        for (int f = 0; f <= 20; ++f) EXPECT_EQ(count[f], f >= std::lround(std::max(start, 0.0) * 30) ? 1 : 0) << "start " << start << " frame " << f;
    }
    // No record of the lead: still before frame 0 after the start tick.
    EXPECT_TRUE(ou::fx::clipEventSpans(-0.1, -0.0833, dur, true, std::nullopt).empty());
    EXPECT_TRUE(ou::fx::clipEventSpans(0, -0.0833, dur, false, -0.1).empty());
    // Restarted (S431 <-> S432 mirror swap of ledgeb_right_3) half way: from its start, not the old pass's rest.
    const auto r = ou::fx::clipEventSpans(0.5, 1.0 / 60.0, dur, true, 0.0);
    for (int f = 1; f <= 20; ++f) EXPECT_EQ(fired(r, f), 0) << f;
    EXPECT_EQ(fired(r, 0), 1);
    // A loop wraps: the rest of the cycle, then its start.
    const auto w = ou::fx::clipEventSpans(dur - dt / 2, dur + dt / 2, dur, true, std::nullopt);
    EXPECT_EQ(fired(w, 0), 1);
    EXPECT_EQ(fired(w, 19), 0);
}

// Enemies and script objects (no `start`): a new clip standing at 0 fires frame 0 once, on its first advance (it fired
// twice before: at 0 and again from 0); a script object's loop clock (fmod, World::tickUber) wraps.
TEST(Fx, ClipEventSpansNewClipAtZeroAndWrappedObjectLoop) {
    const auto fired = [](const std::vector<std::pair<float, float>>& spans, int frame) {
        int n = 0;
        for (const auto& [a, z] : spans) n += float(frame) >= a && float(frame) < z;
        return n;
    };
    const double dt = 1.0 / 60.0, dur = 20.0 / 30.0;
    int zero = 0;
    zero += fired(ou::fx::clipEventSpans(0.3, 0.0, dur, false, std::nullopt), 0); // new clip, standing at 0
    zero += fired(ou::fx::clipEventSpans(0.0, dt, dur, true, std::nullopt), 0);   // its first advance
    zero += fired(ou::fx::clipEventSpans(dt, 2 * dt, dur, true, std::nullopt), 0);
    EXPECT_EQ(zero, 1);
    const auto w = ou::fx::clipEventSpans(dur - dt / 2, dt / 2, dur, true, std::nullopt); // fmod'd clock went back
    EXPECT_EQ(fired(w, 19), 0);
    EXPECT_EQ(fired(w, 0), 1);
    EXPECT_EQ(fired(w, 20), 1);
    // a restarted enemy clip (start 0): nothing of the old pass
    const auto r = ou::fx::clipEventSpans(0.5, dt, dur, true, 0.0);
    EXPECT_EQ(fired(r, 0), 1);
    EXPECT_EQ(fired(r, 16), 0);
}
