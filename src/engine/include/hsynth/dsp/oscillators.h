// Band-limited virtual-analog oscillators (polyBLEP / polyBLAMP) and a shared
// sine lookup table.
#pragma once

#include <cmath>

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

// 4096-point sine table with linear interpolation (phase in cycles). The table
// is a plain global so lookups carry no static-initialisation guard.
constexpr int kSineTableSize = 4096;
extern const float* const g_sineTable;  // kSineTableSize + 1 entries

inline float sineLookup(float phase) {
    phase -= fastFloor(phase);
    const float idx = phase * (float)kSineTableSize;
    const int i0 = (int)idx;
    const float f = idx - (float)i0;
    const int i = i0 & (kSineTableSize - 1);  // guards phase rounding to exactly 1.0
    return g_sineTable[i] + f * (g_sineTable[i + 1] - g_sineTable[i]);
}

// Integer-phase variant: phase is a 32-bit fraction of a cycle (wraps for free).
inline float sineLookupU32(uint32_t phase) {
    const uint32_t i = phase >> 20;  // 12-bit table index
    const float f = (float)(phase & 0xFFFFFu) * (1.0f / 1048576.0f);
    return g_sineTable[i] + f * (g_sineTable[i + 1] - g_sineTable[i]);
}

// Convert cycles (any sign, |x| < 2^31) to a 32-bit phase offset.
inline uint32_t cyclesToPhase(float cycles) { return (uint32_t)(int64_t)(cycles * 4294967296.0f); }
inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline float polyBlamp(float t, float dt) {
    if (t < dt) {
        t = t / dt - 1.0f;
        return -(1.0f / 3.0f) * t * t * t;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return (1.0f / 3.0f) * t * t * t;
    }
    return 0.0f;
}

class VaOsc {
public:
    void reset(float phase) { phase_ = phase - fastFloor(phase); }

    // dt = frequency / sampleRate (cycles per sample), pw = pulse width 0..1
    float tick(VaWave wave, float dt, float pw) {
        dt = clampf(dt, 0.0f, 0.45f);
        const float t = phase_;
        float v;
        switch (wave) {
            case VaWave::Saw:
                v = 2.0f * t - 1.0f - polyBlep(t, dt);
                break;
            case VaWave::Square:
                pw = 0.5f;
                [[fallthrough]];
            case VaWave::Pulse: {
                v = t < pw ? 1.0f : -1.0f;
                v += polyBlep(t, dt);
                float t2 = t + 1.0f - pw;
                t2 -= fastFloor(t2);
                v -= polyBlep(t2, dt);
                // remove DC of asymmetric pulse
                v -= 2.0f * pw - 1.0f;
                break;
            }
            case VaWave::Triangle: {
                v = 1.0f - 4.0f * std::fabs(t - 0.5f);  // -1 at phase 0, +1 at 0.5
                float t2 = t + 0.5f;
                t2 -= fastFloor(t2);
                v += 4.0f * dt * (polyBlamp(t, dt) - polyBlamp(t2, dt));
                break;
            }
            case VaWave::Sine:
            default:
                v = sineLookup(t);
                break;
        }
        phase_ += dt;
        wrapped_ = phase_ >= 1.0f;
        if (wrapped_) {
            phase_ -= 1.0f;
            wrapFrac_ = dt > 0.0f ? phase_ / dt : 0.0f;  // fraction of this sample after the wrap
        }
        return v;
    }
    // hard sync: did the last tick wrap, and how far past the wrap (in samples)
    bool wrapped() const { return wrapped_; }
    float wrapFrac() const { return wrapFrac_; }

private:
    float phase_ = 0.0f;
    bool wrapped_ = false;
    float wrapFrac_ = 0.0f;
};

}  // namespace hsynth
