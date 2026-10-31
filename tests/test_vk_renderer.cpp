// Vulkan backend against a real window: swapchain recreation on an SDL resize. Skips without a
// display or a usable Vulkan 1.3 device.
#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>

#include "render/Renderer.hpp"
#include "scene/Placeholder.hpp"

namespace {

size_t nonClearPixels(const ou::render::Image& img) {
    size_t n = 0;
    for (size_t i = 0; i + 2 < img.rgb.size(); i += 3) // clear colour is (20, 23, 28)
        n += std::abs(img.rgb[i] - 20) + std::abs(img.rgb[i + 1] - 23) + std::abs(img.rgb[i + 2] - 28) > 6;
    return n;
}

double meanAbsDiff(const ou::render::Image& a, const ou::render::Image& b) {
    if (a.rgb.size() != b.rgb.size() || a.rgb.empty()) return 1e9;
    double s = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i) s += std::abs(int(a.rgb[i]) - int(b.rgb[i]));
    return s / double(a.rgb.size());
}

size_t brightness(const ou::render::Image& img) {
    size_t s = 0;
    for (uint8_t v : img.rgb) s += v;
    return s;
}

// A 320x240 Vulkan window + renderer; `ok` false = skip (no display / no Vulkan 1.3).
struct VkWindow {
    std::unique_ptr<ou::render::Renderer> r = ou::render::createVulkanRenderer();
    SDL_Window* w = nullptr;
    std::string skip;
    ou::render::Camera cam;
    VkWindow() {
        if (!SDL_Init(SDL_INIT_VIDEO)) { skip = std::string("no display: ") + SDL_GetError(); return; }
        std::string err;
        if (!r->preWindowSetup(err)) { skip = "Vulkan unavailable: " + err; return; }
        w = SDL_CreateWindow("ou vk rt test", 320, 240, SDL_WindowFlags(r->windowFlags()));
        if (!w || !r->init(w, err)) { skip = "init: " + err; return; }
        cam.position = {-1100.0f, -1300.0f, 700.0f};
        cam.lookAt({0.0f, 0.0f, 100.0f});
    }
    ~VkWindow() {
        r->shutdown();
        if (w) SDL_DestroyWindow(w);
        SDL_Quit();
    }
    ou::render::Image frame(const ou::render::FrameOptions& o) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        r->drawFrame(cam, {pw, ph}, o);
        ou::render::Image img = r->readBackbuffer({pw, ph});
        r->present();
        return img;
    }
};

ou::render::FrameOptions enhanced(ou::render::RayTracing rt = ou::render::RayTracing::Off) {
    ou::render::FrameOptions o;
    o.msaa = 4, o.shadows = true, o.rt = rt;
    return o;
}

// Backend whose preWindowSetup succeeds or fails on demand (selectRenderer logic, no GPU).
struct FakeRenderer final : ou::render::Renderer {
    bool vulkan, usable;
    FakeRenderer(bool vk, bool ok) : vulkan(vk), usable(ok) {}
    uint64_t windowFlags() const override { return 0; }
    bool preWindowSetup(std::string& e) override {
        if (!usable) e = vulkan ? "no Vulkan 1.3 device" : "no GL";
        return usable;
    }
    bool init(SDL_Window*, std::string&) override { return true; }
    std::vector<std::string> upload(const ou::scene::SceneData&) override { return {}; }
    bool updateVertices(size_t, std::span<const ou::scene::Vertex>) override { return false; }
    void resize(ou::render::Viewport) override {}
    void drawFrame(const ou::render::Camera&, ou::render::Viewport, const ou::render::FrameOptions&) override {}
    ou::render::Image readBackbuffer(ou::render::Viewport) override { return {}; }
    void present() override {}
    bool setVsync(bool) override { return true; }
    std::string description() const override { return vulkan ? "fake vulkan" : "fake gl"; }
    void shutdown() override {}
};

} // namespace

