// Building blocks shared by the effect rack: delay lines, biquads, envelope
// followers, LFOs, an oversampler. All allocation happens in allocate()/
// prepare(); the per-sample methods never allocate.
#pragma once

#include <vector>

#include "hsynth/common.h"
#include "hsynth/fx/fx_params.h"

namespace hsynth {
namespace fx {

inline float dbToLin(float db) { return fastExp2(db * 0.16609640f); }  // 10^(db/20)
inline float linToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }
// 1-pole coefficient for a time constant in seconds (0 => instant)
inline float tauCoef(float seconds, float sr) { return seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (seconds * sr)); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// log2 for x > 0 via exponent extraction + quadratic (abs err < 5e-3, i.e.
// 0.03 dB: enough for level detectors).
inline float fastLog2(float x) {
    x = std::max(x, 1e-20f);
    int32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    const float e = (float)(((bits >> 23) & 255) - 128);
    bits = (bits & 0x007FFFFF) | 0x3F800000;  // mantissa in [1, 2)
    float m;
    std::memcpy(&m, &bits, sizeof m);
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
inline float fastLinToDb(float g) { return 6.0205999f * fastLog2(g); }

// sin(2*pi*p) for p in cycles; odd polynomial after folding, |err| < 2e-4.
inline float sinCycles(float p) {
    p -= fastFloor(p);
    float x = p < 0.5f ? p : p - 1.0f;  // -0.5..0.5
    if (x > 0.25f) x = 0.5f - x;
    else if (x < -0.25f) x = -0.5f - x;
    const float y = x * kTwoPi;
    const float y2 = y * y;
    return y * (1.0f + y2 * (-0.16666667f + y2 * (0.0083333310f + y2 * -0.00019840874f)));
}

// Dry/wet gains: equal loudness at 50 %, dry stays at unity below it.
inline void mixGains(float mix, float& dry, float& wet) {
    dry = mix <= 0.5f ? 1.0f : 2.0f * (1.0f - mix);
    wet = mix >= 0.5f ? 1.0f : 2.0f * mix;
}

// Power-of-two ring buffer with fractional reads.
class DelayLine {
public:
    void allocate(int maxSamples) {
        int size = 16;
        while (size < maxSamples + 8) size <<= 1;
        buf_.assign((size_t)size, 0.0f);
        mask_ = size - 1;
        write_ = 0;
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); }
    int capacity() const { return mask_ - 4; }
    void push(float x) {
        buf_[(size_t)write_] = x;
        write_ = (write_ + 1) & mask_;
    }
    // delay >= 1 sample (the sample pushed last is delay 1)
    float read(float delay) const {
        const float pos = (float)write_ - delay;
        const int i = (int)fastFloor(pos);
        const float f = pos - (float)i;
        const float a = buf_[(size_t)(i & mask_)];
        const float b = buf_[(size_t)((i + 1) & mask_)];
        return a + f * (b - a);
    }
    // 4-point Hermite (for modulated pitch-sensitive lines)
    float readCubic(float delay) const {
        const float pos = (float)write_ - delay;
        const int i = (int)fastFloor(pos);
        const float f = pos - (float)i;
        const float xm = buf_[(size_t)((i - 1) & mask_)], x0 = buf_[(size_t)(i & mask_)];
        const float x1 = buf_[(size_t)((i + 1) & mask_)], x2 = buf_[(size_t)((i + 2) & mask_)];
        const float c1 = 0.5f * (x1 - xm);
        const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
    float readInt(int delay) const { return buf_[(size_t)((write_ - delay) & mask_)]; }

private:
    std::vector<float> buf_;
    int mask_ = 0;
    int write_ = 0;
};

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void identity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void copyCoefs(const Biquad& o) { b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2; }

    enum Type { LP, HP, BP, Notch, Peak, LowShelf, HighShelf, AllPass };
    void set(Type t, float freq, float q, float gainDb, float sr) {
        freq = clampf(freq, 5.0f, sr * 0.495f);
        q = std::max(q, 0.025f);
        const double w0 = 2.0 * 3.14159265358979 * freq / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double alpha = sw / (2.0 * q);
        double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
        switch (t) {
            case LP: B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = B0; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case HP: B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = B0; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case BP: B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Notch: B0 = 1; B1 = -2 * cw; B2 = 1; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case AllPass: B0 = 1 - alpha; B1 = -2 * cw; B2 = 1 + alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Peak:
                B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A;
                A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A;
                break;
            case LowShelf:
            case HighShelf: {
                const double sA = 2 * std::sqrt(A) * (sw / 2 * std::sqrt(2.0));  // shelf slope S = 1
                if (t == LowShelf) {
                    B0 = A * ((A + 1) - (A - 1) * cw + sA); B1 = 2 * A * ((A - 1) - (A + 1) * cw);
                    B2 = A * ((A + 1) - (A - 1) * cw - sA); A0 = (A + 1) + (A - 1) * cw + sA;
                    A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - sA;
                } else {
                    B0 = A * ((A + 1) + (A - 1) * cw + sA); B1 = -2 * A * ((A - 1) + (A + 1) * cw);
                    B2 = A * ((A + 1) + (A - 1) * cw - sA); A0 = (A + 1) - (A - 1) * cw + sA;
                    A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - sA;
                }
                break;
            }
        }
        b0 = (float)(B0 / A0); b1 = (float)(B1 / A0); b2 = (float)(B2 / A0);
        a1 = (float)(A1 / A0); a2 = (float)(A2 / A0);
    }
};

// Stereo biquad sharing one coefficient set.
struct StereoBiquad {
    Biquad l, r;
    void reset() { l.reset(); r.reset(); }
    void set(Biquad::Type t, float f, float q, float g, float sr) { l.set(t, f, q, g, sr); r.copyCoefs(l); }
    void identity() { l.identity(); r.identity(); }
    void process(float& a, float& b) { a = l.process(a); b = r.process(b); }
};

// 1-pole low-pass (y += a (x - y)); also gives the complementary high-pass.
struct OnePole {
    float a = 1.0f, y = 0.0f;
    void setFreq(float hz, float sr) { a = 1.0f - std::exp(-kTwoPi * clampf(hz, 1.0f, sr * 0.49f) / sr); }
    void reset() { y = 0.0f; }
    float lp(float x) { y += a * (x - y); return y; }
    float hp(float x) { return x - lp(x); }
};

// DC blocker (~10 Hz).
struct DcBlock {
    float x1 = 0, y1 = 0, r = 0.9987f;
    void setSampleRate(float sr) { r = 1.0f - kTwoPi * 10.0f / sr; }
    void reset() { x1 = y1 = 0; }
    float process(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

// Peak envelope follower with separate attack / release.
struct EnvFollower {
    float att = 0, rel = 0, env = 0;
    void set(float attackSec, float releaseSec, float sr) { att = tauCoef(attackSec, sr); rel = tauCoef(releaseSec, sr); }
    void reset() { env = 0; }
    float process(float x) {
        const float a = std::fabs(x);
        env = a > env ? a + (env - a) * att : a + (env - a) * rel;
        return env;
    }
};

// Free-running / tempo-synced LFO with per-sample output in [-1, 1].
class FxLfo {
public:
    void reset(float phase = 0.0f) { phase_ = phase; held_ = 0; prevHeld_ = 0; }
    // Rate in Hz, or a sync division (non-zero) with the tempo.
    void setRate(float hz, int sync, float tempo, float sr) {
        const float beats = syncBeats(sync);
        const float f = beats > 0.0f ? tempo / 60.0f / beats : hz;
        inc_ = f / sr;
    }
    // Snap the phase to a beat position (tempo-synced LFOs follow the clock).
    void syncTo(double beat, int sync) {
        const float beats = syncBeats(sync);
        if (beats > 0.0f) {
            const double cyc = beat / beats;
            phase_ = (float)(cyc - std::floor(cyc));
        }
    }
    // Phase-lock gently towards the beat (for LFOs that move delay lines or
    // all-pass networks, where a phase jump would click): closes ~5 % of the
    // error per call.
    void syncSoft(double beat, int sync) {
        const float beats = syncBeats(sync);
        if (beats <= 0.0f) return;
        const double cyc = beat / beats;
        float err = (float)(cyc - std::floor(cyc)) - phase_;
        if (err > 0.5f) err -= 1.0f;
        if (err < -0.5f) err += 1.0f;
        phase_ += 0.05f * err;
        phase_ -= fastFloor(phase_);
    }
    float phase() const { return phase_; }
    float inc() const { return inc_; }
    // advance one sample; offset (cycles) gives a phase-shifted copy
    float tick(FxWave w, Rng& rng) {
        const float v = value(w, 0.0f);
        phase_ += inc_;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            prevHeld_ = held_;
            held_ = rng.bipolar();
        }
        return v;
    }
    float value(FxWave w, float offset) const {
        float p = phase_ + offset;
        p -= fastFloor(p);
        switch (w) {
            case FxWave::Sine: return sinCycles(p);
            case FxWave::Triangle: return 1.0f - 4.0f * std::fabs(p - 0.5f);
            case FxWave::SawUp: return 2.0f * p - 1.0f;
            case FxWave::SawDown: return 1.0f - 2.0f * p;
            case FxWave::Square: return p < 0.5f ? 1.0f : -1.0f;
            case FxWave::SampleHold: return held_;
            case FxWave::Smooth: {
                const float t = phase_;  // smooth random ignores offset
                const float s = t * t * (3.0f - 2.0f * t);
                return prevHeld_ + (held_ - prevHeld_) * s;
            }
            default: return 0.0f;
        }
    }

private:
    float phase_ = 0, inc_ = 0, held_ = 0, prevHeld_ = 0;
};

// Polyphase half-band FIR for 2x / 4x oversampling of non-linear stages.
// Linear phase; ~90 dB stop band above 0.55 * fs/2 at the original rate.
class HalfBand {
public:
    static constexpr int kTaps = 47;  // odd, symmetric
    void design() {
        const int c = kTaps / 2;
        double sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            const int n = i - c;
            const double sinc = n == 0 ? 0.5 : std::sin(3.14159265358979 * n * 0.5) / (3.14159265358979 * n);
            // Kaiser window, beta = 8.5
            const double r = (double)n / c;
            const double w = bessel0(8.5 * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(8.5);
            h_[i] = (float)(sinc * w);
            sum += h_[i];
        }
        for (float& v : h_) v = (float)(v / sum);
        reset();
    }
    void reset() { std::fill(std::begin(z_), std::end(z_), 0.0f); pos_ = 0; }
    // Upsample one input sample into two output samples (gain-compensated).
    void up(float x, float& y0, float& y1) {
        push(2.0f * x);
        y0 = convolve();
        push(0.0f);
        y1 = convolve();
    }
    // Downsample two samples into one.
    float down(float x0, float x1) {
        push(x0);
        push(x1);
        return convolve();
    }

private:
    static double bessel0(double x) {
        double s = 1, t = 1;
        for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
        return s;
    }
    void push(float x) {
        pos_ = (pos_ + 1) % kTaps;
        z_[pos_] = x;
        z_[pos_ + kTaps] = x;
    }
    float convolve() const {
        // z_ is mirrored so z_[pos_+1 .. pos_+kTaps] is contiguous, oldest first
        const float* z = &z_[pos_ + 1];
        float acc = 0;
        for (int i = 0; i < kTaps; ++i) acc += h_[i] * z[i];  // symmetric taps
        return acc;
    }
    float h_[kTaps] = {};
    float z_[2 * kTaps] = {};
    int pos_ = 0;
};

// Applies a per-sample non-linear function at 1x, 2x or 4x the sample rate.
class Oversampler {
public:
    void prepare() { for (auto& h : up_) h.design(); for (auto& h : dn_) h.design(); }
    void reset() { for (auto& h : up_) h.reset(); for (auto& h : dn_) h.reset(); }
    template <typename F>
    float process(float x, int factor, F&& f) {
        if (factor <= 1) return f(x);
        float a, b;
        up_[0].up(x, a, b);
        if (factor == 2) return dn_[0].down(f(a), f(b));
        float a0, a1, b0, b1;
        up_[1].up(a, a0, a1);
        up_[1].up(b, b0, b1);
        const float ya = dn_[1].down(f(a0), f(a1));
        const float yb = dn_[1].down(f(b0), f(b1));
        return dn_[0].down(ya, yb);
    }
    // group delay in base-rate samples
    static float latency(int factor) {
        const float d = (float)(HalfBand::kTaps / 2);
        return factor <= 1 ? 0.0f : factor == 2 ? d : d + 0.5f * d;
    }

private:
    HalfBand up_[2], dn_[2];
};

// Context passed to every block for one process() call.
struct FxContext {
    float sampleRate = 48000.0f;
    float tempo = 120.0f;        // bpm
    double beat = 0.0;           // beat position at the start of the block
    int oversample = 2;          // 1, 2 or 4 (from engine quality)
    bool highQuality = false;
    float lastNoteHz = 261.63f;  // most recent note-on (ring-mod key track)
    bool notesHeld = false;
    float sustain = 0.0f;        // sustain pedal 0..1 (string resonance)
    const float* inL = nullptr;  // external audio input (vocoder), may be null
    const float* inR = nullptr;
};

}  // namespace fx
}  // namespace hsynth
