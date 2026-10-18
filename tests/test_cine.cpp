// src/cine: CINE stream decode, playback (camera, bones, cuts, subtitles), unit cinematic binding.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"
#include "formats/Formats.hpp"

using namespace ou;
using cdc::FormatError;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

void put32(std::vector<uint8_t>& b, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}
void putF(std::vector<uint8_t>& b, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(b, v);
}
void set32(std::vector<uint8_t>& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }

// Channels: marker 0-8, instance 9-39 (3 + 2 bones x 14), camera 40-55, event 56.
constexpr uint32_t kInst = 9, kBone0 = kInst + 3, kBone1 = kBone0 + 14, kCam = 40, kEvent = 56, kChannels = 57;
constexpr int kFrames = 4;

float synthValue(uint32_t c, int f) {
    if (c < 3 || (c >= kBone0 && c < kBone0 + 3) || (c >= kBone1 && c < kBone1 + 3) || (c >= kCam && c < kCam + 3)) return 1; // scales
    if (c == kBone0 + 6) return 100.0f * float(f);             // bone 0 walks along +x
    if (c == kBone1 + 7) return 10;                            // bone 1 sits 10 along its parent's y
    if (c == kBone1 + 5) return float(f) * 0.25f;              // and turns about z
    if (c == kCam + 3) return 1.5707964f;                      // camera Rx(90): looks along +Y
    if (c == kCam + 7) return f >= 2 ? 1000.0f : -500.0f + f;  // cut at frame 2
    if (c == kCam + 8) return 100;
    if (c == kCam + 9) return 1.0f;                            // horizontal FOV
    if (c == kCam + 10) return f == 2 ? 13.0f : 12.0f;         // bit 0 = cut
    if (c == kEvent) return f >= 1 ? 1.0f : 0.0f;
    return 0;
}

std::vector<uint8_t> syntheticCine() {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'E', 'N', 'I', 'C'});
    put32(b, 9);
    put32(b, 0); // header size, patched below
    std::string name = "l1_test";
    name.resize(64);
    b.insert(b.end(), name.begin(), name.end());
    put32(b, 44);                                   // unit id
    put32(b, 1); put32(b, 44); put32(b, 1364); put32(b, 0); // marker
    put32(b, 1);                                    // instances
    put32(b, ~0u); put32(b, ~0u); put32(b, 2); put32(b, kInst);
    for (int bone = 0; bone < 2; ++bone)            // bone fix: identity, bone 1 Rz(90) (+ translation, dropped)
        for (int i = 0; i < 16; ++i) {
            const float rz[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 7, 7, 7, 1};
            putF(b, bone ? rz[i] : (i % 5 == 0 ? 1.0f : 0.0f));
        }
    put32(b, 0); put32(b, kInst + 31);              // lights
    put32(b, 0); put32(b, kInst + 31);              // vars
    put32(b, 1); put32(b, kCam);                    // cameras
    put32(b, 44); put32(b, 1); put32(b, 77); put32(b, kEvent); // events
    put32(b, 0);
    putF(b, 1); putF(b, 2); putF(b, 3); putF(b, 0); putF(b, 0); putF(b, 0.5f); put32(b, 44); // instance tail
    set32(b, 8, uint32_t(b.size() - 8));
    for (int f = 0; f < kFrames; ++f) {
        const size_t start = b.size();
        put32(b, 0);
        put32(b, uint32_t(f));
        for (uint32_t c = 0; c < kChannels; ++c) { // channel c: one animated run over all frames when odd, else per frame
            if (c % 2) {
                if (f == 0) put32(b, uint32_t(kFrames) | 0x10000000u);
                putF(b, synthValue(c, f));
            } else {
                put32(b, 1); // a fresh constant run every frame
                putF(b, synthValue(c, f));
            }
        }
        const std::string sub = f == 1 ? "0\rLara: Hello\r1\rBonjour\r" : "";
        put32(b, uint32_t(sub.size()));
        b.insert(b.end(), sub.begin(), sub.end());
        b.resize((b.size() + 15) / 16 * 16);
        set32(b, start, uint32_t(b.size() - start - 4));
    }
    for (int k = 0; k < 2; ++k) { put32(b, 12); put32(b, ~0u); put32(b, 0); put32(b, 0); } // terminator + mux padding
    return b;
}

