// v0.6 effect blocks: de-esser, isolator, step filter, multi-tap delay,
// doubler, granular, tape stop, pump, resonator, slow attack (swell), talk
// box, gated / reverse reverb. Same contract as the other blocks (block.h):
// everything is allocated in prepare(), process() is allocation-free.
#include <algorithm>
#include <cmath>

#include "fx_blocks.h"
#include "hsynth/dsp/svf.h"

namespace hsynth {
namespace fx {

namespace {

// note divisions shared by step filter / pump ("1/1" .. "1/32"), in beats
const float kDivBeats[8] = {4.0f, 2.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
float divBeats(float p) { return kDivBeats[(int)clampf(p, 0.0f, 7.0f)]; }

inline void panGains(float pan, float& l, float& r) {
    const float t = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.125f;  // equal power
    l = sinCycles(t + 0.25f);
    r = sinCycles(t);
}

inline float softClip(float x) { return x / (1.0f + std::fabs(x) * 0.25f); }

// ================================================================ de-esser

class DeEsserFx : public FxBlock {
public:
    // Dynamic high shelf: the sidechain band (around Freq) drives the cut, so
    // the signal itself is never split / recombined (no phase colouring).
    void prepare(float sr) override { sr_ = sr; det_.set(0.0005f, 0.06f, sr); reset(); }
    void reset() override { sc_.reset(); shelf_.reset(); shelf_.identity(); det_.reset(); grDb_ = 0; lastDb_ = 0; counter_ = 0; }
    void update(const float* p, const FxContext&) override {
        freq_ = p[deesser::Freq];
        sc_.set(Biquad::BP, freq_, 1.4f, 0, sr_);
        thresh_ = p[deesser::Thresh];
        range_ = p[deesser::Range];
        listen_ = p[deesser::Listen] > 0.5f;
        lastDb_ = 1e9f;  // recompute the shelf
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        for (int i = 0; i < n; ++i) {
            const float s = sc_.process(0.5f * (L[i] + R[i]));
            const float lvl = fastLinToDb(det_.process(s) + 1e-9f);
            const float over = lvl - thresh_;
            const float gr = over > 0.0f ? std::min(range_, over * 0.75f) : 0.0f;  // 4:1 above the threshold
            grDb_ = gr > grDb_ ? gr : gr + (grDb_ - gr) * 0.9995f;                // fast attack, ~40 ms release
            if (--counter_ <= 0) {
                counter_ = 16;
                if (std::fabs(grDb_ - lastDb_) > 0.05f) {
                    lastDb_ = grDb_;
                    if (grDb_ < 0.05f) shelf_.identity();
                    else shelf_.set(Biquad::HighShelf, freq_ * 0.75f, 0.707f, -grDb_, sr_);
                }
            }
            if (listen_) { L[i] = R[i] = s; continue; }
            shelf_.process(L[i], R[i]);
        }
    }
private:
    float sr_ = 48000, freq_ = 6500, thresh_ = -30, range_ = 10, grDb_ = 0, lastDb_ = 0;
    int counter_ = 0;
    bool listen_ = false;
    Biquad sc_;
    StereoBiquad shelf_;
    EnvFollower det_;
};

// ================================================================ isolator

class IsolatorFx : public FxBlock {
public:
    // Linkwitz-Riley 4th-order 3-band split (low | mid | high); the low band
    // goes through the high crossover's all-pass so the bands sum flat.
    void prepare(float sr) override {
        sr_ = sr;
        for (auto& s : sm_) s.setTime(0.01f, sr);
        reset();
    }
    void reset() override {
        for (auto& f : f_) f.reset();
        for (auto& s : sm_) s.reset(1.0f);
    }
    void update(const float* p, const FxContext&) override {
        const float xl = p[isolator::XLow], xh = std::max(p[isolator::XHigh], xl * 1.5f);
        for (int k = 0; k < 2; ++k) {
            f_[kLpL + k].set(Biquad::LP, xl, 0.7071f, 0, sr_);
            f_[kHpL + k].set(Biquad::HP, xl, 0.7071f, 0, sr_);
            f_[kLpH + k].set(Biquad::LP, xh, 0.7071f, 0, sr_);
            f_[kHpH + k].set(Biquad::HP, xh, 0.7071f, 0, sr_);
        }
        f_[kAp].set(Biquad::AllPass, xh, 0.5f, 0, sr_);
        for (int b = 0; b < 3; ++b) {
            const float db = p[isolator::Low + b];
            tgt_[b] = db <= -59.5f ? 0.0f : dbToLin(db);  // the bottom of the range kills the band
        }
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        for (int i = 0; i < n; ++i) {
            const float g0 = sm_[0].process(tgt_[0]), g1 = sm_[1].process(tgt_[1]), g2 = sm_[2].process(tgt_[2]);
            float lo[2] = {L[i], R[i]}, rest[2] = {L[i], R[i]};
            f_[kLpL].process(lo[0], lo[1]); f_[kLpL + 1].process(lo[0], lo[1]); f_[kAp].process(lo[0], lo[1]);
            f_[kHpL].process(rest[0], rest[1]); f_[kHpL + 1].process(rest[0], rest[1]);
            float mid[2] = {rest[0], rest[1]}, hi[2] = {rest[0], rest[1]};
            f_[kLpH].process(mid[0], mid[1]); f_[kLpH + 1].process(mid[0], mid[1]);
            f_[kHpH].process(hi[0], hi[1]); f_[kHpH + 1].process(hi[0], hi[1]);
            // LR4 bands are in phase (low, mid) / inverted (high) at the crossover: sum with +, +, +
            L[i] = lo[0] * g0 + mid[0] * g1 + hi[0] * g2;
            R[i] = lo[1] * g0 + mid[1] * g1 + hi[1] * g2;
        }
    }
private:
    enum { kLpL = 0, kHpL = 2, kLpH = 4, kHpH = 6, kAp = 8, kCount = 9 };
    float sr_ = 48000, tgt_[3] = {1, 1, 1};
    StereoBiquad f_[kCount];
    Smoother sm_[3];
};

// ================================================================ step filter

class StepFilterFx : public FxBlock {
public:
    void prepare(float sr) override { sr_ = sr; mixR_.prepare(sr, 1.0f); reset(); }
    void reset() override { f_[0].reset(); f_[1].reset(); logF_ = -1.0f; }
    void update(const float* p, const FxContext&) override {
        type_ = (int)p[stepfilter::Type];
        beats_ = divBeats(p[stepfilter::Division]);
        steps_ = (int)clampf(p[stepfilter::Steps], 2, 16);
        base_ = p[stepfilter::Base];
        depth_ = p[stepfilter::Depth];
        reso_ = p[stepfilter::Reso];
        glide_ = p[stepfilter::Glide];
        for (int k = 0; k < 16; ++k) step_[k] = p[stepfilter::Step1 + k];
        mixR_.target = p[stepfilter::Mix];
    }
    void process(float* L, float* R, int n, const FxContext& c) override {
        const double beatInc = c.tempo / 60.0 / c.sampleRate;
        // glide: time constant up to half a step
        const float stepSec = beats_ * 60.0f / std::max(20.0f, c.tempo);
        const float a = glide_ <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / std::max(1.0f, glide_ * 0.5f * stepSec * sr_));
        for (int i = 0; i < n; ++i) {
            if ((i & 7) == 0) {
                const double b = c.beat + beatInc * i;
                const int idx = (int)((int64_t)std::floor(b / beats_) % steps_);
                const float target = std::log2(base_) + depth_ * step_[(idx + steps_) % steps_];
                logF_ = logF_ < 0.0f ? target : logF_ + (target - logF_) * std::min(1.0f, a * 8.0f);
                const float fc = std::exp2(logF_);
                f_[0].setCoefs(fc, reso_ * 0.95f, sr_);
                f_[1].setCoefs(fc, reso_ * 0.95f, sr_);
            }
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            float* io[2] = {L + i, R + i};
            for (int ch = 0; ch < 2; ++ch) {
                const Svf::Out o = f_[ch].tick(*io[ch]);
                const float y = type_ == 0 ? o.lp : type_ == 1 ? o.bp : o.hp;
                *io[ch] = *io[ch] * dg + y * wg;
            }
        }
    }
private:
    float sr_ = 48000, beats_ = 0.25f, base_ = 200, depth_ = 5, reso_ = 0.6f, glide_ = 0.2f, logF_ = -1;
    int type_ = 0, steps_ = 16;
    float step_[16] = {};
    Svf f_[2];
    MixRamp mixR_;
};

// ================================================================ multi-tap delay

class MultiTapFx : public FxBlock {
public:
    void prepare(float sr) override {
        sr_ = sr;
        line_.allocate((int)(2.1f * sr));
        mixR_.prepare(sr, 0.35f);
        reset();
    }
    void reset() override { line_.clear(); tone_.reset(); fb_ = 0; }
    void update(const float* p, const FxContext& c) override {
        const float beats = syncBeats((int)p[multitap::Sync]);
        timeS_ = beats > 0.0f ? beats * 60.0f / std::max(20.0f, c.tempo) : p[multitap::Time] * 0.001f;
        timeS_ = clampf(timeS_, 0.005f, 2.0f);
        feedback_ = p[multitap::Feedback];
        tone_.setFreq(p[multitap::Tone], sr_);
        mixR_.target = p[multitap::Mix];
        for (int k = 0; k < 4; ++k) {
            t_[k] = std::max(0.0005f, p[multitap::Tap1Time + 3 * k]);
            panGains(p[multitap::Tap1Pan + 3 * k], gl_[k], gr_[k]);
            const float lv = p[multitap::Tap1Level + 3 * k];
            gl_[k] *= lv;
            gr_[k] *= lv;
        }
        last_ = 0;
        for (int k = 1; k < 4; ++k)
            if (t_[k] > t_[last_]) last_ = k;
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        const float base = timeS_ * sr_;
        for (int i = 0; i < n; ++i) {
            line_.push(0.5f * (L[i] + R[i]) + fb_);
            float wl = 0, wr = 0, lastTap = 0;
            for (int k = 0; k < 4; ++k) {
                const float v = line_.read(std::max(1.0f, base * t_[k]));
                if (k == last_) lastTap = v;
                wl += v * gl_[k];
                wr += v * gr_[k];
            }
            fb_ = softClip(tone_.lp(lastTap) * feedback_);
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            L[i] = L[i] * dg + wl * wg;
            R[i] = R[i] * dg + wr * wg;
        }
    }
    bool spillover() const override { return false; }
private:
    float sr_ = 48000, timeS_ = 0.5f, feedback_ = 0.3f, fb_ = 0;
    float t_[4] = {0.25f, 0.5f, 0.75f, 1}, gl_[4] = {}, gr_[4] = {};
    int last_ = 3;
    DelayLine line_;
    OnePole tone_;
    MixRamp mixR_;
};

// ================================================================ doubler

class DoublerFx : public FxBlock {
public:
    void prepare(float sr) override {
        sr_ = sr;
        for (auto& g : sh_) g.allocate((int)(0.07f * sr));
        mixR_.prepare(sr, 0.5f);
        reset();
    }
    void reset() override {
        for (auto& g : sh_) g.clear();
    }
    void update(const float* p, const FxContext&) override {
        const float c = p[doubler::Detune];
        up_ = std::exp2(c / 1200.0f);
        down_ = std::exp2(-c / 1200.0f);
        sh_[0].window = sh_[1].window = clampf(p[doubler::Delay], 10, 60) * 0.001f * sr_;
        width_ = p[doubler::Width];
        mixR_.target = p[doubler::Mix];
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        for (int i = 0; i < n; ++i) {
            const float a = sh_[0].process(L[i], up_), b = sh_[1].process(R[i], down_);
            const float mid = 0.5f * (a + b), side = 0.5f * (a - b) * width_;
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            L[i] = L[i] * dg + (mid + side) * wg;
            R[i] = R[i] * dg + (mid - side) * wg;
        }
    }
private:
    float sr_ = 48000, up_ = 1, down_ = 1, width_ = 1;
    GrainShifter sh_[2];
    MixRamp mixR_;
};

// ================================================================ granular

class GranularFx : public FxBlock {
public:
    static constexpr int kGrains = 24;
    void prepare(float sr) override {
        sr_ = sr;
        cap_ = (int)(3.0f * sr);
        buf_[0].assign((size_t)cap_, 0.0f);
        buf_[1].assign((size_t)cap_, 0.0f);
        mixR_.prepare(sr, 0.5f);
        reset();
    }
    void reset() override {
        std::fill(buf_[0].begin(), buf_[0].end(), 0.0f);
        std::fill(buf_[1].begin(), buf_[1].end(), 0.0f);
        write_ = 0;
        for (auto& g : grains_) g.left = 0;
        untilNext_ = 0;
        fbL_ = fbR_ = 0;
    }
    void update(const float* p, const FxContext&) override {
        size_ = clampf(p[granular::Size], 10, 500) * 0.001f * sr_;
        interval_ = sr_ / clampf(p[granular::Density], 1, 60);
        ratio_ = std::exp2(p[granular::Pitch] / 12.0f);
        spray_ = p[granular::Spray];
        spread_ = p[granular::Spread];
        reverse_ = p[granular::Reverse];
        feedback_ = p[granular::Feedback];
        freeze_ = p[granular::Freeze] > 0.5f;
        mixR_.target = p[granular::Mix];
        // overlap -> output normalisation
        norm_ = 1.0f / std::sqrt(std::max(1.0f, size_ / interval_));
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        for (int i = 0; i < n; ++i) {
            if (!freeze_) {
                buf_[0][(size_t)write_] = L[i] + fbL_;
                buf_[1][(size_t)write_] = R[i] + fbR_;
                write_ = (write_ + 1) % cap_;
            }
            if (--untilNext_ <= 0) {
                spawn();
                untilNext_ = (int)(interval_ * (0.75f + 0.5f * (0.5f + 0.5f * rng_.bipolar())));
            }
            float wl = 0, wr = 0;
            for (auto& g : grains_) {
                if (g.left <= 0) continue;
                const float t = 1.0f - (float)g.left / (float)g.len;  // 0..1
                const float w = sinCycles(t * 0.5f);                     // sin(pi t)
                const float win = w * w;
                const float pos = g.pos;
                int i0 = (int)fastFloor(pos);
                const float f = pos - (float)i0;
                i0 = ((i0 % cap_) + cap_) % cap_;
                const int i1 = (i0 + 1) % cap_;
                const float vl = buf_[0][(size_t)i0] + f * (buf_[0][(size_t)i1] - buf_[0][(size_t)i0]);
                const float vr = buf_[1][(size_t)i0] + f * (buf_[1][(size_t)i1] - buf_[1][(size_t)i0]);
                wl += vl * win * g.gl;
                wr += vr * win * g.gr;
                g.pos += g.rate;
                --g.left;
            }
            wl *= norm_;
            wr *= norm_;
            fbL_ = softClip(wl * feedback_);
            fbR_ = softClip(wr * feedback_);
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            L[i] = L[i] * dg + wl * wg;
            R[i] = R[i] * dg + wr * wg;
        }
    }
private:
    struct Grain { double pos = 0; float rate = 1, gl = 1, gr = 1; int left = 0, len = 1; };
    void spawn() {
        Grain* g = nullptr;
        for (auto& x : grains_)
            if (x.left <= 0) { g = &x; break; }
        if (!g) return;
        const bool rev = reverse_ > 0.0f && (0.5f + 0.5f * rng_.bipolar()) < reverse_;
        const float rate = rev ? -ratio_ : ratio_;
        const int len = std::max(16, (int)size_);
        // start far enough behind the write head that the grain never reads
        // samples that have not been written yet
        const float need = rate > 1.0f ? (rate - 1.0f) * (float)len : 0.0f;
        const float back = 64.0f + need + (rev ? (float)len * ratio_ : 0.0f) + spray_ * (0.5f + 0.5f * rng_.bipolar()) * sr_;
        g->pos = (double)write_ - std::min(back, (float)cap_ - (float)len * 2.0f - 64.0f);
        g->rate = rate;
        g->len = g->left = len;
        panGains(spread_ * rng_.bipolar(), g->gl, g->gr);
        g->gl *= 1.4142f;
        g->gr *= 1.4142f;
    }
    float sr_ = 48000, size_ = 4000, interval_ = 3000, ratio_ = 1, spray_ = 0.3f, spread_ = 0.7f, reverse_ = 0,
          feedback_ = 0, norm_ = 1, fbL_ = 0, fbR_ = 0;
    bool freeze_ = false;
    int cap_ = 1, write_ = 0, untilNext_ = 0;
    std::vector<float> buf_[2];
    Grain grains_[kGrains];
    Rng rng_{0x6A41u};
    MixRamp mixR_;
};

// ================================================================ tape stop

class TapeStopFx : public FxBlock {
public:
    void prepare(float sr) override {
        sr_ = sr;
        dl_.allocate((int)(6.0f * sr));
        dr_.allocate((int)(6.0f * sr));
        reset();
    }
    void reset() override { dl_.clear(); dr_.clear(); speed_ = 1; delay_ = 1; xfade_ = 0; }
    void update(const float* p, const FxContext&) override {
        stop_ = p[tapestop::Stop] > 0.5f;
        time_ = p[tapestop::Time];
        curve_ = p[tapestop::Curve];
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        const float step = 1.0f / (time_ * sr_);
        const int maxDelay = dl_.capacity() - 4;
        for (int i = 0; i < n; ++i) {
            dl_.push(L[i]);
            dr_.push(R[i]);
            if (stop_) {
                progress_ = std::min(1.0f, progress_ + step);
                // curve 0: linear slow-down, 1: stays fast, then drops (motor inertia)
                const float x = progress_;
                speed_ = 1.0f - (curve_ * x * x + (1.0f - curve_) * x);
                delay_ = std::min((float)maxDelay, delay_ + (1.0f - speed_));
                const float g = speed_ < 0.02f ? speed_ * 50.0f : 1.0f;  // silence at a standstill
                L[i] = dl_.readCubic(delay_) * g;
                R[i] = dr_.readCubic(delay_) * g;
                xfade_ = 1.0f;
            } else {
                progress_ = 0.0f;
                if (xfade_ > 0.0f) {  // released: back to the live signal with a short crossfade
                    const float a = dl_.readCubic(delay_), b = dr_.readCubic(delay_);
                    L[i] = L[i] + (a * speed_ - L[i]) * xfade_;
                    R[i] = R[i] + (b * speed_ - R[i]) * xfade_;
                    xfade_ = std::max(0.0f, xfade_ - 1.0f / (0.01f * sr_));
                    if (xfade_ <= 0.0f) { delay_ = 1.0f; speed_ = 1.0f; }
                }
            }
        }
    }
private:
    float sr_ = 48000, time_ = 0.8f, curve_ = 0.5f, speed_ = 1, delay_ = 1, progress_ = 0, xfade_ = 0;
    bool stop_ = false;
    DelayLine dl_, dr_;
};

// ================================================================ pump

class PumpFx : public FxBlock {
public:
    void prepare(float sr) override { sr_ = sr; reset(); }
    void reset() override { g_ = 1.0f; }
    void update(const float* p, const FxContext&) override {
        beats_ = divBeats(p[pump::Division]);
        depth_ = p[pump::Depth];
        release_ = p[pump::Release];
        curve_ = p[pump::Curve];
    }
    void process(float* L, float* R, int n, const FxContext& c) override {
        const double beatInc = c.tempo / 60.0 / c.sampleRate;
        const float stepSec = beats_ * 60.0f / std::max(20.0f, c.tempo);
        const float att = std::min(0.25f, 0.004f / stepSec);  // ~4 ms duck
        const float k = 1.0f + 4.0f * curve_;
        for (int i = 0; i < n; ++i) {
            const double b = (c.beat + beatInc * i) / beats_;
            const float t = (float)(b - std::floor(b));
            float env;  // 1 = ducked
            if (t < att) env = t / att;
            else env = 1.0f - std::pow(std::min(1.0f, (t - att) / release_), 1.0f / k);
            const float g = 1.0f - depth_ * clampf(env, 0.0f, 1.0f);
            g_ = g + (g_ - g) * 0.9f;
            L[i] *= g_;
            R[i] *= g_;
        }
    }
private:
    float sr_ = 48000, beats_ = 1, depth_ = 0.7f, release_ = 0.6f, curve_ = 0.5f, g_ = 1;
};

// ================================================================ resonator

class ResonatorFx : public FxBlock {
public:
    void prepare(float sr) override {
        sr_ = sr;
        for (auto& ch : comb_)
            for (auto& d : ch) d.allocate((int)(sr / 25.0f) + 8);
        mixR_.prepare(sr, 0.4f);
        reset();
    }
    void reset() override {
        for (auto& ch : comb_)
            for (auto& d : ch) d.clear();
        for (auto& ch : damp_)
            for (auto& d : ch) d = 0;
    }
    void update(const float* p, const FxContext& c) override {
        static const float kChords[8][4] = {{0, 0.08f, -0.08f, 12}, {-12, 0, 12, 24}, {0, 7, 12, 19}, {0, 4, 7, 12},
                                            {0, 3, 7, 12},          {0, 5, 7, 12},     {0, 4, 7, 11}, {0, 3, 7, 10}};
        const int chord = (int)clampf(p[resonator::Chord], 0, 7);
        const float root = p[resonator::KeyFollow] > 0.5f ? c.lastNoteHz : 440.0f * std::exp2((p[resonator::Root] - 69.0f) / 12.0f);
        const float decay = p[resonator::Decay];
        for (int k = 0; k < 4; ++k) {
            float f = root * std::exp2(kChords[chord][k] / 12.0f);
            while (f < 30.0f) f *= 2.0f;
            while (f > sr_ * 0.25f) f *= 0.5f;
            period_[k] = sr_ / f;
            fb_[k] = std::pow(10.0f, -3.0f * (period_[k] / sr_) / decay);  // -60 dB after `decay` seconds
        }
        dampA_ = 0.05f + 0.95f * p[resonator::Tone];
        mixR_.target = p[resonator::Mix];
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            for (int ch = 0; ch < 2; ++ch) {
                const float x = io[ch][i];
                float w = 0;
                for (int k = 0; k < 4; ++k) {
                    // slight detune between the channels widens the image
                    const float d = period_[k] * (ch ? 1.0007f : 1.0f);
                    float y = comb_[ch][k].read(d);
                    damp_[ch][k] += dampA_ * (y - damp_[ch][k]);
                    y = x * 0.25f + fb_[k] * damp_[ch][k];
                    comb_[ch][k].push(softClip(y));
                    w += y;
                }
                io[ch][i] = x * dg + softClip(w * 0.5f) * wg;
            }
        }
    }
private:
    float sr_ = 48000, period_[4] = {100, 100, 100, 100}, fb_[4] = {}, dampA_ = 0.6f, damp_[2][4] = {};
    DelayLine comb_[2][4];
    MixRamp mixR_;
};

// ================================================================ swell

class SwellFx : public FxBlock {
public:
    void prepare(float sr) override { sr_ = sr; det_.set(0.002f, 0.08f, sr); reset(); }
    void reset() override { det_.reset(); gain_ = 1.0f; armed_ = true; }
    void update(const float* p, const FxContext&) override {
        step_ = 1.0f / (p[swell::Attack] * 0.001f * sr_);
        thresh_ = dbToLin(p[swell::Thresh]);
        curve_ = 1.0f + 2.0f * p[swell::Curve];
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        for (int i = 0; i < n; ++i) {
            const float e = det_.process(0.5f * (std::fabs(L[i]) + std::fabs(R[i])));
            if (e < thresh_ * 0.5f) armed_ = true;           // quiet: the next note swells in
            if (armed_ && e > thresh_) { armed_ = false; gain_ = 0.0f; }
            gain_ = std::min(1.0f, gain_ + step_);
            const float g = std::pow(gain_, curve_);
            L[i] *= g;
            R[i] *= g;
        }
    }
private:
    float sr_ = 48000, step_ = 0.001f, thresh_ = 0.01f, curve_ = 2, gain_ = 1;
    bool armed_ = true;
    EnvFollower det_;
};

// ================================================================ talk box

class TalkboxFx : public FxBlock {
public:
    void prepare(float sr) override { sr_ = sr; det_.set(0.005f, 0.15f, sr); mixR_.prepare(sr, 1.0f); reset(); }
    void reset() override {
        for (auto& b : bp_) b.reset();
        det_.reset();
        lfo_.reset();
        counter_ = 0;
    }
    void update(const float* p, const FxContext& c) override {
        mode_ = (int)p[talkbox::Mode];
        vowel_ = p[talkbox::Vowel];
        sync_ = (int)p[talkbox::Sync];
        lfo_.setRate(p[talkbox::Rate], sync_, c.tempo, sr_);
        depth_ = p[talkbox::Depth];
        sens_ = p[talkbox::Sens];
        q_ = 3.0f + 17.0f * p[talkbox::Reso];
        mixR_.target = p[talkbox::Mix];
    }
    void process(float* L, float* R, int n, const FxContext& c) override {
        // formants (Hz) and gains of a, e, i, o, u
        static const float kF[5][3] = {{800, 1150, 2900}, {400, 1700, 2600}, {300, 2200, 3000}, {450, 800, 2830}, {325, 700, 2530}};
        static const float kG[5][3] = {{1.0f, 0.6f, 0.25f}, {1.0f, 0.5f, 0.3f}, {1.0f, 0.35f, 0.3f}, {1.0f, 0.5f, 0.15f}, {1.0f, 0.3f, 0.1f}};
        if (sync_ > 0) lfo_.syncTo(c.beat, sync_);
        for (int i = 0; i < n; ++i) {
            const float env = det_.process(0.5f * (L[i] + R[i]));
            const float lv = lfo_.tick(FxWave::Triangle, rng_);
            if (counter_-- <= 0) {
                counter_ = 15;
                float pos = vowel_;
                if (mode_ == 1) pos = vowel_ + depth_ * (0.5f + 0.5f * lv) * (1.0f - vowel_);
                if (mode_ == 2) pos = vowel_ + depth_ * clampf(env * (1.0f + 15.0f * sens_), 0.0f, 1.0f) * (1.0f - vowel_);
                pos = clampf(pos, 0.0f, 1.0f) * 4.0f;
                const int a = std::min(3, (int)pos);
                const float t = pos - (float)a;
                for (int k = 0; k < 3; ++k) {
                    const float f = kF[a][k] + (kF[a + 1][k] - kF[a][k]) * t;
                    gain_[k] = kG[a][k] + (kG[a + 1][k] - kG[a][k]) * t;
                    bp_[k].set(Biquad::BP, f, q_, 0, sr_);
                }
            }
            float yl = 0, yr = 0;
            for (int k = 0; k < 3; ++k) {
                float a = L[i], b = R[i];
                bp_[k].process(a, b);
                yl += a * gain_[k];
                yr += b * gain_[k];
            }
            const float makeup = 1.0f + 0.15f * q_;
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            L[i] = L[i] * dg + softClip(yl * makeup) * wg;
            R[i] = R[i] * dg + softClip(yr * makeup) * wg;
        }
    }
    void retrigger() override { lfo_.reset(); }
private:
    float sr_ = 48000, vowel_ = 0, depth_ = 1, sens_ = 0.5f, q_ = 10, gain_[3] = {1, 0.5f, 0.25f};
    int mode_ = 1, sync_ = 0, counter_ = 0;
    StereoBiquad bp_[3];
    EnvFollower det_;
    FxLfo lfo_;
    Rng rng_;
    MixRamp mixR_;
};

// ================================================================ gated / reverse reverb

class GatedVerbFx : public FxBlock {
public:
    static constexpr int kTaps = 32;
    void prepare(float sr) override {
        sr_ = sr;
        line_.allocate((int)(1.3f * sr));
        for (auto& ch : ap_)
            for (auto& a : ch) a.allocate((int)(0.02f * sr));
        mixR_.prepare(sr, 0.35f);
        reset();
    }
    void reset() override {
        line_.clear();
        for (auto& ch : ap_)
            for (auto& a : ch) a.clear();
        lpL_.reset();
        lpR_.reset();
    }
    void update(const float* p, const FxContext&) override {
        const int mode = (int)p[gatedverb::Mode];
        const float size = p[gatedverb::Size] * 0.001f * sr_;
        const float pre = p[gatedverb::PreDelay] * 0.001f * sr_;
        Rng r(0x51EDu);
        float sum = 0;
        for (int k = 0; k < kTaps; ++k) {
            const float t = ((float)k + 0.5f + 0.4f * r.bipolar()) / (float)kTaps;  // jittered, 0..1 across the length
            tap_[k] = 1.0f + pre + t * size;
            float g = 1.0f;
            if (mode == 0) g = 1.0f - 0.25f * t;                // gated: flat, then cut
            else if (mode == 1) g = 0.08f + 0.92f * t * t;       // reverse: swelling
            else g = std::exp(-4.0f * t);                        // decay: short dense room
            g *= 0.75f + 0.25f * r.bipolar();
            gain_[k] = g;
            side_[k] = (k & 1) ? 1.0f : -1.0f;
            sum += g * g;
        }
        const float norm = 1.0f / std::sqrt(std::max(1e-6f, sum));
        for (auto& g : gain_) g *= norm * 2.0f;
        apFb_ = 0.3f + 0.4f * p[gatedverb::Density];
        static const float kApMs[2][2] = {{4.77f, 7.13f}, {5.31f, 6.67f}};
        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < 2; ++k) apLen_[ch][k] = kApMs[ch][k] * 0.001f * sr_;
        lpL_.setFreq(18000.0f * std::exp2(-5.0f * p[gatedverb::Damp]), sr_);
        lpR_.a = lpL_.a;
        width_ = p[gatedverb::Width];
        mixR_.target = p[gatedverb::Mix];
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        for (int i = 0; i < n; ++i) {
            line_.push(0.5f * (L[i] + R[i]));
            float wl = 0, wr = 0;
            for (int k = 0; k < kTaps; ++k) {
                const float v = line_.readInt((int)tap_[k]) * gain_[k];
                const float s = side_[k] * width_;
                wl += v * (1.0f - 0.5f * s);
                wr += v * (1.0f + 0.5f * s);
            }
            wl = allpass(0, wl);
            wr = allpass(1, wr);
            wl = lpL_.lp(wl);
            wr = lpR_.lp(wr);
            float dg, wg;
            mixGains(mixR_.next(), dg, wg);
            L[i] = L[i] * dg + wl * wg;
            R[i] = R[i] * dg + wr * wg;
        }
    }
private:
    float allpass(int ch, float x) {
        for (int k = 0; k < 2; ++k) {
            DelayLine& d = ap_[ch][k];
            const float z = d.read(apLen_[ch][k]);
            const float y = -apFb_ * x + z;
            d.push(x + apFb_ * y);
            x = y;
        }
        return x;
    }
    float sr_ = 48000, tap_[kTaps] = {}, gain_[kTaps] = {}, side_[kTaps] = {}, apFb_ = 0.5f, apLen_[2][2] = {}, width_ = 1;
    DelayLine line_;
    DelayLine ap_[2][2];
    OnePole lpL_, lpR_;
    MixRamp mixR_;
};


// ================================================================ string resonance

// Sympathetic strings of an acoustic piano: a bank of 24 lightly damped comb
// filters tuned to two octaves of pitch classes (C3..B4). With the sustain
// pedal down every string is free to ring; without it (pedal follow on) only
// a short soundboard-like decay remains.
class SympatheticFx : public FxBlock {
public:
    static constexpr int kStrings = 24;
    void prepare(float sr) override {
        sr_ = sr;
        for (auto& ch : comb_)
            for (auto& d : ch) d.allocate((int)(sr / 120.0f) + 8);
        for (int k = 0; k < kStrings; ++k) period_[k] = sr / (130.81f * std::exp2((float)k / 12.0f));  // C3 ..
        reset();
    }
    void reset() override {
        for (auto& ch : comb_)
            for (auto& d : ch) d.clear();
        for (auto& ch : lp_)
            for (auto& v : ch) v = 0;
        ped_ = 0;
    }
    void update(const float* p, const FxContext&) override {
        amount_ = p[sympathetic::Amount];
        decay_ = p[sympathetic::Decay];
        follow_ = p[sympathetic::PedalFollow] > 0.5f;
        damp_ = 0.15f + 0.8f * p[sympathetic::Tone];
    }
    void process(float* L, float* R, int n, const FxContext& c) override {
        // pedal smoothing (~30 ms), decay follows the pedal (half pedal = partly damped)
        const float target = follow_ ? clampf(c.sustain, 0.0f, 1.0f) : 1.0f;
        ped_ += (target - ped_) * std::min(1.0f, (float)n / (0.03f * sr_));
        const float t60 = 0.08f + (decay_ - 0.08f) * ped_ * ped_;
        for (int k = 0; k < kStrings; ++k) fb_[k] = std::pow(10.0f, -3.0f * (period_[k] / sr_) / t60);
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) {
                const float x = io[ch][i];
                float w = 0;
                for (int k = 0; k < kStrings; ++k) {
                    const float d = period_[k] * (ch ? 1.0011f : 1.0f);  // slight mistuning between the channels
                    const float y0 = comb_[ch][k].read(d);
                    lp_[ch][k] += damp_ * (y0 - lp_[ch][k]);
                    const float y = x * 0.04f + fb_[k] * lp_[ch][k];
                    comb_[ch][k].push(y);
                    w += y;
                }
                io[ch][i] = x + softClip(w * amount_ * 0.7f);
            }
    }
    bool spillover() const override { return false; }
private:
    float sr_ = 48000, amount_ = 0.3f, decay_ = 4, damp_ = 0.5f, ped_ = 0;
    bool follow_ = true;
    float period_[kStrings] = {}, fb_[kStrings] = {}, lp_[2][kStrings] = {};
    DelayLine comb_[2][kStrings];
};

