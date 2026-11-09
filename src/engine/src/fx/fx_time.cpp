// Time / space and rhythm: delay (digital / analog / tape / reverse /
// diffuse), algorithmic reverb (FDN + spring), convolution reverb, slicer,
// stutter, stereo image.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

namespace {
// Schroeder all-pass on a DelayLine with an integer length.
inline float allpass(DelayLine& d, int len, float x, float g) {
    const float v = d.readInt(len);
    const float y = v - g * x;
    d.push(x + g * y);
    return y;
}
inline void widthMs(float& l, float& r, float width) {
    const float m = 0.5f * (l + r), s = 0.5f * (l - r) * width;
    l = m + s;
    r = m - s;
}
// Spillover blocks: dry/wet targets while on, dry only (wet tail keeps
// playing) while off.
inline void spillTargets(bool on, float mix, float& dry, float& wet, float& feed) {
    mixGains(mix, dry, wet);
    if (!on) dry = 1.0f;
    feed = on ? 1.0f : 0.0f;
}
}  // namespace

// ------------------------------------------------------------ delay

void DelayFx::prepare(float sr) {
    sr_ = sr;
    dl_.allocate((int)(4.3f * sr));
    dr_.allocate((int)(4.3f * sr));
    tL_.setTime(0.12f, sr);
    tR_.setTime(0.12f, sr);
    tL_.reset(timeL_ * sr);
    tR_.reset(timeR_ * sr);
    for (Smoother* s : {&feed_, &dryS_, &wetS_}) s->setTime(0.02f, sr);
    dryS_.reset(1.0f);  // dry passes at full level from the first sample
    static const float kApMs[4] = {4.7f, 7.3f, 11.1f, 15.7f};
    for (int k = 0; k < 4; ++k) {
        apLen_[k] = (int)(kApMs[k] * 0.001f * sr);
        apL_[k].allocate(apLen_[k] + 2);
        apR_[k].allocate(apLen_[k] + 2);
    }
    duckEnv_.set(0.005f, 0.25f, sr);
    timeInit_ = false;
    reset();
}
void DelayFx::reset() {
    dl_.clear(); dr_.clear(); hc_.reset(); lc_.reset(); duckEnv_.reset();
    for (int k = 0; k < 4; ++k) { apL_[k].clear(); apR_[k].clear(); }
    revPos_ = 0;
    tail_ = 0;
    duckG_ = 1;
    timeInit_ = false;  // the next update jumps to the current time instead of gliding
}
void DelayFx::update(const float* p, const FxContext& c) {
    on_ = p[delay::On] > 0.5f;
    type_ = (int)p[delay::Type];
    mode_ = (int)p[delay::Mode];
    float t = p[delay::Time];
    const float beats = syncBeats((int)p[delay::Sync]);
    if (beats > 0.0f) t = beats * 60.0f / c.tempo;
    timeL_ = clampf(t, 0.001f, 2.0f);
    timeR_ = mode_ == 2 ? clampf(t * p[delay::Ratio], 0.001f, 2.0f) : timeL_;
    if (!timeInit_) {
        // start at the preset's time; later changes glide (tape-style)
        tL_.reset(timeL_ * sr_);
        tR_.reset(timeR_ * sr_);
        timeInit_ = true;
    }
    fb_ = p[delay::Feedback];
    float hc = p[delay::HighCut];
    if (type_ == 1) hc = std::min(hc, 4500.0f);   // BBD bandwidth
    if (type_ == 2) hc = std::min(hc, 7000.0f);   // tape head loss
    hc_.set(Biquad::LP, hc, 0.707f, 0, sr_);
    lc_.set(Biquad::HP, p[delay::LowCut], 0.707f, 0, sr_);
    modDepth_ = p[delay::ModDepth];
    lfo_.setRate(p[delay::ModRate], 0, c.tempo, sr_);
    duck_ = p[delay::Ducking];
    width_ = p[delay::Width];
    mix_ = p[delay::Mix];
}
void DelayFx::process(float* L, float* R, int n, const FxContext&) {
    float dryT, wetT, feedT;
    spillTargets(on_, mix_, dryT, wetT, feedT);
    // modulation depth in samples: subtle for digital, chorus-like for BBD/tape
    const float modAmt = modDepth_ * sr_ * (type_ == 0 ? 0.0005f : type_ == 1 ? 0.002f : 0.0015f);
    const float flutInc = 7.3f / sr_;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float feed = feed_.process(feedT), dg = dryS_.process(dryT), wg = wetS_.process(wetT);
        const float inL = L[i] * feed, inR = R[i] * feed;
        // Tape-style time glide. A plain float one-pole stalls a few samples
        // short of a target ~1e4 (its step drops below float resolution), so
        // the step never gets smaller than 0.01 samples (0.6 % pitch).
        auto glide = [](Smoother& s, float target) {
            const float d = target - s.value;
            float step = d * (1.0f - s.coef);
            if (std::fabs(step) < 0.01f) step = d > 0.0f ? std::min(d, 0.01f) : std::max(d, -0.01f);
            s.value += step;
            return s.value;
        };
        const float tl = glide(tL_, timeL_ * sr_), tr = glide(tR_, timeR_ * sr_);
        float mod = lfo_.tick(FxWave::Sine, rng_) * modAmt;
        if (type_ == 2) {
            flutPh_ += flutInc;
            if (flutPh_ >= 1.0f) flutPh_ -= 1.0f;
            mod += sinCycles(flutPh_) * modDepth_ * 0.0002f * sr_;
        }
        float rl, rr;
        if (type_ == 3) {
            // reverse: two readers half a chunk apart, triangular windows sum to 1
            const float T = std::max(64.0f, tl);
            revPos_ += 1.0f;
            if (revPos_ >= T) revPos_ -= T;
            float c2 = revPos_ + 0.5f * T;
            if (c2 >= T) c2 -= T;
            const float w1 = 1.0f - std::fabs(2.0f * revPos_ / T - 1.0f), w2 = 1.0f - w1;
            rl = w1 * dl_.read(2.0f * revPos_ + 1.0f) + w2 * dl_.read(2.0f * c2 + 1.0f);
            rr = w1 * dr_.read(2.0f * revPos_ + 1.0f) + w2 * dr_.read(2.0f * c2 + 1.0f);
        } else {
            rl = dl_.readCubic(std::max(2.0f, tl + mod));  // read before push: read(d) = x[n - d]
            rr = dr_.readCubic(std::max(2.0f, tr + mod));
        }
        float fl = rl, fr = rr;
        hc_.process(fl, fr);
        lc_.process(fl, fr);
        if (type_ == 1 || type_ == 2) { fl = fastTanh(fl * 1.3f) * 0.77f; fr = fastTanh(fr * 1.3f) * 0.77f; }
        if (type_ == 4)
            for (int k = 0; k < 4; ++k) { fl = allpass(apL_[k], apLen_[k], fl, 0.6f); fr = allpass(apR_[k], apLen_[k], fr, 0.6f); }
        fl *= fb_;
        fr *= fb_;
        if (fb_ > 0.9f) { fl = fastTanh(fl); fr = fastTanh(fr); }  // keeps runaway feedback bounded
        if (mode_ == 1) {
            dl_.push(0.5f * (inL + inR) + fr);
            dr_.push(fl);
        } else {
            dl_.push(inL + fl);
            dr_.push(inR + fr);
        }
        float wl = rl, wr = rr;
        widthMs(wl, wr, width_);
        if (duck_ > 0.0f) {
            const float e = duckEnv_.process(0.5f * (inL + inR));
            duckG_ = 1.0f - duck_ * clampf(e * 4.0f, 0.0f, 1.0f);
        }
        wl *= wg * duckG_;
        wr *= wg * duckG_;
        peak = std::max(peak, std::fabs(wl) + std::fabs(wr));
        L[i] = L[i] * dg + wl;
        R[i] = R[i] * dg + wr;
    }
    tail_ = peak;
}

