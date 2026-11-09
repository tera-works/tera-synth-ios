// Drive / tone shaping: drive pedal, amp, cabinet, bit crusher, lo-fi, exciter.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

// ------------------------------------------------------------ drive

namespace {
// pre-gain range (dB) per drive type: soft, od, dist, fuzz, metal, tube, tape, fold, rectify
const float kDriveMinDb[9] = {0, 6, 10, 20, 20, 0, 0, 0, 6};
const float kDriveMaxDb[9] = {30, 40, 50, 60, 60, 36, 24, 24, 36};
}  // namespace

float DriveFx::shape(float x) const {
    switch (type_) {
        case 0: return fastTanh(x);
        case 1: return x >= 0.0f ? fastTanh(x) : 0.8f * fastTanh(1.25f * x);
        case 2: { const float c = clampf(x, -1.0f, 1.0f); return 1.5f * c - 0.5f * c * c * c; }
        case 3: return x > 0.0f ? 0.6f * fastTanh(2.5f * x) : fastTanh(0.8f * x);
        case 4: return fastTanh(4.0f * fastTanh(x));
        case 5: return x >= 0.0f ? 1.0f - fastExp2(-1.4427f * x) : -(1.0f - fastExp2(1.4427f * 1.3f * x)) / 1.3f;
        case 6: return 1.3f * x / (1.0f + std::fabs(x));
        case 7: return sinCycles(x * 0.25f);
        case 8: return 2.0f * std::fabs(fastTanh(x)) - 0.6f;
        default: return x;
    }
}

void DriveFx::prepare(float sr) {
    sr_ = sr;
    osL_.prepare();
    osR_.prepare();
    dcL_.setSampleRate(sr);
    dcR_.setSampleRate(sr);
    dryL_.allocate(64);
    dryR_.allocate(64);
    mixR_.prepare(sr, 1.0f);
    fizz_.set(Biquad::LP, 11000.0f, 0.6f, 0, sr);
    reset();
}
void DriveFx::reset() {
    tight_.reset(); emph_.reset(); tone_.reset(); fizz_.reset();
    osL_.reset(); osR_.reset(); dcL_.reset(); dcR_.reset(); dryL_.clear(); dryR_.clear();
}
void DriveFx::update(const float* p, const FxContext& c) {
    type_ = (int)clampf(p[drive::Type], 0, 8);
    const float a = p[drive::Amount];
    pre_ = dbToLin(kDriveMinDb[type_] + a * (kDriveMaxDb[type_] - kDriveMinDb[type_]));
    post_ = dbToLin(p[drive::Level]) * clampf(1.0f / std::sqrt(pre_), 0.12f, 1.0f);
    bias_ = p[drive::Bias] * 0.5f;
    biasOut_ = shape(bias_);
    if (p[drive::Tight] > 21.0f) tight_.set(Biquad::HP, p[drive::Tight], 0.707f, 0, sr_);
    else tight_.identity();
    useEmph_ = type_ == 1 || type_ == 4;
    if (type_ == 1) emph_.set(Biquad::Peak, 720.0f, 0.7f, 6.0f, sr_);
    else if (type_ == 4) emph_.set(Biquad::Peak, 1000.0f, 0.8f, 9.0f, sr_);
    tone_.set(Biquad::LP, 800.0f * std::pow(20.0f, p[drive::Tone]), 0.707f, 0, sr_);
    mixR_.target = p[drive::Mix];
    os_ = c.oversample;
}
void DriveFx::process(float* L, float* R, int n, const FxContext&) {
    const float lat = Oversampler::latency(os_);
    auto f = [this](float x) { return shape(x * pre_ + bias_) - biasOut_; };
    for (int i = 0; i < n; ++i) {
        const float xl = L[i], xr = R[i];
        dryL_.push(xl);
        dryR_.push(xr);
        float l = xl, r = xr;
        tight_.process(l, r);
        if (useEmph_) emph_.process(l, r);
        l = dcL_.process(osL_.process(l, os_, f));
        r = dcR_.process(osR_.process(r, os_, f));
        tone_.process(l, r);
        if (type_ >= 2 && type_ <= 4) fizz_.process(l, r);
        const float m = mixR_.next();
        const float dl = lat > 0 ? dryL_.read(lat + 1.0f) : xl;
        const float dr = lat > 0 ? dryR_.read(lat + 1.0f) : xr;
        L[i] = dl + (l * post_ - dl) * m;
        R[i] = dr + (r * post_ - dr) * m;
    }
}

