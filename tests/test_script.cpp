// ou_script: trigger shapes, event dispatch, switches/doors/blocks, save state; real-data parsing of every
// unit's script sections and trigger intros (skipped without OU_GAME_DIR).
#include "script/Script.hpp"
#include "script/Vm.hpp"

#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdlib>
#include <numbers>
#include <optional>
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
TickInput at(vec3 p) { return {p - vec3(0, 0, kTestPointHeight), false}; }

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
    plane.oneSided = true; // toward local +Y only (tru.exe 0x5CCC20 -> 0x461640)
    EXPECT_FALSE(crosses(plane, {}, {}, {0, 10, 0}, {0, -10, 0}));
    EXPECT_TRUE(crosses(plane, {}, {}, {0, -10, 0}, {0, 10, 0}));
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

TEST(ScriptRuntime, PlaneCrossAndSwitchUse) {
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
    r.tick({{0, 50, 0}, false});
    r.tick({{0, -50, 0}, false});
    ASSERT_EQ(r.events().size(), 2u);
    EXPECT_EQ(r.events()[0].kind, EventKind::Cross);
    EXPECT_EQ(r.events()[1].target, 20);

    r.tick({{900, 0, 0}, true}); // press near the switch
    ASSERT_EQ(r.events().size(), 2u);
    EXPECT_EQ(r.events()[0].kind, EventKind::Use);
    EXPECT_EQ(r.find(21)->state, 2);
    r.tick({{900, 0, 0}, true}); // held: no second use
    EXPECT_TRUE(r.events().empty());
    EXPECT_EQ(r.find(3)->state, 2);

}