scene::Skeleton skeleton3() { // bone 2 is not animated by the stream: it rides on bone 1
    scene::Skeleton s;
    s.bones.resize(3);
    s.bones[1].parent = 0;
    s.bones[2].parent = 1, s.bones[2].pivot[2] = 5;
    return s;
}

void expectNear(glm::vec3 a, glm::vec3 b, float eps = 1e-3f) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

} // namespace

TEST(Cine, DemuxesAndParsesSyntheticStream) {
    const auto cine = syntheticCine();
    std::vector<uint8_t> mul(2048);
    auto packet = [&](uint32_t type, std::span<const uint8_t> data) {
        put32(mul, type), put32(mul, uint32_t(data.size())), put32(mul, 0), put32(mul, 0);
        mul.insert(mul.end(), data.begin(), data.end());
    };
    packet(1, std::span(cine).first(100));
    packet(0, std::vector<uint8_t>(48, 0x55));
    packet(1, std::span(cine).subspan(100));
    EXPECT_EQ(cine::demux(mul), cine);
    mul.resize(mul.size() - 1);
    EXPECT_THROW(cine::demux(mul), FormatError);

    const auto s = cine::scan(cine);
    const auto& h = s.header;
    EXPECT_EQ(h.version, 9u);
    EXPECT_EQ(h.unitName, "l1_test");
    EXPECT_EQ(h.unitId, 44u);
    ASSERT_EQ(h.markers.size(), 1u);
    EXPECT_EQ(h.markers[0].intro, 1364);
    ASSERT_EQ(h.instances.size(), 1u);
    EXPECT_EQ(h.instances[0].intro, -1);
    EXPECT_EQ(h.instances[0].boneChannel(1), kBone1);
    EXPECT_EQ(h.instances[0].boneFix[1][3], glm::vec4(0, 0, 0, 1)); // translation dropped
    ASSERT_TRUE(h.instances[0].endPos);
    EXPECT_EQ(*h.instances[0].endPos, glm::vec3(1, 2, 3));
    EXPECT_EQ(h.cameras, std::vector<uint32_t>{kCam});
    ASSERT_EQ(h.events.size(), 1u);
    EXPECT_EQ(h.events[0].id, 77u);
    EXPECT_EQ(h.channels, kChannels);
    EXPECT_EQ(s.frames, kFrames);
    EXPECT_TRUE(s.cleanEnd);
    ASSERT_EQ(s.subtitles.size(), 2u);
    EXPECT_EQ(s.subtitles[0].frame, 1);
    EXPECT_EQ(s.subtitles[1].text, "Bonjour");

    cine::Decoder d(cine, h);
    for (int f = 0; f < kFrames; ++f) {
        ASSERT_TRUE(d.next());
        for (uint32_t c = 0; c < kChannels; ++c) ASSERT_EQ(d.values()[c], synthValue(c, f)) << "frame " << f << " channel " << c;
    }
    EXPECT_FALSE(d.next());
    EXPECT_TRUE(d.terminated());

    auto bad = cine;
    bad[0] = 'X';
    EXPECT_THROW(cine::scan(bad), FormatError);
    bad = cine;
    set32(bad, h.framesOffset + 4, 5); // frame number out of sequence
    EXPECT_THROW(cine::scan(bad), FormatError);
}

