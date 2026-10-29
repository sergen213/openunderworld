// ou_script: trigger shapes, event dispatch, switches/doors/blocks, save state; real-data parsing of every
// unit's script sections and trigger intros (skipped without OU_GAME_DIR).
#include "script/Script.hpp"
#include "script/Vm.hpp"

#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdlib>
#include <set>
#include <tuple>

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
    Object scripted = door(11);
    scripted.script = std::make_shared<vm::Instance>();
    scripted.script->fields[8] = 1;
    r.addObject(scripted);
    r.tick(at({0, 0, 0}));
    const auto saved = r.serialize();
    EXPECT_TRUE(saved.contains("1") && saved.contains("10") && saved.contains("4") && saved.contains("11"));

    r.tick(at({500, 0, 0}));
    EXPECT_FALSE(r.find(1)->inside);
    r.restore(saved);
    EXPECT_TRUE(r.find(1)->inside && r.find(1)->firstIn);
    EXPECT_EQ(r.find(10)->state, 2);
    EXPECT_EQ(r.find(4)->offset, vec3(1, 2, 3));
    EXPECT_EQ(r.find(11)->script->fields.at(8), 1u);
    EXPECT_EQ(r.serialize(), saved);
    r.tick(at({0, 0, 0})); // restored inside: no re-entry
    EXPECT_TRUE(r.events().empty());
    r.restore(nlohmann::json::object()); // empty save: everything back to birth state
    EXPECT_FALSE(r.find(1)->inside);
    EXPECT_EQ(r.find(10)->state, 0);
    EXPECT_EQ(r.find(11)->script->fields.count(8), 0u);
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

TEST(ScriptRuntime, VolumeHandlerSleepsAcrossTicks) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    Instr delay; delay.op = 0x02; delay.imm = std::bit_cast<uint32_t>(0.02f);
    ScriptType type;
    type.id = 100;
    Function onIn;
    onIn.vtIndex = kVtOnIn; onIn.self = {2, 0x64}; onIn.localCount = 1;
    onIn.code = {ins(0x0F, 4), ins(0x04, 1), ins(0x2F), delay, ins(0x80),
                 ins(0x0F, 8), ins(0x04, 2), ins(0x2F), ins(0x7B)};
    type.functions = {onIn};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    Runtime r;
    auto v = volume(1, {0, 0, 0}, {100, 100, 100});
    v.script = program->instantiate(v.uid, type.id);
    r.addObject(std::move(v));
    r.tick(at({0, 0, 0}));
    const Object* object = r.find(1);
    ASSERT_TRUE(object && object->script);
    EXPECT_EQ(object->script->fields.at(4), 1u);
    EXPECT_FALSE(object->script->fields.contains(8));
    r.tick(at({0, 0, 0}));
    EXPECT_FALSE(object->script->fields.contains(8));
    r.tick(at({0, 0, 0}));
    EXPECT_EQ(object->script->fields.at(8), 2u);
}

TEST(ScriptRuntime, StringFieldsRoundTrip) {
    Runtime r;
    auto v = volume(1, {0, 0, 0}, {100, 100, 100});
    ScriptType type;
    type.id = 100;
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    v.script = program->instantiate(v.uid, 100);
    v.script->fields[4] = 1;
    v.script->strings[4] = "open";
    v.script->properties[(8ull << 32) | 7] = 1;
    r.addObject(std::move(v));
    const auto saved = r.serialize();
    Runtime restored;
    auto copy = volume(1, {0, 0, 0}, {100, 100, 100});
    copy.script = program->instantiate(copy.uid, 100);
    copy.script->fields[4] = 1;
    copy.script->strings[4] = "closed";
    restored.addObject(std::move(copy));
    restored.restore(saved);
    const Object* object = restored.find(1);
    ASSERT_TRUE(object && object->script);
    EXPECT_EQ(object->script->strings.at(4), "open");
    EXPECT_EQ(object->script->properties.at((8ull << 32) | 7), 1u);
}

// ---- Bytecode VM --------------------------------------------------------------------------------------
TEST(ScriptVm, ArithmeticRawWordBranchAndNativeCall) {
    auto ins = [](uint8_t op, int32_t arg, uint32_t word) {
        Instr i; i.op = op; i.arg = arg; i.word = word; return i;
    };
    ScriptType native;
    native.id = 69; native.nativeName = "stickybool";
    Function set;
    set.self = {1, 0x54}; set.native = true; set.argCount = 1;
    native.functions.push_back(set);

    ScriptType type;
    type.id = 100;
    Function main;
    main.vtIndex = 54; main.self = {2, 0x64}; main.localCount = 1; main.wordCount = 14;
    for (bool condition : {false, true}) {
        main.code = {ins(0x0F, 4, 0), ins(0x03, 7, 1), ins(0x04, 3, 2), ins(0x40, 0, 3), ins(0x03, 2, 4),
                     ins(0x3E, 0, 5), ins(0x2F, 0, 6), ins(0x04, condition, 7),
                     ins(0x64, 4, 8), ins(0x04, 1, 9), ins(0x0F, 8, 10), ins(0x65, 0, 11), ins(0x7B, 0, 13)};
        main.code[11].ptrs = {set.self};
        type.functions = {main};
        auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{native, type});
        auto object = program->instantiate(1, 100);
        int calls = 0;
        vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext& nc) {
            ++calls; object->fields[nc.receiver.offset] = nc.args[0].bits; return true;
        });
        const bool completed = machine.call(*object, 54);
        EXPECT_EQ(object->fields[4], 23u);
        EXPECT_EQ(object->fields[8], condition ? 1u : 0u);
        EXPECT_EQ(calls, condition ? 1 : 0);
        ASSERT_TRUE(completed);
    }
}

