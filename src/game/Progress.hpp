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
#include <set>
#include <string>
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
// Thor's gear objects Lara carries entering chapter n (GAMEPLAY.md "Thor's gear"). [O] the original under Wine
// (larabase 38's slots, *(*0xFADB58+0x500)): a new game's prologue and a load of l4_trailstart carry none; loading
// l5_startsnow (even from a fresh prologue) gives both gauntlets and the belt, and l7_thin_ice keeps all three.
// Carrying is not wearing: the draw groups stay hidden in all of these (GAMEPLAY.md). Mjolnir ("wp_thorhammer", larabase
// 37 at the end of l5_valaskjalf's 250_val_hammer) from chapter 6 on [I: no unit script gives it again; the Andaman
// Sea start has it, GAMEPLAY.md "Mjolnir"].
inline std::set<std::string> chapterGear(int chapter) {
    if (chapter < 5) return {};
    if (chapter == 5) return {"g_belt", "g_gauntletleft", "g_gauntletright"};
    return {"g_belt", "g_gauntletleft", "g_gauntletright", "wp_thorhammer"};
}
int chapterIndex(std::string_view startUnit); // -1 if no chapter starts there
// Chapter a unit belongs to when nothing else says (saves without a chapter): its l<N>_ prefix [I]; the
// prologue shares Croft Manor units with chapter 3 and is never derived.
int chapterOfUnit(std::string_view unit);
// A level end: a cinematic object whose script names chapter start units (all 7 chapter ends, survey in
// GAMEPLAY.md). The next chapter is the lowest-numbered one after `current` among the names; nullopt if
// none (l3_main_hall's names l1_seatop and l3_caverns_start: only the prologue leaves there) [I].
std::optional<int> nextChapter(int current, const std::vector<std::string>& startUnits);

