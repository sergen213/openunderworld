// See WaterField.hpp.
#include "render/WaterField.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::render::waterfield {

namespace {
const char* kWaterFieldComp =
#include "water_field.comp.inc"
    ;
} // namespace

float omega(float k, float depth) {
    return std::sqrt((kGravity * k + kTension * k * k * k) * std::tanh(k * depth));
}

float damping(float k, float cellMetres) {
    const float q = k * cellMetres / 3.14159265f; // 1 at the grid's Nyquist wave number
    const float q2 = q * q, q4 = q2 * q2;
    return kBaseDamping + 2.0f * kViscosity * k * k + kGridDamping * q4 * q4;
}

bool wanted(const FrameOptions& o) {
    return o.water == WaterQuality::Enhanced && o.waterField.active && o.waterField.cellSize > 0.0f;
}

std::optional<Push> Stepper::frame(const FrameOptions& o, KickBuffer& kicks, FrameBuffer& bodies) {
    if (!wanted(o)) {
        active_ = false;
        return std::nullopt;
    }
    const WaterField& f = o.waterField;
    const glm::ivec2 corner(int(std::lround(f.originX / f.cellSize)), int(std::lround(f.originY / f.cellSize)));
    glm::ivec2 shift = corner - corner_;
    bool reset = !active_ || f.generation != generation_ || f.cellSize != cell_ || std::abs(shift.x) >= kN ||
                 std::abs(shift.y) >= kN;
    float dt = o.time - time_;
    if (reset) shift = glm::ivec2(0), dt = 0.0f;
    dt = std::clamp(dt, 0.0f, 1.0f / 15.0f); // a pause holds the waves; a hitch does not run them for seconds
    active_ = true, generation_ = f.generation, cell_ = f.cellSize, corner_ = corner, time_ = o.time;

    kicks.count = uint32_t(std::min(f.kicks.size(), kMaxKicks));
    std::copy_n(f.kicks.begin(), kicks.count, kicks.kicks);
    bodies.bodyCount = uint32_t(std::min(f.bodies.size(), kMaxBodies));
    std::copy_n(f.bodies.begin(), bodies.bodyCount, bodies.bodies);
    if (f.maskSize == kMaskN && f.mask.size() == size_t(kMaskN * kMaskN)) std::memcpy(bodies.mask, f.mask.data(), sizeof bodies.mask);
    else std::fill(std::begin(bodies.mask), std::end(bodies.mask), 0xFFFFFFFFu); // no mask: all open water

    Push p{};
    p.cfg = {Prep, shift.x, shift.y, reset ? kReset : 0};
    p.par = {dt, std::max(f.depth, 1.0f) / kMetre, f.cellSize / kMetre, kSteepMax};
    p.win = {f.originX, f.originY, f.cellSize, kMetre};
    p.phys = {kGravity, kTension, kSponge, kRough};
    p.damp = {kBaseDamping, kViscosity, kGridDamping, kSpongeCells};
    return p;
}

std::string computeSource() { return kWaterFieldComp; }

} // namespace ou::render::waterfield
