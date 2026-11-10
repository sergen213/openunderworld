// Class-30 breakables (tru.exe 0x6D3160 messages, 0x6D2D70 update, 0x6D4030 pieces) and explosion records: the object
// data World's breakable system and blasts read. Evidence: docs/GAMEPLAY.md "Breakables".
#pragma once

#include "cdc/Cdc.hpp"
#include "physics/Physics.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace ou::game {

// ObjectBase +0xC8 record of a class-30 object (u16 +2 == 30). The object is drawn and collides as its pieces: record
// +0x18 -> 0x20-byte piece records (+0x1C count), each a child instance of the object on model byte +0x0A (0x6D4030 ->
// 0x5D07E0). A break (0x6D2960 / 0x6D2A50 -> 0x69BC70) hands every piece to the physics world as its own rigid body.
struct BreakableData {
    int hits = 0; // +0x10: hits of a listed damage class it takes (message 0x11C, 0x6D3160)
    struct Piece {
        int model = 0;                  // +0x0A
        physics::ObjectPhysics physics; // that model's collision, markup and mass
    };
    std::vector<Piece> pieces;
    // Action record (+0x18 of a condition): what a met condition starts (0x6D2BF0 -> 0x6D2C80).
    struct Action {
        int16_t id = 0;       // +0: scripts break it by this id (message 0x2C)
        int16_t type = 0;     // +2: 0 / 1 break at once, 2 / 3 after `delay`
        uint32_t flags = 0;   // +8: 0x10 the pieces' markup goes with the break (0x6D2050), 0x40 the wait only runs
                              // while the contact lasts (it is dropped after `grace` frames without it)
        float delay = 0;      // +0x1C: 30 Hz frames from the first contact to the break
        glm::vec3 shake{0};   // +0x20: the pieces' random jitter while it waits (units, per axis)
        float grace = 0;      // +0x38: frames
    };
    // Break condition (+0x34 -> 0x1C-byte records, +0x28 count). `flags`: Lara's contact kinds (message 0x2B from her
    // attached markup, 0x6EC510, or what she stands on): 0x08 standing on it (kind 0), 0x10 kind 1, 0x20 a horizontal
    // pole of it (2), 0x40 hanging from its ledge (3), 0x80 / 0x100 kinds 4 / 5, 0x04 kind 6 from anyone; 0x200 a
    // rigid-body impact (kind 10) faster than `speed` or spinning faster than `spin`; 0x400 a hit of damage class 6,
    // 0x800 / 0x1000 / 0x2000 Lara's hits of class 0 / 1 / 2 (0x69BFC0). `flags2` bit 0: walking its balance beam (13).
    // `classes`: damage classes that count as hits whoever deals them.
    struct Condition {
        uint32_t flags = 0, flags2 = 0;
        float speed = 0, spin = 0;
        std::vector<int32_t> classes;
        int action = -1; // index into `actions`, -1 none
    };
    std::vector<Condition> conditions;
    std::vector<Action> actions;
};
inline constexpr int kClassBreakable = 30;
// nullopt for objects of another class. Throws cdc::FormatError on a malformed record.
std::optional<BreakableData> loadBreakable(const cdc::Drm& object);

// Explosion record (tag 0x2775) of an effect object ("squib"): tru.exe's anim-graph records point at it (+0x18) and
// hand it to the area hit 0x69F1E0 when they start (0x6A5C50): every instance within `radius` of the squib takes
// `damage` (message 0x11C, damage class 4) and is thrown at `knock` .. `knockEdge` units per frame [I unit], `angle`
// degrees up. Record: +4 i16 damage, +6 u16, +8 u16, +0xA u16, +0xC i16 radius, +0xE i16 knock, +0x10 i16 knock at the
// edge, +0x12 u16 angle. [I] +6 (800 on the barrel's, as its damage) is the damage at the edge.
struct ExplosionRecord {
    float damage = 0, edgeDamage = 0, radius = 0, knock = 0, knockEdge = 0, angle = 0;
};
inline constexpr int kDamageClassBlast = 4;
// The record at the object's physical data +0x70; nullopt without one or when it deals no damage.
std::optional<ExplosionRecord> loadExplosion(const cdc::Drm& object);

} // namespace ou::game
