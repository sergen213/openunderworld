// Backend-free helpers shared by the GL and Vulkan renderers (no GPU API types here).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "render/Renderer.hpp"

namespace ou::render {

// 8x8 glyphs for ASCII 32..126 from SDL3's built-in debug font, in a 16x6-glyph R8 atlas.
constexpr int kGlyph = 8, kAtlasCols = 16, kAtlasRows = 6;
constexpr int kAtlasW = kGlyph * kAtlasCols, kAtlasH = kGlyph * kAtlasRows;
std::vector<uint8_t> buildFontAtlas();

struct TextVertex {
    float x, y, u, v; // pixels from top-left; u < 0 = solid background quad
    uint32_t color;   // RGBA8
};
// Triangle list for the diagnostics overlay (leading '!' = highlighted line).
std::vector<TextVertex> buildOverlayVertices(Viewport vp, std::span<const std::string> lines);

// Bytes per 4x4 block for BCn formats, 0 for uncompressed 4-byte-per-pixel formats.
size_t blockBytes(scene::PixelFormat format);
// Number of leading mips forming a valid chain (0 = unusable). `problem` describes any defect.
uint32_t usableMips(const scene::TextureData& t, std::string& problem);

} // namespace ou::render
