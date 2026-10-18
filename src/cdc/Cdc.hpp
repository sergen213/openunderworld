// Read-only parsers for Tomb Raider: Underworld (cdc engine, "TR8") PC containers.
// Format notes and evidence: docs/formats/CONTAINERS.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ou::cdc {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// CRC32 used for bigfile path hashes: MSB-first, poly 0x04C11DB7, init/xorout 0xFFFFFFFF,
// path lower-cased. Verified: "pc-w\\lara.drm" and "pc-w\\local\\locals.bin" resolve.
uint32_t pathHash(std::string_view path);

// ---- Bigfile (bigfile.000..00N, patch.000) --------------------------------------------
struct BigfileRecord {
    uint32_t hash;
    uint32_t size;
    uint32_t sector;    // file offset = sector << 11, spanning split parts
    uint32_t specMask;  // specialisation (language etc.); 0xFFFFFFFF = all
    uint32_t compressedSize; // 0 in every observed TRU PC record
};

// Specialisation (language) masks. tru.exe opens archives with mask 1 (InitArchive 0x473840) and
// then sets 1 << Language for every mounted archive (0x49A900); bits 0-4 = EN FR DE IT ES.
inline constexpr uint32_t kSpecEnglish = 1u << 0, kSpecFrench = 1u << 1, kSpecGerman = 1u << 2,
                          kSpecItalian = 1u << 3, kSpecSpanish = 1u << 4;

class Bigfile {
public:
    // `firstPart` is e.g. ".../bigfile.000". Split parts are found by replacing the extension.
    explicit Bigfile(std::filesystem::path firstPart, uint64_t partAlignment = kPartAlignment);

    static constexpr uint64_t kPartAlignment = 0x7FF00000; // verified against CDRM magics

    const std::vector<BigfileRecord>& records() const { return records_; }
    // All records with this hash (several specialisations may share one hash).
    std::span<const BigfileRecord> find(uint32_t hash) const;
    // First record whose mask contains every bit of `specMask`, as ArchiveFileSystem_FindEntry
    // 0x4726E0 does: (record.specMask & specMask) == specMask.
    std::optional<BigfileRecord> find(std::string_view path, uint32_t specMask = kSpecEnglish) const;

    std::vector<uint8_t> read(const BigfileRecord& r) const;
    std::filesystem::path partPath(uint32_t part) const;
    const std::filesystem::path& path() const { return first_; }

private:
    std::filesystem::path first_;
    uint64_t alignment_;
    std::vector<BigfileRecord> records_; // sorted by hash (as stored)
};

// The archives the game mounts: patch.000 (if present) in front of bigfile.000, so a path present
// in both comes from the patch (InitPatchArchive 0x478930 adds at the front; the multi-FS lookup
// 0x472B70 takes the first archive that has the file). The exe aborts without patch.000; we accept
// installs without it.
class BigfileSet {
public:
    explicit BigfileSet(const std::filesystem::path& gameDir, uint32_t specMask = kSpecEnglish);
    std::optional<std::vector<uint8_t>> read(std::string_view path) const;
    bool hasPatch() const { return archives_.size() > 1; }
    uint32_t specMask() const { return specMask_; }

private:
    std::vector<Bigfile> archives_; // lookup order
    uint32_t specMask_;
};

// ---- CDRM: zlib block container wrapping DRM files ------------------------------------
bool isCdrm(std::span<const uint8_t> data);
// Returns decompressed payload. Throws FormatError on truncation / bad zlib / size mismatch.
std::vector<uint8_t> decompressCdrm(std::span<const uint8_t> data);

// ---- DRM (version 19): section list ---------------------------------------------------
enum class SectionType : uint8_t {
    General = 0, Empty = 1, Animation = 2, PushBufferWC = 3, PushBuffer = 4, Texture = 5,
    Wave = 6, DtpData = 7, Script = 8, ShaderLib = 9, Material = 10, Object = 11,
    RenderResource = 12, PdaMap = 13,
};
std::string_view sectionTypeName(SectionType t);

struct Section {
    SectionType type;
    uint8_t flags;            // byte after type; meaning unknown
    uint16_t versionId;
    uint32_t packed;          // bit0 debug info, bits1-7 resource type, bits 8-31 reloc bytes
    uint32_t id;
    uint32_t specMask;
    uint32_t resourceType() const { return (packed >> 1) & 0x7F; }
    uint32_t relocBytes() const { return packed >> 8; }
    std::span<const uint8_t> relocations; // raw relocation table bytes
    std::span<const uint8_t> data;        // section payload
};

struct Drm {
    uint32_t version = 0;
    std::vector<std::string> dependencies; // names stored in header (objects, then units)
    std::vector<Section> sections;         // spans point into `bytes`
    std::vector<uint8_t> bytes;
};

inline constexpr uint32_t kDrmVersion = 19;
// Accepts raw DRM or CDRM-wrapped bytes.
Drm parseDrm(std::vector<uint8_t> bytes);

// Resolve a 32-bit pointer stored at `offset` inside section `s` via its internal
// relocation table. Returns the target offset within the same section's data if relocated.
std::optional<uint32_t> internalPointer(const Section& s, uint32_t offset);

// Internal and external relocations of `drm.sections[index]`: patch offset -> target
// {section index in drm.sections, offset in its data}.
struct Pointer { uint32_t section, offset; };
std::unordered_map<uint32_t, Pointer> sectionPointers(const Drm& drm, uint32_t index);

} // namespace ou::cdc
