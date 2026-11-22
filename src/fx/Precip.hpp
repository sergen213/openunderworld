// Enhanced precipitation (docs/RENDERING.md "Weather"): rain drops and snow flakes in world space. Ours, not tru.exe's:
// the original keeps its drops and flakes in a camera-relative pool (fx::Weather, 0x5346A0 / 0x539EF0: spawned in the
// view frustum each frame and culled when they leave it), so they slide and rotate with the camera. Here every
// particle belongs to a fixed world cell (kRainCell / kSnowCell: x, y tiles and a vertical layer); its offset, phase,
// speed and size are hashes of the cell and its fall cycle, and its position is a closed form of the game time. The
// camera only picks which cells are live (those within the draw radius), so turning or moving shows true parallax and
// a cell's particles look the same however the camera reached it. Nothing is simulated per particle.
//
// Cover: a world-anchored grid of the top surface around the camera (Precip::Cover, 32-unit cells), filled by downward
// segment queries (the caller's batch: Jolt narrow phase on the level collision, physics::Scene::castSegments).
// Particles below their column's top surface are hidden (no rain under roofs); a drop that passed it within the last
// kSplashLife seconds draws a splash there (ring + droplets), from the same closed form.
//
// Snow on Lara (SnowCoat): per body region an amount that builds while she stands in snowfall and fades with her
// motion; strong motion (running, jumping, landing, climbing) sheds it as clumps that fall under gravity and settle
// where a segment query finds the ground.
#pragma once

#include "render/Renderer.hpp"
#include "scene/Assets.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace ou::fx {

// Procedural textures of the Enhanced weather (ids outside the cdc texture id range).
inline constexpr uint32_t kPrecipDropTexture = 0x7EA70001;  // rain drop: a thin bright-edged streak, alpha shape
inline constexpr uint32_t kPrecipFlakeTexture = 0x7EA70002; // snow flake / droplet: soft disc
inline constexpr uint32_t kPrecipRingTexture = 0x7EA70003;  // splash ring
std::vector<scene::TextureData> precipTextures();

class Precip {
public:
    enum class Kind : uint8_t { None, Rain, Snow };
    struct Frame {
        Kind kind = Kind::None;
        float intensity = 0;        // 0..1
        double time = 0;            // game time, s
        glm::vec3 eye{0}, forward{0, 1, 0};
        float tanHalfFovY = 0.41421f, viewportHeight = 1080;
        glm::vec2 wind{0};          // horizontal drift, units / s
        glm::vec3 light{1};         // ambient light of the scene at the eye (0..1 per channel)
    };
    // Downward segments `from` -> `to`; hits[i] = the z of the first surface (nullopt: none).
    using Query = std::function<void(std::span<const glm::vec3> from, std::span<const glm::vec3> to,
                                     std::span<std::optional<float>> hits)>;

    // Fills / refreshes the cover grid around `eye`, at most `budget` segments this call (cells whose column the grid
    // does not hold yet first, then those queried from an eye height more than kRequery away).
    void updateCover(const glm::vec3& eye, const Query& query, size_t budget);
    // Top surface z of the column at p: nullopt while not yet queried, -inf where the column holds nothing.
    std::optional<float> cover(glm::vec2 p) const;
    // Live particles as fx quads (alpha-blended drops / flakes, additive splashes), appended.
    void build(const Frame& f, std::vector<render::FxVertex>& vertices, std::vector<render::FxBatch>& batches) const;
    size_t lastCount() const { return count_; }
    void clear();

    static constexpr float kCoverCell = 32.0f;
    static constexpr int kCoverSize = 192; // cells per side (6144 units)
    static constexpr float kRequery = 1500.0f;
    static constexpr float kSplashLife = 0.28f;

private:
    struct Cell { int32_t x = INT32_MIN, y = INT32_MIN; float height = 0, fromZ = 0; };
    std::vector<Cell> cover_ = std::vector<Cell>(size_t(kCoverSize) * kCoverSize);
    mutable size_t count_ = 0;
};

// Snow on Lara (Enhanced weather): per region the coverage 0..1 the scene shaders blend in (render::SnowLayer), the
// clumps she sheds.
class SnowCoat {
public:
    static constexpr int kRegions = 4; // head + shoulders, arms, torso, legs
    struct Input {
        double time = 0;
        float dt = 0;
        float snowfall = 0;          // 0..1 at Lara (intensity x open sky above her)
        glm::vec3 position{0};       // Lara's root (feet)
        glm::vec3 velocity{0};       // units / s
        bool grounded = true;
        bool climbing = false;       // hanging / climbing / swimming: strong motion
        float height = 180;          // model height (units)
    };
    // `ground`: z of the surface below a point (segment query), nullopt when none within reach.
    using Ground = std::function<std::optional<float>(const glm::vec3&)>;
    void tick(const Input& in, const Ground& ground);
    const std::array<float, kRegions>& coverage() const { return amount_; }
    void build(const Precip::Frame& f, std::vector<render::FxVertex>& vertices, std::vector<render::FxBatch>& batches) const;
    size_t clumps() const { return clumps_.size(); }
    void clear();

private:
    struct Clump { glm::vec3 pos{0}, vel{0}; float size = 0, rest = -1e9f, age = 0, life = 0; bool landed = false; };
    void shed(const Input& in, int region, float amount, const Ground& ground);
    std::array<float, kRegions> amount_{}, carry_{}; // carry_: lost amount not yet shed as a clump
    std::vector<Clump> clumps_;
    glm::vec3 lastVel_{0};
    bool lastGrounded_ = true;
    uint32_t seed_ = 0x9E3779B9u;
};

} // namespace ou::fx
