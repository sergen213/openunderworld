// Original particle effects: definition parser, deterministic simulation, sprite batches (docs/formats/EFFECTS.md).
#include "formats/Formats.hpp"
#include "fx/Fx.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <memory>

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
    EXPECT_EQ(textured, 1908); // per-object distinct particle textures, all present in the object DRM
}