// ------------------------------------------------------------ reverb

namespace {
struct ReverbType { float scale, er, damp, decayMul, mod; bool density; };
const ReverbType kRvTypes[8] = {
    {0.45f, 1.0f, 1.0f, 1.0f, 1.0f, false},  // room
    {1.0f, 0.6f, 1.0f, 1.0f, 1.0f, true},    // hall
    {0.7f, 0.0f, 0.5f, 1.0f, 1.4f, true},    // plate
    {0.6f, 1.2f, 1.1f, 1.0f, 1.0f, true},    // chamber
    {1.6f, 0.8f, 0.9f, 1.0f, 0.8f, true},    // cathedral
    {0.6f, 0.0f, 1.0f, 1.0f, 0.0f, false},   // spring (own structure)
    {1.0f, 0.4f, 0.8f, 1.0f, 1.2f, true},    // shimmer
    {0.3f, 1.0f, 1.0f, 0.35f, 1.0f, true},   // ambience
};
const float kLineMs[8] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.3f};
const float kApMs[8] = {3.1f, 3.7f, 4.3f, 5.1f, 5.9f, 6.7f, 7.3f, 8.1f};
const float kInApMs[4] = {4.77f, 3.59f, 12.73f, 9.31f};
const float kErMs[8] = {7.1f, 11.3f, 16.9f, 23.2f, 29.4f, 36.8f, 43.3f, 52.7f};
constexpr float kMaxScale = 1.6f * 1.6f;
}  // namespace