TEST(ScriptRuntime, StateRoundTrip) {
    Runtime r;
    r.addObject(volume(1, {0, 0, 0}, {100, 100, 100}, {{kVtOnIn, {10}}}));
    r.addObject(door(10));
    Object block;
    block.uid = 4, block.kind = Kind::Scripted, block.offset = {1, 2, 3};
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
    // The object state is kept; only the running-script block changes: 10 is no longer alive (runtime.live), has no
    // threads to resume (runtime.machines) and is not in the living objects' order (runtime.order), so a load of this
    // save constructs it afresh when its unit streams back in, as streaming it back in would (Runtime::resume).
    auto after = r.serialize(), before = j;
    EXPECT_TRUE(before.at("runtime").at("live").get<std::set<int32_t>>().contains(10));
    EXPECT_FALSE(after.at("runtime").at("live").get<std::set<int32_t>>().contains(10));
    EXPECT_FALSE(after.at("runtime").at("machines").contains("10"));
    EXPECT_EQ(after.at("runtime").at("order"), nlohmann::json::array());
    for (auto* s : {&before, &after}) (*s)["runtime"].erase("live"), (*s)["runtime"].erase("machines"), (*s)["runtime"].erase("order");
    EXPECT_EQ(after, before) << nlohmann::json::diff(before, after).dump();
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

// The music requests and reverb a load does not ask for again (Runtime::AudioState): per music slot, the track the
// requests leave there (Load = loaded, Play = playing), saved.
TEST(ScriptRuntime, AudioStateFollowsMusicRequestsAndRoundTrips) {
    using Request = Runtime::MusicRequest;
    using Slot = Runtime::AudioState::Slot;
    Runtime::AudioState a;
    a.apply(Request{0, 2, "l4_ballcourt_mus_01", 0, 2, 0});
    EXPECT_EQ(a.slots[0], (Slot{"l4_ballcourt_mus_01", true}));
    a.apply(Request{0, 1, "l1_intro", 7, 0, 0}); // another slot: both kept
    EXPECT_EQ(a.slots[7], (Slot{"l1_intro", false}));
    EXPECT_EQ(a.slots[0].track, "l4_ballcourt_mus_01");
    a.apply(Request{0, 2, "l1_intro", 7, 0, 3}); // Play of the loaded track
    EXPECT_TRUE(a.slots[7].playing);
    a.apply(Request{0, 1, "L1_INTRO", 7, 0, 0}); // a Load of the slot's own track leaves it playing
    EXPECT_TRUE(a.slots[7].playing);
    a.apply(Request{0, 3, {}, 0, 3, 0}); // a stop in one slot leaves the other
    EXPECT_TRUE(a.slots[0].track.empty());
    EXPECT_EQ(a.slots[7].track, "l1_intro");
    a.apply(Request{0, 1, "l1_seatop_mus_01", -1, 0, 0}); // unknown slot: 0; a stop with an unknown slot clears all
    EXPECT_EQ(a.slots[0].track, "l1_seatop_mus_01");
    a.apply(Request{0, 3, {}, -1, 3, 0});
    EXPECT_TRUE(a.slots[0].track.empty() && a.slots[7].track.empty());

    Runtime r;
    r.restore({{"audio", {{"music", {{{"slot", 1}, {"track", "l3_bedroom_mus_01"}, {"playing", true}},
                                     {{"slot", 6}, {"track", "l3_main_hall_mus_02"}, {"playing", false}}}},
                          {"reverbUnit", "l4_trailstart"}, {"reverbAt", {3, 128}}}}});
    Runtime restored;
    restored.restore(r.serialize());
    const auto& s = restored.audioState();
    EXPECT_EQ(s.slots[1], (Slot{"l3_bedroom_mus_01", true}));
    EXPECT_EQ(s.slots[6], (Slot{"l3_main_hall_mus_02", false}));
    EXPECT_TRUE(s.slots[0].track.empty());
    EXPECT_EQ(s.reverbUnit, "l4_trailstart");
    EXPECT_EQ(s.reverbAt.section, 3u);
    EXPECT_EQ(s.reverbAt.offset, 128u);
    restored.stopAudio(); // leaving play
    EXPECT_TRUE(restored.audioState().slots[1].track.empty() && restored.audioState().reverbUnit.empty());
    restored.restore({{"audio", {{"track", "l3_bedroom_mus_01"}, {"slot", 1}}}}); // an older save: one playing track
    EXPECT_EQ(restored.audioState().slots[1], (Slot{"l3_bedroom_mus_01", true}));
    restored.restore(nlohmann::json::object()); // older saves / a chapter start: nothing
    EXPECT_TRUE(restored.audioState().slots[1].track.empty());
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

// Camera trigger volumes (intro data +0x18 block, kind 0: inline command lists; docs/re/CAMERA.md §9): which commands the
// game's volumes use, counted over the 82 units (the survey behind the commands World::updateOrbitCamera models). The
// 0x17 / 0x18 force sets carry their force records, all of types the global sets use (2 = her move, 3 / 4 curves, 5 =
// k x dt: l2_cliffclimb 3851 k 20, 3859 k 2; 7 = her mean move), and every 0x12 points at a record.
TEST(ScriptRealData, CameraTriggerCommandSurvey) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(list);
    std::string text(list->begin(), list->end());
    std::map<uint32_t, int> uses;
    int volumes = 0, late = 0, setsWithForces = 0, records = 0;
    std::set<uint32_t> forceTypes;
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string name = line.substr(line.rfind('\\') + 1);
        auto bytes = bf.read("pc-w\\" + name + ".drm");
        if (!bytes) continue;
        auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)));
        Runtime r;
        r.setBigfile(&bf);
        r.setUnits({{name, drm, {}}});
        for (const auto& o : r.objects()) {
            if (o.kind != Kind::Volume || o.camera.empty()) continue;
            ++volumes, late += o.cameraLate;
            for (const auto& c : o.camera) {
                ++uses[c.command];
                if ((c.command == 0x17 || c.command == 0x18) && !c.forces.empty()) {
                    ++setsWithForces;
                    EXPECT_EQ(c.forces.size(), std::bit_cast<uint32_t>(c.arg[3])) << name << " " << o.uid;
                    for (const auto& f : c.forces) {
                        forceTypes.insert(f.type);
                        EXPECT_TRUE(f.type >= 1 && f.type <= 7) << name << " " << o.uid;
                    }
                }
                if (c.command == 0x12) records += c.record.has_value();
                if (c.command == 0xC || c.command == 0x14) // the cuts and the focus points flagged (unit, volume, id)
                    std::printf("%s %d: 0x%X %d (%zu commands)\n", name.c_str(), o.uid, c.command, std::bit_cast<int32_t>(c.arg[0]), o.camera.size());
            }
        }
    }
    for (const auto& [c, n] : uses) std::printf("command 0x%X: %d\n", c, n);
    std::printf("%d volumes (%d with priority), %d force sets with forces, %d records\n", volumes, late, setsWithForces, records);
    const std::map<uint32_t, int> expected = {{0x0, 4},   {0x1, 4},    {0x2, 129},  {0x3, 2},    {0x4, 7},    {0x5, 1},    {0x6, 15},
                                              {0x7, 1},   {0x8, 4},    {0xC, 2},    {0xD, 109},  {0x12, 116}, {0x14, 53},  {0x15, 33},
                                              {0x16, 54}, {0x17, 24},  {0x18, 155}, {0x19, 65},  {0x1A, 73},  {0x1B, 47},  {0x1C, 51}};
    EXPECT_EQ(uses, expected);
    EXPECT_EQ(volumes, 459);
    EXPECT_EQ(setsWithForces, 89);
    EXPECT_EQ(records, uses[0x12]);
    EXPECT_EQ(forceTypes, (std::set<uint32_t>{2, 3, 5, 7}));
}

