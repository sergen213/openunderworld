// Game progression data: the chapter (level) sequence, level ends, pickups/collectibles and the music
// named by unit scripts. Evidence and [O]/[I]/[U] labels: docs/GAMEPLAY.md "Progression".
#pragma once

#include "cdc/Cdc.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::game {

// Start unit of each chapter: the table in pc-w\generalbank.drm script section 6 (offsets 0xF920..0xF9C0) [O],
// in that order; New Game starts chapter 0 (generalbank DTP 2: "chapter0", "l3_main_bedroom") after
// Movies\explode.bik (frontend state 8 "PlayNewGameMovie", tru.exe 0x5DB110) [O]. nameId: locals.bin
// chapter names 52..58 "MEDITERRANEAN SEA" .. "ARCTIC SEA" in chapter order [I mapping]; the prologue
// plays in Croft Manor units (cinematics 001_pro_corridor, 010_pro_surprise) [O] -> 54; the epilogue unit
// l7_himalaya_end ends the Arctic Sea chapter (cinematic 343) [I] -> 58.
struct Chapter {
    std::string_view unit;
    int16_t nameId;
};
inline constexpr std::array<Chapter, 9> kChapters{{
    {"l3_main_bedroom", 54},  // 0 prologue (burning Croft Manor)
    {"l1_seatop", 52},        // 1 Mediterranean Sea
    {"l2_cliffclimb", 53},    // 2 Coastal Thailand
    {"l3_caverns_start", 54}, // 3 Croft Manor
    {"l4_trailstart", 55},    // 4 Southern Mexico
    {"l5_startsnow", 56},     // 5 Jan Mayen Island
    {"l6_ship_exterior", 57}, // 6 Andaman Sea
    {"l7_thin_ice", 58},      // 7 Arctic Sea
    {"l7_himalaya_end", 58},  // 8 epilogue
}};
inline constexpr int16_t kStringOneWeekEarlier = 1307; // "ONE WEEK EARLIER..." (after the prologue) [I placement]
int chapterIndex(std::string_view startUnit); // -1 if no chapter starts there
// Chapter a unit belongs to when nothing else says (saves without a chapter): its l<N>_ prefix [I]; the
// prologue shares Croft Manor units with chapter 3 and is never derived.
int chapterOfUnit(std::string_view unit);
// A level end: a cinematic object whose script names chapter start units (all 7 chapter ends, survey in
// GAMEPLAY.md). The next chapter is the lowest-numbered one after `current` among the names; nullopt if
// none (l3_main_hall's names l1_seatop and l3_caverns_start: only the prologue leaves there) [I].
std::optional<int> nextChapter(int current, const std::vector<std::string>& startUnits);

// ---- pickups -------------------------------------------------------------------------------------
// Reward objects by objectlist name [O names/counts, I roles]: `*_rw_artifact` (7..22 per chapter) =
// treasures, `*_rw_treasure` (exactly one per chapter) = the chapter's relic, `*_rw_shard` = health packs
// (their base script 3924 names "g_healthpack" and the native types health/invslot/larabase) [O strings].
enum class PickupKind : uint8_t { Treasure, Relic, HealthPack };
std::optional<PickupKind> pickupKind(std::string_view objectName);
// locals.bin ids for the notification: 1251 "TREASURES", 1252 "RELICS", 1274 "HEALTH PACKS" [O strings].
int16_t pickupStringId(PickupKind k);
const char* pickupKindName(PickupKind k); // "treasure", "relic", "healthpack" (logs)
struct Pickup {
    int32_t id = 0; // intro unique id (World::collected keys)
    PickupKind kind = PickupKind::Treasure;
    glm::vec3 pos{0}; // unit frame
};
// [I] reach: the pickup base script (3674) uses a playersensor; Lara picks up on interact (hint 1300 "Press
// ... to pick up objects", lara_pickup_ground/crouch clips in the reward DTPs) within this cylinder.
inline constexpr float kPickupRadius = 200.0f, kPickupHeight = 250.0f;

// NUL-terminated identifiers ([A-Za-z0-9_], 4+ chars) in the data of the type-8 section with id
// `sectionId`, lower-cased, in order: the string constants a script pushes (unit and music names).
std::vector<std::string> scriptStrings(const cdc::Drm& drm, uint32_t sectionId);

} // namespace ou::game
