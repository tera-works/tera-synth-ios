// Multimode filter based on the topology-preserving-transform state variable
// filter (Zavalishin / Cytomic). Stable under fast modulation. Optional input
// drive and a soft-saturated band-pass state give a mild non-linear character.
#pragma once

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

class Svf {
public:
    void reset() { ic1_ = ic2_ = 0.0f; }

    // cutoff in Hz, res 0..1
    void setCoefs(float cutoff, float res, float sampleRate) {
        cutoff = clampf(cutoff, 10.0f, sampleRate * 0.49f);
        g_ = std::tan(kPi * cutoff / sampleRate);
        k_ = 2.0f - 1.98f * clampf(res, 0.0f, 1.0f);  // k = 1/Q, Q up to ~50
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }

    struct Out { float lp, bp, hp; };

    Out tick(float x) {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return {v2, v1, x - k_ * v1 - v2};
    }

    // Non-linear variant: saturate the integrator state (keeps resonance tame
    // when driven hard).
    Out tickNonlinear(float x) {
        Out o = tick(x);
        ic1_ = fastTanh(ic1_ * 0.5f) * 2.0f;
        return o;
    }

    float k() const { return k_; }
    void copyCoefs(const Svf& o) { g_ = o.g_; k_ = o.k_; a1_ = o.a1_; a2_ = o.a2_; a3_ = o.a3_; }

private:
    float g_ = 0, k_ = 2, a1_ = 0, a2_ = 0, a3_ = 0;
    float ic1_ = 0, ic2_ = 0;
};

// 4-pole transistor-ladder low-pass (TPT / zero-delay feedback, Zavalishin
// ch. 5) with a tanh input stage. Resonance 1 self-oscillates; the pass-band
// level drop of the ladder is partly compensated.
class Ladder {
public:
    void reset() { s_[0] = s_[1] = s_[2] = s_[3] = 0.0f; }
    void setCoefs(float cutoff, float res, float sampleRate) {
        cutoff = clampf(cutoff, 10.0f, sampleRate * 0.45f);
        const float g = std::tan(kPi * cutoff / sampleRate);
        G_ = g / (1.0f + g);
        k_ = 4.2f * clampf(res, 0.0f, 1.0f);
        const float b = 1.0f - G_;
        c_[0] = G_ * G_ * G_ * b;
        c_[1] = G_ * G_ * b;
        c_[2] = G_ * b;
        c_[3] = b;
        G4_ = G_ * G_ * G_ * G_;
        inv_ = 1.0f / (1.0f + k_ * G4_);
        inGain_ = 1.0f + 0.5f * k_;
    }
    void copyCoefs(const Ladder& o) {
        G_ = o.G_; k_ = o.k_; G4_ = o.G4_; inv_ = o.inv_; inGain_ = o.inGain_;
        for (int i = 0; i < 4; ++i) c_[i] = o.c_[i];
    }
    float tick(float x, bool nonlinear) {
        const float G = G_;
        // linear estimate of the output, solved without a unit delay
        const float S = c_[0] * s_[0] + c_[1] * s_[1] + c_[2] * s_[2] + c_[3] * s_[3];
        x *= inGain_;
        const float y4 = (G4_ * x + S) * inv_;
        float u = x - k_ * y4;
        u = nonlinear ? fastTanh(u) : fastTanh(u * 0.5f) * 2.0f;  // tames self-oscillation
        for (int i = 0; i < 4; ++i) {
            const float v = (u - s_[i]) * G;
            const float y = v + s_[i];
            s_[i] = y + v;
            u = y;
        }
        return u;
    }

private:
    float G_ = 0.5f, k_ = 0.0f, G4_ = 0.0625f, inv_ = 1.0f, inGain_ = 1.0f;
    float c_[4] = {0.0625f, 0.125f, 0.25f, 0.5f};
    float s_[4] = {};
};