// ------------------------------------------------------------ amp

namespace {
struct AmpModel { float minDb, maxDb; int stages; float hp, lp, mid, midBase, bright; };
const AmpModel kAmps[6] = {
    {0, 24, 1, 60, 9000, 500, 0, 4},       // clean
    {6, 38, 2, 90, 8000, 650, 0, 3},       // crunch
    {12, 50, 3, 120, 7000, 700, 0, 0},     // lead
    {18, 60, 3, 150, 7000, 650, -3, 0},    // higain
    {20, 66, 4, 180, 6500, 500, -6, 0},    // modern
    {0, 30, 2, 30, 6000, 600, 0, 0},       // bass
};
inline float knobDb(float k, float range) { return (k - 5.0f) / 5.0f * range; }
}  // namespace

void AmpFx::configure(int os) {
    const AmpModel& m = kAmps[model_];
    const float rate = sr_ * (float)os;
    for (int s = 0; s < 4; ++s) {
        const float hp = s == 0 ? m.hp : m.hp * 1.6f;
        const float a = std::exp(-kTwoPi * hp / rate);
        const float lp = 1.0f - std::exp(-kTwoPi * (s == 0 ? 14000.0f : m.lp) / rate);
        stL_[s].hpA = stR_[s].hpA = a;
        stL_[s].lpA = stR_[s].lpA = lp;
    }
    configuredOs_ = os;
}

float AmpFx::preamp(float x, Stage* st) {
    for (int s = 0; s < stages_; ++s) {
        Stage& t = st[s];
        // coupling capacitor (1-pole high-pass)
        const float y = t.hpA * (t.hpY + x - t.hpX);
        t.hpX = x;
        t.hpY = y;
        float v = y * stageGain_ + bias_;
        v = v >= 0.0f ? v / (1.0f + v) * 1.4f : fastTanh(v * 0.9f) * 1.1f;  // asymmetric triode
        // Miller capacitance low-pass
        t.lpY += t.lpA * (v - t.lpY);
        x = t.lpY;
    }
    return x;
}

void AmpFx::prepare(float sr) {
    sr_ = sr;
    osL_.prepare();
    osR_.prepare();
    dcL_.setSampleRate(sr);
    dcR_.setSampleRate(sr);
    configuredOs_ = 0;
    reset();
}
void AmpFx::reset() {
    for (auto& s : stL_) s = Stage{s.hpA, 0, 0, s.lpA, 0};
    for (auto& s : stR_) s = Stage{s.hpA, 0, 0, s.lpA, 0};
    inHp_.reset(); bright_.reset(); bass_.reset(); mid_.reset(); treble_.reset();
    presence_.reset(); depth_.reset(); outLp_.reset();
    osL_.reset(); osR_.reset(); dcL_.reset(); dcR_.reset();
}
void AmpFx::update(const float* p, const FxContext& c) {
    bool changed = configuredOs_ != c.oversample;
    for (int k = 0; k < amp::Count; ++k)
        if (p[k] != p_[k]) { changed = true; p_[k] = p[k]; }
    if (!changed) return;
    const int model = (int)clampf(p[amp::Model], 0, 5);
    model_ = model;
    const AmpModel& m = kAmps[model];
    stages_ = m.stages;
    os_ = c.oversample;
    configure(os_);
    const float g = p[amp::Gain] / 10.0f;
    const float totalDb = m.minDb + g * (m.maxDb - m.minDb);
    stageGain_ = dbToLin(totalDb / (float)stages_);
    bias_ = model == 0 ? 0.05f : 0.15f;
    inHp_.set(Biquad::HP, model == 5 ? 25.0f : 70.0f, 0.707f, 0, sr_);
    bright_.set(Biquad::HighShelf, 3000.0f, 0.707f, m.bright * (1.0f - g), sr_);
    bass_.set(Biquad::LowShelf, 120.0f, 0.707f, knobDb(p[amp::Bass], 12), sr_);
    mid_.set(Biquad::Peak, m.mid, 0.7f, knobDb(p[amp::Middle], 12) + m.midBase, sr_);
    treble_.set(Biquad::HighShelf, 2800.0f, 0.707f, knobDb(p[amp::Treble], 12), sr_);
    presence_.set(Biquad::HighShelf, 4000.0f, 0.707f, knobDb(p[amp::Presence], 8), sr_);
    depth_.set(Biquad::Peak, model == 5 ? 70.0f : 90.0f, 1.0f, knobDb(p[amp::Depth], 8), sr_);
    outLp_.set(Biquad::LP, 12000.0f, 0.707f, 0, sr_);
    powerDrive_ = 1.2f + g * 1.5f;
    outGain_ = dbToLin(p[amp::Level]) * 0.5f;
}
void AmpFx::process(float* L, float* R, int n, const FxContext&) {
    auto fl = [this](float x) { return preamp(x, stL_); };
    auto fr = [this](float x) { return preamp(x, stR_); };
    for (int i = 0; i < n; ++i) {
        float l = L[i], r = R[i];
        inHp_.process(l, r);
        bright_.process(l, r);
        l = dcL_.process(osL_.process(l, os_, fl));
        r = dcR_.process(osR_.process(r, os_, fr));
        bass_.process(l, r);
        mid_.process(l, r);
        treble_.process(l, r);
        // power amp: gentle symmetric saturation, then presence / depth
        l = fastTanh(l * powerDrive_);
        r = fastTanh(r * powerDrive_);
        presence_.process(l, r);
        depth_.process(l, r);
        outLp_.process(l, r);
        L[i] = l * outGain_;
        R[i] = r * outGain_;
    }
}

