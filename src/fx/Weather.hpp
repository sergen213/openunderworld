// The original weather (tru.exe 0x546FC0 per frame): script-set rain falling as camera-relative sheets of streak
// quads, splashes and lightning from the unit's weather object. Layouts and evidence: docs/formats/EFFECTS.md
// "Weather". Renderer-neutral; the caller supplies the world queries.
#pragma once

#include "render/Renderer.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace ou::fx {

// The weather object's data block (ObjectBase +0xC8 of rainfx / rainfx_mex: 0x5346A0, 0x546FC0, 0x53A650). Colours are
// D3D 0xAARRGGBB words.
struct WeatherDef {
    std::array<uint32_t, 3> textures{}; // +0x00 sheet textures, dense .. sparse
    std::array<float, 4> levels{};      // +0x0C..+0x18 rain amounts (0..1) where the textures take over
    float minWidth = 0, width = 0;      // +0x20 / +0x24: quad half-width max(width x 512 / depth, minWidth), view units
    float length = 0;                   // +0x28 streak = velocity x length
    float nearZ = 0, farZ = 0;          // +0x2C drawn beyond this view depth; +0x30 far colour scale
    float wind = 0;                     // +0x34 drift = unit wind x this
    uint32_t colourA = 0, colourB = 0;  // +0x38 / +0x3C rgb of the two textures' drops
    uint32_t farColour = 0;             // +0x40 rgb OR'd in x depth / farZ
    uint32_t alpha = 0;                 // +0x44: >> 24 the drops' alpha, low 24 bits OR'd into both colours
    uint8_t flags = 0;                  // +0x48: 1 the two textures cross-fade by alpha, 2 lightning
    uint32_t flashColour = 0;           // +0x4C bytes R, G, B of the lightning light
    uint32_t flashLight = 0;            // +0x50 light DTP id (0 = no lightning)
    float flashRadius = 0;              // +0x54
    float flashDistance = 0, flashHeight = 0; // +0x64 / +0x68 (s32): where the light stands from the camera [I]
};
inline constexpr size_t kWeatherDefSize = 0x6C;
std::optional<WeatherDef> parseWeatherDef(std::span<const uint8_t> data); // nullopt when shorter than kWeatherDefSize

class Weather {
public:
    // Camera axes (world): screen x right, screen y down, view forward.
    struct Camera { glm::vec3 eye{0}, right{1, 0, 0}, down{0, 0, -1}, forward{0, 1, 0}; float tanHalfFovY = 0.41421f, aspect = 16.0f / 9.0f; };
    struct World {
        std::function<int32_t(glm::vec3)> ceiling;            // precipitation height map at p (kNone -8000000)
        std::function<std::optional<float>(glm::vec3)> water; // water surface z near p (0x5C8F50)
        std::function<glm::vec2(glm::vec3)> wind;             // unit wind at p
        // Collision segment `from` -> `to`: hit point and surface material (0x4935F0 flags 0x10003).
        std::function<std::optional<std::pair<glm::vec3, uint8_t>>(glm::vec3, glm::vec3)> ray;
    };
    // An object event the weather runs on Lara with the weather object's list (0x5BA000): 0x2E splash in view, 0x2F
    // splash behind the camera, 0x30 lightning; at `pos`, records filtered by `material`.
    struct Event { uint8_t event = 0; glm::vec3 pos{0}; uint8_t material = 0; };
    struct Flash { glm::vec3 pos{0}, colour{0}; float radius = 0; }; // the lightning light, colour x intensity

    // Script amounts (script::Runtime::Weather): rain %, lightning interval seconds (a min change != 0 restarts it).
    void setScript(int32_t rainPercent, int32_t lightningMin, int32_t lightningMax);
    // One 60 Hz tick with the weather object's data (null: none loaded): drops fall and spawn, splashes and lightning
    // go to events(). `cinematic`: 0x5EA390 (no new lightning light, no drawing).
    void tick(const WeatherDef* def, const Camera& cam, const World& world, bool cinematic = false);
    // The drops as additive quads (0x5346A0), appended.
    void build(const WeatherDef& def, const Camera& cam, std::vector<render::FxVertex>& vertices,
               std::vector<render::FxBatch>& batches) const;
    const std::vector<Event>& events() const { return events_; }
    std::optional<Flash> flash() const;
    size_t dropCount() const { return live_; }
    int32_t rain() const { return rain_; } // 0..1024
    void clear();

private:
    struct Drop { int32_t x = 0, y = 0, z = 0, ground = 0; int16_t vx = 0, vy = 0, vz = 0, life = 0; uint8_t variant = 0; };
    static constexpr size_t kDrops = 0x200;
    int32_t rand(); // the CRT rand() tru.exe's weather calls (0x814118)
    glm::vec3 fromScreen(const Camera& cam, float sx, float sy, float depth) const; // 0x539C00
    void spawn(const WeatherDef& def, const Camera& cam, const World& world);
    void splashes(const Camera& cam, const World& world);
    void lightning(const WeatherDef* def, const Camera& cam, bool cinematic);
    uint32_t seed_ = 1;
    std::array<Drop, kDrops> drops_{};
    size_t live_ = 0;
    int32_t rain_ = 0, ramp_ = 0;          // DAT_00AE4D84 (% x 1024 / 100), DAT_00AE4D8C (0..4096 fade in)
    int32_t lightningMin_ = 600, lightningMax_ = 900; // 30 Hz frames (DAT_00AE5148 / 514C)
    int32_t timer_ = 0, kind_ = 0, step_ = 0;         // DAT_00AE5144 / 5154 / 5150
    bool flashOn_ = false;                 // DAT_00AE5120: the lightning light exists
    float intensity_ = 0;                  // light +0x34
    glm::vec3 flashPos_{0}, flashColour_{0};
    float flashRadius_ = 0;
    uint64_t ticks_ = 0;
    glm::vec3 prevEye_{0}, eyeDelta_{0};   // DAT_00B04680: the camera's last position (streaks stretch with its motion)
    std::vector<Event> events_;
};

} // namespace ou::fx