// "auto" prefers Vulkan and falls back to OpenGL with a note; explicit backends never fall back.
TEST(RendererSelect, AutoFallsBackToOpenGl) {
    auto pick = [](std::string_view name, bool vulkanOk, bool glOk, std::string& chosen, std::string& note, std::string& err) {
        return ou::render::selectRenderer(
            name, [&](bool vk) { return std::make_unique<FakeRenderer>(vk, vk ? vulkanOk : glOk); }, chosen, note, err);
    };
    std::string chosen, note, err;
    auto r = pick("auto", true, true, chosen, note, err);
    ASSERT_TRUE(r);
    EXPECT_EQ(chosen, "vulkan");
    EXPECT_EQ(r->description(), "fake vulkan");
    EXPECT_TRUE(note.empty());
    r = pick("auto", false, true, chosen, note, err);
    ASSERT_TRUE(r);
    EXPECT_EQ(chosen, "opengl");
    EXPECT_EQ(note, "Vulkan unavailable (no Vulkan 1.3 device), falling back to OpenGL");
    EXPECT_TRUE(err.empty());
    note.clear();
    EXPECT_FALSE(pick("vulkan", false, true, chosen, note, err)); // explicit: no fallback
    EXPECT_EQ(err, "no Vulkan 1.3 device");
    EXPECT_TRUE(note.empty());
    err.clear();
    r = pick("opengl", true, true, chosen, note, err);
    ASSERT_TRUE(r);
    EXPECT_EQ(chosen, "opengl");
    EXPECT_FALSE(pick("auto", false, false, chosen, note, err));
    EXPECT_FALSE(pick("d3d9", true, true, chosen, note, err));
    EXPECT_EQ(err, "renderer must be auto, opengl or vulkan, got d3d9");
}

// Ray tracing on a capable device: acceleration structures for the scene (built by the first RT
// frame), counts, the merged static BLAS and a mesh leaving it on its first updateVertices (TLAS rebuild), BLAS refit
// of skinned meshes + TLAS update, shading close to the shadow map, RT off unchanged, teardown. Skips without RT support.
TEST(VulkanRenderer, RayTracingBuildsUpdatesAndTearsDown) {
    using ou::render::RayTracing;
    VkWindow v;
    if (!v.skip.empty()) GTEST_SKIP() << v.skip;
    if (!v.r->rtStatus().supported) GTEST_SKIP() << "no ray tracing: " << v.r->rtStatus().reason;

    ou::scene::SceneData scene = ou::scene::makePlaceholderScene();
    uint32_t traced = 0, geometries = 0; // meshes / submeshes that cast shadows (Opaque + Alpha blends)
    for (const auto& m : scene.meshes) {
        uint32_t n = 0;
        for (const auto& s : m.submeshes) n += s.blend == ou::scene::Blend::Opaque || s.blend == ou::scene::Blend::Alpha;
        traced += n > 0, geometries += n;
    }
    ASSERT_GT(traced, 0u);
    EXPECT_TRUE(v.r->upload(scene).empty());
    const ou::render::Image offBefore = v.frame(enhanced());
    EXPECT_EQ(v.r->rtStatus().blas, 0u); // nothing built while RT is off
    EXPECT_FALSE(v.r->rtStatus().active);

    const ou::render::Image rt = v.frame(enhanced(RayTracing::Shadows));
    auto st = v.r->rtStatus();
    EXPECT_TRUE(st.active);
    // The placeholder's traced submeshes are all solid casters (one ray mask): every mesh gets its own BLAS, and the
    // static ones (all, before any updateVertices) are traced through one merged BLAS, the only TLAS instance.
    EXPECT_EQ(st.blas, traced + 1);
    EXPECT_EQ(st.instances, 1u);
    EXPECT_EQ(st.geometries, 2 * geometries);
    EXPECT_EQ(st.waterInstances, 0u);
    EXPECT_EQ(st.tlasBuilds, 1u);
    EXPECT_EQ(st.tlasUpdates, 0u);
    // RT shadows darken like the shadow map and land close to it (same sun, same casters).
    ou::render::FrameOptions unshadowed = enhanced();
    unshadowed.shadows = false;
    EXPECT_LT(brightness(rt), brightness(v.frame(unshadowed)));
    EXPECT_LT(meanAbsDiff(rt, offBefore), 3.0);
    EXPECT_GT(meanAbsDiff(rt, offBefore), 0.0); // but traced, not the map

    // A static mesh changed: its own BLAS rebuilt, the merged one rebuilt without it, the TLAS rebuilt with its instance.
    EXPECT_TRUE(v.r->updateVertices(0, scene.meshes[0].vertices));
    EXPECT_EQ(v.r->rtStatus().blasRebuilds, 0u); // staged; nothing submitted yet
    v.frame(enhanced(RayTracing::Full));
    EXPECT_EQ(v.r->rtStatus().blasRebuilds, 2u);
    EXPECT_EQ(v.r->rtStatus().tlasBuilds, 2u);
    EXPECT_EQ(v.r->rtStatus().instances, 2u);
    // Moved again: only its own BLAS, then a TLAS update.
    EXPECT_TRUE(v.r->updateVertices(0, scene.meshes[0].vertices));
    v.frame(enhanced(RayTracing::Full));
    EXPECT_EQ(v.r->rtStatus().blasRebuilds, 3u);
    EXPECT_EQ(v.r->rtStatus().tlasUpdates, 1u);
    v.frame(enhanced(RayTracing::Full));
    EXPECT_EQ(v.r->rtStatus().tlasUpdates, 1u); // clean TLAS: no update

    // RT off again renders exactly what it did before any acceleration structure existed.
    EXPECT_EQ(meanAbsDiff(v.frame(enhanced()), offBefore), 0.0);

    // Skinned mesh: refit (ALLOW_UPDATE BLAS) instead of rebuild.
    scene.meshes[0].skin.assign(scene.meshes[0].vertices.size(), {});
    EXPECT_TRUE(v.r->upload(scene).empty());
    v.frame(enhanced(RayTracing::Shadows));
    EXPECT_EQ(v.r->rtStatus().tlasBuilds, 3u);
    std::vector<ou::scene::Vertex> moved = scene.meshes[0].vertices;
    for (auto& p : moved) p.position[2] += 50.0f;
    EXPECT_TRUE(v.r->updateVertices(0, scene.meshes[0].vertices));
    EXPECT_TRUE(v.r->updateVertices(0, moved)); // overwrites the staged pose: one copy, one refit
    v.frame(enhanced(RayTracing::Shadows));
    EXPECT_EQ(v.r->rtStatus().blasRefits, 1u);
    EXPECT_EQ(v.r->rtStatus().blasRebuilds, 3u);
    EXPECT_EQ(v.r->rtStatus().tlasUpdates, 2u);
}

