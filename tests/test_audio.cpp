#include "audio/Mixer.hpp"
#include "audio/MusicPlayer.hpp"
#include "audio/SoundPlex.hpp"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using namespace ou::audio;

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void putf(std::vector<uint8_t>& v, float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    put32(v, x);
}

// IMA block: header sample `s`, step index `idx`, first data byte `b0`, other data bytes 0.
std::vector<uint8_t> imaBlock(int16_t s, uint8_t idx, uint8_t b0) {
    std::vector<uint8_t> b(kImaBlockBytes, 0);
    b[0] = uint8_t(s);
    b[1] = uint8_t(uint16_t(s) >> 8);
    b[2] = idx;
    b[4] = b0;
    return b;
}

std::vector<int16_t> decode(const std::vector<uint8_t>& block, ImaVariant v) {
    std::vector<int16_t> out(kImaBlockSamples);
    decodeImaBlock(block.data(), v, out.data());
    return out;
}

// FSB4 sample header (0x50 bytes).
void fsbSample(std::vector<uint8_t>& v, const char* name, uint32_t samples, uint32_t bytes, uint32_t mode) {
    put16(v, 0x50);
    char n[30] = {};
    std::strncpy(n, name, sizeof n - 1);
    v.insert(v.end(), n, n + 30);
    put32(v, samples); put32(v, bytes); put32(v, 0); put32(v, samples - 1); put32(v, mode);
    put32(v, 44100); put16(v, 255); put16(v, 128); put16(v, 128); put16(v, 1);
    putf(v, 1); putf(v, 10000); put32(v, 0); put16(v, 0); put16(v, 0);
}

std::vector<uint8_t> fsbHeader(uint32_t n, uint32_t shdr, uint32_t data) {
    std::vector<uint8_t> v{'F', 'S', 'B', '4'};
    put32(v, n); put32(v, shdr); put32(v, data); put32(v, 0x40000); put32(v, 0x20);
    v.resize(0x30, 0);
    return v;
}

// Bank: sample 0 = IMA mono (1 block), sample 1 = PCM16 mono {100, -100, 3000, -32768}.
std::vector<uint8_t> makeBank() {
    auto v = fsbHeader(2, 0xA0, 36 + 8);
    fsbSample(v, "ima.wav", 64, 36, kFsbModeImaAdpcm | kFsbModeMono);
    fsbSample(v, "pcm.wav", 4, 8, kFsbModePcm16 | kFsbModeMono);
    const auto b = imaBlock(0, 0, 0x07);
    v.insert(v.end(), b.begin(), b.end());
    for (int16_t s : {100, -100, 3000, -32768}) put16(v, uint16_t(s));
    return v;
}

// Two-channel .mul: a scene-less cinematic packet with one subtitle, then two sound packets
// (2 blocks per channel = 128 frames each), numSamples 100.
std::vector<uint8_t> makeMul() {
    std::vector<uint8_t> v;
    put32(v, 22050); put32(v, uint32_t(-1)); put32(v, 100); put32(v, 2);
    v.resize(0x24, 0);
    put32(v, 1); // hasSubtitles
    v.resize(0x38, 0);
    putf(v, 3);
    putf(v, 1); putf(v, 0); // volLeft
    v.resize(0x6C, 0);
    putf(v, 0); putf(v, 1); // volRight
    v.resize(kMulHeaderSize, 0);

    std::vector<uint8_t> cine{'E', 'N', 'I', 'C'};
    put32(cine, 9); put32(cine, 0x60);
    cine.resize(0x68, 0);
    const std::string text = "0\rHello.\r2\rHallo.\r";
    put32(cine, uint32_t(4 + 4 + text.size() + 2)); put32(cine, 0); put32(cine, uint32_t(text.size()));
    cine.insert(cine.end(), text.begin(), text.end());
    cine.push_back(0); cine.push_back(0);
    put32(v, 1); put32(v, uint32_t(cine.size())); put32(v, 0); put32(v, 0);
    v.insert(v.end(), cine.begin(), cine.end());

    for (uint32_t pkt = 0; pkt < 2; ++pkt) {
        put32(v, 0); put32(v, 16 + 4 * 36); put32(v, pkt * 128); put32(v, 0);
        put32(v, 4 * 36); put32(v, 0); put32(v, 0); put32(v, 0);
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 2; ++b) {
                const auto blk = imaBlock(int16_t(c ? -1000 : 1000), 0, 0);
                v.insert(v.end(), blk.begin(), blk.end());
            }
    }
    put32(v, 2); put32(v, 16); put32(v, 0); put32(v, 0); // padding packet
    v.resize(v.size() + 16, 0);
    return v;
}

std::shared_ptr<const Pcm> constant(uint16_t ch, std::vector<int16_t> frame, size_t frames, uint32_t rate = 48000) {
    auto p = std::make_shared<Pcm>();
    p->sampleRate = rate;
    p->channels = ch;
    for (size_t i = 0; i < frames; ++i) p->samples.insert(p->samples.end(), frame.begin(), frame.end());
    return p;
}

std::vector<float> run(Mixer& m, size_t frames) {
    std::vector<float> out(frames * 2);
    m.mix(out.data(), frames);
    return out;
}

} // namespace

// ---- IMA ADPCM ---------------------------------------------------------------------------
TEST(Ima, XboxKnownVector) {
    // nibbles 7, 15, 0, 0 from step 7: +11 -> idx 8; -30 -> idx 16; -(34>>3) -> idx 15; -(31>>3)
    const auto o = decode(imaBlock(0, 0, 0xF7), ImaVariant::Xbox);
    EXPECT_EQ(o[0], 0);
    EXPECT_EQ(o[1], 11);
    EXPECT_EQ(o[2], -19);
    EXPECT_EQ(o[3], -15);
    EXPECT_EQ(o[4], -12);
}

TEST(Ima, CrystalDynamicsKnownVector) {
    // Nibble 0 (7) is skipped. Nibble 15 at step 7: (28 * -0x7800) >> 16 = -14 -> idx 8 (step 16);
    // then nibble 0: (64*0x800)>>16 = 2, idx 7 (14): 1, idx 6 (13): 1.
    const auto o = decode(imaBlock(0, 0, 0xF7), ImaVariant::CrystalDynamics);
    EXPECT_EQ(o[0], 0);
    EXPECT_EQ(o[1], -14);
    EXPECT_EQ(o[2], -12);
    EXPECT_EQ(o[3], -11);
    EXPECT_EQ(o[4], -10);
}

TEST(Ima, StepClampAndSaturation) {
    // tru.exe clamps steps to 0x1FFF before x4; plain IMA saturates at 32767.
    EXPECT_EQ(decode(imaBlock(0, 88, 0x70), ImaVariant::CrystalDynamics)[1], 15358);
    EXPECT_EQ(decode(imaBlock(0, 88, 0x07), ImaVariant::Xbox)[1], 32767);
    EXPECT_EQ(decode(imaBlock(32000, 88, 0x07), ImaVariant::Xbox)[1], 32767);
    // header index > 88 is clamped instead of reading past the table
    EXPECT_EQ(decode(imaBlock(0, 200, 0x07), ImaVariant::Xbox)[1], 32767);
}

TEST(Ima, StrideInterleaves) {
    std::vector<int16_t> out(kImaBlockSamples * 2, 7);
    decodeImaBlock(imaBlock(5, 0, 0).data(), ImaVariant::Xbox, out.data() + 1, 2);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[1], 5);
    EXPECT_EQ(out[2], 7);
}

// ---- FSB4 / Wave ---------------------------------------------------------------------------
TEST(Fsb4, ParsesAndDecodesSyntheticBank) {
    const auto v = makeBank();
    const auto f = parseFsb4(v);
    EXPECT_EQ(f.version, 0x40000u);
    ASSERT_EQ(f.samples.size(), 2u);
    EXPECT_EQ(f.samples[0].name, "ima.wav");
    EXPECT_EQ(f.samples[0].dataOffset, 0x30u + 0xA0u);
    EXPECT_EQ(f.samples[1].dataOffset, 0x30u + 0xA0u + 36u);
    EXPECT_FLOAT_EQ(f.samples[1].maxDistance, 10000);
    const auto a = decodeFsb4(v, f.samples[0]);
    EXPECT_EQ(a.frames(), 64u);
    EXPECT_EQ(a.samples[1], 11);
    EXPECT_EQ(a.sampleRate, 44100u);
    const auto b = decodeFsb4(v, f.samples[1]);
    EXPECT_EQ(b.samples, (std::vector<int16_t>{100, -100, 3000, -32768}));
}

TEST(Fsb4, BasicHeadersShareFirstSampleFormat) {
    auto v = fsbHeader(2, 0x50 + 8, 72);
    v[20] = 0x22; // mode: basic headers
    fsbSample(v, "a", 64, 36, kFsbModeImaAdpcm | kFsbModeMono);
    put32(v, 64); put32(v, 36);
    for (int i = 0; i < 2; ++i) {
        const auto b = imaBlock(int16_t(i), 0, 0);
        v.insert(v.end(), b.begin(), b.end());
    }
    const auto f = parseFsb4(v);
    ASSERT_EQ(f.samples.size(), 2u);
    EXPECT_EQ(f.samples[1].mode, f.samples[0].mode);
    EXPECT_EQ(decodeFsb4(v, f.samples[1]).samples[0], 1);
}

