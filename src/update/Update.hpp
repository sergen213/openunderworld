// Signed updater core for OpenUnderworld's own binaries (never game data). No Qt here.
//
// Trust: a detached Ed25519 signature (libsodium) over the exact manifest bytes, checked before the
// manifest is parsed, against a public key compiled into the binary. Every file is checked for size
// and SHA-256, staged under the install root, then swapped in with renameat2(RENAME_EXCHANGE).
// A journal makes an interrupted swap repairable on the next start. Design notes: docs/UPDATES.md.
//
// Install root layout (all project-owned):
//   .openunderworld-install   ownership marker; a non-empty root without it is refused
//   .lock                     flock() held during install / rollback / recovery
//   current/                  active version (contains .ou-manifest.json + .ou-manifest.json.sig)
//   previous/                 backup of the version before the last swap (rollback target)
//   .staging/                 fully verified download of the next version
//   .journal                  present only while a swap is in progress: {"version": "<target>"}
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ou::update {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using PublicKey = std::array<uint8_t, 32>;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct FileEntry {
    std::string path;   // relative, '/'-separated, inside the version directory
    uint64_t size = 0;
    std::string sha256; // lowercase hex
    std::string url;    // https:// only
    bool executable = false;
};

struct Manifest {
    std::string version, channel, changelog, minVersion; // minVersion: oldest installed version allowed to update
    std::vector<FileEntry> files;
};

constexpr size_t kMaxManifestBytes = 1 << 20;
constexpr std::string_view kManifestName = ".ou-manifest.json";

// The release key compiled in via -DOU_UPDATE_PUBLIC_KEY=<64 hex>; nullopt if the build has none.
std::optional<PublicKey> releasePublicKey();
std::optional<PublicKey> parsePublicKeyHex(std::string_view hex);

bool verifySignature(std::span<const uint8_t> message, std::span<const uint8_t> signature, const PublicKey& key);
// Validates schema, versions, https URLs, safe relative paths, no duplicates, no game-data files.
Manifest parseManifest(std::string_view json);
// Signature first; the bytes are only parsed after it verifies. Throws Error.
Manifest verifyAndParse(std::span<const uint8_t> manifest, std::span<const uint8_t> signature, const PublicKey& key);

// Dotted numeric versions ("0.1.0", "1.2"); missing components are 0. Throws Error on malformed input.
int compareVersions(std::string_view a, std::string_view b);
std::string sha256Hex(std::span<const uint8_t> data);

// Transport: returns the body or throws. Must abort once more than maxBytes arrive.
using Fetch = std::function<Bytes(const std::string& url, uint64_t maxBytes)>;
Fetch httpsFetch(); // libcurl, https only (redirects too), TLS >= 1.2

struct Options {
    fs::path root;
    std::string manifestUrl; // signature is fetched from manifestUrl + ".sig" (64 raw bytes)
    std::string channel = "stable";
    PublicKey key{};
    Fetch fetch;
    bool enabled = true;                  // config switch; OU_DISABLE_UPDATES=1 also disables
    std::vector<fs::path> forbiddenRoots; // e.g. the game directory: the root may not overlap it
    std::function<void(const std::string&)> progress;
};

enum class Status { Disabled, UpToDate, Available, Installed };
struct Result {
    Status status = Status::Disabled;
    std::optional<std::string> installedVersion; // before the operation
    Manifest manifest;                           // the verified remote manifest (unless Disabled)
};

bool disabledByEnvironment();
Result check(const Options& opt);   // fetch + verify manifest, compare; never writes
Result update(const Options& opt);  // check, then install if newer

// Lower-level pieces (used by update(); exposed for tests and the CLI).
void install(const fs::path& root, const Manifest& m, std::span<const uint8_t> manifestBytes,
             std::span<const uint8_t> signature, const Fetch& fetch,
             const std::vector<fs::path>& forbiddenRoots = {},
             const std::function<void(const std::string&)>& progress = {});
std::string rollback(const fs::path& root);  // swap current <-> previous; returns the now-active version
std::string recover(const fs::path& root);   // repair an interrupted swap; "" if nothing to do
std::optional<std::string> installedVersion(const fs::path& root);
std::optional<std::string> versionOf(const fs::path& versionDir);

} // namespace ou::update
