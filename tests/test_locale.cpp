#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <gtest/gtest.h>

#include "cdc/Cdc.hpp"
#include "locale/Locale.hpp"
#include "video/Movie.hpp"

using namespace ou::locale;
using ou::cdc::FormatError;

namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void set32(std::vector<uint8_t>& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
void putStr(std::vector<uint8_t>& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }

std::vector<uint8_t> table(uint32_t lang, const std::vector<std::optional<std::string>>& strings) {
    std::vector<uint8_t> b;
    put32(b, lang);
    put32(b, uint32_t(strings.size()));
    b.resize(8 + 4 * strings.size());
    for (size_t i = 0; i < strings.size(); ++i) {
        if (!strings[i]) continue;
        set32(b, 8 + 4 * i, uint32_t(b.size()));
        putStr(b, *strings[i]);
        b.push_back(0);
    }
    return b;
}

// RFC 3629: no overlongs, surrogates or code points above U+10FFFF.
bool strictUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        int n;
        uint32_t cp;
        if (c < 0x80) n = 0, cp = c;
        else if ((c & 0xE0) == 0xC0) n = 1, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) n = 2, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) n = 3, cp = c & 0x07;
        else return false;
        if (i + n >= s.size() + (n == 0)) return false;
        for (int k = 1; k <= n; ++k) {
            if ((uint8_t(s[i + k]) & 0xC0) != 0x80) return false;
            cp = cp << 6 | (uint8_t(s[i + k]) & 0x3F);
        }
        static constexpr uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMin[n] || (cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF) return false;
        i += n + 1;
    }
    return true;
}

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

} // namespace

TEST(Strings, DecodesSyntheticTableWithExeSemantics) {
    const Strings s(table(3, {std::nullopt, "Complete", "Gr\xC3\xBC\xC3\x9F" "e", "\xC0\x80", ""}));
    EXPECT_EQ(s.size(), 5u);
    EXPECT_EQ(s.headerLanguage(), 3u);
    EXPECT_EQ(s.get(0), "");
    EXPECT_EQ(s.get(1), "Complete");
    EXPECT_EQ(s.get(2), "Gr\xC3\xBC\xC3\x9F" "e");
    EXPECT_EQ(s.get(3), "Invalid UTF8 encoding");
    EXPECT_EQ(s.get(4), "");
    EXPECT_EQ(s.get(5), "ERROR OUT OF TABLE RANGE");
    EXPECT_EQ(s.get(-1), "ERROR OUT OF TABLE RANGE");
    EXPECT_FALSE(s.valid(0));
    EXPECT_TRUE(s.valid(2));
    EXPECT_FALSE(s.valid(3));
}

TEST(Strings, RejectsTruncation) {
    EXPECT_THROW(Strings(std::vector<uint8_t>{0, 0, 0, 0, 1, 0}), FormatError); // header
    auto b = table(0, {"abc", "de"});
    EXPECT_THROW(Strings(std::vector<uint8_t>(b.begin(), b.begin() + 12)), FormatError); // offset table
    EXPECT_THROW(Strings(std::vector<uint8_t>(b.begin(), b.end() - 1)), FormatError);    // last NUL missing
    auto past = b;
    set32(past, 12, uint32_t(past.size()));
    EXPECT_THROW(Strings(std::move(past)), FormatError); // offset at end of file
    auto huge = b;
    set32(huge, 4, 0x40000000);
    EXPECT_THROW(Strings(std::move(huge)), FormatError);
    EXPECT_NO_THROW(Strings(std::move(b)));
}

TEST(Strings, ExeUtf8Check) {
    EXPECT_TRUE(exeUtf8Valid("plain"));
    EXPECT_TRUE(exeUtf8Valid("\xC3\xA9\xE2\x80\xA6\xF0\x9F\x98\x80"));
    EXPECT_FALSE(exeUtf8Valid("\xC3"));         // truncated sequence
    EXPECT_FALSE(exeUtf8Valid("\xE2\x80" "a")); // continuation without top bit
    EXPECT_FALSE(exeUtf8Valid("\xC1\xBF"));     // forbidden lead
    EXPECT_FALSE(exeUtf8Valid("\xF5\x80\x80\x80"));
    EXPECT_FALSE(exeUtf8Valid("\x80"));          // stray continuation
    EXPECT_TRUE(exeUtf8Valid("\xC3\xC3"));       // the exe only tests the top bit of continuations
}

