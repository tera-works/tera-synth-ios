// Pitch: two-voice harmonizer / pitch shifter, analog-style octaver.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

float GrainShifter::process(float x, float ratio) {
    line.push(x);
    // the read tap drifts at (1 - ratio) samples per sample; two taps half a
    // window apart with sin^2 windows (summing to 1) hide the wrap-around
    phase += (1.0f - ratio) / window;
    phase -= fastFloor(phase);
    float p2 = phase + 0.5f;
    if (p2 >= 1.0f) p2 -= 1.0f;
    const float c = sinCycles(phase + 0.25f);  // cos(2 pi phase)
    const float g1 = 0.5f - 0.5f * c, g2 = 0.5f + 0.5f * c;
    return g1 * line.readCubic(phase * window + 2.0f) + g2 * line.readCubic(p2 * window + 2.0f);
}

namespace {
inline void panGains(float pan, float& l, float& r) {
    const float t = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.125f;  // 0..0.25 cycles
    l = sinCycles(t + 0.25f);
    r = sinCycles(t);
}
}  // namespace

void PitchFx::prepare(float sr) {
    sr_ = sr;
    v1_.allocate((int)(0.13f * sr));
    v2_.allocate((int)(0.13f * sr));
    mixR_.prepare(sr, 0.5f);
    reset();
}
void PitchFx::reset() { v1_.clear(); v2_.clear(); fb1_ = 0; }
void PitchFx::update(const float* p, const FxContext&) {
    ratio1_ = fastExp2((p[pitch::Semi1] + p[pitch::Fine1] * 0.01f) / 12.0f);
    ratio2_ = fastExp2((p[pitch::Semi2] + p[pitch::Fine2] * 0.01f) / 12.0f);
    level2_ = p[pitch::Level2];
    panGains(p[pitch::Pan1], pan1L_, pan1R_);
    panGains(p[pitch::Pan2], pan2L_, pan2R_);
    v1_.window = v2_.window = p[pitch::Window] * 0.001f * sr_;
    fb_ = p[pitch::Feedback];
    mixR_.target = p[pitch::Mix];
}
void PitchFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        const float x = 0.5f * (L[i] + R[i]);
        const float a = v1_.process(x + fb_ * fb1_, ratio1_);
        fb1_ = fastTanh(a);
        float wl = a * pan1L_, wr = a * pan1R_;
        if (level2_ > 0.0001f) {
            const float b = v2_.process(x, ratio2_) * level2_;
            wl += b * pan2L_;
            wr += b * pan2R_;
        }
        float dg, wg;
        mixGains(mixR_.next(), dg, wg);
        L[i] = L[i] * dg + wl * wg;
        R[i] = R[i] * dg + wr * wg;
    }
}

// ------------------------------------------------------------ octaver

void OctaverFx::prepare(float sr) {
    sr_ = sr;
    track_.set(Biquad::LP, 900.0f, 0.707f, 0, sr);
    track2_.set(Biquad::LP, 900.0f, 0.707f, 0, sr);
    upHp_.setFreq(60.0f, sr);
    env_.set(0.001f, 0.05f, sr);
    reset();
}
void OctaverFx::reset() {
    track_.reset(); track2_.reset(); subLp_.reset(); subLp2_.reset(); upHp_.reset(); env_.reset();
    high_ = false;
    ff1_ = ff2_ = 1.0f;
}
void OctaverFx::update(const float* p, const FxContext&) {
    sub1_ = p[octaver::Sub1];
    sub2_ = p[octaver::Sub2];
    up_ = p[octaver::Up];
    dry_ = p[octaver::Dry];
    const float f = 150.0f * std::pow(10.0f, p[octaver::Tone]);
    subLp_.set(Biquad::LP, f, 0.707f, 0, sr_);
    subLp2_.copyCoefs(subLp_);
}
void OctaverFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        const float x = 0.5f * (L[i] + R[i]);
        const float t = track2_.process(track_.l.process(x));
        const float e = env_.process(t);
        // Schmitt trigger on the low-passed signal drives two flip-flops
        const float thr = 0.15f * e + 1e-5f;
        if (!high_ && t > thr) {
            high_ = true;
            ff1_ = -ff1_;
            if (ff1_ > 0.0f) ff2_ = -ff2_;
        } else if (high_ && t < -thr) {
            high_ = false;
        }
        // OC-2 style: the signal itself is polarity-flipped by the divider
        float sub = t * (sub1_ * ff1_ + sub2_ * ff2_) * 1.5f;
        sub = subLp2_.process(subLp_.process(sub));
        const float up = up_ > 0.0001f ? upHp_.hp(std::fabs(x)) * up_ * 1.5f : 0.0f;
        L[i] = L[i] * dry_ + sub + up;
        R[i] = R[i] * dry_ + sub + up;
    }
}

}  // namespace fx
}  // namespace hsynth
