// Synthetic content for when real assets are missing. Everything here sets origin "placeholder:*".
#pragma once

#include "scene/Assets.hpp"

namespace ou::scene {

// Checker-textured ground plane + 5x5 cube grid (Z-up). Exercises RGBA8, BGRA8 and BC1 uploads.
SceneData makePlaceholderScene();

// One upright quad per texture laid out in a grid on the XZ plane, facing -Y
// (real textures, placeholder geometry).
std::vector<MeshData> makeTextureGallery(const std::vector<TextureData>& textures);

} // namespace ou::scene
