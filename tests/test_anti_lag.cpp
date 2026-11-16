// AMD Anti-Lag marker sequencing (src/render/vk/AntiLag.hpp) against a model of Mesa's VK_LAYER_MESA_anti_lag frame
// ring (mesa src/vulkan/anti-lag-layer, 26.2): while ON every vkQueuePresentKHR takes one of 8 frame slots, an INPUT
// update frees the evaluated ones (all but the newest, here with the GPU always done), OFF frees all. A present into a
// full ring is the SIGSEGV seen during cinematics.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "render/vk/AntiLag.hpp"

namespace {

using ou::render::AntiLagMarkers;
using Call = AntiLagMarkers::Call;

struct MesaLayer {
    static constexpr int kFrames = 8; // MAX_FRAMES
    bool enabled = false, crashed = false;
    int frames = 0;
    int presentsSinceInput = 0, maxPresentsPerInput = 0;
    std::vector<std::pair<Call, uint64_t>> calls;

    void update(Call c, uint64_t index) { // vkAntiLagUpdateAMD
        calls.emplace_back(c, index);
        if (c == Call::Off) {
            enabled = false, frames = 0;
            return;
        }
        enabled = true;
        if (c == Call::Present) return;
        presentsSinceInput = 0;
        // get_wait_time: the oldest frame is evaluated when presented (only the newest is still submitting); after the
        // first one, stop once at most 2 remain.
        while (frames >= 2) {
            --frames;
            if (frames <= 2) break;
        }
    }
    void queuePresent() { // vkQueuePresentKHR
        if (!enabled) return;
        if (frames == kFrames) crashed = true; // ringbuffer_alloc -> NULL, reset_frame(NULL)
        else ++frames;
        maxPresentsPerInput = std::max(maxPresentsPerInput, ++presentsSinceInput);
    }
    auto emit() {
        return [this](Call c, uint64_t i) { update(c, i); };
    }
};

// One renderer frame: present() marks, then the image(s) go to vkQueuePresentKHR.
void present(AntiLagMarkers& m, MesaLayer& layer, bool generated = false) {
    m.beforePresent(generated, layer.emit());
    if (generated) layer.queuePresent();
    layer.queuePresent();
}

TEST(AntiLag, ModelCrashesOnPresentsWithoutInput) { // the 0.49.1 sequence: one INPUT, then a cinematic's presents
    MesaLayer layer;
    layer.update(Call::Input, 1);
    for (int i = 0; i < 20; ++i) layer.queuePresent();
    EXPECT_TRUE(layer.crashed);
}

TEST(AntiLag, MainLoopPairsInputAndPresent) {
    AntiLagMarkers m;
    MesaLayer layer;
    for (int i = 0; i < 100; ++i) {
        m.beginInput(true, false, layer.emit());
        present(m, layer);
    }
    EXPECT_FALSE(layer.crashed);
    ASSERT_EQ(layer.calls.size(), 200u);
    for (size_t i = 0; i < layer.calls.size(); i += 2) {
        EXPECT_EQ(layer.calls[i], std::make_pair(Call::Input, uint64_t(i / 2 + 1)));
        EXPECT_EQ(layer.calls[i + 1], std::make_pair(Call::Present, uint64_t(i / 2 + 1)));
    }
}

TEST(AntiLag, NestedPresentLoopGetsAnInputPerPresent) { // main loop INPUT, then a cinematic / loading screen loop
    AntiLagMarkers m;
    MesaLayer layer;
    m.beginInput(true, false, layer.emit());
    for (int i = 0; i < 600; ++i) present(m, layer);
    m.beginInput(true, false, layer.emit()); // back in the main loop
    present(m, layer);
    EXPECT_FALSE(layer.crashed);
    EXPECT_EQ(layer.maxPresentsPerInput, 1);
    EXPECT_EQ(layer.calls.size(), 2u * 601u);
}

TEST(AntiLag, NoMarkersWhileOff) {
    AntiLagMarkers m;
    MesaLayer layer;
    for (int i = 0; i < 20; ++i) {
        m.beginInput(false, false, layer.emit());
        present(m, layer);
    }
    EXPECT_TRUE(layer.calls.empty());
    m.beginInput(true, false, layer.emit());
    present(m, layer);
    m.beginInput(false, false, layer.emit()); // switched off: one OFF, then silence
    for (int i = 0; i < 20; ++i) present(m, layer);
    m.beginInput(false, false, layer.emit());
    ASSERT_EQ(layer.calls.size(), 3u);
    EXPECT_EQ(layer.calls.back().first, Call::Off);
    EXPECT_FALSE(m.on());
}

TEST(AntiLag, FrameGenerationExcluded) { // two presents per frame: the layer would count two frames per INPUT
    AntiLagMarkers m;
    MesaLayer layer;
    m.beginInput(true, false, layer.emit());
    present(m, layer, true); // frame generation switched on by this frame's settings
    EXPECT_FALSE(layer.enabled);
    EXPECT_EQ(layer.calls.back().first, Call::Off);
    for (int i = 0; i < 100; ++i) {
        m.beginInput(true, true, layer.emit());
        present(m, layer, true);
    }
    EXPECT_FALSE(layer.crashed);
    EXPECT_EQ(layer.calls.size(), 2u); // INPUT, OFF
    m.beginInput(true, false, layer.emit()); // frame generation off again: Anti-Lag back on
    present(m, layer);
    EXPECT_TRUE(m.on());
    EXPECT_EQ(layer.calls.size(), 4u);
}

} // namespace
