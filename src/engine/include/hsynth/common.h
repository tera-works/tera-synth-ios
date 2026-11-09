// HSynth - hybrid synthesizer engine
// Common constants and small math helpers. No platform-specific code here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hsynth {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

// Modulation / envelope control rate: modulation is evaluated once per
// kControlBlock samples and audio parameters are ramped inside the block.
constexpr int kControlBlock = 16;

// Hard upper bounds. All buffers are allocated for these sizes in prepare().
constexpr int kMaxVoices = 256;         // upper bound of the user-selectable polyphony
constexpr int kDefaultVoices = 128;     // default polyphony (flagship-class)
constexpr int kLowPowerVoices = 64;     // suggested for low-power devices (Quality Low)
constexpr int kVoicePoolExtra = 16;     // extra voices used to fade out stolen notes
constexpr int kMaxBlockSize = 1024;     // engine splits larger host buffers
constexpr int kNumLayers = 3;           // layer slots per voice (A/B/C)

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// floor() without a libm call (valid for |x| < 2^31). std::floor is not inlined
// on baseline x86-64, and it sits in the innermost oscillator loops.
inline float fastFloor(float x) {
    const int i = (int)x;
    return (float)(i - (x < (float)i ? 1 : 0));
}

// 2^x via range reduction + degree-7 polynomial (rel. error ~1e-6, i.e. well
// below 0.01 cent). Used for per-block pitch / cutoff conversions.
inline float fastExp2(float x) {
    x = clampf(x, -126.0f, 126.0f);
    const float fl = fastFloor(x);
    const float f = x - fl;
    float p = 1.5252734e-5f;
    p = p * f + 1.5403530e-4f;
    p = p * f + 1.3333558e-3f;
    p = p * f + 9.6181291e-3f;
    p = p * f + 5.5504109e-2f;
    p = p * f + 2.4022651e-1f;
    p = p * f + 6.9314718e-1f;
    p = p * f + 1.0f;
    const int32_t bits = ((int32_t)fl + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

inline float midiToHz(float note) { return 440.0f * fastExp2((note - 69.0f) * (1.0f / 12.0f)); }

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// Rational tanh approximation; deterministic and cheap. Accurate to ~1e-3 for |x|<3.
inline float fastTanh(float x) {
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// fastTanh with first-order antiderivative anti-aliasing (ADAA): the output
// is the average of the shaper over the segment between consecutive inputs,
// which suppresses the aliasing of the harmonics the shaper creates. Costs a
// log per sample (double precision: the difference quotient cancels), so it
// is used for Quality::High only. One instance per signal (keeps the previous
// input).
struct AdaaTanh {
    static double antiderivative(double x) {
        // fastTanh(x) = x / 9 + 24 x / (9 x^2 + 27) for |x| <= 3, +-1 beyond
        const double a = std::fabs(x);
        if (a <= 3.0) return x * x / 18.0 + (4.0 / 3.0) * std::log(9.0 * x * x + 27.0);
        return 0.5 + (4.0 / 3.0) * std::log(108.0) + (a - 3.0);
    }
    double x1 = 0.0, f1 = 0.0;
    bool primed = false;  // false: the next sample starts a new segment (no stale history)
    void reset() { primed = false; }
    float process(float xf) {
        const double x = xf, fx = antiderivative(x);
        if (!primed) {  // first sample after (re)start: plain shaper, remember the point
            primed = true;
            x1 = x;
            f1 = fx;
            return fastTanh(xf);
        }
        const double d = x - x1;
        const float y = std::fabs(d) > 1e-5 ? (float)((fx - f1) / d) : fastTanh((float)(0.5 * (x + x1)));
        x1 = x;
        f1 = fx;
        return y;
    }
};

// Deterministic per-engine random generator (xorshift32). Avoids std::rand and
// keeps offline renders reproducible.
struct Rng {
    uint32_t state = 0x12345678u;
    explicit Rng(uint32_t seed = 0x12345678u) : state(seed ? seed : 1u) {}
    uint32_t next() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
    // Uniform in [-1, 1)
    float bipolar() { return (float)(next() >> 8) * (2.0f / 16777216.0f) - 1.0f; }
    // Uniform in [0, 1)
    float unipolar() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
};

// One-pole parameter smoother.
struct Smoother {
    float value = 0.0f;
    float coef = 0.0f;
    void setTime(float seconds, float sampleRate) {
        coef = seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (seconds * sampleRate));
    }
    void reset(float v) { value = v; }
    float process(float target) {
        value = target + (value - target) * coef;
        return value;
    }
};

}  // namespace hsynth