TEST(Fsb4, TruncationThrows) {
    const auto v = makeBank();
    for (size_t n : {size_t(0), size_t(3), size_t(0x20), size_t(0x30 + 0x40), v.size() - 1})
        EXPECT_THROW(parseFsb4(std::span(v.data(), n)), FormatError) << n;
    auto bad = v;
    bad[0x30 + 0x25] = 0xFF; // sample 0 dataSize now runs past the data block
    EXPECT_THROW(parseFsb4(bad), FormatError);
    auto badCount = v;
    badCount[4] = 200; // more samples than header bytes
    EXPECT_THROW(parseFsb4(badCount), FormatError);
}

TEST(Wave, UsesSectionSampleRate) {
    auto bank = fsbHeader(1, 0x50, 36);
    fsbSample(bank, "w", 64, 36, kFsbModeImaAdpcm | kFsbModeMono);
    const auto b = imaBlock(0, 0, 0x07);
    bank.insert(bank.end(), b.begin(), b.end());
    std::vector<uint8_t> sec;
    put32(sec, 22050); put32(sec, 16); put32(sec, 48); put32(sec, 100);
    sec.insert(sec.end(), bank.begin(), bank.end());
    const auto w = parseWave(sec);
    EXPECT_EQ(w.unknown, 100u);
    const auto p = decodeWave(sec);
    EXPECT_EQ(p.sampleRate, 22050u);
    EXPECT_EQ(p.loopStart, 16);
    EXPECT_EQ(p.loopEnd, 48);
    EXPECT_EQ(p.frames(), 64u);
    EXPECT_THROW(decodeWave(std::span(sec.data(), sec.size() - 1)), FormatError);
    EXPECT_THROW(decodeWave(std::span(sec.data(), 8)), FormatError);
}

// ---- .mul ------------------------------------------------------------------------------------
TEST(Mul, DecodesChannelsAndSubtitles) {
    const auto m = decodeMul(makeMul());
    EXPECT_EQ(m.header.sampleRate, 22050u);
    EXPECT_EQ(m.header.channels, 2u);
    EXPECT_EQ(m.header.loopStart, -1);
    EXPECT_FLOAT_EQ(m.header.mediaLength, 3);
    EXPECT_EQ(m.pcm.frames(), 100u); // 128 decoded, trimmed to numSamples
    EXPECT_EQ(m.pcm.samples[0], 1000);
    EXPECT_EQ(m.pcm.samples[1], -1000);
    EXPECT_EQ(m.pcm.samples[64 * 2], 1000); // second block's header sample, channel 0
    EXPECT_EQ(m.pcm.volLeft, (std::vector<float>{1, 0}));
    EXPECT_EQ(m.pcm.volRight, (std::vector<float>{0, 1}));
    EXPECT_FALSE(m.sceneFrames);
    EXPECT_EQ(m.cinematicPackets, 1u);
    ASSERT_EQ(m.subtitles.size(), 1u);
    EXPECT_EQ(m.subtitles[0].text(0), "Hello.");
    EXPECT_EQ(m.subtitles[0].text(2), "Hallo.");
    EXPECT_EQ(m.subtitles[0].text(1), "");
}

TEST(Mul, TruncationAndCorruptionThrow) {
    const auto v = makeMul();
    EXPECT_THROW(decodeMul(std::span(v.data(), 0x7FF)), FormatError);
    EXPECT_THROW(decodeMul(std::span(v.data(), v.size() - 40)), FormatError); // cut inside last sound packet
    auto seq = v;
    // second sound packet claims the wrong start sample
    const size_t second = kMulHeaderSize + 16 + 0x68 + 12 + 18 + 2 + (16 + 16 + 4 * 36) + 8;
    seq[second] = 1;
    EXPECT_THROW(decodeMul(seq), FormatError);
    auto ch = v;
    ch[12] = 0; // zero channels
    EXPECT_THROW(decodeMul(ch), FormatError);
}

// ---- Mus! ------------------------------------------------------------------------------------
namespace {
void putName(std::vector<uint8_t>& v, const char* s) {
    char n[0x14] = {};
    std::strncpy(n, s, sizeof n);
    v.insert(v.end(), n, n + sizeof n);
}

// A Mus! file around one track: sequence tempo 600000 us (100 BPM, as retail) but header BPM 120 (the stream
// clock), 480 ppqn, 1 tone/program/sample, one stereo stem (500 ms/beat), 2 labels.
std::vector<uint8_t> wrapMus(const std::vector<uint8_t>& t, uint32_t l0, uint32_t l1) {
    std::vector<uint8_t> v{'M', 'u', 's', '!'};
    const uint32_t stems = 0x12C, labels = stems + 0x58, seq = labels + 8;
    for (uint32_t x : {seq - 8, kMusVersion, 0u, 0u, 0u, 1u, 1u, 120u, stems, 2u, labels, 1u, 1u, 1u}) put32(v, x);
    put32(v, 0); put32(v, seq);                   // sequence table {id, offset}
    put32(v, 0x80);                               // tone offsets
    put32(v, 0x100);                              // program offsets
    putName(v, "Sine_440"); put32(v, 0x40); put32(v, 0); put32(v, 0xC0); v.resize(0x80, 0); // sample
    putName(v, "Atk 11"); put32(v, 1);            // tone: 1 layer of 0x68 bytes
    v.resize(0x80 + 0x18 + 0x40, 0);
    v.insert(v.end(), {0, 0x7F, 0, 0x7F}); put32(v, 0); v.resize(0x100, 0);
    putName(v, "Rel 1"); put32(v, 2); put32(v, 5); put32(v, 1); // program: bank 2, program 5
    put32(v, 0xFFFFFFFF); v.insert(v.end(), {10, 20, 0, 0x7F}); put32(v, 0);
    for (uint32_t x : {44100u, 0u, 2u, 0u, 1000u, 7u, 500u}) put32(v, x); // stem
    for (float x : {1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.5f, 0.5f}) putf(v, x);
    v.resize(labels, 0);
    put32(v, l0); put32(v, l1);
    v.insert(v.end(), {'Q', 'S', 'M', 'a'});
    put32(v, 600000); put16(v, 480); put16(v, 0x106); put16(v, 1); put16(v, 0); put32(v, 0x14);
    v.insert(v.end(), t.begin(), t.end());
    return v;
}

// Script: volume 64, fade 0 now; L0: if var[0x7D] > 0 goto L1; fade to 127 over 2 beats;
// goto L0. L1: var[0x0E] = 5; note on + running-status note; 480 ticks later end of track.
std::vector<uint8_t> makeMus(uint8_t gotoLabel = 0) {
    std::vector<uint8_t> t; // track
    auto ev = [&](std::initializer_list<uint8_t> b) { t.insert(t.end(), b); return uint32_t(t.size()); };
    ev({0x00, 0xFF, 0xF0, 3, 'a', 'b', 'c'});
    ev({0x00, 0xFF, 0x60, 2, 0, 64});
    ev({0x00, 0xFF, 0x62, 3, 0, 0, 0});
    const uint32_t l0 = ev({0x00, 0xFF, 0x31, 1, 0});
    ev({0x00, 0xFF, 0x3C, 3, 0x7D, 0, 1});
    ev({0x00, 0xFF, 0x62, 3, 0, 2, 0x7F});
    const uint32_t l1 = ev({0x00, 0xFF, 0x32, 1, gotoLabel});
    ev({0x00, 0xFF, 0x33, 2, 5, 0x0E});
    ev({0x00, 0x90, 0x3C, 0x40});
    ev({0x00, 0x3C, 0x00});
    ev({0x83, 0x60, 0xFF, 0x44, 0});
    return wrapMus(t, l0, l1);
}
} // namespace

TEST(Mus, ParsesSyntheticScript) {
    const auto f = parseMus(makeMus());
    EXPECT_EQ(f.version, kMusVersion);
    EXPECT_EQ(f.bpm, 120u);
    ASSERT_EQ(f.samples.size(), 1u);
    EXPECT_EQ(f.samples[0].name, "Sine_440");
    EXPECT_EQ(f.samples[0].samOffset, 0x40u);
    EXPECT_EQ(f.samples[0].size, 0xC0u);
    ASSERT_EQ(f.tones.size(), 1u);
    EXPECT_EQ(f.tones[0].name, "Atk 11");
    ASSERT_EQ(f.tones[0].layers.size(), 1u);
    EXPECT_EQ(f.tones[0].layers[0].keyHi, 0x7F);
    ASSERT_EQ(f.programs.size(), 1u);
    EXPECT_EQ(f.programs[0].bank, 2u);
    EXPECT_EQ(f.programs[0].program, 5u);
    ASSERT_EQ(f.programs[0].zones.size(), 1u);
    EXPECT_EQ(f.programs[0].zones[0].keyLo, 10);
    EXPECT_EQ(f.programs[0].zones[0].keyHi, 20);
    ASSERT_EQ(f.stems.size(), 1u);
    EXPECT_EQ(f.stems[0].channels, 2u);
    EXPECT_EQ(f.stems[0].frames, 1000u);
    EXPECT_EQ(f.stems[0].msPerBeat, 500u);
    EXPECT_FLOAT_EQ(f.stems[0].pan, 0.5f);
    ASSERT_EQ(f.sequences.size(), 1u);
    const auto& s = f.sequences[0];
    EXPECT_EQ(s.tempo, 600000u);
    EXPECT_EQ(s.ppqn, 480u);
    ASSERT_EQ(s.tracks.size(), 1u);
    const auto& e = s.tracks[0];
    ASSERT_EQ(e.size(), 11u);
    EXPECT_EQ(e[0].meta, 0xF0);
    EXPECT_EQ(std::string(e[0].data.begin(), e[0].data.end()), "abc");
    EXPECT_EQ(e[3].offset, f.labels[0] - 5);
    EXPECT_EQ(e[4].offset, f.labels[0]);
    EXPECT_EQ(e[8].status, 0x90);
    EXPECT_EQ(e[9].status, 0x90); // running status
    EXPECT_EQ(e[9].data, (std::vector<uint8_t>{0x3C, 0}));
    EXPECT_EQ(e[10].delta, 480u);
    EXPECT_EQ(e[10].meta, 0x44);
}