void ReverbFx::prepare(float sr) {
    sr_ = sr;
    for (int k = 0; k < kLines; ++k) {
        line_[k].allocate((int)(kLineMs[k] * 0.001f * sr * kMaxScale) + 64);
        ap_[k].allocate((int)(kApMs[k] * 0.001f * sr * kMaxScale) + 4);
        modInc_[k] = (0.31f + 0.11f * (float)k) / sr;
        modPh_[k] = 0.13f * (float)k;
    }
    for (int k = 0; k < 4; ++k) {
        inApLen_[k] = (int)(kInApMs[k] * 0.001f * sr);
        inAp_[k].allocate(inApLen_[k] + 2);
    }
    pre_.allocate((int)(0.26f * sr));
    erL_.allocate((int)(0.13f * sr));
    erR_.allocate((int)(0.15f * sr));
    shimL_.allocate((int)(0.1f * sr));
    shimL_.window = 0.08f * sr;
    springL_.allocate((int)(0.1f * sr));
    springR_.allocate((int)(0.1f * sr));
    springLpL_.setFreq(4500.0f, sr);
    springLpR_.setFreq(4200.0f, sr);
    for (Smoother* s : {&feed_, &dryS_, &wetS_}) s->setTime(0.02f, sr);
    dryS_.reset(1.0f);  // dry passes at full level from the first sample
    reset();
}
void ReverbFx::reset() {
    for (int k = 0; k < kLines; ++k) { line_[k].clear(); ap_[k].clear(); lp_[k] = 0; }
    for (auto& a : inAp_) a.clear();
    pre_.clear(); erL_.clear(); erR_.clear(); shimL_.clear(); springL_.clear(); springR_.clear();
    std::fill(std::begin(sapL_), std::end(sapL_), 0.0f);
    std::fill(std::begin(sapR_), std::end(sapR_), 0.0f);
    springLpL_.reset(); springLpR_.reset(); lowCut_.reset();
    shimFb_ = 0;
    tail_ = 0;
}
void ReverbFx::update(const float* p, const FxContext&) {
    on_ = p[reverb::On] > 0.5f;
    type_ = (int)clampf(p[reverb::Type], 0, 7);
    const ReverbType& t = kRvTypes[type_];
    const float size = p[reverb::Size];
    const float scale = t.scale * (0.4f + 1.2f * size);
    freeze_ = p[reverb::Freeze] > 0.5f;
    const float decay = std::max(0.05f, p[reverb::Decay] * t.decayMul);
    for (int k = 0; k < kLines; ++k) {
        lineLen_[k] = kLineMs[k] * 0.001f * sr_ * scale;
        apLen_[k] = std::max(1, (int)(kApMs[k] * 0.001f * sr_ * scale));
        g_[k] = freeze_ ? 1.0f : std::pow(10.0f, -3.0f * lineLen_[k] / (decay * sr_));
    }
    dampA_ = freeze_ ? 1.0f : clampf(1.0f - 0.85f * p[reverb::Damp] * t.damp, 0.05f, 1.0f);
    modDepth_ = p[reverb::Modulation] * 8.0f * t.mod * std::min(scale, 1.5f);
    preDelay_ = p[reverb::PreDelay] * 0.001f * sr_;
    diff_ = 0.75f * p[reverb::Diffusion];
    early_ = p[reverb::Early] * t.er;
    const float erScale = t.scale * (0.5f + size);
    for (int k = 0; k < 8; ++k) {
        erTapL_[k] = std::max(1.0f, kErMs[k] * 0.001f * sr_ * erScale);
        erTapR_[k] = std::max(1.0f, kErMs[k] * 1.13f * 0.001f * sr_ * erScale);
        erGain_[k] = std::pow(0.82f, (float)k) * ((k & 1) ? -0.5f : 0.5f);
    }
    lowCut_.set(Biquad::HP, p[reverb::LowCut], 0.707f, 0, sr_);
    width_ = p[reverb::Width];
    shimmer_ = type_ == 6 ? p[reverb::Shimmer] * 0.6f : 0.0f;
    mix_ = p[reverb::Mix];
    // spring tank: dispersive loop, two slightly different springs
    springLen_ = (0.033f + 0.02f * size) * sr_;
    springG_ = freeze_ ? 0.999f : std::pow(10.0f, -3.0f * springLen_ / (decay * sr_));
}
void ReverbFx::process(float* L, float* R, int n, const FxContext&) {
    float dryT, wetT, feedT;
    spillTargets(on_, mix_, dryT, wetT, feedT);
    if (freeze_) feedT = 0.0f;
    const bool density = kRvTypes[type_].density;
    const float invN = 0.35355339f;  // 1/sqrt(8)
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float feed = feed_.process(feedT), dg = dryS_.process(dryT), wg = wetS_.process(wetT);
        const float x = 0.5f * (L[i] + R[i]) * feed;
        pre_.push(x);
        float xp = preDelay_ >= 1.0f ? pre_.read(preDelay_ + 1.0f) : x;
        xp = lowCut_.l.process(xp);
        float outL, outR;
        if (type_ == 5) {
            // spring: delay loop with a chain of first-order all-passes (chirp)
            float vl = springL_.read(springLen_), vr = springR_.read(springLen_ * 1.17f);
            for (int s = 0; s < 12; ++s) {
                const float a = -0.62f;
                const float yl = a * vl + sapL_[s];
                sapL_[s] = vl - a * yl;
                vl = yl;
                const float yr = a * vr + sapR_[s];
                sapR_[s] = vr - a * yr;
                vr = yr;
            }
            vl = springLpL_.lp(vl);
            vr = springLpR_.lp(vr);
            springL_.push(xp * 0.5f + vl * springG_);
            springR_.push(xp * 0.5f + vr * springG_);
            outL = vl * 1.5f;
            outR = vr * 1.5f;
        } else {
            // input diffusion
            float d = xp;
            for (int k = 0; k < 4; ++k) d = allpass(inAp_[k], inApLen_[k], d, diff_);
            // read the 8 modulated lines, damp, decay
            float y[kLines];
            for (int k = 0; k < kLines; ++k) {
                modPh_[k] += modInc_[k];
                if (modPh_[k] >= 1.0f) modPh_[k] -= 1.0f;
                float v = line_[k].read(lineLen_[k] + modDepth_ * (1.0f + sinCycles(modPh_[k])) + 1.0f);
                lp_[k] += dampA_ * (v - lp_[k]);
                v = lp_[k] * g_[k];
                if (density) v = allpass(ap_[k], apLen_[k], v, 0.5f);
                y[k] = v;
            }
            outL = (y[0] - y[1] + y[2] - y[3] + y[4] - y[5] + y[6] - y[7]) * invN;
            outR = (y[0] + y[1] - y[2] - y[3] + y[4] + y[5] - y[6] - y[7]) * invN;
            // Hadamard feedback matrix (fast Walsh-Hadamard transform)
            for (int h = 1; h < kLines; h <<= 1)
                for (int a = 0; a < kLines; a += h << 1)
                    for (int b = a; b < a + h; ++b) {
                        const float u = y[b], w = y[b + h];
                        y[b] = u + w;
                        y[b + h] = u - w;
                    }
            float inj = d * 0.5f;
            if (shimmer_ > 0.0f) inj += shimFb_ * shimmer_;
            for (int k = 0; k < kLines; ++k) line_[k].push(y[k] * invN + ((k & 1) ? -inj : inj));
            if (shimmer_ > 0.0f) shimFb_ = shimL_.process(0.5f * (outL + outR), 2.0f);
        }
        // early reflections
        if (early_ > 0.001f) {
            erL_.push(xp);
            erR_.push(xp);
            float el = 0, er = 0;
            for (int k = 0; k < 8; ++k) { el += erL_.read(erTapL_[k]) * erGain_[k]; er += erR_.read(erTapR_[k]) * erGain_[k]; }
            outL += el * early_;
            outR += er * early_;
        }
        widthMs(outL, outR, width_);
        outL *= wg * 0.5f;  // wet level calibrated to the pre-rack reverb
        outR *= wg * 0.5f;
        peak = std::max(peak, std::fabs(outL) + std::fabs(outR));
        L[i] = L[i] * dg + outL;
        R[i] = R[i] * dg + outR;
    }
    tail_ = freeze_ ? 1.0f : peak;
}