TEST(Language, ParsesCodesNamesAndIndices) {
    EXPECT_EQ(parseLanguage("en"), Language::English);
    EXPECT_EQ(parseLanguage("german"), Language::German);
    EXPECT_EQ(parseLanguage("lang_dutch"), Language::Dutch);
    EXPECT_EQ(parseLanguage("3"), Language::Italian);
    EXPECT_FALSE(parseLanguage("13"));
    EXPECT_FALSE(parseLanguage("klingon"));
    EXPECT_EQ(specMask(Language::Dutch), 1u << 11);
    EXPECT_EQ(languageName(Language::Portuguese), "lang_portugese"); // the exe's spelling
}

TEST(MovieSchema, ParsesTimedCuesPerLanguage) {
    const std::string sch = "Version 1\r\nTextEntry\r\nline0\r\n"
                            "lang_english 2008-08-25 10:59:38 \"A\\(1.5)B\\nC\\(3)D \\\"q\\\"\"\r\n"
                            "LANG_FRENCH 2008 \"X\"\r\n";
    const auto en = parseMovieSchema(sch, Language::English);
    ASSERT_EQ(en.size(), 3u);
    EXPECT_EQ(en[0].start, 0);
    EXPECT_EQ(en[0].text, "A");
    EXPECT_DOUBLE_EQ(en[1].start, 1.5);
    EXPECT_EQ(en[1].text, "B\nC");
    EXPECT_EQ(en[2].start, 3);
    EXPECT_EQ(en[2].text, "D \"q\"");
    const auto fr = parseMovieSchema(sch, Language::French); // key match is case-insensitive
    ASSERT_EQ(fr.size(), 1u);
    EXPECT_EQ(fr[0].text, "X");
    EXPECT_TRUE(parseMovieSchema(sch, Language::German).empty());
}

TEST(Subtitles, TickerScrollsLikeTheExe) {
    MovieSubtitles m({{0, "A"}, {1, "B\nC"}});
    m.update(0, 0);
    EXPECT_EQ(m.ticker().top(), "");
    EXPECT_EQ(m.ticker().bottom(), "A");
    m.update(0.5, 10);
    EXPECT_EQ(m.ticker().bottom(), "A");
    m.update(1.0, 1);
    EXPECT_EQ(m.ticker().top(), "B");
    EXPECT_EQ(m.ticker().bottom(), "C");
    m.update(2.0, 90); // 90 ticks: bottom line moves up
    EXPECT_EQ(m.ticker().top(), "C");
    EXPECT_EQ(m.ticker().bottom(), "");
    m.update(3.0, 90);
    EXPECT_EQ(m.ticker().top(), "");
}

TEST(Subtitles, CineLinesDropSpeaker) {
    EXPECT_EQ(cineSubtitleLines("Lara: Hello\n\nAmanda: Hi: there"), (std::vector<std::string>{"Hello", "Hi: there"}));
    EXPECT_EQ(cineSubtitleLines("Odd: "), (std::vector<std::string>{"Odd: "}));
}

namespace {

// CINE stream: 1 "A" entry (9 channels), 1 instance with 1 bone (17) and 2 extra channels, 2 frames.
std::vector<uint8_t> syntheticCine(int frames, std::string_view subtitle) {
    std::vector<uint8_t> b;
    putStr(b, "ENIC");
    put32(b, 9);
    put32(b, 0); // header size, patched below
    std::string name = "l1_test";
    name.resize(64);
    putStr(b, name);
    put32(b, 0x2C);
    put32(b, 1);
    b.resize(b.size() + 12);
    put32(b, 1);                                  // instances
    put32(b, 7); put32(b, ~0u); put32(b, 1); put32(b, 0);
    b.resize(b.size() + 64);                      // bone matrix
    put32(b, 0); put32(b, 0);                     // nC
    put32(b, 2); put32(b, 0);                     // nD channels
    put32(b, 0);                                  // nE
    put32(b, 0); put32(b, 0);                     // nF
    put32(b, 0);
    b.resize(b.size() + 28);                      // instance dwords
    set32(b, 8, uint32_t(b.size() - 8));
    const int channels = 9 + 14 + 3 + 2;
    for (int f = 0; f < frames; ++f) {
        const size_t start = b.size();
        put32(b, 0);
        put32(b, uint32_t(f));
        for (int c = 0; c < channels; ++c) {
            const bool animated = c % 2;
            if (f == 0) put32(b, uint32_t(frames) | (animated ? 0x10000000u : 0));
            if (f == 0 || animated) put32(b, 0x3F800000);
        }
        const std::string_view sub = f == 0 ? subtitle : "";
        put32(b, uint32_t(sub.size()));
        putStr(b, sub);
        b.resize((b.size() + 15) / 16 * 16);
        set32(b, start, uint32_t(b.size() - start - 4));
    }
    put32(b, 0);
    put32(b, ~0u);
    return b;
}

} // namespace