TEST(Mus, TruncationAndBadDataThrow) {
    const auto v = makeMus();
    for (size_t n = 0; n < v.size(); ++n) EXPECT_THROW(parseMus(std::span(v.data(), n)), FormatError) << n;
    auto ver = v;
    ver[8] = 0x13;
    EXPECT_THROW(parseMus(ver), FormatError);
    EXPECT_THROW(parseMus(makeMus(2)), FormatError); // goto past the label table
}

TEST(Mus, SequencerDrivesStemGainsFromVariables) {
    const auto f = parseMus(makeMus());
    MusSequencer q(f);
    std::array<uint8_t, 256> vars{};
    const float vol = 64 / 127.0f;
    q.update(0, vars); // volume 64, fade 0, start fade to 1 over 2 * 500 ms, goto L0 (yield)
    EXPECT_FLOAT_EQ(q.stemGains()[0], 0);
    q.update(0.25f, vars); // ramp to 0.25; L0 re-issues the fade from there
    EXPECT_NEAR(q.stemGains()[0], vol * 0.25f, 1e-6);
    vars[0x7D] = 1;
    q.update(0, vars); // branch to L1 (yield)
    q.update(0, vars); // var[0x0E] = 5; end of track waits for tick 480 (0.5 s)
    EXPECT_EQ(vars[0x0E], 5);
    EXPECT_FALSE(q.finished());
    q.update(0.25f, vars);
    EXPECT_TRUE(q.finished());
    EXPECT_NEAR(q.stemGains()[0], vol * (0.25f + 0.75f * 0.25f), 1e-6);
}

TEST(Mus, ClockIsTheStreamBeatAtTheHeaderBpm) {
    // Tick 480 = beat 1 at the header's 120 BPM = 0.5 s of stream (0x595250); the sequence tempo (100 BPM)
    // would put it at 0.6 s.
    const auto f = parseMus(makeMus());
    MusSequencer q(f);
    std::array<uint8_t, 256> vars{};
    vars[0x7D] = 1;
    q.update(0, vars); // L0: branch to L1 (yield)
    EXPECT_EQ(q.section(), 1);
    q.update(0, vars); // var[0x0E] = 5, notes; end of track due at tick 480
    q.update(0.499f, vars);
    EXPECT_EQ(q.ticks(), 478u); // 499 ms -> 255/256 beat in 8.8 fixed point
    EXPECT_FALSE(q.finished());
    q.update(0.001f, vars);
    EXPECT_DOUBLE_EQ(q.beats(), 1.0);
    EXPECT_TRUE(q.finished());
}

TEST(Mus, FadeLengthIsBeatsOfTheStem) {
    // volume 1, fade 0 now, fade to 1 over 4 beats (2 s at 500 ms/beat); at beat 4: fade to 0 over 64/32 beats.
    std::vector<uint8_t> t{0x00, 0xFF, 0x60, 2, 0, 0x7F, 0x00, 0xFF, 0x62, 3, 0, 0, 0, 0x00, 0xFF, 0x62, 3, 0, 4, 0x7F};
    const auto l = uint32_t(t.size());
    t.insert(t.end(), {0x8F, 0x00, 0xFF, 0x66, 3, 0, 64, 0, 0x00, 0xFF, 0x44, 0}); // delta 1920 ticks
    const auto f = parseMus(wrapMus(t, l, l));
    MusSequencer q(f);
    std::array<uint8_t, 256> vars{};
    q.update(0, vars);
    EXPECT_FLOAT_EQ(q.stemGains()[0], 0);
    q.update(1, vars);
    EXPECT_FLOAT_EQ(q.stemGains()[0], 0.5f);
    EXPECT_FALSE(q.finished());
    q.update(1, vars); // beat 4: the fade lands on the bar line, the timed fade-out starts there
    EXPECT_FLOAT_EQ(q.stemGains()[0], 1);
    EXPECT_EQ(q.ticks(), 1920u);
    EXPECT_TRUE(q.finished());
    q.update(0.5f, vars);
    EXPECT_FLOAT_EQ(q.stemGains()[0], 0.5f);
    q.update(0.5f, vars);
    EXPECT_FLOAT_EQ(q.stemGains()[0], 0);
}

TEST(Mus, LoopsSubstitutionAndMuteRamp) {
    std::vector<uint8_t> t;
    auto ev = [&](std::initializer_list<uint8_t> b) { const auto o = uint32_t(t.size()); t.insert(t.end(), b); return o; };
    ev({0x00, 0xFF, 0x41, 0});                // operand 0 of the next meta = var[operand 0]
    ev({0x00, 0xFF, 0x33, 2, 0x12, 0x20});    // var[0x20] = var[0x12]
    ev({0x00, 0xFF, 0x68, 1, 0});             // stem 0 mute ramps down 0.2 per update
    ev({0x00, 0xFF, 0x2E, 1, 3});             // loop 3 passes
    ev({0x00, 0xFF, 0x35, 2, 1, 0x10});       //   var[0x10] += 1
    ev({0x00, 0xFF, 0x2F, 0});
    ev({0x00, 0xFF, 0x2E, 1, 0});             // loop forever
    ev({0x00, 0xFF, 0x35, 2, 1, 0x11});       //   var[0x11] += 1
    const uint32_t brk = ev({0x00, 0xFF, 0x30, 3, 0, 0, 0}); // break to the end
    ev({0x00, 0xFF, 0x2F, 0});
    const uint32_t end = ev({0x00, 0xFF, 0x44, 0});
    t[brk + 4] = uint8_t(end);
    const auto f = parseMus(wrapMus(t, end, end));
    MusSequencer q(f);
    std::array<uint8_t, 256> vars{};
    vars[0x12] = 42;
    for (int i = 0; i < 3; ++i) q.update(0, vars); // each loop jump ends the frame
    EXPECT_EQ(vars[0x20], 42);
    EXPECT_EQ(vars[0x10], 3);
    EXPECT_EQ(vars[0x11], 1); // the forever loop broke out on its first pass
    EXPECT_FALSE(q.finished());
    q.update(0, vars);
    EXPECT_TRUE(q.finished());
    EXPECT_NEAR(q.stemGains()[0], 0.4f, 1e-6); // ramp started in update 1: three steps by update 4
}

// ---- Mixer -----------------------------------------------------------------------------------
TEST(Mixer, MonoCentreIsEqualPower) {
    Mixer m;
    m.play(constant(1, {16384}, 100));
    const auto o = run(m, 10);
    EXPECT_NEAR(o[0], 0.5f * std::sqrt(0.5f), 1e-4);
    EXPECT_NEAR(o[1], 0.5f * std::sqrt(0.5f), 1e-4);
}

TEST(Mixer, PanVolumeAndBus) {
    Mixer m;
    m.play(constant(1, {16384}, 100), {.volume = 0.5f, .pan = 1});
    m.setBusVolume(Bus::Sfx, 0.5f);
    const auto o = run(m, 4);
    EXPECT_NEAR(o[0], 0, 1e-6);
    EXPECT_NEAR(o[1], 0.125f, 1e-4);
}

TEST(Mixer, PositionalAttenuationAndPan) {
    Mixer m;
    m.setListener({});
    const auto id = m.play(constant(1, {16384}, 1000), {.position = glm::vec3(10, 0, 0)});
    auto o = run(m, 2);
    EXPECT_NEAR(o[0], 0, 1e-6);       // fully right of a +Y-facing, Z-up listener
    EXPECT_NEAR(o[1], 0.05f, 1e-4);   // 0.5 * min/d = 0.5 * 1/10
    m.setPosition(id, glm::vec3(-0.5f, 0, 0)); // inside minDistance: no attenuation, left
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0.5f, 1e-4);
    EXPECT_NEAR(o[1], 0, 1e-6);
    m.setAttenuation([](float, float, float) { return 0.f; });
    o = run(m, 2);
    EXPECT_EQ(o[0], 0);
}

TEST(Mixer, OneShotEndsLoopContinuesAndResamples) {
    Mixer m;
    const auto once = m.play(constant(1, {16384}, 10));
    const auto loop = m.play(constant(2, {8192, 0}, 10, 24000), {.loop = true});
    EXPECT_TRUE(m.playing(once));
    const auto o = run(m, 30);
    EXPECT_FALSE(m.playing(once));
    EXPECT_TRUE(m.playing(loop));
    EXPECT_NEAR(o[2 * 25], 0.25f, 1e-4);  // only the looping stereo voice, left channel
    EXPECT_NEAR(o[2 * 25 + 1], 0, 1e-6);
    m.stop(loop);
    EXPECT_EQ(m.activeVoices(), 0u);
    EXPECT_EQ(m.play(12345u), 0u); // unknown sound id
}