// ------------------------------------------------------------ convolution reverb

namespace {
// Synthetic stereo IRs: decorrelated noise, exponential decay, darkening
// over time, sparse early reflections.
std::unique_ptr<ConvIr> makeIr(int kind, float sr) {
    struct K { float len, t60, endHz, er; };
    const K kinds[3] = {{0.9f, 0.6f, 4000.0f, 1.0f}, {3.2f, 2.4f, 2500.0f, 0.6f}, {2.4f, 1.8f, 6000.0f, 0.0f}};
    const K& k = kinds[kind];
    const int len = (int)(k.len * sr);
    std::vector<float> h[2];
    Rng rng(0x51EDu + (uint32_t)kind * 77u);
    double energy = 0;
    for (int ch = 0; ch < 2; ++ch) {
        h[ch].assign((size_t)len, 0.0f);
        float lp = 0;
        for (int t = 0; t < len; ++t) {
            const float sec = (float)t / sr;
            const float hz = 16000.0f * std::pow(k.endHz / 16000.0f, std::min(1.0f, sec / k.t60));
            const float a = 1.0f - std::exp(-kTwoPi * hz / sr);
            lp += a * (rng.bipolar() - lp);
            float env = std::pow(10.0f, -3.0f * sec / k.t60);
            if (kind != 2) env *= std::min(1.0f, sec / 0.012f);  // rooms build up, plates start dense
            h[ch][(size_t)t] = lp * env;
        }
        // early reflections
        if (k.er > 0) {
            for (int r = 0; r < 10; ++r) {
                const int t = (int)((0.004f + 0.007f * r + 0.003f * rng.unipolar()) * sr * (kind == 1 ? 1.8f : 1.0f));
                if (t < len) h[ch][(size_t)t] += k.er * 0.5f * std::pow(0.8f, (float)r) * (rng.unipolar() < 0.5f ? -1.0f : 1.0f);
            }
        }
        for (float v : h[ch]) energy += (double)v * v;
    }
    const float g = (float)(1.0 / std::sqrt(energy + 1e-12));
    for (auto& c : h) for (float& v : c) v *= g;
    auto ir = ConvIr::build(h[0].data(), h[1].data(), len, ConvIr::StereoIrMonoIn);
    ir->name = kind == 0 ? "room" : kind == 1 ? "hall" : "plate";
    return ir;
}
}  // namespace

