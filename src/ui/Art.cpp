#include "ui/Art.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

#include <algorithm>
#include <cstring>

namespace ou::ui {

namespace {

constexpr uint32_t kFontDtp = 46;     // generalbank.drm DTP section of the game font
constexpr uint32_t kMissingGlyph = 0x18AF;
constexpr int kAtlasWidth = 2048, kGutter = 2; // gutter: edge texels repeated, so bilinear taps never cross textures

template <class T> T rd(std::span<const uint8_t> d, size_t at) {
    T v;
    std::memcpy(&v, d.data() + at, sizeof v);
    return v;
}

} // namespace

const Glyph* Font::find(uint32_t code) const {
    if (code == 0xA0) code = ' ';
    if (auto it = glyphs.find(code); it != glyphs.end()) return &it->second;
    if (auto it = glyphs.find(kMissingGlyph); it != glyphs.end()) return &it->second;
    return nullptr;
}

const AtlasRect* Art::find(uint32_t id) const {
    const auto it = textures.find(id);
    return it == textures.end() ? nullptr : &it->second;
}

std::optional<Font> parseFont(const cdc::Drm& drm, uint32_t dtpId, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return std::optional<Font>(); };
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::DtpData || s.id != dtpId) continue;
        const auto d = s.data;
        if (d.size() < 32) return fail("font DTP too short");
        Font f;
        f.texture = rd<uint32_t>(d, 0), f.texture2 = rd<uint32_t>(d, 4);
        f.cellHeight = int(rd<uint32_t>(d, 8));
        const uint32_t count = rd<uint32_t>(d, 12);
        f.spacing = rd<int16_t>(d, 20);
        f.scaleX = rd<float>(d, 24), f.scaleY = rd<float>(d, 28);
        const auto table = cdc::internalPointer(s, 16);
        if (!table) return fail("font glyph table not relocated");
        // The last record's trailing pad byte is not stored, so 11 bytes suffice for it.
        if (count == 0 || *table + size_t(count - 1) * 12 + 11 > d.size()) return fail("font glyph table truncated");
        for (uint32_t i = 0; i < count; ++i) {
            const size_t at = *table + size_t(i) * 12;
            const uint32_t code = rd<uint32_t>(d, at);
            f.glyphs[code] = {d[at + 4], rd<uint16_t>(d, at + 6), rd<uint16_t>(d, at + 8), code >= 0x1800 && code < 0x1900};
        }
        return f;
    }
    return fail("font DTP section not found");
}

std::optional<Art> loadArt(const cdc::BigfileSet& bigfile, std::string* why) {
    auto bytes = bigfile.read("pc-w\\generalbank.drm");
    if (!bytes) {
        if (why) *why = "pc-w\\generalbank.drm not in bigfile";
        return std::nullopt;
    }
    const cdc::Drm drm = cdc::parseDrm(std::move(*bytes));
    Art art;
    auto font = parseFont(drm, kFontDtp, why);
    if (!font) return std::nullopt;
    art.font = std::move(*font);

    struct Pending { uint32_t id; int w, h; std::vector<uint8_t> rgba; };
    std::vector<Pending> list;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::Texture) continue;
        const auto t = formats::decodeTexture(s);
        if (!t || t->cube || t->mips.empty()) continue;
        const auto& m = t->mips[0];
        list.push_back({s.id, int(m.width), int(m.height), formats::toRgba8(t->format, m.width, m.height, m.data)});
    }
    std::stable_sort(list.begin(), list.end(), [](const Pending& a, const Pending& b) { return a.h > b.h; });
    // Shelf packing, tallest first.
    struct Place { int x, y; };
    std::vector<Place> at(list.size());
    int x = 0, y = 0, shelf = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const int w = list[i].w + 2 * kGutter, h = list[i].h + 2 * kGutter;
        if (x + w > kAtlasWidth) x = 0, y += shelf, shelf = 0;
        at[i] = {x + kGutter, y + kGutter};
        x += w, shelf = std::max(shelf, h);
    }
    art.width = kAtlasWidth, art.height = y + shelf;
    art.rgba.assign(size_t(art.width) * size_t(art.height) * 4, 0);
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& p = list[i];
        for (int ty = -kGutter; ty < p.h + kGutter; ++ty)
            for (int tx = -kGutter; tx < p.w + kGutter; ++tx) {
                const int sx = std::clamp(tx, 0, p.w - 1), sy = std::clamp(ty, 0, p.h - 1);
                std::memcpy(&art.rgba[(size_t(at[i].y + ty) * size_t(art.width) + size_t(at[i].x + tx)) * 4],
                            &p.rgba[(size_t(sy) * size_t(p.w) + size_t(sx)) * 4], 4);
            }
        art.textures[p.id] = {float(at[i].x) / float(art.width), float(at[i].y) / float(art.height),
                              float(at[i].x + p.w) / float(art.width), float(at[i].y + p.h) / float(art.height), p.w, p.h};
    }
    if (!art.find(art.font.texture) || !art.find(art.font.texture2)) {
        if (why) *why = "font textures missing from generalbank.drm";
        return std::nullopt;
    }
    return art;
}

std::u32string decodeUtf8(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const auto c = uint8_t(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + size_t(n) > s.size()) {
            out.push_back(0xFFFD), ++i;
            continue;
        }
        char32_t cp = n == 1 ? c : c & (0x7F >> n);
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const auto b = uint8_t(s[i + size_t(k)]);
            ok = ok && (b >> 6) == 2;
            cp = (cp << 6) | (b & 0x3F);
        }
        out.push_back(ok ? cp : 0xFFFD);
        i += ok ? size_t(n) : 1;
    }
    return out;
}

} // namespace ou::ui