// ---- pickups -------------------------------------------------------------------------------------
// Reward objects by objectlist name [O: the reward table below lists every `*_rw_artifact` intro and every urn
// `*_pt_weightbreak_reward` as a treasure and every `*_rw_treasure` as a relic]: `*_rw_artifact` = treasures lying in
// the world, `tp_rw_artifact_spawned` = the treasure a reward urn gives birth to when it breaks, `*_rw_treasure`
// (exactly one per chapter with any) = the chapter's relic, `*_rw_shard` = health packs (their base script 3924 names
// "g_healthpack" and the native types health/invslot/larabase) [O strings].
enum class PickupKind : uint8_t { Treasure, Relic, HealthPack };
std::optional<PickupKind> pickupKind(std::string_view objectName);
// locals.bin ids for the notification: 1251 "TREASURES", 1252 "RELICS", 1274 "HEALTH PACKS" [O strings].
int16_t pickupStringId(PickupKind k);
const char* pickupKindName(PickupKind k); // "treasure", "relic", "healthpack" (logs)
struct Pickup {
    int32_t id = 0; // intro unique id (World::collected keys); a born treasure's formats::bornUid(urn)
    PickupKind kind = PickupKind::Treasure;
    glm::vec3 pos{0}; // unit frame
    int32_t parent = 0;  // the urn whose breaking gives birth to it (World::born holds `id` from then on); 0: placed
    uint16_t reward = 0; // index in the game's reward table (intro data +0x30; the urn's for a born treasure); 0: none
};
// The reward table: generalbank.drm's DTP named by GlobalInfo +0x194 (tru.exe 0x5D5F80 builds the totals from it; the
// `rewardsystem` script type's natives 0x5D6200.. and the PDA read them). +0 reward count (206: index 0 unused), +4 ->
// rewards of 8 bytes {u16 type}, +0x10 group count, +0x14 -> groups of 0x10 bytes {category, found (runtime), count,
// -> reward indices}, +0x58 float[3] health given per category, +0x64 float[3] headroom per category. Category 1 =
// treasures, 2 = relics (0: twenty rewards, 59..78, that no unit places). A reward's type is its area: 1 Mediterranean,
// 2 Thailand, 3 Croft Manor, 4 Mexico, 5 Jan Mayen, 7 Arctic Sea (= the kChapters index); types 1..7 add up into type
// 0, the totals (0x5D5F80's switch). Collecting reward n (0x5DB6A0) sets bit n of the saved 2048-bit set (game state
// +0x3ED90) and counts found[category][type] (+0x3EE90).
struct RewardTable {
    struct Group {
        int category = 0;
        std::vector<uint32_t> rewards;
    };
    std::vector<uint16_t> types; // per reward index
    std::vector<Group> groups;
    std::array<float, 3> heal{};     // +0x58: 0, 0, 300
    std::array<float, 3> headroom{}; // +0x64: 0, 0, 50
    // rewards of `category` whose type is `type` (0 = every type)
    int total(int category, int type = 0) const;
};
std::optional<RewardTable> loadRewardTable(const cdc::BigfileSet& bigfile); // nullopt: generalbank or the table missing
inline constexpr int kRewardTreasures = 1, kRewardRelics = 2; // RewardTable categories
// A relic (tru.exe 0x5DB6A0, reward table category 2) [O]: Lara's maximum health is base x difficulty x (1 + relics
// found / relics in the table) (0x6E7990 -> 0x5D6170: six relics double it), read live, so it rises at once; then
// she is healed by `heal` (300), but to no more than the new maximum + `headroom` (50), with overheal allowed
// (0x6F4EF0(amount, 1)): health above the maximum decays as after a health pack (Health::tick).
inline constexpr float kRelicHeal = 300.0f, kRelicHeadroom = 50.0f; // RewardTable::heal[2] / headroom[2]
inline constexpr int kRelicCount = 6;                               // RewardTable::total(kRewardRelics)
// Health packs (g_healthpack data, GAMEPLAY.md "Combat") [O]: +1 per pickup (+0xE8), at most 100 carried at every
// AMMO tailoring level (+0xE2 / +0xE4 / +0xE6; 0x6F5E70 leaves the pack lying when she is full).
inline constexpr int kHealthPackMax = 100;
// Weapon intros (and armed enemies' bodies) Lara takes ammunition from: game::ammoWeapon(object) names the gun whose
// reserve it fills (Combat.hpp).
struct WeaponPickup {
    int32_t id = 0; // intro unique id (World::collected keys)
    std::string object;
    glm::vec3 pos{0}; // unit frame
};
// [I] reach: the pickup base script (3674) uses a playersensor; Lara picks up on interact (hint 1300 "Press
// ... to pick up objects", lara_pickup_ground/crouch clips in the reward DTPs) within this cylinder.
inline constexpr float kPickupRadius = 200.0f, kPickupHeight = 250.0f;
// PDA sonar map reveal (World::mapReveal): a point every this many units Lara moves [I]. The original marks the reveal
// volumes (PDAMap RevealedInfo box / sphere / capsule) Lara's position + 200 z lies in (tru.exe 0x6E33D0), not decoded.
inline constexpr float kSonarRevealStep = 1000.0f;

// Treasure and relic ids per chapter (kChapters index; the epilogue shares the Arctic Sea's), for the PDA's AREA INFO
// found / total and the EXTRAS unlocks [O: the reward table's groups, mapped to intros by the reward index in each
// reward intro's data; `ChaptersReal.RewardSurveyMatchesTheRewardTable` checks every unit against it]. A treasure in
// an urn is listed by its born id (formats::bornUid(urn intro)). Treasures 26 / 30 / 13 / 50 / 30 / 0 / 30 = 179, one
// relic per chapter with any = 6; the prologue and the Andaman Sea have none.
struct ChapterRewards {
    int32_t relic = 0; // 0 = none
    std::vector<int32_t> treasures;
};
const ChapterRewards& chapterRewards(int chapter);

// NUL-terminated identifiers ([A-Za-z0-9_], 4+ chars) in the data of the type-8 section with id
// `sectionId`, lower-cased, in order: the string constants a script pushes (unit and music names).
std::vector<std::string> scriptStrings(const cdc::Drm& drm, uint32_t sectionId);

} // namespace ou::game
