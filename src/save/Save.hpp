// OpenUnderworld's own save games: versioned JSON with a CRC32 header line, atomic writes with one
// backup copy. Format, versions and file layout: docs/SAVES.md.
#pragma once

#include "game/Game.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ou::save {

inline constexpr int kVersion = 2; // written version; parse() migrates 1 -> 2

struct SaveError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SaveGame {
    std::string unit;          // unit Lara is in; positions below are in its frame
    game::PlayerState player;  // `ledge` / `water` are replaced by the unit-relative references below
    std::string ledgeUnit;     // empty = not on a ledge
    int ledgeIndex = -1;       // markup index within ledgeUnit
    std::string waterUnit;     // empty = not swimming
    int waterIndex = -1;       // water markup index within waterUnit
    float cameraYaw = 0, cameraPitch = -0.25f;
    std::optional<game::CheckpointRef> checkpoint;
    std::set<int32_t> collected; // World::collected (pickup intro ids)
    int chapter = 0; // game::kChapters index; saves without it: chapterOfUnit(unit), else 0
    std::array<int, 3> found{}; // World::found (per PickupKind)
    std::string outfit; // World::outfit (objectlist name, [a-z0-9_]); empty (older saves) = the unit's
    std::optional<float> health; // Lara's health (World::health); missing = full
    nlohmann::json script = nlohmann::json::object(); // trigger/puzzle state (src/script), opaque here
    nlohmann::json enemies; // ai::Enemies::serialize(), opaque here; null (older saves) = enemies left as they are
    uint64_t ticks = 0;        // simulation ticks played when saved (informational)
};

// World <-> SaveGame. capture() needs a streaming World (units() not empty).
SaveGame capture(const game::World& world);
// Streams the save's unit in (World::enterUnit) and restores Lara, camera, checkpoint, items and script.
void apply(const SaveGame& save, game::World& world);

// File bytes: "OUSAVE <version> <crc32 hex of body> <body bytes>\n" + JSON body.
std::string serialize(const SaveGame& save);
// Rejects bad magic, truncation, CRC mismatch, unknown versions and schema errors (SaveError);
// migrates older versions.
SaveGame parse(std::string_view bytes);

std::filesystem::path saveDir(); // $XDG_DATA_HOME/openunderworld/saves (fallback ~/.local/share)
// <dir>/<slot>.ousave; slot is 1-32 of [A-Za-z0-9_-] (throws SaveError otherwise).
std::filesystem::path slotPath(std::string_view slot, const std::filesystem::path& dir = saveDir());

// Atomic replace: writes <path>.tmp, fsyncs, hard-links the current file to <path>.bak, renames the
// tmp over <path>, fsyncs the directory. Creates the directory. Throws SaveError.
void writeFile(const std::filesystem::path& path, std::string_view bytes);
// parse() of <path>; when it is missing or invalid, the backup <path>.bak (sets *usedBackup).
// Throws SaveError naming both failures.
SaveGame readFile(const std::filesystem::path& path, bool* usedBackup = nullptr);

} // namespace ou::save