// ---- Real data ---------------------------------------------------------------------------------------
TEST(ScriptVm, StringCopyCompareAndConvert) {
    auto ins = [](uint8_t op, int32_t arg, std::string text = {}) {
        Instr i; i.op = op; i.arg = arg; i.text = std::move(text); if (op == 0x05) i.ptrs = {{1, 0x100}}; return i;
    };
    ScriptType type;
    type.id = 100;
    Member stringMember;
    stringMember.type = 4; stringMember.offset = 4;
    type.members = {stringMember};
    Function main;
    main.vtIndex = 54; main.self = {2, 0x64}; main.localCount = 1;
    main.code = {ins(0x0F, 4), ins(0x05, 0, "123"), ins(0x31, 0),
                 ins(0x0F, 8), ins(0x0E, 4), ins(0x05, 0, "123"), ins(0x4F, 0), ins(0x2F, 0),
                 ins(0x0F, 12), ins(0x0E, 4), ins(0x60, 0), ins(0x2F, 0), ins(0x7B, 0)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto object = program->instantiate(1, 100);
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54));
    EXPECT_EQ(object->fields[8], 1u);
    EXPECT_EQ(object->fields[12], 123u);
}

TEST(ScriptVm, MoveUCopiesNativeWidthAndReferences) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType spline;
    spline.id = 900; spline.nativeName = "cameraspline";
    ScriptType instanceRef;
    instanceRef.id = 901; instanceRef.nativeName = "instanceref";
    ScriptType targetType;
    targetType.id = 200;
    Member targetField; targetField.type = 2; targetField.offset = 4;
    targetType.members = {targetField};
    ScriptType type;
    type.id = 100;
    Member splineMember; splineMember.type = 10; splineMember.typeId = spline.id; splineMember.offset = 16;
    Member sourceRef; sourceRef.type = 10; sourceRef.typeId = instanceRef.id; sourceRef.offset = 28;
    type.members = {splineMember, sourceRef};
    Function main;
    main.vtIndex = 54; main.self = {2, 0x64}; main.localCount = 16;
    auto moveSpline = ins(0x34); moveSpline.ref = spline.id;
    auto moveRef = ins(0x34); moveRef.ref = instanceRef.id;
    main.code = {ins(0x09, 0), ins(0x03, 0x112233), ins(0x2F),
                 ins(0x09, 4), ins(0x03, 0x556677), ins(0x2F),
                 ins(0x09, 8), ins(0x10, 28), ins(0x30),
                 ins(0x0F, 16), ins(0x09, 0), moveSpline,
                 ins(0x09, 12), ins(0x09, 8), moveRef,
                 ins(0x0A, 12), ins(0x14, 4), ins(0x03, 7), ins(0x2F), ins(0x7B)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{spline, instanceRef, type, targetType});
    auto object = program->instantiate(1, 100);
    auto target = program->instantiate(2, 200);
    object->fields[28] = 2;
    vm::Machine machine([&](int32_t uid) { return uid == 2 ? target : object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54));
    EXPECT_EQ(object->fields.at(16), 0x112233u);
    EXPECT_EQ(object->fields.at(20), 0x556677u);
    EXPECT_EQ(target->fields.at(4), 7u);
}

TEST(ScriptRealData, MoveUClockMemberFromLocal) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    auto bytes = bf.read("pc-w\\l4_ballcourt.drm");
    ASSERT_TRUE(bytes);
    const auto drm = cdc::parseDrm(std::move(*bytes));
    std::vector<ScriptType> types;
    for (uint32_t i = 0; i < drm.sections.size(); ++i)
        if (drm.sections[i].type == cdc::SectionType::Script) types.push_back(parseScriptType(drm, i));
    auto program = std::make_shared<vm::Program>(std::move(types));
    auto object = program->instantiate(1, 13909);
    ASSERT_TRUE(object->program->type(125));
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    const vm::Value clock = vm::Value::scalar(1);
    ASSERT_TRUE(machine.call(*object, 65, {&clock, 1})); // vt 65(clock): MOVEU member 36 <- argument at local 0
    EXPECT_EQ(object->fields.at(36), 1u);
}

