// Read-only installation importer: inventories a Tomb Raider: Underworld install into a JSON
// manifest (file hashes, exe identification, bigfile record analysis, name recovery).
// Nothing in here ever opens a game file for writing. Notes: docs/formats/INVENTORY.md.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::importer {

using nlohmann::json;
namespace fs = std::filesystem;

std::string_view toolVersion();

struct ScanOptions {
    bool deep = false;           // fully decompress + parse every CDRM, harvest names from DRM strings
    bool hashFiles = true;       // sha256/md5 every install file (slowest part on HDD)
    unsigned threads = 0;        // 0 = hardware_concurrency
    uint64_t partAlignment = 0x7FF00000; // tests use small values to exercise split parts
    std::vector<fs::path> nameLists; // optional external name lists (one path per line, ';' comments)
};

json scan(const fs::path& gameDir, const ScanOptions& opt = {});
json diffManifests(const json& a, const json& b);

// Extract one record (patch.000 wins over bigfile, as the game does). `nameOrHash` is a bigfile
// path ("pc-w\\lara.drm") or a hash ("0x1234abcd"). CDRM payloads are decompressed unless `raw`.
struct Extracted {
    std::vector<uint8_t> bytes;
    std::string archive;  // "bigfile" | "patch"
    uint32_t hash = 0, specMask = 0;
    size_t variants = 0;  // records sharing the hash in that archive
};
Extracted extract(const fs::path& gameDir, std::string_view nameOrHash, std::optional<uint32_t> specMask,
                  bool raw, uint64_t partAlignment = 0x7FF00000);

// True if `p` is `dir` or lies below it (after resolving symlinks of existing prefixes).
bool isInside(const fs::path& p, const fs::path& dir);
// $XDG_DATA_HOME/openunderworld/manifests/<prefix>.json (fallback ~/.local/share).
fs::path defaultManifestPath(std::string_view exeSha256);

// ---- pieces exposed for tests ----------------------------------------------------------
struct PeInfo {
    uint16_t machine = 0;
    uint32_t timestamp = 0;
    std::optional<std::string> fixedFileVersion, fixedProductVersion; // from VS_FIXEDFILEINFO
    std::vector<std::pair<std::string, std::string>> strings;           // StringFileInfo key/value
};
std::optional<PeInfo> parsePe(const std::vector<uint8_t>& image);

struct HashDbEntry {
    std::string path; // UTF-8, '\\' separators as stored
    std::string md5;  // lowercase hex
};
// GOG goggame-*.hashdb: a ZIP holding one deflated table {u32 ?, u32 ?, u32 count, then count x
// (char path[1024] UTF-8 (Latin-1 accepted), char md5hex[32])}. Throws std::runtime_error if the layout doesn't hold.
std::vector<HashDbEntry> parseGogHashdb(const fs::path& file);

} // namespace ou::importer
