// Phase-modulation (DX-style FM) operator stack: up to 8 operators.
//
// Algorithms are data: each operator lists a bitmask of the operators that
// modulate it, plus a carrier mask. Modulators always have a higher index than
// the operator they modulate, so evaluation runs from the last operator down.
// Each algorithm states how many operators it uses (the others are not
// computed). Algorithms 1-16 are the original 4- and 6-operator set (indices
// kept for presets); 17-88 are 8-operator algorithms written in a small
// notation (fmAlgorithmSpec) and parsed once.
//
// Per operator: ratio / detune or a fixed frequency, level, ADSR, velocity
// sensitivity, keyboard level scaling and one of 8 waveforms.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/dsp/envelope.h"
#include "hsynth/dsp/oscillators.h"
#include "hsynth/params.h"

namespace hsynth {

struct FmAlgorithm {
    uint8_t modulators[P::kFmOps];  // bit i set => op i modulates this op
    uint8_t carriers;               // bit i set => op i is audible
    int8_t feedbackOp;              // operator with self-feedback
    int8_t numOps;                  // operators used; the others are not computed
};

constexpr int kNumFmAlgorithms = 88;

// Display names ("17: 8op 8>7>6>5>4>3>2>1"), index = algorithm.
const std::vector<std::string>& fmAlgorithmNames();
// Builds the tables (not real-time; the engine calls it in prepare()).
const FmAlgorithm& fmAlgorithm(int index);

// Operator waveforms (DX11 / TX81Z style plus square / saw).
enum class FmWave : int { Sine = 0, Half, Abs, Quarter, Alt, AltAbs, Square, Saw, Count };

inline float fmRatio(float coarse) { return coarse < 0.5f ? 0.5f : coarse; }

inline float fmWave(uint32_t ph, int w) {
    const float s = sineLookupU32(ph);
    switch (w) {
        case 1: return s > 0.0f ? s : 0.0f;                                  // half sine
        case 2: return std::fabs(s);                                         // |sine|
        case 3: return (ph & 0x40000000u) ? 0.0f : std::fabs(s);             // quarter pulses
        case 4: return (ph & 0x80000000u) ? 0.0f : sineLookupU32(ph << 1);   // double speed, first half
        case 5: return (ph & 0x80000000u) ? 0.0f : std::fabs(sineLookupU32(ph << 1));
        case 6: return clampf(s * 3.0f, -1.0f, 1.0f);                        // soft square
        case 7: return (s + 0.5f * sineLookupU32(ph * 2u) + 0.3333f * sineLookupU32(ph * 3u)) * 0.6f;  // 3-partial saw
        default: return s;
    }
}

class FmVoice {
public:
    void setSampleRate(float sr) {
        sampleRate_ = sr;
        for (auto& e : env_) e.setSampleRate(sr);
    }

    void noteOn(float velocity, int key = 60) {
        velocity_ = velocity;
        key_ = key;
        for (int i = 0; i < P::kFmOps; ++i) {
            phase_[i] = 0;
            env_[i].reset();
            env_[i].gate(true);
            lastCoarse_[i] = lastDetune_[i] = -1e9f;
        }
        fb1_ = fb2_ = 0.0f;
    }
    void noteOff() {
        for (auto& e : env_) e.gate(false);
    }
    void kill(float seconds) {
        for (auto& e : env_) e.kill(seconds);
    }

    // Update per-control-block settings from the parameter array.
    // levelMod: optional per-operator level modulation (x(1 + m), clamped 0..2).
    // modPitch: semitones added to every modulator (non-carrier) operator.
    // lfo: the voice's LFO1 (-1..1) for the operators' amplitude modulation.
    void control(const float* p, int layerIdx, const float* levelMod = nullptr, float modPitch = 0.0f, float lfo = 0.0f) {
        algo_ = &fmAlgorithm((int)p[P::layer(layerIdx, P::FmAlgo)]);
        const float modRatio = modPitch != 0.0f ? std::exp2(modPitch / 12.0f) : 1.0f;
        for (int i = 0; i < algo_->numOps; ++i) {
            const float coarse = p[P::fmOp(layerIdx, i, P::OpRatio)];
            const float det = p[P::fmOp(layerIdx, i, P::OpDetune)];
            if (coarse != lastCoarse_[i] || det != lastDetune_[i]) {
                lastCoarse_[i] = coarse;
                lastDetune_[i] = det;
                baseRatio_[i] = fmRatio(coarse) * std::exp2(det / 1200.0f);
            }
            ratio_[i] = (algo_->carriers & (1 << i)) ? baseRatio_[i] : baseRatio_[i] * modRatio;
            fixedInc_[i] = p[P::fmOp(layerIdx, i, P::OpFixed)] > 0.0f
                               ? p[P::fmOp(layerIdx, i, P::OpFixed)] * std::exp2(det / 1200.0f) / sampleRate_
                               : 0.0f;
            wave_[i] = (int)p[P::fmOp(layerIdx, i, P::OpWave)];
            const float vs = p[P::fmOp(layerIdx, i, P::OpVelSens)];
            const float ks = p[P::fmOp(layerIdx, i, P::OpKeyScale)];  // dB per octave from C4
            level_[i] = p[P::fmOp(layerIdx, i, P::OpLevel)] * (1.0f - vs + vs * velocity_);
            if (ks != 0.0f) level_[i] *= std::min(4.0f, dbToGain(ks * (float)(key_ - 60) / 12.0f));
            if (levelMod && levelMod[i] != 0.0f) level_[i] *= clampf(1.0f + levelMod[i], 0.0f, 2.0f);
            const float ams = p[P::fmOp(layerIdx, i, P::OpAms)];
            if (ams > 0.0f) level_[i] *= 1.0f - ams * (0.5f - 0.5f * lfo);
            // rate scaling: envelopes run faster up the keyboard (x2 per octave at 1)
            const float rs = p[P::fmOp(layerIdx, i, P::OpRateScale)];
            const float tsc = rs > 0.0f ? clampf(std::exp2(-rs * (float)(key_ - 60) / 12.0f), 0.05f, 8.0f) : 1.0f;
            env_[i].set(p[P::fmOp(layerIdx, i, P::OpAttack)] * tsc, p[P::fmOp(layerIdx, i, P::OpDecay)] * tsc,
                        p[P::fmOp(layerIdx, i, P::OpSustain)], p[P::fmOp(layerIdx, i, P::OpRelease)] * tsc);
        }
        int carriers = 0;
        for (int i = 0; i < algo_->numOps; ++i)
            if (algo_->carriers & (1 << i)) ++carriers;
        carrierGain_ = carriers > 0 ? 1.0f / (float)carriers : 0.0f;
    }