TEST(ScriptVm, SleepResumesNestedHandler) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    auto pushFloat = [](float value) { Instr i; i.op = 0x02; i.imm = std::bit_cast<uint32_t>(value); return i; };
    ScriptType type;
    type.id = 100;
    Function child;
    child.self = {2, 0x80}; child.localCount = 1;
    child.code = {pushFloat(0.02f), ins(0x80), ins(0x0F, 16), ins(0x04, 7), ins(0x2F), ins(0x7B)};
    Function main;
    main.vtIndex = 54; main.self = {2, 0x64}; main.localCount = 1;
    auto call = ins(0x66); call.ptrs = {child.self};
    main.code = {ins(0x0F, 4), ins(0x04, 1), ins(0x2F), call,
                 ins(0x0F, 8), ins(0x04, 3), ins(0x2F), ins(0x7B)};
    type.functions = {child, main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto object = program->instantiate(1, 100);
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54));
    ASSERT_TRUE(machine.active());
    EXPECT_EQ(object->fields[4], 1u);
    EXPECT_FALSE(object->fields.contains(8));
    EXPECT_FALSE(object->fields.contains(16));
    ASSERT_TRUE(machine.tick(0.01f));
    EXPECT_TRUE(machine.active());
    EXPECT_FALSE(object->fields.contains(16));
    ASSERT_TRUE(machine.tick(0.02f));
    EXPECT_FALSE(machine.active());
    EXPECT_EQ(object->fields[16], 7u);
    EXPECT_EQ(object->fields[8], 3u);
}

TEST(ScriptVm, SequenceReplacesParentHandler) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType type;
    type.id = 100;
    Function child;
    child.self = {2, 0x80}; child.localCount = 1;
    child.code = {ins(0x0F, 16), ins(0x04, 7), ins(0x2F), ins(0x7B)};
    Function main;
    main.vtIndex = 54; main.self = {2, 0x64}; main.localCount = 1;
    auto sequence = ins(0x6B); sequence.ptrs = {child.self};
    main.code = {ins(0x0F, 4), ins(0x04, 1), ins(0x2F), ins(0x0A, 0), sequence,
                 ins(0x0F, 8), ins(0x04, 3), ins(0x2F), ins(0x7B)};
    type.functions = {child, main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto object = program->instantiate(1, 100);
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54));
    EXPECT_FALSE(machine.active());
    EXPECT_EQ(object->fields[4], 1u);
    EXPECT_EQ(object->fields[16], 7u);
    EXPECT_FALSE(object->fields.contains(8));
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
    ASSERT_TRUE(door->script);
    EXPECT_EQ(door->script->fields.at(8), 1u); // 12171 F0: stickybool.Set(true), not the legacy link fallback
    EXPECT_EQ(door->script->fields.at(4), 0u); // modifiable.Set(0) = open
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

TEST(ScriptRealData, PlayerRefReturnsLaraScript) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    ScriptType playerRef;
    playerRef.id = 208; playerRef.nativeName = "playerref";
    Function get;
    get.self = {1, 0x54}; get.native = true; get.returnType = 11;
    playerRef.functions = {get};
    ScriptType type;
    type.id = 100;
    Function onIn;
    onIn.vtIndex = kVtOnIn; onIn.self = {2, 0x64}; onIn.localCount = 1;
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    Instr call = ins(0x65); call.ptrs = {get.self};
    onIn.code = {ins(0x0F, 4), ins(0x0F, 8), call, ins(0x7B)};
    type.functions = {onIn};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{playerRef, type});
    Runtime r;
    r.setBigfile(&bf);
    auto v = volume(1, {0, 0, 0}, {100, 100, 100});
    v.script = program->instantiate(v.uid, 100);
    r.addObject(std::move(v));
    r.tick(at({0, 0, 0}));
    const Object* object = r.find(1);
    ASSERT_TRUE(object && object->script);
    ASSERT_TRUE(r.playerScript());
    EXPECT_EQ(r.playerScript()->typeId, 205u);
    EXPECT_EQ(object->script->fields.at(4), uint32_t(r.playerScript()->uid));
}

TEST(ScriptRealData, InstanceRefWaitContinuesWhenTargetLoaded) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    for (const uint32_t function : {3u, 6u}) {
        ScriptType instanceRef;
        instanceRef.id = 51; instanceRef.nativeName = "instanceref";
        Function wait;
        wait.self = {1, 0x54}; wait.native = true;
        instanceRef.functions.resize(function + 1);
        instanceRef.functions[function] = wait;
        ScriptType type;
        type.id = 100;
        Function onIn;
        onIn.vtIndex = kVtOnIn; onIn.self = {2, 0x64}; onIn.localCount = 1;
        auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
        Instr call = ins(0x65); call.ptrs = {wait.self};
        onIn.code = {ins(0x0F, 8), call, ins(0x0F, 4), ins(0x04, 1), ins(0x2F), ins(0x7B)};
        type.functions = {onIn};
        auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{instanceRef, type});
        Runtime r;
        r.setBigfile(&bf);
        auto target = door(10);
        target.script = program->instantiate(target.uid, 100);
        r.addObject(std::move(target));
        auto v = volume(1, {0, 0, 0}, {100, 100, 100});
        v.script = program->instantiate(v.uid, 100);
        v.script->fields[8] = 10;
        r.addObject(std::move(v));
        r.tick(at({0, 0, 0}));
        const Object* object = r.find(1);
        ASSERT_TRUE(object && object->script);
        EXPECT_EQ(object->script->fields.at(4), 1u) << "function " << function;
    }
}

