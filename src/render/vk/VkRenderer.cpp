// Vulkan 1.3 backend: dynamic rendering, synchronization2, 2 frames in flight, a linear
// device-memory allocator (no VMA), staging uploads, SPIR-V embedded at build time (glslc).
// Entry points come straight from the linked loader (libvulkan); extension functions via
// vkGetInstanceProcAddr. Output is meant to match the GL backend pixel-for-pixel modulo rasterisation.
#include "render/Cortao.hpp"
#include "render/Fluid.hpp"
#include "render/Ocean.hpp"
#include "render/WaterField.hpp"
#include "render/OriginalShading.hpp"
#include "render/RenderCommon.hpp"
#include "render/Water.hpp"
#include "render/TimeOfDay.hpp"
#include "render/vk/AntiLag.hpp"
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
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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
const uint32_t kSceneRtVert[] = { // scene.vert -DOU_RT: invariant position (any depth pre-pass: RT or screen-space AO)
#include "scene_rt.vert.inc"
};
const uint32_t kSceneRtFrag[] = { // scene.frag -DOU_RT: ray-queried sun shadows + water reflections
#include "scene_rt.frag.inc"
};
const uint32_t kRtIndirect[] = { // RT AO / sun bounce: trace (rt_indirect.comp), from MSAA depth, denoise
#include "rt_indirect.comp.inc"
};
const uint32_t kRtIndirectMs[] = {
#include "rt_indirect_ms.comp.inc"
};
const uint32_t kRtDenoise[] = {
#include "rt_denoise.comp.inc"
};
const uint32_t kOceanComp[] = { // the FFT ocean (render/Ocean.hpp)
#include "ocean.comp.inc"
};
const uint32_t kFluidComp[] = { // the splash fluid (render/Fluid.hpp)
#include "fluid.comp.inc"
};
const uint32_t kWaterFieldComp[] = { // the interactive wave field (render/WaterField.hpp)
#include "water_field.comp.inc"
};
const uint32_t kPrepassVert[] = {
#include "prepass.vert.inc"
};
const uint32_t kPrepassFrag[] = {
#include "prepass.frag.inc"
};
// CORTAO ambient occlusion (third_party/cortao; recordCortao): prepass, main pass per CORTAO_SAMPLE_COUNT (8 / 12 / 16 /
// 32) and MSAA (sample 0 of the pre-pass targets), filter, temporal accumulation; the occluder lookup tables.
const uint32_t kCortaoPrepass[] = {
#include "cortao_prepass.comp.inc"
};
const uint32_t kCortaoPrepassMs[] = {
#include "cortao_prepass_ms.comp.inc"
};
const uint32_t kCortaoMain8[] = {
#include "cortao_main8.comp.inc"
};
const uint32_t kCortaoMain8Ms[] = {
#include "cortao_main8_ms.comp.inc"
};
const uint32_t kCortaoMain12[] = {
#include "cortao_main12.comp.inc"
};
const uint32_t kCortaoMain12Ms[] = {
#include "cortao_main12_ms.comp.inc"
};
const uint32_t kCortaoMain16[] = {
#include "cortao_main16.comp.inc"
};
const uint32_t kCortaoMain16Ms[] = {
#include "cortao_main16_ms.comp.inc"
};
const uint32_t kCortaoMain32[] = {
#include "cortao_main32.comp.inc"
};
const uint32_t kCortaoMain32Ms[] = {
#include "cortao_main32_ms.comp.inc"
};
const uint32_t kCortaoFilter[] = {
#include "cortao_filter.comp.inc"
};
const uint32_t kCortaoTemporal[] = {
#include "cortao_temporal.comp.inc"
};
const uint8_t kCortaoTable16[] = { // 24x24x16 R16_UINT, 16 rays
#include "cortao_24x24x16x16_R16_UINT.inc"
};
const uint8_t kCortaoTable32[] = { // 24x24x16 R32_UINT, 32 rays
#include "cortao_24x24x16x32_R32_UINT.inc"
};
static_assert(sizeof kCortaoTable16 == 24 * 24 * 16 * 2 && sizeof kCortaoTable32 == 24 * 24 * 16 * 4);
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
const uint32_t kShaftMarchFrag[] = { // Enhanced under-water sun shafts, passes 1-2 (render/Water.hpp, post.vert)
#include "uw_shafts.frag.inc"
};
const uint32_t kShaftBlurFrag[] = {
#include "uw_shafts_blur.frag.inc"
};
const uint32_t kAgWaterFrag[] = { // Enhanced water's animated water texture (abyssal-garden W_WATER, render/Water.hpp)
#include "uw_water.frag.inc"
};
const uint32_t kAgCausticVert[] = { // its floor caustics, added into g (shaders/uw_caustics.*, render/Water.hpp)
#include "uw_caustics.vert.inc"
};
const uint32_t kAgCausticFrag[] = {
#include "uw_caustics.frag.inc"
};
const uint32_t kSkyVert[] = { // time of day backdrop (shaders/sky.*, render/TimeOfDay.hpp)
#include "sky.vert.inc"
};
const uint32_t kSkyFrag[] = {
#include "sky.frag.inc"
};
const uint32_t kWaterVert[] = { // Enhanced water + splashes (shared GLSL, render/shaders)
#include "water.vert.inc"
};
const uint32_t kWaterTessVert[] = { // water.vert -DOU_TESS: the undisplaced surface for water.tesc / water.tese
#include "water_tess.vert.inc"
};
const uint32_t kWaterTesc[] = {
#include "water.tesc.inc"
};
const uint32_t kWaterTese[] = {
#include "water.tese.inc"
};
const uint32_t kWaterFrag[] = {
#include "water.frag.inc"
};
const uint32_t kWaterRtFrag[] = { // water.frag -DOU_RT: ray-traced reflections instead of SSR (FrameOptions::rtReflections)
#include "water_rt.frag.inc"
};
const uint32_t kSplashVert[] = {
#include "splash.vert.inc"
};
const uint32_t kSplashSplat[] = { // the splash fluid as liquid: splat, blur, composite (render/Water.hpp splash passes)
#include "splash_splat.frag.inc"
};
const uint32_t kSplashScreen[] = {
#include "splash_screen.vert.inc"
};
const uint32_t kSplashBlur[] = {
#include "splash_blur.frag.inc"
};
const uint32_t kSplashComposite[] = {
#include "splash_composite.frag.inc"
};
const uint32_t kUwSnowVert[] = { // Enhanced under-water particles (render/Water.hpp UwParticlePush)
#include "uw_snow.vert.inc"
};
const uint32_t kUwSnowFrag[] = {
#include "uw_snow.frag.inc"
};
const uint32_t kUwBubbleVert[] = {
#include "uw_bubble.vert.inc"
};
const uint32_t kUwBubbleFrag[] = {
#include "uw_bubble.frag.inc"
};
const uint32_t kUwBloomFrag[] = { // Enhanced under-water composite (render/Water.hpp UwGrade, post.vert)
#include "uw_bloom.frag.inc"
};
const uint32_t kUwGradeFrag[] = {
#include "uw_grade.frag.inc"
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
// Pssm: tru.exe's shadow atlas (original::Pssm): as Shadow with the slope bias as dynamic state (the depth bias is in
// the split's matrix).
// SkyMultiply / SkyAdd: the time of day backdrop (render/TimeOfDay.hpp): dst x src, src + dst; rgb only, no depth test.
enum class PipelineKind { Opaque, Alpha, Additive, Average, Shadow, Pssm, Lines, Overlay, Post, Prepass, Water, SplashSplat,
                          SplashBlur, SplashComposite, FxAlpha, FxAdditive, FxSubtract, FxMultiply, // Fx*: FxBatch::blend order
                          CausticSplat, // ONE + ONE into G only (uw_caustics.*)
                          SkyMultiply, SkyAdd };

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
    VkDeviceAddress transform; // merged static BLAS: the mesh model (VkTransformMatrixKHR), else 0 (sun.glsl OuGeom)
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
        const bool map = want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
            // Mapped and unmapped blocks chain separately: a device-local request can pick a host-visible type
            // (lavapipe, UMA, ReBAR), and a later mapped request of that type must not land in its unmapped block.
            if (it->type != type || map != (it->mapped != nullptr)) continue;
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
        if (map)
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

// One BLAS (and TLAS instance) per ray mask of a mesh's traced submeshes (sun.glsl kOuMask*), kept with the mesh.
struct RtBlas {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    Buffer buffer;
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<RtGeometry> records; // per geometry, texture = 0 (each upload resolves the slot from `textures`)
    std::vector<uint32_t> textures;  // per geometry: SubMesh::textureId
    VkDeviceSize scratch = 0; // aligned max(build, update) scratch size
    VkAccelerationStructureInstanceKHR instance{}; // its TLAS instance (custom index set per upload)
    uint32_t mask = 0;
    bool skinned = false; // built for refits (ALLOW_UPDATE)
    bool built = false;   // holds its geometries' current vertices
};

// Device memory + descriptor pool of the scene resources created together (one upload, or one renderer part), freed
// with the last of them (VkRenderer::bury: after the frames using them retired).
struct Generation {
    VkDevice device = VK_NULL_HANDLE;
    LinearAllocator memory;
    VkDescriptorPool pool = VK_NULL_HANDLE; // texture sets
    Generation() = default;
    Generation(const Generation&) = delete;
    Generation& operator=(const Generation&) = delete;
    ~Generation() {
        memory.reset();
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
    }
};

struct GpuMesh {
    std::shared_ptr<Generation> gen; // its buffers' memory (prevVertices too)
    uint64_t key = 0, part = 0;      // MeshData::key (0: sent each upload), owning renderer part (0: none)
    bool dirty = false;              // updateVertices since it was sent: sent again by the next upload
    Bounds bounds;                   // world bounds of its vertices under `model`
    Buffer vertices, indices;
    std::vector<scene::SubMesh> submeshes;
    size_t vertexCount = 0, indexCount = 0;
    glm::mat4 model;
    glm::vec3 exteriorShift{0.0f}; // exterior terrain: translation added to `model` (followExterior)
    glm::vec3 center{0.0f}; // world-space bounds centre (blended draw sorting; light sets with `radius`)
    float radius = 0.0f;    // world-space bounding-sphere radius (original::lightSet)
    float lodNear = 0, lodFar = std::numeric_limits<float>::max(); // batch LOD (render::setLod)
    glm::vec3 lodOffset{0.0f};
    uint32_t lightMask = ~0u; // scene::MeshData::lightMask
    std::vector<std::vector<std::array<float, 6>>> lightBoxes; // terrain light groups (scene::MeshData::lightBoxes)
    float reflectZ = std::numeric_limits<float>::quiet_NaN(); // original::reflectionPlaneZ (NaN: nothing reflects)
    std::vector<uint8_t> reflective; // per submesh: a planar-reflection material (render::reflectsWater candidates)
    // Ray tracing (empty without RT support): one BLAS geometry per traced submesh (Opaque + Alpha
    // blends, as the shadow map). Skinned meshes are refitted, others rebuilt, on updateVertices.
    bool skinned = false, water = false;
    bool irradiance = false; // a submesh's program reads IrradianceState (original::IrradianceSlots)
    std::vector<RtBlas> blases;
    bool rtStatic = false; // traced through a merged static BLAS (RtGroup; never moved by updateVertices, no batch LOD)
    bool rtLodHidden = false; // its TLAS instances have mask 0: batch LOD out of range (updateRtLods)
    std::vector<std::pair<uint32_t, uint32_t>> rtMerged; // its geometries there: (rtGroups_ index, geometry index)
    uint64_t rtSerial = 0;        // identity of its BLAS geometries for RtGroup::members (new mesh / new masks)
    std::vector<uint8_t> rtMasks; // per submesh: the ray mask its BLASes were made with (0: not traced)
    // FSR motion vectors of meshes moved by updateVertices: the previous drawn frame's vertices (GPU copy at the start
    // of the frame) and the frame counter of the last update; the staging slot of a pending updateVertices.
    Buffer prevVertices;
    uint64_t movedFrame = 0, stagingFrame = 0;
    VkDeviceSize stagingOffset = 0;
};

// Merged BLAS over the static meshes of one renderer part (or the keyed meshes outside parts, or the unkeyed ones) for
// one ray mask, kept while its members (GpuMesh::rtSerial, sorted) and their models stay the same.
struct RtGroup {
    uint64_t id = 0;
    std::vector<uint64_t> members;
    std::vector<VkTransformMatrixKHR> transforms; // per member: geometry transform (its model, VkTransformMatrixKHR rows)
    std::shared_ptr<Generation> gen;              // the BLAS's and transform buffer's memory
    Buffer transformBuffer;
    RtBlas blas;
    bool stale = false; // a member left it (evictStatic): rebuilt by the next upload
    // Batch LOD shared by every member (render::lodShown fields; center = the members' LOD point, offset 0): the group's
    // instance is masked out while out of range (updateRtLods).
    float lodNear = 0, lodFar = std::numeric_limits<float>::max();
    glm::vec3 center{0.0f}, lodOffset{0.0f};
    bool lodHidden = false;
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
    bool fluidTimed = false;               // ... including the splash fluid's (VkRenderer::kFluidMarks)
    bool aoTimed = false;                  // ... and the screen-space AO passes' (VkRenderer::kAoMarks)
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

    // Keyed content stays resident (Renderer::upload): textures by TextureData::key, meshes by MeshData::key, each in
    // the Generation it was created in; the rest, the RT structures and the material sets are per upload. Nothing waits
    // for the device: dropped objects go to the graveyard (bury) and new content is staged, its copies recorded at the
    // start of the next frame (recordPending).
    std::vector<std::string> upload(const scene::SceneData& scene) override {
        std::vector<std::string> warnings;
        pumpParts(SIZE_MAX, warnings); // parts still in flight: the scene may list them
        ++uploads_;
        std::unordered_map<uint64_t, GpuMesh> pool = std::move(parked_);
        parked_.clear();
        for (auto& g : meshes_) {
            g.rtStatic = false, g.rtMerged.clear(); // until buildRayTracing; its BLASes stay with it
            if (!g.key || g.dirty || !pool.try_emplace(g.key, std::move(g)).second) buryMesh(g);
        }
        meshes_.clear();
        meshSlot_.clear();
        for (auto& st : staging_) st.meshes.clear(), st.used = 0; // pending updateVertices copies of the old slots
        retireScene();
        // Merged BLASes of live parts wait for buildRayTracing to reuse them; released parts' and unkeyed meshes' go.
        std::erase_if(rtGroups_, [&](RtGroup& g) {
            if (!g.stale && (g.id == 0 || parts_.contains(g.id))) return false;
            buryGroup(g);
            return true;
        });
        gen_ = newGeneration(kBlockSize, scene.textures.size());

        UploadBatch batch;
        std::vector<SceneImage> transient;
        for (const auto& t : scene.textures) {
            // First id wins (as formats::loadTextures).
            if (textures_.contains(t.id)) {
                warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): duplicate texture id, first one wins");
                continue;
            }
            GpuImage img;
            if (t.key) {
                auto it = resident_.find(t.key);
                if (it == resident_.end()) {
                    auto created = createTexture(t, gen_, batch, warnings);
                    if (!created) continue;
                    it = resident_.emplace(t.key, std::move(*created)).first;
                }
                it->second.upload = uploads_;
                img = it->second.img;
            } else {
                auto created = createTexture(t, gen_, batch, warnings);
                if (!created) continue;
                img = created->img;
                transient.push_back(std::move(*created));
            }
            img.slot = t.cube ? 0 : uint32_t(textures_.size() + 1);
            if (rtStatus_.supported && img.slot >= kMaxRtTextures) {
                warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): beyond the " +
                                   std::to_string(kMaxRtTextures) + " ray-traced textures: rays see it white");
                img.slot = 0;
            }
            textures_.emplace(t.id, img);
        }
        std::erase_if(resident_, [&](auto& kv) {
            if (kv.second.upload == uploads_ || parts_.contains(kv.second.part)) return false;
            buryTexture(kv.second);
            return true;
        });
        transient_ = std::move(transient);

        meshSlot_.assign(scene.meshes.size(), SIZE_MAX);
        bounds_ = {};
        std::unordered_set<uint64_t> listed; // a key twice: the copies are sent each upload
        for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
            const scene::MeshData& m = scene.meshes[mi];
            const bool keyed = m.key && listed.insert(m.key).second;
            const auto it = keyed ? pool.find(m.key) : pool.end();
            GpuMesh g;
            if (it != pool.end() && it->second.vertexCount == m.vertices.size() && it->second.indexCount == m.indices.size()) {
                g = std::move(it->second);
                pool.erase(it);
                place(g, m);
            } else if (!createMesh(m, gen_, g, batch, warnings)) {
                continue;
            }
            g.key = keyed ? m.key : 0;
            g.lightMask = m.lightMask;
            bounds_.lo = glm::min(bounds_.lo, g.bounds.lo), bounds_.hi = glm::max(bounds_.hi, g.bounds.hi);
            meshSlot_[mi] = meshes_.size();
            meshes_.push_back(std::move(g));
        }
        for (auto& [key, g] : pool) {
            if (parts_.contains(g.part)) parked_.emplace(key, std::move(g));
            else buryMesh(g);
        }
        // AttenuationSampler light rows follow the scene's light falloff curves (SHADERS.md "Engine inputs").
        const auto lut = original::attenuationLut(scene.environment);
        batch.images.push_back({attenuation_.image, {{lut.rgba.data(), lut.rgba.size(), uint32_t(lut.width), uint32_t(lut.height)}}});
        fogRow_ = original::fogLutRow(nullptr);
        cookieIds_ = original::cookieIds(scene.environment);
        const auto cookies = original::cookieArray(cookieIds_, scene.textures);
        batch.images.push_back({cookies_.image, {{cookies.rgba.data(), cookies.rgba.size(), uint32_t(cookies.width), uint32_t(cookies.height)}},
                                uint32_t(cookies.layers)});
        stage(batch);
        uploadOriginals(scene, warnings);
        return warnings; // acceleration structures: built by the first frame that asks for RT
    }

    // Created now (memory, images, buffers, staged copies) in budget-sized steps per frame (pumpParts), drawn once an
    // upload lists their keys.
    void uploadPart(uint64_t id, std::shared_ptr<const scene::SceneData> part) override { parts_[id] = {std::move(part)}; }

    void releasePart(uint64_t id) override {
        if (!parts_.erase(id)) return;
        std::erase_if(resident_, [&](auto& kv) {
            if (kv.second.part != id) return false;
            kv.second.part = 0;
            if (kv.second.upload == uploads_) return false; // listed by the current upload: until the next one
            buryTexture(kv.second);
            return true;
        });
        std::erase_if(parked_, [&](auto& kv) {
            if (kv.second.part != id) return false;
            buryMesh(kv.second);
            return true;
        });
        for (auto& g : meshes_)
            if (g.part == id) g.part = 0;
    }

    size_t residentCount() const override {
        size_t n = resident_.size() + parked_.size();
        for (const auto& g : meshes_) n += g.key != 0;
        return n;
    }

    void resize(Viewport) override { swapchainDirty_ = true; } // SDL pixel-size event → recreate next frame

    // Copied into this frame-in-flight's host-visible staging buffer; drawFrame records the copies (and BLAS refits)
    // at the start of its command buffer (recordVertexUploads). Repeated updates before a draw overwrite their slot.
    bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) override {
        if (meshIndex >= meshSlot_.size() || meshSlot_[meshIndex] == SIZE_MAX) return false;
        const size_t slot = meshSlot_[meshIndex];
        GpuMesh& g = meshes_[slot];
        if (vertices.size() != g.vertexCount) return false;
        g.dirty = true;
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
            g.prevVertices = createBuffer(g.gen->memory, vertices.size_bytes(),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        g.movedFrame = frameCounter_ + 1;
        if (g.rtStatic) { // evictStatic, after this frame's BLAS rebuilds; later uploads keep it out of the merged BLASes
            g.rtStatic = false, rtEvicted_.push_back(slot);
            if (g.key) movers_.insert(g.key);
        }
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
        evictStatic(cmd);
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
        if (options.unitLook) original::applyUnitLook(environment_, *options.unitLook);
        original::applySky(environment_, uploadSun_, options.sky); // time of day: its sun, else the scene's
        if (vp.width != requested_.width || vp.height != requested_.height) swapchainDirty_ = true;
        if (presentOutOfDate_.exchange(false)) swapchainDirty_ = true; // reported by the present thread
        applySettings(options);
        antiLagWanted_ = options.antiLag;
        Frame& f = frames_[frameIndex_];
        {
            const auto start = std::chrono::steady_clock::now();
            VK_CHECK(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));
            fenceNs_ += (std::chrono::steady_clock::now() - start).count();
        }
        retire(false);
        {
            std::vector<std::string> ignored; // the upload listing them reports them
            pumpParts(kPartBytesPerFrame, ignored);
        }
        followExterior(meshes_, eye.position, options.exteriorOrigin);
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
        recordPending(cmd);
        if (rtPipelines_ && !tlas_ && gen_) buildRayTracing(cmd); // first RT frame after upload()
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
        rtStatus_.waterSurfaces = 0; // the Enhanced water pass counts this frame's
        rtStatus_.reflections = rtOn && options.rtReflections;
        ScenePush push{zRemap * camera.projection(aspect) * camera.view(), glm::mat4(1), options.lighting ? 1 : 0, 0.0f, 0, 1.0f};
        const glm::mat4 currentVP = zRemap * eye.projection(aspect) * eye.view(); // unjittered (FSR motion)
        const bool enhancedWater = options.water == WaterQuality::Enhanced;
        sea_.assign(enhancedWater ? meshes_.size() : 0, 0);
        for (size_t i = 0; i < sea_.size(); ++i)
            sea_[i] = reflectsWater(meshes_[i].center, meshes_[i].radius, meshes_[i].reflectZ, options.waterVolumes);
        bool splashDrawn = false; // the splash fluid's composite ran (FSR reactivity)
        WaterBlock wb = waterBlock(camera, {int(extent_.width), int(extent_.height)}, push.viewProj, environment_, options);
        if (!enhancedWater) wb.fogColor.w = 0.0f; // Original: no underwater post either
        // water_rt.frag: gain of off-screen reflection hits (generic texture x vertex colour x lambert) toward the translated
        // shaders' look. [O] l2_cliffclimb deck: raster / RT luminance of on-screen hits, median 2.2 (226k pixels).
        // ponytail: one global gain; per-material shading at the hit would need the translated programs in the ray path.
        wb.waterPass[2].w = options.originalShaders && !origPrograms_.empty() ? 2.0f : 1.0f;
        // The splash fluid's frame (render/Fluid.hpp): its bursts go to this frame's host-visible buffer now; it simulates
        // and draws after the water surfaces (recordFluid / recordSplashFluid).
        fluid::Frame fluidFrame;
        if (enhancedWater && fluidPipeline_ && splashSplatPipeline_) {
            fluidFrame = fluidSim_.frame(options, camera.position);
            if (!fluidFrame.bursts.empty())
                std::memcpy(fluidBursts_[frameIndex_].mapped, fluidFrame.bursts.data(), fluidFrame.bursts.size() * sizeof(fluid::GpuBurst));
        }
        if (wb.ocean[0].x > 0.5f) { // FFT ocean this frame (render/Ocean.hpp)
            if (oceanPipeline_) recordOcean(cmd, options);
            else wb.ocean[0].x = 0.0f; // afl's waves
        }
        gpuMark(cmd, 13);
        if (!recordWaterField(cmd, options, wb)) wb.rippleInfo.y = 0.0f; // the interactive waves (render/WaterField.hpp)
        gpuMark(cmd, 14);
        if (enhancedWater) recordAgWater(cmd, wb.eye.w); // uAgWater: this frame's caustics / slopes (water + post read it)
        // The post pass samples opaqueDepth_ for the atmosphere / weather too, not only for Enhanced water; translated
        // materials reading the back/depth buffer (the sea, OrigDraw::screen) draw in the second pass from the copies.
        const bool screenReaders = options.originalShaders && !origPrograms_.empty() && !screenSets_.empty();
        // Material particles that refract / fade softly sample the same copies (FxBatch::screen).
        const bool fxScreen = std::any_of(options.fxBatches.begin(), options.fxBatches.end(), [](const FxBatch& b) { return b.screen(); });
        const bool filter = options.screenFilter && options.screenFilter->active(); // original screen filters (post)
        // The half-size scene + mips (recordFilterBlur): the screen filters' blur levels.
        const bool blurChain = filter;
        // PCFeedbackEffect: copies the post output at the end of the frame; its draw needs last frame's copy (+0x3D).
        const bool feedback = filter && options.screenFilter->feedback() && feedbackCopyable_;
        wb.filter[7].w = feedback && hasFeedback_ ? 1.0f : 0.0f;
        std::memcpy(waterUbo_[frameIndex_].mapped, &wb, sizeof wb);
        const bool needDepth = enhancedWater || wb.haze.w > 0.5f || wb.weather.x > 0.5f || wb.waterPass[2].x > 0 || screenReaders || fxScreen ||
                               (filter && options.screenFilter->dof);
        for (size_t i = 0; i < uiImages_.size(); ++i) updateUiImage(uiImages_[i], options.uiImages[i]);
        constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        meshOpacities(options.meshOpacity, meshes_.size(), opacity_);
        drawOrder(meshes_, camera.position, order_, opacity_);
        lodEye_ = camera.position;
        unitClips_ = options.unitClips;
        meshClip_.assign(meshes_.size(), -1);
        for (size_t i = 0; i < meshSlot_.size(); ++i)
            if (meshSlot_[i] < meshes_.size()) meshClip_[meshSlot_[i]] = clipIndex(unitClips_, i);
        std::erase_if(order_, [&](const DrawRef& d) { return clippedAway(d.mesh); });
        if (rtOn) updateRtLods(cmd);
        if (rtOn && tlasDirty_) recordTlasUpdate(cmd);
        gpuMark(cmd, 2);
        // RT AO / sun bounce (recordIndirect): traced from the opaque pre-pass depth, between the pre-pass and the draws.
        const bool indirectReady = rtOn && indTrace_ && ind_.final.image;
        const bool rtAo = indirectReady && options.ao == AmbientOcclusion::Rt;
        const bool indirect = indirectReady && (rtAo || options.rtGi);
        rtStatus_.gi = indirect && options.rtGi;
        // Screen-space AO (recordCortao, RENDERING.md "Ambient occlusion"): requested, or RT AO requested without RT.
        const bool cortao = cortaoReady() && (options.ao == AmbientOcclusion::Cortao || (options.ao == AmbientOcclusion::Rt && !rtAo));
        rtStatus_.ao = rtAo ? AmbientOcclusion::Rt : cortao ? AmbientOcclusion::Cortao : AmbientOcclusion::Off;
        // Sun shadow block (render/sun.glsl) for both the generic and the translated shaders, per frame in flight.
        SunShadow sun = sunShadow(camera, {int(extent_.width), int(extent_.height)}, bounds_, sunToward(environment_), true,
                                  shadowMap, rtOn ? (options.rtReflections ? 2 : 1) : 0, std::clamp(options.rtRays, 1, 64));
        sun.block.screen.w = float((rtAo ? 1 : 0) | (rtStatus_.gi ? 2 : 0) | (cortao ? 4 : 0)); // sun.glsl ouAo / ouIndirect
        std::memcpy(shadowUbo_[frameIndex_].mapped, &sun.block, sizeof sun.block); // host-coherent
        // Local-light shadows (sun.glsl ouLightsBegin): the shadow lights of the table (scene::Light::shadow, tru.exe
        // DTP +0x144C: the flashlight's spot, fixtures), half the sun's rays per light (each pixel may trace 4 lights).
        glm::uvec4 rtLights(0);
        rtStatus_.lights = rtOn && options.rtLights;
        if (rtStatus_.lights) {
            for (size_t i = 0; i < std::min(environment_.lights.size(), size_t(sm3::kMaxLights)); ++i) {
                const scene::Light& l = environment_.lights[i]; // colour 0: a switched-off dynamic light (flashlight)
                if (l.shadow && l.color[0] + l.color[1] + l.color[2] > 0) rtLights[i >> 5] |= 1u << (i & 31);
            }
            rtLights.z = uint32_t(std::clamp(options.rtRays / 2, 1, 32));
        }
        std::memcpy(shadowUbo_[frameIndex_].mapped + offsetof(SunShadowBlock, lights), &rtLights, sizeof rtLights);
        if (shadowMap) recordShadowPass(cmd, sun, zRemap);
        // Original preset: tru.exe's own sun shadows for the translated programs (they read lit otherwise).
        original::Pssm pssm;
        if (options.pssm && options.originalShaders && !origPrograms_.empty()) {
            pssm = original::pssm(camera, aspect, environment_);
            if (pssm.active) recordPssmPass(cmd, pssm);
        }
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
        // Negative height flips Y so GL-convention clip space and CCW front faces carry over unchanged.
        const VkViewport flipped{0, float(extent_.height), float(extent_.width), -float(extent_.height), 0, 1};
        const VkRect2D scissor{{0, 0}, extent_};

        VkPipeline bound = VK_NULL_HANDLE;
        uint32_t boundMesh = UINT32_MAX;
        auto bindMesh = [&](uint32_t mesh) {
            if (mesh == boundMesh) return;
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &meshes_[mesh].vertices.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, meshes_[mesh].indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            boundMesh = mesh;
        };
        int scissorClip = -1; // the portal clip set as the scissor (-1: the whole target)
        bool portalClips = true; // off in the reflection pass (the clips are the camera's)
        auto clipTo = [&](uint32_t mesh) {
            const int c = mesh < meshClip_.size() ? meshClip_[mesh] : -1;
            if (!portalClips || c == scissorClip) return;
            scissorClip = c;
            const VkRect2D r = clipScissor(mesh);
            vkCmdSetScissor(cmd, 0, 1, &r);
        };
        // Original Fidelity: translated TRU shaders where a submesh's material has them, with the sun shadow / RT hooks.
        const bool originals = options.originalShaders && !origPrograms_.empty();
        if (originals) warmOriginalPipelines(rtOn, shadowMap, options.rtReflections);
        glm::mat4 viewProjD3D = original::viewProjD3D(camera, aspect);
        VkDescriptorSet frameSet = origFrameSet_[frameIndex_];
        std::optional<original::Reflection> reflection; // the planar reflection the reflecting draws sample this frame
        if (originals) {
            // The fog curve (Fog) changed: the LUT again, staged after upload()'s (recorded with the next frame).
            if (const auto row = original::fogLutRow(options.fog); row != fogRow_) {
                const auto lut = original::attenuationLut(environment_, options.fog);
                UploadBatch b;
                b.images.push_back({attenuation_.image, {{lut.rgba.data(), lut.rgba.size(), uint32_t(lut.width), uint32_t(lut.height)}}});
                stage(b);
                fogRow_ = row;
            }
            irradiance_.assign(environment_, meshes_);
            sm3::FrameBlock fb;
            original::fillFrame(fb, camera, {int(extent_.width), int(extent_.height)}, options.time, environment_, false,
                                options.underwater, cookieIds_, options.fog, options.sky);
            original::fillPssm(fb, pssm, camera);
            irradiance_.fill(fb, options.sky ? options.sky->probes : nullptr);
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
        // With a depth pre-pass the opaque pipelines set their depth writes per draw (createPipeline): off for the pre-passed
        // draws of the main pass, on where nothing pre-passed them (planar reflection, screen readers).
        bool opaqueWrites = true;
        auto depthWrites = [&](const scene::SubMesh& s, float op, bool prepassDraw) {
            const scene::Blend b = drawBlend(s, op);
            if (b == scene::Blend::Alpha) vkCmdSetDepthWriteEnable(cmd, drawDepthWrite(s, op));
            else if (b == scene::Blend::Opaque && prepassPipelines_ && !prepassDraw) vkCmdSetDepthWriteEnable(cmd, opaqueWrites);
        };
        // A submesh's draw with its cull mode; two-sided blended ones as a back-face then a front-face draw (drawSidesApart).
        auto drawSides = [&](const scene::SubMesh& s, float op) {
            if (drawSidesApart(s, op, options.cullBackFaces)) {
                vkCmdSetCullMode(cmd, VK_CULL_MODE_FRONT_BIT);
                vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
                vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
            } else {
                vkCmdSetCullMode(cmd, options.cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
            }
            vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
        };
        auto drawOriginal = [&](const DrawRef& d, const OrigDraw& od, VkPipeline p, bool prepassDraw = false) {
            if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            if (layout != Bound::Original) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 0, 1, &frameSet, 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 2, 1, &shadowSets_[frameIndex_], 0, nullptr);
                if (rtOn) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 3, 1, &rtSet_, 0, nullptr);
                layout = Bound::Original;
            }
            const GpuMesh& m = meshes_[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, origLayout_, 1, 1, &od.set, 0, nullptr);
            sm3::ObjectBlock ob;
            // Once per mesh (or terrain light group) and frame (the lights move: dynamicLights), not per submesh and pass.
            if (meshLights_.size() < meshes_.size()) meshLights_.resize(meshes_.size());
            auto& cache = meshLights_[d.mesh];
            const size_t group = s.lightGroup < m.lightBoxes.size() ? s.lightGroup + 1u : 0u; // 0: the mesh's sphere
            if (cache.size() <= group) cache.resize(group + 1);
            auto& [stamp, lights] = cache[group];
            if (stamp != frameCounter_ + 1)
                stamp = frameCounter_ + 1,
                lights = group ? original::lightSet(environment_, m.lightBoxes[group - 1], m.model, m.lightMask, &camera.position)
                               : original::lightSet(environment_, m.center, m.radius, m.lightMask, &camera.position);
            original::fillObject(ob, viewProjD3D, m.model, lights, opacityOf(d.mesh));
            original::fillWaves(ob, options.waterVolumes, m.center, m.radius);
            ob.irradiance = irradiance_.slot(d.mesh);
            if (reflection && od.reflection) std::memcpy(ob.inst, reflection->rows, sizeof ob.inst);
            vkCmdPushConstants(cmd, origLayout_, kPushStages, 0, sizeof ob, &ob);
            bindMesh(d.mesh);
            clipTo(d.mesh);
            depthWrites(s, opacityOf(d.mesh), prepassDraw);
            drawSides(s, opacityOf(d.mesh));
        };
        auto origDraw = [&](const DrawRef& d) -> const OrigDraw* {
            const OrigDraw* od = originals ? &origDraws_[d.mesh][d.sub] : nullptr;
            return od && od->program >= 0 ? od : nullptr;
        };
        // Time of day backdrop (Sky::backdropPass, shaders/sky.*): `modes` bit 0 = the tint (the frame so far x the tint)
        // after the opaque exterior (or first without one), bit 1 = the stars and the moon, added under the star coverage
        // in alpha (`exterior`: kept from the opaque exterior's painted scenery and the blended exterior drawn between the
        // two passes; else cleared by the tint).
        auto drawSky = [&](const Camera& c, VkExtent2D ext, int modes, bool exterior) {
            if (!options.sky || !options.sky->backdropPass()) return;
            clipTo(UINT32_MAX);
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            for (int mode = 0; mode < 2; ++mode) {
                if (!(modes & (1 << mode)) || (mode == 1 && options.sky->stars <= 0 && options.sky->moon <= 0)) continue;
                SkyPush sp = skyPush(c, float(ext.width) / float(std::max(ext.height, 1u)), *options.sky, mode, options.time);
                sp.stars.w = exterior ? 1.0f : 0.0f; // the tint keeps the exterior's star coverage, else clears it (sky.frag)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = skyPipelines_[size_t(mode)]);
                vkCmdPushConstants(cmd, skyLayout_, kPushStages, 0, sizeof sp, &sp);
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        };
        // Planar reflection (original::reflection): the plane of the nearest drawn reflecting submesh, with the camera above
        // it; the scene except the water surfaces and the screen / reflection readers, seen from the mirrored camera, into
        // reflColor_ (half size). Enhanced water draws its reflecting submeshes itself (waterDraw): no reader, no pass.
        float planeZ = 0, nearest = std::numeric_limits<float>::max();
        for (const DrawRef& d : order_)
            if (const OrigDraw* od = origDraw(d); od && od->reflection && !(enhancedWater && waterDraw(d.mesh, d.sub))) {
                const GpuMesh& m = meshes_[d.mesh];
                if (const float dist = glm::distance(m.center, camera.position) - m.radius; dist < nearest) nearest = dist, planeZ = m.reflectZ;
            }
        if (nearest < std::numeric_limits<float>::max() && camera.position.z > planeZ) {
            reflection = original::reflection(camera, aspect, planeZ, false);
            const VkExtent2D half{std::max(1u, extent_.width / 2), std::max(1u, extent_.height / 2)};
            sm3::FrameBlock fb;
            original::fillFrame(fb, reflection->camera, {int(half.width), int(half.height)}, options.time, environment_, false,
                                options.underwater, cookieIds_, options.fog, options.sky);
            original::fillPssm(fb, pssm, camera);
            irradiance_.fill(fb, options.sky ? options.sky->probes : nullptr);
            std::memcpy(reflFrameUbo_[frameIndex_].mapped, &fb, sizeof fb);
            std::vector<VkImageMemoryBarrier2> rb = {
                imageBarrier(reflColor_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kColorOut,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                imageBarrier(reflDepth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kDepthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT),
            };
            if (reflMsColor_.image)
                rb.push_back(imageBarrier(reflMsColor_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kColorOut,
                                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
            barriers(cmd, rb);
            VkRenderingAttachmentInfo rc = color;
            rc.imageView = reflMsColor_.image ? reflMsColor_.view : reflColor_.view;
            rc.storeOp = reflMsColor_.image ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
            rc.resolveImageView = reflMsColor_.image ? reflColor_.view : VK_NULL_HANDLE; // color resolves only with MSAA
            VkRenderingAttachmentInfo rd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            rd.imageView = reflDepth_.view;
            rd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            rd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            rd.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            rd.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo rri = ri;
            rri.renderArea = {{0, 0}, half};
            rri.pColorAttachments = &rc;
            rri.pDepthAttachment = &rd;
            vkCmdBeginRendering(cmd, &rri);
            const VkViewport hv{0, float(half.height), float(half.width), -float(half.height), 0, 1};
            const VkRect2D hs{{0, 0}, half};
            vkCmdSetViewport(cmd, 0, 1, &hv);
            vkCmdSetScissor(cmd, 0, 1, &hs);
            viewProjD3D = original::viewProjD3D(reflection->camera, aspect);
            frameSet = reflFrameSet_[frameIndex_];
            portalClips = false;
            ScenePush rp = push;
            rp.viewProj = zRemap * reflection->camera.projection(aspect) * reflection->camera.view();
            rp.rt = 0;
            // Exterior terrain first, then a depth clear, as in the main pass (SubMesh::exterior).
            const size_t exterior = exteriorCount(meshes_, std::span<const DrawRef>(order_));
            const size_t skyAt = exteriorBlendStart(meshes_, std::span<const DrawRef>(order_), exterior, opacityOf);
            if (!exterior) drawSky(reflection->camera, half, 3, false);
            for (const DrawRef& d : order_) {
                const size_t i = size_t(&d - order_.data());
                if (exterior && i == skyAt && skyAt < exterior) drawSky(reflection->camera, half, 1, true);
                if (exterior && i == exterior) {
                    VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT};
                    clear.clearValue.depthStencil = {1.0f, 0};
                    const VkClearRect rect{hs, 0, 1};
                    vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
                    drawSky(reflection->camera, half, skyAt < exterior ? 2 : 3, true);
                }
                const GpuMesh& m = meshes_[d.mesh];
                const scene::SubMesh& s = m.submeshes[d.sub];
                if (m.water) continue;
                const scene::Blend blend = drawBlend(s, opacityOf(d.mesh));
                if (const OrigDraw* od = origDraw(d)) {
                    if (!od->screen && !od->reflection)
                        drawOriginal(d, *od, origPipeline(od->program, shadowMap ? kOrigShadowMap : kOrigPlain, blend));
                    continue;
                }
                useGeneric();
                const VkPipeline p = scenePipelines_[size_t(blend)];
                if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
                bindMesh(d.mesh);
                rp.model = m.model;
                rp.alphaRef = s.alphaRef / 255.0f;
                rp.opacity = s.blend == scene::Blend::Opaque ? -opacityOf(d.mesh) : opacityOf(d.mesh);
                vkCmdPushConstants(cmd, sceneLayout_, kPushStages, 0, sizeof rp, &rp);
                depthWrites(s, opacityOf(d.mesh), false);
                const VkDescriptorSet set = textureSet(s.textureId);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
                drawSides(s, opacityOf(d.mesh));
            }
            vkCmdEndRendering(cmd);
            const VkImageMemoryBarrier2 toRead = imageBarrier(reflColor_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorOut,
                                                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            barriers(cmd, {&toRead, 1});
            viewProjD3D = original::viewProjD3D(camera, aspect);
            frameSet = origFrameSet_[frameIndex_];
            portalClips = true;
            layout = Bound::None; // the frame set changes back
        }
        gpuMark(cmd, 4);
        // Depth pre-pass (RT shadows / AO / GI, screen-space AO; prepassPipelines_): the exterior draws first with depth
        // writes, then the pre-pass renders the inside's opaque depth + normals in a pass of its own (prepassNormal_), the
        // AO / GI passes read them, and the draws load the targets and test against the depth. No resolves before the last.
        VkRenderingAttachmentInfo color1 = color, depth1 = depth;
        VkRenderingInfo ri1 = ri;
        if (prepassPipelines_) {
            color1.resolveMode = depth1.resolveMode = VK_RESOLVE_MODE_NONE;
            color1.resolveImageView = depth1.resolveImageView = VK_NULL_HANDLE;
            color1.storeOp = depth1.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ri1.pColorAttachments = &color1, ri1.pDepthAttachment = &depth1;
        }
        vkCmdBeginRendering(cmd, &ri1);
        vkCmdSetViewport(cmd, 0, 1, &flipped);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        scissorClip = -1;
        auto drawMain = [&](const DrawRef& d) {
            const GpuMesh& m = meshes_[d.mesh];
            const scene::SubMesh& s = m.submeshes[d.sub];
            if (enhancedWater && waterDraw(d.mesh, d.sub)) return; // the water pass below draws them with the opaque copies
            const float op = opacityOf(d.mesh);
            const scene::Blend blend = drawBlend(s, op);
            // Rays on opaque surfaces (pre-passed: each visible pixel traces once) and on water for reflections; none on
            // the exterior (not pre-passed, and placed in the world only as a backdrop).
            const bool reflective = rtOn && m.water && options.rtReflections && !s.exterior;
            const bool rays = rtOn && !s.exterior && (blend == scene::Blend::Opaque || reflective);
            if (const OrigDraw* od = origDraw(d)) {
                if (od->screen) {
                    screenDraws_.push_back(d);
                    return;
                }
                drawOriginal(d, *od, origPipeline(od->program, rays ? kOrigRays : shadowMap ? kOrigShadowMap : kOrigPlain, blend));
                return;
            }
            useGeneric();
            const VkPipeline p = scenePipelines_[size_t(blend)];
            if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            bindMesh(d.mesh);
            clipTo(d.mesh);
            push.model = m.model;
            push.alphaRef = s.alphaRef / 255.0f;
            push.rt = (rays && blend == scene::Blend::Opaque ? 1 : 0) | (reflective ? 2 : 0);
            push.opacity = s.blend == scene::Blend::Opaque ? -op : op;
            vkCmdPushConstants(cmd, sceneLayout_, kPushStages, 0, sizeof push, &push);
            depthWrites(s, op, false);
            const VkDescriptorSet set = textureSet(s.textureId);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
            drawSides(s, op);
        };
        // Depth pre-pass over the leading opaque submeshes of `refs` (opaque ones come first in each drawOrder group):
        // depth + the normal target (prepass.frag, the translated programs' OU_PREPASS variant).
        auto prepass = [&](std::span<const DrawRef> refs) {
            for (const DrawRef& d : refs) {
                const scene::SubMesh& s = meshes_[d.mesh].submeshes[d.sub];
                if (drawBlend(s, opacityOf(d.mesh)) != scene::Blend::Opaque) break;
                if (enhancedWater && waterDraw(d.mesh, d.sub)) continue;
                // Translated programs pre-pass with their own (invariant) VS and the pre-pass PS (alpha test, normal out).
                if (const OrigDraw* od = origDraw(d)) {
                    if (od->screen) continue; // drawn after the copies, depth included
                    drawOriginal(d, *od, origPipeline(od->program, kOrigPrepass, s.blend), true);
                    continue;
                }
                useGeneric(); // prepass.frag reads OuShadow (eye)
                if (prepassPipeline_ != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = prepassPipeline_);
                bindMesh(d.mesh);
                clipTo(d.mesh);
                push.model = meshes_[d.mesh].model;
                push.alphaRef = s.alphaRef / 255.0f;
                vkCmdPushConstants(cmd, sceneLayout_, kPushStages, 0, sizeof push, &push);
                vkCmdSetCullMode(cmd, options.cullBackFaces && !s.doubleSided ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
                const VkDescriptorSet set = textureSet(s.textureId);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout_, 0, 1, &set, 0, nullptr);
                vkCmdDrawIndexed(cmd, s.indexCount, 1, s.firstIndex, 0, 0);
            }
        };
        // Exterior terrain first, then a depth clear: it stays behind everything (tru.exe's far depth range, SubMesh::exterior).
        // Not pre-passed: its opaque draws write depth themselves (and get no AO: the clear removes it from the pre-pass).
        const size_t exterior = exteriorCount(meshes_, std::span<const DrawRef>(order_));
        const std::span<const DrawRef> outside(order_.data(), exterior), inside(order_.data() + exterior, order_.size() - exterior);
        // The backdrop tint after the opaque exterior, its stars after the blended exterior (dome cards cover them).
        const size_t skyAt = exteriorBlendStart(meshes_, std::span<const DrawRef>(order_), exterior, opacityOf);
        if (exterior) {
            for (size_t i = 0; i < exterior; ++i) {
                if (i == skyAt) drawSky(camera, extent_, 1, true);
                drawMain(outside[i]);
            }
            VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT};
            clear.clearValue.depthStencil = {1.0f, 0};
            const VkClearRect rect{{{0, 0}, extent_}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        }
        drawSky(camera, extent_, !exterior || skyAt == exterior ? 3 : 2, exterior != 0);
        if (prepassPipelines_) {
            vkCmdEndRendering(cmd);
            const VkImage normalImage = prepassNormal_.image;
            // After the AO passes' reads of the last frame's normals, or its pre-pass store when they did not run (sync
            // validation WRITE_AFTER_WRITE on the layout transition, viewer_vulkan_matches_gl_agx).
            const VkImageMemoryBarrier2 toTarget = imageBarrier(
                normalImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kColorOut,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barriers(cmd, {&toTarget, 1});
            VkRenderingAttachmentInfo pn{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            pn.imageView = prepassNormal_.view;
            pn.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            pn.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            pn.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo pd = depth1;
            pd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            VkRenderingInfo pri = ri1;
            pri.pColorAttachments = &pn;
            pri.pDepthAttachment = &pd;
            vkCmdBeginRendering(cmd, &pri);
            vkCmdSetViewport(cmd, 0, 1, &flipped);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            scissorClip = -1;
            prepass(inside);
            vkCmdEndRendering(cmd);
            if (indirect) recordIndirect(cmd, options, rtAo, camera, aspect, zRemap, push.viewProj);
            if (cortao) recordCortao(cmd, options, camera, aspect, zRemap * camera.projection(aspect) * camera.view());
            color.loadOp = depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // ri: the same attachments, resolves included
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &flipped);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            scissorClip = -1;
            opaqueWrites = false; // pre-passed
        }
        for (const DrawRef& d : inside) drawMain(d);
        opaqueWrites = true;
        clipTo(UINT32_MAX);
        gpuMark(cmd, 5);

        // Refracting particles (TRU's water rings around a swimmer) sample the scene with the water surfaces in it, as in
        // tru.exe where they draw after the sea: the copies are refreshed (colour only) between the water and the fx.
        const bool fxRefract = needDepth && std::any_of(options.fxBatches.begin(), options.fxBatches.end(),
                                                        [](const FxBatch& b) { return b.distort > 0; });
        VkRenderingAttachmentInfo wc = color;
        wc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkRenderingAttachmentInfo wd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        wd.imageView = depth_.view;
        wd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        wd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        // The splash fluid ends the pass for its simulation and targets and resumes it (recordFluid, recordSplashFluid):
        // keep the depth then too.
        const bool splashing = enhancedWater && fluidFrame.active;
        wd.storeOp = fxRefract || splashing ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE; // fx reload it
        if (needDepth) { // second pass: water surfaces + splashes (Enhanced water), then the debug lines below
            vkCmdEndRendering(cmd);
            recordWaterCopies(cmd);
            ri.pColorAttachments = &wc;
            ri.pDepthAttachment = &wd;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &flipped);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            scissorClip = -1;
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE); // the surface also reads from below
            for (const DrawRef& d : screenDraws_) {
                const OrigDraw& od = origDraws_[d.mesh][d.sub];
                const scene::Blend blend = drawBlend(meshes_[d.mesh].submeshes[d.sub], opacityOf(d.mesh));
                const bool rays = rtOn && blend == scene::Blend::Opaque;
                drawOriginal(d, od, origPipeline(od.program, rays ? kOrigRays : shadowMap ? kOrigShadowMap : kOrigPlain, blend));
            }
            screenDraws_.clear();
            clipTo(UINT32_MAX);
        }
        gpuMark(cmd, 6);
        if (enhancedWater) {
            // Ray-traced reflections: the -DOU_RT variant traces the TLAS (set 2) instead of the SSR march.
            const bool rtWater = rtOn && options.rtReflections && waterRtPipeline_;
            // Meshes reaching into the wave field's window draw tessellated (water.tesc), the rest per vertex.
            const VkPipeline flat = rtWater ? waterRtPipeline_ : waterPipeline_, tessellated = rtWater ? waterTessRtPipeline_ : waterTessPipeline_;
            VkPipeline bound = flat;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
            // The surface also reads from below (water.frag's Snell's window); the translated screen draws above leave
            // their own cull mode set (drawOriginal: back faces for single-sided materials).
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            if (rtWater) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout_, 2, 1, &rtSet_, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout_, 0, 1, &waterSets_[frameIndex_], 0,
                                    nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout_, 1, 1, &shadowSets_[frameIndex_], 0,
                                    nullptr); // water.frag caustics: the sun shadow map
            for (uint32_t mi = 0; mi < meshes_.size(); ++mi) {
                const GpuMesh& m = meshes_[mi];
                if (!(m.water || sea_[mi]) || clippedAway(mi)) continue;
                const VkPipeline want = tessellated && waterfield::touches(options, m.center, m.radius) ? tessellated : flat;
                if (want != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = want);
                bindMesh(mi);
                clipTo(mi);
                const glm::mat4 wp[2] = {m.model, waterWaves(m.center, m.radius, options.waterVolumes)};
                vkCmdPushConstants(cmd, waterLayout_, waterPushStages(), 0, sizeof wp, wp);
                for (uint32_t s = 0; s < m.submeshes.size(); ++s)
                    if (waterDraw(mi, s)) vkCmdDrawIndexed(cmd, m.submeshes[s].indexCount, 1, m.submeshes[s].firstIndex, 0, 0), ++rtStatus_.waterSurfaces;
            }
            clipTo(UINT32_MAX);
            if (splashing) {
                vkCmdEndRendering(cmd);
                fluidMark(cmd, 0);
                recordFluid(cmd, fluidFrame);
                fluidMark(cmd, 1);
                recordSplashFluid(cmd, flipped, scissor);
                recordWaterCopies(cmd, false); // the refraction sees the water surfaces just drawn
                vkCmdBeginRendering(cmd, &ri); // ri: the second pass's load attachments (wc / wd)
                vkCmdSetViewport(cmd, 0, 1, &flipped);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
                scissorClip = -1;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splashCompositePipeline_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splashLayout_, 0, 1,
                                        &splashSets_[frameIndex_][1], 0, nullptr); // the blurred splat target
                vkCmdDraw(cmd, 3, 1, 0, 0);
                fluidMark(cmd, 2);
                splashDrawn = true;
            }
        }
        gpuMark(cmd, 7);

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
            if (fxRefract) {
                vkCmdEndRendering(cmd);
                recordWaterCopies(cmd, false);
                vkCmdBeginRendering(cmd, &ri); // ri: the second pass's load attachments (wc / wd)
                vkCmdSetViewport(cmd, 0, 1, &flipped);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                scissorClip = -1;
            }
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
        const bool particles = enhancedWater && uwParticlesOn(wb) && uwSnowPipeline_; // counted in the "fx" GPU time
        if (particles) recordUwParticles(cmd, camera, wb, options, flipped, scissor);
        gpuMark(cmd, 8);
        if (fsrOn) recordFsrMasks(cmd, options, splashDrawn, wb.weather.x > 0.5f, push.viewProj, currentVP,
                                  jitter / glm::vec2(extent_.width, extent_.height), enhancedWater, options.cullBackFaces);
        gpuMark(cmd, 9);
        // Submerged in Enhanced water (UwGrade): post.frag writes the linear frame into uwHdr_, the bloom chain and the
        // grade (abyssal-garden W_POST) write the post target instead (recordUwComposite); the grade adds the shafts.
        const UwGrade grade = enhancedWater && uwGradePipeline_ ? uwGrade(wb) : UwGrade{};
        const bool shafts = grade.on && options.lightShafts && underwaterShaftsActive(wb);
        if (shafts) recordUnderwaterShafts(cmd);
        gpuMark(cmd, 10);

        // Post pass: resolved scene → swapchain (tonemap / FXAA), then the overlay on top. With FSR the post pass writes
        // fsrInput_ at the render resolution; FSR upscales it into the swapchain image(s), then the overlay.
        const VkImage postTarget = fsrOn ? fsrInput_.image : target;
        if (blurChain) recordFilterBlur(cmd, sceneColor_.image, filterBlur_.image); // leaves sceneColor_ TRANSFER_SRC
        const VkImageMemoryBarrier2 toPost[] = {
            imageBarrier(sceneColor_.image, blurChain ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, blurChain ? VK_PIPELINE_STAGE_2_BLIT_BIT : kColorOut,
                         blurChain ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            // Chained to the acquire semaphore wait (same COLOR_ATTACHMENT_OUTPUT stage); old contents discarded.
            imageBarrier(postTarget, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorOut, 0,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        barriers(cmd, toPost);
        // tonemap: before FSR (fsrInput_ stays display-referred 0..1 like the untouched scene) and before the overlay.
        struct { int32_t fxaa, hdr; float brightness, fade; int32_t tonemap, uw; } post{
            options.fxaa && !fsrOn ? 1 : 0, options.hdr ? 1 : 0, options.brightness, std::clamp(options.screenFade, 0.0f, 1.0f),
            options.tonemap == Tonemap::Agx ? 1 : 0, (particles ? 1 : 0) | (grade.on ? 2 : 0)};
        static_assert(sizeof post == kPostPushBytes);
        auto drawPost = [&](VkPipeline pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(cmd, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof post, &post);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &postSet_, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 1, 1, &postWaterSets_[frameIndex_], 0,
                                    nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 2, 1, &shadowSets_[frameIndex_], 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        if (grade.on) recordUwComposite(cmd, [&] { drawPost(uwResolvePipeline_); });
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
        vkCmdSetScissor(cmd, 0, 1, &ri.renderArea); // the shaft passes (recordUnderwaterShafts) left a half-size one
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        if (grade.on) {
            const glm::vec4 v(shafts ? 1.0f : 0.0f, grade.exposure, post.fade, grade.strength);
            const VkDescriptorSet sets[3] = {uwHdrSet_, uwBloomSets_[0], postWaterSets_[frameIndex_]}; // 2: the shafts
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uwGradePipeline_);
            vkCmdPushConstants(cmd, uwGradeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof v, &v);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uwGradeLayout_, 0, 3, sets, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        } else {
            drawPost(postPipeline_);
        }
        if (feedback) { // 0x509630 -> 0x51A2E0: this frame's image (before the HUD) for the next frame's feedback
            vkCmdEndRendering(cmd);
            recordFilterBlur(cmd, postTarget, feedback_.image);
            const VkImageMemoryBarrier2 back = imageBarrier(postTarget, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                                            VK_ACCESS_2_NONE, kColorOut,
                                                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barriers(cmd, {&back, 1});
            swap.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // the overlay draws over the post output
            vkCmdBeginRendering(cmd, &ri);
        }
        hasFeedback_ = feedback;

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
        gpuMark(cmd, 11);

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
        gpuMark(cmd, 12);
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
        // Anti-Lag PRESENT marker of the frame whose INPUT beginInput() marked: here on the render thread, once all of
        // the frame's submits are done, not on the present thread. With frame generation the present thread then holds
        // the real image for half an interval behind a generated one; that is display pacing, not this frame's CPU
        // work, so it must not count. A present without its own INPUT (nested loops: cinematics, the loading screen)
        // gets one here, and a generated image switches Anti-Lag off (src/render/vk/AntiLag.hpp: Mesa's layer crashes
        // on more presents than INPUTs).
        if (antiLagUpdate_) antiLag_.beforePresent(generatedIndex_ != UINT32_MAX, std::bind_front(&VkRenderer::antiLagCall, this));
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

    bool fsrSupported() const override { return device_ && fsrReason_.empty(); }
    bool antiLagSupported() const override { return antiLagUpdate_ != nullptr; }

    // Anti-Lag INPUT marker (the layer may sleep here so input is sampled later). Mode follows the last drawFrame's
    // FrameOptions::antiLag; switching it off sends one OFF update, after that nothing is called.
    void beginInput() override {
        if (!antiLagUpdate_) return;
        const bool was = antiLag_.on();
        antiLag_.beginInput(antiLagWanted_, fsr_.frameGeneration(), std::bind_front(&VkRenderer::antiLagCall, this));
        if (!was && antiLag_.on()) std::fprintf(stderr, "ou: AMD Anti-Lag on\n");
    }
    void antiLagCall(AntiLagMarkers::Call call, uint64_t frameIndex) {
        if (call == AntiLagMarkers::Call::Off) {
            const VkAntiLagDataAMD off{VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, nullptr, VK_ANTI_LAG_MODE_OFF_AMD, 0, nullptr};
            antiLagUpdate_(device_, &off);
            std::fprintf(stderr, "ou: AMD Anti-Lag off%s\n", antiLagWanted_ ? " (frame generation)" : "");
            return;
        }
        const VkAntiLagPresentationInfoAMD info{VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD, nullptr,
            call == AntiLagMarkers::Call::Input ? VK_ANTI_LAG_STAGE_INPUT_AMD : VK_ANTI_LAG_STAGE_PRESENT_AMD, frameIndex};
        const VkAntiLagDataAMD data{VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, nullptr, VK_ANTI_LAG_MODE_ON_AMD, 0, &info};
        antiLagUpdate_(device_, &data);
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
    uint64_t generatedFrames() const override { return presentedGenerated_; }

    std::string description() const override {
        auto ver = [](uint32_t v) {
            return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
                   std::to_string(VK_API_VERSION_PATCH(v));
        };
        return "Vulkan " + ver(apiVersion_) + " | " + deviceName_ + " | " + driver_ + " | BC " + (bc_ ? "yes" : "NO") +
               " | aniso " + std::to_string(int(maxAniso_)) + "x | ray queries " +
               (rtStatus_.supported ? "yes" : "no (" + rtStatus_.reason + ")") + " | Anti-Lag " +
               (!antiLagUpdate_ ? "unavailable" : antiLag_.on() ? "on" : "off") + " | " +
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
            destroyImage(pssm_);
            destroyImage(attenuation_);
            destroyImage(cookies_);
            destroyImage(blackCube_);
            destroyWaterField();
            destroyOcean();
            for (auto& u : uiImages_)
                if (u.gpu.image) destroyImage(u.gpu);
            for (auto& b : waterUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            destroyFluid();
            for (auto& b : bubbleBuffers_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& m : bubbleMemory_) m.reset();
            vkDestroyPipelineLayout(device_, std::exchange(uwLayout_, VK_NULL_HANDLE), nullptr);
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
            vkDestroyPipelineLayout(device_, std::exchange(splashLayout_, VK_NULL_HANDLE), nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(splashSetLayout_, VK_NULL_HANDLE), nullptr);
            for (auto& b : origFrameUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& b : reflFrameUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            for (auto& b : shadowUbo_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
            vkDestroyDescriptorPool(device_, std::exchange(origStaticPool_, VK_NULL_HANDLE), nullptr);
            vkDestroyPipelineLayout(device_, std::exchange(origLayout_, VK_NULL_HANDLE), nullptr);
            for (VkDescriptorSetLayout* l : {&origFrameLayout_, &origMatLayout_})
                vkDestroyDescriptorSetLayout(device_, std::exchange(*l, VK_NULL_HANDLE), nullptr);
            vkDestroyDescriptorPool(device_, staticPool_, nullptr);
            destroyScenePipelines();
            destroyFsr();
            for (VkPipeline p : {textPipeline_, shadowPipeline_, pssmPipeline_, postPipeline_,
                                 shaftMarchPipeline_, shaftBlurPipeline_, uwResolvePipeline_, uwBloomPipeline_, uwBloomUpPipeline_,
                                 uwGradePipeline_, agWaterPipeline_, agCausticPipeline_})
                vkDestroyPipeline(device_, p, nullptr);
            for (VkPipelineLayout l : {sceneLayout_, textLayout_, lineLayout_, postLayout_, fxLayout_, uwGradeLayout_, agWaterLayout_, skyLayout_})
                vkDestroyPipelineLayout(device_, l, nullptr);
            for (auto& m : lineMemory_) m.reset();
            vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            vkDestroyDescriptorSetLayout(device_, std::exchange(shadowSetLayout_, VK_NULL_HANDLE), nullptr);
            destroyIndirect();
            destroyCortao();
            vkDestroyDescriptorSetLayout(device_, std::exchange(rtSetLayout_, VK_NULL_HANDLE), nullptr);
            for (VkSampler s : {sceneSampler_, fontSampler_, shadowSampler_, postSampler_, biasedSampler_, blurSampler_, agWaterSampler_}) vkDestroySampler(device_, s, nullptr);
            vkDestroyImageView(device_, std::exchange(agWaterLevel0_, VK_NULL_HANDLE), nullptr);
            if (agWater_.image) destroyImage(agWater_);
            vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
            vkDestroyCommandPool(device_, uploadPool_, nullptr);
            staticMemory_.reset();
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
            antiLagUpdate_ = nullptr;
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
    // Pixel shader variants: plain (no sun term), cascaded shadow map, ray-traced sun + water reflections (render/sun.glsl
    // via originalPixelSource), depth pre-pass (OU_PREPASS: alpha test + the AO normal target, with the VS's pre-pass
    // variant). Pipelines: [variant][scene::Blend], pre-pass at [kOrigPrepass][0].
    enum OrigVariant { kOrigPlain, kOrigShadowMap, kOrigRays, kOrigPrepass };
    struct OrigProgram {
        std::vector<uint32_t> vs, vsPrepass;       // SPIR-V (libshaderc at upload); vsPrepass: originalPrepassVertex
        std::string vsGlsl, psGlsl;                // translated shaders
        std::array<std::vector<uint32_t>, 4> ps;   // per OrigVariant (compiled at upload; rays only with RT support)
        uint8_t attributes = 0;                    // vertex attribute locations the VS reads
        std::array<std::array<VkPipeline, 4>, 4> pipelines{}; // created on first use
        uint64_t vsHash = 0, psHash = 0;           // original::Program::vs / ps: identity across uploads (origStash_)
    };
    struct OrigDraw {
        int program = -1; // origPrograms_ index, -1 = generic shading
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t screen = 0; // original::screenSamplers: low 16 back buffer, high 16 depth (drawn after the copies)
        uint32_t reflection = 0; // original::reflectionSamplers: read reflColor_, InstanceParams 0-2 = the reflection
    };

    VkPipeline origPipeline(int program, OrigVariant variant, scene::Blend blend) {
        OrigProgram& o = origPrograms_[size_t(program)];
        const bool prepass = variant == kOrigPrepass;
        VkPipeline& p = o.pipelines[variant][prepass ? 0 : size_t(blend)];
        if (!p) {
            std::string error;
            if (o.ps[size_t(variant)].empty()) {
                o.ps[size_t(variant)] =
                    original::spirv(originalPixelSource(o.psGlsl, true, variant == kOrigRays, prepass), true, error);
                if (o.ps[size_t(variant)].empty()) { // keep drawing without the sun term (pre-pass: no normals)
                    std::fprintf(stderr, "[vk] original pixel shader variant %d: %s\n", int(variant), error.c_str());
                    o.ps[size_t(variant)] = o.ps[kOrigPlain];
                }
            }
            if (prepass && o.vsPrepass.empty()) {
                o.vsPrepass = original::spirv(originalPrepassVertex(o.vsGlsl), false, error);
                if (o.vsPrepass.empty()) std::fprintf(stderr, "[vk] original pre-pass vertex shader: %s\n", error.c_str());
            }
            const bool withNormal = prepass && !o.vsPrepass.empty();
            std::vector<VkVertexInputAttributeDescription> attrs; // only consumed locations (validation-clean)
            for (const auto& a : kOriginalAttrs)
                if ((o.attributes | (withNormal ? 1u << sm3::kAttrNormal : 0u)) >> a.location & 1) attrs.push_back(a);
            p = createPipeline(withNormal ? o.vsPrepass : o.vs, o.ps[size_t(variant)], origLayout_, &kSceneBinding, attrs,
                               prepass ? PipelineKind::Prepass : PipelineKind(blend), samples_,
                               prepass ? kPrepassNormalFormat : sceneFormat_, depthFormat_);
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
                if (prepassPipelines_ && opaque && !od.screen) n.push_back({kOrigPrepass, blend});
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

    static void destroyPipelines(VkDevice device, std::vector<OrigProgram>& programs) {
        for (auto& o : programs)
            for (auto& byBlend : o.pipelines)
                for (VkPipeline& p : byBlend) vkDestroyPipeline(device, std::exchange(p, VK_NULL_HANDLE), nullptr);
    }
    void destroyOriginalPipelines() {
        destroyPipelines(device_, origPrograms_);
        destroyPipelines(device_, origStash_);
        origWarmKey_ = -1;
    }

    // Translates and compiles each material's programs (the previous upload's ones carry over: a streamed unit
    // re-uploads the scene, and compiling every program again stalled the crossing ~650 ms), then one material UBO
    // block + descriptor set per distinct (program, material, instance textures, alpha ref) of the uploaded submeshes.
    void uploadOriginals(const scene::SceneData& scene, std::vector<std::string>& warnings) {
        environment_ = scene.environment;
        uploadSun_ = original::sunFields(environment_);
        irradiance_.reset(); // mesh indices and probes change with the scene
        const original::Prepared prep = original::prepare(scene, true);
        for (const auto& w : prep.warnings) warnings.push_back(w);
        const size_t n = prep.programs.size();
        origPrograms_.resize(n);
        std::map<std::pair<uint64_t, uint64_t>, size_t> stashed;
        for (size_t s = 0; s < origStash_.size(); ++s)
            if (!origStash_[s].vs.empty()) stashed.emplace(std::pair{origStash_[s].vsHash, origStash_[s].psHash}, s);
        std::vector<bool> reused(n);
        for (size_t i = 0; i < n; ++i) {
            const auto it = stashed.find({prep.programs[i].vs, prep.programs[i].ps});
            if (it == stashed.end() || origStash_[it->second].psGlsl != prep.programs[i].psGlsl.source) continue;
            origPrograms_[i] = std::exchange(origStash_[it->second], {});
            stashed.erase(it);
            reused[i] = true;
        }
        bury([this, unused = std::move(origStash_)]() mutable { destroyPipelines(device_, unused); }); // in-flight frames
        origStash_.clear();
        // Every new program's VS and PS variants compile in parallel here (shaderc is reentrant): compiling the sun /
        // rays variants on first draw cost ~30 % of l4_ballcourt's frames as new materials came into view.
        constexpr size_t kTasks = 6;
        std::vector<std::array<std::string, kTasks>> errors(n); // per task: each written by one thread
        const bool rays = rtStatus_.supported;
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t task; (task = next++) < n * kTasks;) {
                const size_t i = task / kTasks;
                if (reused[i]) continue;
                const auto& p = prep.programs[i];
                OrigProgram& o = origPrograms_[i];
                std::string& error = errors[i][task % kTasks];
                switch (task % kTasks) {
                case 0: o.vs = original::spirv(p.vsGlsl.source, false, error); break;
                case 1: o.ps[kOrigPlain] = original::spirv(p.psGlsl.source, true, error); break;
                case 2: o.ps[kOrigShadowMap] = original::spirv(originalPixelSource(p.psGlsl.source, true, false), true, error); break;
                case 3: if (rays) o.ps[kOrigRays] = original::spirv(originalPixelSource(p.psGlsl.source, true, true), true, error); break;
                case 4: o.vsPrepass = original::spirv(originalPrepassVertex(p.vsGlsl.source), false, error); break;
                case 5: o.ps[kOrigPrepass] = original::spirv(originalPixelSource(p.psGlsl.source, true, false, true), true, error); break;
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
            o.vsGlsl = prep.programs[i].vsGlsl.source, o.psGlsl = prep.programs[i].psGlsl.source;
            o.vsHash = prep.programs[i].vs, o.psHash = prep.programs[i].ps;
            if (o.vs.empty()) {
                o.ps = {};
                o.vsPrepass.clear();
            }
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
            bool exterior; // MaterialBlock::fixed[3] value 16 (fillMaterial)
            auto operator<=>(const Key&) const = default;
        };
        std::map<Key, size_t> keys;
        std::vector<std::pair<Key, const scene::SubMesh*>> unique;
        std::vector<std::vector<size_t>> slot(meshes_.size());
        origDraws_.assign(meshes_.size(), {});
        for (size_t mi = 0; mi < meshes_.size(); ++mi) {
            meshes_[mi].irradiance = false;
            for (const auto& s : meshes_[mi].submeshes) {
                OrigDraw d;
                d.program = prep.program(s.materialId);
                if (d.program >= 0 && !ok[size_t(d.program)]) d.program = -1;
                if (d.program >= 0) {
                    d.screen = original::screenSamplers(*prep.materials.at(s.materialId), s.instanceTextures,
                                                        prep.programs[size_t(d.program)].psGlsl.readsDepth);
                    d.reflection = original::reflectionSamplers(*prep.materials.at(s.materialId));
                    meshes_[mi].irradiance |= prep.programs[size_t(d.program)].psGlsl.readsIrradiance;
                }
                size_t k = SIZE_MAX;
                if (d.program >= 0) {
                    const Key key{d.program, s.materialId, {s.instanceTextures[0], s.instanceTextures[1], s.instanceTextures[2],
                                                            s.instanceTextures[3]}, s.alphaRef, meshes_[mi].water, s.exterior};
                    const auto [it, fresh] = keys.try_emplace(key, unique.size());
                    if (fresh) unique.push_back({key, &s});
                    k = it->second;
                }
                origDraws_[mi].push_back(d);
                slot[mi].push_back(k);
            }
        }
        if (unique.empty()) return;
        origMaterialUbo_ = createBuffer(gen_->memory, stride * unique.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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
                if (ps.samplerType[n] == sm3::kSamplerCookies) ii[size_t(n)] = {postSampler_, cookies_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                if (ps.samplerType[n] == sm3::kSamplerShadow) ii[size_t(n)] = {shadowSampler_, pssm_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
                    if (od.reflection) reflectSets_.push_back({od.set, od.reflection});
                }
        for (size_t si = 0; si < scene.meshes.size() && si < meshSlot_.size(); ++si)
            if (meshSlot_[si] < meshes_.size()) {
                GpuMesh& g = meshes_[meshSlot_[si]];
                g.reflectZ = original::reflectionPlaneZ(scene.meshes[si], g.model, prep.materials);
                g.reflective.clear();
                for (const auto& s : g.submeshes) {
                    const auto it = prep.materials.find(s.materialId);
                    g.reflective.push_back(it != prep.materials.end() && original::reflectionSamplers(*it->second) != 0);
                }
            }
        writeScreenSets();
    }

    // Back/depth-buffer samplers of the translated materials (OrigDraw::screen) -> the single-sample opaque copies;
    // rewritten with the water sets when the targets change.
    void writeScreenSets() {
        if (!opaqueColor_.view || !opaqueDepth_.view) return;
        for (const auto& [set, mask] : reflectSets_) // the planar reflection target (reflColor_)
            for (uint32_t n = 0; n < 16; ++n) {
                if (!(mask >> n & 1)) continue;
                const VkDescriptorImageInfo ii{postSampler_, reflColor_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = set;
                w.dstBinding = 1 + n;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &ii;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
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
        // Settings that change the look (menu, mid-run): drop the temporal histories (FSR, RT AO / GI, screen-space AO) so
        // the old image is not smeared into the new one. Weather is left out: its intensity fades every frame under shelter.
        const std::array<float, 15> look{float(o.rt), float(o.rtRays), float(o.rtLights), float(o.rtReflections), float(o.ao),
                                         float(o.aoSamples), float(o.rtGi), float(o.originalShaders), float(o.water),
                                         float(o.waterDetail), float(o.atmosphere), float(o.lightShafts), float(o.shadows),
                                         float(o.hdr), o.brightness};
        if (look != look_) look_ = look, fsrHistory_ = false, indLast_ = 0, cortaoLast_ = 0;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        for (int s = fsrActiveRequest() ? 1 : std::clamp(o.msaa, 1, 8); s > 1; s >>= 1)
            if (supportedSamples_ & VkSampleCountFlags(s)) { samples = VkSampleCountFlagBits(s); break; }
        const VkFormat format = o.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const bool rt = rtStatus_.supported && o.rt != RayTracing::Off;
        // A depth pre-pass whenever something reads it: RT (shadows, AO / GI) or the screen-space AO.
        const bool prepass = rt || (o.ao != AmbientOcclusion::Off && cortaoReason_.empty());
        if (samples == samples_ && format == sceneFormat_ && rt == rtPipelines_ && prepass == prepassPipelines_ && scenePipelines_[0])
            return;
        waitIdle();
        samples_ = samples;
        sceneFormat_ = format;
        rtPipelines_ = rt;
        prepassPipelines_ = prepass;
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
        vkDestroyPipeline(device_, std::exchange(waterRtPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(waterTessPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipeline(device_, std::exchange(waterTessRtPipeline_, VK_NULL_HANDLE), nullptr);
        for (VkPipeline* p : {&splashSplatPipeline_, &splashBlurPipeline_, &splashCompositePipeline_, &uwSnowPipeline_, &uwBubblePipeline_})
            vkDestroyPipeline(device_, std::exchange(*p, VK_NULL_HANDLE), nullptr);
        for (VkPipeline& p : fxPipelines_) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
        for (VkPipeline& p : skyPipelines_) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
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
                if (!lodShown(m, lodEye_) || outsideOrtho(sun.lightViewProj[c], m.center, m.radius)) continue;
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                push.model = m.model;
                for (const auto& s : m.submeshes) {
                    if ((s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) || !s.castsShadow) continue;
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

    // tru.exe's PSSM atlas (original::Pssm): cleared to 1 (the area outside the split tiles is the ShadowLights' lit
    // texel), one 1024² tile per split with the shadow map's casters. The splits' matrices are D3D clip (y up, z 0..1):
    // negative-height viewports put row 0 at the top, as tru.exe's lookups count rows.
    void recordPssmPass(VkCommandBuffer cmd, const original::Pssm& p) {
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        const VkImageMemoryBarrier2 toDepth = imageBarrier(
            pssm_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        barriers(cmd, {&toDepth, 1});
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = pssm_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {uint32_t(original::kPssmWidth), uint32_t(original::kPssmHeight)}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pssmPipeline_);
        vkCmdSetDepthBias(cmd, 0.0f, 0.0f, p.slopeBias);
        ScenePush push{};
        for (int k = 0; k < original::kPssmSplits; ++k) {
            constexpr float t = float(original::kPssmTile);
            const VkViewport viewport{float(p.tile[k].x), float(p.tile[k].y) + t, t, -t, 0, 1};
            const VkRect2D tile{{p.tile[k].x, p.tile[k].y}, {uint32_t(original::kPssmTile), uint32_t(original::kPssmTile)}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &tile);
            push.viewProj = p.tileClip[k];
            for (const auto& m : meshes_) {
                if (!lodShown(m, lodEye_) || outsideOrtho(push.viewProj, m.center, m.radius)) continue;
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                push.model = m.model;
                for (const auto& s : m.submeshes) {
                    if ((s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) || !s.castsShadow) continue;
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
            pssm_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        barriers(cmd, {&toRead, 1});
    }

    // ---- RT ambient occlusion + one-bounce sun GI (shaders/rt_indirect.comp, docs/RENDERING.md "RT AO / GI") ----
    // Half render resolution: trace from the depth pre-pass (cosine hemisphere rays, AO + sun bounce, receiver albedo
    // and direct sun from a primary ray), temporal accumulation, a-trous x3 into ind_.final, which the opaque ray-traced
    // draws composite through sun.glsl ouIndirect. Recorded with no render pass active; leaves depth_ DEPTH_ATTACHMENT.
    static constexpr float kAoStrength = 0.85f, kGiStrength = 1.5f, kAoRange = 250.0f, kGiRange = 2000.0f;
    struct IndirectPush { glm::mat4 reproject; glm::vec4 depthW, sunColor, sky; glm::ivec4 cfg; }; // rt_indirect.comp Push
    void recordIndirect(VkCommandBuffer cmd, const FrameOptions& o, bool ao, const Camera& camera, float aspect,
                        const glm::mat4& zRemap, const glm::mat4& viewProj) {
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        // Pre-pass depth -> sampled; this frame's vertex uploads / BLAS refits / TLAS update and the earlier frames'
        // fragment reads of the targets -> this pass.
        const VkImageMemoryBarrier2 toRead =
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                         kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkMemoryBarrier2 in{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
                                  VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kCompute,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                                      VK_ACCESS_2_SHADER_WRITE_BIT,
                                  kCompute,
                                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
        barriers(cmd, {&toRead, 1}, &in);
        // History: the previous frame ran the pass and the camera did not cut (FSR's 500-unit rule).
        const glm::dvec3 eye(camera.position);
        const bool history = indLast_ == frameCounter_ && glm::distance(eye, indPrevEye_) < 500.0;
        // Reprojection in double (both view-projections carry the ~40000-unit world translation; RENDERING.md "FSR"):
        // eye-relative position -> previous jittered clip.
        glm::dmat4 toWorld(1.0);
        toWorld[3] = glm::dvec4(eye, 1.0);
        const glm::mat4 proj = zRemap * camera.projection(aspect);
        const scene::Environment& e = environment_;
        const glm::vec3 sunColor = e.sun ? glm::vec3(e.sunColor[0], e.sunColor[1], e.sunColor[2]) : glm::vec3(0.7f);
        // Escaping rays see a dim sky in the fog colour (the art's ambient already holds most of the sky light).
        const glm::vec3 sky = 0.15f * glm::vec3(e.fogColor[0], e.fogColor[1], e.fogColor[2]);
        IndirectPush p{glm::mat4(indPrevVP_ * toWorld), {proj[2][2], proj[3][2], kAoRange, kGiRange},
                       {sunColor * kGiStrength, kAoStrength}, {sky * kGiStrength, 0.0f},
                       {(ao ? 1 : 0) | (o.rtGi ? 2 : 0) | (history ? 4 : 0), int(frameCounter_ % 1024), 0, o.rtRays >= 4 && o.rtGi ? 2 : 1}};
        indPrevVP_ = glm::dmat4(viewProj), indPrevEye_ = eye, indLast_ = frameCounter_ + 1;
        const VkDescriptorSet sets[3] = {indSets_[frameIndex_], rtSet_, shadowSets_[frameIndex_]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, indLayout_, 0, 3, sets, 0, nullptr);
        const uint32_t gx = (indExtent_.width + 7) / 8, gy = (indExtent_.height + 7) / 8;
        const VkMemoryBarrier2 step{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute, VK_ACCESS_2_SHADER_WRITE_BIT, kCompute,
                                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
        auto dispatch = [&](VkPipeline pipeline, int z) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            p.cfg.z = z;
            vkCmdPushConstants(cmd, indLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cmd, gx, gy, 1);
            barriers(cmd, {}, &step);
        };
        dispatch(samples_ != VK_SAMPLE_COUNT_1_BIT ? indTraceMs_ : indTrace_, 0);
        for (int z : {0, 1, 2, 4}) dispatch(indDenoise_, z); // temporal, a-trous strides 1 / 2 / 4
        const VkImageMemoryBarrier2 back =
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         kCompute, 0, kDepthStages,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkMemoryBarrier2 out{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute, VK_ACCESS_2_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT};
        barriers(cmd, {&back, 1}, &out);
    }

    // Set 0 = depth + the targets (per frame in flight: the two geometry / history / length images swap roles), set 1 =
    // the RT set, set 2 = the sun shadow set (OuShadow: eye, camera rays, sun). RT devices only.
    void createIndirect() {
        constexpr VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
        std::array<VkDescriptorSetLayoutBinding, 12> b{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr};
        for (uint32_t i = 1; i < b.size(); ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, uint32_t(b.size()), b.data()};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &indSetLayout_));
        const VkDescriptorSetLayout sets[3] = {indSetLayout_, rtSetLayout_, shadowSetLayout_};
        const VkPushConstantRange push{cs, 0, sizeof(IndirectPush)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 3, sets, 1, &push};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &indLayout_));
        auto pipeline = [&](std::span<const uint32_t> code) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shaderModule(code),
                        "main", nullptr};
            ci.layout = indLayout_;
            VkPipeline p;
            VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &p));
            vkDestroyShaderModule(device_, ci.stage.module, nullptr);
            return p;
        };
        indTrace_ = pipeline(kRtIndirect), indTraceMs_ = pipeline(kRtIndirectMs), indDenoise_ = pipeline(kRtDenoise);
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 11 * kFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 2, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &indPool_));
        for (VkDescriptorSet& s : indSets_) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, indPool_, 1, &indSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &s));
        }
    }
    // The splash fluid (render/Fluid.hpp, shaders/fluid.comp): the particle pool, the neighbour grid and the C3 kick buffer
    // (device-local; the pool and kicks zeroed: every particle dead, no kicks), the bursts (host-visible, one per frame in
    // flight), the compute pipeline and one set per frame in flight (written by writeWaterSets: depth / ripples change).
    void createFluid() {
        constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        fluidParticles_ = createBuffer(staticMemory_, fluid::kParticleBytes, kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        fluidGrid_ = createBuffer(staticMemory_, fluid::kGridBytes, kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        fluidKicks_ = createBuffer(staticMemory_, fluid::kKickBytes, kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (Buffer& b : fluidBursts_)
            b = createBuffer(staticMemory_, fluid::kBurstBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        oneShot([&](VkCommandBuffer cmd) {
            vkCmdFillBuffer(cmd, fluidParticles_.buffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(cmd, fluidKicks_.buffer, 0, VK_WHOLE_SIZE, 0);
            const VkMemoryBarrier2 done{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
            barriers(cmd, {}, &done);
        });
        constexpr VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorSetLayoutBinding b[8] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, cs, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
                                                   {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
                                                   {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
                                                   {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
                                                   {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr},
                                                   {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr},
                                                   {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 8, b};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &fluidSetLayout_));
        const VkPushConstantRange push{cs, 0, sizeof(fluid::Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &fluidSetLayout_, 1, &push};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &fluidLayout_));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                    shaderModule(kFluidComp), "main", nullptr};
        ci.layout = fluidLayout_;
        VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &fluidPipeline_));
        vkDestroyShaderModule(device_, ci.stage.module, nullptr);
        const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 3, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &fluidPool_));
        for (VkDescriptorSet& s : fluidSets_) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, fluidPool_, 1, &fluidSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &s));
        }
    }
    void destroyFluid() {
        vkDestroyPipeline(device_, std::exchange(fluidPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipelineLayout(device_, std::exchange(fluidLayout_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(fluidPool_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorSetLayout(device_, std::exchange(fluidSetLayout_, VK_NULL_HANDLE), nullptr);
        for (Buffer* b : {&fluidParticles_, &fluidGrid_, &fluidKicks_}) vkDestroyBuffer(device_, std::exchange(*b, {}).buffer, nullptr);
        for (Buffer& b : fluidBursts_) vkDestroyBuffer(device_, std::exchange(b, {}).buffer, nullptr);
    }
    // This frame's splash fluid (fluid::Frame): zero the C3 kick count, kill every particle if asked, emit the bursts, then
    // each fixed step: clear the grid's bucket heads, Predict, kIterations x (Lambda, Delta), Finalize (one dispatch each,
    // a barrier between). Outside a render pass, after the opaque depth copy; the pool is read by splash.vert after it.
    void recordFluid(VkCommandBuffer cmd, const fluid::Frame& f) {
        constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kFill = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        constexpr VkAccessFlags2 kRw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
            const VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, src, srcAccess, dst, dstAccess};
            barriers(cmd, {}, &m);
        };
        // last frame's splat reads of the pool, a kick consumer, the previous fluid passes' writes
        barrier(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kFill | kCompute,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | kRw);
        vkCmdFillBuffer(cmd, fluidKicks_.buffer, 0, 16, 0);
        if (f.clear) vkCmdFillBuffer(cmd, fluidParticles_.buffer, 0, VK_WHOLE_SIZE, 0);
        barrier(kFill, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, kRw);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fluidPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fluidLayout_, 0, 1, &fluidSets_[frameIndex_], 0, nullptr);
        auto run = [&](fluid::Pass pass, int source, int threads) {
            const fluid::Push p = fluidSim_.push(pass, f, source);
            vkCmdPushConstants(cmd, fluidLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cmd, uint32_t((threads + fluid::kThreads - 1) / fluid::kThreads), 1, 1);
            barrier(kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute | kFill, kRw | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        };
        if (f.emitted) run(fluid::Emit, 0, f.emitted);
        for (int s = 0; s < f.steps; ++s) {
            vkCmdFillBuffer(cmd, fluidGrid_.buffer, 0, VkDeviceSize(fluid::kHashSize) * 4, 0xFFFFFFFFu);
            barrier(kFill, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, kRw);
            run(fluid::Predict, 0, fluid::kCapacity);
            for (int it = 0; it < fluid::kIterations; ++it) {
                run(fluid::Lambda, it & 1, fluid::kCapacity);
                run(fluid::Delta, it & 1, fluid::kCapacity);
            }
            run(fluid::Finalize, 0, fluid::kCapacity);
        }
        barrier(kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | kCompute, kRw);
    }
    void fluidMark(VkCommandBuffer cmd, uint32_t i) { // OU_VK_PRESENT_STATS: the splash fluid's timestamps (kFluidMarks)
        if (!presentStats_ || !frames_[frameIndex_].gpuTimes) return;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frames_[frameIndex_].gpuTimes, kGpuMarks + i);
        frames_[frameIndex_].fluidTimed = true;
    }
    // FFT ocean (render/Ocean.hpp, shaders/ocean.comp): the h0 spectra (device-local SSBO), the FFT scratch, the rgba16f
    // uOcean array (always GENERAL: ocean.comp's storage image, sampled by water.vert / water.frag / the post caustics;
    // cleared so frames before the first dispatch see a flat sea) with kLevels mips, one storage view per mip and one
    // compute set per mip step (set l: binding 2 = level l, 3 = level l + 1; the FFT of grid 128 / 64 runs in set 0 / 1)
    // plus the Up set (binding 2 = level 1, 3 = level 0).
    void createOcean() {
        const auto spectrum = ocean::spectrumBuffer();
        const VkDeviceSize specBytes = spectrum.size() * sizeof(glm::vec4);
        oceanSpectrum_ = createBuffer(staticMemory_, specBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        oceanWork_ = createBuffer(staticMemory_, ocean::kWorkVec4s * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        ocean_ = createImage(staticMemory_, kFormat, ocean::kMaxN, ocean::kMaxN, ocean::kLevels,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, false, ocean::kLayers);
        for (uint32_t level = 0; level < uint32_t(ocean::kLevels); ++level) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = ocean_.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            vi.format = kFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, uint32_t(ocean::kLayers)};
            VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &oceanStorage_[level]));
        }
        UploadBatch batch;
        batch.buffers.push_back({oceanSpectrum_.buffer, {spectrum.data(), specBytes, 0, 0}});
        flush(batch, [&](VkCommandBuffer cmd) {
            const auto clear = imageBarrier(ocean_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            barriers(cmd, {&clear, 1});
            const VkClearColorValue zero{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(ocean::kLevels), 0, uint32_t(ocean::kLayers)};
            vkCmdClearColorImage(cmd, ocean_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
            const auto done = imageBarrier(ocean_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            barriers(cmd, {&done, 1});
        });
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; // water.frag filters through the mips; the rest pick the grid's level
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.anisotropyEnable = maxAniso_ > 1.0f; // grazing views keep the waves across the line of sight
        sci.maxAnisotropy = std::clamp(maxAniso_, 1.0f, ocean::kMaxAniso);
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &oceanSampler_));

        constexpr VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr},
                                                   {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr},
                                                   {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, b};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &oceanSetLayout_));
        const VkPushConstantRange push{cs, 0, sizeof(ocean::Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &oceanSetLayout_, 1, &push};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &oceanLayout_));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shaderModule(kOceanComp), "main", nullptr};
        ci.layout = oceanLayout_;
        VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &oceanPipeline_));
        vkDestroyShaderModule(device_, ci.stage.module, nullptr);
        constexpr uint32_t kSets = uint32_t(ocean::kLevels); // kLevels - 1 mip steps + Up
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSets}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kSets, 2, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &oceanPool_));
        for (uint32_t s = 0; s < kSets; ++s) {
            const bool up = s == kSets - 1;
            const uint32_t src = up ? 1 : s, dst = up ? 0 : s + 1;
            const int n = s == 0 ? 128 : 64; // the FFT's grid in sets 0 / 1 (the rest never read the spectra)
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, oceanPool_, 1, &oceanSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &oceanSets_[s]));
            const VkDescriptorBufferInfo spec{oceanSpectrum_.buffer, ocean::spectrumOffset(n) * sizeof(glm::vec4),
                                              VkDeviceSize(ocean::kCascades) * VkDeviceSize(n * n) * sizeof(glm::vec4)};
            const VkDescriptorBufferInfo work{oceanWork_.buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorImageInfo img[2] = {{VK_NULL_HANDLE, oceanStorage_[src], VK_IMAGE_LAYOUT_GENERAL},
                                                  {VK_NULL_HANDLE, oceanStorage_[dst], VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet w[4] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            for (uint32_t i = 0; i < 4; ++i) w[i].dstSet = oceanSets_[s], w[i].dstBinding = i, w[i].descriptorCount = 1;
            w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[0].pBufferInfo = &spec, w[1].pBufferInfo = &work;
            w[2].descriptorType = w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[2].pImageInfo = &img[0], w[3].pImageInfo = &img[1];
            vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
        }
    }
    void destroyOcean() {
        vkDestroyPipeline(device_, std::exchange(oceanPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipelineLayout(device_, std::exchange(oceanLayout_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(oceanPool_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorSetLayout(device_, std::exchange(oceanSetLayout_, VK_NULL_HANDLE), nullptr);
        vkDestroySampler(device_, std::exchange(oceanSampler_, VK_NULL_HANDLE), nullptr);
        for (VkImageView& v : oceanStorage_) vkDestroyImageView(device_, std::exchange(v, VK_NULL_HANDLE), nullptr);
        if (ocean_.image) destroyImage(ocean_);
        vkDestroyBuffer(device_, std::exchange(oceanSpectrum_, {}).buffer, nullptr);
        vkDestroyBuffer(device_, std::exchange(oceanWork_, {}).buffer, nullptr);
    }
    // This frame's ocean: spectrum, row and column inverse DFTs, assemble (one dispatch each, z = cascade), then grid
    // 64's doubling into level 0 and the mip chain below the grid (Ocean.hpp Pass), after the previous frames' reads of
    // uOcean and before this frame's water / post reads.
    void recordOcean(VkCommandBuffer cmd, const FrameOptions& o) {
        ocean::Push p = oceanClock_.frame(o);
        const int n = p.cfg.y, lod = ocean::gridLod(n);
        constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkAccessFlags2 kRw = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        const VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kReaders | kCompute, kRw, kCompute, kRw};
        barriers(cmd, {}, &war);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, oceanPipeline_);
        auto run = [&](size_t set, int pass, int size, bool lines) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, oceanLayout_, 0, 1, &oceanSets_[set], 0, nullptr);
            p.cfg.x = pass, p.cfg.y = size;
            vkCmdPushConstants(cmd, oceanLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cmd, lines ? uint32_t(size) : uint32_t((size * size + ocean::kThreads - 1) / ocean::kThreads), 1, ocean::kCascades);
            const VkMemoryBarrier2 step{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute, kRw, kReaders | kCompute, kRw};
            barriers(cmd, {}, &step);
        };
        for (int pass : {ocean::Spectrum, ocean::Rows, ocean::Columns, ocean::Assemble})
            run(size_t(lod), pass, n, pass == ocean::Rows || pass == ocean::Columns);
        if (lod > 0) run(size_t(ocean::kLevels - 1), ocean::Up, ocean::kMaxN, false);
        for (int l = lod; l < ocean::kLevels - 1; ++l) run(size_t(l), ocean::Mip, ocean::kMaxN >> (l + 1), false);
    }
    // Interactive wave field (render/WaterField.hpp, shaders/water_field.comp): its state and FFT scratch (device-local
    // SSBOs), per frame in flight the kick buffer (contract C3) and the bodies + wall mask (host-visible), the external
    // kick buffer (setExternalWaveKicks; a zero-count dummy until one is given) and uRipples, the rgba16f texture the
    // water shaders read (always GENERAL: water_field.comp's storage image; cleared, unread until a frame ran).
    void createWaterField() {
        constexpr VkDeviceSize cells = VkDeviceSize(waterfield::kN) * waterfield::kN;
        constexpr VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        constexpr VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        fieldState_ = createBuffer(staticMemory_, cells * sizeof(glm::vec4), ssbo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); // h, v, whitewater, spare
        fieldWork_ = createBuffer(staticMemory_, cells * sizeof(glm::vec2), ssbo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        fieldNoKicks_ = createBuffer(staticMemory_, sizeof(waterfield::KickBuffer), ssbo, host);
        std::memset(fieldNoKicks_.mapped, 0, sizeof(waterfield::KickBuffer));
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            fieldKicks_[i] = createBuffer(staticMemory_, sizeof(waterfield::KickBuffer), ssbo, host);
            fieldFrame_[i] = createBuffer(staticMemory_, sizeof(waterfield::FrameBuffer), ssbo, host);
        }
        ripples_ = createImage(staticMemory_, VK_FORMAT_R16G16B16A16_SFLOAT, waterfield::kN, waterfield::kN, 1,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        UploadBatch batch;
        flush(batch, [&](VkCommandBuffer cmd) {
            const auto clear = imageBarrier(ripples_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            barriers(cmd, {&clear, 1});
            const VkClearColorValue zero{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, ripples_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
            const auto done = imageBarrier(ripples_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            barriers(cmd, {&done, 1});
        });
        constexpr VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutBinding b[7];
        for (uint32_t i = 0; i < 5; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr};
        b[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr};
        b[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 7, b};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &fieldSetLayout_));
        const VkPushConstantRange push{cs, 0, sizeof(waterfield::Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &fieldSetLayout_, 1, &push};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &fieldLayout_));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shaderModule(kWaterFieldComp), "main", nullptr};
        ci.layout = fieldLayout_;
        VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &fieldPipeline_));
        vkDestroyShaderModule(device_, ci.stage.module, nullptr);
        const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 3, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &fieldPool_));
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, fieldPool_, 1, &fieldSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &fieldSets_[i]));
        }
        writeWaterFieldSets();
    }
    void writeWaterFieldSets() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            const VkDescriptorBufferInfo buf[5] = {{fieldState_.buffer, 0, VK_WHOLE_SIZE},
                                                   {fieldWork_.buffer, 0, VK_WHOLE_SIZE},
                                                   {fieldKicks_[i].buffer, 0, VK_WHOLE_SIZE},
                                                   {fieldExternal_ ? fieldExternal_ : fieldNoKicks_.buffer, 0, VK_WHOLE_SIZE},
                                                   {fieldFrame_[i].buffer, 0, VK_WHOLE_SIZE}};
            const VkDescriptorImageInfo oc{oceanSampler_, ocean_.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo img{VK_NULL_HANDLE, ripples_.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[7];
            for (uint32_t b = 0; b < 7; ++b) {
                w[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[b].dstSet = fieldSets_[i], w[b].dstBinding = b, w[b].descriptorCount = 1;
                w[b].descriptorType = b < 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : b == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (b < 5) w[b].pBufferInfo = &buf[b];
            }
            w[5].pImageInfo = &oc, w[6].pImageInfo = &img;
            vkUpdateDescriptorSets(device_, 7, w, 0, nullptr);
        }
    }
    // Contract C3 (b): a GPU buffer with the kick layout (`uint count; uint pad[3]; vec4 kicks[4096]`, k = world x, y,
    // centre displacement, sigma) that the field consumes each frame it runs, setting its count back to 0; written by
    // the GPU before this frame's field (e.g. WaterFluid's particle impacts). VK_NULL_HANDLE detaches it.
    void setExternalWaveKicks(VkBuffer buffer) {
        if (buffer == fieldExternal_) return;
        waitIdle(); // the sets are in use by frames in flight
        fieldExternal_ = buffer;
        writeWaterFieldSets();
    }
    void destroyWaterField() {
        vkDestroyPipeline(device_, std::exchange(fieldPipeline_, VK_NULL_HANDLE), nullptr);
        vkDestroyPipelineLayout(device_, std::exchange(fieldLayout_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(fieldPool_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorSetLayout(device_, std::exchange(fieldSetLayout_, VK_NULL_HANDLE), nullptr);
        if (ripples_.image) destroyImage(ripples_);
        for (Buffer* b : {&fieldState_, &fieldWork_, &fieldNoKicks_, &fieldKicks_[0], &fieldKicks_[1], &fieldFrame_[0], &fieldFrame_[1]})
            vkDestroyBuffer(device_, std::exchange(*b, {}).buffer, nullptr);
    }
    // This frame's field (WaterField.hpp Pass, one dispatch each), after the previous frames' reads of uRipples and the
    // ocean's passes (rough-sea damping reads uOcean), before this frame's water / post reads. False: the field rests.
    bool recordWaterField(VkCommandBuffer cmd, const FrameOptions& o, const WaterBlock& wb) {
        static_assert(kFramesInFlight == 2);
        auto* kicks = reinterpret_cast<waterfield::KickBuffer*>(fieldKicks_[frameIndex_].mapped);
        auto* frame = reinterpret_cast<waterfield::FrameBuffer*>(fieldFrame_[frameIndex_].mapped);
        std::optional<waterfield::Push> push = fieldStepper_.frame(o, *kicks, *frame);
        if (!push || !fieldPipeline_) return false;
        waterfield::Push p = *push;
        if (wb.ocean[0].x > 0.5f) p.cfg.w |= waterfield::kOcean, p.ocean = glm::vec4(glm::vec3(wb.ocean[1]), wb.ocean[0].y);
        constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkAccessFlags2 kRw = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        const VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kReaders | kCompute, kRw, kCompute, kRw};
        barriers(cmd, {}, &war);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fieldPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fieldLayout_, 0, 1, &fieldSets_[frameIndex_], 0, nullptr);
        for (waterfield::Pass pass : waterfield::kPasses) {
            p.cfg.x = pass;
            vkCmdPushConstants(cmd, fieldLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cmd, waterfield::groups(pass), 1, 1);
            const VkMemoryBarrier2 step{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute, kRw, kReaders | kCompute, kRw};
            barriers(cmd, {}, &step);
        }
        return true;
    }
    void destroyIndirect() {
        for (VkPipeline* p : {&indTrace_, &indTraceMs_, &indDenoise_}) vkDestroyPipeline(device_, std::exchange(*p, VK_NULL_HANDLE), nullptr);
        vkDestroyPipelineLayout(device_, std::exchange(indLayout_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(indPool_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorSetLayout(device_, std::exchange(indSetLayout_, VK_NULL_HANDLE), nullptr);
    }
    std::array<GpuImage*, 11> indImages() {
        return {&ind_.signal, &ind_.geom[0], &ind_.geom[1], &ind_.albedo, &ind_.hist[0], &ind_.hist[1], &ind_.len[0], &ind_.len[1],
                &ind_.tmpA, &ind_.tmpB, &ind_.final};
    }
    // Per swapchain (half the render resolution), with the RT pipelines: the targets in GENERAL, cleared (no history,
    // no surface), and their descriptors, including the shadow sets' ouIndirectMap / ouIndirectGeom (binding 2 / 3).
    void createIndirectTargets() {
        if (!rtPipelines_ || !indLayout_) return;
        indExtent_ = {(extent_.width + 1) / 2, (extent_.height + 1) / 2};
        constexpr VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (GpuImage* img : indImages()) {
            const VkFormat f = img == &ind_.albedo                           ? VK_FORMAT_R8G8B8A8_UNORM
                               : img == &ind_.len[0] || img == &ind_.len[1] ? VK_FORMAT_R32G32_SFLOAT
                                                                             : VK_FORMAT_R16G16B16A16_SFLOAT;
            *img = createImage(swapMemory_, f, indExtent_.width, indExtent_.height, 1, u, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        oneShot([&](VkCommandBuffer cmd) {
            std::vector<VkImageMemoryBarrier2> b;
            for (GpuImage* img : indImages())
                b.push_back(imageBarrier(img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));
            barriers(cmd, b);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            for (GpuImage* img : indImages()) vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
            const VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
            barriers(cmd, {}, &m);
        });
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            const uint32_t j = (i + 1) % kFramesInFlight; // the previous frame's
            const VkImageView views[14] = {depth_.view, ind_.signal.view, ind_.geom[i].view, ind_.albedo.view, ind_.geom[j].view,
                                           ind_.hist[j].view, ind_.hist[i].view, ind_.len[j].view, ind_.len[i].view,
                                           ind_.tmpA.view, ind_.tmpB.view, ind_.final.view,
                                           ind_.final.view, ind_.geom[i].view}; // 12, 13: shadow set binding 2, 3
            VkDescriptorImageInfo ii[14];
            VkWriteDescriptorSet w[14];
            for (uint32_t n = 0; n < 14; ++n) {
                const bool shadowSet = n >= 12;
                ii[n] = {fontSampler_, views[n], n == 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
                w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[n].dstSet = shadowSet ? shadowSets_[i] : indSets_[i];
                w[n].dstBinding = shadowSet ? n - 10 : n;
                w[n].descriptorCount = 1;
                w[n].descriptorType = n == 0 || shadowSet ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[n].pImageInfo = &ii[n];
            }
            vkUpdateDescriptorSets(device_, 14, w, 0, nullptr);
        }
        indLast_ = 0; // new targets: no history
    }

    // ---- CORTAO screen-space AO (shaders/cortao_*.comp, render/Cortao.hpp, docs/RENDERING.md "Ambient occlusion") ----
    // Full render resolution from the depth pre-pass (depth + interpolated world normals, MSAA sample 0): CORTAO's
    // prepass, main pass and 13-tap filter, then our temporal accumulation into aoMap_[frame], which the opaque draws read
    // through sun.glsl ouAo (screen.w bit 4) and multiply into their ambient / indirect light only.
    bool cortaoReady() const { return prepassPipelines_ && cortaoReason_.empty() && cortaoPacked_.image && prepassNormal_.image; }

    // Set 0 bindings (one set for all passes; cortao_common.glsl): 0 depth, 1 normal, 2 packed mips (storage), 3 packed,
    // 4 / 14 the 16 / 32-ray occluder tables, 5 / 6 / 7 samplers (border int, border float, clamp), 8 / 9 main pass
    // output (storage / sampled), 10 / 11 filter output, 12 history (last frame's aoMap_), 13 this frame's aoMap_.
    static constexpr uint32_t kCortaoPush = 128;
    void createCortao() {
        if (!cortaoFeatures_) {
            cortaoReason_ = "no shaderStorageImageExtendedFormats / shaderStorageImageArrayDynamicIndexing / shaderImageGatherExtended";
            std::fprintf(stderr, "[vk] CORTAO ambient occlusion unavailable: %s\n", cortaoReason_.c_str());
            return;
        }
        constexpr VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
        std::array<VkDescriptorSetLayoutBinding, 15> b{};
        for (uint32_t i = 0; i < b.size(); ++i) {
            const VkDescriptorType t = i == 2 || i == 8 || i == 10 || i == 13 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                       : i >= 5 && i <= 7                     ? VK_DESCRIPTOR_TYPE_SAMPLER
                                                                              : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            b[i] = {i, t, i == 2 ? 8u : 1u, cs, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, uint32_t(b.size()), b.data()};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &cortaoSetLayout_));
        const VkPushConstantRange push{cs, 0, kCortaoPush};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &cortaoSetLayout_, 1, &push};
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &cortaoLayout_));
        auto pipeline = [&](std::span<const uint32_t> code) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shaderModule(code),
                        "main", nullptr};
            ci.layout = cortaoLayout_;
            VkPipeline p;
            VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &p));
            vkDestroyShaderModule(device_, ci.stage.module, nullptr);
            return p;
        };
        cortaoPrepass_[0] = pipeline(kCortaoPrepass), cortaoPrepass_[1] = pipeline(kCortaoPrepassMs);
        const std::span<const uint32_t> mains[4][2] = {{kCortaoMain8, kCortaoMain8Ms}, {kCortaoMain12, kCortaoMain12Ms},
                                                       {kCortaoMain16, kCortaoMain16Ms}, {kCortaoMain32, kCortaoMain32Ms}};
        for (int q = 0; q < 4; ++q)
            for (int m = 0; m < 2; ++m) cortaoMain_[q][m] = pipeline(mains[q][m]);
        cortaoFilter_ = pipeline(kCortaoFilter), cortaoTemporal_ = pipeline(kCortaoTemporal);
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &cortaoBorderInt_));
        sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &cortaoBorderFloat_));
        // The occluder tables: 24 x 24 x 16 texels, uploaded once.
        auto table = [&](VkFormat format, std::span<const uint8_t> bytes) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_3D;
            ci.format = format;
            ci.extent = {24, 24, 16};
            ci.mipLevels = ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            GpuImage img;
            VK_CHECK(vkCreateImage(device_, &ci, nullptr, &img.image));
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device_, img.image, &req);
            const auto a = staticMemory_.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkBindImageMemory(device_, img.image, a.memory, a.offset));
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &img.view));
            const Buffer staging = createBuffer(staticMemory_, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            std::memcpy(staging.mapped, bytes.data(), bytes.size());
            oneShot([&](VkCommandBuffer cmd) {
                const auto toDst = imageBarrier(img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                barriers(cmd, {&toDst, 1});
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = ci.extent;
                vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                const auto toRead = imageBarrier(img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                barriers(cmd, {&toRead, 1});
            });
            vkDestroyBuffer(device_, staging.buffer, nullptr); // its memory stays in staticMemory_ (36 KiB)
            return img;
        };
        cortaoTable16_ = table(VK_FORMAT_R16_UINT, kCortaoTable16);
        cortaoTable32_ = table(VK_FORMAT_R32_UINT, kCortaoTable32);
        const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 11 * kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_SAMPLER, 3 * kFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 3, ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &cortaoPool_));
        for (VkDescriptorSet& s : cortaoSets_) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, cortaoPool_, 1, &cortaoSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &s));
        }
        cortaoReason_.clear();
    }
    void destroyCortao() {
        for (VkPipeline* p : {&cortaoPrepass_[0], &cortaoPrepass_[1], &cortaoFilter_, &cortaoTemporal_})
            vkDestroyPipeline(device_, std::exchange(*p, VK_NULL_HANDLE), nullptr);
        for (auto& q : cortaoMain_)
            for (VkPipeline& p : q) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
        for (VkSampler* s : {&cortaoBorderInt_, &cortaoBorderFloat_}) vkDestroySampler(device_, std::exchange(*s, VK_NULL_HANDLE), nullptr);
        for (GpuImage* img : {&cortaoTable16_, &cortaoTable32_})
            if (img->image) destroyImage(*img);
        vkDestroyPipelineLayout(device_, std::exchange(cortaoLayout_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorPool(device_, std::exchange(cortaoPool_, VK_NULL_HANDLE), nullptr);
        vkDestroyDescriptorSetLayout(device_, std::exchange(cortaoSetLayout_, VK_NULL_HANDLE), nullptr);
    }

    // Per swapchain (render resolution). The AO maps always exist (shadow set binding 4 is statically used), cleared to
    // "no surface"; the pre-pass normal target with a depth pre-pass, the CORTAO intermediates when it can run.
    void createCortaoTargets() {
        const bool cortao = cortaoReason_.empty() && prepassPipelines_;
        constexpr VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (GpuImage& m : aoMap_)
            m = createImage(swapMemory_, VK_FORMAT_R16G16_SFLOAT, extent_.width, extent_.height, 1,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (cortaoReason_.empty() ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                            VK_IMAGE_ASPECT_COLOR_BIT);
        if (prepassPipelines_)
            prepassNormal_ = createImage(swapMemory_, kPrepassNormalFormat, extent_.width, extent_.height, 1,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                         samples_);
        if (cortao) {
            cortaoMips_ = cortao::mipCount(extent_.width, extent_.height);
            cortaoPacked_ = createImage(swapMemory_, VK_FORMAT_R32_UINT, extent_.width, extent_.height, cortaoMips_, kStorage,
                                        VK_IMAGE_ASPECT_COLOR_BIT);
            for (uint32_t i = 0; i < cortaoMipViews_.size(); ++i) {
                if (i >= cortaoMips_) {
                    cortaoMipViews_[i] = VK_NULL_HANDLE;
                    continue;
                }
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image = cortaoPacked_.image;
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.format = VK_FORMAT_R32_UINT;
                vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1};
                VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &cortaoMipViews_[i]));
            }
            cortaoRaw_ = createImage(swapMemory_, VK_FORMAT_R8_UNORM, extent_.width, extent_.height, 1, kStorage, VK_IMAGE_ASPECT_COLOR_BIT);
            cortaoFiltered_ = createImage(swapMemory_, VK_FORMAT_R16_SFLOAT, extent_.width, extent_.height, 1, kStorage,
                                          VK_IMAGE_ASPECT_COLOR_BIT);
        }
        oneShot([&](VkCommandBuffer cmd) { // GENERAL from the start; the maps cleared to 0 (no surface: ouAo = 1)
            std::vector<VkImageMemoryBarrier2> bs;
            for (const GpuImage* img : {&aoMap_[0], &aoMap_[1], &cortaoPacked_, &cortaoRaw_, &cortaoFiltered_})
                if (img->image)
                    bs.push_back(imageBarrier(img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                              VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));
            barriers(cmd, bs);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            for (const GpuImage& m : aoMap_) vkCmdClearColorImage(cmd, m.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
            const VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
            barriers(cmd, {}, &mb);
        });
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            const VkDescriptorImageInfo map{fontSampler_, aoMap_[i].view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = shadowSets_[i];
            w.dstBinding = 4;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &map;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
        if (cortao) {
            const uint32_t last = cortaoMips_ - 1;
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                const uint32_t j = (i + 1) % kFramesInFlight; // the previous frame's map: history
                constexpr VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                std::array<VkDescriptorImageInfo, 8> mips;
                for (uint32_t m = 0; m < mips.size(); ++m) mips[m] = {VK_NULL_HANDLE, cortaoMipViews_[std::min(m, last)], G};
                const VkDescriptorImageInfo ii[14] = {
                    {VK_NULL_HANDLE, depth_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, prepassNormal_.view, R},
                    {}, {VK_NULL_HANDLE, cortaoPacked_.view, G}, {VK_NULL_HANDLE, cortaoTable16_.view, R},
                    {cortaoBorderInt_, VK_NULL_HANDLE, G}, {cortaoBorderFloat_, VK_NULL_HANDLE, G}, {fontSampler_, VK_NULL_HANDLE, G},
                    {VK_NULL_HANDLE, cortaoRaw_.view, G}, {VK_NULL_HANDLE, cortaoRaw_.view, G},
                    {VK_NULL_HANDLE, cortaoFiltered_.view, G}, {VK_NULL_HANDLE, cortaoFiltered_.view, G},
                    {VK_NULL_HANDLE, aoMap_[j].view, G}, {VK_NULL_HANDLE, aoMap_[i].view, G}};
                const VkDescriptorImageInfo table32{VK_NULL_HANDLE, cortaoTable32_.view, R};
                std::array<VkWriteDescriptorSet, 15> w{};
                for (uint32_t n = 0; n < w.size(); ++n) {
                    w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w[n].dstSet = cortaoSets_[i];
                    w[n].dstBinding = n;
                    w[n].descriptorCount = n == 2 ? 8u : 1u;
                    w[n].descriptorType = n == 2 || n == 8 || n == 10 || n == 13 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                          : n >= 5 && n <= 7                     ? VK_DESCRIPTOR_TYPE_SAMPLER
                                                                                 : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    w[n].pImageInfo = n == 2 ? mips.data() : n == 14 ? &table32 : &ii[n];
                }
                vkUpdateDescriptorSets(device_, uint32_t(w.size()), w.data(), 0, nullptr);
            }
        }
        cortaoLast_ = 0; // new targets: no history
    }
    void destroyCortaoTargets() {
        for (VkImageView& v : cortaoMipViews_) vkDestroyImageView(device_, std::exchange(v, VK_NULL_HANDLE), nullptr);
        for (GpuImage* img : {&aoMap_[0], &aoMap_[1], &prepassNormal_, &cortaoPacked_, &cortaoRaw_, &cortaoFiltered_})
            if (img->image) destroyImage(*img);
    }

    // Records the passes after the pre-pass (no render pass active); leaves depth_ DEPTH_ATTACHMENT, the normal target
    // read-only (the next pre-pass discards it) and aoMap_[frameIndex_] for the fragment shaders.
    struct CortaoPrepassPush { cortao::PrepassParams params; glm::vec4 viewRows[3], unproject; };
    struct CortaoMainPush { cortao::MainParams params; glm::vec4 viewRows[3]; };
    struct CortaoTemporalPush { glm::mat4 reproject; glm::vec4 unproject, size; glm::ivec4 cfg; };
    static_assert(sizeof(CortaoPrepassPush) <= kCortaoPush && sizeof(CortaoMainPush) <= kCortaoPush &&
                  sizeof(CortaoTemporalPush) <= kCortaoPush);
    void recordCortao(VkCommandBuffer cmd, const FrameOptions& o, const Camera& camera, float aspect, const glm::mat4& viewProj) {
        constexpr VkPipelineStageFlags2 kDepthStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr VkPipelineStageFlags2 kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        aoMark(cmd, 0);
        const VkImageMemoryBarrier2 in[2] = {
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                         kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT),
            imageBarrier(prepassNormal_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        };
        // The intermediates and maps: the previous frames' compute / fragment reads before this frame's writes.
        const VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, kCompute,
                                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
        barriers(cmd, in, &war);
        const uint32_t w = extent_.width, h = extent_.height;
        // Seed per frame (temporal accumulation below, FSR's jittered accumulation): CORTAO's pattern moves every frame.
        const uint32_t seed = uint32_t(frameCounter_ * 0x9E3779B9ull >> 7) ^ 0x5bd1e995u;
        const glm::mat4 proj = camera.projection(aspect); // GL convention, FSR jitter included (camera.jitter)
        const cortao::Params cp = cortao::params(w, h, seed, proj, camera.nearZ, camera.farZ, true);
        const glm::mat4 view = camera.view();
        glm::vec4 rows[3];
        for (int k = 0; k < 3; ++k) rows[k] = glm::vec4(view[0][k], view[1][k], view[2][k], 0.0f);
        const glm::vec4 unproject(cp.main.unproject[0], cp.main.unproject[1], cp.main.unproject[2], cp.main.unproject[3]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cortaoLayout_, 0, 1, &cortaoSets_[frameIndex_], 0, nullptr);
        const VkMemoryBarrier2 step{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute, VK_ACCESS_2_SHADER_WRITE_BIT, kCompute,
                                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
        auto dispatch = [&](VkPipeline p, const void* push, uint32_t size, uint32_t gx, uint32_t gy) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdPushConstants(cmd, cortaoLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, push);
            vkCmdDispatch(cmd, gx, gy, 1);
            barriers(cmd, {}, &step);
        };
        const int ms = samples_ != VK_SAMPLE_COUNT_1_BIT ? 1 : 0;
        CortaoPrepassPush pre{cp.prepass, {rows[0], rows[1], rows[2]}, unproject};
        dispatch(cortaoPrepass_[ms], &pre, sizeof pre, (w + 7) / 8, (h + 7) / 8);
        const int samples = cortao::sampleCount(o.aoSamples);
        const int quality = samples <= 8 ? 0 : samples <= 12 ? 1 : samples <= 16 ? 2 : 3;
        CortaoMainPush main{cp.main, {rows[0], rows[1], rows[2]}};
        dispatch(cortaoMain_[quality][ms], &main, sizeof main, (w + 15) / 16, (h + 15) / 16);
        dispatch(cortaoFilter_, &cp.filter, sizeof cp.filter, (w + 15) / 16, (h + 15) / 16);
        // Temporal: this frame's view space -> the previous frame's jittered clip, in double (world translation ~40000).
        const glm::dvec3 eye(camera.position);
        const bool history = cortaoLast_ == frameCounter_ && glm::distance(eye, cortaoPrevEye_) < 500.0;
        CortaoTemporalPush tp{glm::mat4(cortaoPrevVP_ * glm::inverse(glm::dmat4(view))), unproject,
                              {float(w), float(h), 1.0f / float(w), 1.0f / float(h)}, {history ? 1 : 0, 0, 0, 0}};
        dispatch(cortaoTemporal_, &tp, sizeof tp, (w + 7) / 8, (h + 7) / 8);
        cortaoPrevVP_ = glm::dmat4(viewProj), cortaoPrevEye_ = eye, cortaoLast_ = frameCounter_ + 1;
        const VkImageMemoryBarrier2 back =
            imageBarrier(depth_.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         kCompute, 0, kDepthStages,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkMemoryBarrier2 out{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCompute, VK_ACCESS_2_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT};
        barriers(cmd, {&back, 1}, &out);
        aoMark(cmd, 1);
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
        auto hasLayer = [&](const char* name) {
            return std::any_of(layers.begin(), layers.end(), [&](const auto& l) { return std::strcmp(l.layerName, name) == 0; });
        };
        validation_ = wantValidation && hasLayer(kValidation);
        std::vector<const char*> enabledLayers;
        if (validation_) enabledLayers.push_back(kValidation);
        // AMD Anti-Lag (docs/RENDERING.md "Anti-Lag"): RADV has no VK_AMD_anti_lag of its own; Mesa's implicit layer
        // provides it but loads only with ENABLE_LAYER_MESA_ANTI_LAG=1, so request it by name when installed. It stays
        // idle (no vkAntiLagUpdateAMD calls) while the setting is off. AMD's own driver exposes the extension directly.
        const char* kAntiLag = "VK_LAYER_MESA_anti_lag";
        if (hasLayer(kAntiLag)) enabledLayers.push_back(kAntiLag);

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
        ci.enabledLayerCount = uint32_t(enabledLayers.size());
        ci.ppEnabledLayerNames = enabledLayers.data();
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
        if (props.properties.limits.maxPushConstantsSize < sizeof(sm3::ObjectBlock)) // 256: the translated shaders' object block
            throw std::runtime_error("maxPushConstantsSize < " + std::to_string(sizeof(sm3::ObjectBlock)));

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
        // Enhanced water's tessellated surface (water.tesc / water.tese); OU_WATER_NO_TESS for comparisons.
        waterTess_ = supported.tessellationShader && !std::getenv("OU_WATER_NO_TESS");
        f2.features.tessellationShader = waterTess_;
        f2.features.samplerAnisotropy = supported.samplerAnisotropy;
        // CORTAO (createCortao): r8 / r16f / rg16f storage images, the packed buffer's mips as a storage image array,
        // textureGatherOffset in cortao_filter.comp (SPIR-V ImageGatherExtended).
        cortaoFeatures_ = supported.shaderStorageImageExtendedFormats && supported.shaderStorageImageArrayDynamicIndexing &&
                          supported.shaderImageGatherExtended;
        f2.features.shaderStorageImageExtendedFormats = f2.features.shaderStorageImageArrayDynamicIndexing =
            f2.features.shaderImageGatherExtended = cortaoFeatures_;
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
        VkPhysicalDeviceAntiLagFeaturesAMD antiLag{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
        if (hasExtension(deviceExtensions(gpu_), VK_AMD_ANTI_LAG_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &antiLag};
            vkGetPhysicalDeviceFeatures2(gpu_, &q);
        }
        if (antiLag.antiLag) {
            extensions.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
            antiLag.pNext = f2.pNext;
            f2.pNext = &antiLag;
        }
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
        antiLagUpdate_ = antiLag.antiLag
            ? reinterpret_cast<PFN_vkAntiLagUpdateAMD>(vkGetDeviceProcAddr(device_, "vkAntiLagUpdateAMD")) : nullptr;
        antiLag_.reset();
        std::fprintf(stderr, "ou: AMD Anti-Lag %s\n", antiLagUpdate_ ? "available (VK_AMD_anti_lag)" : "unavailable (no VK_AMD_anti_lag)");
        if (rtStatus_.supported) loadRayTracingFunctions();
        staticMemory_.init(device_, gpu_);
        for (auto& m : lineMemory_) m.init(device_, gpu_, 0);
        for (auto& m : bubbleMemory_) m.init(device_, gpu_, 0);
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
        // Binding 2 / 3: the RT AO / GI composite map and its geometry (sun.glsl ouIndirect; createIndirectTargets).
        constexpr VkShaderStageFlags fcs = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // + rt_indirect.comp
        // Binding 4: the screen-space AO map (sun.glsl ouAoMap; createCortaoTargets), always written.
        const VkDescriptorSetLayoutBinding sb[5] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fcs, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, fcs, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fcs, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fcs, nullptr},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fcs, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 5, sb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &shadowSetLayout_));
        if (rtStatus_.supported) {
            constexpr VkShaderStageFlags fs = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // + rt_indirect.comp
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
        VkPushConstantRange postPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPostPushBytes}; // post.frag's Push
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
        // Enhanced water: set 0 = WaterBlock UBO + opaque colour/depth + ripple field + the FFT ocean (uOcean) + (5) the
        // animated water texture (uAgWater: agWater_, recordAgWater; bed caustics).
        // The tessellated surface (waterTess_) displaces in water.tese and reads the block in water.tesc too.
        const VkShaderStageFlags tess = waterTess_ ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0;
        const VkDescriptorSetLayoutBinding wb[6] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tess, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tess, nullptr}, // water.vert / .tese displace by it
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tess, nullptr}, // ... and by this
            {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // uAgWater
        };
        VkDescriptorSetLayoutCreateInfo wl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 6, wb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &wl, nullptr, &waterSetLayout_));
        // Post: set 1 = WaterBlock UBO + opaque depth (underwater, atmosphere, weather) + the screen filters' blur levels
        // (filterBlur_) + the ripple field (caustics) + the feedback effect's previous frame (feedback_) + the FFT ocean
        // (caustics) + the under-water sun shafts' march output and blurred shafts (6: the blur's input, 7: the grade's;
        // shaftTargets_, recordUnderwaterShafts) + (9) the under-water particles (uwParticles_; recordUwParticles) + (10)
        // the animated water texture (uAgWater: agWater_, recordAgWater; caustics, shafts); no binding 8. Set 2 = the sun
        // shadow set (shafts).
        VkDescriptorSetLayoutBinding pb[10];
        for (uint32_t i = 0; i < 10; ++i) {
            const uint32_t b = i < 8 ? i : i + 1;
            pb[i] = {b, b ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo pwl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 10, pb};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &pwl, nullptr, &postWaterSetLayout_));
        VkPushConstantRange modelPush{waterPushStages(), 0, 2 * sizeof(glm::mat4)}; // model, render::waterWaves (.tese: the waves)
        // 1: sun shadow (caustics); 2 (RT devices): the scene TLAS for water_rt.frag's reflections.
        const VkDescriptorSetLayout waterSets[3] = {waterSetLayout_, shadowSetLayout_, rtSetLayout_};
        VkPipelineLayoutCreateInfo wpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, rtSetLayout_ ? 3u : 2u, waterSets, 1,
                                       &modelPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &wpl, nullptr, &waterLayout_));
        // Splash fluid passes: set 0 = WaterBlock UBO + the pass input (opaque depth / a splat target) + (2) the particle
        // pool (splash.vert) + (3) the scene colour copy (the composite's refraction); push = blur direction.
        const VkDescriptorSetLayoutBinding splashBindings[4] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo ssl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, splashBindings};
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ssl, nullptr, &splashSetLayout_));
        VkPushConstantRange blurPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec2)};
        VkPipelineLayoutCreateInfo spl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &splashSetLayout_, 1, &blurPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &spl, nullptr, &splashLayout_));
        const VkDescriptorSetLayout postSets[3] = {setLayout_, postWaterSetLayout_, shadowSetLayout_}; // 2: sun shafts
        pl.setLayoutCount = 3;
        pl.pSetLayouts = postSets;
        pl.pPushConstantRanges = &postPush;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &postLayout_));
        // The animated water texture's passes (recordAgWater): no sets, push = the time (uw_water.frag and
        // uw_caustics.vert AgWaterPush).
        const VkPushConstantRange agPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
        const VkPipelineLayoutCreateInfo agl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &agPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &agl, nullptr, &agWaterLayout_));
        // The time of day backdrop (render/TimeOfDay.hpp SkyPush): no sets.
        const VkPushConstantRange skyPushRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPush)};
        const VkPipelineLayoutCreateInfo skl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &skyPushRange};
        VK_CHECK(vkCreatePipelineLayout(device_, &skl, nullptr, &skyLayout_));
        // Under-water particles (recordUwParticles): the post pass's sets 1-2 (set 0 unused), UwParticlePush.
        VkPushConstantRange uwPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UwParticlePush)};
        pl.pPushConstantRanges = &uwPush;
        VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &uwLayout_));
        VkPushConstantRange linePush{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo ll{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &linePush};
        VK_CHECK(vkCreatePipelineLayout(device_, &ll, nullptr, &lineLayout_));
        // set 0 = the sprite texture, 1 = the water set's opaque colour / depth copies, 2 = the normal map (FxBatch).
        const VkDescriptorSetLayout fxSets[3] = {setLayout_, waterSetLayout_, setLayout_};
        VkPushConstantRange fxPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::mat4) + 2 * sizeof(glm::vec4)};
        VkPipelineLayoutCreateInfo fxl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 3, fxSets, 1, &fxPush};
        VK_CHECK(vkCreatePipelineLayout(device_, &fxl, nullptr, &fxLayout_));
        // Under-water composite (recordUwComposite): sets 0 and 1 = one image each (the bloom chain binds only set 0), set
        // 2 = the post-water set (the grade reads the blurred shafts, binding 7).
        const VkDescriptorSetLayout gradeSets[3] = {setLayout_, setLayout_, postWaterSetLayout_};
        VkPushConstantRange gradePush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4)};
        VkPipelineLayoutCreateInfo gl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 3, gradeSets, 1, &gradePush};
        VK_CHECK(vkCreatePipelineLayout(device_, &gl, nullptr, &uwGradeLayout_));

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
        shaftMarchPipeline_ = createPipeline(kPostVert, kShaftMarchFrag, postLayout_, nullptr, {}, PipelineKind::Post,
                                             VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        shaftBlurPipeline_ = createPipeline(kPostVert, kShaftBlurFrag, postLayout_, nullptr, {}, PipelineKind::Post,
                                            VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        agWaterPipeline_ = createPipeline(kPostVert, kAgWaterFrag, agWaterLayout_, nullptr, {}, PipelineKind::Post,
                                          VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        agCausticPipeline_ = createPipeline(kAgCausticVert, kAgCausticFrag, agWaterLayout_, nullptr, {}, PipelineKind::CausticSplat,
                                            VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        // Under-water composite (render/Water.hpp UwGrade): post.frag writing the linear frame, the bloom chain (up passes
        // added: Additive with alpha 1), the grade into the post target.
        uwResolvePipeline_ = createPipeline(kPostVert, kPostFrag, postLayout_, nullptr, {}, PipelineKind::Post,
                                            VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        uwBloomPipeline_ = createPipeline(kPostVert, kUwBloomFrag, uwGradeLayout_, nullptr, {}, PipelineKind::Post,
                                          VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        uwBloomUpPipeline_ = createPipeline(kPostVert, kUwBloomFrag, uwGradeLayout_, nullptr, {}, PipelineKind::Additive,
                                            VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED);
        uwGradePipeline_ = createPipeline(kPostVert, kUwGradeFrag, uwGradeLayout_, nullptr, {}, PipelineKind::Post,
                                          VK_SAMPLE_COUNT_1_BIT, swap, VK_FORMAT_UNDEFINED);
        shadowPipeline_ = createPipeline(kShadowVert, kShadowFrag, sceneLayout_, &kSceneBinding, kShadowAttrs,
                                         PipelineKind::Shadow, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, shadowFormat_);
        pssmPipeline_ = createPipeline(kShadowVert, kShadowFrag, sceneLayout_, &kSceneBinding, kShadowAttrs,
                                       PipelineKind::Pssm, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, shadowFormat_);

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
        if (rtStatus_.supported) createIndirect();
        if (fsrReason_.empty()) createFsrPipelines();
    }

    // One pipeline per scene::Blend plus the debug lines, at the current MSAA count and scene format.
    void createScenePipelines() {
        // With a depth pre-pass the colour pass needs the invariant VS (scene_rt.vert) to test LEQUAL against it.
        const std::span<const uint32_t> vert = prepassPipelines_ ? std::span<const uint32_t>(kSceneRtVert) : kSceneVert;
        for (size_t b = 0; b < scenePipelines_.size(); ++b)
            scenePipelines_[b] = createPipeline(vert, rtPipelines_ ? std::span<const uint32_t>(kSceneRtFrag) : kSceneFrag, sceneLayout_,
                                                &kSceneBinding, kSceneAttrs, PipelineKind(b), samples_, sceneFormat_, depthFormat_);
        if (prepassPipelines_)
            prepassPipeline_ = createPipeline(kPrepassVert, kPrepassFrag, sceneLayout_, &kSceneBinding, kSceneAttrs,
                                              PipelineKind::Prepass, samples_, kPrepassNormalFormat, depthFormat_);
        const VkVertexInputBindingDescription lineBinding{0, sizeof(LineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription lineAttrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, pos)},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineVertex, color)},
        };
        linePipeline_ = createPipeline(kLineVert, kLineFrag, lineLayout_, &lineBinding, lineAttrs, PipelineKind::Lines,
                                       samples_, sceneFormat_, depthFormat_);
        waterPipeline_ = createPipeline(kWaterVert, kWaterFrag, waterLayout_, &kSceneBinding, kSceneAttrs, PipelineKind::Water,
                                        samples_, sceneFormat_, depthFormat_);
        if (rtPipelines_)
            waterRtPipeline_ = createPipeline(kWaterVert, kWaterRtFrag, waterLayout_, &kSceneBinding, kSceneAttrs, PipelineKind::Water,
                                              samples_, sceneFormat_, depthFormat_);
        // Tessellated (waterTess_: water.tesc / water.tese subdivide the coarse surface in the wave field's window).
        if (waterTess_) {
            waterTessPipeline_ = createPipeline(kWaterTessVert, kWaterFrag, waterLayout_, &kSceneBinding, kSceneAttrs, PipelineKind::Water,
                                                samples_, sceneFormat_, depthFormat_, kWaterTesc, kWaterTese);
            if (rtPipelines_)
                waterTessRtPipeline_ = createPipeline(kWaterTessVert, kWaterRtFrag, waterLayout_, &kSceneBinding, kSceneAttrs,
                                                      PipelineKind::Water, samples_, sceneFormat_, depthFormat_, kWaterTesc, kWaterTese);
        }
        // The splash fluid (render/Water.hpp splash passes): splat (MAX-blended) and blur into the RGBA32F targets,
        // composite into the scene's second pass. Without blendable RGBA32F (not required by the spec) there is no fluid.
        VkFormatProperties rgba;
        vkGetPhysicalDeviceFormatProperties(gpu_, kSplashFormat, &rgba);
        if (rgba.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) {
            splashSplatPipeline_ = createPipeline(kSplashVert, kSplashSplat, splashLayout_, nullptr, {}, PipelineKind::SplashSplat,
                                                  VK_SAMPLE_COUNT_1_BIT, kSplashFormat, VK_FORMAT_UNDEFINED);
            splashBlurPipeline_ = createPipeline(kSplashScreen, kSplashBlur, splashLayout_, nullptr, {}, PipelineKind::SplashBlur,
                                                 VK_SAMPLE_COUNT_1_BIT, kSplashFormat, VK_FORMAT_UNDEFINED);
            splashCompositePipeline_ = createPipeline(kSplashScreen, kSplashComposite, splashLayout_, nullptr, {},
                                                      PipelineKind::SplashComposite, samples_, sceneFormat_, depthFormat_);
        }
        // Enhanced under-water particles (render/Water.hpp UwParticlePush): premultiplied (the splash composite's blend)
        // into the RGBA16F particle target, no depth attachment.
        const VkVertexInputBindingDescription bubbleBinding{0, sizeof(Bubble), VK_VERTEX_INPUT_RATE_INSTANCE};
        const VkVertexInputAttributeDescription bubbleAttrs[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Bubble, pos)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Bubble, alpha)},
        };
        uwSnowPipeline_ = createPipeline(kUwSnowVert, kUwSnowFrag, uwLayout_, nullptr, {}, PipelineKind::SplashComposite,
                                         VK_SAMPLE_COUNT_1_BIT, kUwParticleFormat, VK_FORMAT_UNDEFINED);
        uwBubblePipeline_ = createPipeline(kUwBubbleVert, kUwBubbleFrag, uwLayout_, &bubbleBinding, bubbleAttrs,
                                           PipelineKind::SplashComposite, VK_SAMPLE_COUNT_1_BIT, kUwParticleFormat,
                                           VK_FORMAT_UNDEFINED);
        const VkVertexInputBindingDescription fxBinding{0, sizeof(FxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription fxAttrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FxVertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FxVertex, uv)},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(FxVertex, rgba)},
        };
        for (size_t b = 0; b < fxPipelines_.size(); ++b)
            fxPipelines_[b] = createPipeline(kFxVert, kFxFrag, fxLayout_, &fxBinding, fxAttrs,
                                             PipelineKind(size_t(PipelineKind::FxAlpha) + b), samples_, sceneFormat_, depthFormat_);
        for (size_t m = 0; m < skyPipelines_.size(); ++m)
            skyPipelines_[m] = createPipeline(kSkyVert, kSkyFrag, skyLayout_, nullptr, {},
                                              m ? PipelineKind::SkyAdd : PipelineKind::SkyMultiply, samples_, sceneFormat_, depthFormat_);
    }

    // `binding` null = no vertex input (fullscreen triangle). colorFormat UNDEFINED = depth-only. tesc / tese: optional
    // tessellation stages (both or neither; patch lists of 3 control points).
    VkPipeline createPipeline(std::span<const uint32_t> vs, std::span<const uint32_t> fs, VkPipelineLayout layout,
                              const VkVertexInputBindingDescription* binding,
                              std::span<const VkVertexInputAttributeDescription> attrs, PipelineKind kind,
                              VkSampleCountFlagBits samples, VkFormat colorFormat, VkFormat depthFormat,
                              std::span<const uint32_t> tesc = {}, std::span<const uint32_t> tese = {}) {
        const bool tessellated = !tesc.empty() && !tese.empty();
        VkShaderModule v = shaderModule(vs), f = shaderModule(fs);
        VkShaderModule tc = tessellated ? shaderModule(tesc) : VK_NULL_HANDLE, te = tessellated ? shaderModule(tese) : VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo stages[4] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tc, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, te, "main", nullptr},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
                                                binding ? 1u : 0u, binding, uint32_t(attrs.size()), attrs.data()};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                  tessellated ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                                  : kind == PipelineKind::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                  VK_FALSE};
        const VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, nullptr, 0, 3};
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
        if (kind == PipelineKind::Pssm) rs.depthBiasEnable = VK_TRUE; // vkCmdSetDepthBias
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        // With a depth pre-pass, opaque colour draws test LEQUAL with their depth writes set per draw (off where pre-passed:
        // each visible pixel shades once; on for the exterior, the planar reflection and the screen readers).
        const bool prepassed = prepassPipelines_ && kind == PipelineKind::Opaque;
        const bool writesDepth = kind == PipelineKind::Opaque || kind == PipelineKind::Shadow || kind == PipelineKind::Pssm ||
                                 kind == PipelineKind::Prepass;
        const bool sky = kind == PipelineKind::SkyMultiply || kind == PipelineKind::SkyAdd;
        ds.depthTestEnable = depthFormat != VK_FORMAT_UNDEFINED && kind != PipelineKind::SplashComposite && !sky;
        ds.depthWriteEnable = writesDepth;
        // As GL: LESS for depth writers; blended submeshes (translucent pass), debug lines and pre-passed draws LEQUAL.
        ds.depthCompareOp = writesDepth && !prepassed ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        // The pre-pass writes the AO normal target (octahedral, RG).
        blend.colorWriteMask = kind == PipelineKind::Prepass ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                                              : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
        auto factors = [&](VkBlendFactor src, VkBlendFactor dst) {
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = src;
            blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = dst;
        };
        if (kind == PipelineKind::Alpha || kind == PipelineKind::Overlay)
            factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        else if (kind == PipelineKind::SplashComposite) factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        else if (kind == PipelineKind::SplashSplat) {
            factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
            blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_MAX;
        } else if (kind == PipelineKind::Additive) factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);
        else if (kind == PipelineKind::Average) factors(VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_COLOR);
        else if (kind == PipelineKind::FxAlpha) factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        else if (kind == PipelineKind::FxAdditive || kind == PipelineKind::FxSubtract) {
            factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);
            if (kind == PipelineKind::FxSubtract) blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        } else if (kind == PipelineKind::FxMultiply) factors(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA);
        else if (kind == PipelineKind::CausticSplat) {
            factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
            blend.colorWriteMask = VK_COLOR_COMPONENT_G_BIT;
        } else if (sky) { // multiply: rgb x tint, alpha cleared; stars: added under the blended exterior's coverage (sky.frag)
            if (kind == PipelineKind::SkyMultiply) factors(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO);
            else {
                factors(VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ONE);
                blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            }
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const bool hasColor = colorFormat != VK_FORMAT_UNDEFINED;
        cb.attachmentCount = hasColor ? 1 : 0;
        cb.pAttachments = &blend;
        for (float& c : cb.blendConstants) c = 0.5f; // Blend::Average = D3DBLEND_BLENDFACTOR 0x80808080
        // Alpha scene pipelines: depth write set per draw (SubMesh::depthWrite, blend modes 2-3; the scene draw loops), as
        // the pre-passed opaque ones (VkRenderer::drawFrame depthWrites).
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                                VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
        if (kind == PipelineKind::Pssm) dyn[3] = VK_DYNAMIC_STATE_DEPTH_BIAS;
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
                                            kind == PipelineKind::Alpha || kind == PipelineKind::Pssm || prepassed ? 4u : 3u, dyn};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = hasColor ? 1 : 0;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = depthFormat;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        ci.stageCount = tessellated ? 4 : 2;
        ci.pTessellationState = tessellated ? &ts : nullptr;
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
        if (tc) vkDestroyShaderModule(device_, tc, nullptr);
        if (te) vkDestroyShaderModule(device_, te, nullptr);
        check(r, "vkCreateGraphicsPipelines");
        return p;
    }

    // ---- AMD FSR (docs/RENDERING.md "FSR") ----
    // Mask pass pipelines: attachment 0 = motion (RG16F), 1 = reactivity (R8), the scene depth read-only. Motion: moving
    // meshes over the camera motion (LEQUAL, pulled toward the camera: the translated programs' positions differ in the
    // last bits). Reactive*: max-blended into attachment 1 (meshes; fx sprites from their own vertex stream; rain / snow
    // and the splash fluid (its blurred splat target) fullscreen). Camera: fullscreen, writes both, no depth.
    enum class FsrPipeline { Motion, Reactive, Camera, FxReactive, SplashReactive, WeatherReactive };
    VkPipeline createFsrPipeline(FsrPipeline kind) {
        const bool fullscreen = kind == FsrPipeline::Camera || kind == FsrPipeline::WeatherReactive || kind == FsrPipeline::SplashReactive;
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
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
                                                2, meshBindings, 4, meshAttrs};
        switch (kind) {
        case FsrPipeline::Camera: vs = kPostVert, fs = kFsrCameraFrag, layout = fsrCameraLayout_, vi = {vi.sType}; break;
        case FsrPipeline::WeatherReactive: vs = kPostVert, fs = kFsrWeatherFrag, layout = postLayout_, vi = {vi.sType}; break;
        case FsrPipeline::FxReactive: vs = kFxVert, fs = kFsrFxFrag, layout = fsrFxLayout_, vi = {vi.sType, nullptr, 0, 1, &fxBinding, 3, fxAttrs}; break;
        case FsrPipeline::SplashReactive: vs = kSplashScreen, fs = kFsrSplashFrag, layout = splashLayout_, vi = {vi.sType}; break;
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
        fsrInput_ = createImage(swapMemory_, surfaceFormat_.format, extent_.width, extent_.height, 1,
                                kRead | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT); // + feedback copy
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
    // Scissor of GPU mesh `mesh`'s portal clip (FrameOptions::unitClips via meshClip_); unclipped: the whole target.
    VkRect2D clipScissor(uint32_t mesh) const {
        const int c = mesh < meshClip_.size() ? meshClip_[mesh] : -1;
        if (c < 0) return {{0, 0}, extent_};
        const PixelRect p = clipPixels(unitClips_[size_t(c)], int(extent_.width), int(extent_.height), true);
        return {{p.x, p.y}, {uint32_t(p.width), uint32_t(p.height)}};
    }
    bool clippedAway(uint32_t mesh) const {
        return mesh < meshClip_.size() && meshClip_[mesh] >= 0 && clipHidden(unitClips_[size_t(meshClip_[mesh])]);
    }

    // Motion vectors + reactivity at the render resolution after the scene (docs/RENDERING.md "FSR"). depth_ goes
    // DEPTH_ATTACHMENT -> DEPTH_READ_ONLY (tested + sampled here) -> SHADER_READ_ONLY with motion_ / reactive_ for FSR.
    void recordFsrMasks(VkCommandBuffer cmd, const FrameOptions& options, bool splashDrawn, bool weather,
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
        // In double: both view-projections carry the world translation (tens of thousands of units); the float product
        // with the inverse was off by up to a pixel in the distance and changed with each jitter phase, so a still
        // camera's static scenery drifted with the jitter in FSR's output.
        const glm::mat4 reproject(glm::dmat4(fsrHistory_ ? prevViewProj_ : currentVP) * glm::inverse(glm::dmat4(jitteredVP)));
        const FsrCameraPush cam{reproject, jitterUv};
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
            if (clippedAway(mi)) return;
            const GpuMesh& m = meshes_[mi];
            const VkRect2D clip = clipScissor(mi);
            vkCmdSetScissor(cmd, 0, 1, &clip);
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
                if (s.blend == scene::Blend::Opaque || s.depthWrite) draw(mi, s, {s.alphaRef / 255.0f, 0, 0, 0});
        }
        // Reactivity: blended surfaces that do not write depth by their alpha (90 %), Enhanced water surfaces 60 %
        // (animated shading with no motion of its own), fx sprites and splash droplets by their contribution (90 %).
        const bool reactive = fsr_.reactiveUsed();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrReactivePipeline_);
        for (uint32_t mi = 0; reactive && mi < meshes_.size(); ++mi) {
            const GpuMesh& m = meshes_[mi];
            for (uint32_t si = 0; si < m.submeshes.size(); ++si) {
                const scene::SubMesh& s = m.submeshes[si];
                if (enhancedWater && waterDraw(mi, si)) draw(mi, s, {0, 0.6f, 0, 0});
                // Depth writers (blend modes 2-3: foliage, seaweed; SubMesh::depthWrite) are in the depth and motion
                // FSR reprojects, so their history is valid; marked reactive, the alpha-tested plants shimmered with
                // every jitter phase (l2_cliffclimb cliffs).
                else if (s.blend != scene::Blend::Opaque && !s.depthWrite) draw(mi, s, {s.alphaRef / 255.0f, 0.9f, 1, 0});
            }
        }
        vkCmdSetScissor(cmd, 0, 1, &scissor);
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
        if (reactive && splashDrawn) { // fullscreen over the blurred splat target (recordSplashFluid, Enhanced water)
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrSplashPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splashLayout_, 0, 1, &splashSets_[frameIndex_][1], 0,
                                    nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
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
        for (const auto& [_, t] : resident_) // parked ones too: an upload adopts them without rewriting
            if (!t.img.cube) writeSet(t.img.set, t.img.view, textureSampler());
        for (const SceneImage& t : transient_)
            if (!t.img.cube) writeSet(t.img.set, t.img.view, textureSampler());
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

        // post, white, font, the three UI images, the under-water composite's 1 + kUwBloomLevels images + the per-frame sun
        // shadow sets (atlas + OuShadow UBO + the RT AO / GI map and geometry).
        constexpr uint32_t kUwSets = 1 + kUwBloomLevels;
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 + kUwSets + 4 * kFramesInFlight},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 6 + kUwSets + kFramesInFlight, 2, ps};
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
        // tru.exe's PSSM atlas: sampled only after a frame's pass cleared and filled it (frame bools.z), so not cleared here.
        pssm_ = createImage(staticMemory_, shadowFormat_, original::kPssmWidth, original::kPssmHeight, 1,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        oneShot([&](VkCommandBuffer cmd) {
            for (VkImage image : {shadow_.image, pssm_.image}) {
                const auto b = imageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                barriers(cmd, {&b, 1});
            }
        });
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, staticPool_, 1, &setLayout_};
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &postSet_)); // written per swapchain (recreateSwapchain)
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &uwHdrSet_)); // ... as these (the under-water composite's images)
        for (VkDescriptorSet& s : uwBloomSets_) VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &s));

        // Enhanced water: one WaterBlock UBO and one water + post-water set (+ 3 splash fluid sets) per frame in flight;
        // images: water set 5 + post-water set 9 + splash sets 3 x 2; storage buffers: splash sets 3 x 1.
        const VkDescriptorPoolSize wps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 20 * kFramesInFlight},
                                             {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5 * kFramesInFlight},
                                             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kFramesInFlight}};
        VkDescriptorPoolCreateInfo wpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 5 * kFramesInFlight, 3, wps};
        VK_CHECK(vkCreateDescriptorPool(device_, &wpci, nullptr, &waterPool_));
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            waterUbo_[i] = createBuffer(staticMemory_, sizeof(WaterBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDescriptorSetAllocateInfo wai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, waterPool_, 1,
                                            &waterSetLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &wai, &waterSets_[i]));
            wai.pSetLayouts = &postWaterSetLayout_;
            VK_CHECK(vkAllocateDescriptorSets(device_, &wai, &postWaterSets_[i]));
            wai.pSetLayouts = &splashSetLayout_;
            for (VkDescriptorSet& s : splashSets_[i]) VK_CHECK(vkAllocateDescriptorSets(device_, &wai, &s));
        }
        createAgWater();
        createOcean();
        createFluid();
        // contract C3: the splash fluid's particles falling back kick the interactive waves (written each frame the fluid
        // runs, consumed by the next frame's field); set before the field writes its sets
        fieldExternal_ = fluidPipeline_ ? fluidKicks_.buffer : VK_NULL_HANDLE;
        createWaterField();
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
        // s12/s13 spot cookies (original::cookieArray): white until upload() stages the scene's.
        const auto cookies = original::cookieArray({}, {});
        cookies_ = createImage(staticMemory_, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(cookies.width), uint32_t(cookies.height), 1, usage,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, false, uint32_t(cookies.layers));
        batch.images.push_back({cookies_.image, {{cookies.rgba.data(), cookies.rgba.size(), uint32_t(cookies.width), uint32_t(cookies.height)}},
                                uint32_t(cookies.layers)});
        flush(batch);
        // Per frame in flight: the camera's frame block and the planar reflection camera's (reflFrame*).
        VkDescriptorPoolSize ups{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kFramesInFlight};
        VkDescriptorPoolCreateInfo upci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2 * kFramesInFlight, 1, &ups};
        VK_CHECK(vkCreateDescriptorPool(device_, &upci, nullptr, &origStaticPool_));
        for (uint32_t i = 0; i < 2 * kFramesInFlight; ++i) {
            Buffer& ubo = i < kFramesInFlight ? origFrameUbo_[i] : reflFrameUbo_[i - kFramesInFlight];
            VkDescriptorSet& set = i < kFramesInFlight ? origFrameSet_[i] : reflFrameSet_[i - kFramesInFlight];
            ubo = createBuffer(staticMemory_, sizeof(sm3::FrameBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDescriptorSetAllocateInfo fai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, origStaticPool_, 1, &origFrameLayout_};
            VK_CHECK(vkAllocateDescriptorSets(device_, &fai, &set));
            const VkDescriptorBufferInfo bi{ubo.buffer, 0, sizeof(sm3::FrameBlock)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
        createCortao(); // after the upload pool: the occluder tables
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
                      bool cube = false, uint32_t layers = 1) {
        if (cube) layers = 6;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = levels;
        ci.arrayLayers = layers;
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
        vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, levels, 0, layers};
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
            for (VkDescriptorSet s : splashSets_[i]) {
                u.dstSet = s;
                vkUpdateDescriptorSets(device_, 1, &u, 0, nullptr);
            }
            if (ocean_.view) { // the FFT ocean: water set binding 4, post set binding 5 (GENERAL: ocean.comp writes it)
                const VkDescriptorImageInfo oc{oceanSampler_, ocean_.view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = waterSets_[i];
                w.dstBinding = 4;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &oc;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
                w.dstSet = postWaterSets_[i];
                w.dstBinding = 5;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
            if (!white_.view) continue;
            const VkDescriptorImageInfo color{postSampler_, opaqueColor_.view ? opaqueColor_.view : white_.view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo depth{fontSampler_, opaqueDepth_.view ? opaqueDepth_.view : white_.view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo rip{postSampler_, ripples_.view, VK_IMAGE_LAYOUT_GENERAL}; // the wave field (GENERAL: its storage image)
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
            const VkDescriptorImageInfo fb{blurSampler_, feedback_.view ? feedback_.view : white_.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w.dstBinding = 4; // the feedback effect's previous frame
            w.pImageInfo = &fb;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            w.dstBinding = 3; // the ripple field (underwater caustics)
            w.pImageInfo = &rip;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            const VkDescriptorImageInfo uw{fontSampler_, uwParticles_.view ? uwParticles_.view : white_.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w.dstBinding = 9; // the under-water particles (recordUwParticles)
            w.pImageInfo = &uw;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            const VkDescriptorImageInfo shafts[2] = { // the under-water sun shafts (recordUnderwaterShafts)
                {fontSampler_, shaftTargets_[0].view ? shaftTargets_[0].view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {fontSampler_, shaftTargets_[1].view ? shaftTargets_[1].view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            };
            for (uint32_t s = 0; s < 2; ++s) {
                w.dstBinding = 6 + s;
                w.pImageInfo = &shafts[s];
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
            const VkDescriptorImageInfo ag{agWaterSampler_, agWater_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w.dstBinding = 10; // the animated water texture (recordAgWater)
            w.pImageInfo = &ag;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            w.dstSet = waterSets_[i];
            w.dstBinding = 5;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            w.dstSet = postWaterSets_[i];
            const VkDescriptorImageInfo splat[3] = { // recordSplashFluid pass inputs
                depth,
                {fontSampler_, splashTargets_[0].view ? splashTargets_[0].view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {fontSampler_, splashTargets_[1].view ? splashTargets_[1].view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            };
            w.dstBinding = 1;
            for (int s = 0; s < 3; ++s) {
                w.dstSet = splashSets_[i][s];
                w.pImageInfo = &splat[s];
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
            if (!fluidPipeline_) continue;
            // The splash fluid: the particle pool (splash.vert, binding 2) and the colour copy (the composite, 3) in every
            // splash set; its compute set (createFluid).
            const VkDescriptorBufferInfo pool{fluidParticles_.buffer, 0, VK_WHOLE_SIZE};
            for (VkDescriptorSet s : splashSets_[i]) {
                VkWriteDescriptorSet sw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                sw[0].dstSet = sw[1].dstSet = s;
                sw[0].dstBinding = 2, sw[1].dstBinding = 3;
                sw[0].descriptorCount = sw[1].descriptorCount = 1;
                sw[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sw[0].pBufferInfo = &pool;
                sw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sw[1].pImageInfo = &color;
                vkUpdateDescriptorSets(device_, 2, sw, 0, nullptr);
            }
            const VkDescriptorBufferInfo fbufs[5] = {{waterUbo_[i].buffer, 0, sizeof(WaterBlock)}, pool,
                                                  {fluidGrid_.buffer, 0, VK_WHOLE_SIZE}, {fluidKicks_.buffer, 0, VK_WHOLE_SIZE},
                                                  {fluidBursts_[i].buffer, 0, VK_WHOLE_SIZE}};
            // binding 7: the FFT ocean (shaders/water_surface.glsl; createOcean runs before createFluid)
            const VkDescriptorImageInfo oc{oceanSampler_, ocean_.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo* fi[3] = {&depth, &rip, &oc};
            const uint32_t fn = ocean_.view ? 8 : 7;
            VkWriteDescriptorSet fw[8];
            for (uint32_t b = 0; b < fn; ++b) {
                fw[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                fw[b].dstSet = fluidSets_[i];
                fw[b].dstBinding = b;
                fw[b].descriptorCount = 1;
                fw[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                       : b < 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                if (b < 5) fw[b].pBufferInfo = &fbufs[b];
                else fw[b].pImageInfo = fi[b - 5];
            }
            vkUpdateDescriptorSets(device_, fn, fw, 0, nullptr);
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
        if (total == 0) { // nothing to copy; `after` still runs (createWaterField's clear of ripples_ to GENERAL)
            if (after) oneShot(after);
            return;
        }
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

    // Original screen filters: `src` (render size, COLOR_ATTACHMENT) blitted to half size into `dst` mip 0, each further
    // mip from the one above (linear): the resolved scene into filterBlur_ (the blur levels the post pass samples,
    // shaders/filter.glsl) and the post output into feedback_. Leaves `src` TRANSFER_SRC and `dst` SHADER_READ_ONLY.
    void recordFilterBlur(VkCommandBuffer cmd, VkImage src, VkImage dst) {
        constexpr VkPipelineStageFlags2 kBlit = VK_PIPELINE_STAGE_2_BLIT_BIT;
        const VkImageMemoryBarrier2 pre[] = {
            imageBarrier(src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kBlit,
                         VK_ACCESS_2_TRANSFER_READ_BIT),
            imageBarrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
                auto up = imageBarrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       kBlit, VK_ACCESS_2_TRANSFER_WRITE_BIT, kBlit, VK_ACCESS_2_TRANSFER_READ_BIT);
                up.subresourceRange.baseMipLevel = level - 1, up.subresourceRange.levelCount = 1;
                barriers(cmd, {&up, 1});
            }
            vkCmdBlitImage(cmd, level ? dst : src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &b, VK_FILTER_LINEAR);
        }
        VkImageMemoryBarrier2 post[2] = {
            imageBarrier(dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kBlit,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            imageBarrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kBlit,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        };
        post[0].subresourceRange.levelCount = filterLevels_ - 1;
        post[1].subresourceRange.baseMipLevel = filterLevels_ - 1, post[1].subresourceRange.levelCount = 1;
        barriers(cmd, {post + (filterLevels_ > 1 ? 0 : 1), size_t(filterLevels_ > 1 ? 2 : 1)});
    }

    // Between the scene pass and the water pass: copy the resolved opaque colour (and depth, when
    // there is no MSAA resolve) into the single-sample textures the water and post shaders sample.
    // withDepth false: colour only (after the water, for refracting particles; the depth copy stays as it was).
    void recordWaterCopies(VkCommandBuffer cmd, bool withDepth = true) {
        const bool copyDepth = withDepth && !msColor_.image;
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
        if (copyDepth) {
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
        if (copyDepth) {
            region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.dstSubresource = region.srcSubresource;
            vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opaqueDepth_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        std::vector<VkImageMemoryBarrier2> post = {
            imageBarrier(opaqueColor_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            imageBarrier(sceneColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, // pass 2 loads it without MSAA (FSR)
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        if (withDepth)
            post.push_back(imageBarrier(opaqueDepth_.image,
                                        msColor_.image ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        msColor_.image ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_COPY_BIT,
                                        msColor_.image ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT, // resolve
                                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                        VK_IMAGE_ASPECT_DEPTH_BIT));
        if (copyDepth)
            post.push_back(imageBarrier(depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_READ_BIT, kDepthStages,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                        VK_IMAGE_ASPECT_DEPTH_BIT));
        barriers(cmd, post);
    }

    // The splash fluid's passes 1-2 (render/Water.hpp splash passes), between the scene's second-pass scopes: splat the
    // particle pool (recordFluid) into splashTargets_[0], blur it horizontally into [1] and vertically back into [0],
    // left SHADER_READ_ONLY for the composite. `vp` is the scene's flipped viewport (splash.vert's GL-convention clip space).
    void recordSplashFluid(VkCommandBuffer cmd, const VkViewport& vp, const VkRect2D& scissor) {
        constexpr VkPipelineStageFlags2 kOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        kFrag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        auto toRead = [&](const GpuImage& t) {
            return imageBarrier(t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kOut,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kFrag, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        };
        for (uint32_t pass = 0; pass < 3; ++pass) { // splat, blur (1, 0), blur (0, 1)
            const GpuImage& out = splashTargets_[pass & 1];
            // The output's old contents are discarded (its last reads: this frame's horizontal blur, the last composite).
            const VkImageMemoryBarrier2 b[2] = {
                imageBarrier(out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kFrag, 0, kOut,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                toRead(splashTargets_[(pass + 1) & 1]), // the previous pass's output is this one's input
            };
            barriers(cmd, {b, pass ? 2u : 1u});
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            a.imageView = out.view;
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = pass ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR; // the blurs write every pixel
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.clearValue.color = {{kSplashEmpty, 0.0f, 0.0f, 0.0f}};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = scissor;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &a;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass ? splashBlurPipeline_ : splashSplatPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splashLayout_, 0, 1, &splashSets_[frameIndex_][pass], 0,
                                    nullptr);
            if (pass == 0) {
                vkCmdDraw(cmd, 6, uint32_t(fluid::kCapacity), 0, 0);
            } else {
                const glm::vec2 dir = pass == 1 ? glm::vec2(1, 0) : glm::vec2(0, 1);
                vkCmdPushConstants(cmd, splashLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof dir, &dir);
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
            vkCmdEndRendering(cmd);
        }
        const VkImageMemoryBarrier2 last = toRead(splashTargets_[0]);
        barriers(cmd, {&last, 1});
    }

    // Enhanced under-water particles (render/Water.hpp UwParticlePush), after the scene's second pass: marine snow, then
    // this frame's bubbles, premultiplied into uwParticles_ (cleared to 0; no depth attachment: the shaders test the
    // opaque depth copy, set 1 binding 1). Left SHADER_READ_ONLY for the post pass. `vp`: the scene's flipped viewport.
    void recordUwParticles(VkCommandBuffer cmd, const Camera& camera, const WaterBlock& wb, const FrameOptions& options,
                           const VkViewport& vp, const VkRect2D& scissor) {
        constexpr VkPipelineStageFlags2 kOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        kFrag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        const VkDeviceSize bytes = options.bubbles.size_bytes();
        if (bytes > bubbleCapacity_[frameIndex_]) { // this frame's fence was waited: its old buffer is idle
            vkDestroyBuffer(device_, bubbleBuffers_[frameIndex_].buffer, nullptr);
            bubbleMemory_[frameIndex_].reset();
            bubbleCapacity_[frameIndex_] = std::bit_ceil(std::max<VkDeviceSize>(bytes, 16 << 10));
            bubbleBuffers_[frameIndex_] = createBuffer(bubbleMemory_[frameIndex_], bubbleCapacity_[frameIndex_],
                                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if (bytes) std::memcpy(bubbleBuffers_[frameIndex_].mapped, options.bubbles.data(), bytes);
        // Old contents discarded (last read: the previous frame's post pass).
        const VkImageMemoryBarrier2 toWrite = imageBarrier(uwParticles_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kFrag, 0, kOut,
                                                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        barriers(cmd, {&toWrite, 1});
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = uwParticles_.view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &a;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        const UwParticlePush push = uwParticlePush(camera, wb);
        const VkDescriptorSet sets[2] = {postWaterSets_[frameIndex_], shadowSets_[frameIndex_]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uwLayout_, 1, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, uwLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uwSnowPipeline_);
        vkCmdDraw(cmd, 6, uint32_t(push.params.x), 0, 0);
        if (bytes) {
            const VkDeviceSize zero = 0;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uwBubblePipeline_);
            vkCmdBindVertexBuffers(cmd, 0, 1, &bubbleBuffers_[frameIndex_].buffer, &zero);
            vkCmdDraw(cmd, 6, uint32_t(options.bubbles.size()), 0, 0);
        }
        vkCmdEndRendering(cmd);
        const VkImageMemoryBarrier2 toRead = imageBarrier(uwParticles_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kOut,
                                                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kFrag,
                                                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barriers(cmd, {&toRead, 1});
    }

    // Enhanced under-water sun shafts (render/Water.hpp, shaders/underwater.glsl agRays / agRaysBlur), before the post
    // pass; the grade upsamples shaftTargets_[1]: the half-resolution march into [0], its blur into [1]. Each output's old
    // contents are discarded (last read: this frame's blur or the last grade); both are left SHADER_READ_ONLY. Post
    // layout: set 1 = the post-water set, set 2 = the sun shadow set.
    void recordUnderwaterShafts(VkCommandBuffer cmd) {
        constexpr VkPipelineStageFlags2 kOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        kFrag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        const VkDescriptorSet sets[2] = {postWaterSets_[frameIndex_], shadowSets_[frameIndex_]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 1, 2, sets, 0, nullptr);
        const std::pair<const GpuImage*, VkPipeline> passes[2] = {{&shaftTargets_[0], shaftMarchPipeline_},
                                                                  {&shaftTargets_[1], shaftBlurPipeline_}};
        for (const auto& [out, pipeline] : passes) {
            const uint32_t w = (extent_.width + 1) / 2, h = (extent_.height + 1) / 2;
            const VkImageMemoryBarrier2 toWrite = imageBarrier(out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                               kFrag, 0, kOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barriers(cmd, {&toWrite, 1});
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            a.imageView = out->view;
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the fullscreen triangle writes every pixel
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {w, h}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &a;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport vp{0, 0, float(w), float(h), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            const VkImageMemoryBarrier2 toRead = imageBarrier(out->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kOut,
                                                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kFrag,
                                                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            barriers(cmd, {&toRead, 1});
        }
    }

    // Enhanced water's animated water texture (render/Water.hpp kAgWaterSize; abyssal-garden W_WATER): kAgWaterLevels
    // mips of RGBA16F, repeat / trilinear / anisotropic 8, kept SHADER_READ_ONLY between frames; agWaterLevel0_ = the
    // render target view of mip 0.
    void createAgWater() {
        agWater_ = createImage(staticMemory_, VK_FORMAT_R16G16B16A16_SFLOAT, kAgWaterSize, kAgWaterSize, kAgWaterLevels,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = agWater_.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &agWaterLevel0_));
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.anisotropyEnable = maxAniso_ > 1.0f;
        sci.maxAnisotropy = std::clamp(maxAniso_, 1.0f, 8.0f); // the demo's sR: maxAnisotropy 8
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &agWaterSampler_));
        oneShot([&](VkCommandBuffer cmd) { // a valid sampled layout before the first frame writes it
            const auto b = imageBarrier(agWater_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            barriers(cmd, {&b, 1});
        });
    }
    // This frame's water texture, before any render pass: uw_water.frag into mip 0 (the earlier frames' reads by the water
    // and post fragment shaders done), then each mip blitted (linear: the 2x2 box of the demo's fsMip) from the one above.
    void recordAgWater(VkCommandBuffer cmd, float time) {
        constexpr VkPipelineStageFlags2 kOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        kFrag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kXfer = VK_PIPELINE_STAGE_2_BLIT_BIT;
        const auto mips = [](VkImageMemoryBarrier2 b, uint32_t base, uint32_t count) {
            b.subresourceRange.baseMipLevel = base, b.subresourceRange.levelCount = count;
            return b;
        };
        const VkImageMemoryBarrier2 toWrite[2] = {
            mips(imageBarrier(agWater_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kFrag, 0, kOut,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                 0, 1),
            mips(imageBarrier(agWater_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kFrag, 0, kXfer,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT),
                 1, kAgWaterLevels - 1)};
        barriers(cmd, toWrite);
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = agWaterLevel0_;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the fullscreen triangle writes every texel
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {uint32_t(kAgWaterSize), uint32_t(kAgWaterSize)}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &a;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0, 0, float(kAgWaterSize), float(kAgWaterSize), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, agWaterPipeline_);
        vkCmdPushConstants(cmd, agWaterLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof time, &time);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, agCausticPipeline_); // same layout: the push stays
        vkCmdDraw(cmd, uint32_t(kAgCausVertices), 1, 0, 0);
        vkCmdEndRendering(cmd);
        auto src = mips(imageBarrier(agWater_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     kOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kXfer, VK_ACCESS_2_TRANSFER_READ_BIT),
                        0, 1);
        barriers(cmd, {&src, 1});
        for (int l = 1; l < kAgWaterLevels; ++l) {
            const int32_t s = kAgWaterSize >> (l - 1), d = kAgWaterSize >> l;
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l - 1), 0, 1};
            b.srcOffsets[1] = {s, s, 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l), 0, 1};
            b.dstOffsets[1] = {d, d, 1};
            vkCmdBlitImage(cmd, agWater_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, agWater_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            src = mips(imageBarrier(agWater_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kXfer,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, kXfer, VK_ACCESS_2_TRANSFER_READ_BIT),
                       uint32_t(l), 1);
            barriers(cmd, {&src, 1});
        }
        const auto toRead = imageBarrier(agWater_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         kXfer, 0, kFrag, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barriers(cmd, {&toRead, 1});
    }

    // Enhanced under-water composite (render/Water.hpp UwGrade), before the post target's rendering: `resolve` draws the
    // post pass into uwHdr_ (the linear frame), then the bloom chain of abyssal-garden main.js: down0 uwHdr_ -> level 0,
    // down level i - 1 -> i, then up level i -> i - 1 added (loaded) back to level 0, which uw_grade.frag samples. Each
    // written image's old contents are discarded (except the up passes' targets); all are left SHADER_READ_ONLY.
    void recordUwComposite(VkCommandBuffer cmd, const std::function<void()>& resolve) {
        constexpr VkPipelineStageFlags2 kOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        kFrag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        auto pass = [&](const GpuImage& out, VkExtent2D size, bool load, const std::function<void()>& draw) {
            const VkImageMemoryBarrier2 toWrite =
                imageBarrier(out.image, load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kFrag, 0, kOut,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barriers(cmd, {&toWrite, 1});
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            a.imageView = out.view;
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; // fullscreen triangle
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, size};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &a;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport vp{0, 0, float(size.width), float(size.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            draw();
            vkCmdEndRendering(cmd);
            const VkImageMemoryBarrier2 toRead = imageBarrier(out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kOut,
                                                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kFrag,
                                                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            barriers(cmd, {&toRead, 1});
        };
        auto level = [&](int i) { return VkExtent2D{std::max(1u, extent_.width >> (i + 1)), std::max(1u, extent_.height >> (i + 1))}; };
        auto blit = [&](VkPipeline pipeline, float mode, VkDescriptorSet input) {
            const glm::vec4 v(mode, 0.0f, 0.0f, 0.0f);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(cmd, uwGradeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof v, &v);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uwGradeLayout_, 0, 1, &input, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        pass(uwHdr_, extent_, false, resolve);
        for (int i = 0; i < kUwBloomLevels; ++i)
            pass(uwBloom_[size_t(i)], level(i), false,
                 [&] { blit(uwBloomPipeline_, i ? 1.0f : 0.0f, i ? uwBloomSets_[size_t(i - 1)] : uwHdrSet_); });
        for (int i = kUwBloomLevels - 1; i > 0; --i)
            pass(uwBloom_[size_t(i - 1)], level(i - 1), true, [&] { blit(uwBloomUpPipeline_, 2.0f, uwBloomSets_[size_t(i)]); });
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
    VkAccelerationStructureBuildGeometryInfoKHR blasInfo(const RtBlas& b, VkBuildAccelerationStructureModeKHR mode) const {
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = b.skinned ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                             : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = mode;
        bi.srcAccelerationStructure = mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? b.as : VK_NULL_HANDLE;
        bi.dstAccelerationStructure = b.as;
        bi.geometryCount = uint32_t(b.geometries.size());
        bi.pGeometries = b.geometries.data();
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

    // Acceleration structure + its backing buffer (from `memory`) of `size` bytes.
    VkAccelerationStructureKHR createAs(VkAccelerationStructureTypeKHR type, VkDeviceSize size, Buffer& backing, LinearAllocator& memory) {
        backing = createBuffer(memory, size,
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

    // After upload(): per mesh with traced submeshes (Opaque + Alpha blends; Additive / Average neither cast shadows nor
    // reflect, as in the shadow map) one BLAS and TLAS instance per ray mask (sun.glsl kOuMask*: 1 reflection rays =
    // solid and not shadow-only, 2 water, 4 shadow rays = casters), the geometry record buffer and the set-2 descriptors.
    // The masks keep shadow rays out of non-casters instead of rejecting every candidate in the shader (l4_trailstart:
    // 1786 of its 2996 traced submeshes are non-casters; rejecting them cost 15 of the scene pass's 17 ms at 4K/FSR).
    // Meshes never moved by updateVertices (GpuMesh::rtStatic) are traced through merged BLASes instead (the mesh model
    // as the geometry transform): thousands of overlapping per-mesh instances made every ray descend into dozens of
    // BLASes (l2_junglehub scene pass 12.6 -> 7.9 ms). One merged BLAS per ray mask and renderer part (RtGroup; keyed
    // meshes outside parts and unkeyed meshes form one group each). BLASes live with their mesh / group: an upload builds
    // those of new meshes and changed groups only, then the TLAS. A static mesh's own BLAS is created but first built
    // when updateVertices moves it (rebuildBlases, then evictStatic moves it out of its group). Models are fixed between
    // uploads (GpuMesh::model; a moved resident mesh changes its group's transforms: rebuilt).
    void buildRayTracing(VkCommandBuffer cmd) {
        std::vector<RtGeometry> records;
        std::vector<RtBlas*> pending; // built below
        VkDeviceSize maxScratch = 0, sumScratch = 0;
        auto maskOf = [](const GpuMesh& m, const scene::SubMesh& s) -> uint32_t {
            // Exterior terrain is a backdrop placed somewhere in the world (a 3072-unit cylinder round l2_junglehub's
            // start): rays would hit it in front of the scene it stands behind.
            if ((s.blend != scene::Blend::Opaque && s.blend != scene::Blend::Alpha) || s.indexCount < 3 || s.exterior) return 0;
            // Water = `*watersurface*` meshes and the planar-reflection (sea) materials: reflection rays must not hit the
            // sea they start on (l2_cliffclimb's sea is not m.water; mask 1 made every ray hit it within ~2000 units).
            const size_t i = size_t(&s - m.submeshes.data());
            const bool water = m.water || (i < m.reflective.size() && m.reflective[i]);
            return (s.shadowOnly ? 0u : water ? 2u : 1u) | (s.castsShadow ? 4u : 0u);
        };
        // Geometries of `mask` (GpuMesh::rtMasks) into `b`.
        auto geometries = [&](const GpuMesh& m, uint32_t mask, RtBlas& b, VkDeviceAddress transform) {
            const VkDeviceAddress va = address(m.vertices.buffer), ia = address(m.indices.buffer);
            for (size_t i = 0; i < m.submeshes.size(); ++i) {
                if (m.rtMasks[i] != mask) continue;
                const scene::SubMesh& s = m.submeshes[i];
                const uint8_t ref = s.blend == scene::Blend::Alpha ? std::max<uint8_t>(s.alphaRef, 128) : s.alphaRef;
                VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                g.flags = ref == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0; // non-opaque: alpha test in sun.glsl
                auto& t = g.geometry.triangles;
                t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                t.vertexData.deviceAddress = va;
                t.vertexStride = sizeof(scene::Vertex);
                t.maxVertex = uint32_t(m.vertexCount - 1);
                t.indexType = VK_INDEX_TYPE_UINT32;
                t.indexData.deviceAddress = ia;
                t.transformData.deviceAddress = transform;
                b.geometries.push_back(g);
                b.ranges.push_back({s.indexCount / 3, s.firstIndex * uint32_t(sizeof(uint32_t)), 0, 0});
                b.records.push_back({va, ia, s.firstIndex, 0, ref / 255.0f, 0, transform});
                b.textures.push_back(s.textureId);
            }
        };
        auto create = [&](RtBlas& b, bool skinned, uint32_t mask, LinearAllocator& memory) {
            b.mask = mask, b.skinned = skinned;
            std::vector<uint32_t> counts;
            for (const auto& r : b.ranges) counts.push_back(r.primitiveCount);
            const auto bi = blasInfo(b, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, counts.data(), &sz);
            b.as = createAs(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize, b.buffer, memory);
            b.scratch = alignUp(std::max(sz.buildScratchSize, sz.updateScratchSize));
            VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, nullptr, b.as};
            b.instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR_(device_, &ai);
            b.instance.mask = mask;
            b.instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR; // both faces, as the shadow pass
        };
        // This upload's instance of `b` (custom index = its first geometry record) and records (texture slots resolved).
        auto emit = [&](RtBlas& b, const glm::mat4& model) {
            if (records.size() >= (1u << 24)) throw std::runtime_error("RT: more than 2^24 geometries");
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) b.instance.transform.matrix[r][c] = model[c][r]; // glm is column-major
            b.instance.instanceCustomIndex = uint32_t(records.size());
            for (size_t i = 0; i < b.records.size(); ++i) {
                RtGeometry& r = records.emplace_back(b.records[i]);
                const auto tex = textures_.find(b.textures[i]);
                r.texture = tex != textures_.end() ? tex->second.slot : 0;
            }
            maxScratch = std::max(maxScratch, b.scratch); // rebuildBlases / evictStatic rebuild any of them later
        };
        constexpr uint32_t kMasks[] = {1u, 2u, 4u, 5u, 6u};
        size_t meshInstances = 0;
        for (GpuMesh& m : meshes_) {
            std::vector<uint8_t> masks;
            for (const scene::SubMesh& s : m.submeshes) masks.push_back(uint8_t(maskOf(m, s)));
            if (masks != m.rtMasks) { // new mesh (or its materials changed)
                buryRt(m);
                m.rtSerial = ++rtSerials_; // groups holding its old geometries no longer match
                m.rtMasks = std::move(masks);
                for (const uint32_t mask : kMasks) {
                    RtBlas b;
                    geometries(m, mask, b, 0);
                    if (b.geometries.empty()) continue;
                    create(b, m.skinned, mask, m.gen->memory);
                    m.blases.push_back(std::move(b));
                }
            }
            m.rtMerged.clear();
            m.rtStatic = !m.skinned && !m.movedFrame && !m.blases.empty() && !movers_.contains(m.key);
            for (RtBlas& b : m.blases) {
                emit(b, m.model);
                if (!b.built && !m.rtStatic) pending.push_back(&b); // after the pushes: stable addresses
            }
            meshInstances += m.blases.size();
            rtStatus_.waterInstances += m.water && !m.blases.empty();
        }
        // The merged BLASes: the static meshes per group, by serial (independent of the scene's mesh order). Models with
        // several batch LODs (meshes at one LOD point with different ranges) get one group per LOD, masked per frame
        // (updateRtLods); a lone LOD (a far cut-off: l4_ballcourt's thousands of plants) stays merged, still traced past
        // its range [I], as one instance per plant cost more scene time than the hidden geometry saved.
        using LodPoint = std::tuple<uint64_t, float, float, float>;
        std::map<LodPoint, std::set<std::pair<float, float>>> lodRanges;
        auto lodPoint = [&](const GpuMesh& m) -> LodPoint {
            const glm::vec3 p = m.center + m.lodOffset;
            return {m.key ? m.part : kUnkeyedGroup, p.x, p.y, p.z};
        };
        for (const GpuMesh& m : meshes_)
            if (m.rtStatic && hasLod(m)) lodRanges[lodPoint(m)].insert({m.lodNear, m.lodFar});
        using GroupKey = std::tuple<uint64_t, float, float, float, float, float>;
        std::map<GroupKey, std::vector<GpuMesh*>> statics;
        for (GpuMesh& m : meshes_)
            if (m.rtStatic) {
                const uint64_t id = m.key ? m.part : kUnkeyedGroup;
                if (!hasLod(m) || lodRanges[lodPoint(m)].size() < 2) statics[{id, 0.0f, std::numeric_limits<float>::max(), 0.0f, 0.0f, 0.0f}].push_back(&m);
                else {
                    const auto [_, x, y, z] = lodPoint(m);
                    statics[{id, m.lodNear, m.lodFar, x, y, z}].push_back(&m);
                }
            }
        const VkBufferUsageFlags copyDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        const VkBufferUsageFlags asInput = copyDst | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        UploadBatch batch;
        std::vector<RtGroup> old = std::move(rtGroups_);
        rtGroups_.clear();
        rtGroups_.reserve(statics.size() * std::size(kMasks)); // stable addresses for `pending`
        for (auto& [gk, ms] : statics) {
            const uint64_t id = std::get<0>(gk);
            const auto [_, lodNear, lodFar, px, py, pz] = gk;
            const glm::vec3 point{px, py, pz};
            std::sort(ms.begin(), ms.end(), [](const GpuMesh* a, const GpuMesh* b) { return a->rtSerial < b->rtSerial; });
            std::vector<uint64_t> members;
            std::vector<VkTransformMatrixKHR> transforms;
            for (const GpuMesh* m : ms) {
                members.push_back(m->rtSerial);
                VkTransformMatrixKHR& x = transforms.emplace_back();
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) x.matrix[r][c] = m->model[c][r];
            }
            for (const uint32_t mask : kMasks) {
                if (std::none_of(ms.begin(), ms.end(), [&](const GpuMesh* m) { return std::ranges::find(m->rtMasks, mask) != m->rtMasks.end(); }))
                    continue;
                const auto same = std::find_if(old.begin(), old.end(), [&](const RtGroup& g) {
                    return !g.stale && g.id == id && g.lodNear == lodNear && g.lodFar == lodFar && g.center == point &&
                           g.blas.mask == mask && g.members == members &&
                           std::memcmp(g.transforms.data(), transforms.data(), transforms.size() * sizeof(transforms[0])) == 0;
                });
                if (same != old.end()) {
                    rtGroups_.push_back(std::move(*same));
                    old.erase(same);
                } else {
                    RtGroup& g = rtGroups_.emplace_back();
                    g.id = id, g.members = members, g.transforms = transforms;
                    g.lodNear = lodNear, g.lodFar = lodFar, g.center = point;
                    g.gen = std::make_shared<Generation>();
                    g.gen->device = device_;
                    g.gen->memory.init(device_, gpu_, 0, true);
                    const VkDeviceSize bytes = transforms.size() * sizeof(VkTransformMatrixKHR);
                    g.transformBuffer = createBuffer(g.gen->memory, bytes, asInput | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    batch.buffers.push_back({g.transformBuffer.buffer, {g.transforms.data(), bytes, 0, 0}});
                    VkDeviceAddress transform = address(g.transformBuffer.buffer);
                    for (const GpuMesh* m : ms) {
                        geometries(*m, mask, g.blas, transform);
                        transform += sizeof(VkTransformMatrixKHR);
                    }
                    create(g.blas, false, mask, g.gen->memory);
                    pending.push_back(&g.blas);
                }
                RtGroup& g = rtGroups_.back();
                uint32_t geo = 0;
                for (GpuMesh* m : ms)
                    for (const uint8_t k : m->rtMasks)
                        if (k == mask) m->rtMerged.push_back({uint32_t(rtGroups_.size() - 1), geo++});
                emit(g.blas, glm::mat4(1.0f));
            }
        }
        for (RtGroup& g : old) buryGroup(g);
        for (const RtBlas* b : pending) sumScratch += b->scratch;
        rtStatus_.blas = uint32_t(meshInstances + rtGroups_.size());
        rtStatus_.blasBuilds += uint32_t(pending.size());
        rtStatus_.geometries = uint32_t(records.size());
        const std::vector<VkAccelerationStructureInstanceKHR> instances = tlasInstances();
        rtStatus_.instances = tlasInstanceCount_ = uint32_t(instances.size());

        // Instance + record buffers (a 0-instance TLAS is valid and keeps set 2 complete). Sized and the TLAS built for
        // every instance there can be (all meshes evicted): evictStatic rewrites the list and rebuilds the TLAS in place.
        tlasInstanceCapacity_ = uint32_t(meshInstances + rtGroups_.size());
        instanceBuffer_ = createBuffer(gen_->memory, std::max<size_t>(1, tlasInstanceCapacity_) * sizeof(VkAccelerationStructureInstanceKHR),
                                       asInput, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        geometryBuffer_ = createBuffer(gen_->memory, std::max<size_t>(1, records.size()) * sizeof(RtGeometry),
                                       copyDst | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!instances.empty()) batch.buffers.push_back({instanceBuffer_.buffer, {instances.data(), instances.size() * sizeof(instances[0]), 0, 0}});
        if (!records.empty()) batch.buffers.push_back({geometryBuffer_.buffer, {records.data(), records.size() * sizeof(RtGeometry), 0, 0}});
        stage(batch);
        recordPending(cmd);

        tlasGeometry_ = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlasGeometry_.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry_.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeometry_.geometry.instances.data.deviceAddress = address(instanceBuffer_.buffer);
        auto ti = tlasInfo(VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
        VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &tlasInstanceCapacity_, &tsz);
        tlas_ = createAs(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tsz.accelerationStructureSize, tlasBuffer_, gen_->memory);
        ti.dstAccelerationStructure = tlas_;

        // Scratch: a shared BLAS region (upload batches, later refits/rebuilds one at a time) + the TLAS's own.
        const VkDeviceSize blasScratch = std::max(std::min(sumScratch, kRtScratchBudget), maxScratch);
        const VkDeviceSize tlasScratch = alignUp(std::max(tsz.buildScratchSize, tsz.updateScratchSize));
        scratch_ = createBuffer(gen_->memory, blasScratch + tlasScratch + scratchAlign_,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        scratchBlas_ = alignUp(address(scratch_.buffer));
        scratchTlas_ = scratchBlas_ + blasScratch;

        {
            // Vertex/index/instance copies (recordPending) → build inputs and fragment-shader reads; earlier frames may
            // still trace kept BLASes (not rebuilt here) and read the scratch of their last refits.
            asBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | kAsBuild, VK_ACCESS_2_TRANSFER_WRITE_BIT | kAsWrite,
                      kAsBuild | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | kAsRead | kAsWrite);
            std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
            VkDeviceSize used = 0;
            auto submit = [&] {
                if (infos.empty()) return;
                vkCmdBuildAccelerationStructuresKHR_(cmd, uint32_t(infos.size()), infos.data(), ranges.data());
                asBarrier(cmd, kAsBuild, kAsWrite, kAsBuild, kAsRead | kAsWrite); // scratch reuse + TLAS input
                infos.clear(), ranges.clear(), used = 0;
            };
            for (RtBlas* b : pending) {
                if (used + b->scratch > blasScratch) submit();
                auto bi = blasInfo(*b, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
                bi.scratchData.deviceAddress = scratchBlas_ + used;
                used += b->scratch;
                infos.push_back(bi);
                ranges.push_back(b->ranges.data());
                b->built = true;
            }
            submit();
            ti.scratchData.deviceAddress = scratchTlas_;
            const VkAccelerationStructureBuildRangeInfoKHR tr{tlasInstanceCount_, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
            vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &ti, &trp);
            asBarrier(cmd, kAsBuild, kAsWrite, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
        }
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
        if (!tlas_) { // no structures for this upload yet: buildRayTracing builds these with the new vertices
            for (const size_t slot : slots)
                for (RtBlas& b : meshes_[slot].blases) b.built = false;
            return;
        }
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
            for (RtBlas& b : meshes_[slot].blases) {
                if (used + b.scratch > capacity) submit();
                const bool refit = b.skinned && b.built; // a static mesh's own BLAS: first built now
                auto bi = blasInfo(b, refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
                bi.scratchData.deviceAddress = scratchBlas_ + used;
                used += b.scratch;
                infos.push_back(bi);
                ranges.push_back(b.ranges.data());
                ++(refit ? rtStatus_.blasRefits : rtStatus_.blasRebuilds);
                b.built = true;
                tlasDirty_ = true;
            }
        }
        submit();
        if (tlasDirty_) asBarrier(cmd, kAsBuild, kAsWrite, kAsBuild | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
    }

    // TLAS instances: the merged static BLASes, then every BLAS of the meshes that move; mask 0 while their batch LOD is
    // out of range (updateRtLods).
    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances() const {
        std::vector<VkAccelerationStructureInstanceKHR> out;
        for (const RtGroup& g : rtGroups_) {
            out.push_back(g.blas.instance);
            if (g.lodHidden) out.back().mask = 0;
        }
        for (const GpuMesh& m : meshes_)
            if (!m.rtStatic)
                for (const RtBlas& b : m.blases) {
                    out.push_back(b.instance);
                    if (m.rtLodHidden) out.back().mask = 0;
                }
        return out;
    }

    // The instance list into instanceBuffer_ (earlier frames' TLAS builds read the old one), then a TLAS rebuild.
    void writeInstances(VkCommandBuffer cmd) {
        const auto instances = tlasInstances();
        tlasInstanceCount_ = uint32_t(instances.size());
        constexpr VkDeviceSize kChunk = 65536; // vkCmdUpdateBuffer limit
        const VkDeviceSize bytes = instances.size() * sizeof(instances[0]);
        asBarrier(cmd, kAsBuild, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (VkDeviceSize o = 0; o < bytes; o += kChunk)
            vkCmdUpdateBuffer(cmd, instanceBuffer_.buffer, o, std::min(kChunk, bytes - o), reinterpret_cast<const char*>(instances.data()) + o);
        asBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kAsBuild, VK_ACCESS_2_SHADER_READ_BIT | kAsRead);
        rtStatus_.instances = tlasInstanceCount_;
        tlasRebuild_ = tlasDirty_ = true;
    }

    static bool hasLod(const GpuMesh& m) { return m.lodNear > 0 || m.lodFar < std::numeric_limits<float>::max(); }

    // Per frame after lodEye_: batch LODs (as the raster passes, 0x50F7C0) mask their merged groups' and moving meshes'
    // TLAS instances. Only a changed LOD rewrites the instance list (TLAS rebuild, no BLAS work).
    void updateRtLods(VkCommandBuffer cmd) {
        bool changed = false;
        uint32_t hidden = 0;
        auto set = [&](bool& flag, bool h, uint32_t n) {
            changed |= h != flag;
            flag = h;
            hidden += h ? n : 0;
        };
        for (RtGroup& g : rtGroups_) set(g.lodHidden, !lodShown(g, lodEye_), 1);
        for (GpuMesh& m : meshes_)
            if (!m.rtStatic) set(m.rtLodHidden, !lodShown(m, lodEye_), uint32_t(m.blases.size()));
        rtStatus_.lodHidden = hidden;
        if (changed) writeInstances(cmd);
    }

    // After rebuildBlases: meshes updateVertices moved for the first time leave the merged BLASes (their geometries
    // rebuilt there with 0 triangles, so record indices stay) and trace through their own BLASes, just rebuilt with the
    // new vertices; the TLAS is rebuilt with the new instance list. Once per mesh (bend plants on their first tick, a
    // door when it first opens).
    void evictStatic(VkCommandBuffer cmd) {
        if (rtEvicted_.empty() || !tlas_) return;
        std::vector<uint8_t> dirty(rtGroups_.size(), 0);
        for (const size_t slot : rtEvicted_)
            for (const auto& [group, geo] : meshes_[slot].rtMerged) rtGroups_[group].blas.ranges[geo].primitiveCount = 0, dirty[group] = 1;
        rtEvicted_.clear();
        for (size_t i = 0; i < rtGroups_.size(); ++i) {
            if (!dirty[i]) continue;
            rtGroups_[i].stale = true;
            // Earlier frames may still trace it; the scratch may still be in use by rebuildBlases.
            asBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kAsBuild, kAsWrite, kAsBuild, kAsRead | kAsWrite);
            auto bi = blasInfo(rtGroups_[i].blas, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR);
            bi.scratchData.deviceAddress = scratchBlas_;
            const VkAccelerationStructureBuildRangeInfoKHR* r = rtGroups_[i].blas.ranges.data();
            vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &bi, &r);
            ++rtStatus_.blasRebuilds;
        }
        writeInstances(cmd);
    }

    // Per frame when a BLAS changed: TLAS update in place (same instances; BLAS bounds moved), or a rebuild after
    // evictStatic changed the instance list.
    void recordTlasUpdate(VkCommandBuffer cmd) {
        asBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kAsBuild, kAsWrite, kAsBuild, kAsRead | kAsWrite);
        auto ti = tlasInfo(tlasRebuild_ ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR);
        ti.scratchData.deviceAddress = scratchTlas_;
        const VkAccelerationStructureBuildRangeInfoKHR tr{tlasInstanceCount_, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
        vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &ti, &trp);
        asBarrier(cmd, kAsBuild, kAsWrite, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kAsRead);
        ++(tlasRebuild_ ? rtStatus_.tlasBuilds : rtStatus_.tlasUpdates);
        tlasDirty_ = tlasRebuild_ = false;
    }

    // ---- Scene residency (Renderer::upload / uploadPart; docs/RENDERING.md "Resident units") ----
    // A texture kept by key: its image, the Generation holding its memory and set, owning part, last upload listing it.
    struct SceneImage {
        GpuImage img;
        std::shared_ptr<Generation> gen;
        uint64_t part = 0, upload = 0;
    };
    // A renderer part being created (pumpParts): its scene until all of it was looked at, the Generation it fills.
    struct Part {
        std::shared_ptr<const scene::SceneData> data;
        std::shared_ptr<Generation> gen;
        size_t texture = 0, mesh = 0;
    };
    // Copies staged in host memory, recorded at the start of the next command buffer (recordPending).
    struct Pending {
        Buffer src;
        std::shared_ptr<Generation> mem;
        std::vector<std::pair<VkBuffer, VkBufferCopy>> buffers;
        std::vector<std::pair<VkImage, std::vector<VkBufferImageCopy>>> images;
    };
    static constexpr size_t kPartBytesPerFrame = size_t(8) << 20; // ~1 ms of memcpy + create per frame
    static constexpr VkDeviceSize kPartBlock = VkDeviceSize(16) << 20;

    std::shared_ptr<Generation> newGeneration(VkDeviceSize blockSize, size_t textures) {
        auto g = std::make_shared<Generation>();
        g->device = device_;
        g->memory.init(device_, gpu_, blockSize, rtStatus_.supported);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(std::max<size_t>(1, textures))};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, ps.descriptorCount, 1, &ps};
        VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &g->pool));
        return g;
    }

    std::optional<SceneImage> createTexture(const scene::TextureData& t, const std::shared_ptr<Generation>& gen, UploadBatch& batch,
                                            std::vector<std::string>& warnings) {
        auto warn = [&](const std::string& w) { warnings.push_back("texture " + std::to_string(t.id) + " (" + t.origin + "): " + w); };
        VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
        switch (t.format) {
        case scene::PixelFormat::RGBA8: break;
        case scene::PixelFormat::BGRA8: fmt = VK_FORMAT_B8G8R8A8_UNORM; break;
        case scene::PixelFormat::BC1: fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break; // DXT1 with 1-bit alpha, as D3D9
        case scene::PixelFormat::BC2: fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case scene::PixelFormat::BC3: fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        }
        if (blockBytes(t.format) && !bc_) { warn("BCn unsupported (no textureCompressionBC)"); return std::nullopt; }
        std::string problem;
        const uint32_t levels = usableMips(t, problem);
        if (!problem.empty()) warn(problem);
        if (levels == 0) return std::nullopt;
        SceneImage s{createImage(gen->memory, fmt, t.mips[0].width, t.mips[0].height, levels,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_SAMPLE_COUNT_1_BIT, t.cube),
                     gen};
        // Cube maps (environment reflections of the original shaders) never feed the 2D generic / RT paths.
        s.img.cube = t.cube;
        s.img.set = t.cube ? white_.set : allocateSet(gen->pool, s.img.view, textureSampler());
        UploadBatch::ImageJob job{s.img.image, {}, t.cube ? 6u : 1u};
        for (uint32_t i = 0; i < levels; ++i)
            job.mips.push_back({t.mips[i].data.data(), t.mips[i].data.size(), t.mips[i].width, t.mips[i].height});
        batch.images.push_back(std::move(job));
        return s;
    }

    bool createMesh(const scene::MeshData& m, const std::shared_ptr<Generation>& gen, GpuMesh& g, UploadBatch& batch,
                    std::vector<std::string>& warnings) {
        auto warn = [&](const std::string& w) { warnings.push_back("mesh (" + m.origin + "): " + w); };
        if (m.vertices.empty() || m.indices.empty()) { warn("empty"); return false; }
        if (std::any_of(m.indices.begin(), m.indices.end(), [&](uint32_t i) { return i >= m.vertices.size(); })) {
            warn("index out of range, skipped");
            return false;
        }
        for (const auto& s : m.submeshes) {
            if (uint64_t(s.firstIndex) + s.indexCount > m.indices.size()) warn("submesh range out of bounds, skipped");
            else g.submeshes.push_back(s);
        }
        if (m.submeshes.empty()) g.submeshes.push_back({0, uint32_t(m.indices.size()), 0});
        g.lightBoxes = m.lightBoxes;
        g.gen = gen;
        g.rtSerial = ++rtSerials_;
        g.model = glm::make_mat4(m.transform);
        g.skinned = !m.skin.empty();
        // [I] Water surfaces are objects named *watersurface* (l4_pool: ca_goo_pool_watersurface, the
        // surface intro of its watervolume, docs/GAMEPLAY.md "Water"); their meshes get RT reflections.
        g.water = isWaterSurface(m.origin);
        const glm::vec4 sphere = original::boundingSphere(m.vertices, g.model);
        g.center = glm::vec3(sphere), g.radius = sphere.w;
        setLod(g, m);
        g.bounds = meshBounds(m);
        const VkDeviceSize vb = m.vertices.size() * sizeof(scene::Vertex), ib = m.indices.size() * sizeof(uint32_t);
        const VkBufferUsageFlags rtInput = rtStatus_.supported ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                                               : 0;
        g.vertices = createBuffer(gen->memory, vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | rtInput,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        g.indices = createBuffer(gen->memory, ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtInput,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        batch.buffers.push_back({g.vertices.buffer, {m.vertices.data(), vb, 0, 0}});
        batch.buffers.push_back({g.indices.buffer, {m.indices.data(), ib, 0, 0}});
        g.vertexCount = m.vertices.size();
        g.indexCount = m.indices.size();
        return true;
    }

    // A resident mesh listed under a new transform (a part sent in its unit frame, then moved by its stream offset).
    static void place(GpuMesh& g, const scene::MeshData& m) {
        const glm::mat4 model = glm::make_mat4(m.transform);
        if (model == g.model) return;
        g.model = model, g.exteriorShift = glm::vec3(0.0f);
        const glm::vec4 sphere = original::boundingSphere(m.vertices, g.model);
        g.center = glm::vec3(sphere), g.radius = sphere.w;
        setLod(g, m);
        g.bounds = meshBounds(m);
    }

    // Sends the parts' keyed content not resident yet, about `budget` bytes per call (drawFrame: spread over frames), and
    // claims what is already resident for the part. A finished part drops its SceneData.
    void pumpParts(size_t budget, std::vector<std::string>& warnings) {
        UploadBatch batch;
        size_t sent = 0;
        std::vector<Part*> done;
        for (auto& [id, p] : parts_) {
            if (!p.data) continue;
            const scene::SceneData& s = *p.data;
            if (!p.gen) p.gen = newGeneration(kPartBlock, s.textures.size());
            for (; p.texture < s.textures.size() && sent < budget; ++p.texture) {
                const scene::TextureData& t = s.textures[p.texture];
                if (!t.key) continue;
                if (const auto it = resident_.find(t.key); it != resident_.end()) {
                    it->second.part = id;
                    continue;
                }
                auto created = createTexture(t, p.gen, batch, warnings);
                if (!created) continue;
                for (const auto& mip : t.mips) sent += mip.data.size();
                created->part = id;
                resident_.emplace(t.key, std::move(*created));
            }
            for (; p.texture == s.textures.size() && p.mesh < s.meshes.size() && sent < budget; ++p.mesh) {
                const scene::MeshData& m = s.meshes[p.mesh];
                if (!m.key) continue;
                if (const auto it = parked_.find(m.key); it != parked_.end()) {
                    it->second.part = id;
                    continue;
                }
                const auto cur = std::find_if(meshes_.begin(), meshes_.end(), [&](const GpuMesh& g) { return g.key == m.key; });
                if (cur != meshes_.end()) {
                    cur->part = id;
                    continue;
                }
                GpuMesh g;
                if (!createMesh(m, p.gen, g, batch, warnings)) continue;
                sent += m.vertices.size() * sizeof(scene::Vertex) + m.indices.size() * sizeof(uint32_t);
                g.key = m.key, g.part = id;
                parked_.emplace(m.key, std::move(g));
            }
            if (p.texture == s.textures.size() && p.mesh == s.meshes.size()) done.push_back(&p);
            if (sent >= budget) break;
        }
        stage(batch); // reads the parts' data: before dropping it
        for (Part* p : done) p->data.reset(), p->gen.reset();
    }

    // Destroyed once the frames recorded until now retired (retire, after drawFrame's fence wait).
    void bury(std::function<void()> destroy) { graves_.emplace_back(frameCounter_, std::move(destroy)); }
    void retire(bool all) {
        size_t n = 0;
        while (n < graves_.size() && (all || graves_[n].first + kFramesInFlight <= frameCounter_)) graves_[n++].second();
        graves_.erase(graves_.begin(), graves_.begin() + ptrdiff_t(n));
    }
    void destroyBlases(std::span<const RtBlas> blases) {
        for (const RtBlas& b : blases) {
            if (b.as) vkDestroyAccelerationStructureKHR_(device_, b.as, nullptr);
            vkDestroyBuffer(device_, b.buffer.buffer, nullptr);
        }
    }
    void buryRt(GpuMesh& g) {
        if (!g.blases.empty()) bury([this, b = std::move(g.blases)] { destroyBlases(b); });
        g.blases.clear();
        g.rtMerged.clear();
        g.rtMasks.clear();
        g.rtStatic = false;
    }
    void buryGroup(RtGroup& g) {
        bury([this, b = std::move(g.blas), t = g.transformBuffer, gen = std::move(g.gen)] {
            destroyBlases(std::span(&b, 1));
            vkDestroyBuffer(device_, t.buffer, nullptr);
        });
    }
    void buryMesh(GpuMesh& g) {
        bury([this, v = g.vertices, i = g.indices, prev = g.prevVertices, b = std::move(g.blases), gen = std::move(g.gen)] {
            for (const Buffer& x : {v, i, prev}) vkDestroyBuffer(device_, x.buffer, nullptr);
            destroyBlases(b);
        });
    }
    void buryTexture(SceneImage& t) {
        bury([this, img = t.img, gen = std::move(t.gen)]() mutable { destroyImage(img); });
    }

    // The previous upload's per-upload state to the graveyard: material sets + UBO, TLAS + instance / record buffers,
    // unkeyed textures and its Generation (BLASes stay with their meshes / RtGroups). Its programs wait in origStash_
    // for uploadOriginals to take the ones the new scene still uses.
    void retireScene() {
        if (!origStash_.empty()) bury([this, s = std::move(origStash_)]() mutable { destroyPipelines(device_, s); });
        origStash_ = std::move(origPrograms_);
        origPrograms_.clear();
        origWarmKey_ = -1;
        origDraws_.clear();
        screenSets_.clear();
        reflectSets_.clear();
        origMaterialImages_.clear();
        bury([this, ubo = std::exchange(origMaterialUbo_, {}), pool = std::exchange(origScenePool_, VK_NULL_HANDLE),
              tlas = std::exchange(tlas_, VK_NULL_HANDLE),
              buffers = std::array{std::exchange(tlasBuffer_, {}), std::exchange(instanceBuffer_, {}), std::exchange(geometryBuffer_, {}),
                                   std::exchange(scratch_, {})},
              rtPool = std::exchange(rtPool_, VK_NULL_HANDLE), images = std::move(transient_), gen = std::move(gen_)] {
            vkDestroyBuffer(device_, ubo.buffer, nullptr);
            vkDestroyDescriptorPool(device_, pool, nullptr);
            if (tlas) vkDestroyAccelerationStructureKHR_(device_, tlas, nullptr);
            for (const Buffer& b : buffers) vkDestroyBuffer(device_, b.buffer, nullptr);
            vkDestroyDescriptorPool(device_, rtPool, nullptr);
            for (const SceneImage& s : images) {
                GpuImage img = s.img;
                destroyImage(img);
            }
        });
        transient_.clear();
        textures_.clear();
        rtSet_ = VK_NULL_HANDLE;
        rtEvicted_.clear();
        tlasDirty_ = tlasRebuild_ = false;
        rtStatus_.blas = rtStatus_.instances = rtStatus_.geometries = rtStatus_.waterInstances = 0;
    }

    // Copies every region through one host-visible staging buffer, recorded by the next recordPending.
    void stage(const UploadBatch& batch) {
        auto aligned = [](VkDeviceSize s) { return (s + 15) & ~VkDeviceSize(15); }; // BCn block + 4-byte rules
        VkDeviceSize total = 0;
        for (const auto& [_, r] : batch.buffers) total += aligned(r.size);
        for (const auto& job : batch.images)
            for (const auto& r : job.mips) total += aligned(r.size);
        if (total == 0) return;
        Pending p;
        p.mem = std::make_shared<Generation>();
        p.mem->device = device_;
        p.mem->memory.init(device_, gpu_, 0);
        p.src = createBuffer(p.mem->memory, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceSize off = 0;
        for (const auto& [dst, r] : batch.buffers) {
            std::memcpy(p.src.mapped + off, r.src, r.size);
            p.buffers.push_back({dst, {off, 0, r.size}});
            off += aligned(r.size);
        }
        for (const auto& job : batch.images) {
            auto& [image, regions] = p.images.emplace_back(job.image, std::vector<VkBufferImageCopy>{});
            for (uint32_t level = 0; level < job.mips.size(); ++level) {
                const auto& r = job.mips[level];
                std::memcpy(p.src.mapped + off, r.src, r.size);
                VkBufferImageCopy copy{};
                copy.bufferOffset = off;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, job.layers};
                copy.imageExtent = {r.width, r.height, 1};
                regions.push_back(copy);
                off += aligned(r.size);
            }
        }
        pending_.push_back(std::move(p));
    }

    // The staged copies, then images shader-readable and buffers visible to vertex input, shaders and AS builds.
    void recordPending(VkCommandBuffer cmd) {
        if (pending_.empty()) return;
        constexpr VkPipelineStageFlags2 kAll = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kCopy = VK_PIPELINE_STAGE_2_COPY_BIT;
        // Write-after-read: earlier frames on this queue may still read a re-staged resource (the attenuation table).
        std::vector<VkImage> images; // once each: two uploads before a frame both stage the attenuation table
        for (const Pending& p : pending_)
            for (const auto& [image, _] : p.images)
                if (std::find(images.begin(), images.end(), image) == images.end()) images.push_back(image);
        std::vector<VkImageMemoryBarrier2> b;
        for (const VkImage image : images)
            b.push_back(imageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAll, 0, kCopy,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT));
        const VkMemoryBarrier2 war{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kAll, 0, kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT};
        barriers(cmd, b, &war);
        // An image staged twice (upload() and a fog change both stage the attenuation table) is copied in staging order:
        // the later copy waits for the earlier (sync validation WRITE_AFTER_WRITE; unordered, either table could win).
        std::vector<VkImage> copied;
        const VkMemoryBarrier2 waw{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCopy,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT};
        for (const Pending& p : pending_) {
            for (const auto& [dst, copy] : p.buffers) vkCmdCopyBuffer(cmd, p.src.buffer, dst, 1, &copy);
            for (const auto& [image, regions] : p.images) {
                if (std::find(copied.begin(), copied.end(), image) != copied.end()) {
                    barriers(cmd, {}, &waw);
                    copied.clear();
                }
                copied.push_back(image);
                vkCmdCopyBufferToImage(cmd, p.src.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                                       regions.data());
            }
        }
        b.clear();
        const VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        for (const VkImage image : images)
            b.push_back(imageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCopy,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT, readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
        const VkMemoryBarrier2 data{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | readers | VK_PIPELINE_STAGE_2_COPY_BIT |
                                        (rtStatus_.supported ? kAsBuild : 0),
                                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                                        VK_ACCESS_2_TRANSFER_READ_BIT};
        barriers(cmd, b, &data);
        for (Pending& p : pending_) bury([this, src = p.src, mem = std::move(p.mem)] { vkDestroyBuffer(device_, src.buffer, nullptr); });
        pending_.clear();
    }

    // Shutdown (device idle): everything at once.
    void destroySceneResources() {
        for (auto& g : meshes_) buryMesh(g);
        for (auto& g : rtGroups_) buryGroup(g);
        rtGroups_.clear(), movers_.clear();
        for (auto& [_, g] : parked_) buryMesh(g);
        for (auto& [_, t] : resident_) buryTexture(t);
        meshes_.clear(), parked_.clear(), resident_.clear(), meshSlot_.clear(), parts_.clear();
        retireScene();
        destroyPipelines(device_, origStash_);
        origStash_.clear();
        for (Pending& p : pending_) bury([this, src = p.src, mem = std::move(p.mem)] { vkDestroyBuffer(device_, src.buffer, nullptr); });
        pending_.clear();
        for (auto& st : staging_) st.meshes.clear(), st.used = 0;
        retire(true);
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
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu_, surfaceFormat_.format, &fp);
        feedbackCopyable_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
                            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
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
                                 (fsrActiveRequest() || prepassPipelines_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0), // + pre-pass readers
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
        for (GpuImage& t : splashTargets_) // the splash fluid (recordSplashFluid); laid out per frame
            t = createImage(swapMemory_, kSplashFormat, extent_.width, extent_.height, 1,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        uwParticles_ = createImage(swapMemory_, kUwParticleFormat, extent_.width, extent_.height, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        for (GpuImage& t : shaftTargets_) // under-water sun shafts (recordUnderwaterShafts): half the render size
            t = createImage(swapMemory_, VK_FORMAT_R16G16B16A16_SFLOAT, (extent_.width + 1) / 2, (extent_.height + 1) / 2, 1,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        // under-water composite (recordUwComposite): written each graded frame before any read (old contents discarded)
        uwHdr_ = createImage(swapMemory_, VK_FORMAT_R16G16B16A16_SFLOAT, extent_.width, extent_.height, 1,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        for (int i = 0; i < kUwBloomLevels; ++i)
            uwBloom_[size_t(i)] = createImage(swapMemory_, VK_FORMAT_R16G16B16A16_SFLOAT, std::max(1u, extent_.width >> (i + 1)),
                                              std::max(1u, extent_.height >> (i + 1)), 1,
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                              VK_IMAGE_ASPECT_COLOR_BIT);
        const uint32_t bw = std::max(1u, extent_.width / 2), bh = std::max(1u, extent_.height / 2);
        filterLevels_ = std::min(6u, 1u + uint32_t(std::log2(float(std::max(bw, bh)))));
        filterBlur_ = createImage(swapMemory_, sceneFormat_, bw, bh, filterLevels_,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        reflColor_ = createImage(swapMemory_, sceneFormat_, bw, bh, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        reflDepth_ = createImage(swapMemory_, depthFormat_, bw, bh, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT, samples_);
        if (samples_ != VK_SAMPLE_COUNT_1_BIT)
            reflMsColor_ = createImage(swapMemory_, sceneFormat_, bw, bh, 1,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT, samples_);
        feedback_ = createImage(swapMemory_, sceneFormat_, bw, bh, filterLevels_,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        hasFeedback_ = false;
        oneShot([&](VkCommandBuffer cmd) { // valid sampled layouts from the start (post binds them always)
            const VkImageMemoryBarrier2 b[6] = {
                imageBarrier(uwParticles_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
                imageBarrier(reflColor_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
                imageBarrier(filterBlur_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
                imageBarrier(feedback_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
            for (const GpuImage* t : {&shaftTargets_[0], &shaftTargets_[1]}) { // written before any read
                const auto s = imageBarrier(t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                barriers(cmd, {&s, 1});
            }
        });
        writeWaterSets();
        if (samples_ != VK_SAMPLE_COUNT_1_BIT)
            msColor_ = createImage(swapMemory_, sceneFormat_, extent_.width, extent_.height, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, samples_);
        writeSet(postSet_, sceneColor_.view, postSampler_);
        writeSet(uwHdrSet_, uwHdr_.view, postSampler_);
        for (int i = 0; i < kUwBloomLevels; ++i) writeSet(uwBloomSets_[size_t(i)], uwBloom_[size_t(i)].view, postSampler_);
        createFsrTargets();
        createIndirectTargets();
        createCortaoTargets();
        setMipBias(fsr_.valid() ? std::log2(float(extent_.width) / float(display_.width)) - 1.0f : 0.0f);
    }

    // Destroys the per-swapchain objects (views, semaphores, depth) and `chain` itself. Device must be idle.
    void destroySwapchain(VkSwapchainKHR chain) {
        for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
        for (VkSemaphore s : presentReady_) vkDestroySemaphore(device_, s, nullptr);
        swapViews_.clear();
        presentReady_.clear();
        swapImages_.clear();
        for (GpuImage* img : {&depth_, &sceneColor_, &msColor_, &opaqueColor_, &opaqueDepth_, &filterBlur_, &feedback_, &reflColor_, &reflMsColor_,
                              &reflDepth_, &motion_, &reactive_, &splashTargets_[0], &splashTargets_[1], &uwParticles_,
                              &shaftTargets_[0], &shaftTargets_[1], &uwHdr_,
                              &fsrInput_, &fsrOutput_, &fsrGenerated_})
            if (img->image) destroyImage(*img);
        for (GpuImage& img : uwBloom_)
            if (img.image) destroyImage(img);
        for (GpuImage* img : indImages())
            if (img->image) destroyImage(*img);
        destroyCortaoTargets();
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
    bool waterTess_ = false; // tessellationShader enabled: the Enhanced water surface is tessellated (water.tesc / .tese)
    bool cortaoFeatures_ = false; // the device features CORTAO needs are enabled (createDevice)
    VkShaderStageFlags waterPushStages() const {
        return VK_SHADER_STAGE_VERTEX_BIT | (waterTess_ ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0);
    }
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
    // Enhanced water: single-sample opaque copies (refraction etc.) and the interactive wave field's texture (uRipples).
    GpuImage opaqueColor_, opaqueDepth_, ripples_;
    // FFT ocean (createOcean): uOcean (grid 128 at mip 0 / 64 at mip 1 and level 0, the rest its means) + its storage views, spectra, scratch, pipeline.
    GpuImage ocean_;
    std::array<VkImageView, ocean::kLevels> oceanStorage_{}; // one storage view per mip
    Buffer oceanSpectrum_, oceanWork_;
    VkSampler oceanSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout oceanSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout oceanLayout_ = VK_NULL_HANDLE;
    VkPipeline oceanPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool oceanPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, ocean::kLevels> oceanSets_{}; // mip steps 0..kLevels-2, then Up
    ocean::Clock oceanClock_;
    // Planar reflection (original::reflection): half size, the scene's sample count, resolved into reflColor_.
    GpuImage reflColor_, reflMsColor_, reflDepth_;
    GpuImage filterBlur_;        // screen filters: the half-size scene + mips (recordFilterBlur)
    GpuImage feedback_;          // PCFeedbackEffect: the previous frame's final image, half size + mips (0x51A2E0)
    uint32_t filterLevels_ = 0;
    bool hasFeedback_ = false;   // feedback_ holds last frame's copy (+0x3D)
    bool feedbackCopyable_ = false; // the post target can be a blit source (swapchain usage / format)
    // Interactive wave field (createWaterField): ripples_ above is its uRipples texture.
    Buffer fieldState_, fieldWork_, fieldNoKicks_;
    std::array<Buffer, kFramesInFlight> fieldKicks_{}, fieldFrame_{};
    VkBuffer fieldExternal_ = VK_NULL_HANDLE; // setExternalWaveKicks
    VkDescriptorSetLayout fieldSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fieldLayout_ = VK_NULL_HANDLE;
    VkPipeline fieldPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool fieldPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> fieldSets_{};
    waterfield::Stepper fieldStepper_;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlags supportedSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat sceneFormat_ = VK_FORMAT_R8G8B8A8_UNORM, shadowFormat_ = VK_FORMAT_UNDEFINED;

    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout sceneLayout_ = VK_NULL_HANDLE, textLayout_ = VK_NULL_HANDLE, lineLayout_ = VK_NULL_HANDLE,
                     postLayout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> scenePipelines_{}; // indexed by scene::Blend
    VkPipeline pssmPipeline_ = VK_NULL_HANDLE;
    VkPipeline textPipeline_ = VK_NULL_HANDLE, linePipeline_ = VK_NULL_HANDLE, shadowPipeline_ = VK_NULL_HANDLE,
               postPipeline_ = VK_NULL_HANDLE, prepassPipeline_ = VK_NULL_HANDLE;
    VkPipeline waterPipeline_ = VK_NULL_HANDLE, waterRtPipeline_ = VK_NULL_HANDLE;
    VkPipeline waterTessPipeline_ = VK_NULL_HANDLE, waterTessRtPipeline_ = VK_NULL_HANDLE; // waterTess_: water.tesc / .tese
    VkDescriptorSetLayout waterSetLayout_ = VK_NULL_HANDLE, postWaterSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout waterLayout_ = VK_NULL_HANDLE; // water: set 0 = WaterBlock + textures
    // Splash fluid passes (recordSplashFluid): null without blendable RGBA32F. Targets: [0] splat / blurred, [1] blur
    // temporary.
    static constexpr VkFormat kSplashFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipeline splashSplatPipeline_ = VK_NULL_HANDLE, splashBlurPipeline_ = VK_NULL_HANDLE,
               splashCompositePipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout splashSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout splashLayout_ = VK_NULL_HANDLE;
    std::array<GpuImage, 2> splashTargets_{};
    // Per frame in flight, by pass input: [0] opaque depth (splat), [1] target 0 (horizontal blur, composite), [2] target 1.
    std::array<std::array<VkDescriptorSet, 3>, kFramesInFlight> splashSets_{};
    // Enhanced under-water sun shafts (recordUnderwaterShafts, render/Water.hpp): post layout, post set 1 bindings 6-7.
    // Targets (half the render size, RGBA16F): [0] march, [1] blurred (the grade's upsample).
    VkPipeline shaftMarchPipeline_ = VK_NULL_HANDLE, shaftBlurPipeline_ = VK_NULL_HANDLE;
    std::array<GpuImage, 2> shaftTargets_{};
    // Enhanced water's animated water texture (createAgWater / recordAgWater; uAgWater: post set 1 binding 10, water set 5).
    GpuImage agWater_{};
    VkImageView agWaterLevel0_ = VK_NULL_HANDLE;
    VkSampler agWaterSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout agWaterLayout_ = VK_NULL_HANDLE;
    VkPipeline agWaterPipeline_ = VK_NULL_HANDLE;
    VkPipeline agCausticPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool waterPool_ = VK_NULL_HANDLE;
    std::array<Buffer, kFramesInFlight> waterUbo_{}; // WaterBlock, host-coherent
    std::array<VkDescriptorSet, kFramesInFlight> waterSets_{}, postWaterSets_{};
    // The splash fluid's simulation (render/Fluid.hpp; createFluid / recordFluid): fluid.comp with one set per frame in
    // flight (0 WaterBlock, 1 particle pool, 2 neighbour grid, 3 C3 kicks, 4 this frame's bursts, 5 opaque depth, 6
    // ripples); pool / grid / kicks device-local, bursts host-visible.
    VkDescriptorSetLayout fluidSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fluidLayout_ = VK_NULL_HANDLE;
    VkPipeline fluidPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool fluidPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> fluidSets_{};
    Buffer fluidParticles_, fluidGrid_, fluidKicks_;
    std::array<Buffer, kFramesInFlight> fluidBursts_{};
    fluid::Sim fluidSim_;
    // Contract C3: the particles' wave kicks (fluid::kKickBytes, std430 OuWaveKick buffer), written by recordFluid's
    // compute passes (count zeroed at their start, vkCmdFillBuffer) and left for a compute consumer behind a
    // COMPUTE_SHADER write -> COMPUTE_SHADER read/write barrier. Null without the fluid.
    VkBuffer fluidKickBuffer() const { return fluidKicks_.buffer; }
    // Enhanced under-water particles (recordUwParticles; render/Water.hpp UwParticlePush): premultiplied render-size
    // target sampled by the post pass (set 1 binding 9), kept SHADER_READ_ONLY between frames; per-frame bubble instances.
    static constexpr VkFormat kUwParticleFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t kPostPushBytes = 24; // post.frag Push
    GpuImage uwParticles_;
    VkPipeline uwSnowPipeline_ = VK_NULL_HANDLE, uwBubblePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout uwLayout_ = VK_NULL_HANDLE;
    std::array<LinearAllocator, kFramesInFlight> bubbleMemory_;
    std::array<Buffer, kFramesInFlight> bubbleBuffers_{};
    std::array<VkDeviceSize, kFramesInFlight> bubbleCapacity_{};
    // Enhanced under-water composite (recordUwComposite; render/Water.hpp UwGrade): the post pass's linear HDR frame
    // (uwHdr_, render size) and the bloom chain (uwBloom_[i]: W >> (i + 1)), all RGBA16F, kept SHADER_READ_ONLY between
    // frames; one single-sampler set (setLayout_, postSampler_) per image; uwGradeLayout_: sets 0 and 1 = setLayout_,
    // push vec4 (fragment).
    GpuImage uwHdr_;
    std::array<GpuImage, kUwBloomLevels> uwBloom_{};
    VkDescriptorSet uwHdrSet_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kUwBloomLevels> uwBloomSets_{};
    VkPipelineLayout uwGradeLayout_ = VK_NULL_HANDLE;
    VkPipeline uwResolvePipeline_ = VK_NULL_HANDLE, uwBloomPipeline_ = VK_NULL_HANDLE, uwBloomUpPipeline_ = VK_NULL_HANDLE,
               uwGradePipeline_ = VK_NULL_HANDLE;
    std::array<LinearAllocator, kFramesInFlight> fxMemory_; // original effects (FrameOptions::fxVertices)
    std::array<Buffer, kFramesInFlight> fxBuffers_{};
    std::array<VkDeviceSize, kFramesInFlight> fxCapacity_{};
    std::array<VkPipeline, 4> fxPipelines_{}; // FxBatch::blend: alpha, additive, subtract, multiply
    VkPipelineLayout fxLayout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> skyPipelines_{}; // time of day backdrop: [0] multiply, [1] add (render/TimeOfDay.hpp)
    VkPipelineLayout skyLayout_ = VK_NULL_HANDLE;
    std::array<LinearAllocator, kFramesInFlight> lineMemory_;
    VkSampler sceneSampler_ = VK_NULL_HANDLE, fontSampler_ = VK_NULL_HANDLE, shadowSampler_ = VK_NULL_HANDLE,
              postSampler_ = VK_NULL_HANDLE, biasedSampler_ = VK_NULL_HANDLE, // biasedSampler_: setMipBias
              blurSampler_ = VK_NULL_HANDLE; // screen-filter blur levels (trilinear)
    float mipBias_ = 0.0f;
    std::vector<std::pair<VkDescriptorSet, std::array<VkDescriptorImageInfo, 16>>> origMaterialImages_; // setMipBias
    VkDescriptorPool staticPool_ = VK_NULL_HANDLE;
    GpuImage white_, font_, shadow_;
    GpuImage pssm_; // tru.exe's PSSM atlas (original::Pssm, s14 of the PSSM programs); SHADER_READ_ONLY outside its pass
    std::array<UiImage, 3> uiImages_; // FrameOptions::uiImages
    VkDescriptorSet postSet_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;           // sun.glsl: atlas + OuShadow UBO
    std::array<Buffer, kFramesInFlight> shadowUbo_{};                 // OuShadow, host-coherent
    std::array<VkDescriptorSet, kFramesInFlight> shadowSets_{};
    Bounds bounds_;
    std::vector<DrawRef> order_;
    std::vector<float> opacity_; // per frame: FrameOptions::meshOpacity per mesh (empty: all 1)
    glm::vec3 lodEye_{0.0f};     // per frame: the camera batch LODs are measured from (every pass, 0x50F7C0)
    std::span<const UnitClip> unitClips_; // per frame: FrameOptions::unitClips
    std::vector<int> meshClip_;           // per frame and GPU mesh: index into unitClips_, -1 = unclipped

    LinearAllocator staticMemory_, swapMemory_;
    std::unordered_map<uint32_t, GpuImage> textures_; // the current upload's, by TextureData::id (owned below)
    std::unordered_map<uint64_t, SceneImage> resident_; // keyed textures: listed by the current upload or owned by a part
    std::vector<SceneImage> transient_;                 // the current upload's unkeyed textures
    std::unordered_map<uint64_t, GpuMesh> parked_;      // keyed meshes of live parts not listed by the current upload
    std::map<uint64_t, Part> parts_;
    std::shared_ptr<Generation> gen_;                   // the current upload's memory (unkeyed content, RT, materials)
    std::vector<std::pair<uint64_t, std::function<void()>>> graves_; // (frameCounter_ when buried, destroy)
    std::vector<Pending> pending_;
    uint64_t uploads_ = 0;
    std::vector<GpuMesh> meshes_;
    std::vector<size_t> meshSlot_; // last upload()'s SceneData::meshes index -> meshes_ index (SIZE_MAX = skipped)
    std::vector<uint8_t> sea_;     // per frame (Enhanced): render::reflectsWater per mesh
    // Enhanced water draws this submesh (water pass, not the scene passes).
    bool waterDraw(uint32_t mesh, uint32_t sub) const {
        const GpuMesh& m = meshes_[mesh];
        return m.water || (mesh < sea_.size() && sea_[mesh] && sub < m.reflective.size() && m.reflective[sub]);
    }

    // Original Fidelity (translated TRU shaders).
    VkDescriptorSetLayout origFrameLayout_ = VK_NULL_HANDLE, origMatLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout origLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool origStaticPool_ = VK_NULL_HANDLE, origScenePool_ = VK_NULL_HANDLE;
    std::array<Buffer, kFramesInFlight> origFrameUbo_{};
    std::array<VkDescriptorSet, kFramesInFlight> origFrameSet_{};
    std::array<Buffer, kFramesInFlight> reflFrameUbo_{}; // the planar reflection camera's frame block
    std::array<VkDescriptorSet, kFramesInFlight> reflFrameSet_{};
    GpuImage attenuation_, blackCube_, cookies_; // cookies_: s12/s13 spot cookie array (original::cookieArray)
    std::vector<uint32_t> cookieIds_;            // its layers (upload)
    std::array<uint8_t, 256 * 4> fogRow_ = original::fogLutRow(nullptr); // the attenuation LUT's fog row as uploaded
    std::vector<OrigProgram> origPrograms_;
    std::vector<OrigProgram> origStash_; // the previous upload's programs, during the next upload (destroyOriginalScene)
    int origWarmKey_ = -1; // warmOriginalPipelines' settings since the pipelines were last destroyed
    // meshes_ index -> [0: mesh, 1 + terrain light group] (frameCounter_ + 1, light set)
    std::vector<std::vector<std::pair<uint64_t, original::LightSet>>> meshLights_;
    std::vector<std::vector<OrigDraw>> origDraws_; // meshes_ index -> per submesh
    std::vector<std::pair<VkDescriptorSet, uint32_t>> screenSets_; // material sets with back/depth-buffer samplers
    std::vector<std::pair<VkDescriptorSet, uint32_t>> reflectSets_; // material sets with planar reflection samplers
    std::vector<DrawRef> screenDraws_;                               // per frame: their draws, for the second pass
    Buffer origMaterialUbo_;
    scene::Environment environment_;
    original::SunFields uploadSun_;        // the uploaded environment's sun (applySky restores it without a time of day)
    original::IrradianceSlots irradiance_; // per frame, cached per mesh between uploads

    // Ray tracing.
    RtStatus rtStatus_;
    bool rtPipelines_ = false, tlasDirty_ = false; // scene pipelines use scene_rt.frag; TLAS needs an update
    bool tlasRebuild_ = false;                     // ... a rebuild: evictStatic changed its instances
    VkDeviceSize scratchAlign_ = 256;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR_ = nullptr;
    VkDescriptorSetLayout rtSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool rtPool_ = VK_NULL_HANDLE;
    VkDescriptorSet rtSet_ = VK_NULL_HANDLE;
    // RT AO / GI (recordIndirect): pipelines, targets at half render resolution, the previous frame's jittered
    // view-projection (double) and eye, and the frame (frameCounter_ + 1) that last ran the pass (0 = none).
    VkDescriptorSetLayout indSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout indLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool indPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> indSets_{};
    VkPipeline indTrace_ = VK_NULL_HANDLE, indTraceMs_ = VK_NULL_HANDLE, indDenoise_ = VK_NULL_HANDLE;
    struct { GpuImage signal, geom[2], albedo, hist[2], len[2], tmpA, tmpB, final; } ind_;
    VkExtent2D indExtent_{};
    glm::dmat4 indPrevVP_{1.0};
    glm::dvec3 indPrevEye_{0.0};
    uint64_t indLast_ = 0;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    Buffer tlasBuffer_, instanceBuffer_, geometryBuffer_, scratch_;
    std::vector<RtGroup> rtGroups_; // merged BLASes over the GpuMesh::rtStatic meshes (this upload's after buildRayTracing)
    static constexpr uint64_t kUnkeyedGroup = ~0ull; // RtGroup::id of unkeyed meshes (others: GpuMesh::part)
    std::unordered_set<uint64_t> movers_; // keys of meshes updateVertices moved out of a merged BLAS: kept out
    uint64_t rtSerials_ = 0;              // GpuMesh::rtSerial counter
    std::vector<size_t> rtEvicted_;    // meshes that left them since the last frame (evictStatic)
    VkDeviceAddress scratchBlas_ = 0, scratchTlas_ = 0; // aligned scratch regions: shared BLAS builds, TLAS
    VkAccelerationStructureGeometryKHR tlasGeometry_{};
    uint32_t tlasInstanceCount_ = 0, tlasInstanceCapacity_ = 0;

    // AMD FSR (docs/RENDERING.md "FSR").
    vkfsr::Context fsr_;
    std::string fsrReason_, fsrError_; // why FSR cannot run here; a runtime failure (FSR off until the settings change)
    bool fsrRequested_ = false, fsrFrameGen_ = false, fsrHistory_ = false;
    int fsrQuality_ = 1;
    // AMD Anti-Lag (docs/RENDERING.md "Anti-Lag"): null = no VK_AMD_anti_lag on this device.
    PFN_vkAntiLagUpdateAMD antiLagUpdate_ = nullptr;
    bool antiLagWanted_ = false; // the setting (last drawFrame's FrameOptions::antiLag)
    AntiLagMarkers antiLag_;
    std::array<float, 15> look_{}; // applySettings: the look-changing settings of the last frame
    // Depth pre-pass (RT or screen-space AO): opaque colour draws test LEQUAL against it; its normal target
    // (octahedral world normal, render/sun.glsl ouOctEncode), at the MSAA sample count (the AO passes read sample 0).
    static constexpr VkFormat kPrepassNormalFormat = VK_FORMAT_R16G16_SFLOAT;
    bool prepassPipelines_ = false;
    GpuImage prepassNormal_;
    // CORTAO (recordCortao, docs/RENDERING.md "Ambient occlusion"). cortaoReason_ non-empty: unsupported on this device.
    std::string cortaoReason_ = "not initialised";
    VkDescriptorSetLayout cortaoSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout cortaoLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool cortaoPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> cortaoSets_{};
    VkPipeline cortaoPrepass_[2]{}, cortaoMain_[4][2]{}, cortaoFilter_ = VK_NULL_HANDLE, cortaoTemporal_ = VK_NULL_HANDLE;
    VkSampler cortaoBorderInt_ = VK_NULL_HANDLE, cortaoBorderFloat_ = VK_NULL_HANDLE;
    GpuImage cortaoTable16_, cortaoTable32_;           // occluder lookup tables (3D, static)
    GpuImage cortaoPacked_, cortaoRaw_, cortaoFiltered_; // per swapchain: packed data (mips), main pass, filter output
    std::array<GpuImage, kFramesInFlight> aoMap_;     // temporal output = sun.glsl ouAoMap (r AO, g view depth); history
    std::array<VkImageView, 8> cortaoMipViews_{};     // storage views of cortaoPacked_'s mips (the last repeated)
    uint32_t cortaoMips_ = 1;
    uint64_t cortaoLast_ = 0;                          // frameCounter_ + 1 of the last frame that ran the passes
    glm::dmat4 cortaoPrevVP_{1.0};                     // its jittered view-projection (temporal reprojection)
    glm::dvec3 cortaoPrevEye_{0.0};
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
    // OU_VK_PRESENT_STATS GPU pass times: timestamps after begin (0), vertex uploads + BLAS refits (1), TLAS update (2),
    // shadow map (3), planar reflection (4), scene draws (5), copies + screen readers (6), Enhanced water (7), fx + lines
    // (8), FSR masks (9), under-water sun shafts (10), post (11), FSR / FG / end (12); per timed frame. Marks inside a
    // render pass time what the GPU has finished by then, so neighbouring scene segments can trade some time. 13 / 14
    // bracket the interactive wave field (recordWaterField; inside segment 1).
    static constexpr uint32_t kGpuMarks = 15, kGpuSegments = 12;
    // ... and the splash fluid's own (queries kGpuMarks + 0..2: before its simulation, after it, after its composite),
    // averaged over the frames that drew it.
    static constexpr uint32_t kFluidMarks = 3;
    // ... and the screen-space AO passes' (queries kGpuMarks + kFluidMarks + 0..1: around recordCortao).
    static constexpr uint32_t kAoMarks = 2, kAoQuery = kGpuMarks + kFluidMarks;
    std::vector<std::array<double, kGpuSegments>> gpuMs_; // per timed frame
    std::vector<double> fieldMs_;                         // the wave field per timed frame
    double fluidSimMs_ = 0, fluidDrawMs_ = 0;
    uint64_t fluidTimedFrames_ = 0;
    double aoMs_ = 0;
    uint64_t aoTimedFrames_ = 0;
    void gpuBegin(Frame& f, VkCommandBuffer cmd) {
        if (!presentStats_) return;
        if (!f.gpuTimes) {
            const VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
                                           kGpuMarks + kFluidMarks + kAoMarks};
            VK_CHECK(vkCreateQueryPool(device_, &qi, nullptr, &f.gpuTimes));
        } else if (f.gpuTimed) { // its fence was waited: the previous results are final
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(gpu_, &props);
            const double toMs = props.limits.timestampPeriod * 1e-6;
            uint64_t t[kGpuMarks];
            if (vkGetQueryPoolResults(device_, f.gpuTimes, 0, kGpuMarks, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                auto& ms = gpuMs_.emplace_back();
                for (uint32_t i = 1; i <= kGpuSegments; ++i) ms[i - 1] = double(t[i] - t[i - 1]) * toMs;
                fieldMs_.push_back(double(t[14] - t[13]) * toMs);
            }
            uint64_t ft[kFluidMarks];
            if (f.fluidTimed && vkGetQueryPoolResults(device_, f.gpuTimes, kGpuMarks, kFluidMarks, sizeof ft, ft, sizeof ft[0],
                                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                fluidSimMs_ += double(ft[1] - ft[0]) * toMs, fluidDrawMs_ += double(ft[2] - ft[1]) * toMs;
                ++fluidTimedFrames_;
            }
            uint64_t at[kAoMarks];
            if (f.aoTimed && vkGetQueryPoolResults(device_, f.gpuTimes, kAoQuery, kAoMarks, sizeof at, at, sizeof at[0],
                                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                aoMs_ += double(at[1] - at[0]) * toMs;
                ++aoTimedFrames_;
            }
        }
        vkCmdResetQueryPool(cmd, f.gpuTimes, 0, kGpuMarks + kFluidMarks + kAoMarks);
        f.gpuTimed = true;
        f.fluidTimed = f.aoTimed = false;
        gpuMark(cmd, 0);
    }
    void gpuMark(VkCommandBuffer cmd, uint32_t i) {
        if (presentStats_) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frames_[frameIndex_].gpuTimes, i);
    }
    void aoMark(VkCommandBuffer cmd, uint32_t i) {
        if (!presentStats_) return;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frames_[frameIndex_].gpuTimes, kAoQuery + i);
        frames_[frameIndex_].aoTimed = true;
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
        // whole run (load, cinematics) and the last 500 frames (the steady state ou-game's frame time line measures)
        for (const size_t n : {gpuMs_.size(), std::min<size_t>(gpuMs_.size(), 500)}) {
            if (!n) break;
            std::array<double, kGpuSegments> s{};
            double field = 0.0;
            for (size_t f = gpuMs_.size() - n; f < gpuMs_.size(); ++f) {
                for (size_t i = 0; i < s.size(); ++i) s[i] += gpuMs_[f][i] / double(n);
                field += fieldMs_[f] / double(n);
            }
            std::fprintf(stderr, "ou: GPU ms per frame (last %zu): uploads+BLAS %.2f, TLAS %.2f, shadow map %.2f, reflection %.2f, "
                         "scene %.2f, copies+screen %.2f, water %.2f, fx %.2f, FSR masks %.2f, uw shafts %.2f, post %.2f, "
                         "FSR/end %.2f; wave field %.3f\n",
                         n, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], field);
        }
        if (fluidTimedFrames_) {
            const double n = double(fluidTimedFrames_);
            std::fprintf(stderr, "ou: splash fluid GPU ms per drawing frame (%llu frames): simulation %.3f, drawing %.3f, total %.3f\n",
                         (unsigned long long)fluidTimedFrames_, fluidSimMs_ / n, fluidDrawMs_ / n, (fluidSimMs_ + fluidDrawMs_) / n);
        }
        if (aoTimedFrames_) // inside the "scene" segment above
            std::fprintf(stderr, "ou: screen-space AO (CORTAO prepass + main + filter + temporal) GPU ms per frame (%llu frames): %.3f\n",
                         (unsigned long long)aoTimedFrames_, aoMs_ / double(aoTimedFrames_));
    }
    std::atomic<int64_t> fenceNs_{0}, acquireNs_{0}, presentNs_{0}; // OU_VK_PRESENT_STATS wait totals
    int64_t frameAcquireNs_ = 0; // this frame's acquire wait (present())
};

} // namespace

std::unique_ptr<Renderer> createVulkanRenderer() { return std::make_unique<VkRenderer>(); }

} // namespace ou::render
