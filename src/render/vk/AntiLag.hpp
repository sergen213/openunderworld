// AMD Anti-Lag marker sequencing (docs/RENDERING.md "Anti-Lag"), kept free of Vulkan so it can be unit tested.
//
// Mesa's VK_LAYER_MESA_anti_lag does not track frames by the app's markers: while it is ON, every vkQueuePresentKHR
// takes one slot of an 8-entry frame ring (anti_lag_QueuePresentKHR), and only the INPUT update (get_wait_time) frees
// slots. Presenting more than ~7 times without an INPUT makes ringbuffer_alloc return NULL and the layer writes through
// it (SIGSEGV in anti_lag_QueuePresentKHR, Mesa 26.2). So every present must follow its own INPUT:
// - present loops nested inside one main-loop iteration (cinematics, the loading screen) never call beginInput(); a
//   present without one gets an INPUT of its own right before it;
// - FSR frame generation presents twice per frame (the generated image from the present thread), which the layer
//   would count as two frames with one INPUT, so Anti-Lag stays OFF while frame generation is on.
#pragma once

#include <cstdint>
#include <utility>

namespace ou::render {

class AntiLagMarkers {
public:
    enum class Call { Off, Input, Present }; // vkAntiLagUpdateAMD: mode OFF / ON + stage INPUT / ON + stage PRESENT

    // Main loop, before input is read. `wanted` = the setting, `frameGeneration` = generated images being presented.
    template <class Emit>
    void beginInput(bool wanted, bool frameGeneration, Emit&& emit) {
        if (!wanted || frameGeneration) {
            off(emit);
            return;
        }
        emit(Call::Input, ++frameIndex_);
        on_ = marked_ = true;
    }

    // Right before the frame's image(s) are presented (or queued for the present thread); `generated` = a generated
    // image goes out before the frame's own.
    template <class Emit>
    void beforePresent(bool generated, Emit&& emit) {
        if (!on_) return;
        if (generated) {
            off(emit);
            return;
        }
        if (!std::exchange(marked_, false)) emit(Call::Input, ++frameIndex_);
        emit(Call::Present, frameIndex_);
    }

    bool on() const { return on_; }
    void reset() { on_ = marked_ = false; } // new device: the layer starts OFF

private:
    template <class Emit>
    void off(Emit& emit) {
        marked_ = false;
        if (std::exchange(on_, false)) emit(Call::Off, frameIndex_);
    }

    bool on_ = false;     // ON sent since the last OFF
    bool marked_ = false; // INPUT sent, its present still due
    uint64_t frameIndex_ = 0;
};

} // namespace ou::render