TEST(Cine, ParsesSyntheticStreamThroughMul) {
    const auto cine = syntheticCine(2, "0\rLara: Hello\r1\rBonjour\r");
    // .mul: 2048-byte header, cinematic stream split over two type-1 packets around a sound packet.
    std::vector<uint8_t> mul(2048);
    auto packet = [&](uint32_t type, std::span<const uint8_t> data) {
        put32(mul, type);
        put32(mul, uint32_t(data.size()));
        put32(mul, 0);
        put32(mul, 0);
        mul.insert(mul.end(), data.begin(), data.end());
    };
    packet(1, std::span(cine).first(100));
    packet(0, std::vector<uint8_t>(48, 0x55));
    packet(1, std::span(cine).subspan(100));
    EXPECT_EQ(mulCinematicStream(mul), cine);

    const CineInfo info = parseCineStream(cine);
    EXPECT_EQ(info.unitName, "l1_test");
    EXPECT_EQ(info.channels, 28);
    EXPECT_EQ(info.frames, 2);
    ASSERT_EQ(info.subtitles.size(), 2u);
    EXPECT_EQ(info.subtitles[0].frame, 0);
    EXPECT_EQ(info.subtitles[0].language, 0);
    EXPECT_EQ(info.subtitles[0].text, "Lara: Hello");
    EXPECT_EQ(info.subtitles[1].language, 1);
    EXPECT_EQ(info.subtitles[1].text, "Bonjour");

    EXPECT_THROW(parseCineStream(std::span(cine).first(cine.size() - 6)), FormatError);
    auto bad = cine;
    bad[0] = 'X';
    EXPECT_THROW(parseCineStream(bad), FormatError);
    mul.resize(mul.size() - 1);
    EXPECT_THROW(mulCinematicStream(mul), FormatError);
}

// ---- Real data (skipped without OU_GAME_DIR) -------------------------------------------

TEST(LocaleRealData, AllLocalsBinVariantsDecode) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    struct Want { Language lang; uint32_t header; const char* first; };
    const Want want[] = {{Language::English, 0, "Complete"}, {Language::French, 1, "Termin\xC3\xA9"},
                         {Language::German, 3, "Erledigt"},  {Language::Italian, 2, "Completato"},
                         {Language::Spanish, 4, "Finalizado"}, {Language::Dutch, 5, "Voltooid"}};
    for (const Want& w : want) {
        SCOPED_TRACE(std::string(languageName(w.lang)));
        const auto s = loadStrings(gameDir(), w.lang);
        ASSERT_TRUE(s);
        EXPECT_EQ(s->size(), 5136u);
        EXPECT_EQ(s->headerLanguage(), w.header);
        EXPECT_EQ(s->get(1), w.first);
        size_t nonEmpty = 0;
        for (size_t i = 0; i < s->size(); ++i) {
            const std::string_view t = s->get(int(i));
            nonEmpty += !t.empty();
            EXPECT_TRUE(strictUtf8(t)) << "string " << i;
            EXPECT_NE(t, "Invalid UTF8 encoding") << "string " << i;
        }
        EXPECT_EQ(nonEmpty, 1448u); // 3688 offsets are 0 in every variant
    }
}

