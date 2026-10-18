// ou_script: trigger shapes, event dispatch, switches/doors/blocks, save state; real-data parsing of every
// unit's script sections and trigger intros (skipped without OU_GAME_DIR).
#include "script/Script.hpp"

#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace ou;
using namespace ou::script;
using glm::vec3;

namespace {

Object volume(int32_t uid, vec3 pos, vec3 half, std::map<int16_t, std::vector<int32_t>> links = {}) {
    Object o;
    o.uid = uid, o.kind = Kind::Volume, o.pos = pos, o.shape = {ShapeKind::Box, half, 0, false}, o.links = std::move(links);
    return o;
}
Object door(int32_t uid) {
    Object o;
    o.uid = uid, o.kind = Kind::Door, o.pos = {5000, 0, 0};
    return o;
}
// Lara's root: the exe tests root + (0, 0, 200).
TickInput at(vec3 p) { return {p - vec3(0, 0, kTestPointHeight), 0, {0, 0}, false}; }

std::vector<std::pair<EventKind, int32_t>> kinds(const Runtime& r) {
    std::vector<std::pair<EventKind, int32_t>> out;
    for (const auto& e : r.events()) out.push_back({e.kind, e.kind == EventKind::Activate ? e.target : e.source});
    return out;
}

} // namespace

TEST(ScriptShapes, BoxSphereRotationAndPlane) {
    const Shape box{ShapeKind::Box, {100, 50, 10}, 0, false};
    EXPECT_TRUE(inside(box, {0, 0, 0}, {0, 0, 0}, {90, 40, 5}));
    EXPECT_FALSE(inside(box, {0, 0, 0}, {0, 0, 0}, {40, 90, 5}));
    // Rotated 90 degrees about Z: local x runs along world y.
    EXPECT_TRUE(inside(box, {0, 0, 0}, {0, 0, 1.5707964f}, {40, 90, 5}));
    EXPECT_FALSE(inside(box, {0, 0, 0}, {0, 0, 1.5707964f}, {90, 40, 5}));
    const Shape ball{ShapeKind::Sphere, {}, 150, false};
    EXPECT_TRUE(inside(ball, {10, 10, 10}, {}, {100, 10, 10}));
    EXPECT_FALSE(inside(ball, {10, 10, 10}, {}, {170, 10, 10}));

    // Plane: local XZ rectangle, normal local +Y.
    Shape plane{ShapeKind::Plane, {100, 0, 50}, 0, false};
    EXPECT_FALSE(inside(plane, {}, {}, {0, 0, 0}));
    EXPECT_TRUE(crosses(plane, {}, {}, {0, 10, 0}, {0, -10, 0}));
    EXPECT_TRUE(crosses(plane, {}, {}, {0, -10, 0}, {0, 10, 0}));
    EXPECT_FALSE(crosses(plane, {}, {}, {0, 10, 0}, {0, 5, 0}));
    EXPECT_FALSE(crosses(plane, {}, {}, {150, 10, 0}, {150, -10, 0})); // outside the rectangle
    plane.oneSided = true;
    EXPECT_TRUE(crosses(plane, {}, {}, {0, 10, 0}, {0, -10, 0}));
    EXPECT_FALSE(crosses(plane, {}, {}, {0, -10, 0}, {0, 10, 0}));
}

TEST(ScriptRuntime, VolumeEnterExitAndDispatchOrder) {
    Runtime r;
    r.addObject(volume(1, {0, 0, 0}, {100, 100, 100}, {{kVtOnFirstIn, {10}}, {kVtOnIn, {11, 12}}, {kVtOnOut, {13}}}));
    for (const int32_t uid : {10, 11, 12, 13}) r.addObject(door(uid));

    r.tick(at({500, 0, 0}));
    EXPECT_TRUE(r.events().empty());
    r.tick(at({50, 0, 0})); // enter: Enter, FirstIn + its calls, In + its calls (base script 3484 order)
    using K = EventKind;
    EXPECT_EQ(kinds(r), (std::vector<std::pair<K, int32_t>>{{K::Enter, 1}, {K::FirstIn, 1}, {K::Activate, 10},
                                                              {K::In, 1}, {K::Activate, 11}, {K::Activate, 12}}));
    EXPECT_EQ(r.find(10)->state, 2); // clip-less door opens at once
    r.tick(at({60, 0, 0}));
    EXPECT_TRUE(r.events().empty()); // still inside
    r.tick(at({500, 0, 0}));
    EXPECT_EQ(kinds(r), (std::vector<std::pair<K, int32_t>>{{K::Out, 1}, {K::Activate, 13}}));
    r.tick(at({0, 0, 0})); // second entry: no FirstIn
    EXPECT_EQ(kinds(r), (std::vector<std::pair<K, int32_t>>{{K::Enter, 1}, {K::In, 1}, {K::Activate, 11}, {K::Activate, 12}}));
}