// setUnits adds a unit once: it used to take "no objects of the unit" for "not loaded" and add an object-less unit again
// on every call, restarting its unit script (a second instance and thread). container1 is the one unit whose intros make
// no script objects (it has no unit script); l3_main_bedroom's unit script must stay the same instance while loaded.
TEST(ScriptRealData, SetUnitsAddsEachUnitOnce) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    auto load = [&](const char* name) {
        auto bytes = bf.read(std::string("pc-w\\") + name + ".drm");
        return bytes ? std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes))) : nullptr;
    };
    const auto bedroom = load("l3_main_bedroom"), container = load("container1");
    ASSERT_TRUE(bedroom && container);
    const std::vector<UnitRef> both{{"l3_main_bedroom", bedroom, {}}, {"container1", container, {}}};
    Runtime r;
    r.setBigfile(&bf);
    r.setUnits(both);
    const size_t objects = r.objects().size();
    EXPECT_TRUE(std::none_of(r.objects().begin(), r.objects().end(), [](const Object& o) { return o.unit == "container1"; }));
    const int32_t uid = r.unitScriptUid("l3_main_bedroom");
    EXPECT_NE(uid, 0);
    r.setUnits(both);
    EXPECT_EQ(r.unitScriptUid("l3_main_bedroom"), uid);
    EXPECT_EQ(r.objects().size(), objects);
    r.setUnits({both[1]});
    EXPECT_EQ(r.unitScriptUid("l3_main_bedroom"), 0);
    r.setUnits(both); // streamed back in: a new instance
    EXPECT_NE(r.unitScriptUid("l3_main_bedroom"), 0);
    EXPECT_NE(r.unitScriptUid("l3_main_bedroom"), uid);
    EXPECT_EQ(r.objects().size(), objects);
}

// A resident unit (registered only for its cinematics) keeps its unit script's startup until the stream really loads
// it: l3_mansion_exterior_afterburn's (11388) sets globaleffect 11 Rain 20, which tru.exe never runs elsewhere
// (l5_startsnow reads rain 0 under Wine) and which put rain and lightning flashes over every level.
TEST(ScriptRealData, ResidentUnitScriptWaitsForItsUnit) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    auto bytes = bf.read("pc-w\\l3_mansion_exterior_afterburn.drm");
    ASSERT_TRUE(bytes);
    const auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)));
    Runtime r;
    r.setBigfile(&bf);
    UnitRef resident{"l3_mansion_exterior_afterburn", drm, {}, true}, loaded = resident;
    loaded.resident = false;
    r.setUnits({resident});
    EXPECT_NE(r.unitScriptUid(resident.name), 0); // created and listening
    EXPECT_EQ(r.weather().rain, 0);
    r.setUnits({loaded, resident});
    EXPECT_EQ(r.weather().rain, 20);
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

    r.tick({sw->pos, true});
    EXPECT_EQ(sw->state, 1); // pulling
    for (int i = 0; i < 600 && door->state != 2; ++i) r.tick({sw->pos, true});
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
    ASSERT_TRUE(sw && sw->script && sw->script->program);
    // 498's constructor locks it (modifiable 7 (2)) until the unit's "Kraken dead" broadcast on its channel (vt 0 ->
    // 7 (1); OU-PZL-009): deliver it first.
    std::optional<uint32_t> channel;
    const auto& p = *sw->script->program;
    for (const auto* t = p.type(sw->script->typeId); t && !channel; t = p.type(t->superId))
        for (const auto& m : t->members)
            if (!std::strcmp(p.nativeName(m.typeId), "messagechannel"))
                if (const auto f = sw->script->fields.find(m.offset); f != sw->script->fields.end()) { channel = f->second; break; }
    ASSERT_TRUE(channel);
    r.sendMessage(*channel, 0, "Kraken dead, so remove tentacle hazards");
    for (int i = 0; i < 30; ++i) r.tick({});
    for (int i = 0; i < 600; ++i) r.tick({sw->pos, true});
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