TEST(ScriptRealData, InstanceRefPropertySetGet) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    ScriptType instanceRef;
    instanceRef.id = 51; instanceRef.nativeName = "instanceref";
    instanceRef.functions.resize(16);
    Function set, get;
    set.self = {1, 0x54}; set.native = true; set.argCount = 2;
    get.self = {1, 0x64}; get.native = true; get.argCount = 1; get.returnType = 1;
    instanceRef.functions[9] = set;
    instanceRef.functions[15] = get;
    ScriptType type;
    type.id = 100;
    Function onIn;
    onIn.vtIndex = kVtOnIn; onIn.self = {2, 0x64}; onIn.localCount = 1;
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    Instr setCall = ins(0x65); setCall.ptrs = {set.self};
    Instr getCall = ins(0x65); getCall.ptrs = {get.self};
    onIn.code = {ins(0x03, 7), ins(0x04, 1), ins(0x0F, 8), setCall,
                 ins(0x0F, 4), ins(0x03, 7), ins(0x0F, 8), getCall, ins(0x7B)};
    type.functions = {onIn};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{instanceRef, type});
    Runtime r;
    r.setBigfile(&bf);
    auto v = volume(1, {0, 0, 0}, {100, 100, 100});
    v.script = program->instantiate(v.uid, 100);
    v.script->fields[8] = 10;
    r.addObject(std::move(v));
    r.tick(at({0, 0, 0}));
    const Object* object = r.find(1);
    ASSERT_TRUE(object && object->script);
    EXPECT_TRUE(object->inside);
    EXPECT_EQ(object->script->fields.count(4), 1u);
    EXPECT_EQ(object->script->properties.count((8ull << 32) | 7), 1u);
}

TEST(ScriptVm, StartRunsFreshFiberAndReturnsToCaller) {
    auto ins = [](uint8_t op, int32_t arg, uint32_t word) { Instr i; i.op = op; i.arg = arg; i.word = word; return i; };
    ScriptType type;
    type.id = 100;
    Function child;
    child.self = {2, 0x80}; child.localCount = 1;
    child.code = {ins(0x00, 0, 0), ins(0x0F, 4, 1), ins(0x04, 1, 2), ins(0x2F, 0, 3), ins(0x77, 0, 4),
                  ins(0x00, 1, 5), ins(0x0F, 8, 6), ins(0x04, 2, 7), ins(0x2F, 0, 8),
                  ins(0x0F, 12, 9), ins(0x04, 3, 10), ins(0x2F, 0, 11), ins(0x7B, 0, 12)};
    Function main;
    main.vtIndex = 54; main.self = {2, 0x64}; main.localCount = 1;
    auto call = ins(0x66, 0, 0); call.ptrs = {child.self};
    main.code = {call, ins(0x0F, 16, 1), ins(0x04, 4, 2), ins(0x2F, 0, 3), ins(0x7B, 0, 4)};
    type.functions = {child, main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto object = program->instantiate(1, 100);
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54));
    EXPECT_FALSE(machine.active());
    EXPECT_EQ(object->fields[4], 1u);
    EXPECT_EQ(object->fields[8], 2u);
    EXPECT_EQ(object->fields[12], 3u);
    EXPECT_EQ(object->fields[16], 4u);
}

TEST(ScriptRealData, StartFibersClearInitialFields) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    for (const auto& test : {std::tuple<const char*, int32_t, uint32_t>{"l2_shrineroom", 2768, 8},
                             {"l2_towerroom", 1948, 12}}) {
        const auto& [unit, uid, offset] = test;
        Runtime r;
        r.setBigfile(&bf);
        r.setUnits({{unit, std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read(std::string("pc-w\\") + unit + ".drm"))), {}}});
        const Object* object = r.find(uid);
        ASSERT_TRUE(object && object->script) << unit;
        EXPECT_FALSE(object->script->initial.contains(offset)) << unit;
        EXPECT_EQ(object->script->fields.at(offset), 0u) << unit;
    }
}

TEST(ScriptRealData, InspectTriggerNativePaths) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    for (const char* unit : {"l4_pool", "l4_ballcourt", "l3_main_bedroom", "l2_cliffclimb", "l1_seatop", "l4_traprooms"}) {
        Runtime r;
        r.setBigfile(&bf);
        r.setMultibodyNative([](int32_t, uint32_t, std::span<const vm::Value>, uint32_t& result) { result = 0; return true; });
        r.setUnits({{unit, std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read(std::string("pc-w\\") + unit + ".drm"))), {}}});
        for (const auto& o : r.objects())
            if (o.kind == Kind::Volume) r.tick(at(o.pos));
    }
}