// ------------------------------------------------------------ cabinet

namespace {
struct CabModel { float hp, hpQ; float pf[3], pg[3], pq[3]; float lp; };
const CabModel kCabs[8] = {
    {80, 1.2f, {110, 1800, 3200}, {3, -3, 4}, {1.0f, 1.0f, 2.0f}, 5000},     // 4x12 british
    {70, 1.1f, {100, 400, 2500}, {4, -4, 3}, {1.0f, 0.8f, 1.5f}, 5500},      // 4x12 modern
    {90, 0.8f, {200, 2000, 4000}, {1, 3, -2}, {1.0f, 1.2f, 1.5f}, 6000},     // 2x12 open
    {110, 0.9f, {250, 1200, 3500}, {1, 2, 4}, {1.0f, 1.0f, 2.0f}, 6500},     // 1x12 combo
    {120, 0.9f, {300, 2200, 4500}, {-1, 5, -2}, {1.0f, 1.2f, 1.5f}, 5500},   // 2x10 vintage
    {50, 1.0f, {80, 800, 2500}, {3, -2, 3}, {1.0f, 1.0f, 1.5f}, 6000},       // 4x10 bass
    {45, 1.1f, {70, 500, 2000}, {4, -3, 2}, {1.0f, 1.0f, 1.5f}, 4500},       // 8x10 bass
    {38, 1.0f, {60, 900, 2200}, {3, -2, 2}, {1.0f, 1.0f, 1.5f}, 3500},       // 1x15 bass
};
}  // namespace

