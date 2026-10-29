#include "audio/Mixer.hpp"
#include "audio/SoundPlex.hpp"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

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

// One sequence (tempo 500000 us, 480 ppqn), 1 tone/program/sample, one stereo stem, 2 labels.
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
    put32(v, 500000); put16(v, 480); put16(v, 0x106); put16(v, 1); put16(v, 0); put32(v, 0x14);
    v.insert(v.end(), t.begin(), t.end());
    return v;
}
} // namespace

TEST(Mus, ParsesSyntheticScript) {
    const auto f = parseMus(makeMus());
    EXPECT_EQ(f.version, kMusVersion);
    EXPECT_EQ(f.header20, 120u);
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
    EXPECT_EQ(s.tempo, 500000u);
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

TEST(Mixer, MusicCrossfadeAndStems) {
    Mixer m(1000);
    m.playMusic(constant(2, {16384, 0}, 100000, 1000), 0);
    m.playMusic(constant(2, {0, 16384}, 100000, 1000), 0.1f); // 100 output frames
    auto o = run(m, 50);
    EXPECT_NEAR(o[2 * 49], 0.25f, 0.01f);  // old track half faded out
    EXPECT_NEAR(o[2 * 49 + 1], 0.25f, 0.01f);
    run(m, 100);
    EXPECT_EQ(m.activeVoices(), 1u);
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0, 1e-6);
    EXPECT_NEAR(o[1], 0.5f, 1e-4);

    auto four = std::make_shared<Pcm>(*constant(4, {16384, 16384, 8192, 8192}, 1000, 1000));
    m.playMusic(four, 0, {0, 1}); // stem 0 muted, stem 1 (channels 2,3) on
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0.25f, 1e-4);
    m.setMusicStemGains({1, 0});
    o = run(m, 2);
    EXPECT_NEAR(o[0], 0.5f, 1e-4);
    m.stopMusic(0);
    EXPECT_EQ(m.activeVoices(), 0u);
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
    env.type = PlexType::Envelope, env.children = {wave}, env.curves = {{0, {{0.1f, 1}, {0.2f, 0}}}};
    const auto e = sp.start(env);
    EXPECT_TRUE(sp.active(e));
    sp.update(0.25);
    EXPECT_FALSE(sp.active(e));
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