TEST(Mixer, LoopPointsWrap) {
    auto p = std::make_shared<Pcm>(*constant(1, {0}, 8));
    for (int i = 0; i < 8; ++i) p->samples[size_t(i)] = int16_t(i * 1000);
    p->loopStart = 2;
    p->loopEnd = 4; // plays 0 1 2 3 2 3 ...
    Mixer m;
    m.play(p, {.pan = 1, .loop = true});
    const auto o = run(m, 8);
    const float k = 1000.f / 32768.f;
    for (int i = 0; i < 8; ++i) EXPECT_NEAR(o[2 * size_t(i) + 1], (i < 4 ? i : 2 + i % 2) * k, 1e-4) << i;
}

TEST(Mixer, SoundRegistry) {
    Mixer m;
    m.addSound(7, constant(1, {16384}, 10));
    EXPECT_TRUE(m.hasSound(7));
    EXPECT_NE(m.play(7u), 0u);
    EXPECT_EQ(m.activeVoices(), 1u);
}

TEST(Mixer, WorldPauseHoldsAndStopWorldEndsWorldVoicesOnly) {
    Mixer m;
    const auto sfx = m.play(constant(1, {16384}, 100), {.pan = 1, .loop = true});
    const auto menu = m.play(constant(1, {8192}, 100), {.pan = -1, .loop = true, .world = false});
    const auto vo = m.play(constant(1, {16384}, 100), {.pan = 1, .loop = true, .bus = Bus::Voice});
    m.setWorldPaused(true);
    auto o = run(m, 4);
    EXPECT_NEAR(o[0], 0.25f, 1e-4); // the menu voice, left
    EXPECT_NEAR(o[1], 0.5f, 1e-4);  // the voice line plays on; the held sfx adds nothing
    m.setWorldPaused(false);
    o = run(m, 4);
    EXPECT_NEAR(o[1], 1.0f, 1e-4);
    m.stopWorld();
    EXPECT_FALSE(m.playing(sfx));
    EXPECT_FALSE(m.playing(vo));
    EXPECT_TRUE(m.playing(menu));
}

TEST(Mixer, MusicVoicesMasterRampAndStems) {
    Mixer m(1000);
    const auto a = m.playMusic(constant(2, {16384, 0}, 100000, 1000));
    const auto b = m.playMusic(constant(2, {0, 16384}, 100000, 1000), 0); // a second voice, silent
    m.setMusic(b, 1, {}, 0.1f); // ramps over 100 output frames
    m.setMusic(a, 0, {}, 0.1f);
    auto o = run(m, 50);
    EXPECT_NEAR(o[2 * 49], 0.25f, 0.01f); // half way
    EXPECT_NEAR(o[2 * 49 + 1], 0.25f, 0.01f);
    run(m, 100);
    EXPECT_EQ(m.activeVoices(), 2u); // a silent music voice stays (its slot decides)
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0, 1e-6);
    EXPECT_NEAR(o[1], 0.5f, 1e-4);
    m.stop(a), m.stop(b);

    auto four = std::make_shared<Pcm>(*constant(4, {16384, 16384, 8192, 8192}, 1000, 1000));
    const auto v = m.playMusic(four, 1, {0, 1}); // stem 0 muted, stem 1 (channels 2,3) on
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0.25f, 1e-4);
    m.setMusic(v, 1, {1, 0});
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0.5f, 1e-4);
    m.stop(v);
    EXPECT_EQ(m.activeVoices(), 0u);
}

// A music voice's position is its playback clock: every loop pass counted (tru.exe's music clock reads the stream
// counter, which runs on through the .mul loop — Wine, AUDIO.md §5.3).
TEST(Mixer, PositionCountsLoopPasses) {
    Mixer m(1000);
    auto p = std::make_shared<Pcm>(*constant(2, {100, 100}, 100, 1000));
    p->loopStart = 0;
    const auto v = m.playMusic(p);
    run(m, 250);
    ASSERT_TRUE(m.position(v));
    EXPECT_NEAR(*m.position(v), 0.25, 1e-9);
}

TEST(Mixer, PlaysThroughSdlDummyDevice) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"); // no sound card needed; still runs the SDL callback path
    Mixer m;
    ASSERT_TRUE(m.openDevice()) << SDL_GetError();
    const auto v = m.play(constant(1, {16384}, 4800)); // 0.1 s
    const auto t0 = std::chrono::steady_clock::now();
    while (m.playing(v) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(m.playing(v)); // consumed by the device callback
    m.closeDevice();
}

// SFX reverb (AUDIO.md §9.15): the tail of an impulse loses ~60 dB per decay time; the globalsoundinfo default is silent.
TEST(Dsp, SfxReverbTailDecaysAtDecayTime) {
    const uint32_t rate = 48000;
    SfxReverb r(rate);
    std::vector<float> in(2 * rate * 2, 0.f), out(in.size(), 0.f);
    in[0] = in[1] = 1;
    r.process(in.data(), out.data(), rate * 2);
    EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](float x) { return x == 0; })); // kReverbOff
    SfxReverbProps p = kReverbOff;
    p[1] = 0, p[2] = 0, p[4] = 1.0f, p[5] = 1, p[8] = 0, p[7] = 0.01f, p[9] = 0.01f; // room 0 mB, 1 s decay
    r.set(p);
    std::fill(out.begin(), out.end(), 0.f);
    r.process(in.data(), out.data(), rate * 2);
    const auto energy = [&](double a, double b) {
        double e = 0;
        for (size_t i = size_t(a * rate) * 2; i < size_t(b * rate) * 2; ++i) e += double(out[i]) * out[i];
        return e;
    };
    const double early = energy(0.1, 0.2), late = energy(0.6, 0.7);
    ASSERT_GT(early, 0);
    const double db = 10 * std::log10(late / early); // 0.5 s later: about -30 dB
    EXPECT_LT(db, -22);
    EXPECT_GT(db, -38);
}

// Oscillator (0x587C60): a tone of `rate` Hz for `delay` s; an Effect node (0x58DEF0) puts its DSP on the child's voice:
// a 500 Hz lowpass takes a 6 kHz sine down by > 20 dB.
TEST(SoundPlex, OscillatorToneThroughEffectLowpass) {
    const Pcm tone = oscillator(0, 1000, 0.5f, 48000);
    ASSERT_EQ(tone.frames(), 24000u);
    size_t rising = 0; // one rising zero crossing per 48-sample period
    for (size_t i = 1; i < tone.frames(); ++i) rising += tone.samples[i - 1] < 0 && tone.samples[i] >= 0;
    EXPECT_NEAR(double(rising), 500, 1);
    EXPECT_EQ(*std::max_element(tone.samples.begin(), tone.samples.end()), 32767);

    Plex osc;
    osc.type = PlexType::Oscillator, osc.rate = 6000, osc.delay = 0.2f, osc.mode = 0;
    Plex fx;
    fx.type = PlexType::Effect, fx.children = {osc}, fx.effect = {DspType::LowPass, {500, 1}};
    const auto rms = [](const std::vector<float>& o) {
        double e = 0;
        for (size_t i = 4800; i < o.size(); ++i) e += double(o[i]) * o[i];
        return std::sqrt(e / double(o.size() - 4800));
    };
    Mixer m;
    SoundPlayer sp(m, nullptr, nullptr);
    ASSERT_NE(sp.start(osc), 0u);
    const double dry = rms(run(m, 4800));
    run(m, 9600);
    EXPECT_EQ(m.activeVoices(), 0u); // 0.2 s tone ended
    ASSERT_NE(sp.start(fx), 0u);
    const double wet = rms(run(m, 4800));
    EXPECT_GT(dry, 0.1);
    EXPECT_LT(20 * std::log10(wet / dry), -20);
}

// ---- Real data (skipped without OU_GAME_DIR) ---------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
double rms(const Pcm& p) {
    double a = 0;
    for (int16_t s : p.samples) a += double(s) * s;
    return std::sqrt(a / double(p.samples.size())) / 32768;
}
} // namespace

TEST(AudioReal, LaraVoiceLine) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto d = bf.read("pc-w\\audio\\streams\\vo\\lara\\lara_badwater_01.mul");
    ASSERT_TRUE(d);
    const auto m = decodeMul(*d);
    EXPECT_EQ(m.pcm.sampleRate, 44020u);
    EXPECT_EQ(m.pcm.channels, 1u);
    EXPECT_EQ(m.pcm.frames(), 67328u);
    EXPECT_GT(rms(m.pcm), 0.01);
}

TEST(AudioReal, SubtitleMatchesLocals) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto d = bf.read("pc-w\\audio\\streams\\vo\\lara\\sc_04_01_answer.mul");
    ASSERT_TRUE(d);
    const auto m = decodeMul(*d);
    ASSERT_EQ(m.subtitles.size(), 1u);
    const auto en = m.subtitles[0].text(0);
    EXPECT_EQ(en, "\"I need to find an ancient Mayan Ball Court.\"");
    const auto locals = bf.read("pc-w\\local\\locals.bin");
    ASSERT_TRUE(locals);
    const std::string_view text(reinterpret_cast<const char*>(locals->data()), locals->size());
    EXPECT_NE(text.find(std::string(en) + '\0'), text.npos);
}