void CabFx::prepare(float sr) {
    sr_ = sr;
    conv_.prepare(1 << 15);
    wetL_.assign(kMaxBlockSize, 0.0f);
    wetR_.assign(kMaxBlockSize, 0.0f);
    model_ = -1;
    reset();
}
void CabFx::reset() {
    for (auto& b : bq_) b.reset();
    lowCut_.reset();
    highCut_.reset();
    conv_.reset();
}
void CabFx::update(const float* p, const FxContext&) {
    const int model = (int)clampf(p[cab::Model], 0, 8);
    const float mic = p[cab::Mic];
    if (model != model_ || mic != mic_) {
        model_ = model;
        mic_ = mic;
        useIr_ = model == 8;
        if (!useIr_) {
            const CabModel& m = kCabs[model];
            bq_[0].set(Biquad::HP, m.hp, m.hpQ, 0, sr_);
            for (int k = 0; k < 3; ++k)
                bq_[1 + k].set(Biquad::Peak, m.pf[k], m.pq[k], m.pg[k] - (k == 2 ? 6.0f * mic : 0.0f), sr_);
            const float lp = m.lp * (1.0f - 0.45f * mic);
            bq_[4].set(Biquad::LP, lp, 0.707f, 0, sr_);
            bq_[5].set(Biquad::LP, lp * 1.25f, 0.9f, 0, sr_);
        }
    }
    useLow_ = p[cab::LowCut] > 20.5f;
    useHigh_ = p[cab::HighCut] < 19900.0f;
    if (useLow_) lowCut_.set(Biquad::HP, p[cab::LowCut], 0.707f, 0, sr_);
    if (useHigh_) highCut_.set(Biquad::LP, p[cab::HighCut], 0.707f, 0, sr_);
    level_ = dbToLin(p[cab::Level]);
    mix_ = p[cab::Mix];
}
void CabFx::process(float* L, float* R, int n, const FxContext&) {
    float* wl = wetL_.data();
    float* wr = wetR_.data();
    for (int off = 0; off < n; off += kMaxBlockSize) {
        const int m = std::min(kMaxBlockSize, n - off);
        std::copy(L + off, L + off + m, wl);
        std::copy(R + off, R + off + m, wr);
        if (useIr_) {
            conv_.setIr(userIr_);
            if (userIr_) conv_.process(wl, wr, m);
        } else {
            for (int i = 0; i < m; ++i)
                for (auto& b : bq_) b.process(wl[i], wr[i]);
        }
        for (int i = 0; i < m; ++i) {
            float l = wl[i], r = wr[i];
            if (useLow_) lowCut_.process(l, r);
            if (useHigh_) highCut_.process(l, r);
            L[off + i] += (l * level_ - L[off + i]) * mix_;
            R[off + i] += (r * level_ - R[off + i]) * mix_;
        }
    }
}

// ------------------------------------------------------------ bit crusher

void CrushFx::prepare(float sr) { sr_ = sr; reset(); }
void CrushFx::reset() { tone_.reset(); phase_ = 1.0f; holdL_ = holdR_ = 0; }
void CrushFx::update(const float* p, const FxContext&) {
    levels_ = fastExp2(p[crush::Bits] - 1.0f);
    step_ = std::min(1.0f, p[crush::Rate] / sr_);
    jitter_ = p[crush::Jitter];
    useTone_ = p[crush::Tone] < 0.999f;
    if (useTone_) tone_.set(Biquad::LP, 1000.0f * std::pow(20.0f, p[crush::Tone]), 0.707f, 0, sr_);
    mix_ = p[crush::Mix];
    out_ = dbToLin(p[crush::Level]);
}
void CrushFx::process(float* L, float* R, int n, const FxContext&) {
    const float inv = 1.0f / levels_;
    for (int i = 0; i < n; ++i) {
        phase_ += step_ * (1.0f + jitter_ * 0.9f * rng_.bipolar());
        if (phase_ >= 1.0f) {
            phase_ -= fastFloor(phase_);
            holdL_ = std::round(clampf(L[i], -1.0f, 1.0f) * levels_) * inv;
            holdR_ = std::round(clampf(R[i], -1.0f, 1.0f) * levels_) * inv;
        }
        float l = holdL_, r = holdR_;
        if (useTone_) tone_.process(l, r);
        L[i] = (L[i] + (l - L[i]) * mix_) * out_;
        R[i] = (R[i] + (r - R[i]) * mix_) * out_;
    }
}

// ------------------------------------------------------------ lo-fi (tape / vinyl)