// Entering soundvolume 5674 (l4_ragnarok_room) runs unit script 14147 vt 25: `music` 2 plays
// l4_staircase_upper_mus_01 in slot 1 (fade in 2 s) and `music` 3 stops slot 0 (3 s) from a music local; music variable 0x7D = 8 (AUDIO.md §9.4).
TEST(ScriptRealData, RagnarokVolumeRequestsStaircaseMusic) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    r.setUnits({{"l4_pool", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l4_pool.drm"))), {}},
                {"l4_ragnarok_room", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l4_ragnarok_room.drm"))),
                 {-20475, -2872, 12591}}});
    r.tick(at({0, 0, 0}));
    r.takeMusicRequests();
    const Object* v = r.find(5674);
    ASSERT_TRUE(v);
    r.tick(at(v->pos));
    const auto req = r.takeMusicRequests();
    ASSERT_GE(req.size(), 2u);
    EXPECT_EQ(req[0].function, 2u);
    EXPECT_EQ(req[0].track, "l4_staircase_upper_mus_01");
    EXPECT_EQ(req[0].slot, 1);
    EXPECT_FLOAT_EQ(req[0].fadeOut, 0); // Play(fade-out 0 s, fade-in 2 s): the crossfade's new half
    EXPECT_FLOAT_EQ(req[0].fadeIn, 2);
    EXPECT_EQ(req[1].function, 3u);
    EXPECT_EQ(req[1].slot, 0);
    EXPECT_FLOAT_EQ(req[1].fadeOut, 3); // Stop(3 s): the old half
    EXPECT_EQ(r.gameValues()[0x7D], 8);
}

// Tutorial popups (training text): l2_cliffclimb's trigger volumes call unit script 9502 vt 25 with "hint_NN", which
// hides the popup (uipopupmgr 8), reads a uilocalstrproperty member (its locals.bin id) and shows it for 10 s
// (uipopupmgr 0). Volume 4040 sends hint_15 -> member +92 = 1326 "Press ᠁ to knock away clinging enemies", once
// (its stickybool +248).
TEST(ScriptRealData, CliffclimbVolumeShowsTutorialPopup) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    Runtime r;
    r.setBigfile(&bf);
    r.setUnits({{"l2_cliffclimb", std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l2_cliffclimb.drm"))), {}}});
    const vec3 away(1e7f, 1e7f, 1e7f);
    r.tick(at(away));
    r.takePopups();
    const Object* v = r.find(4040);
    ASSERT_TRUE(v);
    const vec3 pos = v->pos;
    r.tick(at(pos));
    const auto p = r.takePopups();
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0].text, 0); // hide the previous one
    EXPECT_EQ(p[1].text, 1326);
    EXPECT_FLOAT_EQ(p[1].seconds, 10.0f);
    r.tick(at(away));
    r.tick(at(pos)); // shown once
    EXPECT_TRUE(r.takePopups().empty());
}

// Operand decode (tru.exe link pass 0x57EE20): every opcode takes its operandWords(), relocated or not. The fire volumes'
// CALLU 0x00000701 (no relocation, bit 0 set) is builtin DYNARRAY method 0 (Size), not CALLU + NOP 1793; START's null
// function word is its operand, not NOP 0; relocated operands and PUSH immediates decode as before.
TEST(ScriptParse, OperandWordsAndBuiltinCallU) {
    const cdc::Pointer fn{3, 0x64};
    const std::vector<CodeWord> words = {
        {0x08000004}, {0x09000008}, {0x10000010}, {0x65000000}, {0x00000701}, // i < sensors.Size()
        {0x08000008}, {0x43000000},
        {0x65000000}, {0xBEEBBEEB, fn},                                       // a relocated CALLU
        {0x02000000}, {0x3F800000},                                           // PUSH 1.0f
        {0x77000000}, {0x00000000},                                           // START, null function
        {0x7E000000}, {205, std::nullopt, true},                              // ISTYPE 205
        {0x7B000000}};
    const auto code = decodeCode(words);
    ASSERT_EQ(code.size(), 11u);
    EXPECT_EQ(code[3].op, 0x65);
    EXPECT_EQ(code[3].builtinType, kDataDynArray);
    EXPECT_EQ(code[3].builtinIndex, 0);
    EXPECT_TRUE(code[3].ptrs.empty());
    EXPECT_EQ(code[4].op, 0x08); // the next instruction, not NOP 1793
    EXPECT_EQ(code[4].word, 5u);
    EXPECT_EQ(code[6].op, 0x65);
    EXPECT_EQ(code[6].builtinType, 0);
    ASSERT_EQ(code[6].ptrs.size(), 1u);
    EXPECT_EQ(code[6].ptrs[0].offset, 0x64u);
    EXPECT_EQ(code[7].imm, 0x3F800000u);
    EXPECT_EQ(code[8].op, 0x77);
    EXPECT_TRUE(code[8].ptrs.empty());
    EXPECT_EQ(code[8].word, 11u);
    EXPECT_EQ(code.back().op, 0x7B);
    EXPECT_EQ(code.back().word, 15u);
    EXPECT_EQ(code[code.size() - 2].ref, 205u);
    // MAP builtin 3 (Erase): {1, 8, 3}
    const std::vector<CodeWord> erase = {{0x65000000}, {0x00030801}};
    const auto e = decodeCode(erase);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].builtinType, kDataMap);
    EXPECT_EQ(e[0].builtinIndex, 3);
    const std::vector<CodeWord> operandFirst = {{0xBEEBBEEB, fn}, {0x7B000000}};
    EXPECT_THROW(decodeCode(operandFirst), cdc::FormatError);
}