// Every truncation before the -1 frame throws FormatError, except a cut at a frame boundary (voice-line streams
// end that way), which yields the frames before it. Once the -1 frame's header is read the rest is ignored.
TEST(Cine, TruncationThrowsOrEndsAtAFrame) {
    const auto cine = syntheticCine();
    const auto full = cine::scan(cine);
    std::vector<size_t> boundaries{full.header.framesOffset};
    cine::Decoder d(cine, full.header);
    while (d.next()) boundaries.push_back(d.offset());
    const size_t terminator = boundaries.back();
    for (size_t n = 0; n < cine.size(); ++n) {
        SCOPED_TRACE(n);
        const auto at = std::find(boundaries.begin(), boundaries.end(), n);
        if (n >= terminator + 8) {
            EXPECT_EQ(cine::scan(std::span(cine).first(n)).frames, kFrames);
        } else if (at != boundaries.end()) {
            const auto s = cine::scan(std::span(cine).first(n));
            EXPECT_EQ(s.frames, int(at - boundaries.begin()));
            EXPECT_TRUE(s.cleanEnd);
        } else {
            EXPECT_THROW(cine::scan(std::span(cine).first(n)), FormatError);
        }
    }
}

TEST(Cine, PlayerInterpolatesCutsAndPoses) {
    cine::Player p(syntheticCine());
    const auto& in = p.header().instances[0];
    const auto sk = skeleton3();

    EXPECT_TRUE(p.seek(0.5));
    auto bones = p.bones(in, sk);
    expectNear(glm::vec3(bones[0][3]), {50, 0, 0}); // halfway between frames 0 and 1
    // Bone 1: Rz(0.125) then the fix Rz(90); position from the unfixed matrix.
    expectNear(glm::vec3(bones[1][3]), {50, 10, 0});
    const glm::mat4 r = glm::rotate(glm::mat4(1), 0.125f + 1.5707964f, glm::vec3(0, 0, 1));
    expectNear(glm::vec3(bones[1][0]), glm::vec3(r[0]));
    // Bone 2 rides on bone 1 (fixed matrix) at its pivot.
    expectNear(glm::vec3(bones[2][3]), glm::vec3(bones[1] * glm::vec4(0, 0, 5, 1)));
    auto cam = p.camera();
    expectNear(cam.forward, {0, 1, 0});
    expectNear(cam.up, {0, 0, 1});
    expectNear(cam.position, {0, -499.5f, 100});
    EXPECT_NEAR(cam.verticalFov(1.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(cam.verticalFov(2.0f), 2 * std::atan(std::tan(0.5f) / 2), 1e-5f);
    ASSERT_EQ(p.newSubtitles().size(), 2u); // frame 1 is decoded at pos 0.5
    EXPECT_EQ(p.newSubtitles()[0].frame, 1);

    EXPECT_TRUE(p.seek(1.5)); // frame 2 starts a new shot: frame 1 is held, no smear across the cut
    expectNear(p.camera().position, {0, -499, 100});
    expectNear(glm::vec3(p.bones(in, sk)[0][3]), {100, 0, 0});
    EXPECT_TRUE(p.newSubtitles().empty());
    EXPECT_TRUE(p.seek(2.25));
    expectNear(p.camera().position, {0, 1000, 100});
    EXPECT_TRUE(p.camera().cut == false);

    p.origin = glm::translate(glm::mat4(1), glm::vec3(0, 0, 1000)); // scene origin moves everything
    expectNear(glm::vec3(p.bones(in, sk)[0][3]), {225, 0, 1000});
    expectNear(p.camera().position, {0, 1000, 1100});

    EXPECT_FALSE(p.seek(10)); // past the end: last frame
    EXPECT_TRUE(p.finished());
    EXPECT_EQ(p.frame(), kFrames - 1);
    expectNear(glm::vec3(p.bones(in, sk)[0][3]), {300, 0, 1000});
}

TEST(Cine, OriginFromMarker) {
    cine::Player p(syntheticCine());
    const glm::mat4 intro = glm::translate(glm::mat4(1), glm::vec3(5, 6, 7));
    expectNear(glm::vec3(p.originFromMarker(p.header().markers[0], intro)[3]), {5, 6, 7});
}

// ---- real data (skipped without OU_GAME_DIR) ----------------------------------------------------------------

namespace {

// Every pc-w\cinstream\*.mul (the cinematic tables of the units, docs/formats/CINEMATICS.md).
const char* const kStreams[] = {
    "001_pro_corridor", "010_pro_surprise", "030_med_yacht", "040_med_ruin", "045_med_kraken", "049_med_kraken_death",
    "050_med_thor", "051_med_thor2", "052_med_shipapproach", "055_med_merc", "060_med_natla", "070_med_dive",
    "080_tha_yacht", "090_tha_ruins", "091_tha_naga", "092_tha_arch", "095_tha_powerup", "100_tha_thor",
    "105_tha_powerdown", "110_tha_yachtexit", "120_man_vault", "130_man_study", "145_man_powerdown", "150_man_pinned",
    "160_man_shot", "170_man_exit", "180_mex_intro", "185_mex_calendar", "186_mex_calendar_mirror", "190_mex_mayan",
    "195_mex_lords", "198_mex_thralls", "200_mex_serpent", "210_mex_belt", "220_mex_exit", "230_val_intro",
    "235_val_shaft", "240_val_gate", "242_val_traps", "245_val_yeti", "250_val_hammer", "260_ship_intro",
    "270_shp_escape", "275_hel_explosion", "280_hel_intro", "290_hel_gate", "300_hel_mother", "310_hel_saved",
    "320_hel_boss", "330_hel_thralls", "340_hel_endgame", "342_hel_endgame", "343_hel_engame",
};

std::vector<uint8_t> stream(const cdc::BigfileSet& bf, const std::string& name) {
    const auto mul = bf.read("pc-w\\cinstream\\" + name + ".mul");
    if (!mul) throw std::runtime_error(name + ".mul missing");
    return cine::demux(*mul);
}

bool strictUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        i += n;
    }
    return true;
}

} // namespace

