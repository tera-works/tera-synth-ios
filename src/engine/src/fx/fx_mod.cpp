// Modulation: chorus / dimension / ensemble, flanger (thru-zero), phaser,
// tremolo / auto-pan / harmonic tremolo, uni-vibe, rotary speaker, ring
// modulator / frequency shifter.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

// ------------------------------------------------------------ chorus

void ChorusFx::prepare(float sr) {
    sr_ = sr;
    dl_.allocate((int)(0.06f * sr));
    dr_.allocate((int)(0.06f * sr));
    bbd_.set(Biquad::LP, 9000.0f, 0.707f, 0, sr);
    mixR_.prepare(sr, 0.5f);
    reset();
}
void ChorusFx::reset() { dl_.clear(); dr_.clear(); fbL_ = fbR_ = 0; bbd_.reset(); }
void ChorusFx::update(const float* p, const FxContext& c) {
    mode_ = (int)p[chorus::Mode];
    voices_ = (int)clampf(p[chorus::Voices], 1, 4);
    rate_ = p[chorus::Rate];
    depth_ = p[chorus::Depth];
    delay_ = p[chorus::Delay];
    fb_ = p[chorus::Feedback];
    width_ = p[chorus::Width];
    float r = mode_ == 1 ? rate_ * 0.5f : rate_;
    if (mode_ >= 3) {  // juno: fixed rates (the Rate knob scales them around 1 Hz)
        static const float kJuno[3] = {0.513f, 0.863f, 8.0f};
        r = kJuno[mode_ - 3] * clampf(rate_, 0.1f, 10.0f);
    }
    lfo_.setRate(r, 0, c.tempo, sr_);
    lfo2_.setRate(rate_ * 9.0f, 0, c.tempo, sr_);
    mixR_.target = p[chorus::Mix];
}
void ChorusFx::process(float* L, float* R, int n, const FxContext&) {
    const float msToS = sr_ * 0.001f;
    const float base = delay_ * msToS;
    const float swing = 0.5f * delay_ * depth_ * msToS;
    int nv = voices_;
    if (mode_ == 1) nv = 2;
    if (mode_ == 2) nv = 3;
    const float norm = 1.0f / std::sqrt((float)nv);
    if (mode_ >= 3) {
        // juno: delay sweeps 1.66 .. 5.35 ms (I / II) or 3.3 .. 3.7 ms (I+II), BBD tone
        const bool both = mode_ == 5;
        const float centre = (both ? 3.5f : 3.5f) * msToS, range = (both ? 0.2f : 1.85f) * msToS * clampf(depth_ * 2.0f, 0.0f, 2.0f);
        for (int i = 0; i < n; ++i) {
            const float m = lfo_.value(FxWave::Triangle, 0.0f);
            lfo_.tick(FxWave::Triangle, rng_);
            dl_.push(L[i]);
            dr_.push(R[i]);
            float wl = dl_.readCubic(std::max(2.0f, centre + range * m));
            float wr = dr_.readCubic(std::max(2.0f, centre - range * m));
            bbd_.process(wl, wr);
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width_;
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            L[i] = L[i] * dg + (mid + side) * wg;
            R[i] = R[i] * dg + (mid - side) * wg;
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        dl_.push(L[i] + fbL_ * fb_);
        dr_.push(R[i] + fbR_ * fb_);
        float wl = 0, wr = 0;
        for (int v = 0; v < nv; ++v) {
            const float off = (float)v / (float)nv;
            float ml, mr;
            if (mode_ == 2) {
                // ensemble: slow sweep plus fast vibrato, 120 degrees apart
                ml = 0.8f * lfo_.value(FxWave::Sine, off) + 0.2f * lfo2_.value(FxWave::Sine, off);
                mr = 0.8f * lfo_.value(FxWave::Sine, off + 0.5f) + 0.2f * lfo2_.value(FxWave::Sine, off + 0.5f);
            } else {
                ml = lfo_.value(mode_ == 1 ? FxWave::Triangle : FxWave::Sine, off);
                mr = lfo_.value(mode_ == 1 ? FxWave::Triangle : FxWave::Sine, off + 0.25f + (mode_ == 1 ? 0.25f : 0.0f));
            }
            wl += dl_.readCubic(std::max(2.0f, base + swing * ml));
            wr += dr_.readCubic(std::max(2.0f, base + swing * mr));
        }
        lfo_.tick(FxWave::Sine, rng_);
        lfo2_.tick(FxWave::Sine, rng_);
        // feedback from the voice average keeps the loop gain below 1
        fbL_ = wl / (float)nv;
        fbR_ = wr / (float)nv;
        wl *= norm;
        wr *= norm;
        if (mode_ == 1) { const float a = wl - 0.4f * wr, b = wr - 0.4f * wl; wl = a; wr = b; }
        if (mode_ == 2) bbd_.process(wl, wr);
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width_;
        float dg, wg;
        mixGains(mixR_.next(), dg, wg);
        L[i] = L[i] * dg + (mid + side) * wg;
        R[i] = R[i] * dg + (mid - side) * wg;
    }
}

// ------------------------------------------------------------ flanger

void FlangerFx::prepare(float sr) {
    sr_ = sr;
    dl_.allocate((int)(0.025f * sr));
    dr_.allocate((int)(0.025f * sr));
    mixR_.prepare(sr, 0.5f);
    reset();
}
void FlangerFx::reset() { dl_.clear(); dr_.clear(); fbL_ = fbR_ = 0; }
void FlangerFx::update(const float* p, const FxContext& c) {
    sync_ = (int)p[flanger::Sync];
    lfo_.setRate(p[flanger::Rate], sync_, c.tempo, sr_);
    depth_ = p[flanger::Depth];
    manual_ = p[flanger::Manual];
    fb_ = p[flanger::Feedback];
    tz_ = p[flanger::ThruZero] > 0.5f;
    phase_ = p[flanger::Phase] / 360.0f;
    mixR_.target = p[flanger::Mix];
}
void FlangerFx::process(float* L, float* R, int n, const FxContext& c) {
    if (sync_ > 0) lfo_.syncSoft(c.beat, sync_);
    const float man = manual_ * sr_ * 0.001f;
    for (int i = 0; i < n; ++i) {
        const float ml = lfo_.value(FxWave::Triangle, 0.0f), mr = lfo_.value(FxWave::Triangle, phase_);
        lfo_.tick(FxWave::Triangle, rng_);
        dl_.push(L[i] + fastTanh(fbL_ * fb_));
        dr_.push(R[i] + fastTanh(fbR_ * fb_));
        const float wl = dl_.readCubic(std::max(1.5f, man * (1.0f + depth_ * ml)));
        const float wr = dr_.readCubic(std::max(1.5f, man * (1.0f + depth_ * mr)));
        fbL_ = wl;
        fbR_ = wr;
        float dryL = L[i], dryR = R[i];
        float sgn = 1.0f;
        if (tz_) {
            // through-zero: the dry path is delayed to the sweep centre and the
            // wet path is inverted, so the notches pass through zero delay
            dryL = dl_.readCubic(std::max(1.5f, man));
            dryR = dr_.readCubic(std::max(1.5f, man));
            sgn = -1.0f;
        }
        float dg, wg;
        mixGains(mixR_.next(), dg, wg);
        L[i] = dryL * dg + sgn * wl * wg;
        R[i] = dryR * dg + sgn * wr * wg;
    }
}

// ------------------------------------------------------------ phaser

void PhaserFx::prepare(float sr) { sr_ = sr; mixR_.prepare(sr, 0.5f); reset(); }
void PhaserFx::reset() {
    std::fill(std::begin(zl_), std::end(zl_), 0.0f);
    std::fill(std::begin(zr_), std::end(zr_), 0.0f);
    fbL_ = fbR_ = 0;
    ctl_ = 0;
}
void PhaserFx::update(const float* p, const FxContext& c) {
    stages_ = 2 + 2 * (int)clampf(p[phaser::Stages], 0, 5);
    sync_ = (int)p[phaser::Sync];
    lfo_.setRate(p[phaser::Rate], sync_, c.tempo, sr_);
    depth_ = p[phaser::Depth];
    center_ = p[phaser::Center];
    fb_ = p[phaser::Feedback];
    phase_ = p[phaser::Phase] / 360.0f;
    mixR_.target = p[phaser::Mix];
}
void PhaserFx::process(float* L, float* R, int n, const FxContext& c) {
    if (sync_ > 0) lfo_.syncSoft(c.beat, sync_);
    auto coef = [this](float f) {
        const float t = std::tan(kPi * clampf(f, 20.0f, sr_ * 0.45f) / sr_);
        return (t - 1.0f) / (t + 1.0f);
    };
    for (int i = 0; i < n; ++i) {
        if (ctl_-- <= 0) {
            ctl_ = 7;
            aL_ = coef(center_ * fastExp2(depth_ * 2.5f * lfo_.value(FxWave::Sine, 0.0f)));
            aR_ = coef(center_ * fastExp2(depth_ * 2.5f * lfo_.value(FxWave::Sine, phase_)));
        }
        lfo_.tick(FxWave::Sine, rng_);
        float xl = L[i] + fb_ * fbL_, xr = R[i] + fb_ * fbR_;
        for (int s = 0; s < stages_; ++s) {
            const float yl = aL_ * xl + zl_[s];
            zl_[s] = xl - aL_ * yl;
            xl = yl;
            const float yr = aR_ * xr + zr_[s];
            zr_[s] = xr - aR_ * yr;
            xr = yr;
        }
        fbL_ = fastTanh(xl);
        fbR_ = fastTanh(xr);
        float dg, wg;
        mixGains(mixR_.next(), dg, wg);
        L[i] = L[i] * dg + xl * wg;
        R[i] = R[i] * dg + xr * wg;
    }
}

// ------------------------------------------------------------ tremolo / auto-pan

void TremoloFx::prepare(float sr) {
    sr_ = sr;
    smooth_ = tauCoef(0.002f, sr);
    lp_.set(Biquad::LP, 800.0f, 0.5f, 0, sr);
    hp_.set(Biquad::HP, 800.0f, 0.5f, 0, sr);
    reset();
}
void TremoloFx::reset() { gl_ = gr_ = 1.0f; lp_.reset(); hp_.reset(); }
void TremoloFx::update(const float* p, const FxContext& c) {
    mode_ = (int)p[tremolo::Mode];
    wave_ = (int)p[tremolo::Wave];
    sync_ = (int)p[tremolo::Sync];
    lfo_.setRate(p[tremolo::Rate], sync_, c.tempo, sr_);
    depth_ = p[tremolo::Depth];
    phase_ = p[tremolo::Phase] / 360.0f;
}
void TremoloFx::process(float* L, float* R, int n, const FxContext& c) {
    if (sync_ > 0) lfo_.syncTo(c.beat, sync_);
    const FxWave w = (FxWave)wave_;
    for (int i = 0; i < n; ++i) {
        const float a = lfo_.value(w, 0.0f), b = lfo_.value(w, phase_);
        lfo_.tick(w, rng_);
        float tl, tr;
        if (mode_ == 1) {
            const float pan = depth_ * a;
            tl = std::sqrt(std::max(0.0f, 1.0f - pan));
            tr = std::sqrt(1.0f + pan);
        } else {
            tl = 1.0f - depth_ * (0.5f - 0.5f * a);
            tr = 1.0f - depth_ * (0.5f - 0.5f * b);
        }
        gl_ = tl + (gl_ - tl) * smooth_;
        gr_ = tr + (gr_ - tr) * smooth_;
        if (mode_ == 2) {
            // harmonic tremolo: lows and highs pulse in opposite phase
            float ll = L[i], lr = R[i], hl = L[i], hr = R[i];
            lp_.process(ll, lr);
            hp_.process(hl, hr);
            const float il = 2.0f - depth_ - gl_, ir = 2.0f - depth_ - gr_;
            L[i] = ll * gl_ + hl * il;
            R[i] = lr * gr_ + hr * ir;
        } else {
            L[i] *= gl_;
            R[i] *= gr_;
        }
    }
}

// ------------------------------------------------------------ uni-vibe

void VibeFx::prepare(float sr) { sr_ = sr; mixR_.prepare(sr, 0.5f); reset(); }
void VibeFx::reset() {
    std::fill(std::begin(zl_), std::end(zl_), 0.0f);
    std::fill(std::begin(zr_), std::end(zr_), 0.0f);
    ctl_ = 0;
}
void VibeFx::update(const float* p, const FxContext& c) {
    mode_ = (int)p[vibe::Mode];
    sync_ = (int)p[vibe::Sync];
    lfo_.setRate(p[vibe::Rate], sync_, c.tempo, sr_);
    depth_ = p[vibe::Depth];
    mixR_.target = mode_ == 1 ? 1.0f : p[vibe::Mix];
}
void VibeFx::process(float* L, float* R, int n, const FxContext& c) {
    if (sync_ > 0) lfo_.syncSoft(c.beat, sync_);
    // staggered stage capacitors -> four different corner frequencies
    static const float kBase[4] = {90.0f, 160.0f, 330.0f, 900.0f};
    for (int i = 0; i < n; ++i) {
        if (ctl_-- <= 0) {
            ctl_ = 7;
            // lamp / photocell response: fast rise, slow lingering fall
            const float s = 0.5f + 0.5f * lfo_.value(FxWave::Sine, 0.0f);
            const float shaped = s * std::sqrt(s);
            for (int k = 0; k < 4; ++k) {
                const float f = kBase[k] * (1.0f + 7.0f * depth_ * shaped);
                const float t = std::tan(kPi * std::min(f, sr_ * 0.45f) / sr_);
                a_[k] = (t - 1.0f) / (t + 1.0f);
            }
        }
        lfo_.tick(FxWave::Sine, rng_);
        float xl = L[i], xr = R[i];
        for (int k = 0; k < 4; ++k) {
            const float yl = a_[k] * xl + zl_[k];
            zl_[k] = xl - a_[k] * yl;
            xl = yl;
            const float yr = a_[k] * xr + zr_[k];
            zr_[k] = xr - a_[k] * yr;
            xr = yr;
        }
        float dg, wg;
        mixGains(mixR_.next(), dg, wg);
        L[i] = L[i] * dg + xl * wg;
        R[i] = R[i] * dg + xr * wg;
    }
}

// ------------------------------------------------------------ rotary speaker

void RotaryFx::prepare(float sr) {
    sr_ = sr;
    split_.set(Biquad::LP, 800.0f, 0.707f, 0, sr);
    splitHp_.set(Biquad::HP, 800.0f, 0.707f, 0, sr);
    hornL_.allocate((int)(0.01f * sr));
    mixR_.prepare(sr, 1.0f);
    reset();
}
void RotaryFx::reset() { split_.reset(); splitHp_.reset(); hornL_.clear(); }
void RotaryFx::update(const float* p, const FxContext&) {
    fast_ = p[rotary::Fast] > 0.5f;
    accel_ = p[rotary::Accel];
    balance_ = p[rotary::Balance];
    drive_ = p[rotary::Drive];
    spread_ = p[rotary::Spread];
    mixR_.target = p[rotary::Mix];
}
void RotaryFx::process(float* L, float* R, int n, const FxContext&) {
    const float hornT = fast_ ? 6.7f : 0.8f, drumT = fast_ ? 5.7f : 0.67f;
    const float kh = 1.0f - std::exp(-1.0f / (accel_ * sr_));
    const float kd = 1.0f - std::exp(-1.0f / (accel_ * 2.5f * sr_));
    const float dg = 1.0f + drive_ * 4.0f, dc = 1.0f / (1.0f + drive_ * 1.5f);
    const float centre = 0.0012f * sr_, swing = 0.00035f * sr_;
    const float micOff = 0.5f * spread_;  // second mic angle in cycles
    for (int i = 0; i < n; ++i) {
        hornRate_ += kh * (hornT - hornRate_);
        drumRate_ += kd * (drumT - drumRate_);
        hornPh_ += hornRate_ / sr_;
        if (hornPh_ >= 1.0f) hornPh_ -= 1.0f;
        drumPh_ += drumRate_ / sr_;
        if (drumPh_ >= 1.0f) drumPh_ -= 1.0f;
        float x = 0.5f * (L[i] + R[i]);
        if (drive_ > 0.001f) x = fastTanh(x * dg) * dc;
        float lo = x, lo2 = x, hi = x, hi2 = x;
        split_.process(lo, lo2);
        splitHp_.process(hi, hi2);
        hornL_.push(hi);
        // horn: doppler (delay) plus directivity (amplitude) seen by two mics
        const float sa = sinCycles(hornPh_), sb = sinCycles(hornPh_ + micOff);
        const float hl = hornL_.readCubic(centre + swing * sa) * (0.65f + 0.35f * sinCycles(hornPh_ + 0.25f));
        const float hr = hornL_.readCubic(centre + swing * sb) * (0.65f + 0.35f * sinCycles(hornPh_ + micOff + 0.25f));
        const float dl = lo * (0.8f + 0.2f * sinCycles(drumPh_));
        const float dr = lo * (0.8f + 0.2f * sinCycles(drumPh_ + micOff));
        const float yl = 1.4f * (dl * (1.0f - balance_) + hl * balance_);
        const float yr = 1.4f * (dr * (1.0f - balance_) + hr * balance_);
        const float m = mixR_.next();
        L[i] += (yl - L[i]) * m;
        R[i] += (yr - R[i]) * m;
    }
}

// ------------------------------------------------------------ ring mod / frequency shifter

void RingModFx::Hilbert::reset() {
    std::fill(std::begin(x1), std::end(x1), 0.0f);
    std::fill(std::begin(x2), std::end(x2), 0.0f);
    std::fill(std::begin(y1), std::end(y1), 0.0f);
    std::fill(std::begin(y2), std::end(y2), 0.0f);
    delayed = 0;
    // Olli Niemitalo's 90-degree phase-difference all-pass pair (coefs squared)
    static const float k[8] = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f,
                               0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};
    for (int i = 0; i < 8; ++i) a[i] = k[i] * k[i];
}
void RingModFx::Hilbert::process(float x, float& re, float& im) {
    float p = x, q = x;
    for (int s = 0; s < 4; ++s) {
        const float y = a[s] * (p + y2[s]) - x2[s];
        x2[s] = x1[s]; x1[s] = p; y2[s] = y1[s]; y1[s] = y;
        p = y;
    }
    for (int s = 4; s < 8; ++s) {
        const float y = a[s] * (q + y2[s]) - x2[s];
        x2[s] = x1[s]; x1[s] = q; y2[s] = y1[s]; y1[s] = y;
        q = y;
    }
    re = delayed;  // path 1 needs one extra sample of delay
    delayed = p;
    im = q;
}