// OU_VK_NO_RT=1 simulates a device without ray queries: RT requests fall back to the shadow map.
TEST(VulkanRenderer, RayTracingFallsBackToShadowMap) {
    using ou::render::RayTracing;
    setenv("OU_VK_NO_RT", "1", 1);
    VkWindow v;
    unsetenv("OU_VK_NO_RT");
    if (!v.skip.empty()) GTEST_SKIP() << v.skip;
    const auto st = v.r->rtStatus();
    EXPECT_FALSE(st.supported);
    EXPECT_EQ(st.reason, "OU_VK_NO_RT=1");
    EXPECT_TRUE(v.r->upload(ou::scene::makePlaceholderScene()).empty());
    const ou::render::Image map = v.frame(enhanced());
    const ou::render::Image fallback = v.frame(enhanced(RayTracing::Full));
    EXPECT_FALSE(v.r->rtStatus().active);
    EXPECT_GT(nonClearPixels(fallback), fallback.rgb.size() / 30);
    EXPECT_EQ(meanAbsDiff(fallback, map), 0.0);
    ou::render::FrameOptions noShadows = enhanced(RayTracing::Shadows);
    noShadows.shadows = false; // a requested RT mode still shadows (via the map)
    EXPECT_EQ(meanAbsDiff(v.frame(noShadows), map), 0.0);
}

