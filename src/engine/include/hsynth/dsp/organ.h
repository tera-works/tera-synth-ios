// Tonewheel organ voice (layer type "organ"): nine drawbars with the classic
// footages, top-octave foldback, single-trigger-style percussion (2nd / 3rd
// harmonic), key click and tonewheel leakage. Pair with the "scanner" vibrato
// / chorus block and the rotary speaker for the full instrument.
#pragma once

#include <cmath>
#include <cstdint>

#include "hsynth/common.h"
#include "hsynth/dsp/oscillators.h"
#include "hsynth/params.h"

namespace hsynth {

class OrganVoice {
public:
    void setSampleRate(float sr) { sr_ = sr; }

    void noteOn(int key, Rng& rng) {
        key_ = key;
        for (auto& p : ph_) p = rng.next();  // tonewheels spin freely: random phases
        leakPh_[0] = rng.next();
        leakPh_[1] = rng.next();
        percPh_ = ph_[3];
        percEnv_ = 1.0f;
        clickEnv_ = 1.0f;
        clickGain_ = 1.0f;
    }
    void noteOff() {
        clickEnv_ = 1.0f;  // key-up contact bounce: a smaller click
        clickGain_ = 0.4f;
    }

    void control(const float* p, int l) {
        static const float kRatio[9] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
        float sum = 0.0f;
        for (int k = 0; k < 9; ++k) {
            const float v = p[P::layer(l, P::OrgDrawbar1 + k)];
            gain_[k] = v <= 0.01f ? 0.0f : std::pow(10.0f, (v - 8.0f) * 3.0f / 20.0f);  // 3 dB per drawbar step
            sum += gain_[k];
            ratio_[k] = kRatio[k];
        }
        norm_ = 0.8f / std::max(1.0f, std::sqrt(sum * 2.0f));
        perc_ = (int)p[P::layer(l, P::OrgPerc)];
        percLevel_ = p[P::layer(l, P::OrgPercLevel)];
        const float percT = (int)p[P::layer(l, P::OrgPercDecay)] == 0 ? 0.25f : 1.0f;  // -60 dB time
        percCoef_ = std::exp(-6.9f / (percT * sr_));
        click_ = p[P::layer(l, P::OrgClick)];
        leak_ = p[P::layer(l, P::OrgLeak)];
        clickCoef_ = std::exp(-1.0f / (0.0015f * sr_));
    }

    // dt: frequency of the 8' drawbar / sample rate. Adds into out.
    void render(float* out, int n, float dt, Rng& rng) {
        constexpr float kTwo32 = 4294967296.0f;
        uint32_t inc[9];
        const float nyq = 0.45f;
        for (int k = 0; k < 9; ++k) {
            float r = ratio_[k];
            // the top octave of tonewheels repeats (foldback above ~5.9 kHz)
            while (dt * r * sr_ > 5900.0f && r > 0.5f) r *= 0.5f;
            inc[k] = (uint32_t)(std::min(nyq, dt * r) * kTwo32);
        }
        const uint32_t percInc = (uint32_t)(std::min(nyq, dt * (perc_ == 2 ? 3.0f : 2.0f)) * kTwo32);
        const uint32_t leakInc0 = (uint32_t)(std::min(nyq, dt * 0.749f) * kTwo32);
        const uint32_t leakInc1 = (uint32_t)(std::min(nyq, dt * 2.997f) * kTwo32);
        for (int i = 0; i < n; ++i) {
            float s = 0.0f;
            for (int k = 0; k < 9; ++k) {
                if (gain_[k] == 0.0f) { ph_[k] += inc[k]; continue; }
                // tonewheels are not pure sines: a trace of the 3rd harmonic
                s += gain_[k] * (sineLookupU32(ph_[k]) + 0.025f * sineLookupU32(ph_[k] * 3u));
                ph_[k] += inc[k];
            }
            if (perc_ > 0 && percEnv_ > 1e-4f) {
                s += percLevel_ * 1.6f * percEnv_ * sineLookupU32(percPh_);
                percEnv_ *= percCoef_;
            }
            percPh_ += percInc;
            if (leak_ > 0.0f) {
                s += leak_ * 0.02f * (sineLookupU32(leakPh_[0]) + sineLookupU32(leakPh_[1]));
                leakPh_[0] += leakInc0;
                leakPh_[1] += leakInc1;
            }
            if (clickEnv_ > 1e-4f) {
                s += click_ * clickGain_ * 0.6f * clickEnv_ * rng.bipolar();
                clickEnv_ *= clickCoef_;
            }
            out[i] += s * norm_;
        }
    }

private:
    float sr_ = 48000.0f;
    int key_ = 60;
    uint32_t ph_[9] = {}, leakPh_[2] = {}, percPh_ = 0;
    float gain_[9] = {}, ratio_[9] = {};
    float norm_ = 0.2f;
    int perc_ = 0;
    float percLevel_ = 0.6f, percEnv_ = 0.0f, percCoef_ = 0.999f;
    float click_ = 0.3f, clickEnv_ = 0.0f, clickGain_ = 1.0f, clickCoef_ = 0.99f;
    float leak_ = 0.2f;
};

}  // namespace hsynth
