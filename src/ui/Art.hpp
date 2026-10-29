// Original front-end art from pc-w\generalbank.drm (plus the PDA windows and icons 680..706 of pc-w\g_pda.drm): every
// texture section packed into one RGBA atlas (render::FrameOptions::uiImages[0]) and the game font (DTP section 46:
// glyph table over textures 109 and 110). Full-screen cdc RAW images (pc-w\image\*.raw: loading and PDA backgrounds)
// load separately. Layout and metrics: docs/GAMEPLAY.md "Menus".
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ou::cdc { class BigfileSet; struct Drm; }

namespace ou::ui {

// A texture inside the atlas: normalised atlas uv and its size in texels.
struct AtlasRect {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    int width = 0, height = 0;
};

// Font DTP (tru.exe Font_GetTextWidth 0x4758C0, glyph lookup 0x475350): header {u32 texture, u32 texture2,
// u32 cellHeight, u32 count, glyph*, i16 spacing, i16, f32 scaleX, f32 scaleY}; glyph {u32 code, u8 width, u8,
// u16 x, u16 y, u8, u8}. Codes U+1800..U+18FF (controller buttons) are cut from the second texture.
// A glyph advances (spacing + width) * scaleX - 1 at text scale 1; missing codes draw U+18AF.
struct Glyph {
    uint8_t width = 0;
    uint16_t x = 0, y = 0;
    bool second = false; // on `texture2`
};
struct Font {
    uint32_t texture = 0, texture2 = 0;
    int cellHeight = 0, spacing = 0;
    float scaleX = 1, scaleY = 1;
    std::unordered_map<uint32_t, Glyph> glyphs;
    const Glyph* find(uint32_t code) const; // falls back to U+18AF, then nullptr
};

struct Art {
    std::vector<uint8_t> rgba; // atlas, RGBA8 top row first
    int width = 0, height = 0;
    uint64_t version = 1;      // render::RgbaImage::version
    std::unordered_map<uint32_t, AtlasRect> textures; // by texture section id
    Font font;
    const AtlasRect* find(uint32_t id) const;
};

// Parses the font DTP section (exposed for tests).
std::optional<Font> parseFont(const cdc::Drm& drm, uint32_t dtpId, std::string* why = nullptr);
// Loads generalbank.drm: nullopt (with `why`) when it is missing or its font does not parse.
std::optional<Art> loadArt(const cdc::BigfileSet& bigfile, std::string* why = nullptr);

// A cdc RAW image, pc-w\image\<name>.raw ("!WAR" header, B,G,R,A pixels) as opaque RGBA8; nullopt if missing/bad.
struct Image {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
};
std::optional<Image> loadRaw(const cdc::BigfileSet& bigfile, std::string_view name);

// PDA sonar map of a unit: pc-w\<unit>_map.drm, two pdamap sections. Section 0 +0 -> the map header in section 1:
// bbox min, max (vec4 each), +0x30 vertices* (vec4, unit frame), +0x34 triangles* ({u32 a, b, c, u32 flags, u32}),
// +0x38 kd nodes* (16 bytes), +0x40 u32 node / triangle / vertex counts [O l1_seafloor_map]. Section 0 also lists the
// neighbouring units (not read). nullopt: the unit has no map.
struct SonarMap {
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
    std::array<float, 3> min{}, max{};
};
std::optional<SonarMap> loadSonarMap(const cdc::BigfileSet& bigfile, std::string_view unit);

// EXTRAS rewards: pc-w\generalbank.drm DTP 36, a header of {u32 count, record*} lists [O]: 3 cinematics, 4 storyboards,
// 5 character concepts, 6 environment concepts, 7 gear concepts (0x1C-byte records {u32 locals id, i32 unlock, u32,
// char[] path*, char* image, char[]*, u32}), 9 trailers & credits, 8 outfits (not read). `unlock` codes [O values, I
// meaning from the shipped game's rewards]: 3 Mediterranean Sea complete, 4 prologue, 5 Thailand, 6 Croft Manor, 7 Mexico,
// 8 Jan Mayen, 9 Andaman Sea, 10 Arctic Sea (= chapter + 2, the prologue 4), 11 the game on Master Survivalist, 12 every
// treasure, 13 every relic.
// List 9 (trailers & credits) has 0x18-byte records {u32 locals id, i32 unlock (-1: always), char* movie (Movies\<movie>),
// u32, char* image, u32} [O: 360 prevontr.bik / previously, 361 intro.bik / teaser, 362 credits.bik / credits].
struct Reward {
    int16_t name = 0;  // locals.bin
    int unlock = 0;
    std::string image{}; // pc-w\image\<image>.raw
    std::string movie{}; // trailers & credits: the Movies\ file
};
struct Rewards {
    std::vector<Reward> environment, character, gear, storyboards, trailers; // EXTRAS 146..149, 151 in that order
};
std::optional<Rewards> loadRewards(const cdc::BigfileSet& bigfile);

// UTF-8 to code points; invalid bytes become U+FFFD.
std::u32string decodeUtf8(std::string_view s);

} // namespace ou::ui