void LofiFx::prepare(float sr) {
    sr_ = sr;
    dl_.allocate((int)(0.03f * sr));
    dr_.allocate((int)(0.03f * sr));
    hissLp_.setFreq(6000.0f, sr);
    crackLp_.setFreq(3000.0f, sr);
    reset();
}
void LofiFx::reset() {
    dl_.clear(); dr_.clear(); band_.reset(); band2_.reset(); low_.reset();
    hissLp_.reset(); crackLp_.reset(); wowVal_ = wowTarget_ = 0; crackEnv_ = 0;
}
void LofiFx::update(const float* p, const FxContext&) {
    wow_ = p[lofi::Wow];
    flutter_ = p[lofi::Flutter];
    sat_ = p[lofi::Saturation];
    hiss_ = p[lofi::Hiss];
    crackle_ = p[lofi::Crackle];
    band_.set(Biquad::LP, p[lofi::Bandwidth], 0.707f, 0, sr_);
    band2_.set(Biquad::LP, p[lofi::Bandwidth] * 1.3f, 0.6f, 0, sr_);
    low_.set(Biquad::HP, p[lofi::LowCut], 0.707f, 0, sr_);
    mix_ = p[lofi::Mix];
}
void LofiFx::process(float* L, float* R, int n, const FxContext&) {
    const float base = 0.008f * sr_;
    const float wowInc = 0.55f / sr_, flutInc = 9.0f / sr_;
    const float satGain = 1.0f + sat_ * 4.0f, satComp = 1.0f / (1.0f + sat_ * 1.5f);
    for (int i = 0; i < n; ++i) {
        wowPh_ += wowInc;
        if (wowPh_ >= 1.0f) { wowPh_ -= 1.0f; wowTarget_ = rng_.bipolar(); }
        wowVal_ += 0.00002f * (wowTarget_ - wowVal_);
        flutPh_ += flutInc;
        if (flutPh_ >= 1.0f) flutPh_ -= 1.0f;
        const float mod = wow_ * 0.004f * sr_ * (0.7f * sinCycles(wowPh_) + 0.3f * wowVal_)
                        + flutter_ * 0.00025f * sr_ * sinCycles(flutPh_);
        dl_.push(L[i]);
        dr_.push(R[i]);
        float l = dl_.readCubic(base + mod), r = dr_.readCubic(base + mod);
        if (sat_ > 0.001f) { l = fastTanh(l * satGain) * satComp; r = fastTanh(r * satGain) * satComp; }
        band_.process(l, r);
        band2_.process(l, r);
        low_.process(l, r);
        const float hiss = hissLp_.lp(rng_.bipolar()) * hiss_ * 0.03f;
        if (rng_.unipolar() < crackle_ * 0.0003f) crackEnv_ = (0.1f + 0.4f * rng_.unipolar()) * crackle_ * (rng_.unipolar() < 0.5f ? -1.0f : 1.0f);
        const float crack = crackLp_.lp(crackEnv_);
        crackEnv_ *= 0.6f;
        l += hiss + crack;
        r += hiss + crack;
        L[i] += (l - L[i]) * mix_;
        R[i] += (r - R[i]) * mix_;
    }
}

// ------------------------------------------------------------ exciter

void ExciterFx::prepare(float sr) { sr_ = sr; reset(); }
void ExciterFx::reset() { hp_.reset(); hp2_.reset(); bassLp_.reset(); bassBp_.reset(); }
void ExciterFx::update(const float* p, const FxContext&) {
    hp_.set(Biquad::HP, p[exciter::Freq], 0.707f, 0, sr_);
    hp2_.set(Biquad::HP, p[exciter::Freq], 0.707f, 0, sr_);
    drive_ = 1.0f + p[exciter::Drive] * 9.0f;
    amount_ = p[exciter::Amount] * 1.5f;
    bass_ = p[exciter::BassAmount];
    bassLp_.set(Biquad::LP, p[exciter::BassFreq], 0.707f, 0, sr_);
    bassBp_.set(Biquad::BP, p[exciter::BassFreq] * 2.0f, 0.8f, 0, sr_);
}
void ExciterFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        float hl = L[i], hr = R[i];
        hp_.process(hl, hr);
        // saturated top band (rich in new harmonics), added back at low level
        float gl = fastTanh(hl * drive_) * 0.3f, gr = fastTanh(hr * drive_) * 0.3f;
        hp2_.process(gl, gr);
        float outL = L[i] + gl * amount_, outR = R[i] + gr * amount_;
        if (bass_ > 0.001f) {
            float bl = L[i], br = R[i];
            bassLp_.process(bl, br);
            bl = fastTanh(bl * 4.0f);
            br = fastTanh(br * 4.0f);
            bassBp_.process(bl, br);
            outL += bl * bass_;
            outR += br * bass_;
        }
        L[i] = outL;
        R[i] = outR;
    }
}

}  // namespace fx
}  // namespace hsynth
