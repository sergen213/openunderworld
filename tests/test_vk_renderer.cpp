// Vulkan backend against a real window: swapchain recreation on an SDL resize. Skips without a
// display or a usable Vulkan 1.3 device.
#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include "render/Renderer.hpp"
#include "scene/Placeholder.hpp"

namespace {

size_t nonClearPixels(const ou::render::Image& img) {
    size_t n = 0;
    for (size_t i = 0; i + 2 < img.rgb.size(); i += 3) // clear colour is (20, 23, 28)
        n += std::abs(img.rgb[i] - 20) + std::abs(img.rgb[i + 1] - 23) + std::abs(img.rgb[i + 2] - 28) > 6;
    return n;
}

} // namespace

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

    r->shutdown();
    SDL_DestroyWindow(w);
    SDL_Quit();
}
