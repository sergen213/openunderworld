// ou-game cutscene playback: a cinstream (src/cine) played in the loaded world with its camera, posed actors
// (unit intros + Lara), the .mul's audio and the subtitle ticker. Docs: docs/formats/CINEMATICS.md.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "audio/Mixer.hpp"
#include "cdc/Cdc.hpp"
#include "game/Game.hpp"
#include "render/Renderer.hpp"

struct SDL_Window;

namespace ou::app {

struct CineView {
    const cdc::BigfileSet& bigfile;
    const game::World& world;          // loaded units and their offsets; Lara's skeleton
    render::Renderer& renderer;
    SDL_Window* window;
    audio::Mixer* mixer = nullptr;     // null: silent
    const scene::SceneData& scene;     // the game scene now uploaded (units, Lara, ...); re-upload it afterwards
    std::vector<size_t> laraMeshes;    // Lara's skinned meshes in `scene`
    std::vector<size_t> hide;          // meshes of `scene` collapsed while playing (pistols)
    render::FrameOptions frame{};      // graphics settings
    double renderHz = 0;               // > 0: fixed frame time (replays, screenshots)
    int maxFrames = 0;                 // > 0: stop after this many frames and keep the last image
    float musicVolume = 1;             // Music bus volume restored afterwards (muted while playing)
    bool overlay = false;
};

struct CineResult {
    int frames = 0;                    // rendered
    bool skipped = false;              // Esc / gamepad Start
    bool quit = false;                 // window closed
    std::optional<render::Image> lastImage; // maxFrames reached
    std::optional<glm::vec3> laraPos;  // where the stream leaves Lara (world), header tail (0x455D90)
    float laraYaw = 0;                 // her heading there
};

// Throws on a missing or malformed stream.
CineResult playCinematic(const std::string& name, const CineView& view);

} // namespace ou::app
