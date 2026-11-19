// Time of day in the outdoor units (docs/RENDERING.md "Time of day"; ours, tru.exe has none): the unit's own lighting is
// its "default". Day, night and the cycle move the sun along a daily arc through the unit's own sun direction, swap it
// for a moon at night, and scale the light the art baked for daylight (ambient, light maps, light probes, fog, the sky
// domes) toward night, with a golden tint around sunrise and sunset. Local lights (torches, lamps, the flashlight) keep
// their strength.
#pragma once

#include <string_view>

#include "render/Renderer.hpp"
#include "scene/Assets.hpp"

namespace ou::render {

// config::kTimesOfDay order.
enum class TimeOfDay : uint8_t { Default, Day, Night, Cycle };

// Whether the time of day applies in a unit: under open sky (a sun record, exterior terrain or rain-map cells) and not
// Croft Manor (l3_*: the prologue's burning manor and the chapter 3 manor, lit by their own scripted storm and fire).
bool timeOfDayApplies(std::string_view unitName, const scene::Environment& env, bool exteriorTerrain, bool rainMap);

// The unit's daily sun arc: the elevation peaks at max(own elevation, 50 deg) at 12:00, rises at 6:00 and sets at 18:00;
// the azimuth turns 15 deg per hour and passes the unit's own sun direction at nativeHour (between 6 and 12), so the
// cycle shows the default lighting there. Units without a sun get the arc of a 50 deg sun from a fixed azimuth.
struct SunArc {
    float maxElevation = 0.8727f; // radians
    float azimuth = 0.6f;         // radians at nativeHour
    float nativeHour = 9.0f;
    glm::vec3 toward(double hours) const; // unit vector toward the sun (below the horizon at night)
};
SunArc sunArc(const scene::Environment& unit);

// Hours (0..24) shown by a mode: Day 12, Night 21, Cycle `cycleStart` + seconds x 24 / (dayMinutes x 60), wrapped.
// Default: negative (none).
double timeOfDayHours(TimeOfDay mode, double seconds, float dayMinutes, double cycleStart);

// The frame's light at `hours` in a unit with `unit`'s own lighting. clearSky: no rain / snow (stars and moon shown).
struct TimeOfDayLight {
    Sky sky;                        // FrameOptions::sky
    float ambient[3] = {1, 1, 1};   // x the look's ambient (applyTimeOfDay)
    float fog[3] = {1, 1, 1};       // x every fog colour: the look's fog / water fog sets, the filters' fog blocks
};
TimeOfDayLight timeOfDayLight(const scene::Environment& unit, double hours, bool clearSky);
// The look's ambient and fog / water fog colours x light's scales.
void applyTimeOfDay(scene::Environment& look, const TimeOfDayLight& light);

// GL sources of shaders/sky.vert (true) / sky.frag (false): the backdrop pass (Sky::backdropPass). Its push block / GL
// uniforms: SkyPush.
std::string_view skySource(bool vertex);
// The backdrop pass's inputs (std430 / push constant layout of shaders/sky.*): view ray = forward + x right + y up for
// NDC (x, y) with y up; mode 0 multiplies the frame by tint, mode 1 adds the stars and the moon.
struct SkyPush {
    glm::vec4 right, up, forward; // world; right / up scaled by the half-FOV tangents
    glm::vec4 tint;               // rgb; w = mode (0 multiply, 1 add)
    glm::vec4 stars;              // x star brightness, y moon brightness, z time (s, twinkle), w unused
    glm::vec4 moon;               // xyz toward the moon
};
// For the camera (aspect = width / height); flipY: the target's NDC y points down (Vulkan without a flipped viewport).
SkyPush skyPush(const Camera& camera, float aspect, const Sky& sky, int mode, float time, bool flipY = false);

} // namespace ou::render
