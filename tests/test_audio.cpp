#include "audio/Mixer.hpp"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <cmath>
#include <cstdlib>
#include <cstring>
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
