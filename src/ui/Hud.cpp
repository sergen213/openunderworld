// In-game HUD as quads (ui::Hud): the original display groups (HEALTH, WEAPONS, GEAR) drawn with generalbank art
// at the layout measured from tru.exe captures, combat reticles, notifications, title cards and subtitles in the
// game font. Layout, sources and [I] choices: docs/GAMEPLAY.md "HUD".
#include "ui/Ui.hpp"

#include "ui/Art.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ou::ui {

namespace {

constexpr float kArt = 2.0f / 3;  // HUD art: virtual units per texel
constexpr uint32_t kDim = 0xFF4D4D4Du; // d-pad items not in use [V fit: ~0.3 of the art]
constexpr float kReticle = 0.35f; // reticles [I]: not captured

uint32_t fade(uint32_t col, float a) {
    return (col & 0x00FFFFFFu) | (uint32_t(float(col >> 24) * std::clamp(a, 0.0f, 1.0f) + 0.5f) << 24);
}

// Texture `id` with its top-left (or centre) at HUD (x, y) from the pixel origin (ox, oy); `scale`: units per texel.
void put(Canvas& c, float ox, float oy, uint32_t id, float x, float y, uint32_t col, float scale = kArt, bool centred = false) {
    const AtlasRect* r = c.art ? c.art->find(id) : nullptr;
    if (!r) return;
    const float u = c.unit(), w = float(r->width) * scale * u, h = float(r->height) * scale * u;
    float px = ox + x * u, py = oy + y * u;
    if (centred) px -= w / 2, py -= h / 2;
    c.image(0, px, py, w, h, r->u0, r->v0, r->u1, r->v1, col);
}

// Game-font text centred on pixel x, top at pixel y.
void fontCentred(Canvas& c, float px, float py, float scale, std::string_view text, uint32_t col) {
    const float u = c.unit();
    c.fontText((px - c.sx(0)) / u - c.fontWidth(text, scale) / 2, py / u, scale, text, col);
}

// Pip art per weapon (game::Weapon order): full, spent, next round; per-pip step and row tops (units).
struct Pips {
    uint32_t icon, full, empty, next;
    float x, step, rows[2];
};
// Icons 96 handgun, 98 uzi, 99 shotgun, 102 speargun, 97 Mjolnir (HUD script weapon list: wp_handgun, wp_thorhammer 97,
// wp_uzi, wp_shotgun, wp_assault_rifle 100, wp_tranq 101, wp_speargun). [V fits of hud_weapon_1080 / _768] bullets 83-85,
// 12 per row from x 63.6 every 6, rows at 36.0 / 43.1; shells 86-88 from (66.8, 36.6) every 11.7. Spears 89/90 [I].
// Mjolnir has no clip (S+0x0B = -1; tru.exe 0x6E8370 sets its HUD clip fraction 0): the icon alone.
constexpr Pips kPips[5] = {{96, 83, 84, 85, 63.6f, 6.0f, {36.0f, 43.1f}},
                           {98, 83, 84, 85, 63.6f, 6.0f, {36.0f, 43.1f}},
                           {99, 86, 87, 88, 66.8f, 11.7f, {36.6f, 36.6f}},
                           {102, 89, 90, 89, 63.6f, 30.0f, {37.0f, 37.0f}},
                           {97, 0, 0, 0, 0, 0, {0, 0}}};

} // namespace

uint32_t Hud::healthColor(float h) {
    // HUD script: four "Player health is between" branches set the meter colour (r, g, b) as 0/1 constants and one
    // variable channel each [V bytecode]: 2000-1500 (0, v, 1), 1500-1000 (0, 1, 0), 1000-500 (v, 1, 0), 500-0 (1, 0, 0).
    // [I] v runs linearly over the band so that 1000-500 turns green into yellow and 2000-1500 cyan into blue.
    auto rgb = [](float r, float g, float b) {
        auto q = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
        return 0xFF000000u | q(b) << 16 | q(g) << 8 | q(r);
    };
    if (h > 1500) return rgb(0, (2000 - h) / 500, 1);
    if (h > 1000) return rgb(0, 1, 0);
    if (h > 500) return rgb((1000 - h) / 500, 1, 0);
    return rgb(1, 0, 0);
}

