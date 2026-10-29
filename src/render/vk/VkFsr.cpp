#include "render/vk/VkFsr.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#if OU_HAS_FSR
#include <ffx_api.h>
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>
#endif

namespace ou::render::vkfsr {
#if OU_HAS_FSR
namespace {

FfxApiResource resource(const Image& i, uint32_t state) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i.format;
    ci.extent = {i.extent.width, i.extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.usage = i.usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(i.image), ffxApiGetImageResourceDescriptionVK(i.image, ci, 0), state);
}

ffxCreateBackendVKDesc backendDesc(VkDevice device, VkPhysicalDevice gpu) {
    ffxCreateBackendVKDesc b{};
    b.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    b.deviceContext = {device, gpu, vkGetDeviceProcAddr};
    return b;
}

std::string version(uint64_t createType) {
    uint64_t count = 1;
    const char* name = nullptr;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = createType;
    q.outputCount = &count;
    q.versionNames = &name;
    return ffxQuery(nullptr, &q.header) == FFX_API_RETURN_OK && count && name ? name : "?";
}

std::string failure(std::string_view what, ffxReturnCode_t rc) { return std::string(what) + " failed (ffx-api " + std::to_string(rc) + ")"; }

} // namespace

std::string probe(VkPhysicalDevice gpu) {
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(gpu, &f);
    if (!f.shaderStorageImageReadWithoutFormat || !f.shaderStorageImageWriteWithoutFormat)
        return "no formatless storage image access";
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg};
    vkGetPhysicalDeviceProperties2(gpu, &p);
    constexpr VkSubgroupFeatureFlags kNeed = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                             VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                             VK_SUBGROUP_FEATURE_QUAD_BIT;
    if (!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (sg.supportedOperations & kNeed) != kNeed)
        return "compute subgroup operations missing";
    if (p.properties.limits.maxPushConstantsSize < 208) return "maxPushConstantsSize < 208";
    for (VkFormat fmt : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu, fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return "storage image formats missing";
    }
    return {};
}

VkExtent2D renderExtent(VkExtent2D display, int quality) {
    static constexpr float kRatio[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f}; // FfxApiUpscaleQualityMode order
    const float r = kRatio[std::clamp(quality, 0, 4)];
    return {std::max(1u, uint32_t(float(display.width) / r)), std::max(1u, uint32_t(float(display.height) / r))};
}

std::string upscalerVersion() { return version(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE); }
std::string frameGenerationVersion() { return version(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION); }

bool Context::create(VkDevice device, VkPhysicalDevice gpu, VkExtent2D render, VkExtent2D display, bool frameGeneration,
                     std::string& error) {
    destroy();
    render_ = render, display_ = display;
    auto backend = backendDesc(device, gpu);
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &backend.header;
    // Input = the post pass output: display-referred (gamma-encoded) LDR colour; exposure 1.
    up.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    up.maxRenderSize = {render.width, render.height};
    up.maxUpscaleSize = {display.width, display.height};
    ffxContext ctx = nullptr;
    if (ffxReturnCode_t rc = ffxCreateContext(&ctx, &up.header, nullptr); rc != FFX_API_RETURN_OK) {
        error = failure("FSR upscaler context", rc);
        return false;
    }
    upscaler_ = ctx;
    if (frameGeneration) {
        auto fgBackend = backendDesc(device, gpu);
        ffxCreateContextDescFrameGeneration fg{};
        fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fg.header.pNext = &fgBackend.header;
        fg.displaySize = {display.width, display.height};
        fg.maxRenderSize = {render.width, render.height};
        fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        ffxContext fctx = nullptr;
        if (ffxReturnCode_t rc = ffxCreateContext(&fctx, &fg.header, nullptr); rc != FFX_API_RETURN_OK) {
            error = failure("FSR frame generation context", rc);
            destroy();
            return false;
        }
        framegen_ = fctx;
    }
    jitterIndex_ = 0;
    return true;
}

void Context::destroy() {
    for (void** c : {&framegen_, &upscaler_})
        if (*c) {
            ffxContext ctx = *c;
            ffxDestroyContext(&ctx, nullptr);
            *c = nullptr;
        }
}

glm::vec2 Context::nextJitter() {
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount pc{};
    pc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    pc.renderWidth = render_.width, pc.displayWidth = display_.width;
    pc.pOutPhaseCount = &phases;
    ffxContext ctx = upscaler_;
    ffxQuery(&ctx, &pc.header);
    glm::vec2 j{0};
    ffxQueryDescUpscaleGetJitterOffset q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index = jitterIndex_ = phases > 0 ? (jitterIndex_ + 1) % phases : 0;
    q.phaseCount = std::max(phases, 1);
    q.pOutX = &j.x, q.pOutY = &j.y;
    ffxQuery(&ctx, &q.header);
    return j;
}