// Feedback comb tuned to the cutoff frequency (delay = 1 / cutoff), resonance
// = feedback amount (sign by mode). The delay line memory is supplied by the
// owner (allocated outside the audio thread).
class Comb {
public:
    static constexpr int kSize = 4096;  // power of two; >= sampleRate / 20 Hz up to 81.9 kHz
    void setBuffer(float* buf) { buf_ = buf; }
    float* buffer() const { return buf_; }
    bool dirty() const { return dirty_; }
    void setDirty(bool d) { dirty_ = d; }
    void reset() {
        // clear the (16 KB) delay line only if it was written since the last
        // clear: voices reset their filters on every note-on
        if (buf_ && dirty_) std::memset(buf_, 0, sizeof(float) * kSize);
        dirty_ = false;
        pos_ = 0;
        damp_ = 0.0f;
    }
    void setCoefs(float cutoff, float res, bool negative, float sampleRate) {
        // minus the group delay of the loop damping (one-pole, a = 0.7)
        delay_ = clampf(sampleRate / clampf(cutoff, 20.0f, sampleRate * 0.45f) - 0.43f, 2.0f, (float)(kSize - 2));
        fb_ = (negative ? -1.0f : 1.0f) * 0.985f * clampf(res, 0.0f, 1.0f);
        gain_ = 1.0f - 0.5f * std::fabs(fb_);
    }
    void copyCoefs(const Comb& o) { delay_ = o.delay_; fb_ = o.fb_; gain_ = o.gain_; }
    void copyState(const Comb& o) {
        if (buf_ && o.buf_ && o.dirty_) std::memcpy(buf_, o.buf_, sizeof(float) * kSize);
        else if (buf_ && dirty_) std::memset(buf_, 0, sizeof(float) * kSize);
        dirty_ = o.dirty_;
        pos_ = o.pos_;
        damp_ = o.damp_;
    }
    float tick(float x) {
        if (!buf_) return x;
        const float rp = (float)pos_ - delay_ + (float)kSize;
        const int i0 = (int)rp;
        const float f = rp - (float)i0;
        const float a = buf_[i0 & (kSize - 1)], b = buf_[(i0 + 1) & (kSize - 1)];
        const float d = a + (b - a) * f;
        damp_ += 0.7f * (d - damp_);  // gentle loop damping keeps high feedback smooth
        const float y = x + fb_ * damp_;
        buf_[pos_] = fastTanh(y * 0.5f) * 2.0f;
        dirty_ = true;
        pos_ = (pos_ + 1) & (kSize - 1);
        return y * gain_;
    }

private:
    float* buf_ = nullptr;
    bool dirty_ = true;  // unknown contents until the first reset
    int pos_ = 0;
    float delay_ = 100.0f, fb_ = 0.0f, gain_ = 1.0f, damp_ = 0.0f;
};

// Formant (vowel) filter: three parallel band-passes on the formants of
// a / e / i / o / u, morphed by `morph` (0..1) and shifted by cutoff / 1 kHz.
inline void formantSet(float morph, float out[3][2]) {
    static const float kF[5][3] = {{800, 1150, 2900}, {350, 2000, 2800}, {270, 2140, 2950}, {450, 800, 2830}, {325, 700, 2700}};
    static const float kA[5][3] = {{1.0f, 0.5f, 0.25f}, {1.0f, 0.3f, 0.2f}, {1.0f, 0.25f, 0.2f}, {1.0f, 0.4f, 0.1f}, {1.0f, 0.2f, 0.06f}};
    const float x = clampf(morph, 0.0f, 1.0f) * 4.0f;
    const int i = std::min(3, (int)x);
    const float t = x - (float)i;
    for (int k = 0; k < 3; ++k) {
        out[k][0] = kF[i][k] + (kF[i + 1][k] - kF[i][k]) * t;
        out[k][1] = kA[i][k] + (kA[i + 1][k] - kA[i][k]) * t;
    }
}

// Voice filter: chooses mode, cascades two SVFs for 24 dB/oct low-pass.
class VoiceFilter {
public:
    void reset() { s1_.reset(); s2_.reset(); s3_.reset(); ladder_.reset(); comb_.reset(); adaa_.reset(); }
    void setCombBuffer(float* buf) { comb_.setBuffer(buf); }

