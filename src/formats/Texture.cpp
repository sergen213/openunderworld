#include "formats/Formats.hpp"

#include <algorithm>
#include <cstring>

namespace ou::formats {
namespace {

constexpr uint32_t kPcd9 = 0x39444350;   // "PCD9"
constexpr uint32_t kDxt1 = 0x31545844, kDxt3 = 0x33545844, kDxt5 = 0x35545844;
constexpr uint32_t kA8R8G8B8 = 0x15;    // D3DFMT_A8R8G8B8

template <class T> T rd(std::span<const uint8_t> d, size_t off) {
    T v;
    std::memcpy(&v, d.data() + off, sizeof v);
    return v;
}

uint32_t levelBytes(scene::PixelFormat f, uint32_t w, uint32_t h) {
    using enum scene::PixelFormat;
    if (f == RGBA8 || f == BGRA8) return w * h * 4;
    return ((w + 3) / 4) * ((h + 3) / 4) * (f == BC1 ? 8 : 16);
}

void rgb565(uint16_t c, uint8_t out[3]) {
    out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes a BC1 colour block into 16 RGBA pixels. `forceFour`: BC2/BC3 colour blocks
// always use the 4-colour mode.
void colourBlock(const uint8_t* b, bool forceFour, uint8_t px[16][4]) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4] = {};
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; ++k) {
        if (forceFour || c0 > c1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (!forceFour && c0 <= c1) pal[3][3] = 0;
    const uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(bits >> (2 * i)) & 3], 4);
}

} // namespace

std::optional<PcdHeader> parsePcdHeader(std::span<const uint8_t> d) {
    if (d.size() < 24 || rd<uint32_t>(d, 0) != kPcd9) return std::nullopt;
    PcdHeader h;
    h.format = rd<uint32_t>(d, 4);
    h.dataSize = rd<uint32_t>(d, 8);
    h.paletteSize = rd<uint32_t>(d, 12);
    h.width = rd<uint16_t>(d, 16);
    h.height = rd<uint16_t>(d, 18);
    h.depth = d[20];
    h.mipCount = d[21];
    h.flags = rd<uint16_t>(d, 22);
    return h;
}

std::optional<scene::TextureData> decodeTexture(const cdc::Section& s, std::string* why) {
    auto fail = [&](std::string m) -> std::optional<scene::TextureData> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    const auto h = parsePcdHeader(s.data);
    if (!h) return fail("not a PCD9 texture");
    scene::TextureData t;
    t.id = s.id;
    switch (h->format) {
    case kA8R8G8B8: t.format = scene::PixelFormat::BGRA8; break;
    case kDxt1: t.format = scene::PixelFormat::BC1; break;
    case kDxt3: t.format = scene::PixelFormat::BC2; break;
    case kDxt5: t.format = scene::PixelFormat::BC3; break;
    default: return fail("unknown PCD9 format " + std::to_string(h->format));
    }
    if (h->cube()) return fail("cube map (unsupported by scene::TextureData)");
    if (h->paletteSize) return fail("palettised texture");
    if (h->dataSize > s.data.size() - 24) return fail("pixel data exceeds section");
    size_t off = 24;
    uint32_t w = h->width, hh = h->height;
    for (int i = 0; i <= h->mipCount; ++i) {
        const uint32_t n = levelBytes(t.format, w, hh);
        if (off + n > 24 + size_t(h->dataSize)) return fail("mip chain exceeds dataSize");
        t.mips.push_back({w, hh, {s.data.begin() + off, s.data.begin() + off + n}});
        off += n;
        w = std::max(1u, w / 2);
        hh = std::max(1u, hh / 2);
    }
    if (off != 24 + size_t(h->dataSize)) return fail("mip chain does not match dataSize");
    return t;
}

std::vector<uint8_t> toRgba8(scene::PixelFormat f, uint32_t width, uint32_t height,
                             std::span<const uint8_t> data) {
    using enum scene::PixelFormat;
    std::vector<uint8_t> out(size_t(width) * height * 4);
    if (data.size() < levelBytes(f, width, height)) return {};
    if (f == RGBA8 || f == BGRA8) {
        std::memcpy(out.data(), data.data(), out.size());
        if (f == BGRA8)
            for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
        return out;
    }
    const size_t blockBytes = f == BC1 ? 8 : 16;
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data.data() + (size_t(by) * bw + bx) * blockBytes;
            uint8_t px[16][4];
            colourBlock(f == BC1 ? b : b + 8, f != BC1, px);
            if (f == BC2) {
                for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
            } else if (f == BC3) {
                uint8_t a[8] = {b[0], b[1]};
                for (int i = 2; i < 8; ++i)
                    a[i] = a[0] > a[1] ? uint8_t(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                         : i < 6       ? uint8_t(((6 - i) * a[0] + (i - 1) * a[1]) / 5)
                                       : uint8_t(i == 6 ? 0 : 255);
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < width && y < height) std::memcpy(&out[(size_t(y) * width + x) * 4], px[i], 4);
            }
        }
    return out;
}

} // namespace ou::formats
