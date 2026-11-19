#include "render/TimeOfDay.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/gtc/type_ptr.hpp>

namespace ou::render {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float deg(float d) { return d * kPi / 180.0f; }

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
glm::vec3 mix3(glm::vec3 a, glm::vec3 b, float t) { return a + (b - a) * t; }
float luminance(glm::vec3 c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }
void put(float* dst, glm::vec3 v) { dst[0] = v.x, dst[1] = v.y, dst[2] = v.z; }

constexpr float kMinMaxElevation = deg(50.0f);
constexpr float kDegPerHour = 15.0f;
// Moonlight in the sun's slot (intensity x colour, as the units' sun records: 0.8..2.0 by day): a cool light about a
// quarter of a typical sun, so moonlit ground, its shadows and the moon's glint on water read at night.
constexpr glm::vec3 kMoon{0.24f, 0.30f, 0.48f};
// Night scales of what the art baked for daylight. Units without a sun record are lit mostly by their own lamps and
// light maps, so those keep more.
constexpr glm::vec3 kNightAmbient{0.20f, 0.24f, 0.38f}, kNightAmbientSunless{0.60f, 0.62f, 0.72f};
constexpr glm::vec3 kNightLightMaps{0.32f, 0.35f, 0.48f}, kNightLightMapsSunless{0.70f, 0.72f, 0.80f};
constexpr glm::vec3 kNightFog{0.10f, 0.12f, 0.20f};
// Sunrise / sunset tints: the sun, and the sky light (ambient, fog, backdrop).
constexpr glm::vec3 kGoldenSun{1.0f, 0.62f, 0.38f}, kGoldenSky{1.05f, 0.78f, 0.62f};
// Day lift of units whose own look is dark (l6_ship_exterior's storm, fog luminance 0.065): the fog, ambient and sky
// toward this fog luminance, at most x kMaxLift (light maps by its square root).
constexpr float kDayFogLuminance = 0.32f, kMaxLift = 2.2f;

} // namespace

bool timeOfDayApplies(std::string_view unitName, const scene::Environment& env, bool exteriorTerrain, bool rainMap) {
    if (unitName.starts_with("l3_")) return false;
    return (env.valid && env.sun) || exteriorTerrain || rainMap;
}

glm::vec3 SunArc::toward(double hours) const {
    const float h = float(hours);
    const float e = maxElevation * std::sin(2.0f * kPi * (h - 6.0f) / 24.0f);
    const float a = azimuth + deg(kDegPerHour * (h - nativeHour));
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}

SunArc sunArc(const scene::Environment& unit) {
    SunArc arc;
    const glm::vec3 d = glm::make_vec3(unit.sunDirection);
    if (!unit.valid || !unit.sun || glm::dot(d, d) < 1e-12f) return arc;
    const glm::vec3 t = -glm::normalize(d);
    const float e = std::asin(std::clamp(t.z, -1.0f, 1.0f));
    arc.maxElevation = std::max(e, kMinMaxElevation);
    arc.azimuth = std::atan2(t.y, t.x);
    // sin(2 pi (h - 6) / 24) = e / max on the morning side; a sun record below the horizon starts at sunrise.
    arc.nativeHour = 6.0f + 24.0f / (2.0f * kPi) * std::asin(std::clamp(e / arc.maxElevation, 0.0f, 1.0f));
    return arc;
}

double timeOfDayHours(TimeOfDay mode, double seconds, float dayMinutes, double cycleStart) {
    switch (mode) {
    case TimeOfDay::Default: return -1.0;
    case TimeOfDay::Day: return 12.0;
    case TimeOfDay::Night: return 21.0; // full night, the moon ~35 deg up opposite the set sun: in view, long shadows
    case TimeOfDay::Cycle: break;
    }
    const double h = cycleStart + seconds * 24.0 / (double(std::max(dayMinutes, 1.0f)) * 60.0);
    return h - 24.0 * std::floor(h / 24.0);
}

