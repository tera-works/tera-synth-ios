// EQ and filter effects: parametric EQ, 10-band graphic EQ, modulated
// multimode / formant filter (auto-filter, envelope follower), wah.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

namespace {
// envelope level (linear) -> 0..1 over a 48 dB window
inline float envNorm(float e) { return clampf((fastLinToDb(e + 1e-9f) + 48.0f) / 48.0f, 0.0f, 1.0f); }
}  // namespace

// ------------------------------------------------------------ parametric EQ

void PeqFx::prepare(float sr) { sr_ = sr; init_ = false; reset(); }
void PeqFx::reset() { for (auto& b : bq_) b.reset(); }
void PeqFx::update(const float* p, const FxContext&) {
    bool changed = !init_;
    for (int k = 0; k < peq::Count; ++k)
        if (p[k] != last_[k]) { changed = true; last_[k] = p[k]; }
    if (!changed) return;
    init_ = true;
    active_[0] = p[peq::LowCut] > 10.5f;
    if (active_[0]) bq_[0].set(Biquad::HP, p[peq::LowCut], 0.707f, 0, sr_);
    active_[1] = std::fabs(p[peq::LsGain]) > 0.01f;
    if (active_[1]) bq_[1].set(Biquad::LowShelf, p[peq::LsFreq], 0.707f, p[peq::LsGain], sr_);
    for (int k = 0; k < 3; ++k) {
        const int f = peq::P1Freq + k * 3;
        active_[2 + k] = std::fabs(p[f + 1]) > 0.01f;
        if (active_[2 + k]) bq_[2 + k].set(Biquad::Peak, p[f], p[f + 2], p[f + 1], sr_);
    }
    active_[5] = std::fabs(p[peq::HsGain]) > 0.01f;
    if (active_[5]) bq_[5].set(Biquad::HighShelf, p[peq::HsFreq], 0.707f, p[peq::HsGain], sr_);
    active_[6] = p[peq::HighCut] < 21500.0f;
    if (active_[6]) bq_[6].set(Biquad::LP, p[peq::HighCut], 0.707f, 0, sr_);
    out_ = dbToLin(p[peq::Output]);
}
void PeqFx::process(float* L, float* R, int n, const FxContext&) {
    for (int k = 0; k < 7; ++k) {
        if (!active_[k]) continue;
        StereoBiquad& b = bq_[k];
        for (int i = 0; i < n; ++i) b.process(L[i], R[i]);
    }
    if (out_ != 1.0f)
        for (int i = 0; i < n; ++i) { L[i] *= out_; R[i] *= out_; }
}

// ------------------------------------------------------------ graphic EQ