    // Render n <= kControlBlock samples, adding into out. Operator envelopes
    // run at control rate and are interpolated; operators are processed one at
    // a time over the block (modulators first). Phases are 32-bit integers
    // (one cycle = 2^32) so wrapping costs nothing.
    // dtStart/dtEnd: base frequency / sampleRate ramp, index: modulation depth
    // in cycles, feedback 0..1.
    void render(float* out, int n, float dtStart, float dtEnd, float index, float feedback) {
        float buf[P::kFmOps][kControlBlock];
        float mod[kControlBlock];
        const float invN = 1.0f / (float)n;
        const float fbAmt = feedback * 0.35f;
        constexpr float kTwo32 = 4294967296.0f;
        const int numOps = algo_->numOps;
        for (int i = numOps - 1; i >= 0; --i) {
            const float e0 = env_[i].level();
            const float e1 = env_[i].processBlock(n);
            const float lvl = level_[i];
            float* o = buf[i];
            const bool fixed = fixedInc_[i] > 0.0f;
            const float inc0 = fixed ? fixedInc_[i] : dtStart * ratio_[i];
            const float incStep = fixed ? 0.0f : (dtEnd - dtStart) * ratio_[i] * invN;
            if (lvl <= 0.0f || (e0 <= 0.0f && e1 <= 0.0f)) {
                for (int s = 0; s < n; ++s) o[s] = 0.0f;
                phase_[i] += (uint32_t)(int64_t)((inc0 + incStep * (float)n * 0.5f) * (float)n * kTwo32);
                continue;
            }
            const uint8_t mask = algo_->modulators[i];
            for (int s = 0; s < n; ++s) mod[s] = 0.0f;
            for (int m = i + 1; m < numOps; ++m)
                if (mask & (1 << m))
                    for (int s = 0; s < n; ++s) mod[s] += buf[m][s];
            const float eStep = (e1 - e0) * invN;
            float e = e0;
            uint32_t ph = phase_[i];
            float inc = inc0;
            const int w = wave_[i];
            if (i == algo_->feedbackOp) {
                float f1 = fb1_, f2 = fb2_;
                for (int s = 0; s < n; ++s) {
                    e += eStep;
                    const uint32_t q = ph + cyclesToPhase(mod[s] * index + fbAmt * (f1 + f2));
                    const float v = (w ? fmWave(q, w) : sineLookupU32(q)) * e * lvl;
                    f2 = f1;
                    f1 = v;
                    o[s] = v;
                    inc += incStep;
                    ph += (uint32_t)(inc * kTwo32);
                }
                fb1_ = f1;
                fb2_ = f2;
            } else if (w == 0) {
                for (int s = 0; s < n; ++s) {
                    e += eStep;
                    o[s] = sineLookupU32(ph + cyclesToPhase(mod[s] * index)) * e * lvl;
                    inc += incStep;
                    ph += (uint32_t)(inc * kTwo32);
                }
            } else {
                for (int s = 0; s < n; ++s) {
                    e += eStep;
                    o[s] = fmWave(ph + cyclesToPhase(mod[s] * index), w) * e * lvl;
                    inc += incStep;
                    ph += (uint32_t)(inc * kTwo32);
                }
            }
            phase_[i] = ph;
            if (algo_->carriers & (1 << i))
                for (int s = 0; s < n; ++s) out[s] += o[s] * carrierGain_;
        }
    }

    // True while any carrier envelope is still running.
    bool active() const {
        for (int i = 0; i < algo_->numOps; ++i)
            if ((algo_->carriers & (1 << i)) && env_[i].active()) return true;
        return false;
    }

private:
    const FmAlgorithm* algo_ = &fmAlgorithm(0);
    Adsr env_[P::kFmOps];
    uint32_t phase_[P::kFmOps] = {};
    float baseRatio_[P::kFmOps] = {1, 1, 1, 1, 1, 1, 1, 1};
    float ratio_[P::kFmOps] = {1, 1, 1, 1, 1, 1, 1, 1};
    float fixedInc_[P::kFmOps] = {};
    int wave_[P::kFmOps] = {};
    float lastCoarse_[P::kFmOps] = {}, lastDetune_[P::kFmOps] = {};
    float level_[P::kFmOps] = {};
    float velocity_ = 1.0f;
    float sampleRate_ = 48000.0f;
    int key_ = 60;
    float carrierGain_ = 1.0f;
    float fb1_ = 0.0f, fb2_ = 0.0f;
};
}  // namespace hsynth
