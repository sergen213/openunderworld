// Binding names (ou::config::InputConfig) <-> SDL3 devices. Shared by ou-game and the launcher's control editor.
// Device state comes from SDL events alone (DeviceState::apply), so a live session, its recording (eventLine) and the
// replay (EventLog) run through the same code.
#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::platform {

struct InputRef {
    enum class Kind { Key, Mouse, PadButton, PadTrigger } kind;
    int code; // SDL_Scancode | SDL mouse button index (1 left, 2 middle, 3 right) | SDL_GamepadButton | SDL_GamepadAxis
    bool operator==(const InputRef&) const = default;
};

// Case-insensitive; see config::InputConfig for the name forms. nullopt = unknown name.
std::optional<InputRef> parseInput(std::string_view name);
// Canonical name ("Left Shift", "Mouse Right", "Pad dpright"), parseInput(inputName(r)) == r.
std::string inputName(const InputRef& r);
inline bool isPad(const InputRef& r) { return r.kind == InputRef::Kind::PadButton || r.kind == InputRef::Kind::PadTrigger; }

// Keyboard, mouse buttons and the gamepad in use. The pad in use is the first one added; once it is removed, the next
// pad that presses a button or moves an axis past half takes over (hotplug). Other pads' events are ignored.
struct DeviceState {
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouseButtons = 0;                         // SDL_BUTTON_MASK bits
    SDL_JoystickID pad = 0;                            // 0 = none
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> padButtons{};
    std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> padAxes{};
    // A keyboard / mouse button / gamepad event updates the state; returns whether it was the pad in use (or adopted it).
    bool apply(const SDL_Event& e);
};
bool pressed(const InputRef& r, const DeviceState& d);
// The input `e` presses (key down, key repeats too; mouse button down, pad button down, trigger past half), as a binding.
std::optional<InputRef> pressedBy(const SDL_Event& e);

// Recorded sessions, text, one device event per line: `<tick> <event> <args>` where tick is the simulation tick the
// event preceded (ou-game: the app tick it was polled before). Events:
//   key <scancode> <0|1> [repeat]   mouse <button> <0|1>   motion <dx> <dy>
//   padadd <id>   padremove <id>   button <id> <button> <0|1>   axis <id> <axis> <value -32768..32767>
// (SDL numbers). '#' starts a comment.
std::string eventLine(uint64_t tick, const SDL_Event& e); // "" = not a device event
class EventLog {
public:
    static EventLog parse(std::string_view text); // throws std::runtime_error with the line number
    static EventLog load(const std::filesystem::path& path);
    // The next event recorded for a tick <= `tick`; nullopt when there is none (yet).
    std::optional<SDL_Event> next(uint64_t tick);
    size_t size() const { return events_.size(); }
    bool done() const { return next_ == events_.size(); }

private:
    std::vector<std::pair<uint64_t, SDL_Event>> events_; // in file order (ticks ascending)
    size_t next_ = 0;
};

} // namespace ou::platform
