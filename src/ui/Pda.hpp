// PDA 3D views — the SONAR MAP, and the weapon / item models of WEAPON SELECTION and INVENTORY — rasterised on the
// CPU into RGBA images that the app hands to the renderers as render::FrameOptions::uiImages[1] (the sonar view
// alone, or the PDA background with the model panel drawn into it): GL and Vulkan show the same pixels and neither
// needs a PDA pass. The original draws them with its renderer (cdc::UIPDAMapWidget, uimodel widgets); look and
// numbers: docs/GAMEPLAY.md "PDA".
#pragma once

#include "ui/Art.hpp"

#include <glm/glm.hpp>

#include <map>
#include <span>
#include <string>
#include <vector>

namespace ou::scene { struct SceneData; }

namespace ou::ui {

// The triangles of a DRM's models (model space, Z up) and their textures as RGBA8 (mip 0).
struct ViewMesh {
    struct Part {
        uint32_t first = 0, count = 0; // into `index`
        int texture = -1;              // into `textures`; -1 untextured
    };
    std::vector<glm::vec3> pos;
    std::vector<glm::vec2> uv;
    std::vector<uint32_t> index;
    std::vector<Part> parts;
    std::vector<Image> textures;
    glm::vec3 min{0}, max{0};
    bool empty() const { return index.empty(); }
};
ViewMesh viewMesh(const scene::SceneData& scene);

// Depth-buffered rasteriser into `target` (RGBA8, top row first), inside the viewport rectangle (pixels). OpenGL
// clip space (glm::perspective); triangles are clipped at the near plane and drawn from both sides.
class Raster {
public:
    Raster(Image& target, int x, int y, int w, int h);
    // shade(bary, pixel): perspective-correct barycentrics of p[0..2]; writes the RGBA pixel and returns true, or
    // returns false to leave the pixel (and the depth) as it was.
    template <class Shade> void triangle(const glm::mat4& clip, const glm::vec3 (&p)[3], Shade&& shade);

private:
    struct V { glm::vec4 c; glm::vec3 b; };
    template <class Shade> void draw(const V& a, const V& b, const V& c, Shade& shade);
    Image& img_;
    int x_, y_, w_, h_;
    std::vector<float> depth_;
};

// Fills the WEAPON SELECTION / INVENTORY model panel (pixel rectangle of `img`) with the original's dark grid and
// draws `mesh` (if any) in a fixed side view: model -Y (the barrels) to the right, fitted to the panel.
void drawModelPanel(Image& img, int x, int y, int w, int h, const ViewMesh* mesh);

// ---- sonar map --------------------------------------------------------------------------------------------------
// Free camera: Left/Right turn, Forward/Back fly along the view, Rise/Sink move up/down; reset() puts it behind Lara.
struct SonarCamera {
    glm::vec3 eye{0};
    float yaw = 0, pitch = 0; // view direction: (cos yaw cos pitch, sin yaw cos pitch, sin pitch)
    void reset(glm::vec3 lara, float laraYaw); // laraYaw: World/PlayerState yaw (model -Y forward)
    // move: x = turn (+ left), y = forward, z = rise; one second of input
    void fly(glm::vec3 move, float dt);
    glm::vec3 dir() const;
    bool operator==(const SonarCamera&) const = default;
};
inline constexpr float kSonarFovDeg = 60.0f;

// A loaded unit's map: its triangles in the unit frame, placed at `offset`, shown where Lara has been (`reveal`:
// the unit-frame points game::World::mapReveal records; a triangle shows once a vertex is within kSonarRevealRadius
// of one).
struct SonarUnit {
    std::string name;
    const SonarMap* map = nullptr;
    glm::vec3 offset{0};
    const std::vector<glm::vec3>* reveal = nullptr;
};
struct SonarPawn {
    const ViewMesh* mesh = nullptr;
    glm::mat4 model{1};
    glm::vec3 color{1};
};
inline constexpr float kSonarRevealRadius = 4000.0f; // [I]
inline constexpr float kSonarGrid = 250.0f;          // world units between grid lines [I: fitted to the captures]

class Sonar {
public:
    // Renders `units` and `pawns` seen from `cam` into a w x h image (black where nothing is).
    const Image& render(int w, int h, const SonarCamera& cam, std::span<const SonarUnit> units, std::span<const SonarPawn> pawns);
    const Image& image() const { return image_; } // the last render()
    // Triangles shown of `unit` (for tests); updated by render().
    const std::vector<uint8_t>* revealed(const std::string& unit) const;
    static std::vector<uint8_t> revealTriangles(const SonarMap& map, std::span<const glm::vec3> points);

private:
    struct Cache { const SonarMap* map = nullptr; size_t points = 0; std::vector<uint8_t> shown; };
    std::map<std::string, Cache> cache_;
    Image image_;
};

} // namespace ou::ui
