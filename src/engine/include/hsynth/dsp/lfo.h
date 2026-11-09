// Low-frequency oscillator evaluated at control rate.
#pragma once

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

class Lfo {
public:
    void reset(float phase = 0.0f) { phase_ = phase; held_ = 0.0f; }

    // Advance by dt seconds at rate Hz, return value in [-1, 1].
    float advance(LfoWave wave, float rateHz, float dt, Rng& rng) {
        phase_ += rateHz * dt;
        if (phase_ >= 1.0f) {
            phase_ -= fastFloor(phase_);
            held_ = rng.bipolar();
        }
        return value(wave);
    }

    float value(LfoWave wave) const {
        switch (wave) {
            case LfoWave::Sine: return std::sin(kTwoPi * phase_);
            case LfoWave::Triangle: return 1.0f - 4.0f * std::fabs(phase_ - 0.5f);
            case LfoWave::Saw: return 2.0f * phase_ - 1.0f;
            case LfoWave::Square: return phase_ < 0.5f ? 1.0f : -1.0f;
            case LfoWave::SampleHold: return held_;
            default: return 0.0f;
        }
    }

private:
    float phase_ = 0.0f;
    float held_ = 0.0f;
};

}  // namespace hsynth