void Hud::update(const HudState& s, double dt) {
    prev_ = first_ ? s : s_;
    s_ = s;
    first_ = false;
    // Triggers [I timing; the HUD script plays per-group fade anims]: HEALTH on damage / healing / adrenaline / breath
    // and while a weapon is out; WEAPONS while drawn, on firing / reloading / switching; GEAR on a d-pad press (drawing
    // counts: the hud_weapon capture shows the d-pad with the weapon item lit). The pressed item stays lit [V captures].
    constexpr float kHold = 3.0f, kIn = 4.0f, kOut = 2.0f; // seconds held after the trigger; fade rates per second
    const int item = s.healthPacks != prev_.healthPacks ? 0 : s.flashlight != prev_.flashlight ? 1 : s.grenades < prev_.grenades ? 3 : s.gearItem;
    const bool trig[kGroups] = {
        s.drawn || s.health != prev_.health || s.breath < 1 || std::abs(s.adrenaline - prev_.adrenaline) > 1e-4f,
        s.drawn || s.weapon != prev_.weapon || s.clip[0] != prev_.clip[0] || s.clip[1] != prev_.clip[1] || s.grenades != prev_.grenades,
        item >= 0};
    if (item >= 0) lit_ = item;
    for (int g = 0; g < kGroups; ++g) {
        if (s.dead) hold_[g] = 0;
        else if (trig[g]) hold_[g] = kHold;
        else hold_[g] = std::max(0.0f, hold_[g] - float(dt));
        alpha_[g] = hold_[g] > 0 ? std::min(1.0f, alpha_[g] + kIn * float(dt)) : std::max(0.0f, alpha_[g] - kOut * float(dt));
    }
    if (alpha_[Gear] <= 0) lit_ = -1;
}

void Hud::subtitles(Canvas& c, std::string_view top, std::string_view bottom) {
    const std::string_view lines[2] = {top, bottom};
    for (int k = 0; k < 2; ++k) {
        if (lines[k].empty()) continue;
        const float y = 352 + 22 * float(k), x = 320 - c.fontWidth(lines[k], 0.6f) / 2;
        c.fontText(x + 1, y + 1, 0.6f, lines[k], 0xC0000000u); // [I] drop shadow
        c.fontText(x, y, 0.6f, lines[k], kWhite);
    }
}

