// Thin RAII over SDL3 lifetime objects. Only apps include this; renderers take SDL_Window*.
#pragma once

#include <memory>

#include <SDL3/SDL.h>

namespace ou::platform {

// SDL_Init/SDL_Quit pair. `ok` is false when no video device is available (headless).
struct SdlInit {
    bool ok;
    explicit SdlInit(SDL_InitFlags flags) : ok(SDL_Init(flags)) {}
    ~SdlInit() { SDL_Quit(); }
    SdlInit(const SdlInit&) = delete;
    SdlInit& operator=(const SdlInit&) = delete;
};

struct WindowDeleter {
    void operator()(SDL_Window* w) const { SDL_DestroyWindow(w); }
};
using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;

struct GamepadDeleter {
    void operator()(SDL_Gamepad* g) const { SDL_CloseGamepad(g); }
};
using GamepadPtr = std::unique_ptr<SDL_Gamepad, GamepadDeleter>;

} // namespace ou::platform