TEST(AudioReal, LevelWavesAndMusic) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    auto drm = bf.read("pc-w\\l4_pool.drm");
    ASSERT_TRUE(drm);
    Mixer m;
    EXPECT_EQ(m.addWaves(ou::cdc::parseDrm(std::move(*drm))), 455u);

    const auto sam = bf.read("pc-w\\music\\l4_pool_mus_01\\l4_pool_mus_01.sam");
    ASSERT_TRUE(sam);
    const auto bank = parseFsb4(*sam);
    ASSERT_EQ(bank.samples.size(), 1u);
    EXPECT_EQ(bank.samples[0].name, "temp.wav"); // placeholder bank, 64 samples
    EXPECT_EQ(bank.samples[0].mode, kFsbModeImaAdpcm | kFsbModeMono);

    const auto mus = bf.read("pc-w\\music\\l4_pool_mus_01\\l4_pool_mus_01.mul");
    ASSERT_TRUE(mus);
    const auto music = decodeMul(*mus);
    EXPECT_EQ(music.pcm.channels, 4u);
    EXPECT_EQ(music.pcm.loopStart, 0);
    EXPECT_EQ(music.pcm.frames(), 5767168u);
    EXPECT_EQ(music.pcm.volLeft, (std::vector<float>{1, 0, 1, 0}));
}

TEST(AudioReal, MusParsesAll75) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    // Bigfile records are hashed, so scan the small ones for the magic (retail .mus are
    // 9996..12176 bytes; patch.000 holds none).
    const ou::cdc::Bigfile big(std::filesystem::path(gameDir()) / "bigfile.000");
    size_t files = 0, events = 0, stems = 0;
    for (const auto& r : big.records()) {
        if (r.size < 4 || r.size > 0x10000) continue;
        const auto d = big.read(r);
        if (std::memcmp(d.data(), "Mus!", 4) != 0) continue;
        ++files;
        SCOPED_TRACE(r.hash);
        const auto f = parseMus(d);
        EXPECT_EQ(f.samples.size(), 4u);
        EXPECT_EQ(f.tones.size(), 52u);
        EXPECT_EQ(f.programs.size(), 50u);
        EXPECT_FALSE(f.stems.empty());
        ASSERT_EQ(f.sequences.size(), 1u);
        ASSERT_EQ(f.sequences[0].tracks.size(), 1u);
        EXPECT_GT(f.sequences[0].tracks[0].size(), 4u);
        events += f.sequences[0].tracks[0].size();
        stems += f.stems.size();
        EXPECT_EQ(f.bpm, 120u); // the stream clock's beat = the stems' 500 ms
        MusSequencer q(f); // every script runs: intensity 0..7 over 20 s, gains stay in range
        std::array<uint8_t, 256> v{};
        for (int i = 0; i < 1200; ++i) {
            v[0x7D] = uint8_t(i / 150);
            q.update(1 / 60.0f, v);
            for (float g : q.stemGains()) ASSERT_TRUE(g >= 0 && g <= 1.0001f) << g;
        }
    }
    EXPECT_EQ(files, 75u);
    EXPECT_EQ(stems, 137u);
    EXPECT_EQ(events, 8462u);
}

TEST(AudioReal, MusPoolStemGainsFollowVariable7D) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto d = bf.read("pc-w\\music\\l4_pool_mus_01\\l4_pool_mus_01.mus");
    ASSERT_TRUE(d);
    const auto f = parseMus(*d);
    ASSERT_EQ(f.stems.size(), 2u); // the .mul's two stereo stems
    EXPECT_EQ(f.stems[0].frames, 5767168u);
    const auto gains = [&](uint8_t level) {
        MusSequencer q(f);
        std::array<uint8_t, 256> v{};
        v[0x7D] = level;
        for (int i = 0; i < 300; ++i) q.update(1 / 30.0f, v);
        return q.stemGains();
    };
    const float base = 0x28 / 127.0f; // meta 0x60 volume; levels select the fade targets
    const auto g0 = gains(0), g7 = gains(7), g8 = gains(8);
    EXPECT_NEAR(g0[0], base, 0.005);
    EXPECT_NEAR(g0[1], base * 0x5F / 127, 0.005);
    EXPECT_NEAR(g7[0], base * 5 / 127, 0.005);
    EXPECT_NEAR(g8[1], base, 0.005);
}

// A change of the intensity variable walks the script one section per update (each branch ends the frame; the walk
// issues no fades). The target section's fade, re-issued every update from the current gain (0x586650), then closes
// 1 - 1/e of the gap per fade length.
TEST(AudioReal, MusPoolTransitionStepsSectionsAndFadesInBeats) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto d = bf.read("pc-w\\music\\l4_pool_mus_01\\l4_pool_mus_01.mus");
    ASSERT_TRUE(d);
    const auto f = parseMus(*d);
    MusSequencer q(f);
    std::array<uint8_t, 256> v{};
    for (int i = 0; i < 600; ++i) q.update(1 / 60.0f, v);
    EXPECT_EQ(q.section(), 0);
    const float base = 0x28 / 127.0f;
    v[0x7D] = 8;
    for (int k = 1; k <= 8; ++k) {
        q.update(1 / 60.0f, v);
        EXPECT_EQ(q.section(), k);
    }
    q.update(1 / 60.0f, v); // the walk only branches; L8's first pass issues its fades (stem 1 -> 0x7F over 3 beats)
    const float gap = 1 - q.stemGains()[1] / base;
    EXPECT_GT(gap, 0.2f);
    for (int i = 0; i < 90; ++i) q.update(1 / 60.0f, v);
    EXPECT_NEAR(1 - q.stemGains()[1] / base, gap * std::pow(1 - 1 / 90.0f, 90.0f), 1e-3);
    EXPECT_EQ(q.section(), 8);
}

// A timed script: l4_laras_new_bike fades its stem at ticks 0, 2400 and 17760 = beats 0, 5 and 37 of the 120 BPM
// stream clock (2.5 s and 18.5 s), each fade 3, 2 and 6 beats long.
TEST(AudioReal, MusBikeTimedFadesLandOnBeats) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto d = bf.read("pc-w\\music\\l4_laras_new_bike\\l4_laras_new_bike.mus");
    ASSERT_TRUE(d);
    const auto f = parseMus(*d);
    MusSequencer q(f);
    std::array<uint8_t, 256> v{};
    const float vol = 0x66 / 127.0f;
    auto runTo = [&, t = 0.0](double until) mutable {
        for (; t + 1e-9 < until; t += 0.01) q.update(0.01f, v);
        return q.stemGains()[0] / vol;
    };
    EXPECT_NEAR(runTo(0.75), 0x20 / 127.0f / 2, 0.01); // halfway through the 3-beat fade-in
    EXPECT_NEAR(runTo(2.49), 0x20 / 127.0f, 1e-5);
    EXPECT_NEAR(runTo(3.0), (0x20 + 0x7F) / 254.0f, 0.01); // beat 5 + 1 of 2
    EXPECT_NEAR(runTo(18.49), 1.0f, 1e-5);
    EXPECT_NEAR(runTo(20.0), (0x7F + 0x64) / 254.0f, 0.01); // beat 37 + 3 of 6
    EXPECT_NEAR(runTo(21.6), 0x64 / 127.0f, 1e-5);
}

// ---- SoundPlex / sfxmarker (AUDIO.md §9) ---------------------------------------------------------
TEST(SfxMarker, PerimeterTriggersFireOnEdges) {
    SfxMarker m;
    m.position = {0, 0, 0};
    m.triggers = {{SfxTrigger::Init, SfxTrigger::SetVariable, 0, false, {}, 0x7D, 3},
                  {SfxTrigger::Enter, SfxTrigger::Start, 100, true, {0}},
                  {SfxTrigger::Exit, SfxTrigger::End, 150, true, {0}},
                  {SfxTrigger::Enter, SfxTrigger::Start, 100, false, {0}}}; // fires once only
    const std::vector<SfxMarker> ms{m};
    SfxMarkerTriggers t(ms);
    auto fired = [&](glm::vec3 p) {
        std::vector<size_t> out;
        for (const auto& f : t.update(ms, p)) out.push_back(f.trigger);
        return out;
    };
    EXPECT_EQ(fired({500, 0, 0}), (std::vector<size_t>{0})); // Init once; outside: nothing else
    EXPECT_EQ(fired({500, 0, 0}), (std::vector<size_t>{}));
    EXPECT_EQ(fired({50, 0, 0}), (std::vector<size_t>{1, 3})); // entered both radii
    EXPECT_EQ(fired({120, 0, 0}), (std::vector<size_t>{}));    // left 100, still inside 150
    EXPECT_EQ(fired({200, 0, 0}), (std::vector<size_t>{2}));   // left the exit radius
    EXPECT_EQ(fired({0, 0, 0}), (std::vector<size_t>{1}));     // re-entry: bAlways only
}

