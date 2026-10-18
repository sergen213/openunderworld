// Vulkan 1.3 backend: dynamic rendering, synchronization2, 2 frames in flight, a linear
// device-memory allocator (no VMA), staging uploads, SPIR-V embedded at build time (glslc).
// Entry points come straight from the linked loader (libvulkan); extension functions via
// vkGetInstanceProcAddr. Output is meant to match the GL backend pixel-for-pixel modulo rasterisation.
#include "render/RenderCommon.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

// Scene pipelines exist per scene::Blend (blend state is baked into Vulkan pipelines).
// Prepass: RT depth pre-pass (depth only; the RT Opaque pipeline then tests LEQUAL without writes).
enum class PipelineKind { Opaque, Alpha, Additive, Average, Shadow, Lines, Overlay, Post, Prepass };

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
    glm::mat4 viewProj, model, lightViewProj;
    int32_t lighting, shadows;
    float alphaRef;
    int32_t rt;     // scene.frag: bit 0 RT sun shadows, bit 1 reflective draw, bits 8-15 rays per pixel
    glm::vec4 eye;  // camera position (RT reflections)
};
static_assert(offsetof(ScenePush, eye) == 208, "std430 push block: vec4 at 16-byte offset");

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
};

struct GpuMesh {
    Buffer vertices, indices;
    std::vector<scene::SubMesh> submeshes;
    size_t vertexCount = 0;
    glm::mat4 model;
    glm::vec3 center{0.0f}; // world-space bounds centre (blended draw sorting)
    // Ray tracing (empty without RT support): one BLAS geometry per traced submesh (Opaque + Alpha
    // blends, as the shadow map). Skinned meshes are refitted, others rebuilt, on updateVertices.
    bool skinned = false, water = false;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    Buffer blasBuffer;
    std::vector<VkAccelerationStructureGeometryKHR> rtGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> rtRanges;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    Buffer text;
    Buffer lines; // debug lines, grown on demand in lineMemory_[frame]
    VkDeviceSize lineCapacity = 0;
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
        std::vector<Region> mips;
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
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
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
        vkDeviceWaitIdle(device_);
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
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            img.set = allocateSet(scenePool_, img.view, sceneSampler_);
            img.slot = uint32_t(textures_.size() + 1);
            if (rtStatus_.supported && img.slot >= kMaxRtTextures) {
                warn("beyond the " + std::to_string(kMaxRtTextures) + " ray-traced textures: rays see it white");
                img.slot = 0;
            }
            UploadBatch::ImageJob job{img.image, {}};
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
            g.water = m.origin.find("watersurface") != std::string::npos;
            const Bounds b = meshBounds(m);
            g.center = (b.lo + b.hi) * 0.5f;
            const VkDeviceSize vb = m.vertices.size() * sizeof(scene::Vertex), ib = m.indices.size() * sizeof(uint32_t);
            const VkBufferUsageFlags rtInput = rtStatus_.supported ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                                                   : 0;
            g.vertices = createBuffer(sceneMemory_, vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtInput,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            g.indices = createBuffer(sceneMemory_, ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtInput,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            batch.buffers.push_back({g.vertices.buffer, {m.vertices.data(), vb, 0, 0}});
            batch.buffers.push_back({g.indices.buffer, {m.indices.data(), ib, 0, 0}});
            g.vertexCount = m.vertices.size();
            meshSlot_[mi] = meshes_.size();
            meshes_.push_back(std::move(g));
        }
        flush(batch);
        bounds_ = sceneBounds(scene);
        return warnings; // acceleration structures: built by the first frame that asks for RT
    }

    void resize(Viewport) override { swapchainDirty_ = true; } // SDL pixel-size event → recreate next frame