TEST(LocaleRealData, PrevontrMovieSubtitles) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto sch = ou::cdc::BigfileSet(gameDir()).read(movieSchemaPath("prevontr"));
    ASSERT_TRUE(sch);
    const std::string_view text(reinterpret_cast<const char*>(sch->data()), sch->size());
    const auto en = parseMovieSchema(text, Language::English);
    ASSERT_EQ(en.size(), 21u); // text before the first marker + 20 "\(t)" markers
    EXPECT_EQ(en[0].start, 0);
    EXPECT_EQ(en[0].text, "You never have to be cold, my Lara, if you don't want to be.");
    EXPECT_DOUBLE_EQ(en[1].start, 6);
    EXPECT_EQ(en.back().text, "AVALON! She's not dead!");
    for (int l = 0; l < kLanguageCount; ++l) {
        const Language lang = Language(l);
        const bool present = lang != Language::Japanese && lang != Language::Portuguese && lang != Language::UkEnglish;
        EXPECT_EQ(!parseMovieSchema(text, lang).empty(), present) << languageName(lang);
    }
}

TEST(LocaleRealData, CinematicSubtitles) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto mul = ou::cdc::BigfileSet(gameDir()).read("pc-w\\cinstream\\060_med_natla.mul");
    ASSERT_TRUE(mul);
    const CineInfo info = parseCineStream(mulCinematicStream(*mul));
    EXPECT_EQ(info.unitName, "l1_ship_natla_room");
    EXPECT_EQ(info.frames, 3181);
    EXPECT_EQ(info.channels, 6933);
    size_t english = 0;
    for (const auto& s : info.subtitles) {
        EXPECT_TRUE(strictUtf8(s.text));
        if (s.language == 0 && english++ == 0) {
            EXPECT_EQ(s.frame, 109);
            EXPECT_EQ(s.text, "Damn, it's too small...");
        }
    }
    EXPECT_GT(english, 10u);
}

TEST(VideoRealData, DecodesBinkVideoAndAudio) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto dir = std::filesystem::path(gameDir()) / "Movies";
    ou::video::Movie m(dir / "explode.bik");
    const auto& i = m.info();
    EXPECT_EQ(i.format, "bink");
    EXPECT_EQ(i.videoCodec, "binkvideo");
    EXPECT_EQ(i.width, 1280);
    EXPECT_EQ(i.height, 720);
    EXPECT_NEAR(i.fps, 29.97, 0.01);
    ASSERT_NE(m.audioTrack(), nullptr);
    EXPECT_EQ(m.audioTrack()->sampleRate, 44100);
    EXPECT_EQ(m.audioTrack()->channels, 2);

    ou::video::VideoFrame f;
    std::vector<float> audio;
    double lastPts = -1;
    int64_t frames = 0;
    uint64_t lumaSum = 0;
    while (m.next(f, audio)) {
        EXPECT_EQ(f.index, frames++);
        EXPECT_GT(f.pts, lastPts);
        lastPts = f.pts;
        ASSERT_EQ(f.rgba.size(), size_t(1280) * 720 * 4);
        if (f.index == 150)
            for (size_t p = 0; p < f.rgba.size(); p += 4) lumaSum += f.rgba[p] + f.rgba[p + 1] + f.rgba[p + 2];
    }
    const double videoSeconds = double(frames) / i.fps;
    const double audioSeconds = double(audio.size()) / 2 / 44100;
    EXPECT_NEAR(videoSeconds, i.duration, 0.05);
    EXPECT_NEAR(audioSeconds, videoSeconds, 0.25);
    EXPECT_GT(lumaSum, 0u); // frame 150 is not black
    for (float s : audio) ASSERT_TRUE(std::isfinite(s) && std::abs(s) <= 1.5f);
}

TEST(VideoRealData, SelectsAudioTrackByLanguage) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto dir = std::filesystem::path(gameDir()) / "Movies";
    ou::video::Movie prev(dir / "prevontr.bik", static_cast<int>(Language::German));
    ASSERT_EQ(prev.info().audio.size(), 5u);
    for (int t = 0; t < 5; ++t) EXPECT_EQ(prev.info().audio[t].id, t);
    ASSERT_NE(prev.audioTrack(), nullptr);
    EXPECT_EQ(prev.audioTrack()->id, 2);
    // intro.bik has a single track (id 0): other languages fall back to it, as tru.exe does.
    ou::video::Movie intro(dir / "intro.bik", static_cast<int>(Language::Dutch));
    ASSERT_NE(intro.audioTrack(), nullptr);
    EXPECT_EQ(intro.audioTrack()->id, 0);
    EXPECT_EQ(intro.audioTrack()->sampleRate, 48000);
    ou::video::Movie silent(dir / "intro.bik", -1);
    EXPECT_EQ(silent.audioTrack(), nullptr);
}
