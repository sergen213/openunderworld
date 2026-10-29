// Vulkan 1.3 backend: dynamic rendering, synchronization2, 2 frames in flight, a linear
// device-memory allocator (no VMA), staging uploads, SPIR-V embedded at build time (glslc).
// Entry points come straight from the linked loader (libvulkan); extension functions via
// vkGetInstanceProcAddr. Output is meant to match the GL backend pixel-for-pixel modulo rasterisation.
#include "render/OriginalShading.hpp"
#include "render/RenderCommon.hpp"
#include "render/Water.hpp"
#include "render/vk/VkFsr.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h> // before SDL_vulkan.h so SDL uses the real handle types
#include <SDL3/SDL_vulkan.h>
#include <glm/gtc/type_ptr.hpp>

namespace ou::render {
namespace {

const uint32_t kSceneVert[] = {
#include "scene.vert.inc"
};
const uint32_t kSceneFrag[] = {
#include "scene.frag.inc"
};
const uint32_t kSceneRtVert[] = { // scene.vert -DOU_RT: invariant position (depth pre-pass)
#include "scene_rt.vert.inc"
};
const uint32_t kSceneRtFrag[] = { // scene.frag -DOU_RT: ray-queried sun shadows + water reflections
#include "scene_rt.frag.inc"
};
const uint32_t kPrepassVert[] = {
#include "prepass.vert.inc"
};
const uint32_t kPrepassFrag[] = {
#include "prepass.frag.inc"
};
const uint32_t kTextVert[] = {
#include "text.vert.inc"
};
const uint32_t kTextFrag[] = {
#include "text.frag.inc"
};
const uint32_t kLineVert[] = {
#include "line.vert.inc"
};
const uint32_t kLineFrag[] = {
#include "line.frag.inc"
};
const uint32_t kShadowVert[] = {
#include "shadow.vert.inc"
};
const uint32_t kShadowFrag[] = {
#include "shadow.frag.inc"
};
const uint32_t kPostVert[] = {
#include "post.vert.inc"
};
const uint32_t kPostFrag[] = {
#include "post.frag.inc"
};
const uint32_t kWaterVert[] = { // Enhanced water + splashes (shared GLSL, render/shaders)
#include "water.vert.inc"
};
const uint32_t kWaterFrag[] = {
#include "water.frag.inc"
};
const uint32_t kSplashVert[] = {
#include "splash.vert.inc"
};
const uint32_t kSplashFrag[] = {
#include "splash.frag.inc"
};
const uint32_t kFsrMaskVert[] = { // FSR motion vectors / reactivity (docs/RENDERING.md "FSR")
#include "fsr_mask.vert.inc"
};
const uint32_t kFsrMaskFrag[] = {
#include "fsr_mask.frag.inc"
};
const uint32_t kFsrCameraFrag[] = {
#include "fsr_camera.frag.inc"
};
const uint32_t kFsrFxFrag[] = { // FSR reactivity of fx sprites, splashes, rain / snow
#include "fsr_fx.frag.inc"
};
const uint32_t kFsrSplashFrag[] = {
#include "fsr_splash.frag.inc"
};
const uint32_t kFsrWeatherFrag[] = {
#include "fsr_weather.frag.inc"
};
const uint32_t kFxVert[] = { // original particle effects (src/fx)
#include "fx.vert.inc"
};
const uint32_t kFxFrag[] = {
#include "fx.frag.inc"
};

// Scene pipelines exist per scene::Blend (blend state is baked into Vulkan pipelines).
// Prepass: RT depth pre-pass (depth only; the RT Opaque pipeline then tests LEQUAL without writes).
enum class PipelineKind { Opaque, Alpha, Additive, Average, Shadow, Lines, Overlay, Post, Prepass, Water, Splash,
                          FxAlpha, FxAdditive, FxSubtract, FxMultiply }; // Fx*: FxBatch::blend order

struct LineVertex {
    float pos[3];
    uint32_t color; // RGBA8
};

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxTextVertices = 6 * 8192; // whole quads; longer overlays are truncated
constexpr VkDeviceSize kBlockSize = 64ull << 20;
constexpr uint32_t kMaxRtTextures = 4096;           // bindless texture array of the ray-traced variant (set 2)
constexpr VkDeviceSize kRtScratchBudget = 64ull << 20; // BLAS builds at upload are batched to share this much scratch

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(r) + ")");
}
#define VK_CHECK(x) check((x), #x)

struct ScenePush {
    glm::mat4 viewProj, model;
    int32_t lighting;
    float alphaRef;
    int32_t rt; // scene.frag: bit 0 RT sun shadows, bit 1 reflective draw (sun, eye, rays: sun.glsl OuShadow)
    float opacity; // scene.frag: instance opacity (FrameOptions::meshOpacity); < 0: opaque material, alpha = -opacity
};
static_assert(sizeof(ScenePush) == 144);
static_assert(sizeof(scene::Vertex) == 18 * 4, "render/sun.glsl kOuVertexWords (ray-traced vertex fetches)");

// scene.frag `Geom`: one per BLAS geometry, indexed by instance custom index + geometry index.
struct RtGeometry {
    VkDeviceAddress vertices, indices;
    uint32_t firstIndex, texture; // texture = slot in the bindless array
    float alphaRef;
    uint32_t pad;
};

const VkVertexInputBindingDescription kSceneBinding{0, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
const VkVertexInputAttributeDescription kSceneAttrs[] = {
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)},
    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
    {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(scene::Vertex, color)},
};
// Shadow pass reads position and uv only.
// Translated original vertex shaders (sm3::Attr locations; docs/formats/SHADERS.md).
const VkVertexInputAttributeDescription kOriginalAttrs[] = {
    {sm3::kAttrPosition, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
    {sm3::kAttrNormal, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)},
    {sm3::kAttrTexcoord0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
    {sm3::kAttrColor0, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(scene::Vertex, color0)},
    {sm3::kAttrTexcoord1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv1)},
    {sm3::kAttrTangent, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, tangent)},
    {sm3::kAttrBinormal, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, binormal)},
};
const VkVertexInputAttributeDescription kShadowAttrs[] = {
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
};
// Depth pre-pass: position, uv, colour (alpha test as the scene pass).
const VkVertexInputAttributeDescription kPrepassAttrs[] = {
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
    {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(scene::Vertex, color)},
};

// Bump allocator over 64 MiB VkDeviceMemory blocks, one chain per memory type. Nothing is freed
// individually; reset() releases everything (scene reload). ponytail: no free list, add one if
// resources ever need per-object lifetimes (streaming).
class LinearAllocator {
public:
    struct Allocation {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint8_t* mapped = nullptr; // host-visible allocations stay persistently mapped
    };

    // blockSize 0 = blocks sized exactly to each request (single-resource arenas: staging, depth, ...).
    // deviceAddress: memory usable by buffers with SHADER_DEVICE_ADDRESS usage (ray tracing inputs).
    void init(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize blockSize = kBlockSize, bool deviceAddress = false) {
        device_ = device;
        blockSize_ = blockSize;
        deviceAddress_ = deviceAddress;
        vkGetPhysicalDeviceMemoryProperties(gpu, &props_);
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpu, &p);
        granularity_ = p.limits.bufferImageGranularity; // keeps linear and optimal resources off shared pages
    }

    Allocation alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want) {
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < props_.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (props_.memoryTypes[i].propertyFlags & want) == want) type = i;
        if (type == UINT32_MAX) throw std::runtime_error("no Vulkan memory type for request");
        const VkDeviceSize align = std::max(req.alignment, granularity_);
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
            if (it->type != type) continue;
            VkDeviceSize off = (it->used + align - 1) / align * align;
            if (off + req.size > it->size) break; // linear: only the newest block of a type is open
            it->used = off + req.size;
            return {it->memory, off, it->mapped ? it->mapped + off : nullptr};
        }
        Block b{VK_NULL_HANDLE, std::max(blockSize_, req.size), req.size, type, nullptr};
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr,
                                        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, deviceAddress_ ? &flags : nullptr, b.size, type};
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &b.memory));
        if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            VK_CHECK(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.mapped)));
        blocks_.push_back(b);
        return {b.memory, 0, b.mapped};
    }

    void reset() {
        for (auto& b : blocks_) vkFreeMemory(device_, b.memory, nullptr); // unmaps implicitly
        blocks_.clear();
    }

private:
    struct Block {
        VkDeviceMemory memory;
        VkDeviceSize size, used;
        uint32_t type;
        uint8_t* mapped;
    };
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    VkDeviceSize granularity_ = 1, blockSize_ = kBlockSize;
    bool deviceAddress_ = false;
    std::vector<Block> blocks_;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE; // sampled images only
    uint32_t slot = 0;                    // index in the RT bindless texture array (0 = white)
    bool cube = false;                    // 6-layer cube view
};

// FrameOptions::uiImages slot: texture + the version/size it was last uploaded with.
struct UiImage {
    GpuImage gpu;
    uint64_t version = 0;
    int width = 0, height = 0;
};

struct GpuMesh {
    Buffer vertices, indices;
    std::vector<scene::SubMesh> submeshes;
    size_t vertexCount = 0;
    glm::mat4 model;
    glm::vec3 center{0.0f}; // world-space bounds centre (blended draw sorting; light sets with `radius`)
    float radius = 0.0f;    // world-space bounding-sphere radius (original::lightSet)
    // Ray tracing (empty without RT support): one BLAS geometry per traced submesh (Opaque + Alpha
    // blends, as the shadow map). Skinned meshes are refitted, others rebuilt, on updateVertices.
    bool skinned = false, water = false;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    Buffer blasBuffer;
    std::vector<VkAccelerationStructureGeometryKHR> rtGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> rtRanges;
    VkDeviceSize rtScratch = 0; // aligned max(build, update) scratch size
    // FSR motion vectors of meshes moved by updateVertices: the previous drawn frame's vertices (GPU copy at the start
    // of the frame) and the frame counter of the last update; the staging slot of a pending updateVertices.
    Buffer prevVertices;
    uint64_t movedFrame = 0, stagingFrame = 0;
    VkDeviceSize stagingOffset = 0;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, acquiredGenerated = VK_NULL_HANDLE; // the latter: FSR frame generation
    Buffer text;
    Buffer lines; // debug lines, grown on demand in lineMemory_[frame]
    VkDeviceSize lineCapacity = 0;
    VkQueryPool gpuTimes = VK_NULL_HANDLE; // OU_VK_PRESENT_STATS: pass timestamps (kGpuMarks)
    bool gpuTimed = false;                 // gpuTimes holds this frame's previous results
};

// updateVertices staging of one frame in flight (host-visible, grown on demand), recorded by recordVertexUploads.
struct Staging {
    LinearAllocator memory;
    Buffer buffer;
    VkDeviceSize capacity = 0, used = 0;
    std::vector<size_t> meshes; // GpuMesh slots with a pending copy
    bool idle = false;          // the frame's fence was waited since its last submit
};

// Staged copies recorded into one command buffer. Sources must outlive flush().
struct UploadBatch {
    struct Region {
        const void* src;
        VkDeviceSize size;
        uint32_t width, height;
    };
    struct ImageJob {
        VkImage image;
        std::vector<Region> mips; // cube maps: each region holds the 6 faces back to back
        uint32_t layers = 1;
    };
    std::vector<std::pair<VkBuffer, Region>> buffers;
    std::vector<ImageJob> images;
};

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                                   VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return b;
}

void barriers(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> images,
              const VkMemoryBarrier2* memory = nullptr) {
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = memory ? 1 : 0;
    d.pMemoryBarriers = memory;
    d.imageMemoryBarrierCount = uint32_t(images.size());
    d.pImageMemoryBarriers = images.data();
    vkCmdPipelineBarrier2(cmd, &d);
}

VKAPI_ATTR VkBool32 VKAPI_CALL onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[vk %s] %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warn",
                     data->pMessage);
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice gpu) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, v.data());
    return v;
}

std::filesystem::path pipelineCachePath() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = xdg && *xdg ? xdg : home && *home ? std::filesystem::path(home) / ".cache" : "";
    return base.empty() ? base : base / "openunderworld" / "vk_pipeline_cache.bin";
}

class VkRenderer final : public Renderer {
public:
    ~VkRenderer() override { shutdown(); }

    uint64_t windowFlags() const override { return SDL_WINDOW_VULKAN; }

    // Instance + a usable 1.3 device must exist before we commit to a Vulkan window.
    bool preWindowSetup(std::string& error) override {
        try {
            if (!SDL_Vulkan_LoadLibrary(nullptr)) throw std::runtime_error(std::string("SDL_Vulkan_LoadLibrary: ") + SDL_GetError());
            libraryLoaded_ = true;
            createInstance();
            pickDevice(VK_NULL_HANDLE);
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            shutdown();
            return false;
        }
    }

