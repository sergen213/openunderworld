#include "audio/Audio.hpp"

#include <algorithm>
#include <cstring>

namespace ou::audio {
namespace {

uint32_t u32(std::span<const uint8_t> d, size_t o) {
    if (o + 4 > d.size()) throw FormatError("audio: truncated");
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}
uint16_t u16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size()) throw FormatError("audio: truncated");
    return uint16_t(d[o] | d[o + 1] << 8);
}
float f32(std::span<const uint8_t> d, size_t o) {
    uint32_t v = u32(d, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

constexpr int kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

int16_t clamp16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }

} // namespace

void decodeImaBlock(const uint8_t* b, ImaVariant v, int16_t* out, size_t stride) {
    int sample = int16_t(b[0] | b[1] << 8);
    int index = std::min<int>(b[2], 88);
    out[0] = int16_t(sample);
    for (int i = 1; i < int(kImaBlockSamples); ++i) {
        const int n = v == ImaVariant::Xbox ? i - 1 : i;
        const int code = (b[4 + n / 2] >> ((n & 1) * 4)) & 15;
        const int step = kStep[index];
        int delta;
        if (v == ImaVariant::Xbox) {
            delta = step >> 3;
            if (code & 1) delta += step >> 2;
            if (code & 2) delta += step >> 1;
            if (code & 4) delta += step;
            if (code & 8) delta = -delta;
        } else {
            int op = (code & 1) ? 0x1800 : 0x800; // tru.exe 0x595670 op table, used with pmulhw
            if (code & 2) op += 0x2000;
            if (code & 4) op += 0x4000;
            if (code & 8) op = -op;
            delta = (std::min(step, 0x1FFF) * 4 * op) >> 16;
        }
        sample = clamp16(sample + delta);
        index = std::clamp(index + kIndex[code], 0, 88);
        out[size_t(i) * stride] = int16_t(sample);
    }
}

// ---- FSB4 -------------------------------------------------------------------------------
bool isFsb4(std::span<const uint8_t> d) { return d.size() >= 4 && std::memcmp(d.data(), "FSB4", 4) == 0; }

Fsb4 parseFsb4(std::span<const uint8_t> d) {
    if (!isFsb4(d)) throw FormatError("fsb4: bad magic");
    const uint32_t n = u32(d, 4), shdr = u32(d, 8), data = u32(d, 12);
    Fsb4 f;
    f.version = u32(d, 16);
    f.mode = u32(d, 20);
    const uint64_t end = 0x30ull + shdr + data;
    if (end > d.size()) throw FormatError("fsb4: truncated bank");
    if (n > shdr / 8) throw FormatError("fsb4: sample count exceeds header size");
    size_t h = 0x30, dataOff = 0x30 + size_t(shdr);
    const size_t hEnd = dataOff;
    for (uint32_t i = 0; i < n; ++i) {
        Fsb4Sample s;
        if (i > 0 && (f.mode & 2)) { // FMOD_FSB_SOURCE_BASICHEADERS: {numSamples, dataSize}
            if (h + 8 > hEnd) throw FormatError("fsb4: truncated sample header");
            s = f.samples[0];
            s.name.clear();
            s.numSamples = u32(d, h);
            s.dataSize = u32(d, h + 4);
            h += 8;
        } else {
            const size_t size = u16(d, h);
            if (size < 0x50 || h + size > hEnd) throw FormatError("fsb4: bad sample header");
            const char* name = reinterpret_cast<const char*>(d.data() + h + 2);
            s.name.assign(name, strnlen(name, 30));
            s.numSamples = u32(d, h + 0x20);
            s.dataSize = u32(d, h + 0x24);
            s.loopStart = u32(d, h + 0x28);
            s.loopEnd = u32(d, h + 0x2C);
            s.mode = u32(d, h + 0x30);
            s.defaultRate = int32_t(u32(d, h + 0x34));
            s.channels = u16(d, h + 0x3E);
            s.minDistance = f32(d, h + 0x40);
            s.maxDistance = f32(d, h + 0x44);
            h += size;
        }
        s.dataOffset = dataOff;
        dataOff += s.dataSize;
        if (dataOff > end) throw FormatError("fsb4: sample data out of bounds");
        f.samples.push_back(std::move(s));
    }
    return f;
}