TEST(SfxMarker, StartsInsideFiresEnterOnFirstUpdate) {
    SfxMarker m;
    m.triggers = {{SfxTrigger::Enter, SfxTrigger::Start, 100, false, {0}}, {SfxTrigger::Exit, SfxTrigger::End, 100, false, {0}}};
    const std::vector<SfxMarker> ms{m};
    SfxMarkerTriggers t(ms);
    ASSERT_EQ(t.update(ms, {10, 0, 0}).size(), 1u);
    EXPECT_EQ(t.update(ms, {300, 0, 0}).at(0).trigger, 1u);
}

TEST(SoundPlayer, ChoiceSequenceAndStop) {
    Mixer mix;
    auto tone = [](size_t frames) {
        auto p = std::make_shared<Pcm>();
        p->sampleRate = 1000, p->channels = 1, p->samples.assign(frames, 1000);
        return p;
    };
    mix.addSound(1, tone(500)), mix.addSound(2, tone(500)), mix.addSound(3, tone(1000));
    Plex w1{PlexType::Wave, 1}, w2{PlexType::Wave, 2}, w3{PlexType::Wave, 3};
    Plex choice{PlexType::ChoiceList};
    choice.mode = 3, choice.children = {w1, w2};
    Plex seq{PlexType::Sequence};
    seq.children = {choice, w3};
    std::map<uint32_t, Plex> bank{{900, seq}};
    SoundPlayer sp(mix, [&](uint32_t id) { return bank.contains(id) ? &bank[id] : nullptr; }, {});
    const auto h = sp.start(Plex{PlexType::Reference, 900});
    ASSERT_NE(h, 0u);
    EXPECT_EQ(sp.waves, (std::vector<std::string>{"1"}));
    std::vector<float> out(2 * 48000);
    mix.mix(out.data(), 24000 + 100); // 0.5 s: wave 1 ends
    sp.update(0.51);
    EXPECT_EQ(sp.waves, (std::vector<std::string>{"1", "3"})); // sequence step 2
    EXPECT_TRUE(sp.active(h));
    sp.stop(h);
    EXPECT_FALSE(sp.active(h));
    sp.start(bank[900].children[0]); // in order: the next pick is wave 2
    EXPECT_EQ(sp.waves.back(), "2");
}

TEST(AudioReal, SoundPlexVoiceDtps) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    PlexBank bank;
    bank.add(std::make_shared<const ou::cdc::Drm>(ou::cdc::parseDrm(*bf.read("pc-w\\generalbank.drm"))));
    bank.add(std::make_shared<const ou::cdc::Drm>(ou::cdc::parseDrm(*bf.read("pc-w\\lara.drm"))));
    const Plex* hint = bank.find(284);
    ASSERT_TRUE(hint);
    EXPECT_EQ(hint->type, PlexType::Assignment);
    ASSERT_EQ(hint->children.size(), 1u);
    EXPECT_EQ(hint->children[0].type, PlexType::Stream);
    EXPECT_EQ(hint->children[0].stream, "vo\\lara\\sc_01_05_hint");
    EXPECT_FLOAT_EQ(hint->children[0].volume, 1.0f);
    const Plex* drown = bank.find(2315);
    ASSERT_TRUE(drown);
    ASSERT_EQ(drown->children.size(), 1u);
    const Plex& choice = drown->children[0];
    EXPECT_EQ(choice.type, PlexType::ChoiceList);
    EXPECT_EQ(choice.mode, 2); // shuffle
    ASSERT_EQ(choice.children.size(), 8u);
    for (size_t k = 0; k < 8; ++k) {
        EXPECT_EQ(choice.children[k].stream, "vo\\lara\\lara_drown_0" + std::to_string(k + 1));
        EXPECT_FLOAT_EQ(choice.children[k].volume, 0.8f);
    }

    Mixer mix; // the hint line plays as a voice stream with its English subtitle
    SoundPlayer sp(mix, [&](uint32_t id) { return bank.find(id); }, [&](const std::string& path) -> std::shared_ptr<const Mul> {
        const auto d = bf.read("pc-w\\audio\\streams\\" + path + ".mul");
        return d ? std::make_shared<Mul>(decodeMul(*d)) : nullptr;
    });
    EXPECT_NE(sp.start(*hint), 0u);
    ASSERT_EQ(sp.lines.size(), 1u);
    EXPECT_FALSE(sp.lines[0].subtitle.empty());
}

TEST(AudioReal, SfxMarkersAllUnits) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(list);
    std::string text(list->begin(), list->end());
    size_t markers = 0, triggers = 0, musicActions = 0, units = 0, unresolved = 0;
    for (size_t p = text.find('\n') + 1; p < text.size();) {
        size_t e = text.find('\n', p);
        if (e == text.npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto name = line.substr(line.rfind('\\') + 1);
        auto d = bf.read("pc-w\\" + name + ".drm");
        if (!d) continue;
        ++units;
        SCOPED_TRACE(name);
        auto drm = std::make_shared<const ou::cdc::Drm>(ou::cdc::parseDrm(std::move(*d)));
        PlexBank bank;
        bank.add(drm);
        const auto ms = parseSfxMarkers(*drm);
        markers += ms.size();
        for (const auto& m : ms) {
            for (const auto& s : m.sounds)
                if (s.type == PlexType::Reference && !bank.find(s.id)) ++unresolved;
            for (const auto& t : m.triggers) {
                ++triggers;
                if (t.action >= SfxTrigger::SetVariable) ++musicActions;
                for (int32_t r : t.refs) EXPECT_LT(size_t(r), m.sounds.size());
            }
        }
        if (name == "l3_main_bedroom") {
            ASSERT_EQ(ms.size(), 40u);
            ASSERT_EQ(ms[0].sounds.size(), 2u);
            EXPECT_EQ(ms[0].sounds[0].type, PlexType::Reference);
            EXPECT_EQ(ms[0].sounds[0].id, 11660u);
            ASSERT_EQ(ms[0].triggers.size(), 4u);
            EXPECT_EQ(ms[0].triggers[0].condition, SfxTrigger::Enter);
            EXPECT_EQ(ms[0].triggers[0].action, SfxTrigger::Start);
            EXPECT_NEAR(ms[0].triggers[0].radius, 3322, 1);
            EXPECT_EQ(ms[0].triggers[1].action, SfxTrigger::End);
            const Plex* amb = bank.find(11673); // Assignment -> ChoiceList of 10 waves
            ASSERT_TRUE(amb);
            ASSERT_EQ(amb->children.size(), 1u);
            EXPECT_EQ(amb->children[0].type, PlexType::ChoiceList);
            EXPECT_EQ(amb->children[0].children.size(), 10u);
            EXPECT_EQ(amb->children[0].children[0].type, PlexType::Wave);
            EXPECT_EQ(amb->children[0].children[0].id, 0xC61u);
        }
    }
    EXPECT_EQ(units, 82u);
    EXPECT_EQ(markers, 701u);
    EXPECT_EQ(triggers, 1996u);
    EXPECT_EQ(musicActions, 0u); // retail sfxmarkers never set music variables
    EXPECT_EQ(unresolved, 0u);   // every referenced DTP lives in the same unit DRM
}

// Reaction: game parameter 2 mapped onto volume; Envelope: a volume curve ending at 0 ends the voice (AUDIO.md §9.7).
TEST(SoundPlex, ReactionAndEnvelopeShapeVolume) {
    Mixer m;
    m.addSound(7, constant(1, {16384}, 48000));
    Plex wave;
    wave.type = PlexType::Wave, wave.id = 7;
    Plex react;
    react.type = PlexType::Reaction, react.children = {wave};
    Plex::Map map;
    map.input = 2, map.inMin = 0, map.inMax = 100, map.keys = {{0, 0}, {1, 1}};
    react.maps = {map};
    SoundPlayer sp(m, nullptr, nullptr);
    const auto h = sp.start(react);
    ASSERT_NE(h, 0u);
    const float full = 0.5f * std::sqrt(0.5f);
    EXPECT_NEAR(run(m, 4)[0], 0, 1e-4); // parameter 0 -> silent
    sp.setParam(h, 2, 50);
    sp.update(0);
    EXPECT_NEAR(run(m, 4)[0], 0.5f * full, 1e-3);
    Plex env;
    env.type = PlexType::Envelope, env.children = {wave}, env.curves = {{.keys = {{0.1f, 1}, {0.2f, 0}}}};
    const auto e = sp.start(env);
    EXPECT_TRUE(sp.active(e));
    sp.update(0.25);
    EXPECT_FALSE(sp.active(e));
}

