// Original front-end art from pc-w\generalbank.drm (plus the PDA picture windows 680..705 of pc-w\g_pda.drm): every
// texture section packed into one RGBA atlas (render::FrameOptions::uiImages[0]) and the game font (DTP section 46:
// glyph table over textures 109 and 110). Full-screen cdc RAW images (pc-w\image\*.raw: loading and PDA backgrounds)
// load separately. Layout and metrics: docs/GAMEPLAY.md "Menus".
#pragma once

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

// UTF-8 to code points; invalid bytes become U+FFFD.
std::u32string decodeUtf8(std::string_view s);

} // namespace ou::ui
