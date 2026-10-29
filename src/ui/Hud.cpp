// In-game HUD as quads (ui::Hud). Layout: docs/GAMEPLAY.md "Menus".
#include "ui/Ui.hpp"

#include <algorithm>

namespace ou::ui {

void Hud::draw(Canvas& c) const {
    const float u = c.unit(), cx = float(c.width) / 2;
    auto fade = [](double left) { return float(std::clamp(left / 0.5, 0.0, 1.0)); }; // last half second fades out
    auto alpha = [](uint32_t col, float a) { return (col & 0x00FFFFFFu) | (uint32_t(float(col >> 24) * a) << 24); };
    if (titleLeft_ > 0) {
        const float a = fade(titleLeft_), g = std::round(18 * u);
        c.rect(0, 200 * u, float(c.width), 80 * u, alpha(kShade, a));
        c.textCentered(cx, 214 * u, g, title_, alpha(kGold, a));
        if (!subtitle_.empty()) c.textCentered(cx, 250 * u, std::round(9 * u), subtitle_, alpha(kWhite, a));
    }
    if (toastLeft_ > 0) {
        const float a = fade(toastLeft_), g = std::round(10 * u);
        const float w = Canvas::textWidth(toast_, g) + 24 * u;
        c.rect(cx - w / 2, 400 * u, w, g + 16 * u, alpha(kShade, a));
        c.textCentered(cx, 408 * u, g, toast_, alpha(kWhite, a));
    }
}

} // namespace ou::ui