    // Staged copy into the device-local vertex buffer; flush() orders it after in-flight reads and
    // waits. ponytail: one queue round-trip per call; switch to per-frame host-visible copies if
    // many skinned meshes per tick show up in profiles.
    bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) override {
        if (meshIndex >= meshSlot_.size() || meshSlot_[meshIndex] == SIZE_MAX) return false;
        GpuMesh& g = meshes_[meshSlot_[meshIndex]];
        if (vertices.size() != g.vertexCount) return false;
        UploadBatch batch;
        batch.buffers.push_back({g.vertices.buffer, {vertices.data(), vertices.size_bytes(), 0, 0}});
        // Ray tracing: the BLAS follows in the same submission (refit when skinned, else rebuilt); the
        // TLAS is updated once per frame in drawFrame.
        if (g.blas) flush(batch, [&](VkCommandBuffer cmd) { rebuildBlas(cmd, g); });
        else flush(batch);
        return true;
    }

    void drawFrame(const Camera& camera, Viewport vp, const FrameOptions& options) override {
        frameActive_ = false;
        if (vp.width <= 0 || vp.height <= 0) return; // minimised
        if (vp.width != requested_.width || vp.height != requested_.height) swapchainDirty_ = true;
        applySettings(options);
        if (rtPipelines_ && !tlas_ && scenePool_) buildRayTracing(); // first RT frame after upload()
        Frame& f = frames_[frameIndex_];
        VK_CHECK(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));
        for (int attempt = 0;; ++attempt) {
            if (swapchainDirty_) recreateSwapchain(vp);
            if (!swapchain_) return;
            VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex_);
            if (r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true; // image acquired; render it, recreate after
            else if (r == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0) { swapchainDirty_ = true; continue; }
            else if (r == VK_ERROR_OUT_OF_DATE_KHR) return;
            else check(r, "vkAcquireNextImageKHR");
            break;
        }
        VK_CHECK(vkResetFences(device_, 1, &f.fence)); // only after a successful acquire, else the next wait deadlocks
        VK_CHECK(vkResetCommandPool(device_, f.pool, 0));
        VkCommandBuffer cmd = f.cmd;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

        // GL clip z in [-w, w] → Vulkan [0, w]; same depth precision as the GL path.
        const glm::mat4 zRemap(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1);
        const float aspect = float(extent_.width) / float(extent_.height);
        // Ray tracing replaces the shadow map on opaque surfaces. Blended ones keep sampling the map (they
        // are drawn layer over layer without depth writes, so tracing them multiplies the cost: ballcourt
        // foliage ~10 ms of 15). Without RT support a requested RT mode falls back to the map entirely.
        const bool rtOn = rtPipelines_ && tlas_ && options.rt != RayTracing::Off;
        const bool shadowMap = options.shadows || options.rt != RayTracing::Off;
        rtStatus_.active = rtOn;
        ScenePush push{zRemap * camera.projection(aspect) * camera.view(), glm::mat4(1), glm::mat4(1),
                       options.lighting ? 1 : 0, shadowMap ? 1 : 0, 0.0f,
                       rtOn ? 1 | std::clamp(options.rtRays, 1, 64) << 8 : 0, glm::vec4(camera.position, 1.0f)};
        constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        drawOrder(meshes_, camera.position, order_);
        if (rtOn && tlasDirty_) recordTlasUpdate(cmd);
        if (shadowMap) {
            push.lightViewProj = zRemap * shadowViewProj(camera, aspect, bounds_);
            recordShadowPass(cmd, push.lightViewProj);
        }

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
                         kColorOut | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        if (msColor_.image)
            begin.push_back(imageBarrier(msColor_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kColorOut,
                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
        barriers(cmd, begin);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = msColor_.image ? msColor_.view : sceneColor_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = msColor_.image ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
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
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
        if (rtPipelines_) { // RT depth pre-pass over the opaque submeshes (they are first in order_)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = prepassPipeline_);
            for (const DrawRef& d : order_) {
                const scene::SubMesh& s = meshes_[d.mesh].submeshes[d.sub];
                if (s.blend != scene::Blend::Opaque) break;
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
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 1, 1, &shadowSet_, 0, nullptr);
        if (rtPipelines_ && tlas_) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 2, 1, &rtSet_, 0, nullptr);
        const int32_t rtFlags = push.rt;
        for (const DrawRef& d : order_) {
            const GpuMesh& m = meshes_[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            const VkPipeline p = scenePipelines_[size_t(s.blend)];
            if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            bindMesh(d.mesh);
            push.model = m.model;
            push.alphaRef = s.alphaRef / 255.0f;
            push.rt = (s.blend == scene::Blend::Opaque ? rtFlags : 0) | (rtOn && m.water && options.rt == RayTracing::Full ? 2 : 0);
            vkCmdPushConstants(cmd, sceneLayout_, kPushStages, 0, sizeof push, &push);
            vkCmdSetCullMode(cmd, options.cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
            const VkDescriptorSet set = textureSet(s.textureId);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
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

        // Post pass: resolved scene → swapchain (tonemap / FXAA), then the overlay on top.
        const VkImageMemoryBarrier2 toPost[] = {
            imageBarrier(sceneColor_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            // Chained to the acquire semaphore wait (same COLOR_ATTACHMENT_OUTPUT stage); old contents discarded.
            imageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorOut, 0,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        barriers(cmd, toPost);
        VkRenderingAttachmentInfo swap{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        swap.imageView = swapViews_[imageIndex_];
        swap.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swap.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the fullscreen triangle covers every pixel
        swap.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pColorAttachments = &swap;
        ri.pDepthAttachment = nullptr;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport upright{0, 0, float(extent_.width), float(extent_.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &upright);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        const int32_t post[2] = {options.fxaa ? 1 : 0, options.hdr ? 1 : 0};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline_);
        vkCmdPushConstants(cmd, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof post, post);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &postSet_, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        if (!options.overlay.empty() || !options.ui.empty()) {
            vkCmdSetViewport(cmd, 0, 1, &flipped);
            std::vector<TextVertex> v = buildOverlayVertices({int(extent_.width), int(extent_.height)}, options.overlay, options.ui);
            const uint32_t count = std::min<uint32_t>(uint32_t(v.size()), kMaxTextVertices);
            std::memcpy(f.text.mapped, v.data(), count * sizeof(TextVertex)); // host-coherent, per-frame buffer
            const float screen[2] = {float(extent_.width), float(extent_.height)};
            const VkDeviceSize zero = 0;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline_);
            vkCmdPushConstants(cmd, textLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen, screen);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textLayout_, 0, 1, &font_.set, 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.text.buffer, &zero);
            vkCmdDraw(cmd, count, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);

        const VkImageMemoryBarrier2 end = imageBarrier(
            target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0); // the present semaphore signal makes the writes visible to the presenter
        barriers(cmd, {&end, 1});
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, f.acquired, 0,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0};
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, presentReady_[imageIndex_], 0,
                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0};
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
        VK_CHECK(vkQueueSubmit2(queue_, 1, &si, f.fence));
        frameActive_ = true;
    }

    Image readBackbuffer(Viewport vp) override {
        if (!frameActive_) // nothing rendered (minimised / out of date): black frame of the requested size
            return {vp.width, vp.height, std::vector<uint8_t>(size_t(std::max(0, vp.width)) * std::max(0, vp.height) * 3)};
        const uint32_t w = extent_.width, h = extent_.height;
        LinearAllocator temp;
        temp.init(device_, gpu_, 0);
        Buffer buf = createBuffer(temp, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const VkImage target = swapImages_[imageIndex_];
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

    void present() override {
        if (!frameActive_) return;
        frameActive_ = false;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &presentReady_[imageIndex_];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &imageIndex_;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
        else check(r, "vkQueuePresentKHR");
        frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    }

    bool setVsync(bool enabled) override {
        if (!enabled && noVsyncMode() == VK_PRESENT_MODE_FIFO_KHR) return false;
        if (vsync_ != enabled) swapchainDirty_ = true;
        vsync_ = enabled;
        return true;
    }

    RtStatus rtStatus() const override { return rtStatus_; }

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
            vkDeviceWaitIdle(device_);
            savePipelineCache();
            destroySceneResources();
            destroySwapchain(swapchain_);
            swapchain_ = VK_NULL_HANDLE;
            for (auto& f : frames_) {
                vkDestroyCommandPool(device_, f.pool, nullptr);
                vkDestroyFence(device_, f.fence, nullptr);
                vkDestroySemaphore(device_, f.acquired, nullptr);
                vkDestroyBuffer(device_, f.text.buffer, nullptr);
                vkDestroyBuffer(device_, f.lines.buffer, nullptr);
                f = {};
            }
            destroyImage(white_);
            destroyImage(font_);
            destroyImage(shadow_);
            vkDestroyDescriptorPool(device_, staticPool_, nullptr);
            destroyScenePipelines();
            for (VkPipeline p : {textPipeline_, shadowPipeline_, postPipeline_}) vkDestroyPipeline(device_, p, nullptr);
            for (VkPipelineLayout l : {sceneLayout_, textLayout_, lineLayout_, postLayout_}) vkDestroyPipelineLayout(device_, l, nullptr);
            for (auto& m : lineMemory_) m.reset();
            vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(rtSetLayout_, VK_NULL_HANDLE), nullptr);
            for (VkSampler s : {sceneSampler_, fontSampler_, shadowSampler_, postSampler_}) vkDestroySampler(device_, s, nullptr);
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
    VkDescriptorSet textureSet(uint32_t id) const {
        auto it = textures_.find(id);
        return it != textures_.end() ? it->second.set : white_.set;
    }

    // MSAA sample count / HDR target format are baked into the scene pipelines and targets: rebuild
    // both when they change (device idle; a settings change is rare).
    void applySettings(const FrameOptions& o) {
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        for (int s = std::clamp(o.msaa, 1, 8); s > 1; s >>= 1)
            if (supportedSamples_ & VkSampleCountFlags(s)) { samples = VkSampleCountFlagBits(s); break; }
        const VkFormat format = o.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const bool rt = rtStatus_.supported && o.rt != RayTracing::Off;
        if (samples == samples_ && format == sceneFormat_ && rt == rtPipelines_ && scenePipelines_[0]) return;
        vkDeviceWaitIdle(device_);
        samples_ = samples;
        sceneFormat_ = format;
        rtPipelines_ = rt;
        destroyScenePipelines();
        createScenePipelines();
        swapchainDirty_ = true; // targets follow the new sample count / format
    }

    void destroyScenePipelines() {
        for (VkPipeline& p : scenePipelines_) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(prepassPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(linePipeline_, VK_NULL_HANDLE), nullptr);
    }

    // Sun shadow map: opaque and alpha-blended submeshes (the latter alpha-tested at 50 %), both
    // faces, slope-scaled bias baked into the pipeline. Upright viewport: uv = ndc * 0.5 + 0.5.
    void recordShadowPass(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
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
        ri.renderArea = {{0, 0}, {uint32_t(kShadowMapSize), uint32_t(kShadowMapSize)}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport viewport{0, 0, float(kShadowMapSize), float(kShadowMapSize), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        ScenePush push{lightViewProj, glm::mat4(1), glm::mat4(1), 0, 0, 0.0f, 0, glm::vec4(0.0f)};
        for (const auto& m : meshes_) {
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
        // Scene: set 0 = material texture, set 1 = sun shadow map (comparison sampler), set 2 (RT devices
        // only, used by scene_rt.frag) = TLAS, geometry records, bindless textures (partially bound).
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
        const VkDescriptorSetLayout sceneSets[3] = {setLayout_, setLayout_, rtSetLayout_};
        VkPushConstantRange scenePush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePush)};
        VkPushConstantRange textPush{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
        VkPushConstantRange postPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(int32_t)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, rtSetLayout_ ? 3u : 2u,
                                      sceneSets, 1, &scenePush};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &sceneLayout_));
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout_;
        pl.pPushConstantRanges = &textPush;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &textLayout_));
        pl.pPushConstantRanges = &postPush;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &postLayout_));
        VkPushConstantRange linePush{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo ll{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &linePush};
        VK_CHECK(vkCreatePipelineLayout(device_, &ll, nullptr, &lineLayout_));

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
        if (kind == PipelineKind::Alpha || kind == PipelineKind::Overlay)
            factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        else if (kind == PipelineKind::Additive) factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);
        else if (kind == PipelineKind::Average) factors(VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_COLOR);
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

    void createStaticResources() {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.anisotropyEnable = maxAniso_ > 1.0f;
        sci.maxAnisotropy = maxAniso_;
        sci.maxLod = VK_LOD_CLAMP_NONE;
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
        sci.compareEnable = VK_TRUE; // shadow map: hardware 2x2 PCF per tap (GL_COMPARE_REF_TO_TEXTURE, LEQUAL)
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &shadowSampler_));

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 4, 1, &ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &staticPool_));

        // Sun shadow map: always allocated so the scene pipelines' set 1 is valid; kept in
        // SHADER_READ_ONLY_OPTIMAL outside the shadow pass.
        shadow_ = createImage(staticMemory_, shadowFormat_, kShadowMapSize, kShadowMapSize, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        shadowSet_ = allocateSet(staticPool_, shadow_.view, shadowSampler_);
        oneShot([&](VkCommandBuffer cmd) {
            const auto b = imageBarrier(shadow_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            barriers(cmd, {&b, 1});
        });
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, staticPool_, 1, &setLayout_};
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &postSet_)); // written per swapchain (recreateSwapchain)

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
        flush(batch);
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
                      VkImageUsageFlags usage, VkImageAspectFlags aspect, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = levels;
        ci.arrayLayers = 1;
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
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, levels, 0, 1};
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
        VkResult r = vkQueueSubmit2(queue_, 1, &si, fence);
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
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
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
        if (limits.maxPerStageDescriptorSampledImages < kMaxRtTextures + 2 || limits.maxPerStageDescriptorSamplers < kMaxRtTextures + 2 ||
            limits.maxDescriptorSetSampledImages < kMaxRtTextures + 2 || limits.maxBoundDescriptorSets < 3)
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
            const VkDeviceSize scratch = alignUp(std::max(sz.buildScratchSize, sz.updateScratchSize));
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

    // Recorded after updateVertices' copy: refit (skinned) or rebuild the mesh's BLAS in place. Earlier
    // frames may still trace it, and the shared scratch may be in use by an earlier build.
    void rebuildBlas(VkCommandBuffer cmd, const GpuMesh& m) {
        asBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kAsBuild,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | kAsWrite, kAsBuild, VK_ACCESS_2_SHADER_READ_BIT | kAsRead | kAsWrite);
        auto bi = blasInfo(m, m.skinned ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
        bi.scratchData.deviceAddress = scratchBlas_;
        const VkAccelerationStructureBuildRangeInfoKHR* r = m.rtRanges.data();
        vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &bi, &r);
        asBarrier(cmd, kAsBuild, kAsWrite, kAsBuild | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
        ++(m.skinned ? rtStatus_.blasRefits : rtStatus_.blasRebuilds);
        tlasDirty_ = true;
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
        for (auto& [_, img] : textures_) destroyImage(img);
        textures_.clear();
        for (auto& m : meshes_) {
            vkDestroyBuffer(device_, m.vertices.buffer, nullptr);
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
        vkDeviceWaitIdle(device_); // ponytail: full stall on resize; fine for a viewer
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
        ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
        ci.imageFormat = surfaceFormat_.format;
        ci.imageColorSpace = surfaceFormat_.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
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
        extent_ = extent;

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
        depth_ = createImage(swapMemory_, depthFormat_, extent.width, extent.height, 1,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, samples_);
        sceneColor_ = createImage(swapMemory_, sceneFormat_, extent.width, extent.height, 1,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (samples_ != VK_SAMPLE_COUNT_1_BIT)
            msColor_ = createImage(swapMemory_, sceneFormat_, extent.width, extent.height, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, samples_);
        writeSet(postSet_, sceneColor_.view, postSampler_);
    }

    // Destroys the per-swapchain objects (views, semaphores, depth) and `chain` itself. Device must be idle.
    void destroySwapchain(VkSwapchainKHR chain) {
        for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
        for (VkSemaphore s : presentReady_) vkDestroySemaphore(device_, s, nullptr);
        swapViews_.clear();
        presentReady_.clear();
        swapImages_.clear();
        for (GpuImage* img : {&depth_, &sceneColor_, &msColor_})
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
    VkExtent2D extent_{};
    Viewport requested_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> presentReady_;
    GpuImage depth_, sceneColor_, msColor_; // msColor_ only with MSAA
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
    std::array<LinearAllocator, kFramesInFlight> lineMemory_;
    VkSampler sceneSampler_ = VK_NULL_HANDLE, fontSampler_ = VK_NULL_HANDLE, shadowSampler_ = VK_NULL_HANDLE,
              postSampler_ = VK_NULL_HANDLE;
    VkDescriptorPool staticPool_ = VK_NULL_HANDLE, scenePool_ = VK_NULL_HANDLE;
    GpuImage white_, font_, shadow_;
    VkDescriptorSet shadowSet_ = VK_NULL_HANDLE, postSet_ = VK_NULL_HANDLE;
    Bounds bounds_;
    std::vector<DrawRef> order_;

    LinearAllocator staticMemory_, sceneMemory_, swapMemory_;
    std::unordered_map<uint32_t, GpuImage> textures_;
    std::vector<GpuMesh> meshes_;
    std::vector<size_t> meshSlot_; // last upload()'s SceneData::meshes index -> meshes_ index (SIZE_MAX = skipped)

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
};

} // namespace

std::unique_ptr<Renderer> createVulkanRenderer() { return std::make_unique<VkRenderer>(); }

} // namespace ou::render