void ConvoFx::prepare(float sr) {
    sr_ = sr;
    for (int k = 0; k < 3; ++k) builtIn_[k] = makeIr(k, sr);
    conv_.prepare((int)(6.0f * sr));
    preL_.allocate((int)(0.26f * sr));
    preR_.allocate((int)(0.26f * sr));
    wetL_.assign(kMaxBlockSize, 0.0f);
    wetR_.assign(kMaxBlockSize, 0.0f);
    for (Smoother* s : {&feed_, &dryS_, &wetS_}) s->setTime(0.02f, sr);
    dryS_.reset(1.0f);  // dry passes at full level from the first sample
    reset();
}
void ConvoFx::reset() { conv_.reset(); preL_.clear(); preR_.clear(); lc_.reset(); hc_.reset(); tail_ = 0; }
void ConvoFx::update(const float* p, const FxContext&) {
    on_ = p[convo::On] > 0.5f;
    ir_ = (int)clampf(p[convo::Ir], 0, 3);
    preDelay_ = p[convo::PreDelay] * 0.001f * sr_;
    useLc_ = p[convo::LowCut] > 21.0f;
    if (useLc_) lc_.set(Biquad::HP, p[convo::LowCut], 0.707f, 0, sr_);
    useHc_ = p[convo::HighCut] < 19900.0f;
    if (useHc_) hc_.set(Biquad::LP, p[convo::HighCut], 0.707f, 0, sr_);
    width_ = p[convo::Width];
    mix_ = p[convo::Mix];
}
void ConvoFx::process(float* L, float* R, int n, const FxContext&) {
    conv_.setIr(ir_ < 3 ? builtIn_[ir_].get() : user_);
    float dryT, wetT, feedT;
    spillTargets(on_, mix_, dryT, wetT, feedT);
    float peak = 0.0f;
    for (int off = 0; off < n; off += kMaxBlockSize) {
        const int m = std::min(kMaxBlockSize, n - off);
        float* wl = wetL_.data();
        float* wr = wetR_.data();
        for (int i = 0; i < m; ++i) {
            const float f = feed_.process(feedT);
            preL_.push(L[off + i] * f);
            preR_.push(R[off + i] * f);
            wl[i] = preDelay_ >= 1.0f ? preL_.read(preDelay_ + 1.0f) : L[off + i] * f;
            wr[i] = preDelay_ >= 1.0f ? preR_.read(preDelay_ + 1.0f) : R[off + i] * f;
        }
        conv_.process(wl, wr, m);
        for (int i = 0; i < m; ++i) {
            float l = wl[i], r = wr[i];
            if (useLc_) lc_.process(l, r);
            if (useHc_) hc_.process(l, r);
            widthMs(l, r, width_);
            const float dg = dryS_.process(dryT), wg = wetS_.process(wetT);
            l *= wg;
            r *= wg;
            peak = std::max(peak, std::fabs(l) + std::fabs(r));
            L[off + i] = L[off + i] * dg + l;
            R[off + i] = R[off + i] * dg + r;
        }
    }
    tail_ = peak;
}