    // morph: formant vowel position (other types ignore it)
    // hq: anti-aliased drive stage (Quality::High)
    void set(FilterType type, float cutoff, float res, float drive, float sampleRate, float morph = 0.0f, bool hq = false) {
        type_ = type;
        hq_ = hq;
        if (!hq || drive <= 0.001f || type == FilterType::Off) adaa_.reset();  // restart cleanly when it comes back
        if (type == FilterType::Off) return;
        drive_ = drive;
        driveGain_ = 1.0f + drive * 8.0f;
        driveComp_ = 1.0f / std::sqrt(driveGain_);
        switch (type) {
            case FilterType::LP24:
                // spread resonance: only the second stage resonates
                s1_.setCoefs(cutoff, 0.0f, sampleRate);
                s2_.setCoefs(cutoff, res, sampleRate);
                break;
            case FilterType::Ladder: ladder_.setCoefs(cutoff, res, sampleRate); break;
            case FilterType::CombPlus:
            case FilterType::CombMinus: comb_.setCoefs(cutoff, res, type == FilterType::CombMinus, sampleRate); break;
            case FilterType::Formant: {
                float f[3][2];
                formantSet(morph, f);
                const float shift = clampf(cutoff / 1000.0f, 0.25f, 4.0f);
                const float q = 0.75f + 0.23f * clampf(res, 0.0f, 1.0f);  // Q ~2 .. 25
                s1_.setCoefs(f[0][0] * shift, q, sampleRate);
                s2_.setCoefs(f[1][0] * shift, q, sampleRate);
                s3_.setCoefs(f[2][0] * shift, q, sampleRate);
                for (int k = 0; k < 3; ++k) fGain_[k] = f[k][1];
                break;
            }
            default: s1_.setCoefs(cutoff, res, sampleRate); break;
        }
    }

    // Share coefficients with another filter (stereo pair) without touching state.
    void copyCoefsFrom(const VoiceFilter& o) {
        type_ = o.type_; drive_ = o.drive_; driveGain_ = o.driveGain_; driveComp_ = o.driveComp_; hq_ = o.hq_;
        s1_.copyCoefs(o.s1_);
        s2_.copyCoefs(o.s2_);
        s3_.copyCoefs(o.s3_);
        ladder_.copyCoefs(o.ladder_);
        comb_.copyCoefs(o.comb_);
        for (int k = 0; k < 3; ++k) fGain_[k] = o.fGain_[k];
    }
    // Full copy (coefficients + state) keeping this filter's own comb memory.
    void copyFrom(const VoiceFilter& o) {
        float* buf = comb_.buffer();
        const bool dirty = comb_.dirty();
        *this = o;
        comb_.setBuffer(buf);
        comb_.setDirty(dirty);  // state of this filter's own memory
        comb_.copyState(o.comb_);
    }

    float process(float x) {
        if (type_ == FilterType::Off) return x;
        if (drive_ > 0.001f) x = (hq_ ? adaa_.process(x * driveGain_) : fastTanh(x * driveGain_)) * driveComp_ * 1.5f;
        const bool nl = drive_ > 0.001f;
        switch (type_) {
            case FilterType::LP12: return (nl ? s1_.tickNonlinear(x) : s1_.tick(x)).lp;
            case FilterType::LP24: {
                const float a = s1_.tick(x).lp;
                return (nl ? s2_.tickNonlinear(a) : s2_.tick(a)).lp;
            }
            case FilterType::BP: {
                Svf::Out o = nl ? s1_.tickNonlinear(x) : s1_.tick(x);
                return o.bp * s1_.k();  // unity gain at centre
            }
            case FilterType::HP: return (nl ? s1_.tickNonlinear(x) : s1_.tick(x)).hp;
            case FilterType::Notch: {
                Svf::Out o = s1_.tick(x);
                return o.lp + o.hp;
            }
            case FilterType::Ladder: return ladder_.tick(x, nl);
            case FilterType::CombPlus:
            case FilterType::CombMinus: return comb_.tick(x);
            case FilterType::Formant:
                return 2.0f * (s1_.tick(x).bp * s1_.k() * fGain_[0] + s2_.tick(x).bp * s2_.k() * fGain_[1] +
                               s3_.tick(x).bp * s3_.k() * fGain_[2]);
            default: return x;
        }
    }

private:
    Svf s1_, s2_, s3_;
    Ladder ladder_;
    Comb comb_;
    float fGain_[3] = {1, 1, 1};
    AdaaTanh adaa_;
    bool hq_ = false;
    FilterType type_ = FilterType::LP24;
    float drive_ = 0.0f, driveGain_ = 1.0f, driveComp_ = 1.0f;
};

}  // namespace hsynth