TEST(CineReal, AllStreamsDecodeToTheEnd) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    size_t subtitles = 0, framesWithText = 0;
    for (const char* name : kStreams) {
        SCOPED_TRACE(name);
        const auto s = cine::scan(stream(bf, name));
        EXPECT_TRUE(s.cleanEnd);
        EXPECT_GE(s.frames, 100);
        EXPECT_FALSE(s.header.cameras.empty());
        EXPECT_FALSE(s.header.markers.empty());
        EXPECT_FALSE(s.header.instances.empty());
        for (const auto& in : s.header.instances) EXPECT_TRUE(in.endPos.has_value());
        std::set<int> frames;
        for (const auto& t : s.subtitles) {
            EXPECT_TRUE(strictUtf8(t.text));
            frames.insert(t.frame);
        }
        subtitles += s.subtitles.size(), framesWithText += frames.size();
    }
    EXPECT_EQ(subtitles, 3253u); // docs/formats/TEXT.md
    EXPECT_EQ(framesWithText, 845u);
}

TEST(CineReal, NatlaSubtitlesCameraAndVoiceLines) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto s = cine::scan(stream(bf, "060_med_natla"));
    EXPECT_EQ(s.header.unitName, "l1_ship_natla_room");
    EXPECT_EQ(s.frames, 3181);
    EXPECT_EQ(s.header.channels, 6933u);
    const auto en = std::find_if(s.subtitles.begin(), s.subtitles.end(), [](const auto& t) { return t.language == 0; });
    ASSERT_NE(en, s.subtitles.end());
    EXPECT_EQ(en->frame, 109);
    EXPECT_EQ(en->text, "Damn, it's too small...");

    // Camera continuity: per-frame steps inside a shot stay small; the jumps are exactly the cut frames.
    cine::Player p(stream(bf, "060_med_natla"));
    glm::vec3 last = p.camera().position, lastFwd = p.camera().forward;
    float maxStep = 0, maxTurn = 0;
    int cuts = 0;
    for (int f = 1; f < s.frames; ++f) {
        p.seek(f);
        const auto c = p.camera();
        // At integer positions the values are frame f exactly; the cut flag sits on the frame that starts a shot.
        if ((int(std::lround(p.value(s.header.cameras[0] + 10))) & 1) != 0) ++cuts;
        else {
            maxStep = std::max(maxStep, glm::distance(c.position, last));
            maxTurn = std::max(maxTurn, glm::degrees(std::acos(std::clamp(glm::dot(c.forward, lastFwd), -1.0f, 1.0f))));
        }
        last = c.position, lastFwd = c.forward;
    }
    EXPECT_EQ(cuts, 24);
    EXPECT_LT(maxStep, 10.0f); // units per frame (measured 8.9)
    EXPECT_LT(maxTurn, 1.5f);  // degrees per frame (measured 1.04)

    // Voice-line streams: scene-less CINE header (0 channels), no -1 terminator.
    const auto vo = bf.read("pc-w\\audio\\streams\\vo\\lara\\sc_04_01_answer.mul");
    ASSERT_TRUE(vo);
    const auto v = cine::scan(cine::demux(*vo));
    EXPECT_EQ(v.header.channels, 0u);
    EXPECT_TRUE(v.cleanEnd);
    ASSERT_FALSE(v.subtitles.empty());
    EXPECT_EQ(v.subtitles[0].language, 0);
    EXPECT_EQ(v.subtitles[0].text, "\"I need to find an ancient Mayan Ball Court.\"");
}