Pcm decodeFsb4(std::span<const uint8_t> bank, const Fsb4Sample& s) {
    if (s.dataOffset + uint64_t(s.dataSize) > bank.size()) throw FormatError("fsb4: truncated data");
    const auto data = bank.subspan(s.dataOffset, s.dataSize);
    Pcm p;
    p.sampleRate = uint32_t(s.defaultRate);
    p.channels = s.channels ? s.channels : (s.mode & kFsbModeStereo) ? 2 : 1;
    if (s.mode & 2) p.loopStart = s.loopStart; // FSOUND_LOOP_NORMAL
    if (s.mode & kFsbModeImaAdpcm) {
        if (p.channels != 1) throw FormatError("fsb4: multichannel IMA ADPCM not supported");
        const size_t blocks = data.size() / kImaBlockBytes;
        if (s.numSamples > blocks * kImaBlockSamples) throw FormatError("fsb4: truncated ADPCM data");
        p.samples.resize(blocks * kImaBlockSamples);
        for (size_t b = 0; b < blocks; ++b)
            decodeImaBlock(data.data() + b * kImaBlockBytes, ImaVariant::Xbox, p.samples.data() + b * kImaBlockSamples);
        p.samples.resize(s.numSamples);
    } else if (s.mode & kFsbModePcm16) {
        const size_t n = size_t(s.numSamples) * p.channels;
        if (n * 2 > data.size()) throw FormatError("fsb4: truncated PCM data");
        p.samples.resize(n);
        std::memcpy(p.samples.data(), data.data(), n * 2);
    } else {
        throw FormatError("fsb4: unsupported sample mode");
    }
    return p;
}

// ---- Wave section -----------------------------------------------------------------------
Wave parseWave(std::span<const uint8_t> d) {
    Wave w;
    w.sampleRate = u32(d, 0);
    w.loopStart = u32(d, 4);
    w.loopEnd = u32(d, 8);
    w.unknown = u32(d, 12);
    w.fsb = d.subspan(16);
    w.bank = parseFsb4(w.fsb);
    if (w.bank.samples.size() != 1) throw FormatError("wave: expected one FSB4 sample");
    return w;
}

Pcm decodeWave(std::span<const uint8_t> d) {
    const Wave w = parseWave(d);
    Pcm p = decodeFsb4(w.fsb, w.bank.samples[0]);
    p.sampleRate = w.sampleRate;
    if (w.loopEnd > w.loopStart && w.loopEnd <= p.frames()) {
        p.loopStart = w.loopStart;
        p.loopEnd = w.loopEnd;
    }
    return p;
}

// ---- .mul ---------------------------------------------------------------------------------
std::string_view Subtitle::text(uint32_t language) const {
    for (const auto& [l, t] : lines)
        if (l == language) return t;
    return {};
}

MulHeader parseMulHeader(std::span<const uint8_t> d) {
    if (d.size() < kMulHeaderSize) throw FormatError("mul: truncated header");
    MulHeader h;
    h.sampleRate = u32(d, 0);
    h.loopStart = int32_t(u32(d, 4));
    h.numSamples = u32(d, 8);
    h.channels = u32(d, 12);
    h.hasCinematic = u32(d, 0x20);
    h.hasSubtitles = u32(d, 0x24);
    h.loopStartFileOffset = u32(d, 0x2C);
    h.mediaLength = f32(d, 0x38);
    for (int i = 0; i < 12; ++i) {
        h.volLeft[i] = f32(d, 0x3C + 4 * i);
        h.volRight[i] = f32(d, 0x6C + 4 * i);
    }
    if (h.sampleRate < 1000 || h.sampleRate > 192000 || h.channels < 1 || h.channels > 12)
        throw FormatError("mul: implausible header");
    return h;
}