TEST(ScriptRealData, BallcourtStartupUsesMultibody) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    std::vector<std::pair<int32_t, uint32_t>> calls;
    r.setMultibodyNative([&](int32_t uid, uint32_t function, std::span<const vm::Value>, uint32_t& result) {
        calls.push_back({uid, function});
        result = 0;
        return true;
    });
    r.setUnits({{"l4_ballcourt", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l4_ballcourt.drm"))), {}}});
    EXPECT_TRUE(std::find(calls.begin(), calls.end(), std::pair{6505, 77u}) != calls.end());
    EXPECT_TRUE(std::find(calls.begin(), calls.end(), std::pair{6505, 2u}) != calls.end());
}

TEST(ScriptRuntime, CheckpointNativeEmitsCheckpoint) {
    ScriptType checkpoint;
    checkpoint.id = 3491; checkpoint.nativeName = "checkpointbase";
    checkpoint.functions.resize(4);
    Function activate;
    activate.self = {1, 0x54}; activate.native = true;
    checkpoint.functions[3] = activate;
    ScriptType type;
    type.id = 100;
    Function main;
    main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
    Instr call; call.op = 0x65; call.ptrs = {activate.self};
    Instr self; self.op = 0x0A; self.arg = 0;
    Instr ret; ret.op = 0x7B;
    main.code = {self, call, ret};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{checkpoint, type});
    Runtime r;
    auto v = volume(1, {0, 0, 0}, {100, 100, 100});
    v.script = program->instantiate(v.uid, type.id);
    r.addObject(std::move(v));
    ASSERT_EQ(r.events().size(), 1u);
    EXPECT_EQ(r.events()[0].kind, EventKind::Checkpoint);
    EXPECT_EQ(r.events()[0].target, 1);
}

TEST(ScriptRuntime, InstanceGetterDrivesModifiable) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType instance;
    instance.id = 3; instance.nativeName = "instance";
    instance.functions.resize(7);
    Function get;
    get.self = {1, 0x54}; get.native = true; get.returnType = 11;
    instance.functions[6] = get;
    ScriptType modifiable;
    modifiable.id = 823; modifiable.nativeName = "modifiable";
    modifiable.functions.resize(5);
    Function set;
    set.self = {1, 0x64}; set.native = true; set.argCount = 1;
    modifiable.functions[4] = set;
    ScriptType type;
    type.id = 100;
    Function main;
    main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
    auto callGet = ins(0x66); callGet.ptrs = {get.self};
    auto callSet = ins(0x65); callSet.ptrs = {set.self};
    main.code = {ins(0x09, 0), callGet, ins(0x03, 0), ins(0x0F, 4), callSet,
                 ins(0x0F, 8), ins(0x04, 1), ins(0x2F), ins(0x7B)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{instance, modifiable, type});
    Runtime r;
    auto v = volume(1, {0, 0, 0}, {100, 100, 100});
    v.script = program->instantiate(v.uid, type.id);
    r.addObject(std::move(v));
    const Object* object = r.find(1);
    ASSERT_TRUE(object && object->script);
    EXPECT_EQ(object->script->fields.at(4), 0u);
    EXPECT_EQ(object->script->fields.at(8), 1u);
}

TEST(ScriptRealData, MessageChannelSendsReachNativeSubscribers) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    size_t messages = 0;
    r.setMessageNative([&](uint32_t channel, uint32_t id, std::string_view payload) {
        if (payload != "stopOthers") return true; // unit scripts also send their level's music/stream names ("l0")
        EXPECT_NE(channel, 0u);
        EXPECT_EQ(id, 0u);
        EXPECT_EQ(payload, "stopOthers");
        ++messages;
        return true;
    });
    r.setUnits({{"l3_main_bedroom", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l3_main_bedroom.drm"))), {}}});
    for (const auto& o : r.objects())
        if (o.kind == Kind::Volume) r.tick(at(o.pos));
    EXPECT_GT(messages, 20u);
}

TEST(ScriptRuntime, UnitEffectResetStoresNativeValue) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType unit;
    unit.id = 900; unit.nativeName = "unit";
    unit.functions.resize(16);
    unit.functions[15].self = {1, 0x54}; unit.functions[15].native = true; unit.functions[15].argCount = 1;
    ScriptType type;
    type.id = 100;
    Function main;
    main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
    Instr call = ins(0x66); call.ptrs = {unit.functions[15].self};
    Instr value = ins(0x02); value.imm = 0x3F800000;
    main.code = {value, call, ins(0x7B)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{unit, type});
    Runtime r;
    auto object = volume(1, {0, 0, 0}, {100, 100, 100});
    object.script = program->instantiate(object.uid, type.id);
    r.addObject(std::move(object));
    EXPECT_EQ(r.unitEffect(), 0x3F800000u);
}

TEST(ScriptRuntime, InstanceVisibilityEngagementResetAndSave) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType instance;
    instance.id = 3; instance.nativeName = "instance";
    instance.functions.resize(61);
    for (const uint32_t fn : {25u, 59u, 60u}) {
        instance.functions[fn].self = {1, fn};
        instance.functions[fn].native = true;
        instance.functions[fn].argCount = fn == 25 ? 0 : 1;
    }
    ScriptType type;
    type.id = 100;
    Function main;
    main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
    auto call = [&](uint32_t fn) { Instr out = ins(0x66); out.ptrs = {instance.functions[fn].self}; return out; };
    main.code = {ins(0x03, 42), call(59), ins(0x04, 1), call(60)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{instance, type});
    Runtime r;
    auto object = volume(1, {0, 0, 0}, {100, 100, 100});
    object.script = program->instantiate(object.uid, type.id);
    r.addObject(std::move(object));
    const auto* stored = r.find(1);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->engagement, 42);
    EXPECT_TRUE(stored->hidden);
    EXPECT_EQ(r.visibilityVersion(), 1u);
    const auto saved = r.serialize();
    EXPECT_EQ(saved["1"]["hidden"], true);
    EXPECT_EQ(saved["1"]["engagement"], 42);
    Runtime restored;
    restored.restore(saved);
    EXPECT_EQ(restored.serialize(), saved);

    ScriptType resetType = type;
    resetType.functions[0].code = {call(25)};
    auto resetProgram = std::make_shared<vm::Program>(std::vector<ScriptType>{instance, resetType});
    Runtime reset;
    auto other = volume(2, {0, 0, 0}, {100, 100, 100});
    other.script = resetProgram->instantiate(other.uid, resetType.id);
    other.hidden = true;
    other.engagement = 42;
    reset.addObject(std::move(other));
    EXPECT_FALSE(reset.find(2)->hidden);
    EXPECT_EQ(reset.find(2)->engagement, -1);
    EXPECT_EQ(reset.visibilityVersion(), 1u);
}