TEST(ScriptRuntime, PlaneCrossSwitchUseAndBlockPush) {
    Runtime r;
    Object plane;
    plane.uid = 2, plane.kind = Kind::Plane, plane.shape = {ShapeKind::Plane, {100, 0, 300}, 0, false};
    plane.links[kVtPlaneCross] = {20};
    r.addObject(plane);
    r.addObject(door(20));
    Object sw;
    sw.uid = 3, sw.kind = Kind::Switch, sw.pos = {1000, 0, 0}, sw.links[kVtSwitchUsed] = {21};
    r.addObject(sw);
    r.addObject(door(21));
    Object block;
    block.uid = 4, block.kind = Kind::Block, block.pos = {0, 2000, 0};
    r.addObject(block);

    r.tick({{0, 50, 0}, 0, {}, false});
    r.tick({{0, -50, 0}, 0, {}, false});
    ASSERT_EQ(r.events().size(), 2u);
    EXPECT_EQ(r.events()[0].kind, EventKind::Cross);
    EXPECT_EQ(r.events()[1].target, 20);

    r.tick({{900, 0, 0}, 0, {}, true}); // press near the switch
    ASSERT_EQ(r.events().size(), 2u);
    EXPECT_EQ(r.events()[0].kind, EventKind::Use);
    EXPECT_EQ(r.find(21)->state, 2);
    r.tick({{900, 0, 0}, 0, {}, true}); // held: no second use
    EXPECT_TRUE(r.events().empty());
    EXPECT_EQ(r.find(3)->state, 2);

    // Facing +Y (yaw 90 deg) next to the block: push moves it along +Y, pull back toward Lara.
    const TickInput push{{0, 1850, 0}, 1.5707964f, {0, 1}, true};
    for (int i = 0; i < 60; ++i) r.tick(push);
    EXPECT_NEAR(r.find(4)->offset.y, 120, 1);
    EXPECT_EQ(r.find(4)->offset.x, 0);
    ASSERT_TRUE(r.pose(*r.find(4)));
    TickInput pull = push;
    pull.move.y = -1;
    for (int i = 0; i < 30; ++i) r.tick(pull);
    EXPECT_NEAR(r.find(4)->offset.y, 60, 1);
}

TEST(ScriptRuntime, StateRoundTrip) {
    Runtime r;
    r.addObject(volume(1, {0, 0, 0}, {100, 100, 100}, {{kVtOnIn, {10}}}));
    r.addObject(door(10));
    Object block;
    block.uid = 4, block.kind = Kind::Block, block.offset = {1, 2, 3};
    r.addObject(block);
    r.tick(at({0, 0, 0}));
    const auto saved = r.serialize();
    EXPECT_TRUE(saved.contains("1") && saved.contains("10") && saved.contains("4"));

    r.tick(at({500, 0, 0}));
    EXPECT_FALSE(r.find(1)->inside);
    r.restore(saved);
    EXPECT_TRUE(r.find(1)->inside && r.find(1)->firstIn);
    EXPECT_EQ(r.find(10)->state, 2);
    EXPECT_EQ(r.find(4)->offset, vec3(1, 2, 3));
    EXPECT_EQ(r.serialize(), saved);
    r.tick(at({0, 0, 0})); // restored inside: no re-entry
    EXPECT_TRUE(r.events().empty());
    r.restore(nlohmann::json::object()); // empty save: everything back to birth state
    EXPECT_FALSE(r.find(1)->inside);
    EXPECT_EQ(r.find(10)->state, 0);
}

