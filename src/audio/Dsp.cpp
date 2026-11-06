// FMOD Ex DSP stand-ins. Evidence: docs/formats/AUDIO.md §9.15.
#include "audio/Dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ou::audio {
namespace {

// RBJ audio-EQ-cookbook biquad, one state per channel.
class Biquad final : public Dsp {
public:
    Biquad(DspType type, const Effect& e, uint32_t rate) {
        const float pi = std::numbers::pi_v<float>, f = std::clamp(e.param[0], 10.0f, 0.49f * float(rate));
        const float w = 2 * pi * f / float(rate), c = std::cos(w), s = std::sin(w);
        float b0, b1, b2, a0, a1, a2;
        if (type == DspType::ParamEq) {
            const float A = std::sqrt(std::max(e.param[2], 1e-3f)); // centre gain = param 2 (linear)
            const float alpha = s * std::sinh(std::numbers::ln2_v<float> / 2 * std::max(e.param[1], 0.05f) * w / s);
            b0 = 1 + alpha * A, b1 = -2 * c, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * c, a2 = 1 - alpha / A;
        } else {
            const float alpha = s / (2 * std::max(e.param[1], 0.1f)); // FMOD resonance 1..10 as Q [I]
            const float k = type == DspType::LowPass ? 1 - c : 1 + c;
            b0 = k / 2, b1 = type == DspType::LowPass ? k : -k, b2 = k / 2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
        }
        b_ = {b0 / a0, b1 / a0, b2 / a0}, a_ = {a1 / a0, a2 / a0};
    }
    void process(float* lr, size_t frames) override {
        for (size_t i = 0; i < frames; ++i)
            for (size_t ch = 0; ch < 2; ++ch) {
                float& x = lr[2 * i + ch];
                auto& z = z_[ch];
                const float y = b_[0] * x + z[0];
                z[0] = b_[1] * x - a_[0] * y + z[1];
                z[1] = b_[2] * x - a_[1] * y;
                x = y;
            }
    }
private:
    std::array<float, 3> b_{};
    std::array<float, 2> a_{};
    std::array<std::array<float, 2>, 2> z_{};
};

float mB(float v) { return v <= -10000 ? 0 : std::pow(10.0f, v / 2000); } // millibels -> linear

} // namespace

std::unique_ptr<Dsp> makeDsp(const Effect& e, uint32_t rate) {
    switch (e.type) {
    case DspType::LowPass: case DspType::HighPass: case DspType::ParamEq: return std::make_unique<Biquad>(e.type, e, rate);
    default: return nullptr;
    }
}

SfxReverb::SfxReverb(uint32_t rate) : rate_(rate) {
    // Freeverb tunings at 44.1 kHz; the right side is 23 samples longer.
    static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, kAllPass[4] = {556, 441, 341, 225};
    const double k = rate / 44100.0;
    for (size_t i = 0; i < 16; ++i) comb_[i].buf.assign(size_t((kComb[i % 8] + (i >= 8 ? 23 : 0)) * k) + 1, 0);
    for (size_t i = 0; i < 8; ++i) ap_[i].buf.assign(size_t((kAllPass[i % 4] + (i >= 4 ? 23 : 0)) * k) + 1, 0);
    pre_.assign(size_t(0.4 * rate) + 2, 0); // I3DL2 maxima: reflections 0.3 s + reverb delay 0.1 s
    set(kReverbOff);
}

void SfxReverb::set(const SfxReverbProps& p) {
    props_ = p;
    const float room = mB(p[1]);
    late_ = room * mB(p[8]), early_ = room * mB(p[6]);
    const float t60 = std::max(p[4], 0.1f), hf = std::clamp(p[5], 0.1f, 2.0f);
    for (auto& c : comb_) { // each comb's loop loses 60 dB in decay time (HF: decay time x HF ratio)
        const float d = float(c.buf.size()) / float(rate_);
        c.feedback = std::pow(10.0f, -3 * d / t60);
        const float high = std::pow(10.0f, -3 * d / (t60 * hf));
        c.damp = std::clamp(1 - high / c.feedback, 0.0f, 0.95f);
    }
    // Room HF: a one-pole lowpass on the input whose gain at the HF reference is mB(room HF).
    const float g = std::clamp(mB(p[2]), 1e-3f, 1.0f), ref = std::clamp(p[12], 1000.0f, 20000.0f);
    const float fc = g >= 0.999f ? 0.49f * float(rate_) : ref / std::sqrt(1 / (g * g) - 1);
    hfCoef_ = 1 - std::exp(-2 * std::numbers::pi_v<float> * std::min(fc, 0.49f * float(rate_)) / float(rate_));
    allpass_ = 0.5f * std::clamp(p[10], 0.0f, 100.0f) / 100;
    earlyTap_ = std::clamp(size_t(std::max(p[7], 0.0f) * float(rate_)), size_t(1), pre_.size() - 1);
    lateTap_ = std::clamp(size_t(std::max(p[7] + p[9], 0.0f) * float(rate_)), size_t(1), pre_.size() - 1);
}

void SfxReverb::process(const float* in, float* out, size_t frames) {
    if (!active()) return;
    const size_t n = pre_.size();
    for (size_t i = 0; i < frames; ++i) {
        hfState_ += hfCoef_ * ((in[2 * i] + in[2 * i + 1]) * 0.5f - hfState_);
        pre_[preI_] = hfState_;
        const float early = pre_[(preI_ + n - earlyTap_) % n], x = pre_[(preI_ + n - lateTap_) % n] * 0.015f * 2;
        preI_ = (preI_ + 1) % n;
        float side[2] = {0, 0};
        for (size_t k = 0; k < 16; ++k) {
            Comb& c = comb_[k];
            const float y = c.buf[c.i];
            c.store = y * (1 - c.damp) + c.store * c.damp;
            c.buf[c.i] = x + c.store * c.feedback;
            c.i = c.i + 1 == c.buf.size() ? 0 : c.i + 1;
            side[k / 8] += y;
        }
        for (size_t k = 0; k < 8; ++k) {
            AllPass& a = ap_[k];
            float& s = side[k / 4];
            const float b = a.buf[a.i];
            a.buf[a.i] = s + b * allpass_;
            s = b - s;
            a.i = a.i + 1 == a.buf.size() ? 0 : a.i + 1;
        }
        out[2 * i] += side[0] * 3 * late_ + early * early_;
        out[2 * i + 1] += side[1] * 3 * late_ + early * early_;
    }
}

} // namespace ou::audio