// ------------------------------------------------------------ slicer

namespace {
// 16-step patterns, bit i = step i
uint16_t patternBits(const int* steps) {
    uint16_t b = 0;
    for (int i = 0; i < 16; ++i)
        if (steps[i]) b = (uint16_t)(b | (1u << i));
    return b;
}
const int kTrance1[16] = {1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1};
const int kTrance2[16] = {1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1};
const int kTrance3[16] = {1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1};
const int kStutter[16] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1};
const int kSparse[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0};
uint16_t slicerPattern(int p) {
    switch (p) {
        case 0: return 0xFFFF;
        case 1: return 0xAAAA;
        case 2: return 0xDDDD;
        case 3: return 0xBBBB;
        case 4: return 0x4949;
        case 5: return patternBits(kTrance1);
        case 6: return patternBits(kTrance2);
        case 7: return patternBits(kTrance3);
        case 8: return 0x00FF;
        case 9: return patternBits(kStutter);
        case 10: return patternBits(kSparse);
        default: return 0xFFFF;
    }
}
const float kSliceBeats[6] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
}  // namespace

void SlicerFx::prepare(float sr) { sr_ = sr; reset(); }
void SlicerFx::reset() { gain_ = 1.0f; lastStep_ = -1; }
void SlicerFx::update(const float* p, const FxContext& c) {
    stepBeats_ = kSliceBeats[(int)clampf(p[slicer::Division], 0, 5)];
    pattern_ = (int)p[slicer::Pattern];
    duty_ = p[slicer::Duty];
    depth_ = p[slicer::Depth];
    swing_ = p[slicer::Swing] * 0.5f;
    const float stepSec = stepBeats_ * 60.0f / c.tempo;
    const float edge = std::max(0.0005f, p[slicer::Smooth] * stepSec * 0.3f);
    att_ = tauCoef(edge * 0.5f, sr_);
    rel_ = tauCoef(edge, sr_);
}
void SlicerFx::process(float* L, float* R, int n, const FxContext& c) {
    const double beatsPerSample = c.tempo / 60.0 / sr_;
    const uint16_t bits = slicerPattern(pattern_);
    for (int i = 0; i < n; ++i) {
        const double b = c.beat + beatsPerSample * i;
        const double s = b / stepBeats_;
        const int step = (int)std::floor(s);
        const float pos = (float)(s - step);
        const int idx = ((step % 16) + 16) % 16;
        if (step != lastStep_) {
            lastStep_ = step;
            stepOn_ = pattern_ == 11 ? rng_.unipolar() < 0.65f : ((bits >> idx) & 1) != 0;
        }
        const float start = (idx & 1) ? swing_ : 0.0f;
        const bool open = stepOn_ && pos >= start && pos < start + duty_ * (1.0f - start);
        const float target = open ? 1.0f : 1.0f - depth_;
        gain_ = target + (gain_ - target) * (target > gain_ ? att_ : rel_);
        L[i] *= gain_;
        R[i] *= gain_;
    }
}