// DYNARRAY builtins (NativeBuiltInImpl records 0x8A7718) over the VM's array model: the member's field is the count,
// element i is elementOffset(member, i); DREFD / DBOUNDS / DREF32 read elements; a null receiver skips the call with the
// result untouched (0x57DE34 -> 0x5766F0).
TEST(ScriptVm, DynamicArrayBuiltins) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    auto callu = [](uint16_t index) { Instr i; i.op = 0x65; i.builtinType = kDataDynArray; i.builtinIndex = index; return i; };
    ScriptType type;
    type.id = 100;
    Member array; array.type = 0x37; array.offset = 16; array.init = 2;
    Member element; element.type = 0x32;
    element.init = 5; array.elements.push_back(element);
    element.init = 6; array.elements.push_back(element);
    type.members = {array};
    Function f;
    f.vtIndex = 54; f.self = {2, 0x64}; f.localCount = 1;
    f.code = {
        ins(0x0F, 4), ins(0x10, 16), callu(0),                                        // +4 = Size() = 2
        ins(0x03, 9), ins(0x10, 16), callu(4),                                        // Push(9)
        ins(0x0F, 8), ins(0x10, 16), callu(0),                                        // +8 = 3
        ins(0x03, 0), ins(0x10, 16), callu(3),                                        // Remove(0): 6, 9
        ins(0x0F, 12), ins(0x10, 16), ins(0x20), ins(0x03, 1), ins(0x22), ins(0x1D), ins(0x2F), // +12 = a[1]
        ins(0x0F, 20), ins(0x03, 0), callu(0),                                        // null receiver
        ins(0x10, 16), callu(5),                                                      // Pop: 6
        ins(0x7B)};
    type.functions = {f};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto object = program->instantiate(1, 100);
    EXPECT_EQ(object->fields.at(16), 2u);
    EXPECT_EQ(object->fields.at(elementOffset(16, 1)), 6u);
    object->fields[20] = 55;
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext&) { return false; });
    ASSERT_TRUE(machine.call(*object, 54)) << machine.failure();
    EXPECT_EQ(object->fields.at(4), 2u);
    EXPECT_EQ(object->fields.at(8), 3u);
    EXPECT_EQ(object->fields.at(12), 9u);
    EXPECT_EQ(object->fields.at(20), 55u); // untouched
    EXPECT_EQ(object->fields.at(16), 1u);
    EXPECT_EQ(object->fields.at(elementOffset(16, 0)), 6u);
    EXPECT_FALSE(object->fields.contains(elementOffset(16, 1)));
    ASSERT_NE(program->member(100, elementOffset(16, 1)), nullptr);
    EXPECT_EQ(program->member(100, elementOffset(16, 1))->init, 6u);

    // A map method fails visibly: maps are not modelled.
    Instr map = callu(0); map.builtinType = kDataMap;
    f.code = {ins(0x0F, 4), ins(0x10, 16), map, ins(0x7B)};
    type.functions = {f};
    auto mapProgram = std::make_shared<vm::Program>(std::vector<ScriptType>{type});
    auto mapObject = mapProgram->instantiate(1, 100);
    vm::Machine mapMachine([&](int32_t) { return mapObject; }, [&](vm::NativeContext&) { return false; });
    EXPECT_FALSE(mapMachine.call(*mapObject, 54));
    EXPECT_EQ(mapMachine.failure(), "builtin map fn 0: maps are not modelled");
}

