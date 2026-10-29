// AMD FSR upscaling + frame generation through the FidelityFX SDK 2.4.0 ffx-api (docs/RENDERING.md "FSR").
// Providers compiled in (third_party/fidelityfx-sdk): FSR 3.1.6 upscaler, FSR 3.1.6 frame generation. The renderer
// owns every image; this wraps context lifetime, jitter and the per-frame dispatches.
#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace ou::render::vkfsr {

// Why FSR cannot run on this device (missing formatless storage access, compute subgroup operations); empty = can.
std::string probe(VkPhysicalDevice gpu);
// Render resolution for a quality mode (0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance):
// display / 1.0, 1.5, 1.7, 2.0, 3.0 (the SDK's ratios), at least 1 pixel.
VkExtent2D renderExtent(VkExtent2D display, int quality);
// SDK provider version strings, e.g. "3.1.6".
std::string upscalerVersion();
std::string frameGenerationVersion();

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
};
struct CameraInfo {
    float nearZ = 1, farZ = 100000, fovY = 1; // fovY in radians
    glm::vec3 position{0}, up{0, 0, 1}, right{1, 0, 0}, forward{0, 1, 0};
};

class Context {
public:
    ~Context() { destroy(); }
    // display = output size; frameGeneration adds the frame generation context (display-sized, RGBA8 output).
    bool create(VkDevice device, VkPhysicalDevice gpu, VkExtent2D render, VkExtent2D display, bool frameGeneration,
                std::string& error);
    void destroy();
    bool valid() const { return upscaler_ != nullptr; }
    bool frameGeneration() const { return framegen_ != nullptr; }
    VkExtent2D render() const { return render_; }
    VkExtent2D display() const { return display_; }

    // Sub-pixel jitter (pixels, x right / y down) for the next frame; advances the sequence.
    glm::vec2 nextJitter();

    // Inputs at render resolution, in SHADER_READ_ONLY_OPTIMAL (depth included); output display-sized in GENERAL.
    // motion: UV-space offsets current -> previous position (unjittered), RG16F. reactive: R8 [0, 1].
    // Returns false (and the error) when a dispatch fails.
    bool upscale(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& reactive,
                 const Image& output, glm::vec2 jitter, float frameMs, bool reset, float sharpness, const CameraInfo& cam,
                 std::string& error);
    // Frame generation between the previous and this frame's HUD-less `present` (display-sized, SHADER_READ_ONLY_OPTIMAL)
    // into `output` (GENERAL). Same depth / motion / jitter as this frame's upscale().
    bool generate(VkCommandBuffer cmd, const Image& present, const Image& depth, const Image& motion, const Image& output,
                  glm::vec2 jitter, float frameMs, bool reset, const CameraInfo& cam, std::string& error);

private:
    void* upscaler_ = nullptr; // ffxContext
    void* framegen_ = nullptr;
    VkExtent2D render_{}, display_{};
    int32_t jitterIndex_ = 0;
    uint64_t frameId_ = 0;
};

} // namespace ou::render::vkfsr