// ================================================================ scanner vibrato

// Tonewheel-organ scanner vibrato / chorus: a short delay line swept by a
// rounded triangle (the scanner's ~7 Hz rotation). V1-V3 = vibrato depths,
// C1-C3 = the same mixed 50/50 with the dry signal.
class ScannerFx : public FxBlock {
public:
    void prepare(float sr) override {
        sr_ = sr;
        dl_.allocate((int)(0.01f * sr));
        dr_.allocate((int)(0.01f * sr));
        reset();
    }
    void reset() override { dl_.clear(); dr_.clear(); phase_ = 0; lp_[0] = lp_[1] = 0; }
    void update(const float* p, const FxContext&) override {
        const int mode = (int)clampf(p[scanner::Mode], 0, 5);
        static const float kDepthMs[3] = {0.35f, 0.65f, 1.1f};
        depth_ = kDepthMs[mode % 3] * 0.001f * sr_;
        chorus_ = mode >= 3;
        inc_ = p[scanner::Rate] / sr_;
    }
    void process(float* L, float* R, int n, const FxContext&) override {
        const float base = 1.5f * 0.001f * sr_;
        for (int i = 0; i < n; ++i) {
            phase_ += inc_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            float tri = 1.0f - 4.0f * std::fabs(phase_ - 0.5f);
            tri = tri * (1.5f - 0.5f * tri * tri);  // rounded (the scanner's capacitor plates)
            dl_.push(L[i]);
            dr_.push(R[i]);
            float wl = dl_.readCubic(base + depth_ * (0.5f + 0.5f * tri));
            float wr = dr_.readCubic(base + depth_ * (0.5f - 0.5f * tri));
            lp_[0] += 0.5f * (wl - lp_[0]);  // the delay line's top-end loss
            lp_[1] += 0.5f * (wr - lp_[1]);
            wl = lp_[0];
            wr = lp_[1];
            if (chorus_) {
                L[i] = 0.5f * (L[i] + wl);
                R[i] = 0.5f * (R[i] + wr);
            } else {
                L[i] = wl;
                R[i] = wr;
            }
        }
    }
private:
    float sr_ = 48000, depth_ = 30, inc_ = 0, phase_ = 0, lp_[2] = {};
    bool chorus_ = true;
    DelayLine dl_, dr_;
};

}  // namespace

std::unique_ptr<FxBlock> createExtraBlock(int type) {
    switch (type) {
        case DeEsser: return std::make_unique<DeEsserFx>();
        case Isolator: return std::make_unique<IsolatorFx>();
        case StepFilter: return std::make_unique<StepFilterFx>();
        case MultiTap: return std::make_unique<MultiTapFx>();
        case Doubler: return std::make_unique<DoublerFx>();
        case Granular: return std::make_unique<GranularFx>();
        case TapeStop: return std::make_unique<TapeStopFx>();
        case Pump: return std::make_unique<PumpFx>();
        case Resonator: return std::make_unique<ResonatorFx>();
        case Swell: return std::make_unique<SwellFx>();
        case Talkbox: return std::make_unique<TalkboxFx>();
        case GatedVerb: return std::make_unique<GatedVerbFx>();
        case Sympathetic: return std::make_unique<SympatheticFx>();
        case Scanner: return std::make_unique<ScannerFx>();
        default: return nullptr;
    }
}

}  // namespace fx
}  // namespace hsynth