void Hud::draw(Canvas& c) const {
    const float u = c.unit(), cx = float(c.width) / 2;
    const float ox = originX(c.width), oy = originY(c.height, false), by = originY(c.height, true);

    if (const float a = alpha_[Health]; a > 0) { // HEALTH: panel, adrenaline/breath bar, health meter, silhouette
        put(c, ox, oy, 78, -5.3f, -3.1f, fade(kWhite, a));
        const float bar = std::clamp(s_.breath < 1 ? s_.breath : s_.adrenaline, 0.0f, 1.0f); // "adrenaline bar as breathbar"
        const float b0 = 16.0f, b1 = 60.0f; // [V] green fill rows, under bar 77's cap
        c.rect(ox + 14.9f * u, oy + (b1 - (b1 - b0) * bar) * u, 7.4f * u, (b1 - b0) * bar * u, fade(0xFF00FF00u, a));
        put(c, ox, oy, 77, 9.3f, 16.4f, fade(kWhite, a));
        const float m0 = -2.7f + 25 * kArt, m1 = -2.7f + 102 * kArt; // inside mask 76's opaque rows 24..102
        const float hp = std::clamp(s_.health / std::max(1.0f, s_.healthMax), 0.0f, 1.0f);
        c.rect(ox + 35.1f * u, oy + (m1 - (m1 - m0) * hp) * u, 32 * kArt * u, (m1 - m0) * hp * u, fade(0xFF00FFFFu, a));
        put(c, ox, oy, 76, 35.1f, -2.7f, fade(kWhite, a));
        put(c, ox, oy, 75, 9.8f, -2.7f, fade(healthColor(s_.health), a));
    }
    if (const float a = alpha_[Weapons]; a > 0 && s_.weapon >= 0 && s_.weapon < 5) { // WEAPONS: icon + clip pips
        const Pips& p = kPips[s_.weapon];
        put(c, ox, oy, p.icon, 59.1f, -4.9f, fade(kWhite, a), 0.65f); // [V fit: pistols icon]
        const int guns = s_.dual ? 2 : 1;
        // [I] clips longer than 12 rounds (uzi 40) squeeze into the 12-pip row width.
        const float step = s_.clipSize > 12 && p.step < 10 ? p.step * 12 / float(s_.clipSize) : p.step;
        for (int g = 0; g < guns; ++g)
            for (int i = 0; i < s_.clipSize; ++i) {
                const bool loaded = i < s_.clip[g];
                // [V captures] one gun's last loaded round is white (at rest: the second row) [I: the next hand's].
                const int row = guns == 2 ? 1 - s_.nextHand % 2 : 0;
                const uint32_t id = !loaded ? p.empty : i == s_.clip[g] - 1 && g == row ? p.next : p.full;
                put(c, ox, oy, id, p.x + float(i) * step, p.rows[g], fade(kWhite, a));
            }
        // [O position: the original's grenade row, 81 full / 82 empty at y 52] one pip per carried maximum, x step [I]
        for (int i = 0; i < s_.grenadesMax; ++i) put(c, ox, oy, i < s_.grenades ? 81u : 82u, 63.6f + 9.0f * float(i), 52.0f, fade(kWhite, a));
    }
    if (const float a = alpha_[Gear]; a > 0) { // GEAR d-pad, from (5 % W, 95 % H)
        // [V fits of hud_dpad_1080] items at 2/3 unit per texel around (104, -73), dimmed except the pressed one.
        const float gx = 104.0f, gy = -73.0f;
        auto col = [&](int item) { return fade(item == lit_ ? kWhite : kDim, a); };
        put(c, ox, by, 91, gx, gy, fade(0x80FFFFFFu, a), kArt, true);
        put(c, ox, by, 92, gx, gy - 33.5f, col(0), kArt, true);         // health packs + count box
        put(c, ox, by, 93, gx - 32.9f, gy + 1.0f, col(1), kArt, true);  // flashlight
        put(c, ox, by, 94, gx + 32.4f, gy - 0.8f, col(2), kArt, true);  // weapon switch
        put(c, ox, by, 95, gx - 0.4f, gy + 34.8f, col(3), kArt, true);  // grenades
        fontCentred(c, ox + (gx + 10.3f) * u, by + (gy - 33.5f + 6.0f - 4.0f) * u, 0.55f, std::to_string(s_.healthPacks), fade(kWhite, a));
    }

    // Reticles [I]: not captured. Free aim 69 (thin arcs), soft target 65 (white brackets), hard lock 72 (red
    // cross), adrenaline window 71 (red glow) with the pip 74.
    if (reticle.kind != Reticle::Kind::None) {
        static constexpr uint32_t kId[] = {0, 69, 65, 72};
        const uint32_t id = reticle.slowMo ? 71 : kId[size_t(reticle.kind)];
        put(c, reticle.x, reticle.y, id, 0, 0, reticle.kind == Reticle::Kind::Free ? 0xC0FFFFFFu : kWhite, kReticle, true);
        if (reticle.slowMo) {
            const float r = 64 * kReticle * u * 0.5f; // reticle radius in pixels
            put(c, reticle.x + reticle.pipX * r, reticle.y - reticle.pipY * r, 74, 0, 0, kWhite, kReticle * 0.3f, true);
        }
    }
    if (grapple) put(c, (*grapple)[0], (*grapple)[1], 70, 0, 0, 0xE0FFFFFFu, kReticle * 0.6f, true); // [I] grapple target

    auto fadeOut = [](double left) { return float(std::clamp(left / 0.5, 0.0, 1.0)); }; // last half second
    if (titleLeft_ > 0) { // [O loading screen] chapter title at font scale ~1.2 around y 220, description ~0.45 below
        const float a = fadeOut(titleLeft_);
        c.rect(0, 200 * u, float(c.width), 80 * u, fade(kShade, a));
        fontCentred(c, cx, 208 * u, 1.2f, title_, fade(kWhite, a));
        if (!subtitle_.empty()) fontCentred(c, cx, 250 * u, 0.45f, subtitle_, fade(kWhite, a));
    }
    if (toastLeft_ > 0) { // [I] pickup / save notifications: not captured
        const float a = fadeOut(toastLeft_);
        const float w = c.fontWidth(toast_, 0.55f) * u + 24 * u;
        c.rect(cx - w / 2, 398 * u, w, 22 * u, fade(kShade, a));
        fontCentred(c, cx, 401 * u, 0.55f, toast_, fade(kWhite, a));
    }
}

} // namespace ou::ui