// CALLU on a native (0x57DE34): a null receiver drops the arguments and the result address without calling, and a
// non-void native given a null result address is not called.
TEST(ScriptVm, CallUNullReceiverAndNullResultSkip) {
    auto ins = [](uint8_t op, int32_t arg = 0) { Instr i; i.op = op; i.arg = arg; return i; };
    ScriptType native;
    native.id = 51; native.nativeName = "instanceref";
    Function get; get.self = {1, 0x54}; get.native = true; get.argCount = 1; get.returnType = 2;
    native.functions = {get};
    ScriptType type;
    type.id = 100;
    Function f;
    f.vtIndex = 54; f.self = {2, 0x64}; f.localCount = 1;
    Instr call = ins(0x65); call.ptrs = {get.self};
    f.code = {ins(0x0F, 4), ins(0x03, 1), ins(0x03, 0), call,   // null receiver: +4 untouched
              ins(0x03, 0), ins(0x03, 1), ins(0x0F, 12), call,  // null result address: not called
              ins(0x0F, 8), ins(0x03, 1), ins(0x0F, 12), call,  // called: +8 = 7
              ins(0x7B)};
    type.functions = {f};
    auto program = std::make_shared<vm::Program>(std::vector<ScriptType>{native, type});
    auto object = program->instantiate(1, 100);
    object->fields[4] = 33;
    int calls = 0;
    vm::Machine machine([&](int32_t) { return object; }, [&](vm::NativeContext& nc) {
        ++calls; nc.result = vm::Value::scalar(7); return true;
    });
    ASSERT_TRUE(machine.call(*object, 54)) << machine.failure();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(object->fields.at(4), 33u);
    EXPECT_EQ(object->fields.at(8), 7u);
}

// l3_main_bedroom's fire volumes (firetriggervolume 6516, constructor fn 4): `for (i = 0; i < sensors.Size(); ++i)
// sensors[i].Attach(this)` over the four initial playersensors (radii 3160 / 2212 / 1580 / 948, onEnter and onExit, off
// until vt 11-13 enable them). It used to stop at the CALLU (OU-SCR-001); now it runs to its end (+40 = 0).
TEST(ScriptRealData, FireVolumeConstructorBuildsSensors) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l3_main_bedroom.drm")));
    std::optional<ScriptType> fire;
    for (uint32_t i = 0; i < drm->sections.size(); ++i)
        if (drm->sections[i].type == cdc::SectionType::Script && drm->sections[i].id == 6516) fire = parseScriptType(*drm, i);
    ASSERT_TRUE(fire);
    ASSERT_GT(fire->functions.size(), 4u);
    const auto& ctor = fire->functions[4].code;
    const auto size = std::find_if(ctor.begin(), ctor.end(), [](const Instr& in) { return in.word == 21; });
    ASSERT_NE(size, ctor.end());
    EXPECT_EQ(size->op, 0x65);
    EXPECT_EQ(size->builtinType, kDataDynArray);
    EXPECT_EQ(size->builtinIndex, 0);
    EXPECT_FALSE(std::any_of(ctor.begin(), ctor.end(), [](const Instr& in) { return in.word == 22; })); // was NOP 1793
    const auto sensors = std::find_if(fire->members.begin(), fire->members.end(), [](const Member& m) { return m.offset == 16; });
    ASSERT_NE(sensors, fire->members.end());
    ASSERT_EQ(sensors->elements.size(), 4u);
    EXPECT_NEAR(std::bit_cast<float>(sensors->elements[0].initWords[1]), 3160.5f, 1.0f);
    EXPECT_NEAR(std::bit_cast<float>(sensors->elements[3].initWords[1]), 948.2f, 1.0f);
    EXPECT_EQ(sensors->elements[0].init2, 0x101u);

    Runtime r;
    r.setBigfile(&bf);
    r.setUnits({{"l3_main_bedroom", drm, {}}});
    r.tick(at(vec3(1e7f)));
    size_t volumes = 0;
    for (const Object& o : r.objects()) {
        if (o.name != "firetriggervolume" || !o.script) continue;
        ++volumes;
        EXPECT_TRUE(o.script->fields.contains(40)) << o.uid << ": constructor did not finish";
        for (const auto& [key, count] : r.failures()) EXPECT_NE(key.first, o.uid) << key.second;
    }
    EXPECT_EQ(volumes, 14u);
    size_t attached = 0;
    for (const auto& s : r.serialize().at("runtime").at("sensors"))
        if (isElement(s.at(1).get<uint32_t>())) {
            ++attached;
            EXPECT_EQ(s.at(2).get<int32_t>(), s.at(0).get<int32_t>()); // attached to the volume itself (instance 6)
        }
    EXPECT_EQ(attached, 4 * volumes);
}