    bool init(SDL_Window* window, std::string& error) override {
        try {
            window_ = window;
            if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_))
                throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
            pickDevice(surface_);
            chooseSurfaceFormat();
            createDevice();
            createPipelines();
            createStaticResources();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    std::vector<std::string> upload(const scene::SceneData& scene) override {
        std::vector<std::string> warnings;
        waitIdle();
        destroySceneResources();
        sceneMemory_.init(device_, gpu_, kBlockSize, rtStatus_.supported);

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(std::max<size_t>(1, scene.textures.size()))};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, ps.descriptorCount, 1, &ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &scenePool_));

        UploadBatch batch;
        for (const auto& t : scene.textures) {
            auto warn = [&](const std::string& w) {
                warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): " + w);
            };
            // First id wins (as formats::loadTextures); replacing would free an image still queued in `batch`.
            if (textures_.contains(t.id)) { warn("duplicate texture id, first one wins"); continue; }
            VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
            switch (t.format) {
            case scene::PixelFormat::RGBA8: break;
            case scene::PixelFormat::BGRA8: fmt = VK_FORMAT_B8G8R8A8_UNORM; break;
            case scene::PixelFormat::BC1: fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break; // DXT1 with 1-bit alpha, as D3D9
            case scene::PixelFormat::BC2: fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
            case scene::PixelFormat::BC3: fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
            }
            if (blockBytes(t.format) && !bc_) { warn("BCn unsupported (no textureCompressionBC)"); continue; }
            std::string problem;
            const uint32_t levels = usableMips(t, problem);
            if (!problem.empty()) warn(problem);
            if (levels == 0) continue;
            GpuImage img = createImage(sceneMemory_, fmt, t.mips[0].width, t.mips[0].height, levels,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_SAMPLE_COUNT_1_BIT, t.cube);
            // Cube maps (environment reflections of the original shaders) never feed the 2D generic / RT paths.
            img.cube = t.cube;
            img.set = t.cube ? white_.set : allocateSet(scenePool_, img.view, textureSampler());
            img.slot = t.cube ? 0 : uint32_t(textures_.size() + 1);
            if (rtStatus_.supported && img.slot >= kMaxRtTextures) {
                warn("beyond the " + std::to_string(kMaxRtTextures) + " ray-traced textures: rays see it white");
                img.slot = 0;
            }
            UploadBatch::ImageJob job{img.image, {}, t.cube ? 6u : 1u};
            for (uint32_t i = 0; i < levels; ++i)
                job.mips.push_back({t.mips[i].data.data(), t.mips[i].data.size(), t.mips[i].width, t.mips[i].height});
            batch.images.push_back(std::move(job));
            textures_.emplace(t.id, img);
        }
        meshSlot_.assign(scene.meshes.size(), SIZE_MAX);
        for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
            const scene::MeshData& m = scene.meshes[mi];
            auto warn = [&](const std::string& w) { warnings.push_back("mesh (" + m.origin + "): " + w); };
            if (m.vertices.empty() || m.indices.empty()) { warn("empty"); continue; }
            if (std::any_of(m.indices.begin(), m.indices.end(), [&](uint32_t i) { return i >= m.vertices.size(); })) {
                warn("index out of range, skipped");
                continue;
            }
            GpuMesh g;
            for (const auto& s : m.submeshes) {
                if (uint64_t(s.firstIndex) + s.indexCount > m.indices.size()) warn("submesh range out of bounds, skipped");
                else g.submeshes.push_back(s);
            }
            if (m.submeshes.empty()) g.submeshes.push_back({0, uint32_t(m.indices.size()), 0});
            g.model = glm::make_mat4(m.transform);
            g.skinned = !m.skin.empty();
            // [I] Water surfaces are objects named *watersurface* (l4_pool: ca_goo_pool_watersurface, the
            // surface intro of its watervolume, docs/GAMEPLAY.md "Water"); their meshes get RT reflections.
            g.water = isWaterSurface(m.origin);
            const glm::vec4 sphere = original::boundingSphere(m.vertices, g.model);
            g.center = glm::vec3(sphere), g.radius = sphere.w;
            const VkDeviceSize vb = m.vertices.size() * sizeof(scene::Vertex), ib = m.indices.size() * sizeof(uint32_t);
            const VkBufferUsageFlags rtInput = rtStatus_.supported ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                                                   : 0;
            g.vertices = createBuffer(sceneMemory_, vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | rtInput,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            g.indices = createBuffer(sceneMemory_, ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtInput,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            batch.buffers.push_back({g.vertices.buffer, {m.vertices.data(), vb, 0, 0}});
            batch.buffers.push_back({g.indices.buffer, {m.indices.data(), ib, 0, 0}});
            g.vertexCount = m.vertices.size();
            meshSlot_[mi] = meshes_.size();
            meshes_.push_back(std::move(g));
        }
        // AttenuationSampler light rows follow the scene's light falloff curves (SHADERS.md "Engine inputs").
        const auto lut = original::attenuationLut(scene.environment);
        batch.images.push_back({attenuation_.image, {{lut.rgba.data(), lut.rgba.size(), uint32_t(lut.width), uint32_t(lut.height)}}});
        flush(batch);
        uploadOriginals(scene, warnings);
        bounds_ = sceneBounds(scene);
        return warnings; // acceleration structures: built by the first frame that asks for RT
    }

    void resize(Viewport) override { swapchainDirty_ = true; } // SDL pixel-size event → recreate next frame

    // Copied into this frame-in-flight's host-visible staging buffer; drawFrame records the copies (and BLAS refits)
    // at the start of its command buffer (recordVertexUploads). Repeated updates before a draw overwrite their slot.
    bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) override {
        if (meshIndex >= meshSlot_.size() || meshSlot_[meshIndex] == SIZE_MAX) return false;
        const size_t slot = meshSlot_[meshIndex];
        GpuMesh& g = meshes_[slot];
        if (vertices.size() != g.vertexCount) return false;
        const glm::vec4 sphere = original::boundingSphere(vertices, g.model); // skinned: follows the pose
        g.center = glm::vec3(sphere), g.radius = sphere.w;
        Staging& st = staging_[frameIndex_];
        if (!st.idle) { // its previous frame may still read the staging buffer
            VK_CHECK(vkWaitForFences(device_, 1, &frames_[frameIndex_].fence, VK_TRUE, UINT64_MAX));
            st.idle = true;
        }
        if (g.stagingFrame != frameCounter_ + 1) { // first update since the last recorded frame: a new slot
            const VkDeviceSize need = st.used + vertices.size_bytes();
            if (need > st.capacity) { // grow, keeping this frame's earlier copies (the old buffer is idle)
                LinearAllocator memory;
                memory.init(device_, gpu_, 0);
                const VkDeviceSize capacity = std::bit_ceil(std::max<VkDeviceSize>(need, 4 << 20));
                // System memory: host-visible VRAM (resizable BAR) cut the GPU copies 1.7 -> 0.4 ms in l4_ballcourt but
                // the CPU writes into it dropped the frame rate 58 -> 39 fps.
                Buffer b = createBuffer(memory, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (st.used) std::memcpy(b.mapped, st.buffer.mapped, st.used);
                vkDestroyBuffer(device_, st.buffer.buffer, nullptr);
                st.memory.reset();
                st.memory = std::move(memory);
                st.buffer = b, st.capacity = capacity;
            }
            g.stagingOffset = st.used, g.stagingFrame = frameCounter_ + 1;
            st.used = (need + 15) & ~VkDeviceSize(15);
            st.meshes.push_back(slot);
        }
        std::memcpy(st.buffer.mapped + g.stagingOffset, vertices.data(), vertices.size_bytes());
        // FSR motion vectors: the pose the last frame drew is copied GPU-side into prevVertices before the new one
        // lands (recordVertexUploads); meshes not moved since that frame draw no motion of their own (movedFrame).
        if (fsr_.valid() && !g.prevVertices.buffer)
            g.prevVertices = createBuffer(sceneMemory_, vertices.size_bytes(),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        g.movedFrame = frameCounter_ + 1;
        return true;
    }

    // Start of the frame's command buffer: last frame's vertices -> prevVertices (FSR), staged vertices -> the vertex
    // buffers, then the BLAS refits / rebuilds (the TLAS update follows in drawFrame).
    void recordVertexUploads(VkCommandBuffer cmd) {
        Staging& st = staging_[frameIndex_];
        if (st.meshes.empty()) return;
        // Write-after-read: earlier frames on this queue may still read the vertex buffers.
        const VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        barriers(cmd, {}, &war);
        bool prev = false;
        for (const size_t slot : st.meshes) {
            const GpuMesh& g = meshes_[slot];
            if (!g.prevVertices.buffer || !fsr_.valid()) continue;
            const VkBufferCopy c{0, 0, g.vertexCount * sizeof(scene::Vertex)};
            vkCmdCopyBuffer(cmd, g.vertices.buffer, g.prevVertices.buffer, 1, &c);
            prev = true;
        }
        if (prev) { // the copies above read what the copies below overwrite
            const VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT,
                                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
            barriers(cmd, {}, &b);
        }
        for (const size_t slot : st.meshes) {
            GpuMesh& g = meshes_[slot];
            const VkBufferCopy c{g.stagingOffset, 0, g.vertexCount * sizeof(scene::Vertex)};
            vkCmdCopyBuffer(cmd, st.buffer.buffer, g.vertices.buffer, 1, &c);
            g.stagingFrame = 0; // a later update takes a new slot
        }
        // Vertex input, and the RT shaders' buffer-address reads of vertices (alpha-tested hits).
        const VkMemoryBarrier2 vertex{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        barriers(cmd, {}, &vertex);
        rebuildBlases(cmd, st.meshes);
        st.meshes.clear();
        st.used = 0;
        st.idle = false; // this frame's submit reads it
    }

    // The present thread presents to the same swapchain on the same queue (both externally synchronised): acquire
    // under queueMutex_ without blocking in the driver, which could wait for an image only the present thread frees.
    VkResult acquireImage(VkSemaphore semaphore, uint32_t& index) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            VkResult r;
            {
                const std::lock_guard lock(queueMutex_);
                r = vkAcquireNextImageKHR(device_, swapchain_, 0, semaphore, VK_NULL_HANDLE, &index);
            }
            if (r != VK_NOT_READY && r != VK_TIMEOUT) {
                const int64_t ns = (std::chrono::steady_clock::now() - start).count();
                acquireNs_ += ns, frameAcquireNs_ += ns;
                return r;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void drawFrame(const Camera& eye, Viewport vp, const FrameOptions& options) override {
        frameActive_ = false;
        if (vp.width <= 0 || vp.height <= 0) return; // minimised
        for (size_t i = 0; i < options.dynamicLights.size() && i < environment_.lights.size(); ++i) environment_.lights[i] = options.dynamicLights[i];
        if (vp.width != requested_.width || vp.height != requested_.height) swapchainDirty_ = true;
        if (presentOutOfDate_.exchange(false)) swapchainDirty_ = true; // reported by the present thread
        applySettings(options);
        if (rtPipelines_ && !tlas_ && scenePool_) buildRayTracing(); // first RT frame after upload()
        Frame& f = frames_[frameIndex_];
        {
            const auto start = std::chrono::steady_clock::now();
            VK_CHECK(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));
            fenceNs_ += (std::chrono::steady_clock::now() - start).count();
        }
        for (int attempt = 0;; ++attempt) {
            if (swapchainDirty_) recreateSwapchain(vp);
            if (!swapchain_) return;
            VkResult r = acquireImage(f.acquired, imageIndex_);
            if (r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true; // image acquired; render it, recreate after
            else if (r == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0) { swapchainDirty_ = true; continue; }
            else if (r == VK_ERROR_OUT_OF_DATE_KHR) return;
            else check(r, "vkAcquireNextImageKHR");
            break;
        }
        // FSR frame generation presents a generated image before this frame's own: a second swapchain image. Without
        // one (out of date) this frame presents only its own.
        generatedIndex_ = UINT32_MAX;
        if (fsr_.frameGeneration()) {
            const VkResult r = acquireImage(f.acquiredGenerated, generatedIndex_);
            if (r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
            else if (r != VK_SUCCESS) swapchainDirty_ = true, generatedIndex_ = UINT32_MAX;
        }
        VK_CHECK(vkResetFences(device_, 1, &f.fence)); // only after a successful acquire, else the next wait deadlocks
        VK_CHECK(vkResetCommandPool(device_, f.pool, 0));
        VkCommandBuffer cmd = f.cmd;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        gpuBegin(f, cmd);
        recordVertexUploads(cmd);
        gpuMark(cmd, 1);
        // FSR: sub-pixel jitter on every scene projection (camera.jitter); `eye` keeps the unjittered one for motion.
        Camera camera = eye;
        const bool fsrOn = fsr_.valid();
        glm::vec2 jitter{0.0f};
        const auto now = std::chrono::steady_clock::now();
        const float frameMs = lastFrame_ == std::chrono::steady_clock::time_point{} ? 16.7f
                              : std::clamp(std::chrono::duration<float, std::milli>(now - lastFrame_).count(), 0.1f, 1000.0f);
        lastFrame_ = now;
        if (fsrOn) {
            jitter = fsr_.nextJitter();
            camera.jitter = {2.0f * jitter.x / float(extent_.width), -2.0f * jitter.y / float(extent_.height)};
            // Shared targets read by the previous frame's FSR compute passes: order its reads before this frame's writes.
            const VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
            barriers(cmd, {}, &war);
        }

        // GL clip z in [-w, w] → Vulkan [0, w]; same depth precision as the GL path.
        const glm::mat4 zRemap(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1);
        const float aspect = float(extent_.width) / float(extent_.height);
        // Ray tracing replaces the shadow map on opaque surfaces. Blended ones keep sampling the map (they
        // are drawn layer over layer without depth writes, so tracing them multiplies the cost: ballcourt
        // foliage ~10 ms of 15). Without RT support a requested RT mode falls back to the map entirely.
        const bool rtOn = rtPipelines_ && tlas_ && options.rt != RayTracing::Off;
        const bool shadowMap = options.shadows || options.rt != RayTracing::Off;
        rtStatus_.active = rtOn;
        ScenePush push{zRemap * camera.projection(aspect) * camera.view(), glm::mat4(1), options.lighting ? 1 : 0, 0.0f, 0, 1.0f};
        const glm::mat4 currentVP = zRemap * eye.projection(aspect) * eye.view(); // unjittered (FSR motion)
        const bool enhancedWater = options.water == WaterQuality::Enhanced;
        uint32_t splashCount = 0; // splash droplet vertices drawn (FSR reactivity)
        WaterBlock wb = waterBlock(camera, {int(extent_.width), int(extent_.height)}, push.viewProj, environment_, options);
        if (!enhancedWater) wb.fogColor.w = 0.0f; // Original: no underwater post either
        std::memcpy(waterUbo_[frameIndex_].mapped, &wb, sizeof wb);
        if (enhancedWater) updateRipples(options);
        // The post pass samples opaqueDepth_ for the atmosphere / weather too, not only for Enhanced water; translated
        // materials reading the back/depth buffer (the sea, OrigDraw::screen) draw in the second pass from the copies.
        const bool screenReaders = options.originalShaders && !origPrograms_.empty() && !screenSets_.empty();
        // Material particles that refract / fade softly sample the same copies (FxBatch::screen).
        const bool fxScreen = std::any_of(options.fxBatches.begin(), options.fxBatches.end(), [](const FxBatch& b) { return b.screen(); });
        const bool filter = options.screenFilter && options.screenFilter->active(); // original screen filters (post)
        const bool needDepth = enhancedWater || wb.haze.w > 0.5f || wb.weather.x > 0.5f || screenReaders || fxScreen ||
                               (filter && options.screenFilter->dof);
        for (size_t i = 0; i < uiImages_.size(); ++i) updateUiImage(uiImages_[i], options.uiImages[i]);
        constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        meshOpacities(options.meshOpacity, meshes_.size(), opacity_);
        drawOrder(meshes_, camera.position, order_, opacity_);
        if (rtOn && tlasDirty_) recordTlasUpdate(cmd);
        gpuMark(cmd, 2);
        // Sun shadow block (render/sun.glsl) for both the generic and the translated shaders, per frame in flight.
        const SunShadow sun = sunShadow(camera, {int(extent_.width), int(extent_.height)}, bounds_, sunToward(environment_), true,
                                        shadowMap, rtOn ? (options.rt == RayTracing::Full ? 2 : 1) : 0, std::clamp(options.rtRays, 1, 64));
        std::memcpy(shadowUbo_[frameIndex_].mapped, &sun.block, sizeof sun.block); // host-coherent
        if (shadowMap) recordShadowPass(cmd, sun, zRemap);
        gpuMark(cmd, 3);

        const VkImage target = swapImages_[imageIndex_];
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkPipelineStageFlags2 kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        std::vector<VkImageMemoryBarrier2> begin = {
            // Scene targets are shared by both frames in flight: order against the previous frame's
            // writes (depth, colour) and its post-pass read of the resolved colour.
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthStages,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kDepthStages,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT),
            imageBarrier(sceneColor_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         kColorOut | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        if (msColor_.image)
            begin.push_back(imageBarrier(msColor_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kColorOut,
                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
        if (needDepth && msColor_.image) // the depth resolve writes it in COLOR_ATTACHMENT_OUTPUT (resolves always do)
            begin.push_back(imageBarrier(opaqueDepth_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
        barriers(cmd, begin);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = msColor_.image ? msColor_.view : sceneColor_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = msColor_.image && !needDepth ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE; // pass 2 loads it
        color.clearValue.color = {{0.08f, 0.09f, 0.11f, 1.0f}};
        if (msColor_.image) {
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = sceneColor_.view;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = needDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (needDepth && msColor_.image) { // water samples the opaque depth: resolve it (SAMPLE_ZERO)
            depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depth.resolveImageView = opaqueDepth_.view;
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);

        // Negative height flips Y so GL-convention clip space and CCW front faces carry over unchanged.
        const VkViewport flipped{0, float(extent_.height), float(extent_.width), -float(extent_.height), 0, 1};
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(cmd, 0, 1, &flipped);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkPipeline bound = VK_NULL_HANDLE;
        uint32_t boundMesh = UINT32_MAX;
        auto bindMesh = [&](uint32_t mesh) {
            if (mesh == boundMesh) return;
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &meshes_[mesh].vertices.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, meshes_[mesh].indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            boundMesh = mesh;
        };
        // Original Fidelity: translated TRU shaders where a submesh's material has them, with the sun shadow / RT hooks.
        const bool originals = options.originalShaders && !origPrograms_.empty();
        if (originals) warmOriginalPipelines(rtOn, shadowMap, options.rt == RayTracing::Full);
        const glm::mat4 viewProjD3D = original::viewProjD3D(camera, aspect);
        if (originals) {
            sm3::FrameBlock fb;
            original::fillFrame(fb, camera, {int(extent_.width), int(extent_.height)}, options.time, environment_);
            std::memcpy(origFrameUbo_[frameIndex_].mapped, &fb, sizeof fb); // host-coherent, per frame in flight
        }
        // Sets shared by every draw of a pipeline layout (binding with the other layout disturbs them).
        enum class Bound { None, Generic, Original } layout = Bound::None;
        auto useGeneric = [&] {
            if (layout == Bound::Generic) return;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 1, 1, &shadowSets_[frameIndex_], 0, nullptr);
            if (rtOn) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 2, 1, &rtSet_, 0, nullptr);
            layout = Bound::Generic;
        };
        auto opacityOf = [&](uint32_t mesh) { return mesh < opacity_.size() ? opacity_[mesh] : 1.0f; };
        auto drawOriginal = [&](const DrawRef& d, const OrigDraw& od, VkPipeline p) {
            if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            if (layout != Bound::Original) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 0, 1, &origFrameSet_[frameIndex_], 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 2, 1, &shadowSets_[frameIndex_], 0, nullptr);
                if (rtOn) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 3, 1, &rtSet_, 0, nullptr);
                layout = Bound::Original;
            }
            const GpuMesh& m = meshes_[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 1, 1, &od.set, 0, nullptr);
            sm3::ObjectBlock ob;
            // Once per mesh and frame (the lights move: dynamicLights), not per submesh and pass.
            if (meshLights_.size() < meshes_.size()) meshLights_.resize(meshes_.size());
            auto& [stamp, lights] = meshLights_[d.mesh];
            if (stamp != frameCounter_ + 1) stamp = frameCounter_ + 1, lights = original::lightSet(environment_, m.center, m.radius);
            original::fillObject(ob, viewProjD3D, m.model, lights, opacityOf(d.mesh));
            vkCmdPushConstants(cmd, origLayout_, kPushStages, 0, sizeof ob, &ob);
            bindMesh(d.mesh);
            vkCmdSetCullMode(cmd, options.cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
        };
        auto origDraw = [&](const DrawRef& d) -> const OrigDraw* {
            const OrigDraw* od = originals ? &origDraws_[d.mesh][d.sub] : nullptr;
            return od && od->program >= 0 ? od : nullptr;
        };
        if (rtPipelines_) { // RT depth pre-pass over the opaque submeshes (they are first in order_)
            for (const DrawRef& d : order_) {
                const scene::SubMesh& s = meshes_[d.mesh].submeshes[d.sub];
                if (drawBlend(s, opacityOf(d.mesh)) != scene::Blend::Opaque) break;
                if (enhancedWater && meshes_[d.mesh].water) continue;
                // Translated programs pre-pass with their own (invariant) VS and plain PS (its alpha test, no sun term).
                if (const OrigDraw* od = origDraw(d)) {
                    if (od->screen) continue; // drawn after the copies, depth included
                    drawOriginal(d, *od, origPipeline(od->program, kOrigPrepass, s.blend));
                    continue;
                }
                if (prepassPipeline_ != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = prepassPipeline_);
                layout = Bound::None; // set 0 of sceneLayout_ below disturbs the translated programs' sets
                bindMesh(d.mesh);
                push.model = meshes_[d.mesh].model;
                push.alphaRef = s.alphaRef / 255.0f;
                vkCmdPushConstants(cmd, sceneLayout_, kPushStages, 0, sizeof push, &push);
                vkCmdSetCullMode(cmd, options.cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
                const VkDescriptorSet set = textureSet(s.textureId);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
                vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
            }
        }
        for (const DrawRef& d : order_) {
            const GpuMesh& m = meshes_[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            if (enhancedWater && m.water) continue; // the water pass below draws them with the opaque copies
            const float op = opacityOf(d.mesh);
            const scene::Blend blend = drawBlend(s, op);
            // Rays on opaque surfaces (pre-passed: each visible pixel traces once) and on water for reflections.
            const bool reflective = rtOn && m.water && options.rt == RayTracing::Full;
            const bool rays = rtOn && (blend == scene::Blend::Opaque || reflective);
            if (const OrigDraw* od = origDraw(d)) {
                if (od->screen) {
                    screenDraws_.push_back(d);
                    continue;
                }
                drawOriginal(d, *od, origPipeline(od->program, rays ? kOrigRays : shadowMap ? kOrigShadowMap : kOrigPlain, blend));
                continue;
            }
            useGeneric();
            const VkPipeline p = scenePipelines_[size_t(blend)];
            if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            bindMesh(d.mesh);
            push.model = m.model;
            push.alphaRef = s.alphaRef / 255.0f;
            push.rt = (rtOn && blend == scene::Blend::Opaque ? 1 : 0) | (reflective ? 2 : 0);
            push.opacity = s.blend == scene::Blend::Opaque ? -op : op;
            vkCmdPushConstants(cmd, sceneLayout_, kPushStages, 0, sizeof push, &push);
            vkCmdSetCullMode(cmd, options.cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
            const VkDescriptorSet set = textureSet(s.textureId);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
        }

        if (needDepth) { // second pass: water surfaces + splashes (Enhanced water), then the debug lines below
            vkCmdEndRendering(cmd);
            recordWaterCopies(cmd);
            VkRenderingAttachmentInfo wc = color;
            wc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            VkRenderingAttachmentInfo wd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            wd.imageView = depth_.view;
            wd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            wd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            wd.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ri.pColorAttachments = &wc;
            ri.pDepthAttachment = &wd;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &flipped);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE); // the surface also reads from below
            for (const DrawRef& d : screenDraws_) {
                const OrigDraw& od = origDraws_[d.mesh][d.sub];
                const scene::Blend blend = drawBlend(meshes_[d.mesh].submeshes[d.sub], opacityOf(d.mesh));
                const bool rays = rtOn && blend == scene::Blend::Opaque;
                drawOriginal(d, od, origPipeline(od.program, rays ? kOrigRays : shadowMap ? kOrigShadowMap : kOrigPlain, blend));
            }
            screenDraws_.clear();
        }
        if (enhancedWater) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout_, 0, 1, &waterSets_[frameIndex_], 0,
                                    nullptr);
            for (uint32_t mi = 0; mi < meshes_.size(); ++mi) {
                const GpuMesh& m = meshes_[mi];
                if (!m.water) continue;
                bindMesh(mi);
                vkCmdPushConstants(cmd, waterLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &m.model);
                for (const auto& s : m.submeshes) vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
            }
            if (!options.splashes.empty()) {
                const auto verts = splashVertices(camera, options.splashes);
                const VkDeviceSize bytes = verts.size() * sizeof(SplashVertex);
                if (bytes) {
                    Frame& fr = frames_[frameIndex_];
                    if (bytes > splashCapacity_[frameIndex_]) {
                        vkDestroyBuffer(device_, splashBuffers_[frameIndex_].buffer, nullptr);
                        splashMemory_[frameIndex_].reset();
                        splashCapacity_[frameIndex_] = std::bit_ceil(std::max<VkDeviceSize>(bytes, 64 << 10));
                        splashBuffers_[frameIndex_] =
                            createBuffer(splashMemory_[frameIndex_], splashCapacity_[frameIndex_],
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                    }
                    std::memcpy(splashBuffers_[frameIndex_].mapped, verts.data(), bytes);
                    const VkDeviceSize zero = 0;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splashPipeline_);
                    vkCmdBindVertexBuffers(cmd, 0, 1, &splashBuffers_[frameIndex_].buffer, &zero);
                    vkCmdDraw(cmd, uint32_t(verts.size()), 1, 0, 0);
                    splashCount = uint32_t(verts.size());
                    (void)fr;
                }
            }
        }

        if (!options.fxBatches.empty() && !options.fxVertices.empty()) { // original effects: after water, before lines
            const VkDeviceSize bytes = options.fxVertices.size_bytes();
            if (bytes > fxCapacity_[frameIndex_]) { // this frame's fence was waited above: its old buffer is idle
                vkDestroyBuffer(device_, fxBuffers_[frameIndex_].buffer, nullptr);
                fxMemory_[frameIndex_].reset();
                fxCapacity_[frameIndex_] = std::bit_ceil(std::max<VkDeviceSize>(bytes, 64 << 10));
                fxBuffers_[frameIndex_] = createBuffer(fxMemory_[frameIndex_], fxCapacity_[frameIndex_], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
            std::memcpy(fxBuffers_[frameIndex_].mapped, options.fxVertices.data(), bytes);
            const VkDeviceSize zero = 0;
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdBindVertexBuffers(cmd, 0, 1, &fxBuffers_[frameIndex_].buffer, &zero);
            VkPipeline fxBound = VK_NULL_HANDLE;
            const float zn = camera.nearZ, zf = camera.farZ;
            const glm::vec4 depthToW(-(zf - zn) / (zf * zn), 1.0f / zn, 0, 0); // OriginalShading c77
            for (const auto& b : options.fxBatches) {
                const VkPipeline p = fxPipelines_[std::min<size_t>(b.blend, fxPipelines_.size() - 1)];
                if (p != fxBound) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxBound = p);
                    vkCmdPushConstants(cmd, fxLayout_, kPushStages, 0, sizeof(glm::mat4), &push.viewProj);
                    vkCmdPushConstants(cmd, fxLayout_, kPushStages, sizeof(glm::mat4) + sizeof(glm::vec4), sizeof depthToW, &depthToW);
                }
                const glm::vec4 params(b.distort, b.softScale, b.softBias, 0);
                vkCmdPushConstants(cmd, fxLayout_, kPushStages, sizeof(glm::mat4), sizeof params, &params);
                // Set 1 is the water set (the opaque copies, written by recordWaterCopies when FxBatch::screen asked).
                const VkDescriptorSet sets[3] = {textureSet(b.textureId), waterSets_[frameIndex_], textureSet(b.normalId)};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxLayout_, 0, 3, sets, 0, nullptr);
                vkCmdDraw(cmd, b.count, 1, b.first, 0);
            }
        }
        if (!options.debugLines.empty()) {
            const VkDeviceSize bytes = options.debugLines.size() * 2 * sizeof(LineVertex);
            if (bytes > f.lineCapacity) { // this frame's fence was waited above: its old buffer is idle
                vkDestroyBuffer(device_, f.lines.buffer, nullptr);
                lineMemory_[frameIndex_].reset();
                f.lineCapacity = std::bit_ceil(std::max<VkDeviceSize>(bytes, 64 << 10));
                f.lines = createBuffer(lineMemory_[frameIndex_], f.lineCapacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
            auto* out = reinterpret_cast<LineVertex*>(f.lines.mapped);
            for (const DebugLine& l : options.debugLines) { // ponytail: re-expanded every frame, cache if it shows up
                *out++ = {{l.a[0], l.a[1], l.a[2]}, l.color};
                *out++ = {{l.b[0], l.b[1], l.b[2]}, l.color};
            }
            const VkDeviceSize zero = 0;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
            vkCmdPushConstants(cmd, lineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &push.viewProj);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines.buffer, &zero);
            vkCmdDraw(cmd, uint32_t(options.debugLines.size() * 2), 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
        gpuMark(cmd, 4);
        if (fsrOn) recordFsrMasks(cmd, options, splashCount, wb.weather.x > 0.5f, push.viewProj, currentVP,
                                  jitter / glm::vec2(extent_.width, extent_.height), enhancedWater, options.cullBackFaces);

        // Post pass: resolved scene → swapchain (tonemap / FXAA), then the overlay on top. With FSR the post pass writes
        // fsrInput_ at the render resolution; FSR upscales it into the swapchain image(s), then the overlay.
        const VkImage postTarget = fsrOn ? fsrInput_.image : target;
        if (filter) recordFilterBlur(cmd); // leaves sceneColor_ TRANSFER_SRC
        const VkImageMemoryBarrier2 toPost[] = {
            imageBarrier(sceneColor_.image, filter ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, filter ? VK_PIPELINE_STAGE_2_BLIT_BIT : kColorOut,
                         filter ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            // Chained to the acquire semaphore wait (same COLOR_ATTACHMENT_OUTPUT stage); old contents discarded.
            imageBarrier(postTarget, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorOut, 0,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        barriers(cmd, toPost);
        VkRenderingAttachmentInfo swap{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        swap.imageView = fsrOn ? fsrInput_.view : swapViews_[imageIndex_];
        swap.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swap.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the fullscreen triangle covers every pixel
        swap.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pColorAttachments = &swap;
        ri.pDepthAttachment = nullptr;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport upright{0, 0, float(extent_.width), float(extent_.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &upright);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        struct { int32_t fxaa, hdr; float brightness, fade; } post{options.fxaa && !fsrOn ? 1 : 0, options.hdr ? 1 : 0, options.brightness,
                                                                  std::clamp(options.screenFade, 0.0f, 1.0f)};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline_);
        vkCmdPushConstants(cmd, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof post, &post);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &postSet_, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 1, 1, &postWaterSets_[frameIndex_], 0,
                                nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 2, 1, &shadowSets_[frameIndex_], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // UI + diagnostic overlay at the display resolution, inside a rendering on the swapchain image.
        const bool hasOverlay = !options.overlay.empty() || !options.ui.empty();
        uint32_t overlayCount = 0;
        if (hasOverlay) {
            std::vector<TextVertex> v = buildOverlayVertices({int(display_.width), int(display_.height)}, options.overlay, options.ui);
            overlayCount = std::min<uint32_t>(uint32_t(v.size()), kMaxTextVertices);
            std::memcpy(f.text.mapped, v.data(), overlayCount * sizeof(TextVertex)); // host-coherent, per-frame buffer
        }
        auto drawOverlay = [&] {
            if (!hasOverlay) return;
            const VkViewport screenFlipped{0, float(display_.height), float(display_.width), -float(display_.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &screenFlipped);
            const float screen[2] = {float(display_.width), float(display_.height)};
            const VkDeviceSize zero = 0;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline_);
            vkCmdPushConstants(cmd, textLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen, screen);
            VkDescriptorSet sets[4] = {font_.set, white_.set, white_.set, white_.set}; // text.frag: font, uiImages[0..2]
            for (size_t i = 0; i < uiImages_.size(); ++i)
                if (uiImages_[i].gpu.image) sets[1 + i] = uiImages_[i].gpu.set;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textLayout_, 0, 4, sets, 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.text.buffer, &zero);
            vkCmdDraw(cmd, overlayCount, 1, 0, 0);
        };
        if (!fsrOn) drawOverlay();
        vkCmdEndRendering(cmd);
        gpuMark(cmd, 5);

        bool fsrDone = false;
        if (fsrOn) {
            // Camera cuts (cinematics, respawns) and the first frame reset FSR's history.
            const bool cut = !fsrHistory_ || glm::distance(eye.position, prevCamPos_) > 500.0f ||
                             glm::dot(eye.forward(), prevCamForward_) < 0.7f;
            const vkfsr::CameraInfo cam{eye.nearZ, eye.farZ, glm::radians(eye.verticalFovDeg), eye.position, eye.up, eye.right(),
                                        eye.forward()};
            fsrDone = recordFsrOutput(cmd, cam, jitter, frameMs, cut, std::clamp(options.sharpness, 0.0f, 1.0f),
                                      [&](VkImageView view) {
                                          VkRenderingAttachmentInfo ui{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                                          ui.imageView = view;
                                          ui.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                          ui.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                                          ui.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                                          VkRenderingInfo uri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                                          uri.renderArea = {{0, 0}, display_};
                                          uri.layerCount = 1;
                                          uri.colorAttachmentCount = 1;
                                          uri.pColorAttachments = &ui;
                                          vkCmdBeginRendering(cmd, &uri);
                                          const VkRect2D all{{0, 0}, display_};
                                          vkCmdSetScissor(cmd, 0, 1, &all);
                                          vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
                                          drawOverlay();
                                          vkCmdEndRendering(cmd);
                                      });
            if (!fsrDone) { // failed dispatch: FSR off from the next frame (swapchain rebuilt without it)
                std::fprintf(stderr, "ou: FSR off: %s\n", fsrError_.c_str());
                swapchainDirty_ = true;
                generatedIndex_ = UINT32_MAX;
            }
            fsrHistory_ = fsrDone;
            prevViewProj_ = currentVP, prevCamPos_ = eye.position, prevCamForward_ = eye.forward();
        }
        if (!fsrDone) { // FSR's path leaves the image(s) in PRESENT_SRC itself
            const VkImageMemoryBarrier2 end = imageBarrier(
                target, fsrOn ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0); // the present semaphore signal makes the writes visible to the presenter
            barriers(cmd, {&end, 1});
        }
        gpuMark(cmd, 6);
        VK_CHECK(vkEndCommandBuffer(cmd));

        // FSR writes the swapchain images by blits: their acquire waits then cover the transfer stage.
        const VkPipelineStageFlags2 acquireStage = fsrOn ? VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                                         : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo wait[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, f.acquired, 0, acquireStage, 0},
                                         {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, f.acquiredGenerated, 0, acquireStage, 0}};
        VkSemaphoreSubmitInfo signal[2] = {
            {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, presentReady_[imageIndex_], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
            {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr,
             generatedIndex_ != UINT32_MAX ? presentReady_[generatedIndex_] : VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0}};
        const uint32_t images = generatedIndex_ != UINT32_MAX ? 2 : 1;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0};
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = images;
        si.pWaitSemaphoreInfos = wait;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = images;
        si.pSignalSemaphoreInfos = signal;
        {
            const std::lock_guard lock(queueMutex_);
            VK_CHECK(vkQueueSubmit2(queue_, 1, &si, f.fence));
        }
        frameActive_ = true;
    }

    Image readBackbuffer(Viewport vp) override {
        if (!frameActive_) // nothing rendered (minimised / out of date): black frame of the requested size
            return {vp.width, vp.height, std::vector<uint8_t>(size_t(std::max(0, vp.width)) * std::max(0, vp.height) * 3)};
        return readSwapImage(imageIndex_);
    }

    // Swapchain image `index` (in PRESENT_SRC, its frame submitted) as RGB8.
    Image readSwapImage(uint32_t index) {
        const uint32_t w = display_.width, h = display_.height;
        LinearAllocator temp;
        temp.init(device_, gpu_, 0);
        Buffer buf = createBuffer(temp, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const VkImage target = swapImages_[index];
        oneShot([&](VkCommandBuffer cmd) {
            // The frame's submit is earlier on this queue; ALL_COMMANDS covers its colour writes.
            auto toSrc = imageBarrier(target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            barriers(cmd, {&toSrc, 1});
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &region);
            auto back = imageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                     VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
            barriers(cmd, {&back, 1}, &host);
        });
        const bool bgr = surfaceFormat_.format == VK_FORMAT_B8G8R8A8_UNORM;
        Image img{int(w), int(h), std::vector<uint8_t>(size_t(w) * h * 3)};
        for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
            const uint8_t* p = buf.mapped + i * 4;
            img.rgb[i * 3 + 0] = bgr ? p[2] : p[0];
            img.rgb[i * 3 + 1] = p[1];
            img.rgb[i * 3 + 2] = bgr ? p[0] : p[2];
        }
        vkDestroyBuffer(device_, buf.buffer, nullptr);
        temp.reset();
        return img;
    }

    // With FSR frame generation the generated image is presented first and this frame's own half a frame interval
    // later (interval = 0.9 / 0.1 moving average of the time between present() calls), so both show for equal times.
    // The pacing runs on a present thread (started with the first generated frame) so the render thread goes on with
    // the next frame instead of sleeping; once it exists every present goes through it, in order.
    void present() override {
        if (!frameActive_) return;
        frameActive_ = false;
        using Clock = std::chrono::steady_clock;
        const auto ready = Clock::now();
        if (captureDir_) capturePresented();
        if (lastReady_ != Clock::time_point{}) {
            // The render thread's own frame time: waiting for an image the present thread still holds is pacing, not
            // work (counting it would hold the interval at whatever pacing it once had).
            const double ms = std::chrono::duration<double, std::milli>(ready - lastReady_).count() - frameAcquireNs_ * 1e-6;
            if (ms < 250.0) frameIntervalMs_ = frameIntervalMs_ > 0.0 ? frameIntervalMs_ * 0.9 + ms * 0.1 : ms;
        }
        frameAcquireNs_ = 0;
        lastReady_ = ready;
        if (generatedIndex_ != UINT32_MAX && !presentThread_.joinable())
            presentThread_ = std::thread([this] { presentLoop(); });
        if (presentThread_.joinable()) {
            {
                const std::lock_guard lock(presentMutex_);
                presentJobs_.push_back({swapchain_, generatedIndex_, imageIndex_, frameIntervalMs_ * 0.5});
            }
            presentCv_.notify_all();
        } else {
            presentImage(swapchain_, imageIndex_);
        }
        if (generatedIndex_ != UINT32_MAX) ++presentedGenerated_;
        ++frameCounter_;
        frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    }

    void presentImage(VkSwapchainKHR swapchain, uint32_t index) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &presentReady_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        VkResult r;
        {
            const std::lock_guard lock(queueMutex_);
            const auto start = std::chrono::steady_clock::now();
            r = vkQueuePresentKHR(queue_, &pi);
            presentNs_ += (std::chrono::steady_clock::now() - start).count();
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) presentOutOfDate_ = true;
        else if (r != VK_SUCCESS) std::fprintf(stderr, "ou: vkQueuePresentKHR: %d\n", int(r)), presentOutOfDate_ = true;
        if (presentStats_) {
            const std::lock_guard lock(presentMutex_);
            presentTimes_.push_back(std::chrono::steady_clock::now());
        }
    }

    // Generated image no earlier than half an interval after the previous real one, the real one half an interval
    // after it; no waiting while further frames are queued (catch up instead of adding latency).
    void presentLoop() {
        using Clock = std::chrono::steady_clock;
        Clock::time_point lastReal{};
        auto sleepUntil = [](Clock::time_point t) {
            if (const auto wait = t - Clock::now(); wait > Clock::duration::zero())
                SDL_DelayPrecise(Uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count()));
        };
        for (;;) {
            PresentJob job;
            bool behind;
            {
                std::unique_lock lock(presentMutex_);
                presentCv_.wait(lock, [&] { return presentQuit_ || !presentJobs_.empty(); });
                if (presentJobs_.empty()) return;
                job = presentJobs_.front();
                presentJobs_.pop_front();
                presentBusy_ = true;
                behind = !presentJobs_.empty();
            }
            const auto half = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(job.halfMs));
            if (job.generated != UINT32_MAX) {
                if (!behind && lastReal != Clock::time_point{}) sleepUntil(lastReal + half);
                presentImage(job.swapchain, job.generated);
                if (!behind) sleepUntil(Clock::now() + half);
            }
            presentImage(job.swapchain, job.real);
            lastReal = Clock::now();
            {
                const std::lock_guard lock(presentMutex_);
                presentBusy_ = false;
            }
            presentCv_.notify_all();
        }
    }

    // Every queued present done (swapchain recreation, device idle, shutdown).
    void drainPresents() {
        if (!presentThread_.joinable()) return;
        std::unique_lock lock(presentMutex_);
        presentCv_.wait(lock, [&] { return presentJobs_.empty() && !presentBusy_; });
    }

    // vkDeviceWaitIdle needs every queue externally synchronised.
    void waitIdle() {
        drainPresents();
        const std::lock_guard lock(queueMutex_);
        vkDeviceWaitIdle(device_);
    }

    // OU_VK_CAPTURE_DIR: every presented image as <dir>/NNNNNN-real.ppm / NNNNNN-gen.ppm (FSR frame generation checks).
    void capturePresented() {
        auto save = [&](uint32_t index, const char* kind) {
            const Image img = readSwapImage(index);
            char name[64];
            std::snprintf(name, sizeof name, "/%06llu-%s.ppm", (unsigned long long)frameCounter_, kind);
            if (FILE* out = std::fopen((std::string(captureDir_) + name).c_str(), "wb")) {
                std::fprintf(out, "P6\n%d %d\n255\n", img.width, img.height);
                std::fwrite(img.rgb.data(), 1, img.rgb.size(), out);
                std::fclose(out);
            }
        };
        if (generatedIndex_ != UINT32_MAX) save(generatedIndex_, "gen");
        save(imageIndex_, "real");
    }

    bool setVsync(bool enabled) override {
        if (!enabled && noVsyncMode() == VK_PRESENT_MODE_FIFO_KHR) return false;
        if (vsync_ != enabled) swapchainDirty_ = true;
        vsync_ = enabled;
        return true;
    }

    RtStatus rtStatus() const override { return rtStatus_; }

    std::string upscalerSummary() const override {
        static constexpr const char* kQuality[] = {"native AA", "quality", "balanced", "performance", "ultra performance"};
        if (!fsrRequested_) return fsrReason_.empty() ? "FSR off" : "FSR off (unavailable: " + fsrReason_ + ")";
        if (!fsr_.valid()) return "!FSR inactive: " + (fsrError_.empty() ? fsrReason_ : fsrError_);
        char line[256];
        std::snprintf(line, sizeof line, "FSR %s %s %ux%u -> %ux%u | frame generation %s (%llu generated)",
                      vkfsr::upscalerVersion().c_str(), kQuality[fsrQuality_], extent_.width, extent_.height, display_.width,
                      display_.height, fsr_.frameGeneration() ? ("on, FG " + vkfsr::frameGenerationVersion()).c_str() : "off",
                      (unsigned long long)presentedGenerated_);
        return line;
    }

    std::string description() const override {
        auto ver = [](uint32_t v) {
            return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
                   std::to_string(VK_API_VERSION_PATCH(v));
        };
        return "Vulkan " + ver(apiVersion_) + " | " + deviceName_ + " | " + driver_ + " | BC " + (bc_ ? "yes" : "NO") +
               " | aniso " + std::to_string(int(maxAniso_)) + "x | ray queries " +
               (rtStatus_.supported ? "yes" : "no (" + rtStatus_.reason + ")") + " | " +
               (validation_ ? "validation on" : messenger_ ? "debug_utils (no validation layer)" : "no debug_utils");
    }

    void shutdown() override {
        if (device_) {
            waitIdle();
            if (presentThread_.joinable()) {
                {
                    const std::lock_guard lock(presentMutex_);
                    presentQuit_ = true;
                }
                presentCv_.notify_all();
                presentThread_.join();
            }
            if (presentStats_) printPresentStats();
            savePipelineCache();
            destroySceneResources();
            destroySwapchain(swapchain_);
            swapchain_ = VK_NULL_HANDLE;
            for (auto& f : frames_) {
                vkDestroyCommandPool(device_, f.pool, nullptr);
                vkDestroyFence(device_, f.fence, nullptr);
                vkDestroySemaphore(device_, f.acquired, nullptr);
                vkDestroySemaphore(device_, f.acquiredGenerated, nullptr);
                vkDestroyBuffer(device_, f.text.buffer, nullptr);
                vkDestroyBuffer(device_, f.lines.buffer, nullptr);
                vkDestroyQueryPool(device_, f.gpuTimes, nullptr);
                f = {};
            }
            destroyImage(white_);
            destroyImage(font_);
            destroyImage(shadow_);
            destroyImage(attenuation_);
            destroyImage(blackCube_);
            if (ripples_.image) destroyImage(ripples_);
            for (auto& u : uiImages_)
                if (u.gpu.image) destroyImage(u.gpu);
            for (auto& b : waterUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& b : splashBuffers_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& m : splashMemory_) m.reset();
            for (auto& b : fxBuffers_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& m : fxMemory_) m.reset();
            for (auto& s : staging_) {
                vkDestroyBuffer(device_, std::exchange(s.buffer, {}).buffer, nullptr);
                s.memory.reset();
            }
            vkDestroyDescriptorPool(device_, std::exchange(waterPool_, VK_NULL_HANDLE), nullptr);
            vkDestroyPipelineLayout(device_, std::exchange(waterLayout_, VK_NULL_HANDLE), nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(waterSetLayout_, VK_NULL_HANDLE), nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(postWaterSetLayout_, VK_NULL_HANDLE), nullptr);
            for (auto& b : origFrameUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& b : shadowUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            vkDestroyDescriptorPool(device_, std::exchange(origStaticPool_, VK_NULL_HANDLE), nullptr);
            vkDestroyPipelineLayout(device_, std::exchange(origLayout_, VK_NULL_HANDLE), nullptr);
            for (VkDescriptorSetLayout* l : {&origFrameLayout_, &origMatLayout_})
                vkDestroyDescriptorSetLayout(device_, std::exchange(*l, VK_NULL_HANDLE), nullptr);
            vkDestroyDescriptorPool(device_, staticPool_, nullptr);
            destroyScenePipelines();
            destroyFsr();
            for (VkPipeline p : {textPipeline_, shadowPipeline_, postPipeline_}) vkDestroyPipeline(device_, p, nullptr);
            for (VkPipelineLayout l : {sceneLayout_, textLayout_, lineLayout_, postLayout_, fxLayout_}) vkDestroyPipelineLayout(device_, l, nullptr);
            for (auto& m : lineMemory_) m.reset();
            vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(shadowSetLayout_, VK_NULL_HANDLE), nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(rtSetLayout_, VK_NULL_HANDLE), nullptr);
            for (VkSampler s : {sceneSampler_, fontSampler_, shadowSampler_, postSampler_, biasedSampler_, blurSampler_}) vkDestroySampler(device_, s, nullptr);
            vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
            vkDestroyCommandPool(device_, uploadPool_, nullptr);
            staticMemory_.reset();
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_) {
            if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
            if (messenger_) {
                auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
                destroy(instance_, messenger_, nullptr);
                messenger_ = VK_NULL_HANDLE;
            }
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (libraryLoaded_) SDL_Vulkan_UnloadLibrary();
        libraryLoaded_ = false;
    }

private:
    // ---- Original Fidelity (translated TRU shaders, docs/formats/SHADERS.md) ----
    // Pixel shader variants: plain (no sun term; RT depth pre-pass), cascaded shadow map, ray-traced sun + water
    // reflections (render/sun.glsl via originalPixelSource). Pipelines: [variant][scene::Blend], pre-pass at [kOrigPrepass][0].
    enum OrigVariant { kOrigPlain, kOrigShadowMap, kOrigRays, kOrigPrepass };
    struct OrigProgram {
        std::vector<uint32_t> vs;                  // SPIR-V (libshaderc at upload)
        std::string psGlsl;                        // translated pixel shader
        std::array<std::vector<uint32_t>, 3> ps;   // per OrigVariant (compiled at upload; rays only with RT support)
        uint8_t attributes = 0;                    // vertex attribute locations the VS reads
        std::array<std::array<VkPipeline, 4>, 4> pipelines{}; // created on first use
    };
    struct OrigDraw {
        int program = -1; // origPrograms_ index, -1 = generic shading
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t screen = 0; // original::screenSamplers: low 16 back buffer, high 16 depth (drawn after the copies)
    };

    VkPipeline origPipeline(int program, OrigVariant variant, scene::Blend blend) {
        OrigProgram& o = origPrograms_[size_t(program)];
        VkPipeline& p = o.pipelines[variant][variant == kOrigPrepass ? 0 : size_t(blend)];
        if (!p) {
            const int v = variant == kOrigPrepass ? kOrigPlain : variant;
            if (o.ps[size_t(v)].empty()) {
                std::string error;
                o.ps[size_t(v)] = sm3::compileSpirv(originalPixelSource(o.psGlsl, true, v == kOrigRays), true, true, error);
                if (o.ps[size_t(v)].empty()) { // keep drawing without the sun term
                    std::fprintf(stderr, "[vk] original pixel shader with sun term: %s\n", error.c_str());
                    o.ps[size_t(v)] = o.ps[kOrigPlain];
                }
            }
            std::vector<VkVertexInputAttributeDescription> attrs; // only consumed locations (validation-clean)
            for (const auto& a : kOriginalAttrs)
                if (o.attributes >> a.location & 1) attrs.push_back(a);
            p = createPipeline(o.vs, o.ps[size_t(v)], origLayout_, &kSceneBinding, attrs,
                               variant == kOrigPrepass ? PipelineKind::Prepass : PipelineKind(blend), samples_, sceneFormat_,
                               depthFormat_);
        }
        return p;
    }

    // Before the first frame that can draw them (after upload or a settings change): every translated pipeline the
    // current RT / shadow settings select for the uploaded submeshes, one program per task on worker threads, instead
    // of creating each on the first draw that needs it (a stall whenever a new material came into view).
    void warmOriginalPipelines(bool rtOn, bool shadowMap, bool rtFull) {
        const int key = rtOn | shadowMap << 1 | rtFull << 2;
        if (key == origWarmKey_) return;
        origWarmKey_ = key;
        std::vector<std::vector<std::pair<OrigVariant, scene::Blend>>> needs(origPrograms_.size());
        for (size_t mi = 0; mi < meshes_.size() && mi < origDraws_.size(); ++mi)
            for (size_t si = 0; si < origDraws_[mi].size(); ++si) {
                const OrigDraw& od = origDraws_[mi][si];
                if (od.program < 0) continue;
                const scene::Blend blend = meshes_[mi].submeshes[si].blend;
                const bool opaque = blend == scene::Blend::Opaque;
                auto& n = needs[size_t(od.program)];
                if (rtPipelines_ && opaque && !od.screen) n.push_back({kOrigPrepass, blend});
                const bool rays = rtOn && (opaque || (meshes_[mi].water && rtFull && !od.screen));
                n.push_back({rays ? kOrigRays : shadowMap ? kOrigShadowMap : kOrigPlain, blend});
            }
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t i; (i = next++) < needs.size();)
                for (const auto& [variant, blend] : needs[i]) origPipeline(int(i), variant, blend);
        };
        std::vector<std::jthread> pool(std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16) - 1);
        for (auto& t : pool) t = std::jthread(work);
        work();
    }

    void destroyOriginalPipelines() {
        for (auto& o : origPrograms_)
            for (auto& byBlend : o.pipelines)
                for (VkPipeline& p : byBlend) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
        origWarmKey_ = -1;
    }

    void destroyOriginalScene() {
        destroyOriginalPipelines();
        origPrograms_.clear();
        origDraws_.clear();
        screenSets_.clear();
        origMaterialImages_.clear();
        vkDestroyBuffer(device_, std::exchange(origMaterialUbo_, {}).buffer, nullptr); // memory: sceneMemory_
        vkDestroyDescriptorPool(device_, std::exchange(origScenePool_, VK_NULL_HANDLE), nullptr);
    }

    // Translates and compiles each material's programs, then one material UBO block + descriptor set per distinct
    // (program, material, instance textures, alpha ref) of the uploaded submeshes.
    void uploadOriginals(const scene::SceneData& scene, std::vector<std::string>& warnings) {
        environment_ = scene.environment;
        const original::Prepared prep = original::prepare(scene, true);
        for (const auto& w : prep.warnings) warnings.push_back(w);
        // Every program's VS and PS variants compile in parallel here (shaderc is reentrant): compiling the sun /
        // rays variants on first draw cost ~30 % of l4_ballcourt's frames as new materials came into view.
        const size_t n = prep.programs.size();
        origPrograms_.resize(n);
        std::vector<std::array<std::string, 4>> errors(n); // per task: each written by one thread
        const bool rays = rtStatus_.supported;
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t task; (task = next++) < n * 4;) {
                const size_t i = task / 4;
                const auto& p = prep.programs[i];
                OrigProgram& o = origPrograms_[i];
                std::string& error = errors[i][task % 4];
                switch (task % 4) {
                case 0: o.vs = sm3::compileSpirv(p.vsGlsl.source, false, true, error); break;
                case 1: o.ps[kOrigPlain] = sm3::compileSpirv(p.psGlsl.source, true, true, error); break;
                case 2: o.ps[kOrigShadowMap] = sm3::compileSpirv(originalPixelSource(p.psGlsl.source, true, false), true, true, error); break;
                case 3: if (rays) o.ps[kOrigRays] = sm3::compileSpirv(originalPixelSource(p.psGlsl.source, true, true), true, true, error); break;
                }
            }
        };
        {
            std::vector<std::thread> pool(std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16) - 1);
            for (auto& t : pool) t = std::thread(work);
            work();
            for (auto& t : pool) t.join();
        }
        std::vector<bool> ok;
        for (size_t i = 0; i < n; ++i) {
            OrigProgram& o = origPrograms_[i];
            o.attributes = prep.programs[i].vsGlsl.attributes;
            o.psGlsl = prep.programs[i].psGlsl.source;
            if (o.vs.empty()) o.ps = {};
            for (int v : {kOrigShadowMap, kOrigRays}) { // keep drawing without the sun term
                if (!o.ps[size_t(v)].empty() || (v == kOrigRays && !rays) || o.ps[kOrigPlain].empty()) continue;
                std::fprintf(stderr, "[vk] original pixel shader with sun term: %s\n", errors[i][size_t(v) + 1].c_str());
                o.ps[size_t(v)] = o.ps[kOrigPlain];
            }
            ok.push_back(!o.ps[kOrigPlain].empty());
            if (!ok.back()) warnings.push_back("original program: " + (o.vs.empty() ? errors[i][0] : errors[i][1]));
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu_, &props);
        const VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
        const VkDeviceSize stride = (sizeof(sm3::MaterialBlock) + align - 1) / align * align;
        struct Key {
            int program;
            uint32_t material, inst[4];
            uint8_t alphaRef;
            bool water; // RT reflections (MaterialBlock::fixed[3] bit 2, sun.glsl ouSunEnd)
            auto operator<=>(const Key&) const = default;
        };
        std::map<Key, size_t> keys;
        std::vector<std::pair<Key, const scene::SubMesh*>> unique;
        std::vector<std::vector<size_t>> slot(meshes_.size());
        origDraws_.assign(meshes_.size(), {});
        for (size_t mi = 0; mi < meshes_.size(); ++mi)
            for (const auto& s : meshes_[mi].submeshes) {
                OrigDraw d;
                d.program = prep.program(s.materialId);
                if (d.program >= 0 && !ok[size_t(d.program)]) d.program = -1;
                if (d.program >= 0)
                    d.screen = original::screenSamplers(*prep.materials.at(s.materialId), s.instanceTextures,
                                                        prep.programs[size_t(d.program)].psGlsl.readsDepth);
                size_t k = SIZE_MAX;
                if (d.program >= 0) {
                    const Key key{d.program, s.materialId, {s.instanceTextures[0], s.instanceTextures[1], s.instanceTextures[2],
                                                            s.instanceTextures[3]}, s.alphaRef, meshes_[mi].water};
                    const auto [it, fresh] = keys.try_emplace(key, unique.size());
                    if (fresh) unique.push_back({key, &s});
                    k = it->second;
                }
                origDraws_[mi].push_back(d);
                slot[mi].push_back(k);
            }
        if (unique.empty()) return;
        origMaterialUbo_ = createBuffer(sceneMemory_, stride * unique.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t(unique.size())},
                                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(16 * unique.size())}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, uint32_t(unique.size()), 2, sizes};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &origScenePool_));
        std::vector<VkDescriptorSet> sets(unique.size());
        std::vector<VkDescriptorSetLayout> layouts(unique.size(), origMatLayout_);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, origScenePool_, uint32_t(sets.size()),
                                       layouts.data()};
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, sets.data()));
        for (size_t u = 0; u < unique.size(); ++u) {
            const auto& [key, sub] = unique[u];
            const scene::Material& mat = *prep.materials.at(key.material);
            auto& block = *reinterpret_cast<sm3::MaterialBlock*>(origMaterialUbo_.mapped + stride * u);
            original::fillMaterial(block, mat, *sub);
            if (key.water) block.fixed[3] = float(int(block.fixed[3]) | 4);
            const VkDescriptorBufferInfo bi{origMaterialUbo_.buffer, stride * u, sizeof(sm3::MaterialBlock)};
            std::array<VkDescriptorImageInfo, 16> ii;
            const sm3::Glsl& ps = prep.programs[size_t(key.program)].psGlsl;
            for (int n = 0; n < 16; ++n) {
                // The view type must match the shader's sampler: unbound cube samplers read black (as D3D9), others white.
                const bool cube = ps.samplerType[n] == 3;
                ii[size_t(n)] = {textureSampler(), cube ? blackCube_.view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                if (n == original::kAttenuationSampler && !cube)
                    ii[size_t(n)] = {postSampler_, attenuation_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                if (n == original::kSunCookieSampler && !cube && environment_.sunCookie)
                    if (const auto it = textures_.find(environment_.sunCookie); it != textures_.end() && !it->second.cube)
                        ii[size_t(n)].imageView = it->second.view;
                if (const uint32_t id = original::samplerTexture(mat, sub->instanceTextures, n))
                    if (const auto it = textures_.find(id); it != textures_.end() && it->second.cube == cube)
                        ii[size_t(n)].imageView = it->second.view;
            }
            VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstSet = w[1].dstSet = sets[u];
            w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[0].pBufferInfo = &bi;
            w[1].dstBinding = 1;
            w[1].descriptorCount = 16;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[1].pImageInfo = ii.data();
            vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
            origMaterialImages_.push_back({sets[u], ii});
        }
        for (size_t mi = 0; mi < meshes_.size(); ++mi)
            for (size_t si = 0; si < slot[mi].size(); ++si)
                if (slot[mi][si] != SIZE_MAX) {
                    auto& od = origDraws_[mi][si];
                    od.set = sets[slot[mi][si]];
                    if (od.screen) screenSets_.push_back({od.set, od.screen});
                }
        writeScreenSets();
    }

    // Back/depth-buffer samplers of the translated materials (OrigDraw::screen) -> the single-sample opaque copies;
    // rewritten with the water sets when the targets change.
    void writeScreenSets() {
        if (!opaqueColor_.view || !opaqueDepth_.view) return;
        for (const auto& [set, mask] : screenSets_)
            for (uint32_t n = 0; n < 32; ++n) {
                if (!(mask >> n & 1)) continue;
                const bool depth = n >= 16;
                const VkDescriptorImageInfo ii{depth ? fontSampler_ : postSampler_, depth ? opaqueDepth_.view : opaqueColor_.view,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = set;
                w.dstBinding = 1 + (n & 15);
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &ii;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
    }

    VkDescriptorSet textureSet(uint32_t id) const {
        auto it = textures_.find(id);
        return it != textures_.end() ? it->second.set : white_.set;
    }

    // MSAA sample count / HDR target format are baked into the scene pipelines and targets: rebuild
    // both when they change (device idle; a settings change is rare).
    void applySettings(const FrameOptions& o) {
        // FSR: the targets follow the requested mode (recreateSwapchain); FSR anti-aliases, so no MSAA with it.
        const bool fsr = o.upscaler == Upscaler::Fsr, fg = fsr && o.frameGeneration;
        const int quality = std::clamp(o.fsrQuality, 0, 4);
        if (fsr != fsrRequested_ || fg != fsrFrameGen_ || quality != fsrQuality_) {
            fsrRequested_ = fsr, fsrFrameGen_ = fg, fsrQuality_ = quality;
            fsrError_.clear();
            swapchainDirty_ = true;
        }
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        for (int s = fsrActiveRequest() ? 1 : std::clamp(o.msaa, 1, 8); s > 1; s >>= 1)
            if (supportedSamples_ & VkSampleCountFlags(s)) { samples = VkSampleCountFlagBits(s); break; }
        const VkFormat format = o.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const bool rt = rtStatus_.supported && o.rt != RayTracing::Off;
        if (samples == samples_ && format == sceneFormat_ && rt == rtPipelines_ && scenePipelines_[0]) return;
        waitIdle();
        samples_ = samples;
        sceneFormat_ = format;
        rtPipelines_ = rt;
        destroyScenePipelines();
        destroyOriginalPipelines(); // recreated on demand at the new sample count / format
        createScenePipelines();
        swapchainDirty_ = true; // targets follow the new sample count / format
    }

    void destroyScenePipelines() {
        for (VkPipeline& p : scenePipelines_) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(prepassPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(linePipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(waterPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(splashPipeline_, VK_NULL_HANDLE), nullptr);
        for (VkPipeline& p : fxPipelines_) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
    }

    // Sun shadow atlas, one tile per cascade: opaque and alpha-blended submeshes (the latter alpha-tested at 50 %),
    // both faces, slope-scaled bias baked into the pipeline. Upright viewport: uv = ndc * 0.5 + 0.5.
    void recordShadowPass(VkCommandBuffer cmd, const SunShadow& sun, const glm::mat4& zRemap) {
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        // WAR against the previous frame's sampling; contents are cleared.
        const VkImageMemoryBarrier2 toDepth = imageBarrier(
            shadow_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, kDepthStages,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        barriers(cmd, {&toDepth, 1});
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = shadow_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {uint32_t(kShadowCascades * kShadowMapSize), uint32_t(kShadowMapSize)}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        ScenePush push{};
        for (int c = 0; c < kShadowCascades; ++c) {
            const VkViewport viewport{float(c * kShadowMapSize), 0, float(kShadowMapSize), float(kShadowMapSize), 0, 1};
            const VkRect2D tile{{c * kShadowMapSize, 0}, {uint32_t(kShadowMapSize), uint32_t(kShadowMapSize)}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &tile);
            push.viewProj = zRemap * sun.lightViewProj[c];
            // Meshes whose bounds miss the cascade are skipped (identical map; the pass was ~3.4 ms in l4_ballcourt).
            for (const auto& m : meshes_) {
                if (outsideOrtho(sun.lightViewProj[c], m.center, m.radius)) continue;
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                push.model = m.model;
                for (const auto& s : m.submeshes) {
                    if (s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) continue;
                    push.alphaRef = (s.blend == scene::Blend::Alpha ? std::max<int>(s.alphaRef, 128) : s.alphaRef) / 255.0f;
                    vkCmdPushConstants(cmd, sceneLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof push, &push);
                    const VkDescriptorSet set = textureSet(s.textureId);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
                    vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
                }
            }
        }
        vkCmdEndRendering(cmd);
        const VkImageMemoryBarrier2 toRead = imageBarrier(
            shadow_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        barriers(cmd, {&toRead, 1});
    }

    void createInstance() {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loaderVersion);
        if (loaderVersion < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan loader < 1.3");

        Uint32 sdlCount = 0;
        const char* const* sdlExt = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
        if (!sdlExt) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
        std::vector<const char*> ext(sdlExt, sdlExt + sdlCount);

        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> available(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
        const bool debugUtils = hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (debugUtils) ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        // Khronos validation only when installed; on by default in Debug, OU_VK_VALIDATION=0/1 overrides.
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> layers(n);
        vkEnumerateInstanceLayerProperties(&n, layers.data());
        const char* kValidation = "VK_LAYER_KHRONOS_validation";
        const char* env = std::getenv("OU_VK_VALIDATION");
#ifdef NDEBUG
        const bool wantValidation = env && *env == '1';
#else
        const bool wantValidation = !env || *env != '0';
#endif
        validation_ = wantValidation && std::any_of(layers.begin(), layers.end(), [&](const auto& l) {
                          return std::strcmp(l.layerName, kValidation) == 0;
                      });

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "OpenUnderworld", 1, "OpenUnderworld", 1,
                              VK_API_VERSION_1_3};
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = onDebugMessage;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pNext = debugUtils ? &dci : nullptr; // also covers vkCreate/DestroyInstance
        ci.pApplicationInfo = &app;
        ci.enabledLayerCount = validation_ ? 1 : 0;
        ci.ppEnabledLayerNames = &kValidation;
        ci.enabledExtensionCount = uint32_t(ext.size());
        ci.ppEnabledExtensionNames = ext.data();
        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
        if (debugUtils) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            VK_CHECK(create(instance_, &dci, nullptr, &messenger_));
        }
    }

    // Prefers a discrete GPU. `surface` null = probe only (any graphics queue).
    void pickDevice(VkSurfaceKHR surface) {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
        int best = -1;
        for (VkPhysicalDevice gpu : gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(gpu, &p);
            if (p.apiVersion < VK_API_VERSION_1_3 || !hasExtension(deviceExtensions(gpu), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
            for (uint32_t q = 0; q < qn; ++q) {
                if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 present = VK_TRUE;
                if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, surface, &present);
                if (!present) continue;
                const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best) { best = score; gpu_ = gpu; queueFamily_ = q; }
                break;
            }
        }
        if (best < 0) throw std::runtime_error("no Vulkan 1.3 device with graphics+present queue and VK_KHR_swapchain");
    }

    void createDevice() {
        VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
        vkGetPhysicalDeviceProperties2(gpu_, &props);
        apiVersion_ = props.properties.apiVersion;
        deviceName_ = props.properties.deviceName;
        driver_ = std::string(p12.driverName) + " " + p12.driverInfo;
        if (props.properties.limits.maxPushConstantsSize < sizeof(ScenePush))
            throw std::runtime_error("maxPushConstantsSize < " + std::to_string(sizeof(ScenePush)));

        VkPhysicalDeviceFeatures supported;
        vkGetPhysicalDeviceFeatures(gpu_, &supported);
        bc_ = supported.textureCompressionBC;
        if (supported.samplerAnisotropy) maxAniso_ = props.properties.limits.maxSamplerAnisotropy;

        // Ray tracing: ray queries in the scene fragment shader against a per-scene TLAS (RENDERING.md "Ray tracing").
        std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asOn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR rqOn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &asOn};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        rtStatus_ = {};
        rtStatus_.reason = probeRayTracing(props.properties.limits);
        rtStatus_.supported = rtStatus_.reason.empty();
        if (rtStatus_.supported) {
            for (const char* e : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                  VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
                extensions.push_back(e);
            asOn.accelerationStructure = rqOn.rayQuery = VK_TRUE;
            f12.pNext = &rqOn;
            f12.bufferDeviceAddress = f12.runtimeDescriptorArray = f12.descriptorBindingPartiallyBound =
                f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        }

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        f13.shaderDemoteToHelperInvocation = VK_TRUE; // required in 1.3; glslc emits it for `discard` (alpha test)
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
        f2.features.textureCompressionBC = supported.textureCompressionBC;
        f2.features.samplerAnisotropy = supported.samplerAnisotropy;
        f2.features.shaderInt64 = rtStatus_.supported; // 64-bit buffer addresses in scene_rt.frag
        // FSR (docs/RENDERING.md "FSR"): its compute passes read and write storage images without a declared format.
        fsrReason_ = std::getenv("OU_VK_NO_FSR") ? "disabled by OU_VK_NO_FSR" : vkfsr::probe(gpu_);
        // Its DXC-compiled passes sample with implicit LOD in compute (compute shader derivatives); our mask pass blends its
        // two attachments differently (independentBlend).
        if (fsrReason_.empty() && !supported.independentBlend) fsrReason_ = "no independentBlend";
        if (fsrReason_.empty() && !hasExtension(deviceExtensions(gpu_), VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME))
            fsrReason_ = "no VK_KHR_compute_shader_derivatives";
        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR, &f13};
        if (fsrReason_.empty()) {
            extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
            derivatives.computeDerivativeGroupLinear = VK_TRUE; // what the SPIR-V declares (RADV: supported)
            f2.pNext = &derivatives;
            f2.features.independentBlend = VK_TRUE;
        }
        f2.features.shaderStorageImageReadWithoutFormat = f2.features.shaderStorageImageWriteWithoutFormat = fsrReason_.empty();
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queueFamily_, 1, &priority};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(extensions.size());
        dci.ppEnabledExtensionNames = extensions.data();
        VK_CHECK(vkCreateDevice(gpu_, &dci, nullptr, &device_));
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
        if (rtStatus_.supported) loadRayTracingFunctions();
        staticMemory_.init(device_, gpu_);
        sceneMemory_.init(device_, gpu_, kBlockSize, rtStatus_.supported);
        for (auto& m : lineMemory_) m.init(device_, gpu_, 0);
        for (auto& m : splashMemory_) m.init(device_, gpu_, 0);
        for (auto& m : fxMemory_) m.init(device_, gpu_, 0);

        for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { depthFormat_ = f; break; }
        }
        if (depthFormat_ == VK_FORMAT_UNDEFINED) throw std::runtime_error("no depth attachment format");
        constexpr VkFormatFeatureFlags kShadowFeatures =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
            if ((fp.optimalTilingFeatures & kShadowFeatures) == kShadowFeatures) { shadowFormat_ = f; break; }
        }
        if (shadowFormat_ == VK_FORMAT_UNDEFINED) throw std::runtime_error("no filterable depth format for the shadow map");
        const auto& limits = props.properties.limits;
        supportedSamples_ = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queueFamily_};
        VK_CHECK(vkCreateCommandPool(device_, &cpi, nullptr, &uploadPool_));
        for (auto& f : frames_) {
            VK_CHECK(vkCreateCommandPool(device_, &cpi, nullptr, &f.pool));
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, f.pool,
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cmd));
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
            VK_CHECK(vkCreateFence(device_, &fci, nullptr, &f.fence));
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.acquired));
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.acquiredGenerated));
            f.text = createBuffer(staticMemory_, kMaxTextVertices * sizeof(TextVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
    }

    VkShaderModule shaderModule(std::span<const uint32_t> code) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code.size_bytes(), code.data()};
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
        return m;
    }

    void createPipelines() {
        // Pipeline cache persisted under $XDG_CACHE_HOME/openunderworld; drivers ignore incompatible blobs.
        std::vector<char> blob;
        if (std::ifstream in{pipelineCachePath(), std::ios::binary}) blob.assign(std::istreambuf_iterator<char>(in), {});
        VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, blob.size(), blob.data()};
        VK_CHECK(vkCreatePipelineCache(device_, &pcci, nullptr, &pipelineCache_));

        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &binding};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &setLayout_));
        // Scene: set 0 = material texture, set 1 = sun shadow (render/sun.glsl: atlas with comparison sampler + OuShadow
        // UBO, per frame in flight), set 2 (RT devices only, used by scene_rt.frag) = TLAS, geometry records, bindless
        // textures (partially bound).
        const VkDescriptorSetLayoutBinding sb[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, sb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &shadowSetLayout_));
        if (rtStatus_.supported) {
            constexpr VkShaderStageFlags fs = VK_SHADER_STAGE_FRAGMENT_BIT;
            const VkDescriptorSetLayoutBinding rb[3] = {
                {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, fs, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fs, nullptr},
                {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxRtTextures, fs, nullptr},
            };
            const VkDescriptorBindingFlags rf[3] = {0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
            VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                              nullptr, 3, rf};
            VkDescriptorSetLayoutCreateInfo rl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags, 0, 3, rb};
            VK_CHECK(vkCreateDescriptorSetLayout(device_, &rl, nullptr, &rtSetLayout_));
        }
        const VkDescriptorSetLayout sceneSets[3] = {setLayout_, shadowSetLayout_, rtSetLayout_};
        VkPushConstantRange scenePush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePush)};
        VkPushConstantRange textPush{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
        VkPushConstantRange postPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(int32_t) + 2 * sizeof(float)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, rtSetLayout_ ? 3u : 2u,
                                      sceneSets, 1, &scenePush};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &sceneLayout_));
        const VkDescriptorSetLayout textSets[4] = {setLayout_, setLayout_, setLayout_, setLayout_}; // font + FrameOptions::uiImages
        pl.setLayoutCount = 4;
        pl.pSetLayouts = textSets;
        pl.pPushConstantRanges = &textPush;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &textLayout_));
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout_;
        // Enhanced water: set 0 = WaterBlock UBO + opaque colour/depth + ripple field; splash uses the same layout.
        const VkDescriptorSetLayoutBinding wb[4] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo wl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, wb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &wl, nullptr, &waterSetLayout_));
        // Post: set 1 = WaterBlock UBO + opaque depth (underwater, atmosphere, weather) + the screen filters' blur levels
        // (filterBlur_); set 2 = the sun shadow set (shafts).
        const VkDescriptorSetLayoutBinding pb[3] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo pwl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 3, pb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &pwl, nullptr, &postWaterSetLayout_));
        VkPushConstantRange modelPush{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo wpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &waterSetLayout_, 1,
                                       &modelPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &wpl, nullptr, &waterLayout_));
        const VkDescriptorSetLayout postSets[3] = {setLayout_, postWaterSetLayout_, shadowSetLayout_}; // 2: sun shafts
        pl.setLayoutCount = 3;
        pl.pSetLayouts = postSets;
        pl.pPushConstantRanges = &postPush;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &postLayout_));
        VkPushConstantRange linePush{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo ll{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &linePush};
        VK_CHECK(vkCreatePipelineLayout(device_, &ll, nullptr, &lineLayout_));
        // set 0 = the sprite texture, 1 = the water set's opaque colour / depth copies, 2 = the normal map (FxBatch).
        const VkDescriptorSetLayout fxSets[3] = {setLayout_, waterSetLayout_, setLayout_};
        VkPushConstantRange fxPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::mat4) + 2 * sizeof(glm::vec4)};
        VkPipelineLayoutCreateInfo fxl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 3, fxSets, 1, &fxPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &fxl, nullptr, &fxLayout_));

        const VkVertexInputBindingDescription textBinding{0, sizeof(TextVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription textAttrs[] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVertex, x)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVertex, u)},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(TextVertex, color)},
        };
        const VkFormat swap = surfaceFormat_.format;
        textPipeline_ = createPipeline(kTextVert, kTextFrag, textLayout_, &textBinding, textAttrs, PipelineKind::Overlay,
                                       VK_SAMPLE_COUNT_1_BIT, swap, VK_FORMAT_UNDEFINED);
        postPipeline_ = createPipeline(kPostVert, kPostFrag, postLayout_, nullptr, {}, PipelineKind::Post,
                                       VK_SAMPLE_COUNT_1_BIT, swap, VK_FORMAT_UNDEFINED);
        shadowPipeline_ = createPipeline(kShadowVert, kShadowFrag, sceneLayout_, &kSceneBinding, kShadowAttrs,
                                         PipelineKind::Shadow, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, shadowFormat_);

        // Original Fidelity register files (ShaderTranslate.hpp): set 0 = frame UBO, set 1 = material UBO + s0-s15,
        // set 2 = sun shadow (as the scene's set 1), set 3 = ray tracing (RT devices); push constants = object block.
        constexpr VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding fb{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, nullptr};
        VkDescriptorSetLayoutCreateInfo fl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &fb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &fl, nullptr, &origFrameLayout_));
        std::array<VkDescriptorSetLayoutBinding, 17> mb{};
        mb[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, nullptr};
        for (uint32_t n = 0; n < 16; ++n) mb[n + 1] = {1 + n, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ml{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, uint32_t(mb.size()), mb.data()};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ml, nullptr, &origMatLayout_));
        const VkDescriptorSetLayout origSets[4] = {origFrameLayout_, origMatLayout_, shadowSetLayout_, rtSetLayout_};
        VkPushConstantRange objectPush{vf, 0, sizeof(sm3::ObjectBlock)};
        VkPipelineLayoutCreateInfo ol{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, rtSetLayout_ ? 4u : 3u, origSets, 1,
                                      &objectPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &ol, nullptr, &origLayout_));
        if (fsrReason_.empty()) createFsrPipelines();
    }

    // One pipeline per scene::Blend plus the debug lines, at the current MSAA count and scene format.
    void createScenePipelines() {
        for (size_t b = 0; b < scenePipelines_.size(); ++b)
            scenePipelines_[b] = rtPipelines_ ? createPipeline(kSceneRtVert, kSceneRtFrag, sceneLayout_, &kSceneBinding, kSceneAttrs,
                                                               PipelineKind(b), samples_, sceneFormat_, depthFormat_)
                                              : createPipeline(kSceneVert, kSceneFrag, sceneLayout_, &kSceneBinding, kSceneAttrs,
                                                               PipelineKind(b), samples_, sceneFormat_, depthFormat_);
        if (rtPipelines_)
            prepassPipeline_ = createPipeline(kPrepassVert, kPrepassFrag, sceneLayout_, &kSceneBinding, kPrepassAttrs,
                                              PipelineKind::Prepass, samples_, sceneFormat_, depthFormat_);
        const VkVertexInputBindingDescription lineBinding{0, sizeof(LineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription lineAttrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, pos)},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineVertex, color)},
        };
        linePipeline_ = createPipeline(kLineVert, kLineFrag, lineLayout_, &lineBinding, lineAttrs, PipelineKind::Lines,
                                       samples_, sceneFormat_, depthFormat_);
        waterPipeline_ = createPipeline(kWaterVert, kWaterFrag, waterLayout_, &kSceneBinding, kSceneAttrs, PipelineKind::Water,
                                        samples_, sceneFormat_, depthFormat_);
        const VkVertexInputBindingDescription splashBinding{0, sizeof(SplashVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription splashAttrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SplashVertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SplashVertex, uv)},
            {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(SplashVertex, alpha)},
        };
        splashPipeline_ = createPipeline(kSplashVert, kSplashFrag, waterLayout_, &splashBinding, splashAttrs,
                                         PipelineKind::Splash, samples_, sceneFormat_, depthFormat_);
        const VkVertexInputBindingDescription fxBinding{0, sizeof(FxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription fxAttrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FxVertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FxVertex, uv)},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(FxVertex, rgba)},
        };
        for (size_t b = 0; b < fxPipelines_.size(); ++b)
            fxPipelines_[b] = createPipeline(kFxVert, kFxFrag, fxLayout_, &fxBinding, fxAttrs,
                                             PipelineKind(size_t(PipelineKind::FxAlpha) + b), samples_, sceneFormat_, depthFormat_);
    }

    // `binding` null = no vertex input (fullscreen triangle). colorFormat UNDEFINED = depth-only.
    VkPipeline createPipeline(std::span<const uint32_t> vs, std::span<const uint32_t> fs, VkPipelineLayout layout,
                              const VkVertexInputBindingDescription* binding,
                              std::span<const VkVertexInputAttributeDescription> attrs, PipelineKind kind,
                              VkSampleCountFlagBits samples, VkFormat colorFormat, VkFormat depthFormat) {
        VkShaderModule v = shaderModule(vs), f = shaderModule(fs);
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main", nullptr},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
                                                binding ? 1u : 0u, binding, uint32_t(attrs.size()), attrs.data()};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                  kind == PipelineKind::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                  VK_FALSE};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE; // dynamic
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        if (kind == PipelineKind::Shadow) { // = GL glPolygonOffset(2, 2)
            rs.depthBiasEnable = VK_TRUE;
            rs.depthBiasConstantFactor = 2.0f;
            rs.depthBiasSlopeFactor = 2.0f;
        }
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        // With the RT depth pre-pass, opaque colour draws only test (each visible pixel traces once).
        const bool prepassed = rtPipelines_ && kind == PipelineKind::Opaque;
        const bool writesDepth = (kind == PipelineKind::Opaque && !prepassed) || kind == PipelineKind::Shadow ||
                                 kind == PipelineKind::Prepass;
        ds.depthTestEnable = depthFormat != VK_FORMAT_UNDEFINED;
        ds.depthWriteEnable = writesDepth;
        // As GL: LESS for depth writers; blended submeshes (translucent pass) and debug lines LEQUAL.
        ds.depthCompareOp = writesDepth ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = kind == PipelineKind::Prepass ? 0 : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
        auto factors = [&](VkBlendFactor src, VkBlendFactor dst) {
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = src;
            blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = dst;
        };
        if (kind == PipelineKind::Alpha || kind == PipelineKind::Overlay || kind == PipelineKind::Splash)
            factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        else if (kind == PipelineKind::Additive) factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);
        else if (kind == PipelineKind::Average) factors(VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_COLOR);
        else if (kind == PipelineKind::FxAlpha) factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        else if (kind == PipelineKind::FxAdditive || kind == PipelineKind::FxSubtract) {
            factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);
            if (kind == PipelineKind::FxSubtract) blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        } else if (kind == PipelineKind::FxMultiply) factors(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA);
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const bool hasColor = colorFormat != VK_FORMAT_UNDEFINED;
        cb.attachmentCount = hasColor ? 1 : 0;
        cb.pAttachments = &blend;
        for (float& c : cb.blendConstants) c = 0.5f; // Blend::Average = D3DBLEND_BLENDFACTOR 0x80808080
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 3, dyn};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = hasColor ? 1 : 0;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = depthFormat;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &p);
        vkDestroyShaderModule(device_, v, nullptr);
        vkDestroyShaderModule(device_, f, nullptr);
        check(r, "vkCreateGraphicsPipelines");
        return p;
    }

    // ---- AMD FSR (docs/RENDERING.md "FSR") ----
    // Mask pass pipelines: attachment 0 = motion (RG16F), 1 = reactivity (R8), the scene depth read-only. Motion: moving
    // meshes over the camera motion (LEQUAL, pulled toward the camera: the translated programs' positions differ in the
    // last bits). Reactive*: max-blended into attachment 1 (meshes; fx sprites and splashes from their own vertex
    // streams; rain / snow fullscreen). Camera: fullscreen, writes both, no depth.
    enum class FsrPipeline { Motion, Reactive, Camera, FxReactive, SplashReactive, WeatherReactive };
    VkPipeline createFsrPipeline(FsrPipeline kind) {
        const bool fullscreen = kind == FsrPipeline::Camera || kind == FsrPipeline::WeatherReactive;
        const bool reactive = kind != FsrPipeline::Motion && kind != FsrPipeline::Camera;
        std::span<const uint32_t> vs = kFsrMaskVert, fs = kFsrMaskFrag;
        VkPipelineLayout layout = fsrMaskLayout_;
        const VkVertexInputBindingDescription meshBindings[2] = {kSceneBinding, {1, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
        const VkVertexInputAttributeDescription meshAttrs[4] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
            {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(scene::Vertex, color)},
            {4, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
        };
        const VkVertexInputBindingDescription fxBinding{0, sizeof(FxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription fxAttrs[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FxVertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FxVertex, uv)},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(FxVertex, rgba)},
        };
        const VkVertexInputBindingDescription splashBinding{0, sizeof(SplashVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription splashAttrs[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SplashVertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SplashVertex, uv)},
            {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(SplashVertex, alpha)},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
                                                2, meshBindings, 4, meshAttrs};
        switch (kind) {
        case FsrPipeline::Camera: vs = kPostVert, fs = kFsrCameraFrag, layout = fsrCameraLayout_, vi = {vi.sType}; break;
        case FsrPipeline::WeatherReactive: vs = kPostVert, fs = kFsrWeatherFrag, layout = postLayout_, vi = {vi.sType}; break;
        case FsrPipeline::FxReactive: vs = kFxVert, fs = kFsrFxFrag, layout = fsrFxLayout_, vi = {vi.sType, nullptr, 0, 1, &fxBinding, 3, fxAttrs}; break;
        case FsrPipeline::SplashReactive:
            vs = kSplashVert, fs = kFsrSplashFrag, layout = waterLayout_, vi = {vi.sType, nullptr, 0, 1, &splashBinding, 3, splashAttrs};
            break;
        default: break;
        }
        VkShaderModule v = shaderModule(vs);
        VkShaderModule f = shaderModule(fs);
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main", nullptr},
        };
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        rs.depthBiasEnable = !fullscreen;
        rs.depthBiasConstantFactor = -64.0f;
        rs.depthBiasSlopeFactor = -1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = !fullscreen;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend[2]{};
        constexpr VkColorComponentFlags kRG = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
        blend[0].colorWriteMask = reactive ? 0 : kRG;
        blend[1].colorWriteMask = kind == FsrPipeline::Motion ? 0 : VK_COLOR_COMPONENT_R_BIT;
        if (reactive) {
            blend[1].blendEnable = VK_TRUE;
            blend[1].srcColorBlendFactor = blend[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend[1].srcAlphaBlendFactor = blend[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend[1].colorBlendOp = blend[1].alphaBlendOp = VK_BLEND_OP_MAX;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 2;
        cb.pAttachments = blend;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 3, dyn};
        const VkFormat formats[2] = {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8_UNORM};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachmentFormats = formats;
        rendering.depthAttachmentFormat = depthFormat_;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &p);
        vkDestroyShaderModule(device_, v, nullptr);
        vkDestroyShaderModule(device_, f, nullptr);
        check(r, "vkCreateGraphicsPipelines (FSR)");
        return p;
    }

    struct FsrMaskPush {
        glm::mat4 jittered, current, previous;
        glm::vec4 params; // alpha test reference, reactivity, scale reactivity by texture alpha
    };
    struct FsrCameraPush {
        glm::mat4 reproject;
        glm::vec2 jitter;
    };

    void createFsrPipelines() {
        VkPushConstantRange mp{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FsrMaskPush)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &setLayout_, 1, &mp};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &fsrMaskLayout_));
        VkPushConstantRange cp{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FsrCameraPush)};
        pl.pPushConstantRanges = &cp;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &fsrCameraLayout_));
        VkPushConstantRange fp{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::mat4) + sizeof(glm::vec4)};
        pl.pPushConstantRanges = &fp; // fx.vert's view-projection + fsr_fx.frag's blend mode
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &fsrFxLayout_));
        fsrMotionPipeline_ = createFsrPipeline(FsrPipeline::Motion);
        fsrReactivePipeline_ = createFsrPipeline(FsrPipeline::Reactive);
        fsrCameraPipeline_ = createFsrPipeline(FsrPipeline::Camera);
        fsrFxPipeline_ = createFsrPipeline(FsrPipeline::FxReactive);
        fsrSplashPipeline_ = createFsrPipeline(FsrPipeline::SplashReactive);
        fsrWeatherPipeline_ = createFsrPipeline(FsrPipeline::WeatherReactive);
        const VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &fsrPool_));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, fsrPool_, 1, &setLayout_};
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &fsrDepthSet_));
    }

    void destroyFsr() {
        fsr_.destroy();
        for (VkPipeline* p : {&fsrMotionPipeline_, &fsrReactivePipeline_, &fsrCameraPipeline_, &fsrFxPipeline_, &fsrSplashPipeline_,
                              &fsrWeatherPipeline_})
            vkDestroyPipeline(device_, std::exchange(*p, VK_NULL_HANDLE), nullptr);
        for (VkPipelineLayout* l : {&fsrMaskLayout_, &fsrCameraLayout_, &fsrFxLayout_})
            vkDestroyPipelineLayout(device_, std::exchange(*l, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(fsrPool_, VK_NULL_HANDLE), nullptr);
    }

    // Render-resolution FSR inputs and display-resolution outputs (with the swapchain targets), and the FSR contexts.
    // A context failure leaves FSR off with the reason in upscalerSummary().
    void createFsrTargets() {
        fsr_.destroy();
        fsrHistory_ = false;
        if (!fsrActiveRequest()) return;
        constexpr VkImageUsageFlags kRead = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        motion_ = createImage(swapMemory_, VK_FORMAT_R16G16_SFLOAT, extent_.width, extent_.height, 1, kRead, VK_IMAGE_ASPECT_COLOR_BIT);
        reactive_ = createImage(swapMemory_, VK_FORMAT_R8_UNORM, extent_.width, extent_.height, 1, kRead, VK_IMAGE_ASPECT_COLOR_BIT);
        fsrInput_ = createImage(swapMemory_, surfaceFormat_.format, extent_.width, extent_.height, 1, kRead, VK_IMAGE_ASPECT_COLOR_BIT);
        fsrOutput_ = createImage(swapMemory_, VK_FORMAT_R8G8B8A8_UNORM, display_.width, display_.height, 1, kStore, VK_IMAGE_ASPECT_COLOR_BIT);
        if (fsrFrameGen_)
            fsrGenerated_ = createImage(swapMemory_, VK_FORMAT_R8G8B8A8_UNORM, display_.width, display_.height, 1, kStore,
                                        VK_IMAGE_ASPECT_COLOR_BIT);
        VkDescriptorImageInfo ii{fontSampler_, depth_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}; // nearest
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = fsrDepthSet_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        std::string error;
        if (!fsr_.create(device_, gpu_, extent_, display_, fsrFrameGen_, error)) {
            fsrError_ = error;
            std::fprintf(stderr, "ou: FSR off: %s\n", error.c_str());
            swapchainDirty_ = true; // native-resolution targets next frame
        }
    }
    bool fsrActiveRequest() const { return fsrRequested_ && fsrReason_.empty() && fsrError_.empty(); }

    // Motion vectors + reactivity at the render resolution after the scene (docs/RENDERING.md "FSR"). depth_ goes
    // DEPTH_ATTACHMENT -> DEPTH_READ_ONLY (tested + sampled here) -> SHADER_READ_ONLY with motion_ / reactive_ for FSR.
    void recordFsrMasks(VkCommandBuffer cmd, const FrameOptions& options, uint32_t splashCount, bool weather,
                        const glm::mat4& jitteredVP, const glm::mat4& currentVP, glm::vec2 jitterUv,
                        bool enhancedWater, bool cullBackFaces) {
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkPipelineStageFlags2 kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkAccessFlags2 kColorRW = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        const VkImageMemoryBarrier2 in[] = {
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                         kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         kDepthStages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
            imageBarrier(motion_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorOut, 0, kColorOut, kColorRW),
            imageBarrier(reactive_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorOut, 0, kColorOut, kColorRW),
        };
        barriers(cmd, in);
        VkRenderingAttachmentInfo color[2]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        for (int i = 0; i < 2; ++i) {
            color[i].imageView = i ? reactive_.view : motion_.view;
            color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the camera pass writes every pixel
            color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        const VkViewport upright{0, 0, float(extent_.width), float(extent_.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &upright);
        const FsrCameraPush cam{(fsrHistory_ ? prevViewProj_ : currentVP) * glm::inverse(jitteredVP), jitterUv};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrCameraPipeline_);
        vkCmdPushConstants(cmd, fsrCameraLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof cam, &cam);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrCameraLayout_, 0, 1, &fsrDepthSet_, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        if (weather && fsr_.reactiveUsed()) { // rain / snow coverage, as the post pass will draw it (its sets 1 + 2)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrWeatherPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 1, 1, &postWaterSets_[frameIndex_], 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 2, 1, &shadowSets_[frameIndex_], 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        const VkViewport flipped{0, float(extent_.height), float(extent_.width), -float(extent_.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &flipped);
        const glm::mat4& previousVP = fsrHistory_ ? prevViewProj_ : currentVP;
        auto draw = [&](uint32_t mi, const scene::SubMesh& s, glm::vec4 params) {
            const GpuMesh& m = meshes_[mi];
            const VkDeviceSize zero[2] = {0, 0};
            const VkBuffer vb[2] = {m.vertices.buffer, m.prevVertices.buffer ? m.prevVertices.buffer : m.vertices.buffer};
            vkCmdBindVertexBuffers(cmd, 0, 2, vb, zero);
            vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            const FsrMaskPush p{jitteredVP * m.model, currentVP * m.model, previousVP * m.model, params};
            vkCmdPushConstants(cmd, fsrMaskLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
            const VkDescriptorSet set = textureSet(s.textureId);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrMaskLayout_, 0, 1, &set, 0, nullptr);
            vkCmdSetCullMode(cmd, cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
        };
        // Meshes moved since the last frame (CPU-skinned characters, animated objects): their own motion.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrMotionPipeline_);
        for (uint32_t mi = 0; mi < meshes_.size(); ++mi) {
            const GpuMesh& m = meshes_[mi];
            if (m.movedFrame != frameCounter_ + 1 || !m.prevVertices.buffer || (enhancedWater && m.water)) continue;
            for (const auto& s : m.submeshes)
                if (s.blend == scene::Blend::Opaque) draw(mi, s, {s.alphaRef / 255.0f, 0, 0, 0});
        }
        // Reactivity: blended surfaces by their alpha (90 %), Enhanced water surfaces 60 % (animated shading with no
        // motion of its own), fx sprites and splash droplets by their contribution (90 %).
        const bool reactive = fsr_.reactiveUsed();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrReactivePipeline_);
        for (uint32_t mi = 0; reactive && mi < meshes_.size(); ++mi) {
            const GpuMesh& m = meshes_[mi];
            const bool water = enhancedWater && m.water;
            for (const auto& s : m.submeshes)
                if (water) draw(mi, s, {0, 0.6f, 0, 0});
                else if (s.blend != scene::Blend::Opaque) draw(mi, s, {s.alphaRef / 255.0f, 0.9f, 1, 0});
        }
        if (reactive && !options.fxBatches.empty() && !options.fxVertices.empty()) { // drawFrame filled fxBuffers_[frameIndex_]
            const VkDeviceSize zero = 0;
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrFxPipeline_);
            vkCmdBindVertexBuffers(cmd, 0, 1, &fxBuffers_[frameIndex_].buffer, &zero);
            vkCmdPushConstants(cmd, fsrFxLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::mat4),
                               &jitteredVP);
            for (const auto& b : options.fxBatches) {
                const glm::vec4 mode(float(b.blend), 0, 0, 0);
                vkCmdPushConstants(cmd, fsrFxLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(glm::mat4),
                                   sizeof mode, &mode);
                const VkDescriptorSet set = textureSet(b.textureId);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrFxLayout_, 0, 1, &set, 0, nullptr);
                vkCmdDraw(cmd, b.count, 1, b.first, 0);
            }
        }
        if (reactive && splashCount) { // drawFrame filled splashBuffers_[frameIndex_] (Enhanced water)
            const VkDeviceSize zero = 0;
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrSplashPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout_, 0, 1, &waterSets_[frameIndex_], 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &splashBuffers_[frameIndex_].buffer, &zero);
            vkCmdDraw(cmd, splashCount, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
        const VkImageMemoryBarrier2 out[] = {
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         kDepthStages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
            imageBarrier(motion_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorOut,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            imageBarrier(reactive_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorOut,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        };
        barriers(cmd, out);
    }

    // After the post pass wrote fsrInput_: FSR upscale into fsrOutput_, frame generation into fsrGenerated_, blits into
    // the swapchain image(s), `overlay` (UI, HUD) drawn on each, left in PRESENT_SRC. false + fsrError_ on a failed
    // dispatch (FSR then turns off at the next frame).
    bool recordFsrOutput(VkCommandBuffer cmd, const vkfsr::CameraInfo& cam, glm::vec2 jitter, float frameMs, bool reset,
                         float sharpness, const std::function<void(VkImageView)>& overlay) {
        constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr VkPipelineStageFlags2 kBlit = VK_PIPELINE_STAGE_2_BLIT_BIT;
        const VkImageMemoryBarrier2 in[] = {
            imageBarrier(fsrInput_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kCompute,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            imageBarrier(fsrOutput_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kCompute, 0, kCompute,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        };
        barriers(cmd, in);
        auto image = [](const GpuImage& i, VkFormat format, VkExtent2D e, VkImageUsageFlags usage) {
            return vkfsr::Image{i.image, format, e, usage};
        };
        constexpr VkImageUsageFlags kRead = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const auto depth = image(depth_, depthFormat_, extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const auto motion = image(motion_, VK_FORMAT_R16G16_SFLOAT, extent_, kRead);
        const auto output = image(fsrOutput_, VK_FORMAT_R8G8B8A8_UNORM, display_, kStore);
        std::string error;
        if (!fsr_.upscale(cmd, image(fsrInput_, surfaceFormat_.format, extent_, kRead), depth, motion,
                          image(reactive_, VK_FORMAT_R8_UNORM, extent_, kRead), output, jitter, frameMs, reset, sharpness, cam, error)) {
            fsrError_ = error;
            return false;
        }
        const bool generated = generatedIndex_ != UINT32_MAX;
        if (fsr_.frameGeneration()) {
            const VkImageMemoryBarrier2 fg[] = {
                imageBarrier(fsrOutput_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
                imageBarrier(fsrGenerated_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kCompute, 0, kCompute,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            };
            barriers(cmd, fg);
            if (!fsr_.generate(cmd, output, depth, motion, image(fsrGenerated_, VK_FORMAT_R8G8B8A8_UNORM, display_, kStore),
                               jitter, frameMs, reset, cam, error)) {
                fsrError_ = error;
                return false;
            }
        }
        const VkImageLayout outputLayout = fsr_.frameGeneration() ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        // Blit (RGBA8 -> swapchain format) and the UI on top; the generated image first (presented first).
        auto finish = [&](const GpuImage& src, VkImageLayout srcLayout, uint32_t index) {
            const VkImage dst = swapImages_[index];
            const VkImageMemoryBarrier2 toBlit[] = {
                imageBarrier(src.image, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kCompute,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kBlit,
                             VK_ACCESS_2_TRANSFER_READ_BIT),
                // Chained to the acquire semaphore wait (BLIT stage); old contents discarded.
                imageBarrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kBlit, 0, kBlit,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT),
            };
            barriers(cmd, toBlit);
            VkImageBlit region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = region.dstOffsets[1] = {int32_t(display_.width), int32_t(display_.height), 1};
            vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region, VK_FILTER_NEAREST);
            const VkImageMemoryBarrier2 toUi = imageBarrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                            kBlit, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barriers(cmd, {&toUi, 1});
            overlay(swapViews_[index]);
            const VkImageMemoryBarrier2 end = imageBarrier(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
            barriers(cmd, {&end, 1});
        };
        if (generated) finish(fsrGenerated_, VK_IMAGE_LAYOUT_GENERAL, generatedIndex_);
        finish(fsrOutput_, outputLayout, imageIndex_);
        return true;
    }

    void savePipelineCache() {
        const auto path = pipelineCachePath();
        size_t size = 0;
        if (path.empty() || !pipelineCache_ || vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS)
            return;
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary).write(data.data(), std::streamsize(size));
    }

    VkSamplerCreateInfo sceneSamplerInfo(float mipBias) const {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.anisotropyEnable = maxAniso_ > 1.0f;
        sci.maxAnisotropy = maxAniso_;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.mipLodBias = mipBias;
        return sci;
    }

    // Texture sampler of the scene's texture and material sets: sceneSampler_, or with FSR biasedSampler_.
    VkSampler textureSampler() const { return biasedSampler_ ? biasedSampler_ : sceneSampler_; }

    // FSR: textures sample with mip bias log2(render / display width) - 1 (AMD's FSR guidance) so the upscaled image
    // keeps display-resolution texture detail; native rendering keeps sceneSampler_ (bias 0). Rewrites the texture
    // and material sets; device idle (recreateSwapchain). The RT set's alpha tests keep sceneSampler_.
    void setMipBias(float bias) {
        if (bias == mipBias_) return;
        const VkSampler old = textureSampler();
        VkSampler biased = VK_NULL_HANDLE;
        if (bias != 0.0f) {
            const VkSamplerCreateInfo sci = sceneSamplerInfo(bias);
            VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &biased));
        }
        mipBias_ = bias;
        biasedSampler_ = biased;
        for (const auto& [_, img] : textures_)
            if (!img.cube) writeSet(img.set, img.view, textureSampler());
        for (auto& [set, ii] : origMaterialImages_) {
            for (auto& i : ii)
                if (i.sampler == old) i.sampler = textureSampler();
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set, w.dstBinding = 1, w.descriptorCount = 16;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = ii.data();
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
        writeScreenSets(); // the back/depth-buffer samplers rewritten above
        if (old != sceneSampler_) vkDestroySampler(device_, old, nullptr);
    }

    void createStaticResources() {
        VkSamplerCreateInfo sci = sceneSamplerInfo(0.0f);
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &sceneSampler_));
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.anisotropyEnable = VK_FALSE;
        sci.maxAnisotropy = 1.0f;
        sci.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &fontSampler_));
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; // post: FXAA taps between texels
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &postSampler_));
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, sci.maxLod = VK_LOD_CLAMP_NONE; // screen-filter blur levels
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &blurSampler_));
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, sci.maxLod = 0.0f;
        sci.compareEnable = VK_TRUE; // shadow map: hardware 2x2 PCF per tap (GL_COMPARE_REF_TO_TEXTURE, LEQUAL)
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &shadowSampler_));

        // post, white, font, the three UI images + the per-frame sun shadow sets (atlas + OuShadow UBO).
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 + kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 6 + kFramesInFlight, 2, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &staticPool_));

        // Sun shadow atlas (kShadowCascades tiles side by side): always allocated so the shadow sets are valid; kept in
        // SHADER_READ_ONLY_OPTIMAL outside the shadow pass.
        shadow_ = createImage(staticMemory_, shadowFormat_, kShadowCascades * kShadowMapSize, kShadowMapSize, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            shadowUbo_[i] = createBuffer(staticMemory_, sizeof(SunShadowBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, staticPool_, 1, &shadowSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &sai, &shadowSets_[i]));
            const VkDescriptorImageInfo ii{shadowSampler_, shadow_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorBufferInfo bi{shadowUbo_[i].buffer, 0, sizeof(SunShadowBlock)};
            VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstSet = w[1].dstSet = shadowSets_[i];
            w[0].descriptorCount = w[1].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].pImageInfo = &ii;
            w[1].dstBinding = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[1].pBufferInfo = &bi;
            vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
        }
        oneShot([&](VkCommandBuffer cmd) {
            const auto b = imageBarrier(shadow_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            barriers(cmd, {&b, 1});
        });
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, staticPool_, 1, &setLayout_};
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &postSet_)); // written per swapchain (recreateSwapchain)

        // Enhanced water: one WaterBlock UBO and one water + post-water set per frame in flight.
        const VkDescriptorPoolSize wps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kFramesInFlight},
                                             {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}};
        VkDescriptorPoolCreateInfo wpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2 * kFramesInFlight, 2, wps};
        VK_CHECK(vkCreateDescriptorPool(device_, &wpci, nullptr, &waterPool_));
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            waterUbo_[i] = createBuffer(staticMemory_, sizeof(WaterBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDescriptorSetAllocateInfo wai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, waterPool_, 1,
                                            &waterSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &wai, &waterSets_[i]));
            wai.pSetLayouts = &postWaterSetLayout_;
            VK_CHECK(vkAllocateDescriptorSets(device_, &wai, &postWaterSets_[i]));
        }
        writeWaterSets();

        const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        white_ = createImage(staticMemory_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        white_.set = allocateSet(staticPool_, white_.view, sceneSampler_);
        font_ = createImage(staticMemory_, VK_FORMAT_R8_UNORM, kAtlasW, kAtlasH, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        font_.set = allocateSet(staticPool_, font_.view, fontSampler_);
        static const uint32_t white = 0xFFFFFFFF;
        const std::vector<uint8_t> atlas = buildFontAtlas();
        UploadBatch batch;
        batch.images.push_back({white_.image, {{&white, 4, 1, 1}}});
        batch.images.push_back({font_.image, {{atlas.data(), atlas.size(), uint32_t(kAtlasW), uint32_t(kAtlasH)}}});
        // Original Fidelity: AttenuationSampler (s15) curve atlas and one frame UBO + set per frame in flight.
        blackCube_ = createImage(staticMemory_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_SAMPLE_COUNT_1_BIT, true);
        static const uint32_t black[6] = {0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000};
        batch.images.push_back({blackCube_.image, {{black, sizeof black, 1, 1}}, 6});
        const auto lut = original::attenuationLut();
        attenuation_ = createImage(staticMemory_, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(lut.width), uint32_t(lut.height), 1, usage,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        batch.images.push_back({attenuation_.image, {{lut.rgba.data(), lut.rgba.size(), uint32_t(lut.width), uint32_t(lut.height)}}});
        flush(batch);
        VkDescriptorPoolSize ups{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight};
        VkDescriptorPoolCreateInfo upci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 1, &ups};
        VK_CHECK(vkCreateDescriptorPool(device_, &upci, nullptr, &origStaticPool_));
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            origFrameUbo_[i] = createBuffer(staticMemory_, sizeof(sm3::FrameBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDescriptorSetAllocateInfo fai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, origStaticPool_, 1, &origFrameLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &fai, &origFrameSet_[i]));
            const VkDescriptorBufferInfo bi{origFrameUbo_[i].buffer, 0, sizeof(sm3::FrameBlock)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = origFrameSet_[i];
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }

    Buffer createBuffer(LinearAllocator& mem, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
        Buffer b;
        VK_CHECK(vkCreateBuffer(device_, &ci, nullptr, &b.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, b.buffer, &req);
        auto a = mem.alloc(req, props);
        VK_CHECK(vkBindBufferMemory(device_, b.buffer, a.memory, a.offset));
        b.mapped = a.mapped;
        return b;
    }

    GpuImage createImage(LinearAllocator& mem, VkFormat format, uint32_t w, uint32_t h, uint32_t levels,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                      bool cube = false) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = levels;
        ci.arrayLayers = cube ? 6 : 1;
        ci.samples = samples;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        GpuImage img;
        VK_CHECK(vkCreateImage(device_, &ci, nullptr, &img.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, img.image, &req);
        auto a = mem.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(device_, img.image, a.memory, a.offset));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &img.view));
        return img;
    }

    void destroyImage(GpuImage& img) {
        vkDestroyImageView(device_, img.view, nullptr);
        vkDestroyImage(device_, img.image, nullptr);
        img = {};
    }

    VkDescriptorSet allocateSet(VkDescriptorPool pool, VkImageView view, VkSampler sampler) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &setLayout_};
        VkDescriptorSet set;
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &set));
        writeSet(set, view, sampler);
        return set;
    }

    void writeSet(VkDescriptorSet set, VkImageView view, VkSampler sampler) {
        VkDescriptorImageInfo ii{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    // Water + post-water sets: UBO always; image bindings refreshed when the swapchain targets or the
    // ripple field change. Until the first valid views exist (init), only the UBO is written.
    void writeWaterSets() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            const VkDescriptorBufferInfo bi{waterUbo_[i].buffer, 0, sizeof(WaterBlock)};
            VkWriteDescriptorSet u{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            u.dstSet = waterSets_[i];
            u.descriptorCount = 1;
            u.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            u.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device_, 1, &u, 0, nullptr);
            u.dstSet = postWaterSets_[i];
            vkUpdateDescriptorSets(device_, 1, &u, 0, nullptr);
            if (!white_.view) continue;
            const VkDescriptorImageInfo color{postSampler_, opaqueColor_.view ? opaqueColor_.view : white_.view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo depth{fontSampler_, opaqueDepth_.view ? opaqueDepth_.view : white_.view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo rip{postSampler_, ripples_.view ? ripples_.view : white_.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo* imgs[3] = {&color, &depth, &rip};
            for (int b = 0; b < 3; ++b) {
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = waterSets_[i];
                w.dstBinding = uint32_t(b + 1);
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = imgs[b];
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = postWaterSets_[i];
            w.dstBinding = 1;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &depth;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            const VkDescriptorImageInfo blur{blurSampler_, filterBlur_.view ? filterBlur_.view : white_.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w.dstBinding = 2;
            w.pImageInfo = &blur;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
        writeScreenSets();
    }

    // Records, submits and waits for one command buffer (uploads, readback). Not for per-frame work.
    void oneShot(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, uploadPool_,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cmd;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        record(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &fence));
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0};
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        VkResult r;
        {
            const std::lock_guard lock(queueMutex_);
            r = vkQueueSubmit2(queue_, 1, &si, fence);
        }
        if (r == VK_SUCCESS) r = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, uploadPool_, 1, &cmd);
        check(r, "one-shot submit");
    }

    // Copies every region through one host-visible staging buffer, then makes images shader-readable.
    // `after` records more work into the same submission (after the copies and their barriers).
    void flush(const UploadBatch& batch, const std::function<void(VkCommandBuffer)>& after = {}) {
        auto aligned = [](VkDeviceSize s) { return (s + 15) & ~VkDeviceSize(15); }; // BCn block + 4-byte rules
        VkDeviceSize total = 0;
        for (const auto& [_, r] : batch.buffers) total += aligned(r.size);
        for (const auto& job : batch.images)
            for (const auto& r : job.mips) total += aligned(r.size);
        if (total == 0) return;
        LinearAllocator staging;
        staging.init(device_, gpu_, 0);
        Buffer src = createBuffer(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        oneShot([&](VkCommandBuffer cmd) {
            // Write-after-read: earlier frames on this queue may still be reading these resources.
            VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
            barriers(cmd, {}, &war);
            VkDeviceSize off = 0;
            for (const auto& [dst, r] : batch.buffers) {
                std::memcpy(src.mapped + off, r.src, r.size);
                VkBufferCopy copy{off, 0, r.size};
                vkCmdCopyBuffer(cmd, src.buffer, dst, 1, &copy);
                off += aligned(r.size);
            }
            std::vector<VkImageMemoryBarrier2> b;
            for (const auto& job : batch.images)
                b.push_back(imageBarrier(job.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT));
            barriers(cmd, b);
            for (const auto& job : batch.images) {
                for (uint32_t level = 0; level < job.mips.size(); ++level) {
                    const auto& r = job.mips[level];
                    std::memcpy(src.mapped + off, r.src, r.size);
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = off;
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, job.layers};
                    copy.imageExtent = {r.width, r.height, 1};
                    vkCmdCopyBufferToImage(cmd, src.buffer, job.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    off += aligned(r.size);
                }
            }
            b.clear();
            for (const auto& job : batch.images)
                b.push_back(imageBarrier(job.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
            VkMemoryBarrier2 vertex{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT};
            barriers(cmd, b, &vertex);
            if (after) after(cmd);
        });
        vkDestroyBuffer(device_, src.buffer, nullptr);
        staging.reset();
    }

    // FrameOptions::uiImages[i] -> RGBA8 texture (recreated on a size change, uploaded when the version changes).
    // Its descriptor set is allocated once and rewritten when the image is recreated.
    void updateUiImage(UiImage& u, const RgbaImage& img) {
        if (img.rgba.empty()) return;
        if (!u.gpu.image || u.width != img.width || u.height != img.height) {
            const VkDescriptorSet set = u.gpu.set;
            if (u.gpu.image) {
                waitIdle();
                destroyImage(u.gpu);
            }
            u.gpu = createImage(staticMemory_, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(img.width), uint32_t(img.height), 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            if (set) writeSet(u.gpu.set = set, u.gpu.view, postSampler_);
            else u.gpu.set = allocateSet(staticPool_, u.gpu.view, postSampler_);
            u.width = img.width, u.height = img.height;
            u.version = 0;
        }
        if (u.version == img.version) return;
        UploadBatch batch;
        batch.images.push_back({u.gpu.image, {{img.rgba.data(), img.rgba.size_bytes(), uint32_t(img.width), uint32_t(img.height)}}});
        flush(batch);
        u.version = img.version;
    }

    // Game ripple height field -> R32F texture, re-uploaded when its version changes (game::WaterSim).
    void updateRipples(const FrameOptions& options) {
        const auto& rip = options.ripples;
        if (rip.cellSize <= 0 || rip.size <= 0 || rip.heights.empty()) return;
        if (!ripples_.image || rippleSize_ != rip.size) {
            if (ripples_.image) {
                waitIdle();
                destroyImage(ripples_);
            }
            ripples_ = createImage(sceneMemory_, VK_FORMAT_R32_SFLOAT, uint32_t(rip.size), uint32_t(rip.size), 1,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            rippleSize_ = rip.size;
            rippleVersion_ = 0;
        }
        if (rippleVersion_ == rip.version) return;
        UploadBatch batch;
        batch.images.push_back({ripples_.image,
                                {{rip.heights.data(), rip.heights.size_bytes(), uint32_t(rip.size), uint32_t(rip.size)}}});
        flush(batch);
        rippleVersion_ = rip.version;
        writeWaterSets();
    }

    // Original screen filters: the resolved scene (sceneColor_, COLOR_ATTACHMENT) blitted to half size into filterBlur_
    // mip 0, each further mip from the one above (linear); the blur levels the post pass samples (shaders/filter.glsl).
    // Leaves sceneColor_ TRANSFER_SRC and filterBlur_ SHADER_READ_ONLY.
    void recordFilterBlur(VkCommandBuffer cmd) {
        constexpr VkPipelineStageFlags2 kBlit = VK_PIPELINE_STAGE_2_BLIT_BIT;
        const VkImageMemoryBarrier2 pre[] = {
            imageBarrier(sceneColor_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kBlit,
                         VK_ACCESS_2_TRANSFER_READ_BIT),
            imageBarrier(filterBlur_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, kBlit, VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        barriers(cmd, pre);
        const int32_t w0 = int32_t(std::max(1u, extent_.width / 2)), h0 = int32_t(std::max(1u, extent_.height / 2));
        for (uint32_t level = 0; level < filterLevels_; ++level) {
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level ? level - 1 : 0, 0, 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            b.srcOffsets[1] = level ? VkOffset3D{std::max(1, w0 >> (level - 1)), std::max(1, h0 >> (level - 1)), 1}
                                    : VkOffset3D{int32_t(extent_.width), int32_t(extent_.height), 1};
            b.dstOffsets[1] = {std::max(1, w0 >> level), std::max(1, h0 >> level), 1};
            if (level) {
                auto up = imageBarrier(filterBlur_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       kBlit, VK_ACCESS_2_TRANSFER_WRITE_BIT, kBlit, VK_ACCESS_2_TRANSFER_READ_BIT);
                up.subresourceRange.baseMipLevel = level - 1, up.subresourceRange.levelCount = 1;
                barriers(cmd, {&up, 1});
            }
            vkCmdBlitImage(cmd, level ? filterBlur_.image : sceneColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           filterBlur_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        }
        VkImageMemoryBarrier2 post[2] = {
            imageBarrier(filterBlur_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kBlit,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            imageBarrier(filterBlur_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kBlit,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        };
        post[0].subresourceRange.levelCount = filterLevels_ - 1;
        post[1].subresourceRange.baseMipLevel = filterLevels_ - 1, post[1].subresourceRange.levelCount = 1;
        barriers(cmd, {post + (filterLevels_ > 1 ? 0 : 1), size_t(filterLevels_ > 1 ? 2 : 1)});
    }

    // Between the scene pass and the water pass: copy the resolved opaque colour (and depth, when
    // there is no MSAA resolve) into the single-sample textures the water and post shaders sample.
    void recordWaterCopies(VkCommandBuffer cmd) {
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        std::vector<VkImageMemoryBarrier2> pre = {
            imageBarrier(sceneColor_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
            imageBarrier(opaqueColor_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT),
        };
        if (!msColor_.image) {
            pre.push_back(imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kDepthStages,
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
            pre.push_back(imageBarrier(opaqueDepth_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
        }
        barriers(cmd, pre);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {extent_.width, extent_.height, 1};
        vkCmdCopyImage(cmd, sceneColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opaqueColor_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (!msColor_.image) {
            region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.dstSubresource = region.srcSubresource;
            vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opaqueDepth_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        std::vector<VkImageMemoryBarrier2> post = {
            imageBarrier(opaqueColor_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            imageBarrier(opaqueDepth_.image,
                         msColor_.image ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         msColor_.image ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
                         msColor_.image ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT, // resolve
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT),
            imageBarrier(sceneColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, // pass 2 loads it without MSAA (FSR)
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        if (!msColor_.image)
            post.push_back(imageBarrier(depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_READ_BIT, kDepthStages,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                        VK_IMAGE_ASPECT_DEPTH_BIT));
        barriers(cmd, post);
    }

    // ---- Ray tracing (VK_KHR_acceleration_structure + VK_KHR_ray_query), RENDERING.md "Ray tracing" ----

    // Empty = usable. OU_VK_NO_RT=1 simulates an unsupported device (fallback testing).
    std::string probeRayTracing(const VkPhysicalDeviceLimits& limits) {
        if (const char* off = std::getenv("OU_VK_NO_RT"); off && *off == '1') return "OU_VK_NO_RT=1";
        const auto ext = deviceExtensions(gpu_);
        for (const char* e : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
            if (!hasExtension(ext, e)) return std::string("no ") + e;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &as};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &rq};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(gpu_, &f);
        if (!as.accelerationStructure || !rq.rayQuery) return "accelerationStructure/rayQuery feature off";
        if (!f12.bufferDeviceAddress || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound ||
            !f12.shaderSampledImageArrayNonUniformIndexing || !f.features.shaderInt64)
            return "missing bufferDeviceAddress/descriptor indexing/shaderInt64";
        // Translated programs: 16 material samplers + the shadow atlas next to the bindless array; 4 sets.
        if (limits.maxPerStageDescriptorSampledImages < kMaxRtTextures + 18 || limits.maxPerStageDescriptorSamplers < kMaxRtTextures + 18 ||
            limits.maxDescriptorSetSampledImages < kMaxRtTextures + 18 || limits.maxBoundDescriptorSets < 4)
            return "descriptor limits below " + std::to_string(kMaxRtTextures) + " textures";
        VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ap};
        vkGetPhysicalDeviceProperties2(gpu_, &p);
        scratchAlign_ = std::max<VkDeviceSize>(ap.minAccelerationStructureScratchOffsetAlignment, 1);
        return {};
    }

    void loadRayTracingFunctions() {
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vkGetDeviceProcAddr(device_, name));
            if (!fn) throw std::runtime_error(std::string("missing ") + name);
        };
        load(vkCreateAccelerationStructureKHR_, "vkCreateAccelerationStructureKHR");
        load(vkDestroyAccelerationStructureKHR_, "vkDestroyAccelerationStructureKHR");
        load(vkGetAccelerationStructureBuildSizesKHR_, "vkGetAccelerationStructureBuildSizesKHR");
        load(vkCmdBuildAccelerationStructuresKHR_, "vkCmdBuildAccelerationStructuresKHR");
        load(vkGetAccelerationStructureDeviceAddressKHR_, "vkGetAccelerationStructureDeviceAddressKHR");
    }

    VkDeviceAddress address(VkBuffer b) const {
        VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, b};
        return vkGetBufferDeviceAddress(device_, &i);
    }

    // Build (or refit: mode UPDATE) info for a BLAS. Skinned meshes are built for refits, the rest for tracing speed.
    VkAccelerationStructureBuildGeometryInfoKHR blasInfo(const GpuMesh& m, VkBuildAccelerationStructureModeKHR mode) const {
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = m.skinned ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                             : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = mode;
        bi.srcAccelerationStructure = mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? m.blas : VK_NULL_HANDLE;
        bi.dstAccelerationStructure = m.blas;
        bi.geometryCount = uint32_t(m.rtGeometries.size());
        bi.pGeometries = m.rtGeometries.data();
        return bi;
    }

    VkAccelerationStructureBuildGeometryInfoKHR tlasInfo(VkBuildAccelerationStructureModeKHR mode) const {
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = mode;
        bi.srcAccelerationStructure = mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? tlas_ : VK_NULL_HANDLE;
        bi.dstAccelerationStructure = tlas_;
        bi.geometryCount = 1;
        bi.pGeometries = &tlasGeometry_;
        return bi;
    }

    // Acceleration structure + its backing buffer (scene arena) of `size` bytes.
    VkAccelerationStructureKHR createAs(VkAccelerationStructureTypeKHR type, VkDeviceSize size, Buffer& backing) {
        backing = createBuffer(sceneMemory_, size,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = backing.buffer;
        ci.size = size;
        ci.type = type;
        VkAccelerationStructureKHR as;
        VK_CHECK(vkCreateAccelerationStructureKHR_(device_, &ci, nullptr, &as));
        return as;
    }

    static void asBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        const VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, srcStage, srcAccess, dstStage, dstAccess};
        barriers(cmd, {}, &m);
    }
    static constexpr VkPipelineStageFlags2 kAsBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    static constexpr VkAccessFlags2 kAsRead = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                                    kAsWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

    // After upload(): one BLAS per mesh with traced submeshes (Opaque + Alpha blends; Additive / Average
    // neither cast shadows nor reflect, as in the shadow map), one TLAS instance per BLAS, the geometry
    // record buffer and the set-2 descriptors. Static meshes don't move (GpuMesh::model is fixed), so
    // instance transforms are written once.
    void buildRayTracing() {
        std::vector<RtGeometry> records;
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        struct Pending { GpuMesh* mesh; VkDeviceSize scratch; };
        std::vector<Pending> pending;
        VkDeviceSize maxScratch = 0, sumScratch = 0;
        for (GpuMesh& m : meshes_) {
            const VkDeviceAddress va = address(m.vertices.buffer), ia = address(m.indices.buffer);
            const uint32_t base = uint32_t(records.size());
            for (const scene::SubMesh& s : m.submeshes) {
                if ((s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) || s.indexCount < 3) continue;
                const uint8_t ref = s.blend == scene::Blend::Alpha ? std::max<uint8_t>(s.alphaRef, 128) : s.alphaRef;
                VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                g.flags = ref == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0; // non-opaque: alpha test in scene_rt.frag
                auto& t = g.geometry.triangles;
                t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                t.vertexData.deviceAddress = va;
                t.vertexStride = sizeof(scene::Vertex);
                t.maxVertex = uint32_t(m.vertexCount - 1);
                t.indexType = VK_INDEX_TYPE_UINT32;
                t.indexData.deviceAddress = ia;
                m.rtGeometries.push_back(g);
                m.rtRanges.push_back({s.indexCount / 3, s.firstIndex * uint32_t(sizeof(uint32_t)), 0, 0});
                auto tex = textures_.find(s.textureId);
                records.push_back({va, ia, s.firstIndex, tex != textures_.end() ? tex->second.slot : 0, ref / 255.0f, 0});
            }
            if (m.rtGeometries.empty()) continue;
            if (records.size() >= (1u << 24)) throw std::runtime_error("RT: more than 2^24 geometries");
            std::vector<uint32_t> counts;
            for (const auto& r : m.rtRanges) counts.push_back(r.primitiveCount);
            const auto bi = blasInfo(m, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, counts.data(), &sz);
            m.blas = createAs(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize, m.blasBuffer);
            const VkDeviceSize scratch = m.rtScratch = alignUp(std::max(sz.buildScratchSize, sz.updateScratchSize));
            pending.push_back({&m, scratch});
            maxScratch = std::max(maxScratch, scratch);
            sumScratch += scratch;

            VkAccelerationStructureInstanceKHR inst{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) inst.transform.matrix[r][c] = m.model[c][r]; // glm is column-major
            inst.instanceCustomIndex = base;
            inst.mask = m.water ? 2 : 1; // scene_rt.frag kMaskWater / kMaskSolid
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR; // both faces, as the shadow pass
            VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                           nullptr, m.blas};
            inst.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR_(device_, &ai);
            instances.push_back(inst);
            rtStatus_.waterInstances += m.water;
        }
        rtStatus_.blas = rtStatus_.instances = tlasInstanceCount_ = uint32_t(instances.size());
        rtStatus_.geometries = uint32_t(records.size());

        // Instance + record buffers (a 0-instance TLAS is valid and keeps set 2 complete).
        const VkBufferUsageFlags copyDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        instanceBuffer_ = createBuffer(sceneMemory_, std::max<size_t>(1, instances.size()) * sizeof(VkAccelerationStructureInstanceKHR),
                                       copyDst | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        geometryBuffer_ = createBuffer(sceneMemory_, std::max<size_t>(1, records.size()) * sizeof(RtGeometry),
                                       copyDst | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        UploadBatch batch;
        if (!instances.empty()) batch.buffers.push_back({instanceBuffer_.buffer, {instances.data(), instances.size() * sizeof(instances[0]), 0, 0}});
        if (!records.empty()) batch.buffers.push_back({geometryBuffer_.buffer, {records.data(), records.size() * sizeof(RtGeometry), 0, 0}});
        flush(batch);

        tlasGeometry_ = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlasGeometry_.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry_.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeometry_.geometry.instances.data.deviceAddress = address(instanceBuffer_.buffer);
        auto ti = tlasInfo(VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
        VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &tlasInstanceCount_, &tsz);
        tlas_ = createAs(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tsz.accelerationStructureSize, tlasBuffer_);
        ti.dstAccelerationStructure = tlas_;

        // Scratch: a shared BLAS region (upload batches, later refits/rebuilds one at a time) + the TLAS's own.
        const VkDeviceSize blasScratch = std::max(std::min(sumScratch, kRtScratchBudget), maxScratch);
        const VkDeviceSize tlasScratch = alignUp(std::max(tsz.buildScratchSize, tsz.updateScratchSize));
        scratch_ = createBuffer(sceneMemory_, blasScratch + tlasScratch + scratchAlign_,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        scratchBlas_ = alignUp(address(scratch_.buffer));
        scratchTlas_ = scratchBlas_ + blasScratch;

        oneShot([&](VkCommandBuffer cmd) {
            // Vertex/index/instance uploads (earlier submissions) → build inputs and fragment-shader reads.
            asBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      kAsBuild | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | kAsRead);
            std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
            VkDeviceSize used = 0;
            auto submit = [&] {
                if (infos.empty()) return;
                vkCmdBuildAccelerationStructuresKHR_(cmd, uint32_t(infos.size()), infos.data(), ranges.data());
                asBarrier(cmd, kAsBuild, kAsWrite, kAsBuild, kAsRead | kAsWrite); // scratch reuse + TLAS input
                infos.clear(), ranges.clear(), used = 0;
            };
            for (const Pending& p : pending) {
                if (used + p.scratch > blasScratch) submit();
                auto bi = blasInfo(*p.mesh, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
                bi.scratchData.deviceAddress = scratchBlas_ + used;
                used += p.scratch;
                infos.push_back(bi);
                ranges.push_back(p.mesh->rtRanges.data());
            }
            submit();
            ti.scratchData.deviceAddress = scratchTlas_;
            const VkAccelerationStructureBuildRangeInfoKHR tr{tlasInstanceCount_, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
            vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &ti, &trp);
            asBarrier(cmd, kAsBuild, kAsWrite, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
        });
        ++rtStatus_.tlasBuilds;

        // Set 2: TLAS, geometry records, every scene texture at its slot (unused slots stay unbound).
        const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxRtTextures}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 3, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &rtPool_));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, rtPool_, 1, &rtSetLayout_};
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &rtSet_));
        VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr, 1, &tlas_};
        const VkDescriptorBufferInfo bufInfo{geometryBuffer_.buffer, 0, VK_WHOLE_SIZE};
        std::vector<VkDescriptorImageInfo> images{{sceneSampler_, white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        std::vector<uint32_t> slots{0};
        for (const auto& [_, img] : textures_)
            if (img.slot) images.push_back({sceneSampler_, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}), slots.push_back(img.slot);
        std::vector<VkWriteDescriptorSet> w(2 + images.size(), {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        w[0].pNext = &asw;
        w[0].dstSet = rtSet_, w[0].dstBinding = 0, w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[1].dstSet = rtSet_, w[1].dstBinding = 1, w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[1].pBufferInfo = &bufInfo;
        for (size_t i = 0; i < images.size(); ++i) {
            auto& x = w[2 + i];
            x.dstSet = rtSet_, x.dstBinding = 2, x.dstArrayElement = slots[i], x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, x.pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_, uint32_t(w.size()), w.data(), 0, nullptr);
    }

    VkDeviceAddress alignUp(VkDeviceAddress v) const { return (v + scratchAlign_ - 1) / scratchAlign_ * scratchAlign_; }

    // Recorded after updateVertices' copies: refit (skinned) or rebuild the meshes' BLASes in place, as few batched
    // builds as the shared scratch allows (one barrier pair per batch; serialising one build + two barriers per mesh
    // held l4_ballcourt 1440p at 51 instead of 58 fps). Earlier frames may still trace them.
    void rebuildBlases(VkCommandBuffer cmd, std::span<const size_t> slots) {
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
        VkDeviceSize used = 0;
        const VkDeviceSize capacity = scratchTlas_ - scratchBlas_;
        auto submit = [&] {
            if (infos.empty()) return;
            asBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kAsBuild,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | kAsWrite, kAsBuild, VK_ACCESS_2_SHADER_READ_BIT | kAsRead | kAsWrite);
            vkCmdBuildAccelerationStructuresKHR_(cmd, uint32_t(infos.size()), infos.data(), ranges.data());
            infos.clear(), ranges.clear(), used = 0;
        };
        for (const size_t slot : slots) {
            const GpuMesh& m = meshes_[slot];
            if (!m.blas) continue;
            if (used + m.rtScratch > capacity) submit();
            auto bi = blasInfo(m, m.skinned ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
            bi.scratchData.deviceAddress = scratchBlas_ + used;
            used += m.rtScratch;
            infos.push_back(bi);
            ranges.push_back(m.rtRanges.data());
            ++(m.skinned ? rtStatus_.blasRefits : rtStatus_.blasRebuilds);
            tlasDirty_ = true;
        }
        submit();
        if (tlasDirty_) asBarrier(cmd, kAsBuild, kAsWrite, kAsBuild | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
    }

    // Per frame when a BLAS changed: TLAS update in place (same instances; BLAS bounds moved).
    void recordTlasUpdate(VkCommandBuffer cmd) {
        asBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kAsBuild, kAsWrite, kAsBuild, kAsRead | kAsWrite);
        auto ti = tlasInfo(VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR);
        ti.scratchData.deviceAddress = scratchTlas_;
        const VkAccelerationStructureBuildRangeInfoKHR tr{tlasInstanceCount_, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
        vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &ti, &trp);
        asBarrier(cmd, kAsBuild, kAsWrite, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
        ++rtStatus_.tlasUpdates;
        tlasDirty_ = false;
    }

    void destroySceneResources() {
        destroyOriginalScene();
        for (auto& [_, img] : textures_) destroyImage(img);
        textures_.clear();
        for (auto& m : meshes_) {
            vkDestroyBuffer(device_, m.vertices.buffer, nullptr);
            vkDestroyBuffer(device_, m.prevVertices.buffer, nullptr);
            vkDestroyBuffer(device_, m.indices.buffer, nullptr);
            if (m.blas) vkDestroyAccelerationStructureKHR_(device_, m.blas, nullptr);
            vkDestroyBuffer(device_, m.blasBuffer.buffer, nullptr);
        }
        if (tlas_) vkDestroyAccelerationStructureKHR_(device_, std::exchange(tlas_, VK_NULL_HANDLE), nullptr);
        for (Buffer* b : {&tlasBuffer_, &instanceBuffer_, &geometryBuffer_, &scratch_})
            vkDestroyBuffer(device_, std::exchange(*b, {}).buffer, nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(rtPool_, VK_NULL_HANDLE), nullptr);
        rtSet_ = VK_NULL_HANDLE;
        tlasDirty_ = false;
        rtStatus_.blas = rtStatus_.instances = rtStatus_.geometries = rtStatus_.waterInstances = 0;
        meshes_.clear();
        meshSlot_.clear();
        for (auto& s : staging_) s.meshes.clear(), s.used = 0; // pending copies of the old meshes
        vkDestroyDescriptorPool(device_, scenePool_, nullptr);
        scenePool_ = VK_NULL_HANDLE;
        sceneMemory_.reset();
    }

    VkPresentModeKHR noVsyncMode() const {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    void recreateSwapchain(Viewport vp) {
        waitIdle(); // ponytail: full stall on resize; fine for a viewer
        swapchainDirty_ = false;
        requested_ = vp;
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps));
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) // Wayland: the swapchain defines the size
            extent = {std::clamp(uint32_t(vp.width), caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(uint32_t(vp.height), caps.minImageExtent.height, caps.maxImageExtent.height)};
        VkSwapchainKHR old = swapchain_;
        swapchain_ = VK_NULL_HANDLE;
        if (extent.width == 0 || extent.height == 0) { destroySwapchain(old); return; }

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        // FSR frame generation holds two images per frame (generated + real) until they are presented.
        const uint32_t extra = fsrActiveRequest() && fsrFrameGen_ ? 2 : 1;
        ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + extra, caps.maxImageCount) : caps.minImageCount + extra;
        ci.imageFormat = surfaceFormat_.format;
        ci.imageColorSpace = surfaceFormat_.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        // FSR blits its upscaled (and generated) image into the swapchain image.
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        (caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        if (fsrActiveRequest() && !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            fsrError_ = "swapchain images cannot be blit targets";
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        ci.presentMode = vsync_ ? VK_PRESENT_MODE_FIFO_KHR : noVsyncMode();
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;
        VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
        destroySwapchain(old);
        // Scene targets at the render resolution: the display's, or FSR's for its quality mode.
        display_ = extent;
        extent_ = fsrActiveRequest() ? vkfsr::renderExtent(extent, fsrQuality_) : extent;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        swapImages_.resize(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
        for (VkImage img : swapImages_) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = surfaceFormat_.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &view));
            swapViews_.push_back(view);
            // One present semaphore per image: reusable once that image is acquired again.
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s;
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &s));
            presentReady_.push_back(s);
        }
        // Scene targets: own allocator, released with the swapchain. MSAA renders into msColor_ and
        // resolves into sceneColor_, which the post pass samples.
        swapMemory_.init(device_, gpu_, 0);
        depth_ = createImage(swapMemory_, depthFormat_, extent_.width, extent_.height, 1,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 (fsrActiveRequest() ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                             VK_IMAGE_ASPECT_DEPTH_BIT, samples_);
        sceneColor_ = createImage(swapMemory_, sceneFormat_, extent_.width, extent_.height, 1,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        opaqueColor_ = createImage(swapMemory_, sceneFormat_, extent_.width, extent_.height, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        opaqueDepth_ = createImage(swapMemory_, depthFormat_, extent_.width, extent_.height, 1,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
        const uint32_t bw = std::max(1u, extent_.width / 2), bh = std::max(1u, extent_.height / 2);
        filterLevels_ = std::min(6u, 1u + uint32_t(std::log2(float(std::max(bw, bh)))));
        filterBlur_ = createImage(swapMemory_, sceneFormat_, bw, bh, filterLevels_,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        oneShot([&](VkCommandBuffer cmd) { // valid sampled layouts from the start (post binds them always)
            const VkImageMemoryBarrier2 b[3] = {
                imageBarrier(filterBlur_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
                imageBarrier(opaqueColor_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
                imageBarrier(opaqueDepth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
            };
            barriers(cmd, b);
        });
        writeWaterSets();
        if (samples_ != VK_SAMPLE_COUNT_1_BIT)
            msColor_ = createImage(swapMemory_, sceneFormat_, extent_.width, extent_.height, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, samples_);
        writeSet(postSet_, sceneColor_.view, postSampler_);
        createFsrTargets();
        setMipBias(fsr_.valid() ? std::log2(float(extent_.width) / float(display_.width)) - 1.0f : 0.0f);
    }

    // Destroys the per-swapchain objects (views, semaphores, depth) and `chain` itself. Device must be idle.
    void destroySwapchain(VkSwapchainKHR chain) {
        for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
        for (VkSemaphore s : presentReady_) vkDestroySemaphore(device_, s, nullptr);
        swapViews_.clear();
        presentReady_.clear();
        swapImages_.clear();
        for (GpuImage* img : {&depth_, &sceneColor_, &msColor_, &opaqueColor_, &opaqueDepth_, &filterBlur_, &motion_, &reactive_,
                              &fsrInput_, &fsrOutput_, &fsrGenerated_})
            if (img->image) destroyImage(*img);
        swapMemory_.reset();
        vkDestroySwapchainKHR(device_, chain, nullptr);
    }

    void chooseSurfaceFormat() {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
        if (formats.empty()) throw std::runtime_error("surface reports no formats");
        surfaceFormat_ = formats[0];
        for (const auto& f : formats) // UNORM like the GL default framebuffer (gamma-space shading, see BUILDING.md)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { surfaceFormat_ = f; break; }
    }

    SDL_Window* window_ = nullptr;
    bool libraryLoaded_ = false, validation_ = false, bc_ = false, vsync_ = true;
    bool swapchainDirty_ = true, frameActive_ = false;
    float maxAniso_ = 1.0f;
    uint32_t apiVersion_ = 0, queueFamily_ = 0, frameIndex_ = 0, imageIndex_ = 0;
    std::string deviceName_, driver_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> frames_{};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_{}; // fixed at init; pipelines are built against it
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};  // scene (render) resolution: display_, or FSR's for its quality mode
    VkExtent2D display_{}; // swapchain size
    Viewport requested_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> presentReady_;
    GpuImage depth_, sceneColor_, msColor_; // msColor_ only with MSAA
    // Enhanced water: single-sample opaque copies (refraction etc.) and the game's ripple height field.
    GpuImage opaqueColor_, opaqueDepth_, ripples_;
    GpuImage filterBlur_;        // screen filters: the half-size scene + mips (recordFilterBlur)
    uint32_t filterLevels_ = 0;
    uint64_t rippleVersion_ = 0;
    int rippleSize_ = 0;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlags supportedSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat sceneFormat_ = VK_FORMAT_R8G8B8A8_UNORM, shadowFormat_ = VK_FORMAT_UNDEFINED;

    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout sceneLayout_ = VK_NULL_HANDLE, textLayout_ = VK_NULL_HANDLE, lineLayout_ = VK_NULL_HANDLE,
                     postLayout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> scenePipelines_{}; // indexed by scene::Blend
    VkPipeline textPipeline_ = VK_NULL_HANDLE, linePipeline_ = VK_NULL_HANDLE, shadowPipeline_ = VK_NULL_HANDLE,
               postPipeline_ = VK_NULL_HANDLE, prepassPipeline_ = VK_NULL_HANDLE;
    VkPipeline waterPipeline_ = VK_NULL_HANDLE, splashPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout waterSetLayout_ = VK_NULL_HANDLE, postWaterSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout waterLayout_ = VK_NULL_HANDLE; // water + splashes: set 0 = WaterBlock + 3 textures, push model
    VkDescriptorPool waterPool_ = VK_NULL_HANDLE;
    std::array<Buffer, kFramesInFlight> waterUbo_{}; // WaterBlock, host-coherent
    std::array<VkDescriptorSet, kFramesInFlight> waterSets_{}, postWaterSets_{};
    std::array<LinearAllocator, kFramesInFlight> splashMemory_;
    std::array<Buffer, kFramesInFlight> splashBuffers_{};
    std::array<VkDeviceSize, kFramesInFlight> splashCapacity_{};
    std::array<LinearAllocator, kFramesInFlight> fxMemory_; // original effects (FrameOptions::fxVertices)
    std::array<Buffer, kFramesInFlight> fxBuffers_{};
    std::array<VkDeviceSize, kFramesInFlight> fxCapacity_{};
    std::array<VkPipeline, 4> fxPipelines_{}; // FxBatch::blend: alpha, additive, subtract, multiply
    VkPipelineLayout fxLayout_ = VK_NULL_HANDLE;
    std::array<LinearAllocator, kFramesInFlight> lineMemory_;
    VkSampler sceneSampler_ = VK_NULL_HANDLE, fontSampler_ = VK_NULL_HANDLE, shadowSampler_ = VK_NULL_HANDLE,
              postSampler_ = VK_NULL_HANDLE, biasedSampler_ = VK_NULL_HANDLE, // biasedSampler_: setMipBias
              blurSampler_ = VK_NULL_HANDLE; // screen-filter blur levels (trilinear)
    float mipBias_ = 0.0f;
    std::vector<std::pair<VkDescriptorSet, std::array<VkDescriptorImageInfo, 16>>> origMaterialImages_; // setMipBias
    VkDescriptorPool staticPool_ = VK_NULL_HANDLE, scenePool_ = VK_NULL_HANDLE;
    GpuImage white_, font_, shadow_;
    std::array<UiImage, 3> uiImages_; // FrameOptions::uiImages
    VkDescriptorSet postSet_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;           // sun.glsl: atlas + OuShadow UBO
    std::array<Buffer, kFramesInFlight> shadowUbo_{};                 // OuShadow, host-coherent
    std::array<VkDescriptorSet, kFramesInFlight> shadowSets_{};
    Bounds bounds_;
    std::vector<DrawRef> order_;
    std::vector<float> opacity_; // per frame: FrameOptions::meshOpacity per mesh (empty: all 1)

    LinearAllocator staticMemory_, sceneMemory_, swapMemory_;
    std::unordered_map<uint32_t, GpuImage> textures_;
    std::vector<GpuMesh> meshes_;
    std::vector<size_t> meshSlot_; // last upload()'s SceneData::meshes index -> meshes_ index (SIZE_MAX = skipped)

    // Original Fidelity (translated TRU shaders).
    VkDescriptorSetLayout origFrameLayout_ = VK_NULL_HANDLE, origMatLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout origLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool origStaticPool_ = VK_NULL_HANDLE, origScenePool_ = VK_NULL_HANDLE;
    std::array<Buffer, kFramesInFlight> origFrameUbo_{};
    std::array<VkDescriptorSet, kFramesInFlight> origFrameSet_{};
    GpuImage attenuation_, blackCube_;
    std::vector<OrigProgram> origPrograms_;
    int origWarmKey_ = -1; // warmOriginalPipelines' settings since the pipelines were last destroyed
    std::vector<std::pair<uint64_t, original::LightSet>> meshLights_; // meshes_ index -> (frameCounter_ + 1, light set)
    std::vector<std::vector<OrigDraw>> origDraws_; // meshes_ index -> per submesh
    std::vector<std::pair<VkDescriptorSet, uint32_t>> screenSets_; // material sets with back/depth-buffer samplers
    std::vector<DrawRef> screenDraws_;                               // per frame: their draws, for the second pass
    Buffer origMaterialUbo_;
    scene::Environment environment_;

    // Ray tracing.
    RtStatus rtStatus_;
    bool rtPipelines_ = false, tlasDirty_ = false; // scene pipelines use scene_rt.frag; TLAS needs an update
    VkDeviceSize scratchAlign_ = 256;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR_ = nullptr;
    VkDescriptorSetLayout rtSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool rtPool_ = VK_NULL_HANDLE;
    VkDescriptorSet rtSet_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    Buffer tlasBuffer_, instanceBuffer_, geometryBuffer_, scratch_;
    VkDeviceAddress scratchBlas_ = 0, scratchTlas_ = 0; // aligned scratch regions: shared BLAS builds, TLAS
    VkAccelerationStructureGeometryKHR tlasGeometry_{};
    uint32_t tlasInstanceCount_ = 0;

    // AMD FSR (docs/RENDERING.md "FSR").
    vkfsr::Context fsr_;
    std::string fsrReason_, fsrError_; // why FSR cannot run here; a runtime failure (FSR off until the settings change)
    bool fsrRequested_ = false, fsrFrameGen_ = false, fsrHistory_ = false;
    int fsrQuality_ = 1;
    GpuImage motion_, reactive_, fsrInput_, fsrOutput_, fsrGenerated_;
    VkPipelineLayout fsrMaskLayout_ = VK_NULL_HANDLE, fsrCameraLayout_ = VK_NULL_HANDLE, fsrFxLayout_ = VK_NULL_HANDLE;
    VkPipeline fsrFxPipeline_ = VK_NULL_HANDLE, fsrSplashPipeline_ = VK_NULL_HANDLE, fsrWeatherPipeline_ = VK_NULL_HANDLE;
    VkPipeline fsrMotionPipeline_ = VK_NULL_HANDLE, fsrReactivePipeline_ = VK_NULL_HANDLE, fsrCameraPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool fsrPool_ = VK_NULL_HANDLE;
    VkDescriptorSet fsrDepthSet_ = VK_NULL_HANDLE;
    glm::mat4 prevViewProj_{1.0f}; // previous frame, unjittered
    glm::vec3 prevCamPos_{0.0f}, prevCamForward_{0.0f};
    uint64_t frameCounter_ = 0, presentedGenerated_ = 0; // presented real / generated frames
    std::array<Staging, kFramesInFlight> staging_;
    // Queue + swapchain access from the render and the present thread; the present thread's job queue.
    struct PresentJob {
        VkSwapchainKHR swapchain;
        uint32_t generated, real;
        double halfMs;
    };
    std::mutex queueMutex_, presentMutex_;
    std::condition_variable presentCv_;
    std::deque<PresentJob> presentJobs_;
    bool presentQuit_ = false, presentBusy_ = false;
    std::atomic<bool> presentOutOfDate_{false};
    std::thread presentThread_;
    uint32_t generatedIndex_ = UINT32_MAX;               // this frame's generated swapchain image
    std::chrono::steady_clock::time_point lastFrame_{}, lastReady_{};
    double frameIntervalMs_ = 0.0;
    const char* captureDir_ = std::getenv("OU_VK_CAPTURE_DIR");
    bool presentStats_ = std::getenv("OU_VK_PRESENT_STATS") != nullptr;
    std::vector<std::chrono::steady_clock::time_point> presentTimes_;
    // OU_VK_PRESENT_STATS GPU pass times: timestamps after begin(0), vertex uploads + BLAS refits (1), TLAS update (2),
    // shadow map (3), scene (4), FSR masks + post (5), FSR / FG / end (6); summed per segment over timed frames.
    static constexpr uint32_t kGpuMarks = 7;
    std::array<double, kGpuMarks - 1> gpuMs_{};
    uint64_t gpuFrames_ = 0;
    void gpuBegin(Frame& f, VkCommandBuffer cmd) {
        if (!presentStats_) return;
        if (!f.gpuTimes) {
            const VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, kGpuMarks};
            VK_CHECK(vkCreateQueryPool(device_, &qi, nullptr, &f.gpuTimes));
        } else if (f.gpuTimed) { // its fence was waited: the previous results are final
            uint64_t t[kGpuMarks];
            if (vkGetQueryPoolResults(device_, f.gpuTimes, 0, kGpuMarks, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(gpu_, &props);
                for (uint32_t i = 1; i < kGpuMarks; ++i) gpuMs_[i - 1] += double(t[i] - t[i - 1]) * props.limits.timestampPeriod * 1e-6;
                ++gpuFrames_;
            }
        }
        vkCmdResetQueryPool(cmd, f.gpuTimes, 0, kGpuMarks);
        f.gpuTimed = true;
        gpuMark(cmd, 0);
    }
    void gpuMark(VkCommandBuffer cmd, uint32_t i) {
        if (presentStats_) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frames_[frameIndex_].gpuTimes, i);
    }

    // OU_VK_PRESENT_STATS: presented images per second and the spacing of consecutive presents (CPU timestamps after
    // vkQueuePresentKHR); with frame generation the two alternating intervals should match (even pacing).
    void printPresentStats() const {
        if (presentTimes_.size() < 3) return;
        std::vector<double> d;
        for (size_t i = 1; i < presentTimes_.size(); ++i)
            d.push_back(std::chrono::duration<double, std::milli>(presentTimes_[i] - presentTimes_[i - 1]).count());
        double sum = 0, alt[2] = {0, 0};
        for (size_t i = 0; i < d.size(); ++i) sum += d[i], alt[i & 1] += d[i];
        const double mean = sum / double(d.size());
        double var = 0;
        for (double x : d) var += (x - mean) * (x - mean);
        std::vector<double> sorted = d;
        std::sort(sorted.begin(), sorted.end());
        const double frames = double(std::max<uint64_t>(frameCounter_, 1));
        std::fprintf(stderr,
                     "ou: presents %zu in %.2f s = %.1f/s (real %llu, generated %llu); interval mean %.2f ms median %.2f sd %.2f "
                     "p99 %.2f; alternate intervals %.2f / %.2f ms; per real frame: fence wait %.2f ms, acquire %.2f ms, "
                     "vkQueuePresentKHR %.2f ms\n",
                     presentTimes_.size(), sum / 1000.0, 1000.0 * double(d.size()) / sum, (unsigned long long)frameCounter_,
                     (unsigned long long)presentedGenerated_, mean, sorted[sorted.size() / 2], std::sqrt(var / double(d.size())),
                     sorted[sorted.size() * 99 / 100], alt[0] / double((d.size() + 1) / 2), alt[1] / double(d.size() / 2),
                     fenceNs_ * 1e-6 / frames, acquireNs_ * 1e-6 / frames, presentNs_ * 1e-6 / frames);
        if (gpuFrames_) {
            const double n = double(gpuFrames_);
            std::fprintf(stderr, "ou: GPU ms per frame: uploads+BLAS %.2f, TLAS %.2f, shadow map %.2f, scene %.2f, post %.2f, FSR/end %.2f\n",
                         gpuMs_[0] / n, gpuMs_[1] / n, gpuMs_[2] / n, gpuMs_[3] / n, gpuMs_[4] / n, gpuMs_[5] / n);
        }
    }
    std::atomic<int64_t> fenceNs_{0}, acquireNs_{0}, presentNs_{0}; // OU_VK_PRESENT_STATS wait totals
    int64_t frameAcquireNs_ = 0; // this frame's acquire wait (present())
};

} // namespace

std::unique_ptr<Renderer> createVulkanRenderer() { return std::make_unique<VkRenderer>(); }

} // namespace ou::render
