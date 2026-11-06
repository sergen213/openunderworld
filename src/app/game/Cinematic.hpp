// ou-game cutscene playback: a cinstream (src/cine) played in the loaded world with its camera, posed actors
// (unit intros + Lara), the .mul's audio and the subtitle ticker. Docs: docs/formats/CINEMATICS.md.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "audio/Mixer.hpp"
#include "cdc/Cdc.hpp"
#include "game/Game.hpp"
#include "fx/Fx.hpp"
#include "render/Renderer.hpp"

struct SDL_Window;
namespace ou::ui { struct Art; }

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
    std::vector<std::pair<uint32_t, uint32_t>> unitMeshes; // per World::units() entry: its first mesh in `scene`, count
    render::FrameOptions frame{};      // graphics settings
    double renderHz = 0;               // > 0: fixed frame time (replays, screenshots)
    int maxFrames = 0;                 // > 0: stop after this many frames and keep the last image
    int shots = 0;                     // > 0: hand `shot` this many images, evenly spread over the stream's frames
    std::function<void(int frame, const render::Image&)> shot;
    float musicVolume = 1, sfxVolume = 1; // Music / Sfx bus volumes restored afterwards (muted while playing)
    bool overlay = false;
    bool depthOfField = false;         // graphics cine_dof: draw the DOF cameras' focus (the original's filter has it off)
    const ui::Art* art = nullptr;      // the game font for the subtitles (null: none drawn)
    int language = 0;                  // subtitle language (locale::Language index)
    // The frame's fog (RENDERING.md "After geometry"): the unit's own filter (weight 1, 0x5C78A0) and the DOF cameras'
    // filter (0x4547D0, while the camera has the flag) sum their fog blocks, topped up by fogSet (the unit look's set).
    bool fog = false;
    const fx::FilterResource* unitFilter = nullptr;
    render::Fog fogSet;
    float farZ = 200000.0f; // camera far plane (Original: the unit look's, as in play)
    // The game's running effects (torches, fires, waterfalls and the actors' own emitters, e.g. co_fx_hell_gate in
    // 290_hel_gate): ticked at 60 Hz on the frame clock, the actors' emitters riding on their posed bones (moved back
    // afterwards), drawn by `drawFx` (sprites into the frame) for the cinematic camera. Null: none (--fx off).
    fx::System* fx = nullptr;
    std::function<void(const render::Camera&, float aspect, render::FrameOptions&)> drawFx;
};

struct CineResult {
    int frames = 0;                    // rendered
    bool skipped = false;              // Esc / gamepad Start
    bool quit = false;                 // window closed
    std::optional<render::Image> lastImage; // maxFrames reached
};

// Throws on a missing or malformed stream.
CineResult playCinematic(const std::string& name, const CineView& view);

// Portal clips (render::portalClips) of the loaded units whose meshes are `unitMeshes` (per World::units() entry, as
// StreamedScene builds the scene), seen from `camera` in unit `cameraUnit`. Empty when the ranges are stale.
std::vector<render::UnitClip> unitClips(const game::World& world, std::span<const std::pair<uint32_t, uint32_t>> unitMeshes,
                                        const std::string& cameraUnit, const render::Camera& camera, float aspect);

} // namespace ou::app