void GeqFx::prepare(float sr) { sr_ = sr; init_ = false; reset(); }
void GeqFx::reset() { for (auto& b : bq_) b.reset(); }
void GeqFx::update(const float* p, const FxContext&) {
    static const float kFreq[10] = {31.25f, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    level_ = dbToLin(p[geq::Level]);
    bool changed = !init_;
    for (int k = 0; k < 10; ++k)
        if (p[geq::B31 + k] != last_[k]) { changed = true; last_[k] = p[geq::B31 + k]; }
    if (!changed) return;
    init_ = true;
    numActive_ = 0;
    for (int k = 0; k < 10; ++k) {
        const float g = p[geq::B31 + k];
        if (std::fabs(g) < 0.01f) continue;
        // constant-Q octave bands; the 16 kHz band is kept below Nyquist
        bq_[k].set(Biquad::Peak, std::min(kFreq[k], sr_ * 0.4f), 1.41f, g, sr_);
        active_[numActive_++] = k;
    }
}
void GeqFx::process(float* L, float* R, int n, const FxContext&) {
    for (int a = 0; a < numActive_; ++a) {
        StereoBiquad& b = bq_[active_[a]];
        for (int i = 0; i < n; ++i) b.process(L[i], R[i]);
    }
    if (level_ != 1.0f)
        for (int i = 0; i < n; ++i) { L[i] *= level_; R[i] *= level_; }
}

// ------------------------------------------------------------ filter

namespace {
// formant frequencies (F1..F3) for the vowels a, e, i, o, u
const float kVowel[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {350, 2300, 3000}, {450, 800, 2830}, {325, 700, 2530}};
const float kVowelGain[3] = {1.0f, 0.5f, 0.3f};
}  // namespace

void FilterFx::prepare(float sr) {
    sr_ = sr;
    mixR_.prepare(sr, 1.0f);
    reset();
}
void FilterFx::reset() {
    l1_.reset(); l2_.reset(); r1_.reset(); r2_.reset();
    for (auto& f : fl_) f.reset();
    for (auto& f : fr_) f.reset();
    env_.reset();
    ctl_ = 0;
}
void FilterFx::update(const float* p, const FxContext& c) {
    type_ = (int)p[filter::Type];
    cutoff_ = p[filter::Cutoff];
    reso_ = p[filter::Reso];
    drive_ = p[filter::Drive];
    wave_ = (int)p[filter::LfoWave];
    sync_ = (int)p[filter::LfoSync];
    lfo_.setRate(p[filter::LfoRate], sync_, c.tempo, sr_);
    lfoDepth_ = p[filter::LfoDepth];
    envAmt_ = p[filter::EnvAmount];
    env_.set(p[filter::EnvAttack] * 0.001f, p[filter::EnvRelease] * 0.001f, sr_);
    phase_ = p[filter::Phase] / 360.0f;
    mixR_.target = p[filter::Mix];
}
void FilterFx::setCoefs(float cutL, float cutR) {
    if (type_ == 7) {
        // formant: cutoff picks a position along a-e-i-o-u
        for (int ch = 0; ch < 2; ++ch) {
            const float cut = ch ? cutR : cutL;
            const float t = clampf(std::log2(cut / 20.0f) / 9.9658f, 0.0f, 1.0f) * 4.0f;
            const int a = std::min(3, (int)t);
            const float f = t - (float)a;
            for (int k = 0; k < 3; ++k) {
                const float hz = kVowel[a][k] + (kVowel[a + 1][k] - kVowel[a][k]) * f;
                (ch ? fr_[k] : fl_[k]).setCoefs(hz, 0.75f + 0.2f * reso_, sr_);
            }
        }
        return;
    }
    const bool cascade = type_ == 1 || type_ == 3;
    l1_.setCoefs(cutL, cascade ? 0.0f : reso_, sr_);
    r1_.setCoefs(cutR, cascade ? 0.0f : reso_, sr_);
    if (cascade) { l2_.setCoefs(cutL, reso_, sr_); r2_.setCoefs(cutR, reso_, sr_); }
}
void FilterFx::process(float* L, float* R, int n, const FxContext&) {
    const float dg = 1.0f + drive_ * 6.0f, dcomp = 1.0f / std::sqrt(dg);
    const FxWave w = (FxWave)wave_;
    for (int i = 0; i < n; ++i) {
        const float e = env_.process(0.5f * (L[i] + R[i]));
        if (ctl_-- <= 0) {
            ctl_ = kControlBlock - 1;
            const float envOct = envAmt_ * 6.0f * envNorm(e);
            const float ol = lfoDepth_ * 4.0f * lfo_.value(w, 0.0f) + envOct;
            const float orr = lfoDepth_ * 4.0f * lfo_.value(w, phase_) + envOct;
            const float top = sr_ * 0.45f;
            setCoefs(clampf(cutoff_ * fastExp2(ol), 20.0f, top), clampf(cutoff_ * fastExp2(orr), 20.0f, top));
        }
        lfo_.tick(w, rng_);
        float xl = L[i], xr = R[i];
        if (drive_ > 0.001f) { xl = fastTanh(xl * dg) * dcomp; xr = fastTanh(xr * dg) * dcomp; }
        float yl, yr;
        switch (type_) {
            case 0: yl = l1_.tick(xl).lp; yr = r1_.tick(xr).lp; break;
            case 1: yl = l2_.tick(l1_.tick(xl).lp).lp; yr = r2_.tick(r1_.tick(xr).lp).lp; break;
            case 2: yl = l1_.tick(xl).hp; yr = r1_.tick(xr).hp; break;
            case 3: yl = l2_.tick(l1_.tick(xl).hp).hp; yr = r2_.tick(r1_.tick(xr).hp).hp; break;
            case 4: yl = l1_.tick(xl).bp * l1_.k(); yr = r1_.tick(xr).bp * r1_.k(); break;
            case 5: yl = xl - l1_.tick(xl).bp * l1_.k(); yr = xr - r1_.tick(xr).bp * r1_.k(); break;
            case 6: { auto a = l1_.tick(xl), b = r1_.tick(xr); yl = a.lp - a.hp; yr = b.lp - b.hp; break; }
            default: {
                yl = yr = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    yl += fl_[k].tick(xl).bp * fl_[k].k() * kVowelGain[k];
                    yr += fr_[k].tick(xr).bp * fr_[k].k() * kVowelGain[k];
                }
                yl *= 1.6f;
                yr *= 1.6f;
                break;
            }
        }
        const float m = mixR_.next();
        L[i] += (yl - L[i]) * m;
        R[i] += (yr - R[i]) * m;
    }
}

// ------------------------------------------------------------ wah

void WahFx::prepare(float sr) { sr_ = sr; env_.set(0.002f, 0.12f, sr); reset(); }
void WahFx::reset() { l_.reset(); r_.reset(); env_.reset(); ctl_ = 0; }
void WahFx::update(const float* p, const FxContext& c) {
    mode_ = (int)p[wah::Mode];
    pos_ = p[wah::Position];
    sens_ = p[wah::Sens];
    lfo_.setRate(p[wah::Rate], 0, c.tempo, sr_);
    reso_ = p[wah::Reso];
    mix_ = p[wah::Mix];
}
void WahFx::process(float* L, float* R, int n, const FxContext&) {
    const float smooth = 1.0f - tauCoef(0.015f, sr_ / kControlBlock);
    for (int i = 0; i < n; ++i) {
        const float e = env_.process(0.5f * (L[i] + R[i]));
        const float lfo = lfo_.tick(FxWave::Sine, rng_);
        if (ctl_-- <= 0) {
            ctl_ = kControlBlock - 1;
            float target = pos_;
            if (mode_ == 1) target = clampf(sens_ * 1.4f * envNorm(e * 2.0f) - 0.1f, 0.0f, 1.0f);
            else if (mode_ == 2) target = 0.5f + 0.5f * lfo;
            posS_ += smooth * (target - posS_);
            const float hz = 350.0f * fastExp2(posS_ * 2.65f);  // 350 Hz .. 2.2 kHz
            l_.setCoefs(hz, 0.6f + 0.35f * reso_, sr_);
            r_.copyCoefs(l_);
        }
        const auto a = l_.tick(L[i]), b = r_.tick(R[i]);
        const float yl = a.bp * l_.k() * 2.0f + a.lp * 0.15f;
        const float yr = b.bp * r_.k() * 2.0f + b.lp * 0.15f;
        L[i] += (yl - L[i]) * mix_;
        R[i] += (yr - R[i]) * mix_;
    }
}

}  // namespace fx
}  // namespace hsynth