TEST(ScriptRuntime, RestoreBeforeLoadAndStreamOut) {
    Runtime r;
    r.restore({{"10", {{"state", 2}}}, {"99", {{"state", 2}}}}); // 99: a unit that is not loaded
    r.addObject(door(10));                                       // loads later (setUnits / streaming)
    EXPECT_EQ(r.find(10)->state, 2);
    const auto j = r.serialize();
    EXPECT_TRUE(j.contains("10") && j.contains("99"));
    r.setUnits({}); // every unit streamed out: state kept
    EXPECT_EQ(r.find(10), nullptr);
    EXPECT_EQ(r.serialize(), j);
}

// ---- Real data ---------------------------------------------------------------------------------------
TEST(ScriptRealData, AllUnitsParse) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(list);
    std::string text(list->begin(), list->end());
    size_t units = 0, scripts = 0, volumes = 0, planes = 0, switches = 0, doors = 0, linked = 0, animated = 0;
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string name = line.substr(line.rfind('\\') + 1);
        auto bytes = bf.read("pc-w\\" + name + ".drm");
        if (!bytes) continue; // 4 listed units are not in the bigfile (LEVELS.md)
        ++units;
        auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)));
        for (uint32_t i = 0; i < drm->sections.size(); ++i)
            if (drm->sections[i].type == cdc::SectionType::Script) {
                EXPECT_NO_THROW(parseScriptType(*drm, i)) << name << " section " << i;
                ++scripts;
            }
        Runtime r;
        r.setBigfile(&bf);
        ASSERT_NO_THROW(r.setUnits({{name, drm, {}}})) << name;
        for (const auto& o : r.objects()) {
            volumes += o.kind == Kind::Volume, planes += o.kind == Kind::Plane;
            switches += o.kind == Kind::Switch, doors += o.kind == Kind::Door;
            linked += !o.links.empty(), animated += bool(o.clips);
        }
    }
    std::printf("%zu units, %zu script sections, %zu volumes, %zu planes, %zu switches, %zu doors, %zu linked, %zu animated\n",
                units, scripts, volumes, planes, switches, doors, linked, animated);
    EXPECT_EQ(units, 82u);
    EXPECT_GT(volumes, 1000u);
    EXPECT_GT(planes, 100u);
}

TEST(ScriptRealData, BedroomSwitchOpensHouseDoor) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    auto bytes = bf.read("pc-w\\l3_main_bedroom.drm");
    ASSERT_TRUE(bytes);
    Runtime r;
    r.setBigfile(&bf);
    r.setUnits({{"l3_main_bedroom", std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes))), {}}});
    const Object* sw = r.find(4525);   // ma_ei_wall_switch_electric
    const Object* door = r.find(4520); // ma_ei_house_door
    ASSERT_TRUE(sw && door);
    EXPECT_EQ(sw->kind, Kind::Switch);
    EXPECT_EQ(door->kind, Kind::Door);
    ASSERT_TRUE(sw->links.contains(kVtSwitchUsed));
    EXPECT_EQ(sw->links.at(kVtSwitchUsed).front(), 4520); // instanceref member init, script 12131
    ASSERT_TRUE(sw->clips && door->clips);

    r.tick({sw->pos, 0, {}, true});
    EXPECT_EQ(sw->state, 1); // pulling
    for (int i = 0; i < 600 && door->state != 2; ++i) r.tick({sw->pos, 0, {}, true});
    EXPECT_EQ(sw->state, 2);
    EXPECT_GE(door->state, 1);
    EXPECT_TRUE(r.pose(*door));

    // l4_pool trigger volume 5513: box half extents 567 x 553 x 248 (intro data +0x34).
    auto pool = bf.read("pc-w\\l4_pool.drm");
    ASSERT_TRUE(pool);
    Runtime p;
    p.setBigfile(&bf);
    p.setUnits({{"l4_pool", std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*pool))), {}}});
    const Object* v = p.find(5513);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->shape.kind, ShapeKind::Box);
    EXPECT_NEAR(v->shape.half.x, 566.96f, 0.1f);
    EXPECT_NEAR(v->shape.half.z, 247.7f, 0.1f);
}