TEST(ScriptRuntime, GameBaseGlobalSetAddSubtractAndGet) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType game;
    game.id = 900; game.nativeName = "gamebase";
    game.functions.resize(8);
    for (uint32_t i = 4; i < 8; ++i) {
        game.functions[i].self = {1, uint32_t(0x54 + i)};
        game.functions[i].native = true;
        game.functions[i].argCount = i == 4 ? 1 : 2;
        game.functions[i].returnType = i == 4 ? 1 : 0;
    }
    ScriptType type;
    type.id = 100;
    Function main;
    main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
    auto call = [&](uint32_t fn) { Instr out = ins(0x66); out.ptrs = {game.functions[fn].self}; return out; };
    main.code = {ins(0x03, 125), ins(0x03, 8), call(5), ins(0x03, 125), ins(0x03, 2), call(6),
                 ins(0x03, 125), ins(0x03, 1), call(7), ins(0x0F, 12), ins(0x03, 125), call(4), ins(0x7B)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{game, type});
    Runtime r;
    auto object = volume(1, {0, 0, 0}, {100, 100, 100});
    object.script = program->instantiate(object.uid, type.id);
    r.addObject(std::move(object));
    EXPECT_EQ(r.find(1)->script->fields.at(12), 9u);
    const auto saved = r.serialize();
    EXPECT_EQ(saved["gameValues"][125], 9);
    Runtime restored;
    restored.restore(saved);
    EXPECT_EQ(restored.serialize(), saved);
}

TEST(ScriptRuntime, HealthNativeReturnsCurrentValue) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType health;
    health.id = 776; health.nativeName = "health";
    health.functions.resize(10);
    Function get;
    get.self = {1, 0x54}; get.native = true; get.returnType = 6;
    health.functions[9] = get;
    ScriptType type;
    type.id = 100;
    Member member;
    member.type = 10; member.typeId = health.id; member.offset = 16;
    type.members = {member};
    Function main;
    main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
    Instr call = ins(0x65); call.ptrs = {get.self};
    main.code = {ins(0x0F, 12), ins(0x0F, 16), call, ins(0x7B)};
    type.functions = {main};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{health, type});
    Runtime r;
    const uint32_t expected = std::bit_cast<uint32_t>(625.0f);
    r.setHealthNative([&](uint32_t function, std::span<const vm::Value> args, uint32_t& result) {
        EXPECT_EQ(function, 9u);
        EXPECT_TRUE(args.empty());
        result = expected;
        return true;
    });
    auto object = volume(1, {0, 0, 0}, {100, 100, 100});
    object.script = program->instantiate(object.uid, type.id);
    r.addObject(std::move(object));
    EXPECT_EQ(r.find(1)->script->fields.at(12), expected);
}

