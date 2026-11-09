// Dynamics: noise gate, compressor, 3-band compressor, transient shaper,
// look-ahead limiter.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

// ------------------------------------------------------------ noise gate

void GateFx::prepare(float sr) { sr_ = sr; det_.set(0.0002f, 0.02f, sr); reset(); }
void GateFx::reset() { det_.reset(); gain_ = 0.0f; holdLeft_ = 0; open_ = false; }
void GateFx::update(const float* p, const FxContext&) {
    thrOpen_ = dbToLin(p[gate::Thresh]);
    thrClose_ = thrOpen_ * 0.7f;  // 3 dB hysteresis
    floor_ = dbToLin(p[gate::Range]);
    att_ = tauCoef(p[gate::Attack] * 0.001f, sr_);
    rel_ = tauCoef(p[gate::Release] * 0.001f, sr_);
    hold_ = (int)(p[gate::Hold] * 0.001f * sr_);
}
void GateFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        const float e = det_.process(std::max(std::fabs(L[i]), std::fabs(R[i])));
        if (e > thrOpen_) { open_ = true; holdLeft_ = hold_; }
        else if (e < thrClose_ && open_) {
            if (holdLeft_ > 0) --holdLeft_;
            else open_ = false;
        }
        const float target = open_ ? 1.0f : floor_;
        gain_ = target + (gain_ - target) * (target > gain_ ? att_ : rel_);
        L[i] *= gain_;
        R[i] *= gain_;
    }
}

// ------------------------------------------------------------ compressor core

float CompCore::computeGainDb(float levelDb) const {
    // soft-knee static curve, returns gain reduction (<= 0)
    const float over = levelDb - thresh;
    if (knee > 0.0f && 2.0f * std::fabs(over) <= knee) {
        const float t = over + 0.5f * knee;
        return (1.0f / ratio - 1.0f) * t * t / (2.0f * knee);
    }
    return over > 0.0f ? over * (1.0f / ratio - 1.0f) : 0.0f;
}

float CompCore::process(float levelDb) {
    const float target = computeGainDb(levelDb);
    // attack when more reduction is needed (gain falling)
    grDb = target + (grDb - target) * (target < grDb ? att : rel);
    return grDb;
}

void CompFx::prepare(float sr) { sr_ = sr; reset(); }
void CompFx::reset() { core_.grDb = 0; rms_ = 0; hpf_.reset(); mixS_.reset(mix_); }
void CompFx::update(const float* p, const FxContext&) {
    core_.thresh = p[comp::Thresh];
    core_.ratio = std::max(1.0f, p[comp::Ratio]);
    core_.knee = p[comp::Knee];
    core_.att = tauCoef(p[comp::Attack] * 0.001f, sr_);
    core_.rel = tauCoef(p[comp::Release] * 0.001f, sr_);
    float makeup = p[comp::Makeup];
    if (p[comp::AutoMakeup] > 0.5f) makeup += std::min(12.0f, -core_.computeGainDb(0.0f) * 0.5f);
    makeup_ = dbToLin(makeup);
    rmsMode_ = p[comp::Detector] > 0.5f;
    rmsCoef_ = tauCoef(0.01f, sr_);
    useHpf_ = p[comp::ScHpf] > 21.0f;
    if (useHpf_) hpf_.set(Biquad::HP, p[comp::ScHpf], 0.707f, 0, sr_);
    mix_ = p[comp::Mix];
    mixS_.setTime(0.02f, sr_);
}
void CompFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        float sl = L[i], sr = R[i];
        if (useHpf_) hpf_.process(sl, sr);
        float levelDb;
        if (rmsMode_) {
            const float sq = 0.5f * (sl * sl + sr * sr);
            rms_ = sq + (rms_ - sq) * rmsCoef_;
            levelDb = 3.0103f * fastLog2(rms_ + 1e-12f) + 3.0f;  // 10*log10(ms) + crest
        } else {
            levelDb = fastLinToDb(std::max(std::fabs(sl), std::fabs(sr)) + 1e-9f);
        }
        const float g = dbToLin(core_.process(levelDb)) * makeup_;
        const float m = mixS_.process(mix_);
        const float wet = g * m + (1.0f - m);
        L[i] *= wet;
        R[i] *= wet;
    }
}

// ------------------------------------------------------------ multiband