namespace {

// Cinematic frame list: {u32 size (bytes after this field), u32 frameIndex, payload}. Frames of
// scene-less streams (all voice lines) have payload {u32 textLength, text, padding}.
void parseCineFrames(std::span<const uint8_t> p, Mul& m) {
    size_t q = 0;
    while (q + 8 <= p.size()) {
        const uint32_t size = u32(p, q);
        if (size < 4 || q + 4 + size > p.size()) throw FormatError("mul: bad cinematic frame");
        if (!m.sceneFrames && size >= 8) {
            const uint32_t len = u32(p, q + 8);
            if (len) {
                if (12 + uint64_t(len) > 4 + uint64_t(size)) throw FormatError("mul: bad subtitle length");
                Subtitle s;
                s.frame = u32(p, q + 4);
                std::string_view t(reinterpret_cast<const char*>(p.data() + q + 12), len);
                // "<lang>\r<text>\r<lang>\r<text>\r"
                while (!t.empty()) {
                    const size_t a = t.find('\r');
                    if (a == t.npos) break;
                    const size_t b = t.find('\r', a + 1);
                    const std::string_view num = t.substr(0, a), text = t.substr(a + 1, (b == t.npos ? t.size() : b) - a - 1);
                    if (num.empty() || num.find_first_not_of("0123456789") != num.npos)
                        throw FormatError("mul: bad subtitle language");
                    s.lines.emplace_back(uint32_t(std::stoul(std::string(num))), std::string(text));
                    t = b == t.npos ? std::string_view{} : t.substr(b + 1);
                }
                m.subtitles.push_back(std::move(s));
            }
        }
        q += 4 + size;
    }
}

} // namespace

Mul decodeMul(std::span<const uint8_t> d) {
    Mul m;
    m.header = parseMulHeader(d);
    const uint32_t ch = m.header.channels;
    Pcm& p = m.pcm;
    p.sampleRate = m.header.sampleRate;
    p.channels = uint16_t(ch);
    p.loopStart = m.header.loopStart;
    p.volLeft.assign(m.header.volLeft, m.header.volLeft + ch);
    p.volRight.assign(m.header.volRight, m.header.volRight + ch);
    // Never trust the header for a huge reservation: 36 bytes hold 64 samples.
    p.samples.reserve(std::min<size_t>(size_t(m.header.numSamples) * ch, d.size() / kImaBlockBytes * kImaBlockSamples));

    size_t frames = 0;
    bool firstCine = true;
    size_t pos = kMulHeaderSize;
    while (pos + 16 <= d.size()) {
        const uint32_t type = u32(d, pos), size = u32(d, pos + 4);
        if (pos + 16 + uint64_t(size) > d.size()) throw FormatError("mul: truncated packet");
        const auto payload = d.subspan(pos + 16, size);
        if (type == 0) { // sound: {u32 dataSize, 12 bytes}, then per-channel runs of IMA blocks
            const uint32_t dataSize = u32(payload, 0);
            if (16 + uint64_t(dataSize) > size || dataSize % (ch * kImaBlockBytes))
                throw FormatError("mul: bad sound packet");
            if (u32(d, pos + 8) != frames) throw FormatError("mul: sound packet out of sequence");
            const size_t per = dataSize / ch, blocks = per / kImaBlockBytes;
            p.samples.resize((frames + blocks * kImaBlockSamples) * ch);
            for (uint32_t c = 0; c < ch; ++c)
                for (size_t b = 0; b < blocks; ++b)
                    decodeImaBlock(payload.data() + 16 + c * per + b * kImaBlockBytes, ImaVariant::CrystalDynamics,
                                   p.samples.data() + (frames + b * kImaBlockSamples) * ch + c, ch);
            frames += blocks * kImaBlockSamples;
        } else if (type == 1) { // cinematic: first one starts {"CINE", u32 version, u32 hdrSize, ...}
            ++m.cinematicPackets;
            size_t q = 0;
            if (firstCine) {
                firstCine = false;
                if (u32(payload, 0) != 0x43494E45u) // bytes "ENIC" (CINE reversed)
                    throw FormatError("mul: bad cinematic header");
                const uint32_t hdr = u32(payload, 8);
                if (8 + uint64_t(hdr) > size) throw FormatError("mul: bad cinematic header size");
                m.sceneFrames = hdr != 0x60 || std::any_of(payload.begin() + 12, payload.begin() + 8 + hdr,
                                                           [](uint8_t b) { return b != 0; });
                q = 8 + hdr;
            }
            if (!m.sceneFrames) parseCineFrames(payload.subspan(q), m);
        } // other types: padding (2 and 3 observed)
        pos += 16 + size;
    }
    // 20 retail records end 256 frames short of numSamples; play what the packets hold.
    p.samples.resize(std::min<size_t>(frames, m.header.numSamples) * ch);
    return m;
}

} // namespace ou::audio