// ------------------------------------------------------------ stutter

namespace {
const float kStutBeats[7] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 0.0625f};
}

void StutterFx::prepare(float sr) {
    sr_ = sr;
    bufL_.assign((size_t)(3.0f * sr), 0.0f);
    bufR_.assign((size_t)(3.0f * sr), 0.0f);
    reset();
}
void StutterFx::reset() {
    std::fill(bufL_.begin(), bufL_.end(), 0.0f);
    std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    write_ = 0;
    capturing_ = false;
    fade_ = 0;
}
void StutterFx::update(const float* p, const FxContext& c) {
    active_ = p[stutter::Active] > 0.5f;
    divBeats_ = kStutBeats[(int)clampf(p[stutter::Division], 0, 6)] * 60.0f / c.tempo;  // seconds
    reverse_ = p[stutter::Reverse] > 0.5f;
    gate_ = p[stutter::Gate];
    mix_ = p[stutter::Mix];
}
void StutterFx::process(float* L, float* R, int n, const FxContext&) {
    const int size = (int)bufL_.size();
    const float fadeStep = 1.0f / (0.005f * sr_);
    const int edge = std::max(1, (int)(0.002f * sr_));
    for (int i = 0; i < n; ++i) {
        if (active_ && !capturing_ && fade_ <= 0.0f) {
            capturing_ = true;
            loopLen_ = std::max(edge * 4, std::min(size - 1, (int)(divBeats_ * sr_)));
            loopStart_ = (write_ - loopLen_ + size) % size;
            loopPos_ = 0;
        }
        if (!capturing_) {
            bufL_[(size_t)write_] = L[i];
            bufR_[(size_t)write_] = R[i];
            write_ = (write_ + 1) % size;
        }
        fade_ = clampf(fade_ + (active_ && capturing_ ? fadeStep : -fadeStep), 0.0f, 1.0f);
        if (capturing_) {
            const int len = loopLen_;
            const int k = reverse_ ? len - 1 - loopPos_ : loopPos_;
            const int at = (loopStart_ + k) % size;
            const int gateLen = std::max(edge * 2, (int)(gate_ * (float)len));
            float w = 0.0f;
            if (loopPos_ < gateLen) {
                w = 1.0f;
                if (loopPos_ < edge) w = (float)loopPos_ / (float)edge;
                else if (loopPos_ > gateLen - edge) w = (float)(gateLen - loopPos_) / (float)edge;
            }
            const float m = fade_ * mix_;
            L[i] += (bufL_[(size_t)at] * w - L[i]) * m;
            R[i] += (bufR_[(size_t)at] * w - R[i]) * m;
            if (++loopPos_ >= len) loopPos_ = 0;
            if (!active_ && fade_ <= 0.0f) capturing_ = false;
        }
    }
}

// ------------------------------------------------------------ stereo image

void StereoFx::prepare(float sr) {
    sr_ = sr;
    dr_.allocate((int)(0.035f * sr));
    reset();
}
void StereoFx::reset() { sideHp_.reset(); sideHp2_.reset(); dr_.clear(); }
void StereoFx::update(const float* p, const FxContext&) {
    width_ = p[stereo::Width];
    const float pan = p[stereo::Pan];
    panL_ = pan > 0.0f ? 1.0f - pan : 1.0f;
    panR_ = pan < 0.0f ? 1.0f + pan : 1.0f;
    haas_ = p[stereo::Haas] * 0.001f * sr_;
    monoBass_ = p[stereo::MonoBass] > 21.0f;
    if (monoBass_) {
        sideHp_.set(Biquad::HP, p[stereo::MonoBass], 0.707f, 0, sr_);
        sideHp2_.copyCoefs(sideHp_.l);
    }
    out_ = dbToLin(p[stereo::Output]);
}
void StereoFx::process(float* L, float* R, int n, const FxContext&) {
    for (int i = 0; i < n; ++i) {
        const float m = 0.5f * (L[i] + R[i]);
        float s = 0.5f * (L[i] - R[i]) * width_;
        if (monoBass_) s = sideHp2_.process(sideHp_.l.process(s));
        float l = m + s, r = m - s;
        dr_.push(r);
        if (haas_ >= 1.0f) r = dr_.read(haas_ + 1.0f);
        L[i] = l * panL_ * out_;
        R[i] = r * panR_ * out_;
    }
}

}  // namespace fx
}  // namespace hsynth
