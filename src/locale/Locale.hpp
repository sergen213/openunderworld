// Localized text: locals.bin string tables, movie subtitle schemas (.sch) and in-engine cinematic
// subtitles carried in cinstream .mul files. Formats and evidence: docs/formats/TEXT.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ou::locale {

// tru.exe language index (the order of its lang_* name table at 0x4A0720, the BINK audio track id,
// the cinematic subtitle language number and the spec-mask bit: mask = 1 << index).
enum class Language : int {
    English = 0, French, German, Italian, Spanish, Japanese, Portuguese, Polish, UkEnglish,
    Russian, Czech, Dutch, Hungarian,
};
inline constexpr int kLanguageCount = 13;

std::string_view languageName(Language l);        // "lang_english" ... (the .sch keys)
std::optional<Language> parseLanguage(std::string_view s); // "en", "english", "lang_english", "11"
inline uint32_t specMask(Language l) { return 1u << static_cast<int>(l); }

// ---- locals.bin -------------------------------------------------------------------------
// u32 headerLanguage; u32 count; u32 offset[count] (absolute, 0 = empty); NUL-terminated UTF-8.
class Strings {
public:
    // Throws cdc::FormatError when the header, the offset table or any string (offset past the end,
    // missing NUL) is truncated.
    explicit Strings(std::vector<uint8_t> bytes);

    // Same results as tru.exe's getter (0x49A7A0) after its loader (0x49A800): out-of-range ids give
    // "ERROR OUT OF TABLE RANGE", offset 0 gives "", strings failing the exe's UTF-8 check give
    // "Invalid UTF8 encoding".
    std::string_view get(int id) const;
    size_t size() const { return offsets_.size(); }
    // locals.bin header value; NOT the Language enum (IT = 2, DE = 3 here; see TEXT.md).
    uint32_t headerLanguage() const { return headerLanguage_; }
    bool valid(int id) const; // in range, non-empty offset and passes the exe UTF-8 check

private:
    std::vector<uint8_t> bytes_;
    std::vector<uint32_t> offsets_;
    uint32_t headerLanguage_ = 0;
};

// pc-w\local\locals.bin for `lang` (spec mask 1 << lang; patch.000 first). nullopt = not present.
std::optional<Strings> loadStrings(const std::filesystem::path& gameDir, Language lang);

// tru.exe's UTF-8 check (0x597DC0): no C0 C1 F5..FF anywhere, lead-byte length classes,
// continuation bytes must have the top bit set.
bool exeUtf8Valid(std::string_view s);

// ---- Movie subtitles: \tr8\local\localization\movies\<movie>.sch --------------------------
struct Cue {
    double start = 0; // seconds of movie time (Bink frame / fps)
    std::string text; // '\n' separates lines
};
// Picks the line whose start matches languageName(lang) (case-insensitive, like the exe) and splits
// its quoted text at "\(seconds)" markers; "\n" escapes become newlines. Empty = language absent.
std::vector<Cue> parseMovieSchema(std::string_view sch, Language lang);
std::string movieSchemaPath(std::string_view movieStem); // "\\tr8\\local\\localization\\movies\\x.sch"

// ---- Cinematic subtitles inside pc-w\cinstream\*.mul ----------------------------------------
// Concatenated payloads of the .mul's cinematic packets (type 1), i.e. the "CINE" stream.
std::vector<uint8_t> mulCinematicStream(std::span<const uint8_t> mul);

struct CineSubtitle {
    int frame = 0;      // cinematic frame (30 per second)
    int language = 0;   // Language index
    std::string text;   // as stored ('\n' separated lines, "Speaker: " prefixes kept)
};
struct CineInfo {
    std::string unitName;
    int channels = 0;   // per-frame float channels derived from the header
    int frames = 0;     // frames before the -1 terminator
    std::vector<CineSubtitle> subtitles;
};
// Parses the CINE header (to get the channel count) and walks every frame. Throws cdc::FormatError
// on a malformed stream.
CineInfo parseCineStream(std::span<const uint8_t> cine);

// ---- On-screen subtitle state, as tru.exe's two-line ticker (0x583D50/0x583DB0/0x583E20) ----
// A pushed line goes to the bottom slot (the previous bottom line moves up); every 90 ticks
// (1 tick = 1/30 s, gameTracker timeMult units) the bottom line moves up and the top one drops off.
class SubtitleTicker {
public:
    void push(std::string_view line);
    void advance(double ticks);
    const std::string& top() const { return lines_[0]; }
    const std::string& bottom() const { return lines_[1]; }

private:
    std::string lines_[2];
    double timer_ = 0;
};

// On-screen lines of one cinematic subtitle text, as 0x4538D0: split at '\n', and drop everything
// up to the first ": " of a line (the speaker name).
std::vector<std::string> cineSubtitleLines(std::string_view text);

// Movie playback driver for SubtitleTicker, as 0x4A0910: every cue with start <= t is pushed (each
// "\n"-separated line separately) exactly once.
class MovieSubtitles {
public:
    explicit MovieSubtitles(std::vector<Cue> cues) : cues_(std::move(cues)) {}
    // `t` movie seconds; `dtTicks` elapsed 1/30 s ticks since the last call.
    void update(double t, double dtTicks);
    const SubtitleTicker& ticker() const { return ticker_; }

private:
    std::vector<Cue> cues_;
    size_t next_ = 0;
    SubtitleTicker ticker_;
};

} // namespace ou::locale
