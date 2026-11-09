// Master safety limiter (the effect rack lives in hsynth/fx/).
#pragma once

#include "hsynth/common.h"

namespace hsynth {

// Peak limiter without look-ahead (no added latency): the gain falls over
// ~0.5 ms instead of in one sample (an instant gain step puts a step into the
// waveform, audible as a click), and the overshoot during that ramp goes into
// a soft clipper that is continuous at the threshold and never exceeds 1.0.
class Limiter {
public:
    bool enabled = true;
    float threshold = 0.89f;  // about -1 dBFS
    void prepare(float sr, int) {
        release_ = std::exp(-1.0f / (0.12f * sr));
        attack_ = std::exp(-1.0f / (0.0005f * sr));
        gain_ = 1.0f;
    }
    void reset() { gain_ = 1.0f; }
    void process(float* L, float* R, int n) {
        for (int i = 0; i < n; ++i) {
            if (enabled) {
                const float peak = std::max(std::fabs(L[i]), std::fabs(R[i]));
                const float target = peak > threshold ? threshold / peak : 1.0f;
                gain_ = target + (gain_ - target) * (target < gain_ ? attack_ : release_);
                L[i] *= gain_;
                R[i] *= gain_;
            }
            L[i] = softClip(L[i]);
            R[i] = softClip(R[i]);
        }
    }
    float gainReduction() const { return gain_; }

private:
    // identity below the threshold, tanh knee from there up to 1.0
    float softClip(float x) const {
        const float a = std::fabs(x);
        if (a <= threshold) return x;
        const float room = 1.0f - threshold;
        const float y = threshold + room * fastTanh((a - threshold) / room);
        return x < 0.0f ? -y : y;
    }
    float gain_ = 1.0f, release_ = 0.999f, attack_ = 0.9f;
};

}  // namespace hsynth