TEST(ScriptRuntime, TerrainGroupSetEnabledAndIsEnabled) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    for (const bool enabled : {false, true}) {
        ScriptType terrain;
        terrain.id = 900; terrain.nativeName = "terraingroup";
        terrain.functions.resize(4);
        Function set, get;
        set.self = {1, 0x54}; set.native = true; set.argCount = 1;
        get.self = {1, 0x64}; get.native = true; get.returnType = 1;
        terrain.functions[2] = set;
        terrain.functions[3] = get;
        ScriptType type;
        type.id = 100;
        Member group;
        group.type = 10; group.typeId = terrain.id; group.offset = 4; group.init = 58; group.init2 = 1;
        type.members = {group};
        Function main;
        main.vtIndex = -1; main.self = {2, 0x64}; main.localCount = 1;
        Instr setCall = ins(0x65); setCall.ptrs = {set.self};
        Instr getCall = ins(0x65); getCall.ptrs = {get.self};
        main.code = {ins(0x04, enabled), ins(0x0F, 4), setCall, ins(0x0F, 12), ins(0x0F, 4), getCall, ins(0x7B)};
        type.functions = {main};
        auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{terrain, type});
        Runtime r;
        bool current = true;
        std::vector<uint32_t> calls;
        r.setTerrainNative([&](int32_t uid, uint16_t streamUnit, uint32_t model, uint32_t function,
                               std::span<const vm::Value> args, uint32_t& result) {
            EXPECT_EQ(uid, 1);
            EXPECT_EQ(streamUnit, 58);
            EXPECT_EQ(model, 1u);
            calls.push_back(function);
            if (function == 2) {
                EXPECT_EQ(args.size(), 1u);
                current = args[0].bits != 0;
            } else if (function == 3) {
                EXPECT_TRUE(args.empty());
                result = current;
            } else {
                return false;
            }
            return true;
        });
        auto object = volume(1, {0, 0, 0}, {100, 100, 100});
        object.script = program->instantiate(object.uid, type.id);
        r.addObject(std::move(object));
        const Object* stored = r.find(1);
        ASSERT_TRUE(stored && stored->script);
        EXPECT_EQ(stored->script->fields.at(4), 58u);
        EXPECT_EQ(stored->script->fields.at(12), uint32_t(enabled));
        EXPECT_EQ(calls, (std::vector<uint32_t>{2, 3}));
    }
}

TEST(ScriptRealData, TerrainGroupMemberIdentity) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    r.setUnits({{"l2_hubhall", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l2_hubhall.drm"))), {}}});
    const Object* object = r.find(2571);
    ASSERT_TRUE(object && object->script);
    EXPECT_EQ(object->script->typeId, 9524u);
    ASSERT_TRUE(object->script->fields.contains(4));
    ASSERT_TRUE(object->script->fields.contains(8));
    EXPECT_EQ(object->script->fields.at(4), 58u);
    EXPECT_EQ(object->script->fields.at(8), 58u);
    const Member* first = object->script->program->member(object->script->typeId, 4);
    const Member* second = object->script->program->member(object->script->typeId, 8);
    ASSERT_TRUE(first && first->init2);
    ASSERT_TRUE(second && second->init2);
    EXPECT_EQ(*first->init2, 1u);
    EXPECT_EQ(*second->init2, 5u);
}

TEST(ScriptRealData, UberCommandDispatchesWithObjectIdentity) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    size_t calls = 0;
    r.setUberNative([&](int32_t uid, uint32_t function, uint32_t command, uint32_t commandIndex, std::span<const vm::Value>, uint32_t& result) {
        // The switch makes hazard_b 497 send one of its command members; the tentacles' own states send theirs too.
        if (uid == 497 && function == 1) ++calls;
        result = 1;
        return true;
    });
    r.setUnits({{"l1_kraken_room", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l1_kraken_room.drm"))), {}}});
    const Object* sw = r.find(498);
    ASSERT_TRUE(sw);
    for (int i = 0; i < 600; ++i) r.tick({sw->pos, 0, {}, true});
    EXPECT_EQ(sw->state, 2);
    EXPECT_GT(calls, 0u);
}

TEST(ScriptRealData, ChapterEndCinematicsHaveRuntimeScripts) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    for (const auto& test : {std::pair{"l1_cine_ship_sinking", 1005},
                             {"l3_mansion_exterior_afterburn", 4070}, {"l6_ship_natla_room", 1387}}) {
        const auto& [unit, uid] = test;
        Runtime r;
        r.setBigfile(&bf);
        r.setUnits({{unit, std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read(std::string("pc-w\\") + unit + ".drm"))), {}}});
        const Object* cinematic = r.find(uid);
        ASSERT_TRUE(cinematic) << unit;
        EXPECT_TRUE(cinematic->script) << unit;
    }
}

// SLEEP (0x7C) yields one tick and pops nothing (tru.exe 0x57CE50 case 0x7B); real data uses it with an empty stack.
TEST(ScriptVm, SleepYieldsOneTickWithoutOperand) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType type;
    type.id = 100;
    Function f;
    f.vtIndex = 54; f.self = {2, 0x64}; f.localCount = 1;
    f.code = {ins(0x7C), ins(0x0F, 4), ins(0x04, 9), ins(0x2F), ins(0x7B)};
    type.functions = {f};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto object = program->instantiate(1, 100);
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54));
    EXPECT_TRUE(machine.active());
    EXPECT_FALSE(object->fields.contains(4));
    ASSERT_TRUE(machine.tick(1.0f / 60));
    EXPECT_FALSE(machine.active());
    EXPECT_EQ(object->fields[4], 9u);
}