// l3_main_bedroom fire volume 4493 (firetriggervolume 6516, state 1 = fn 8): `while (true) { health 2 (300); ...;
// SSEQU sound 1 }`, the burn sound being the loop's only clock (at 4493's centre no triggervolume sends "stopOthers").
// tru.exe always suspends the thread on the sequence's latent (0x4AF230 allocates it even when the sound did not start;
// 0x57CE50 case 0x6A queues it via 0x576A30 and returns) and resumes it once IsActive (0x4ACC30) is false, checked from
// the next thread update (0x57E970). So:
// - a sound that does not play costs one tick per pass: the loop hits again every tick and never runs into the VM's
//   instruction budget (before: it spun until "handler stopped: budget");
// - the sustained burn sound (busy) holds the thread: one hit per entry, as Wine measured with Lara held in fire volume
//   4494 for 14 s (health 1000 -> 700 once, openunderworld-work/w2/ScriptVm6/rec4494.txt).
TEST(ScriptRealData, FireVolumeBurnWaitsOnItsSound) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l3_main_bedroom.drm")));
    const auto run = [&](bool busy, int ticks, int& hits, int& starts) {
        Runtime r;
        r.setBigfile(&bf);
        r.setUnits({{"l3_main_bedroom", drm, {}}});
        hits = starts = 0;
        r.setHealthNative([&](uint32_t fn, std::span<const vm::Value>, uint32_t& result) {
            result = 0;
            hits += fn == 2;
            return true;
        });
        r.setSoundNative([&](int32_t owner, uint32_t member, cdc::Pointer, uint32_t fn, int32_t) {
            starts += owner == 4493 && member == 64 && fn == 1;
            return busy;
        });
        r.setSoundBusy([&](int32_t owner, uint32_t member) { return busy && owner == 4493 && member == 64; });
        r.tick(at(vec3(1e7f)));
        const Object* v = r.find(4493);
        ASSERT_TRUE(v);
        const vec3 pos = v->pos;
        for (int i = 0; i < ticks; ++i) r.tick(at(pos));
        for (const auto& [key, count] : r.failures()) EXPECT_NE(key.first, 4493) << key.second;
    };
    int hits = 0, starts = 0;
    run(false, 30, hits, starts);
    EXPECT_GE(starts, 25) << "a silent sound costs one tick per pass";
    EXPECT_EQ(hits, starts);
    run(true, 600, hits, starts);
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(hits, 1) << "the sustained burn sound holds the loop (tru.exe: one 300 hit in 14 s)";
}

