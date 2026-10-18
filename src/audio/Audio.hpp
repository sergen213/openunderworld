// TRU audio decoders: IMA ADPCM, FMOD FSB4 banks (DRM Wave sections, .sam), cdc MultiplexStream
// (.mul: voice, music, cinematic audio + subtitles). Layouts and evidence: docs/formats/AUDIO.md.
#pragma once

#include "cdc/Cdc.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ou::audio {

using cdc::FormatError;

// Decoded 16-bit PCM, interleaved.
struct Pcm {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples; // frames() * channels
    int64_t loopStart = -1;       // frame where a loop restarts; -1 = stream does not loop
    int64_t loopEnd = -1;         // exclusive frame where a loop wraps; -1 = end of data
    // Per-channel stereo gains (.mul header volLeft/volRight). Empty: mono = centre,
    // stereo = left/right, more channels = alternating left/right pairs.
    std::vector<float> volLeft, volRight;
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// ---- IMA ADPCM: 36-byte mono blocks {s16 sample, u8 stepIndex, u8 0, 32 bytes of nibbles,
// low nibble first} -> 64 samples. Both variants output the header sample first.
inline constexpr size_t kImaBlockBytes = 36, kImaBlockSamples = 64;
enum class ImaVariant {
    Xbox,            // FMOD FSB4: header sample, then nibbles 0..62 (standard IMA expansion)
    CrystalDynamics, // tru.exe .mul decoder 0x5957A0: header sample, nibble 0 skipped, nibbles 1..63,
                     // delta = (min(step,0x1FFF)*4 * op[nibble]) >> 16 (table init 0x595670)
};
// Decodes one block into out[0], out[stride], ... (64 samples).
void decodeImaBlock(const uint8_t* block, ImaVariant v, int16_t* out, size_t stride = 1);

// ---- FMOD FSB4 bank ("FSB4" magic). Observed in DRM Wave sections and music .sam files.
inline constexpr uint32_t kFsbModePcm16 = 0x10, kFsbModeMono = 0x20, kFsbModeStereo = 0x40,
                          kFsbModeImaAdpcm = 0x400000;
struct Fsb4Sample {
    std::string name;
    uint32_t numSamples = 0, dataSize = 0, loopStart = 0, loopEnd = 0, mode = 0;
    int32_t defaultRate = 0; // 8000/32000/44100/48000 seen; Wave sections override it (tru.exe 0x592A50)
    uint16_t channels = 0;
    float minDistance = 0, maxDistance = 0;
    size_t dataOffset = 0;   // from the start of the bank
};
struct Fsb4 {
    uint32_t version = 0, mode = 0;
    std::vector<Fsb4Sample> samples;
};
bool isFsb4(std::span<const uint8_t> d);
Fsb4 parseFsb4(std::span<const uint8_t> bank);
// IMA ADPCM mono and PCM16 mono/stereo; other modes throw FormatError.
Pcm decodeFsb4(std::span<const uint8_t> bank, const Fsb4Sample& s);

// ---- DRM section type 6 (Wave): {u32 sampleRate, u32 loopStart, u32 loopEnd, u32 unknown}
// + FSB4 bank with one sample.
struct Wave {
    uint32_t sampleRate = 0;
    uint32_t loopStart = 0, loopEnd = 0; // frames; loopEnd 0 = no loop points
    uint32_t unknown = 0;                // 10 or 100 in every retail section (meaning unknown)
    Fsb4 bank;
    std::span<const uint8_t> fsb; // into the section data
};
Wave parseWave(std::span<const uint8_t> sectionData);
Pcm decodeWave(std::span<const uint8_t> sectionData); // Wave::sampleRate and loop points

// ---- .mul MultiplexStream: 0x800-byte header, then packets {u32 type, u32 size, u32, u32}.
struct MulHeader {
    uint32_t sampleRate = 0;
    int32_t loopStart = -1; // frame, -1 = no loop
    uint32_t numSamples = 0, channels = 0;
    uint32_t hasCinematic = 0, hasSubtitles = 0;
    uint32_t loopStartFileOffset = 0; // header +0x2C: file offset of the packet holding loopStart
    float mediaLength = 0;            // header +0x38: length in 30 Hz frames
    float volLeft[12] = {}, volRight[12] = {};
};
// Voice-line subtitle; language numbers are tru.exe language indices (locale::Language:
// 0 EN, 1 FR, 2 DE, 3 IT, 4 ES, 7 PL, 9 RU, 10 CS, 11 NL, 12 HU observed).
struct Subtitle {
    uint32_t frame = 0;                                  // cinematic frame (30 Hz) it starts on
    std::vector<std::pair<uint32_t, std::string>> lines; // {language, UTF-8 text}
    std::string_view text(uint32_t language) const;      // empty if absent
};
struct Mul {
    MulHeader header;
    Pcm pcm;
    std::vector<Subtitle> subtitles;
    uint32_t cinematicPackets = 0;
    bool sceneFrames = false; // cinstream: frames carry scene channels; subtitles not extracted here
                              // (cine::scan / cine::Player handle those)
};
inline constexpr size_t kMulHeaderSize = 0x800;
MulHeader parseMulHeader(std::span<const uint8_t> d);
Mul decodeMul(std::span<const uint8_t> d);

// ---- Mus! music script (.mus): an AAD-style instrument bank + MIDI-like sequence whose meta
// events drive the gains of the .mul stems from global music variables. AUDIO.md §5.
inline constexpr uint32_t kMusVersion = 0x114;
struct MusSample {             // 0x34 bytes; offset/size address the .sam bank
    std::string name;
    uint32_t samOffset = 0, size = 0;
};
struct MusKeyRange {           // tone layer -> sample index; program zone -> tone index
    uint8_t keyLo = 0, keyHi = 0, velLo = 0, velHi = 0;
    uint32_t index = 0;
};
struct MusTone { std::string name; std::vector<MusKeyRange> layers; };
struct MusProgram { std::string name; uint32_t bank = 0, program = 0; std::vector<MusKeyRange> zones; };
struct MusStem {               // one per .mul channel group (0x58 bytes)
    uint32_t sampleRate = 0, channels = 0, frames = 0;
    uint32_t unknown14 = 0;    // 10..24 [U]
    uint32_t msPerBeat = 0;    // fade time unit (500 in every retail file)
    float volume = 1, fade = 1, mute = 1, pan = 0.5f; // initial gain factors
};
inline constexpr uint8_t kMusMeta = 0xFF; // MusEvent::status of a meta event
struct MusEvent {
    uint32_t offset = 0;       // of the delta VLQ, from track start (label/jump target space)
    uint32_t delta = 0;        // ticks
    uint8_t status = 0;        // 0x80..0xEF channel message, 0xF0 sysex, 0xFF meta
    uint8_t meta = 0;          // meta command (status 0xFF)
    std::vector<uint8_t> data;
};
struct MusSequence {           // "QSMa"
    uint32_t id = 0, tempo = 0; // tempo: microseconds per quarter note
    uint16_t ppqn = 0, unknown0a = 0;
    std::vector<std::vector<MusEvent>> tracks; // decoded up to the end-of-track meta 0x44
};
struct MusFile {
    uint32_t header04 = 0, version = 0, header20 = 0; // header04 = first sequence offset - 8 [I]
    std::vector<MusSequence> sequences;
    std::vector<MusStem> stems;
    std::vector<uint32_t> labels; // track byte offsets by label number
    std::vector<MusSample> samples;
    std::vector<MusTone> tones;
    std::vector<MusProgram> programs;
};
// Throws FormatError on bad magic/version, truncation, undecodable events, or operands of the
// commands MusSequencer executes that are out of range (stem, label).
MusFile parseMus(std::span<const uint8_t> d);

// The exe's music sequencer (0x5950A0 tick, 0x594520 track loop, 0x580F80 meta dispatch, stem
// fades 0x5891B0), reduced to what drives stem gains: variables, branches/gotos, end of track,
// stem volume/fade/tempo. Other commands (channel/slot/loop/substitution/mute-ramp) are ignored;
// no retail file uses them. Keeps a reference to `f`.
class MusSequencer {
public:
    explicit MusSequencer(const MusFile& f, size_t sequence = 0);
    // Ramps fades by `seconds`, advances the tempo clock and runs due events against the global
    // music variables (exe byte array 0xCCB560). A taken jump ends that track's frame, so scripts
    // poll their variables once per update.
    void update(float seconds, std::span<uint8_t, 256> vars);
    std::vector<float> stemGains() const; // per stem: volume * fade * mute (0x5864E0)
    bool finished() const;                // every track hit meta 0x44

private:
    struct Track { size_t pos = 0; uint64_t time = 0; bool active = true; };
    struct Fade { float target = 0, rate = 0, left = 0; bool on = false; };
    const MusFile& f_;
    const MusSequence& seq_;
    std::vector<Track> tracks_;
    std::vector<MusStem> stems_;
    std::vector<Fade> fades_;
    double seconds_ = 0;
};

} // namespace ou::audio
