// ui::Hud: HUD space (5 % safe margin, height-relative units, 2/3 unit per texel) at several aspect ratios,
// HUD script health colour bands, display-group fades.
#include "ui/Art.hpp"
#include "ui/Ui.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <tuple>

using namespace ou;

namespace {

// Art with the HUD textures at their generalbank sizes (atlas uv unused).
ui::Art hudArt() {
    ui::Art a;
    a.width = a.height = 1;
    a.rgba.assign(4, 255);
    for (const auto& [id, w, h] : {std::tuple{78u, 512, 128}, {75u, 64, 128}, {91u, 128, 128}})
        a.textures[id] = {0, 0, 1, 1, w, h};
    return a;
}

// Screen rect (x, y, w, h) of the first image quad drawn with texture size (w, h) in units.
struct Quad { float x, y, w, h; };
Quad quad(const std::vector<render::TextVertex>& v, size_t index) {
    const auto& a = v[index * 6];
    const auto& c = v[index * 6 + 2];
    return {a.x, a.y, c.x - a.x, c.y - a.y};
}

} // namespace

TEST(Hud, SpaceKeepsTheSafeMarginAtEveryAspect) {
    const ui::Art art = hudArt();
    for (const auto& [w, h] : {std::pair{1920, 1080}, {1024, 768}, {3440, 1440}, {5120, 1440}, {1280, 1024}}) {
        SCOPED_TRACE(std::to_string(w) + "x" + std::to_string(h));
        const float u = float(h) / 480;
        EXPECT_FLOAT_EQ(ui::Hud::unit(h), u);
        EXPECT_FLOAT_EQ(ui::Hud::originX(w), 0.05f * float(w));
        EXPECT_FLOAT_EQ(ui::Hud::originY(h, false), 0.05f * float(h));
        EXPECT_FLOAT_EQ(ui::Hud::originY(h, true), 0.95f * float(h));

        ui::Hud hud;
        ui::HudState s;
        s.drawn = true, s.gearItem = 2;
        for (int i = 0; i < 30; ++i) hud.update(s, 1.0 / 60); // fully faded in
        ui::Canvas c(w, h, &art);
        hud.draw(c);
        // panel 78 first: 512 x 128 texels at 2/3 unit each, top-left (-5.3, -3.1) from the margin
        const Quad panel = quad(c.vertices, 0);
        EXPECT_NEAR(panel.x, 0.05f * float(w) - 5.3f * u, 1e-3f);
        EXPECT_NEAR(panel.y, 0.05f * float(h) - 3.1f * u, 1e-3f);
        EXPECT_NEAR(panel.w, 512 * 2.0f / 3 * u, 1e-3f);
        EXPECT_NEAR(panel.h, 128 * 2.0f / 3 * u, 1e-3f);
        // the d-pad cross 91 hangs from the bottom margin, centred (104, -73) units from (5 % W, 95 % H)
        bool found = false;
        for (size_t q = 0; q < c.vertices.size() / 6; ++q) {
            const Quad r = quad(c.vertices, q);
            if (std::abs(r.w - 128 * 2.0f / 3 * u) > 1e-3f || std::abs(r.h - r.w) > 1e-3f) continue;
            EXPECT_NEAR(r.x + r.w / 2, 0.05f * float(w) + 104 * u, 1e-3f);
            EXPECT_NEAR(r.y + r.h / 2, 0.95f * float(h) - 73 * u, 1e-3f);
            EXPECT_LE(r.y + r.h, 0.95f * float(h));
            found = true;
        }
        EXPECT_TRUE(found);
    }
}

TEST(Hud, HealthColourBands) {
    // HUD script: 2000-1500 (0, v, 1), 1500-1000 green, 1000-500 (v, 1, 0), 500-0 red; 0xAABBGGRR
    EXPECT_EQ(ui::Hud::healthColor(1000), 0xFF00FF00u); // full health on Normal: the captures' (0, 255, 0)
    EXPECT_EQ(ui::Hud::healthColor(1250), 0xFF00FF00u);
    EXPECT_EQ(ui::Hud::healthColor(1500), 0xFF00FF00u);
    EXPECT_EQ(ui::Hud::healthColor(750), 0xFF00FF80u);  // halfway to yellow
    EXPECT_EQ(ui::Hud::healthColor(500), 0xFF0000FFu);  // the red band starts at 500
    EXPECT_EQ(ui::Hud::healthColor(501), 0xFF00FFFEu);  // almost yellow at the band's end
    EXPECT_EQ(ui::Hud::healthColor(100), 0xFF0000FFu);
    EXPECT_EQ(ui::Hud::healthColor(0), 0xFF0000FFu);
    EXPECT_EQ(ui::Hud::healthColor(1750), 0xFFFF8000u); // (0, 0.5, 1)
    EXPECT_EQ(ui::Hud::healthColor(2000), 0xFFFF0000u);
}

TEST(Hud, GroupsShowOnTriggersAndFadeOut) {
    ui::Hud hud;
    ui::HudState s;
    auto run = [&](double seconds) {
        for (int i = 0; i < int(seconds * 60); ++i) hud.update(s, 1.0 / 60);
    };
    run(1);
    for (int g = 0; g < ui::Hud::kGroups; ++g) EXPECT_EQ(hud.alpha(ui::Hud::Group(g)), 0.0f); // idle: hidden
    s.health = 900; // damage: HEALTH only
    run(0.5);
    EXPECT_EQ(hud.alpha(ui::Hud::Health), 1.0f);
    EXPECT_EQ(hud.alpha(ui::Hud::Weapons), 0.0f);
    run(4); // held, then faded
    EXPECT_EQ(hud.alpha(ui::Hud::Health), 0.0f);
    s.breath = 0.5f; // under water: shown for as long as the breath bar is not full
    run(10);
    EXPECT_EQ(hud.alpha(ui::Hud::Health), 1.0f);
    s.breath = 1, s.drawn = true, s.gearItem = 2; // draw: all three
    run(0.5);
    for (int g = 0; g < ui::Hud::kGroups; ++g) EXPECT_EQ(hud.alpha(ui::Hud::Group(g)), 1.0f);
    s.gearItem = -1;
    run(5);
    EXPECT_EQ(hud.alpha(ui::Hud::Gear), 0.0f);
    EXPECT_EQ(hud.alpha(ui::Hud::Weapons), 1.0f); // while drawn
    s.dead = true;
    run(1);
    for (int g = 0; g < ui::Hud::kGroups; ++g) EXPECT_EQ(hud.alpha(ui::Hud::Group(g)), 0.0f);
}