// Fire volumes of type 6515 (l3_main_bedroom: the `fireanim` volumes beside the 6516 burners) run a thread (fn 7) every
// 1/30 s: vt 73 takes anchor 1 VectorTo(Lara) from the volume, Lara's anchor 4 (her local y axis = her back: the model
// faces -Y), zeroes both z (vector 8), rotation 0 between them and its yaw (rotation 9), and names the fire's side:
// [-pi/4, pi/4) "front", [-3pi/4, -pi/4) "left", [pi/4, 3pi/4) "right", else "back". vt 74 (vector 9/0/1/2/10/11, anchor
// 0) is "moved more than one unit since the last pass" (its last position, member +64, starts at 0, so the first pass
// counts as moving). A change posts larabase 22 with her fire overlay pattern, then larabase 21 while it holds:
// moving fire_walk_front / _left / _right (Lara patterns 1501 / 1500 / 1499), standing fire_standing_front / _left /
// _right (1493 / 1494 / 1495); "back" has none. Expected sides are the tru.exe formulas above evaluated by hand.
TEST(ScriptRealData, FireVolumeFlinchSide) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read("pc-w\\l3_main_bedroom.drm")));
    // Lara's root along `direction` from the volume's origin (inside its box: OnIn sets sticky +168, whose vt 6
    // notification enters state 1 = the thread fn 7), heading `yaw`.
    struct Case { glm::vec2 direction; float yaw; std::optional<std::pair<uint32_t, uint32_t>> patterns; const char* what; };
    const float pi = std::numbers::pi_v<float>;
    const Case cases[] = {
        {{1, 0}, pi, std::pair{1501u, 1493u}, "facing the fire: front"},
        {{0, -1}, 0, std::pair{1500u, 1494u}, "fire on her left"},
        {{0, 1}, 0, std::pair{1499u, 1495u}, "fire on her right"},
        {{1, 0}, 0, std::nullopt, "fire behind her: back, no overlay"},
        {{1, 2}, pi, std::pair{1500u, 1494u}, "fire ahead of her and further left: left (-63 degrees)"},
    };
    for (const Case& c : cases) {
        Runtime r;
        r.setBigfile(&bf);
        r.setUnits({{"l3_main_bedroom", drm, {}}});
        r.tick(at(vec3(1e7f)));
        const Object* volume = r.find(4529); // intro script 12135, a subtype of 6515
        ASSERT_TRUE(volume && volume->script && volume->script->program->isA(volume->script->typeId, 6515));
        const int32_t uid = volume->uid;
        const float reach = 0.4f * std::min(volume->shape.half.x, volume->shape.half.y);
        const glm::vec2 d = glm::normalize(c.direction) * reach;
        TickInput in{volume->pos + vec3(d, 0) - vec3(0, 0, kTestPointHeight), false};
        ASSERT_TRUE(inside(volume->shape, volume->pos, volume->rot, volume->pos + vec3(d, 0))) << c.what << " reach " << reach;
        in.yaw = c.yaw;
        std::vector<Runtime::PlayerAnimPost> posts;
        for (int i = 0; i < 30; ++i) {
            r.tick(in);
            for (const auto& p : r.playerAnimPosts()) posts.push_back(p);
        }
        for (const auto& [key, count] : r.failures()) EXPECT_NE(key.first, uid) << c.what << ": " << key.second;
        if (!c.patterns) {
            EXPECT_TRUE(posts.empty()) << c.what << ": " << (posts.empty() ? 0 : posts[0].pattern);
            continue;
        }
        ASSERT_GT(posts.size(), 3u) << c.what;
        EXPECT_EQ(posts[0].message, 0x1DBu) << c.what; // larabase 22: first pass, side new and "moving" from 0
        EXPECT_EQ(posts[0].pattern, c.patterns->first) << c.what;
        EXPECT_EQ(posts[1].message, 0x1DBu) << c.what; // standing now: changed again
        EXPECT_EQ(posts[1].pattern, c.patterns->second) << c.what;
        for (size_t i = 2; i < posts.size(); ++i) { // larabase 21 while it holds
            EXPECT_EQ(posts[i].message, 0x1DAu) << c.what << " post " << i;
            EXPECT_EQ(posts[i].pattern, c.patterns->second) << c.what << " post " << i;
        }
    }
}

// Natives the user logs and the script survey reported missing (docs/re/SCRIPT_NATIVES.md "Natives reported missing"):
// l6_ship_exterior's yacht 7651 sets a colour ramp (gamebase 27; the current colour then moves to the target),
// l3_main_hall's unit script asks unitref 1 IsLoaded of a unit; neither stops its handler any more (instanceref 10 needs
// the World's natives: GameReal.UnitScriptsStoreFloatProperty).
TEST(ScriptRealData, MissingNativesBound) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto run = [&](const char* unit, int ticks) {
        auto r = std::make_unique<Runtime>();
        r->setBigfile(&bf);
        r->setStreamUnitId([unit](const std::string& name) { return name == unit ? 0x37u : 0u; });
        r->setUnits({{unit, std::make_shared<const cdc::Drm>(cdc::parseDrm(*bf.read(std::string("pc-w\\") + unit + ".drm"))), {}}});
        for (int i = 0; i < ticks; ++i) r->tick(at(vec3(1e7f)));
        for (const auto& [key, count] : r->failures())
            for (const char* missing : {"native instanceref fn 10", "native gamebase fn 27", "native unitref fn 1", "native instance fn 27"})
                EXPECT_NE(key.second, missing) << unit << " uid " << key.first;
        return r;
    };
    const auto ship = run("l6_ship_exterior", 600);
    const bool ramped = std::ranges::any_of(ship->colourRamps(), [](const Runtime::ColourRamp& c) {
        return c.target != glm::vec4(1) && c.current == c.target;
    });
    EXPECT_TRUE(ramped);
    run("l3_main_hall", 2);
}