// A blocking native (instanceref WaitUntilBirthed) retries the same call each tick until it is satisfied.
TEST(ScriptVm, BlockingNativeRetriesCall) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType native;
    native.id = 51; native.nativeName = "instanceref";
    Function wait; wait.self = {1, 0x54}; wait.native = true;
    native.functions = {wait};
    ScriptType type;
    type.id = 100;
    Function f;
    f.vtIndex = 54; f.self = {2, 0x64}; f.localCount = 1;
    Instr call = ins(0x65); call.ptrs = {wait.self};
    f.code = {ins(0x0F, 8), call, ins(0x0F, 4), ins(0x04, 5), ins(0x2F), ins(0x7B)};
    type.functions = {f};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{native, type});
    auto object = program->instantiate(1, 100);
    int calls = 0;
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext& nc) {
        nc.wait = ++calls < 3;
        return std::string_view(nc.name) == "instanceref";
    });
    ASSERT_TRUE(machine.call(*object, 54));
    ASSERT_TRUE(machine.tick(0.1f));
    EXPECT_FALSE(object->fields.contains(4));
    ASSERT_TRUE(machine.tick(0.1f));
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(object->fields[4], 5u);
}

// Thor's gear (GAMEPLAY.md "Thor's gear"): outfit primgroups of draw groups 10/11 (gauntlets at the T-pose hands)
// and 12 (belt) start hidden. l3_secretchamber volume 12379 calls Lara's show helpers (type 205 vt 62-64 -> instance
// 61) on OnIn and the hide helpers on OnOut; a helper only shows a piece while Lara carries its g_* object (larabase
// 38/37, given by the equip helpers vt 59-61). Mask, carried objects and the gear stickybools round-trip through saves.
TEST(ScriptRealData, ThorGearDrawGroups) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    for (const char* outfit : {"lara", "lara_casual", "lara_wetsuit_warm", "lara_jungle_a", "lara_jungle_heavy",
                               "lara_snow_heavy", "lara_jungle_b", "lara_wetsuit_cold"}) {
        const auto sc = formats::loadDrmScene(bf, std::string("pc-w\\") + outfit + ".drm");
        std::set<int> groups;
        for (const auto& m : sc.meshes)
            if (m.skeleton >= 0)
                for (const auto& s : m.submeshes) groups.insert(s.drawGroup);
        for (int gear : {10, 11, 12}) EXPECT_TRUE(groups.contains(gear)) << outfit << " group " << gear;
    }
    constexpr uint32_t kGear = 0x7u << 9; // groups 10, 11, 12
    Runtime r;
    r.setBigfile(&bf);
    ASSERT_TRUE(r.playerScript());
    EXPECT_EQ(r.playerHiddenGroups(), kGear);
    auto bytes = bf.read("pc-w\\l3_secretchamber.drm");
    ASSERT_TRUE(bytes);
    r.setUnits({{"l3_secretchamber", std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes))), {}}});
    const Object* v = nullptr;
    for (const auto& o : r.objects())
        if (o.script && o.script->typeId == 12379) v = &o;
    ASSERT_TRUE(v);
    const vec3 inside = v->pos, outside = inside + vec3(0, 0, 20000);
    auto visit = [&] { r.tick(at(outside)); r.tick(at(inside)); r.tick(at(inside)); };
    visit();
    EXPECT_EQ(r.playerHiddenGroups(), kGear); // nothing carried: nothing to show
    auto state = r.serialize();
    state["player"]["objects"] = {"g_belt", "g_gauntletleft", "g_gauntletright"};
    r.restore(state);
    visit();
    EXPECT_EQ(r.playerHiddenGroups(), 0u);
    EXPECT_EQ(r.playerScript()->fields.at(52), 1u); // stickybool: left gauntlet shown
    const auto saved = r.serialize();
    r.tick(at(outside));
    r.tick(at(outside));
    EXPECT_EQ(r.playerHiddenGroups(), kGear); // OnOut puts the gear away
    r.restore(saved);
    EXPECT_EQ(r.playerHiddenGroups(), 0u);
    EXPECT_EQ(r.playerObjects().size(), 3u);
    r.restore(nlohmann::json::object()); // chapter start without a save
    EXPECT_EQ(r.playerHiddenGroups(), kGear);
    EXPECT_TRUE(r.playerObjects().empty());
}

// Help on demand: a unit's trigger volume passes "hint_NN" to its unit script (l1_seafloor volume 972 OnFirstIn ->
// unitref.Get -> unit script 5277 vt 26 stores it in member +228); the unit script itself starts with its unit.
TEST(ScriptRealData, SeafloorVolumeSetsHelpCondition) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    r.setUnits({{"l1_seafloor", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l1_seafloor.drm"))), {}}});
    EXPECT_EQ(r.helpCondition(), "");
    const Object* v = r.find(972);
    ASSERT_TRUE(v);
    r.tick(at(v->pos));
    EXPECT_TRUE(r.find(972)->inside);
    EXPECT_EQ(r.helpCondition(), "hint_05");
}