TEST(VulkanRenderer, ResizeRecreatesSwapchain) {
    if (!SDL_Init(SDL_INIT_VIDEO)) GTEST_SKIP() << "no display: " << SDL_GetError();
    auto r = ou::render::createVulkanRenderer();
    std::string err;
    if (!r->preWindowSetup(err)) {
        SDL_Quit();
        GTEST_SKIP() << "Vulkan unavailable: " << err;
    }
    SDL_Window* w = SDL_CreateWindow("ou vk resize test", 320, 240, SDL_WindowFlags(r->windowFlags()) | SDL_WINDOW_RESIZABLE);
    ASSERT_NE(w, nullptr) << SDL_GetError();
    ASSERT_TRUE(r->init(w, err)) << err;
    EXPECT_TRUE(r->upload(ou::scene::makePlaceholderScene()).empty()); // replaced below: exercises re-upload

    const ou::scene::SceneData scene = ou::scene::makePlaceholderScene();
    EXPECT_TRUE(r->upload(scene).empty());
    EXPECT_TRUE(r->updateVertices(0, scene.meshes[0].vertices));
    EXPECT_FALSE(r->updateVertices(0, std::span(scene.meshes[0].vertices).first(1))); // count mismatch
    EXPECT_FALSE(r->updateVertices(scene.meshes.size(), scene.meshes[0].vertices));    // no such mesh
    ou::render::Camera cam;
    cam.position = {-1100.0f, -1300.0f, 700.0f};
    cam.lookAt({0.0f, 0.0f, 100.0f});
    auto frame = [&](const ou::render::FrameOptions& options = {}) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) // same wiring as ou-viewer
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) r->resize({e.window.data1, e.window.data2});
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        r->drawFrame(cam, {pw, ph}, options);
        ou::render::Image img = r->readBackbuffer({pw, ph});
        r->present();
        EXPECT_EQ(img.width, pw);
        EXPECT_EQ(img.height, ph);
        return img;
    };
    ou::render::Image a = frame();
    ASSERT_TRUE(SDL_SetWindowSize(w, 640, 200));
    SDL_SyncWindow(w);
    ou::render::Image b = frame();
    b = frame(); // and once more on the new swapchain
    EXPECT_NE(a.width, b.width);
    EXPECT_GT(b.width, b.height); // 640x200 logical, any HiDPI scale
    EXPECT_GT(nonClearPixels(a), a.rgb.size() / 30);
    EXPECT_GT(nonClearPixels(b), b.rgb.size() / 30);

    // Debug lines: pure red segments floating above the ground must show up (and nothing red without them).
    std::vector<ou::render::DebugLine> lines;
    for (int i = -5; i <= 5; ++i)
        lines.push_back({{-600.0f, i * 100.0f, 300.0f}, {600.0f, i * 100.0f, 300.0f}, 0xFF0000FFu});
    auto red = [](const ou::render::Image& img) {
        size_t n = 0;
        for (size_t i = 0; i + 2 < img.rgb.size(); i += 3) n += img.rgb[i] > 250 && img.rgb[i + 1] < 5 && img.rgb[i + 2] < 5;
        return n;
    };
    ou::render::FrameOptions withLines;
    withLines.debugLines = lines;
    EXPECT_EQ(red(frame()), 0u);
    EXPECT_GT(red(frame(withLines)), size_t(b.width)); // 11 lines, each a few hundred pixels long

    // Enhanced settings rebuild targets/pipelines (4x MSAA, shadow map, FXAA, HDR) and back again.
    ou::render::FrameOptions enhanced = withLines;
    enhanced.msaa = 4, enhanced.shadows = true, enhanced.fxaa = true, enhanced.hdr = true;
    ou::render::Image e = frame(enhanced);
    EXPECT_GT(nonClearPixels(e), e.rgb.size() / 30);
    EXPECT_GT(red(frame(withLines)), size_t(b.width));
    // Shadows only darken: the plain frame is at least as bright overall as the shadowed one.
    ou::render::FrameOptions shadowed;
    shadowed.shadows = true;
    auto sum = [](const ou::render::Image& img) { size_t s = 0; for (uint8_t v : img.rgb) s += v; return s; };
    const size_t lit = sum(frame()), dark = sum(frame(shadowed));
    EXPECT_LT(dark, lit);

    r->shutdown();
    SDL_DestroyWindow(w);
    SDL_Quit();
}