TimeOfDayLight timeOfDayLight(const scene::Environment& unit, double hours, bool clearSky) {
    TimeOfDayLight out;
    const SunArc arc = sunArc(unit);
    const glm::vec3 sunToward = arc.toward(hours);
    const float elevation = std::asin(std::clamp(sunToward.z, -1.0f, 1.0f));
    const float day = smooth(deg(-8.0f), deg(6.0f), elevation);
    const float golden = smooth(deg(-6.0f), deg(2.0f), elevation) * (1.0f - smooth(deg(4.0f), deg(22.0f), elevation));
    const bool sun = unit.valid && unit.sun;

    // The sun, or the moon opposite it once the sun has set (the slot that is brighter; both are dark at the horizon).
    Sky& s = out.sky;
    const float sunUp = smooth(deg(-1.0f), deg(6.0f), elevation);
    const float moonUp = smooth(deg(-1.0f), deg(8.0f), -elevation) * (1.0f - day);
    if (sun) {
        s.sun = true;
        const glm::vec3 own = glm::make_vec3(unit.sunColor);
        if (sunUp >= moonUp) {
            put(s.sunDirection, -sunToward);
            put(s.sunColor, own * sunUp * mix3(glm::vec3(1.0f), kGoldenSun, golden));
        } else {
            put(s.sunDirection, sunToward);
            put(s.sunColor, kMoon * moonUp);
        }
    }

    // Daylight lift of a dark own look, then day -> night with the golden hour between.
    const glm::vec3 fogOwn = glm::make_vec3(unit.fogColor);
    const float fogLum = luminance(fogOwn);
    const float lift = 1.0f + (fogLum > 0.01f ? std::clamp(kDayFogLuminance / fogLum, 1.0f, kMaxLift) - 1.0f : 0.0f) * day;
    const glm::vec3 skyTint = mix3(glm::vec3(1.0f), kGoldenSky, golden);
    const glm::vec3 ambient = mix3(sun ? kNightAmbient : kNightAmbientSunless, glm::vec3(1.0f), day) * skyTint * lift;
    const glm::vec3 lightMaps = mix3(sun ? kNightLightMaps : kNightLightMapsSunless, glm::vec3(1.0f), day) * skyTint * std::sqrt(lift);
    const glm::vec3 fog = mix3(kNightFog, glm::vec3(1.0f), day) * skyTint * lift;
    put(out.ambient, ambient);
    put(out.fog, fog);
    put(s.lightMaps, lightMaps);
    put(s.probes, ambient);
    put(s.backdrop, fog);
    const float clear = clearSky ? 1.0f : 0.0f;
    s.stars = clear * (1.0f - day) * (1.0f - day);
    s.moon = clear * smooth(deg(-2.0f), deg(5.0f), -elevation) * (1.0f - 0.7f * day);
    put(s.moonToward, -sunToward);
    return out;
}

void applyTimeOfDay(scene::Environment& look, const TimeOfDayLight& light) {
    for (int k = 0; k < 3; ++k) {
        look.ambient[k] *= light.ambient[k];
        look.fogColor[k] *= light.fog[k];
        look.waterFogColor[k] *= light.fog[k];
    }
}

namespace {
const char* kSky[2] = { // shaders/sky.*
#include "sky.vert.inc"
    ,
#include "sky.frag.inc"
};
} // namespace

std::string_view skySource(bool vertex) { return kSky[vertex ? 0 : 1]; }

SkyPush skyPush(const Camera& camera, float aspect, const Sky& sky, int mode, float time, bool flipY) {
    const glm::mat4 inv = glm::inverse(camera.view());
    const float ty = std::tan(glm::radians(camera.verticalFovDeg) * 0.5f), tx = ty * aspect;
    SkyPush p;
    p.right = glm::vec4(glm::vec3(inv[0]) * tx, 0.0f);
    p.up = glm::vec4(glm::vec3(inv[1]) * (flipY ? -ty : ty), 0.0f);
    p.forward = glm::vec4(-glm::vec3(inv[2]), 0.0f);
    p.tint = glm::vec4(glm::make_vec3(sky.backdrop), float(mode));
    p.stars = glm::vec4(sky.stars, sky.moon, std::fmod(time, 1000.0f), 0.0f);
    p.moon = glm::vec4(glm::make_vec3(sky.moonToward), 0.0f);
    return p;
}

} // namespace ou::render
