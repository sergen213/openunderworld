// FMOD Ex DSP effects that TRU's SoundPlex Effect nodes, sound categories and the reverb environment set, as small
// stdlib processors on interleaved stereo float. Layouts and evidence: docs/formats/AUDIO.md §9.15.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ou::audio {

// cdc effect type (u16 at Effect data +4) -> FMOD_DSP_TYPE (tru.exe 0x587620 -> System::createDSPByType).
enum class DspType : uint8_t {
    None = 0, Chorus, Compressor, Distortion, Echo, ParamEq, Flange, HighPass, LowPass, Normalize, PitchShift, Reverb,
};
// One DSP with its parameters in FMOD's parameter-index order, already converted the way 0x582EC0 converts the data
// (seconds -> ms, the compressor makeup x100, bytes -> float).
struct Effect {
    DspType type = DspType::None;
    std::array<float, 8> param{};
};

class Dsp {
public:
    virtual ~Dsp() = default;
    virtual void process(float* lr, size_t frames) = 0; // interleaved stereo, in place
};
// Lowpass / highpass (param 0 cutoff Hz, 1 resonance = Q) and parametric EQ (0 centre Hz, 1 bandwidth octaves, 2 linear
// gain) as RBJ biquads. Null for the other types: retail data uses none of them (AUDIO.md §9.15).
std::unique_ptr<Dsp> makeDsp(const Effect& e, uint32_t rate);

// FMOD_DSP_TYPE_SFXREVERB's 13 I3DL2 parameters (FMOD Ex 4.x order): 0 dry level mB, 1 room mB, 2 room HF mB,
// 3 room rolloff, 4 decay time s, 5 decay HF ratio, 6 reflections level mB, 7 reflections delay s, 8 reverb level mB,
// 9 reverb delay s, 10 diffusion %, 11 density %, 12 HF reference Hz. tru.exe 0x582270 sets all 13 each frame from the
// environment block (globalsoundinfo +0x2C, overwritten by the script `reverb` native 0x582B70).
using SfxReverbProps = std::array<float, 13>;
// globalsoundinfo's block: room -10000 mB, i.e. no reverb until a soundvolume sets one.
inline constexpr SfxReverbProps kReverbOff{-10000, -10000, 0, 10, 1, 0.5f, -10000, 0.02f, 0, 0.04f, 100, 100, 5000};

// Freeverb-style tank (8 combs + 4 allpasses per side) tuned from the I3DL2 decay time, HF ratio, room HF, levels and
// delays [I: FMOD's SFX reverb algorithm is not reproduced]. Wet output only: tru.exe forces the dry level to -10000 mB
// (0x582200) and runs the unit beside the dry mix.
class SfxReverb {
public:
    explicit SfxReverb(uint32_t rate);
    void set(const SfxReverbProps& p);
    const SfxReverbProps& props() const { return props_; }
    bool active() const { return late_ > 0 || early_ > 0; }
    void process(const float* in, float* out, size_t frames); // stereo send in -> wet stereo added to out
private:
    struct Comb { std::vector<float> buf; size_t i = 0; float store = 0, feedback = 0, damp = 0; };
    struct AllPass { std::vector<float> buf; size_t i = 0; };
    uint32_t rate_;
    SfxReverbProps props_ = kReverbOff;
    std::array<Comb, 16> comb_;   // 0-7 left, 8-15 right
    std::array<AllPass, 8> ap_;   // 0-3 left, 4-7 right
    std::vector<float> pre_;      // mono pre-delay line
    size_t preI_ = 0, earlyTap_ = 1, lateTap_ = 1;
    float late_ = 0, early_ = 0, hfCoef_ = 1, hfState_ = 0, allpass_ = 0.5f;
};

} // namespace ou::audio