bool Context::upscale(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& reactive,
                      const Image& output, glm::vec2 jitter, float frameMs, bool reset, float sharpness, const CameraInfo& cam,
                      std::string& error) {
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resource(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.reactive = resource(reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {jitter.x, jitter.y};
    d.motionVectorScale = {float(color.extent.width), float(color.extent.height)}; // UV-space vectors
    d.renderSize = {color.extent.width, color.extent.height};
    d.upscaleSize = {output.extent.width, output.extent.height};
    d.enableSharpening = sharpness > 0.0f;
    d.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = frameMs;
    d.preExposure = 1.0f;
    d.reset = reset;
    d.cameraNear = cam.nearZ, d.cameraFar = cam.farZ, d.cameraFovAngleVertical = cam.fovY;
    d.viewSpaceToMetersFactor = 0.01f; // [INFERENCE] cdc world units are centimetres
    ffxContext ctx = upscaler_;
    if (ffxReturnCode_t rc = ffxDispatch(&ctx, &d.header); rc != FFX_API_RETURN_OK) {
        error = failure("FSR upscale dispatch", rc);
        return false;
    }
    return true;
}

bool Context::generate(VkCommandBuffer cmd, const Image& present, const Image& depth, const Image& motion, const Image& output,
                       glm::vec2 jitter, float frameMs, bool reset, const CameraInfo& cam, std::string& error) {
    ffxContext ctx = framegen_;
    ++frameId_;
    // Swapchain-free use: the renderer presents the generated image itself (VkRenderer::present).
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.frameGenerationEnabled = true;
    cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfg.frameID = frameId_;
    if (ffxReturnCode_t rc = ffxConfigure(&ctx, &cfg.header); rc != FFX_API_RETURN_OK) {
        error = failure("FSR frame generation configure", rc);
        return false;
    }
    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.frameID = frameId_;
    prep.commandList = cmd;
    prep.renderSize = {depth.extent.width, depth.extent.height};
    prep.jitterOffset = {jitter.x, jitter.y};
    prep.motionVectorScale = {float(depth.extent.width), float(depth.extent.height)};
    prep.frameTimeDelta = frameMs;
    prep.reset = reset;
    prep.cameraNear = cam.nearZ, prep.cameraFar = cam.farZ, prep.cameraFovAngleVertical = cam.fovY;
    prep.viewSpaceToMetersFactor = 0.01f;
    prep.depth = resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prep.motionVectors = resource(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    for (int i = 0; i < 3; ++i) {
        prep.cameraPosition[i] = cam.position[i], prep.cameraUp[i] = cam.up[i];
        prep.cameraRight[i] = cam.right[i], prep.cameraForward[i] = cam.forward[i];
    }
    if (ffxReturnCode_t rc = ffxDispatch(&ctx, &prep.header); rc != FFX_API_RETURN_OK) {
        error = failure("FSR frame generation prepare", rc);
        return false;
    }
    ffxDispatchDescFrameGeneration d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    d.commandList = cmd;
    d.presentColor = resource(present, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.outputs[0] = resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.numGeneratedFrames = 1;
    d.reset = reset;
    d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    d.frameID = frameId_;
    if (ffxReturnCode_t rc = ffxDispatch(&ctx, &d.header); rc != FFX_API_RETURN_OK) {
        error = failure("FSR frame generation dispatch", rc);
        return false;
    }
    return true;
}

#else // built without the FidelityFX SDK (no DXC at configure time, third_party/fidelityfx-sdk/CMakeLists.txt)
std::string probe(VkPhysicalDevice) { return "built without FSR (no DXC)"; }
VkExtent2D renderExtent(VkExtent2D display, int) { return display; }
std::string upscalerVersion() { return "-"; }
std::string frameGenerationVersion() { return "-"; }
bool Context::create(VkDevice, VkPhysicalDevice, VkExtent2D, VkExtent2D, bool, std::string& error) {
    error = probe(VK_NULL_HANDLE);
    return false;
}
void Context::destroy() {}
glm::vec2 Context::nextJitter() { return glm::vec2(0); }
bool Context::upscale(VkCommandBuffer, const Image&, const Image&, const Image&, const Image&, const Image&, glm::vec2, float, bool,
                      float, const CameraInfo&, std::string&) { return false; }
bool Context::generate(VkCommandBuffer, const Image&, const Image&, const Image&, const Image&, glm::vec2, float, bool,
                       const CameraInfo&, std::string&) { return false; }
#endif
} // namespace ou::render::vkfsr