// Envelope loops (0x587A90 / 0x587B80): a sustain key holds until stop(), which plays the release tail from loop end;
// without the stop a one-key-loop envelope (generalbank's main theme, DTP 78) never fades.
TEST(SoundPlex, EnvelopeSustainHoldsUntilStopThenReleases) {
    Plex::Curve c{.release = true, .loopStart = 0, .loopEnd = 0, .keys = {{0, 1}, {0.28f, 0.91f}, {0.49f, 0.8f}, {1.05f, 0}}};
    bool ended = true;
    EXPECT_FLOAT_EQ(c.value(100, -1, &ended), 1);
    EXPECT_FALSE(ended);
    EXPECT_NEAR(c.value(100.28f, 100), 0.91f, 1e-4);
    EXPECT_NEAR(c.value(100.385f, 100), 0.855f, 1e-4); // halfway 0.91 -> 0.8
    c.value(101.1f, 100, &ended);
    EXPECT_TRUE(ended);
    // A two-key loop wraps (keys 1..2 repeat); no loop: plays once and a last key of 0 ends it.
    Plex::Curve loop{.loopStart = 1, .loopEnd = 2, .keys = {{0, 0}, {1, 1}, {2, 0.5f}}};
    EXPECT_NEAR(loop.value(2.5f, -1), 0.75f, 1e-4);
    EXPECT_NEAR(loop.value(10.5f, -1), 0.75f, 1e-4);
    Plex::Curve once{.keys = {{0, 1}, {1, 0}}};
    EXPECT_NEAR(once.value(0.25f, -1, &ended), 0.75f, 1e-4);
    EXPECT_FALSE(ended);
    once.value(1.5f, -1, &ended);
    EXPECT_TRUE(ended);

    Mixer m;
    m.addSound(7, constant(1, {16384}, 48000));
    Plex wave;
    wave.type = PlexType::Wave, wave.id = 7;
    Plex env;
    env.type = PlexType::Envelope, env.children = {wave}, env.curves = {c};
    SoundPlayer sp(m, nullptr, nullptr);
    const auto h = sp.start(env);
    sp.update(10);
    EXPECT_TRUE(sp.active(h)); // held at the sustain key
    sp.stop(h);
    sp.update(0.5);
    EXPECT_TRUE(sp.active(h)); // releasing
    sp.update(0.6);
    EXPECT_FALSE(sp.active(h));
}

// 0x585070 with globalsoundinfo's {max 3160.56, min 158.03, curve 1}: full volume inside min, f² between, 0 beyond max.
TEST(SoundPlex, RolloffFollowsGlobalSoundInfo) {
    EXPECT_FLOAT_EQ(rolloff(100, kSoundMinDistance, kSoundMaxDistance), 1);
    const float mid = (kSoundMinDistance + kSoundMaxDistance) / 2;
    EXPECT_NEAR(rolloff(mid, kSoundMinDistance, kSoundMaxDistance), 0.25f, 1e-5);
    EXPECT_EQ(rolloff(kSoundMaxDistance, kSoundMinDistance, kSoundMaxDistance), 0);
    EXPECT_EQ(rolloff(5000, kSoundMinDistance, kSoundMaxDistance), 0);
    EXPECT_FLOAT_EQ(rolloff(4000, kSoundMinDistance, 5000), (1000 / (5000 - kSoundMinDistance)) * (1000 / (5000 - kSoundMinDistance)));
    // The player gives 3D voices those distances; an Assignment's distance replaces the max.
    Mixer m;
    m.setAttenuation(rolloff);
    m.setListener({});
    m.addSound(7, constant(1, {16384}, 48000));
    Plex wave;
    wave.type = PlexType::Wave, wave.id = 7;
    Plex far;
    far.type = PlexType::Assignment, far.maxDistance = 6000, far.children = {wave};
    SoundPlayer sp(m, nullptr, nullptr);
    sp.start(far, glm::vec3(0, 4000, 0));
    EXPECT_GT(std::abs(run(m, 2)[0]), 0.01f); // beyond the default max, inside the Assignment's
}

// Every DTP of the 82 units and the shared sound DRMs parses as a SoundPlex or is rejected as another DTP; the Effect
// nodes carry only no DSP, highpass or lowpass, and the Oscillators have no duration (AUDIO.md §9.15).
TEST(AudioReal, EffectAndOscillatorNodesAllUnits) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(list);
    std::vector<std::string> names{"lara", "generalbank", "motorcycle"};
    const std::string text(list->begin(), list->end());
    for (size_t p = text.find('\n') + 1; p < text.size();) {
        size_t e = text.find('\n', p);
        if (e == text.npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) names.push_back(line.substr(line.rfind('\\') + 1));
    }
    size_t plexes = 0, effects = 0, oscillators = 0;
    std::map<int, size_t> types;
    const auto walk = [&](const auto& self, const Plex& x) -> void {
        if (x.type == PlexType::Effect) {
            ++effects, ++types[int(x.effect.type)];
            if (x.effect.type == DspType::LowPass || x.effect.type == DspType::HighPass) {
                EXPECT_GE(x.effect.param[0], 1000); // cutoff Hz
                EXPECT_GE(x.effect.param[1], 1);    // resonance
            }
        }
        if (x.type == PlexType::Oscillator) {
            ++oscillators;
            EXPECT_LE(x.delay, 0);
        }
        for (const auto& c : x.children) self(self, c);
    };
    for (const auto& name : names) {
        auto d = bf.read("pc-w\\" + name + ".drm");
        if (!d) continue;
        const auto drm = ou::cdc::parseDrm(std::move(*d));
        for (uint32_t i = 0; i < drm.sections.size(); ++i) {
            if (drm.sections[i].type != ou::cdc::SectionType::DtpData) continue;
            try {
                walk(walk, parsePlex(drm, i, 0));
                ++plexes;
            } catch (const FormatError&) { /* another DTP kind */ }
        }
    }
    EXPECT_EQ(plexes, 6147u);
    EXPECT_EQ(effects, 8u);
    EXPECT_EQ(oscillators, 2u);
    EXPECT_EQ(types, (std::map<int, size_t>{{int(DspType::None), 4}, {int(DspType::HighPass), 2}, {int(DspType::LowPass), 2}}));

    // globalsoundinfo: 77 categories; 59 (18 Assignments) under 58 carry the only category DSPs, two ParamEqs.
    const auto gsi = ou::cdc::parseDrm(*bf.read("pc-w\\globalsoundinfo.drm"));
    const auto cats = parseSoundCategories(gsi);
    ASSERT_EQ(cats.size(), 77u);
    EXPECT_EQ(cats[59].parent, 58);
    EXPECT_EQ(cats[58].effect.type, DspType::ParamEq);
    EXPECT_FLOAT_EQ(cats[58].effect.param[0], 175);
    EXPECT_FLOAT_EQ(cats[59].effect.param[0], 8200);
    EXPECT_FLOAT_EQ(cats[59].effect.param[2], 0.2f);
    EXPECT_EQ(std::count_if(cats.begin(), cats.end(), [](const SoundCategory& c) { return c.effect.type != DspType::None; }), 2);
    // The environment block (+0x2C) is the silent default the Mixer starts with.
    const auto data = ou::cdc::sectionPointers(gsi, 2).at(0xC8);
    const auto block = ou::cdc::sectionPointers(gsi, data.section).at(data.offset + 0x2C);
    SfxReverbProps env;
    std::memcpy(env.data(), gsi.sections[block.section].data.data() + block.offset, sizeof env);
    EXPECT_EQ(env, kReverbOff);
}

// Lara's footsteps: Selector -> MaterialList picks a different graph per surface material; the bike engine is an
// Overlap of loops under Reactions on parameter 2 (rpm).
TEST(AudioReal, MaterialListAndEngineReaction) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    auto lara = std::make_shared<const ou::cdc::Drm>(ou::cdc::parseDrm(*bf.read("pc-w\\lara.drm")));
    auto bike = std::make_shared<const ou::cdc::Drm>(ou::cdc::parseDrm(*bf.read("pc-w\\motorcycle.drm")));
    PlexBank bank;
    bank.add(lara), bank.add(bike);
    Mixer m;
    m.addWaves(*lara), m.addWaves(*bike);
    SoundPlayer sp(m, [&](uint32_t id) { return bank.find(id); }, nullptr);
    const Plex* step = bank.find(953);
    ASSERT_TRUE(step);
    EXPECT_EQ(step->type, PlexType::Selector);
    sp.start(*step, glm::vec3(0), 1, 27);
    sp.update(1);
    const auto a = sp.waves;
    sp.waves.clear();
    sp.start(*step, glm::vec3(0), 1, 17);
    sp.update(1);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(sp.waves.empty());
    EXPECT_NE(a, sp.waves);
    const Plex* engine = bank.find(13589);
    ASSERT_TRUE(engine);
    ASSERT_EQ(engine->type, PlexType::Overlap);
    ASSERT_EQ(engine->children.size(), 5u);
    const Plex& r = engine->children[0].children[0];
    ASSERT_EQ(r.type, PlexType::Reaction);
    ASSERT_EQ(r.maps.size(), 4u);
    EXPECT_EQ(r.maps[0].input, 2);
    EXPECT_EQ(r.maps[0].output, 0);
    EXPECT_FLOAT_EQ(r.maps[0].inMin, 1500);
    EXPECT_FLOAT_EQ(r.maps[0].inMax, 8000);
    EXPECT_TRUE(r.maps[0].zeroAbove);
    EXPECT_EQ(r.maps[1].output, 1);
    EXPECT_TRUE(r.maps[1].add);
}

// GameSound asks for the bike engine before it loads motorcycle.drm: the miss must not stick after the add.
TEST(AudioReal, BankFindsDtpAddedAfterMiss) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    PlexBank bank;
    EXPECT_FALSE(bank.find(13589));
    bank.add(std::make_shared<const ou::cdc::Drm>(ou::cdc::parseDrm(*bf.read("pc-w\\motorcycle.drm"))));
    EXPECT_TRUE(bank.find(13589));
}

