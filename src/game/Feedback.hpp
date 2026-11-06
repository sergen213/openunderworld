// Pad force feedback and camera shake, as tru.exe (docs/GAMEPLAY.md "Vibration and camera shake").
// Both are output only: the simulation steps them (script natives, animation effect commands) but never reads them
// back, so the VIBRATION option and the render rate change nothing the game decides.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace ou::game {

// ---- rumble ------------------------------------------------------------------------------------------
// pc-w\padshock\padshocklib.tfb (loaded by 0x481BA0): u32 31, u32 n, u32, then n {u32 offset, u32 size} sorted by
// name; a sequence: +8 char[64] name, +0x48 i32 channels, +0x4C i32 loop (1: between +0x50 and +0x54 s), +0x60
// channels {i32 keys, i32 interpolation (1 linear, 2 step), i32 [unused], f32 offset, f32 scale, {f32 s, f32 value}
// [keys]} [O].
struct Padshock {
    struct Channel {
        int interpolation = 0;
        float offset = 0, scale = 1;
        std::vector<glm::vec2> keys; // seconds, value
    };
    std::string name;
    bool loop = false;
    float loopStart = 0, loopEnd = 0;
    std::vector<Channel> channels;
};
std::vector<Padshock> parsePadshockLib(std::span<const uint8_t> data); // throws std::runtime_error

// The rumble manager (0xA02BD0, ticked by 0x4817B0): 16 sequence slots plus the two direct "shocks", summed per motor
// of the pad (XInput: 2, both type 2, so sequence channel n drives motor n) and clamped to 1. Motor 0 is XInput's left
// (low frequency), 1 the right (high frequency): 0x484940 -> XInputSetState(value * 65535).
class Rumble {
public:
    void setLibrary(std::vector<Padshock> lib) { lib_ = std::move(lib), slots_ = {}; }
    const std::vector<Padshock>& library() const { return lib_; }
    // 0x481CC0 / 0x481CF0 (effect commands 0xE / 0xF, padshock "shock0" / "shock1"): motor 0 fully on (on != 0) or off,
    // motor 1 at level / 255, each for duration / 4096 frames of 30 Hz (effect records pass frames << 12).
    void shock0(int on, int duration);
    void shock1(int level, int duration);
    // 0x481B50 (by name, binary search) -> 0x481340 (first free slot, else the lowest priority: every slot 0x80) ->
    // 0x483180 rate, 0x4831A0 scale, 0x483100 start. Handle: 1, 3, 5, ... (DAT_008904C0 += 2); 0 = unknown name.
    uint32_t play(std::string_view name, float rate = 1, float scale = 1);
    void stop(uint32_t handle, bool release); // 0x483130: release lets a loop run out, else the slot clears
    bool playing(uint32_t handle) const;      // 0x4830C0
    bool looping(uint32_t handle) const;      // 0x4830E0
    void setRate(uint32_t handle, float rate);   // 0x483180
    void setScale(uint32_t handle, float scale); // 0x4831A0
    void tick(float dt); // 0x4817B0: advance (0x481160), evaluate (0x4811E0), count the shocks down
    // Motor values of the last tick, 0..1, before the VIBRATION option (0x4817B0 multiplies by DAT_008904C4).
    std::array<float, 2> motors() const { return motors_; }

private:
    struct Slot {
        const Padshock* seq = nullptr;
        bool active = false, released = false;
        float time = 0, rate = 1, scale = 1;
        uint32_t handle = 0;
    };
    Slot* find(uint32_t handle);
    const Slot* find(uint32_t handle) const;
    float eval(Slot& s, size_t channel); // 0x4811E0; clears the slot after the channel's last key
    std::vector<Padshock> lib_;
    std::array<Slot, 16> slots_{};
    std::array<float, 2> shockTime_{}, shockLevel_{}, motors_{};
    uint32_t nextHandle_ = 1;
};

// ---- camera shake ------------------------------------------------------------------------------------
// camerashakes.drm DTP records (resource type 0x32, 0x3E0 bytes; 0x5F3190): 7 channels of 0x8C bytes {u8 envelope
// [128], f32 amplitude, f32 period s, f32 randomness %}: camera-local translation x (right), y (down), z (forward),
// rotation (degrees) about z, x, y in that channel order (0x1A4 / 0x230 / 0x2BC), FOV (degrees); +0x3D4 duration s,
// +0x3D8 falloff radius, +0x3DC positional flag [O].
struct ShakeData {
    struct Channel {
        std::array<uint8_t, 128> envelope{};
        float amplitude = 0, period = 0, random = 0;
    };
    std::array<Channel, 7> channels{};
    float duration = 0, radius = 0;
    bool positional = false;
};
std::map<uint32_t, ShakeData> parseCameraShakes(std::span<const uint8_t> drmBytes); // DTP section id -> data

// The shake slots (0xE80020, 8 x 0x90). Started by the camerashake native (0x5EE8C0) for its own duration, stopped
// by id (0x5E8750); stepped each tick (0x5F3390, dt = timeMult / 30) and summed onto the camera (0x5EEA10) and its
// FOV (0x5E2150, clamped 0.1 .. 3.0416 rad).
class CameraShake {
public:
    void setLibrary(std::map<uint32_t, ShakeData> lib) { lib_ = std::move(lib), slots_ = {}, prev_ = now_ = {}; }
    const std::map<uint32_t, ShakeData>& library() const { return lib_; }
    bool start(uint32_t id, float duration); // false: no such shake or every slot busy (0x5E86F0)
    void stop(uint32_t id);
    void tick(float dt);
    // Camera-local offsets: translation (x right, y down, z forward), rotation radians about the same axes, FOV radians.
    struct Offset {
        glm::vec3 move{0}, turn{0};
        float fov = 0;
    };
    Offset offset(float alpha) const; // between the last two ticks (render interpolation)
    bool active() const;

private:
    struct Slot {
        const ShakeData* data = nullptr;
        uint32_t id = 0;
        float time = 0, duration = 0;
        Offset out;
    };
    float channel(const ShakeData::Channel& c, float t, float tn, float scale); // 0x5F2F80 / 0x5F3080
    std::map<uint32_t, ShakeData> lib_;
    std::array<Slot, 8> slots_{};
    Offset prev_, now_;
    uint32_t rng_ = 0x2545F491u; // [I] tru.exe draws from its global lagged-Fibonacci generator 0x8A9C78 (0x597780)
};

} // namespace ou::game