// The cinematic each chapter's closing cinematic_object plays (its "cinematic" member indexes the unit's table).
TEST(CineReal, ChapterEndCinematics) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const struct { const char* unit; int32_t intro; const char* cine; } ends[] = {
        {"l3_main_hall", 4106, "010_pro_surprise"},     {"l1_cine_ship_sinking", 1005, "070_med_dive"},
        {"l2_cliffclimb", 2565, "110_tha_yachtexit"},   {"l3_mansion_exterior_afterburn", 4070, "170_man_exit"},
        {"l4_ballcourt", 5483, "220_mex_exit"},         {"l5_valaskjalf", 6846, "250_val_hammer"},
        {"l6_ship_natla_room", 1387, "270_shp_escape"},
    };
    for (const auto& e : ends) {
        SCOPED_TRACE(e.unit);
        const auto drm = cdc::parseDrm(*bf.read(std::string("pc-w\\") + e.unit + ".drm"));
        const auto unit = formats::parseUnit(drm);
        const auto in = std::find_if(unit.intros.begin(), unit.intros.end(), [&](const auto& i) { return i.uniqueId == e.intro; });
        ASSERT_NE(in, unit.intros.end());
        EXPECT_EQ(cine::introCinematic(drm, in->scriptId), e.cine);
        std::string n = cine::parseHeader(stream(bf, e.cine)).unitName;
        for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
        EXPECT_EQ(n, e.unit);
    }
    const auto ballcourt = cdc::parseDrm(*bf.read("pc-w\\l4_ballcourt.drm"));
    EXPECT_EQ(cine::unitCinematics(ballcourt),
              (std::vector<std::string>{"185_mex_calendar", "190_mex_mayan", "220_mex_exit", "186_mex_calendar_mirror"}));
}

// Lara's cinematic skeleton is lara.drm's: the animated bone offsets match the bind pivots.
TEST(CineReal, LaraBonesMatchHerSkeleton) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    std::vector<std::string> warnings;
    const auto skeletons = anim::decodeSkeletons(cdc::parseDrm(*bf.read("pc-w\\lara.drm")), warnings);
    ASSERT_FALSE(skeletons.empty());
    const auto& sk = skeletons[0];
    cine::Player p(stream(bf, "001_pro_corridor"));
    const auto lara = std::find_if(p.header().instances.begin(), p.header().instances.end(), [](const auto& i) { return i.intro == -1; });
    ASSERT_NE(lara, p.header().instances.end());
    EXPECT_LE(lara->bones, sk.bones.size());
    int compared = 0, close = 0;
    for (uint32_t b = 1; b < lara->bones; ++b) {
        const uint32_t c = lara->boneChannel(b) + 6;
        const float a = glm::length(glm::vec3(p.raw(c), p.raw(c + 1), p.raw(c + 2)));
        const float bind = glm::length(glm::vec3(sk.bones[b].pivot[0], sk.bones[b].pivot[1], sk.bones[b].pivot[2]));
        if (bind < 1) continue;
        ++compared;
        close += std::abs(a - bind) < 0.05f * bind + 0.5f;
    }
    EXPECT_GT(compared, 60);
    EXPECT_GT(close, compared * 9 / 10) << close << "/" << compared;
}