// Script `sound` 1 / 2 wait sequences ask SoundPlayer::busy: game time (update()), the data's length, not the mixer.
// A sequence lasts its parts, a looping Wave until stopped, a stop ends it at once.
TEST(SoundPlayer, BusyFollowsGameTimeNotTheMixer) {
    Mixer mix;
    auto tone = [](size_t frames, bool loop = false) {
        auto p = std::make_shared<Pcm>();
        p->sampleRate = 1000, p->channels = 1, p->samples.assign(frames, 1000);
        if (loop) p->loopEnd = uint32_t(frames);
        return p;
    };
    mix.addSound(1, tone(500)), mix.addSound(2, tone(250)), mix.addSound(3, tone(100, true));
    Plex seq{PlexType::Sequence};
    seq.children = {Plex{PlexType::Wave, 1}, Plex{PlexType::Wave, 2}};
    SoundPlayer sp(mix, nullptr, {});
    const auto h = sp.start(seq); // 0.5 s, then 0.25 s
    EXPECT_TRUE(sp.busy(h));
    sp.update(0.49);
    EXPECT_TRUE(sp.busy(h));
    sp.update(0.02); // the second part starts at 0.5 s (the mixer was never run: its voices count as playing)
    EXPECT_TRUE(sp.busy(h));
    sp.update(0.2);
    EXPECT_TRUE(sp.busy(h));
    sp.update(0.05);
    EXPECT_FALSE(sp.busy(h));
    const auto loop = sp.start(Plex{PlexType::Wave, 3});
    sp.update(100);
    EXPECT_TRUE(sp.busy(loop));
    sp.stop(loop);
    EXPECT_FALSE(sp.busy(loop));
    EXPECT_FALSE(sp.busy(0));
}

// 0x595250 reads the stream's play position: a position behind the last one holds the clock and gives a frame time
// of 0 (the guard; in play the counter runs on through the .mul loop).
TEST(Mus, StreamClockHoldsWhenThePositionGoesBack) {
    const auto f = parseMus(makeMus());
    MusSequencer q(f);
    std::array<uint8_t, 256> vars{};
    const float vol = 64 / 127.0f;
    EXPECT_TRUE(q.updateAt(0, vars));
    EXPECT_TRUE(q.updateAt(0.25, vars)); // 250 ms of stream: the fade to 1 over 1 s ramps a quarter
    EXPECT_NEAR(q.stemGains()[0], vol * 0.25f, 1e-6);
    EXPECT_EQ(q.ticks(), 240u);          // beat 0.5 at 120 BPM
    EXPECT_FALSE(q.updateAt(0.1, vars)); // behind: clock and fades hold
    EXPECT_EQ(q.ticks(), 240u);
    EXPECT_NEAR(q.stemGains()[0], vol * 0.25f, 1e-6);
    EXPECT_FALSE(q.updateAt(0.2499, vars));
    EXPECT_TRUE(q.updateAt(0.5, vars)); // past the held position: +250 ms
    EXPECT_EQ(q.ticks(), 480u);
    EXPECT_NEAR(q.stemGains()[0], vol * (0.25f + 0.75f * 0.25f), 1e-6);
}

namespace {
// A player over a 1 kHz mixer whose tracks are constant stereo tones: "left" on the left channel, others on the right.
struct PlayerRig {
    Mixer mixer{1000};
    std::vector<std::string> loads, lines;
    MusicPlayer player{mixer,
                       [this](const std::string& name) -> std::optional<MusicPlayer::Track> {
                           loads.push_back(name);
                           if (name == "missing") return std::nullopt;
                           return MusicPlayer::Track{constant(2, name == "left" ? std::vector<int16_t>{16384, 0} : std::vector<int16_t>{0, 16384},
                                                              100000, 1000)};
                       },
                       [this](const std::string& l) { lines.push_back(l); }};
    std::array<uint8_t, 256> vars{};
    // n game ticks of 1/60 s, each followed by its 1000/60 output frames; returns the last frame.
    std::array<float, 2> ticks(int n) {
        std::array<float, 2> last{};
        for (int i = 0; i < n; ++i) {
            player.update(1 / 60.0f, vars);
            const auto o = run(mixer, 16);
            last = {o[30], o[31]};
        }
        return last;
    }
};
} // namespace

// The scripts' crossfade (pool_to_ragnarok tick 101: Play l4_staircase_upper_mus_01 in slot 1, Stop slot 0 over 3 s):
// two slots, two voices, each master linear over its own fade (0x58ADE0), the old slot freed when its fade ends.
TEST(MusicPlayer, TwoSlotsCrossfadeWithTheirOwnFades) {
    PlayerRig r;
    r.player.play(0, "left", 0, 0);
    r.ticks(1);
    EXPECT_NEAR(r.player.slot(0).master, 1, 1e-4);
    r.player.play(1, "right", 0, 2); // fade in 2 s (60 frames)
    r.player.stop(0, 3);             // fade out 3 s (90 frames)
    EXPECT_TRUE(r.player.slot(0).fadingOut);
    auto o = r.ticks(60); // 1 s
    EXPECT_NEAR(r.player.slot(0).master, 2 / 3.0f, 1e-3);
    EXPECT_NEAR(r.player.slot(1).master, 0.5f, 1e-3);
    EXPECT_NEAR(o[0], 0.5f * 2 / 3.0f, 0.01f); // both audible: the overlap
    EXPECT_NEAR(o[1], 0.5f * 0.5f, 0.01f);
    o = r.ticks(60); // 2 s
    EXPECT_NEAR(r.player.slot(1).master, 1, 1e-4);
    EXPECT_NEAR(r.player.slot(0).master, 1 / 3.0f, 1e-3);
    r.ticks(62); // 3 s and the frame 0x594D10 frees the slot in
    EXPECT_TRUE(r.player.slot(0).track.empty());
    EXPECT_EQ(r.mixer.activeVoices(), 1u);
    o = r.ticks(1);
    EXPECT_NEAR(o[0], 0, 1e-6);
    EXPECT_NEAR(o[1], 0.5f, 1e-4);
    EXPECT_EQ(r.loads, (std::vector<std::string>{"left", "right"}));
}

// A request for the track a slot holds leaves it playing where it is (0x593EB0 compares the names, case-insensitive).
TEST(MusicPlayer, SameTrackRequestDoesNotRestart) {
    PlayerRig r;
    r.player.play(2, "left", 0, 1);
    r.ticks(60);
    const auto voice = r.player.slot(2).voice;
    const double at = *r.mixer.position(voice);
    r.player.play(2, "LEFT", 4, 4);
    r.player.load(2, "left", 4);
    r.ticks(30);
    EXPECT_EQ(r.player.slot(2).voice, voice);
    EXPECT_NEAR(*r.mixer.position(voice), at + 0.48, 0.02);
    EXPECT_NEAR(r.player.slot(2).master, 1, 1e-4);
    EXPECT_EQ(r.loads.size(), 1u);
}

// One slot holds one track: a new one waits for the playing one's fade-out, then loads and fades in (0x594D10
// requests the pending name once the slot is free).
TEST(MusicPlayer, NewTrackInTheSameSlotFollowsTheFadeOut) {
    PlayerRig r;
    r.player.play(0, "left", 0, 0);
    r.ticks(1);
    r.player.play(0, "right", 1, 0.5f);
    EXPECT_EQ(r.player.slot(0).pending, "right");
    r.ticks(30);
    EXPECT_NEAR(r.player.slot(0).master, 0.5f, 1e-3);
    EXPECT_EQ(r.mixer.activeVoices(), 1u);
    r.ticks(31); // fade done, the slot freed
    EXPECT_EQ(r.mixer.activeVoices(), 0u);
    r.ticks(1); // the pending track loads
    EXPECT_EQ(r.player.slot(0).track, "right");
    r.ticks(14); // fade in 0.5 s
    EXPECT_NEAR(r.player.slot(0).master, 0.5f, 0.04f);
    r.ticks(16);
    EXPECT_NEAR(r.player.slot(0).master, 1, 1e-4);
}

// Load (native 1) loads silently; Play of the same track starts it. A Play during the fade-out of a Stop fades it back
// in without a restart. A stop with no fade (or of a track not playing) frees the slot at once.
TEST(MusicPlayer, LoadThenPlayAndFadeBackIn) {
    PlayerRig r;
    r.player.load(1, "left", 0);
    r.ticks(5);
    EXPECT_EQ(r.player.slot(1).track, "left");
    EXPECT_FALSE(r.player.slot(1).playing);
    EXPECT_EQ(r.mixer.activeVoices(), 0u);
    r.player.play(1, "left", 0, 1);
    r.ticks(60);
    EXPECT_NEAR(r.player.slot(1).master, 1, 1e-4);
    const auto voice = r.player.slot(1).voice;
    r.player.stop(1, 2);
    r.ticks(30);
    EXPECT_NEAR(r.player.slot(1).master, 0.75f, 1e-3);
    r.player.play(1, "left", 0, 1); // back up over 1 s from 0.75
    r.ticks(30);
    EXPECT_NEAR(r.player.slot(1).master, 0.875f, 1e-3);
    EXPECT_EQ(r.player.slot(1).voice, voice);
    r.player.stop(1, 0);
    EXPECT_EQ(r.mixer.activeVoices(), 0u);
    r.ticks(1);
    EXPECT_TRUE(r.player.idle());
    r.player.play(3, "missing", 0, 0); // cannot load: reported once, not retried
    r.ticks(3);
    EXPECT_EQ(std::count(r.loads.begin(), r.loads.end(), "missing"), 1);
    r.player.reset();
    EXPECT_TRUE(r.player.idle());
}