void MbCompFx::prepare(float sr) { sr_ = sr; reset(); }
void MbCompFx::reset() {
    for (auto& b : lp1_) b.reset();
    for (auto& b : hp1_) b.reset();
    for (auto& b : lp2_) b.reset();
    for (auto& b : hp2_) b.reset();
    ap_.reset();
    for (auto& c : core_) c.grDb = 0;
}
void MbCompFx::update(const float* p, const FxContext&) {
    const float xl = p[mbcomp::XLow];
    const float xh = std::max(p[mbcomp::XHigh], xl * 1.5f);
    for (int k = 0; k < 2; ++k) {
        lp1_[k].set(Biquad::LP, xl, 0.7071f, 0, sr_);
        hp1_[k].set(Biquad::HP, xl, 0.7071f, 0, sr_);
        lp2_[k].set(Biquad::LP, xh, 0.7071f, 0, sr_);
        hp2_[k].set(Biquad::HP, xh, 0.7071f, 0, sr_);
    }
    ap_.set(Biquad::AllPass, xh, 0.7071f, 0, sr_);  // LR4 phase match for the low band
    const float att = tauCoef(p[mbcomp::Attack] * 0.001f, sr_), rel = tauCoef(p[mbcomp::Release] * 0.001f, sr_);
    for (int b = 0; b < 3; ++b) {
        core_[b].thresh = p[mbcomp::LowThresh + b * 3];
        core_[b].ratio = std::max(1.0f, p[mbcomp::LowRatio + b * 3]);
        core_[b].knee = 6.0f;
        core_[b].att = att;
        core_[b].rel = rel;
        gain_[b] = dbToLin(p[mbcomp::LowGain + b * 3]);
    }
    mix_ = p[mbcomp::Mix];
}
void MbCompFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        float lo[2], mid[2], hi[2];
        const float x[2] = {L[i], R[i]};
        // Linkwitz-Riley 4th order = two cascaded Butterworth sections
        float a = x[0], b = x[1];
        lp1_[0].process(a, b);
        lp1_[1].process(a, b);
        lo[0] = a; lo[1] = b;
        float ha = x[0], hb = x[1];
        hp1_[0].process(ha, hb);
        hp1_[1].process(ha, hb);
        float ma = ha, mb = hb;
        lp2_[0].process(ma, mb);
        lp2_[1].process(ma, mb);
        mid[0] = ma; mid[1] = mb;
        hp2_[0].process(ha, hb);
        hp2_[1].process(ha, hb);
        hi[0] = ha; hi[1] = hb;
        ap_.process(lo[0], lo[1]);
        float outL = 0, outR = 0;
        const float* bands[3][2] = {{&lo[0], &lo[1]}, {&mid[0], &mid[1]}, {&hi[0], &hi[1]}};
        for (int k = 0; k < 3; ++k) {
            const float bl = *bands[k][0], br = *bands[k][1];
            const float lvl = fastLinToDb(std::max(std::fabs(bl), std::fabs(br)) + 1e-9f);
            const float g = dbToLin(core_[k].process(lvl)) * gain_[k];
            outL += bl * g;
            outR += br * g;
        }
        L[i] = x[0] + (outL - x[0]) * mix_;
        R[i] = x[1] + (outR - x[1]) * mix_;
    }
}

// ------------------------------------------------------------ transient shaper

void TransientFx::prepare(float sr) { sr_ = sr; reset(); }
void TransientFx::reset() { fast_.reset(); slow_.reset(); gainDb_ = 0; }
void TransientFx::update(const float* p, const FxContext&) {
    const float speed = 0.25f + 1.5f * (1.0f - p[transient::Speed]);  // 0.25 .. 1.75
    fast_.set(0.0005f * speed, 0.02f * speed, sr_);
    slow_.set(0.025f * speed, 0.25f * speed, sr_);
    attack_ = p[transient::Attack];
    sustain_ = p[transient::Sustain];
    out_ = dbToLin(p[transient::Output]);
    smooth_ = tauCoef(0.001f, sr_);
}
void TransientFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        const float x = 0.5f * (std::fabs(L[i]) + std::fabs(R[i]));
        const float f = fast_.process(x) + 1e-6f, s = slow_.process(x) + 1e-6f;
        const float d = fastLinToDb(f / s);  // > 0 during attacks, < 0 in the tail
        float g = d > 0.0f ? attack_ * d * 1.5f : sustain_ * (-d) * 1.5f;
        g = clampf(g, -24.0f, 18.0f);
        gainDb_ = g + (gainDb_ - g) * smooth_;
        const float k = dbToLin(gainDb_) * out_;
        L[i] *= k;
        R[i] *= k;
    }
}

// ------------------------------------------------------------ limiter

void LimiterFx::prepare(float sr) {
    sr_ = sr;
    const int maxLa = (int)(0.006f * sr) + 2;
    dl_.allocate(maxLa);
    dr_.allocate(maxLa);
    win_.assign((size_t)maxLa + 1, 1.0f);
    reset();
}
void LimiterFx::reset() {
    dl_.clear(); dr_.clear();
    std::fill(win_.begin(), win_.end(), 1.0f);
    winPos_ = 0;
    gain_ = 1.0f;
}
void LimiterFx::update(const float* p, const FxContext&) {
    inGain_ = dbToLin(p[limiter::Input]);
    ceil_ = dbToLin(p[limiter::Ceiling]);
    const int la = std::max(1, (int)(p[limiter::Lookahead] * 0.001f * sr_));
    if (la != la_) { la_ = la; std::fill(win_.begin(), win_.end(), 1.0f); }
    att_ = tauCoef((float)la_ / sr_ / 3.0f, sr_);
    rel_ = tauCoef(p[limiter::Release] * 0.001f, sr_);
}
void LimiterFx::process(float* L, float* R, int n, const FxContext&) {
    const int W = la_ + 1;
    for (int i = 0; i < n; ++i) {
        const float l = L[i] * inGain_, r = R[i] * inGain_;
        const float pk = std::max(std::fabs(l), std::fabs(r));
        win_[(size_t)winPos_] = pk > ceil_ ? ceil_ / pk : 1.0f;
        winPos_ = (winPos_ + 1) % W;
        // minimum required gain over the look-ahead window (W is small)
        float target = 1.0f;
        for (int k = 0; k < W; ++k) target = std::min(target, win_[(size_t)k]);
        gain_ = target + (gain_ - target) * (target < gain_ ? att_ : rel_);
        dl_.push(l);
        dr_.push(r);
        L[i] = clampf(dl_.readInt(W) * gain_, -ceil_, ceil_);  // clamp catches what the ramp misses
        R[i] = clampf(dr_.readInt(W) * gain_, -ceil_, ceil_);
    }
}

}  // namespace fx
}  // namespace hsynth