void RingModFx::prepare(float sr) { sr_ = sr; mixR_.prepare(sr, 0.5f); reset(); }
void RingModFx::reset() { hl_.reset(); hr_.reset(); phL_ = phR_ = 0; }
void RingModFx::update(const float* p, const FxContext& c) {
    mode_ = (int)p[ringmod::Mode];
    freq_ = p[ringmod::Freq];
    key_ = p[ringmod::KeyTrack] > 0.5f;
    lfo_.setRate(p[ringmod::LfoRate], 0, c.tempo, sr_);
    lfoDepth_ = p[ringmod::LfoDepth];
    spread_ = p[ringmod::Spread];
    mixR_.target = p[ringmod::Mix];
}
void RingModFx::process(float* L, float* R, int n, const FxContext& c) {
    const float keyMul = key_ ? c.lastNoteHz / 261.63f : 1.0f;
    for (int i = 0; i < n; ++i) {
        const float f = freq_ * keyMul * fastExp2(lfoDepth_ * 2.0f * lfo_.tick(FxWave::Sine, rng_));
        float yl, yr;
        if (mode_ == 0) {
            phL_ += f / sr_;
            phL_ -= fastFloor(phL_);
            yl = L[i] * sinCycles(phL_);
            yr = R[i] * sinCycles(phL_ + 0.25f * spread_);
        } else {
            // single-sideband shift; the right channel shifts the other way
            // as Spread goes to 1
            phL_ += f / sr_;
            phL_ -= fastFloor(phL_);
            phR_ += f * (1.0f - 2.0f * spread_) / sr_;
            phR_ -= fastFloor(phR_);
            float re, im;
            hl_.process(L[i], re, im);
            yl = re * sinCycles(phL_ + 0.25f) + im * sinCycles(phL_);  // upper sideband
            hr_.process(R[i], re, im);
            yr = re * sinCycles(phR_ + 0.25f) + im * sinCycles(phR_);
        }
        const float m = mixR_.next();
        L[i] += (yl - L[i]) * m;
        R[i] += (yr - R[i]) * m;
    }
}

}  // namespace fx
}  // namespace hsynth
