// Backend interface. Apps and scene code talk to this; only src/render/<backend> sees the GPU API.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "render/Camera.hpp"
#include "scene/Assets.hpp"

struct SDL_Window;

namespace ou::render {

struct Viewport {
    int width = 0, height = 0; // drawable size in pixels
};

struct FrameOptions {
    bool lighting = true;                   // simple lambert vs unlit
    bool cullBackFaces = true;              // front = counter-clockwise (cdc meshes are CCW)
    std::span<const std::string> overlay;   // diagnostic text lines, top-left; leading '!' = highlight
};

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb; // tightly packed RGB8, top row first
};

class Renderer {
public:
    virtual ~Renderer() = default;

    // SDL_WindowFlags the window must be created with (e.g. SDL_WINDOW_OPENGL).
    virtual uint64_t windowFlags() const = 0;
    // Called before window creation (e.g. GL attributes). Returns false + error on failure.
    virtual bool preWindowSetup(std::string& error) = 0;
    virtual bool init(SDL_Window* window, std::string& error) = 0;
    // Replaces all GPU resources. Returns warnings (unsupported formats etc.).
    virtual std::vector<std::string> upload(const scene::SceneData& scene) = 0;
    virtual void resize(Viewport viewport) = 0;
    virtual void drawFrame(const Camera& camera, Viewport viewport, const FrameOptions& options) = 0;
    virtual Image readBackbuffer(Viewport viewport) = 0; // after drawFrame, before present
    virtual void present() = 0;
    virtual bool setVsync(bool enabled) = 0;
    virtual std::string description() const = 0;  // device / API version for diagnostics
    virtual void shutdown() = 0;
};

std::unique_ptr<Renderer> createGlRenderer();

} // namespace ou::render
