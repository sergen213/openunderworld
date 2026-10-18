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
    auto frame = [&] {
        SDL_Event e;
        while (SDL_PollEvent(&e)) // same wiring as ou-viewer
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) r->resize({e.window.data1, e.window.data2});
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        r->drawFrame(cam, {pw, ph}, {});
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

    r->shutdown();
    SDL_DestroyWindow(w);
    SDL_Quit();
}
