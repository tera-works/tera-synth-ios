#include "hsynth/voice.h"

#include "hsynth/dsp/resample.h"
#include "hsynth/dsp/simd.h"

namespace hsynth {

// ---------------------------------------------------------------- PcmPlayer

void PcmPlayer::start(const Region* r, int key, float velocity, float startFrac, float velTrackScale,
                      float sampleRate, float gainDb) {
    detachStream();
    region_ = r;
    released_ = false;
    sr_ = sampleRate;
    const Sample* s = r->sample;
    end_ = r->end > 0 ? std::min<int64_t>(r->end, s->frames) : s->frames;
    int64_t off = std::max<int64_t>(0, r->offset);
    off += (int64_t)((double)clampf(startFrac, 0.0f, 1.0f) * (double)std::max<int64_t>(0, end_ - off));
    if (off >= end_) off = std::max<int64_t>(0, end_ - 1);
    pos_ = (double)off;
    starved_ = false;
    if (s->streamed && s->streamer) {
        // the ring starts a little before the end of the preloaded head (so
        // that every interpolation window is in one of the two)
        streamer_ = s->streamer;
        stream_ = streamer_->claim(s, std::max<int64_t>(0, std::max<int64_t>(s->preloadFrames - StreamSlot::kMirror,
                                                                                off - resample::kTaps)));
    }
    looping_ = (r->loopMode == LoopMode::Continuous || r->loopMode == LoopMode::Sustain) && r->loopEnd > r->loopStart + 1 &&
               r->loopStart >= 0 && r->loopEnd <= s->frames;
    xfLen_ = 0;
    if (looping_ && r->loopCrossfade > 0.0f)
        xfLen_ = std::min<int64_t>({(int64_t)(r->loopCrossfade * s->sampleRate), r->loopStart, r->loopEnd - r->loopStart});
    baseRatio_ = std::exp2(((double)(key - r->keycenter) * r->pitchKeytrack / 100.0 + r->transpose) / 12.0 + r->tune / 1200.0);
    const float t = clampf(r->ampVeltrack / 100.0f * velTrackScale, 0.0f, 1.0f);
    const float vg = (1.0f - t) + t * velocity * velocity;
    float g = dbToGain(r->volumeDb + gainDb + r->ampKeytrack * (float)(key - r->ampKeycenter)) * vg;
    const int vel127 = std::max(1, std::min(127, (int)std::lround(velocity * 127.0f)));
    const float xf = r->crossfadeGain(key, vel127);
    if (xf != 1.0f) g *= xf;
    gainL_ = g * std::min(1.0f, 1.0f - r->pan);
    gainR_ = g * std::min(1.0f, 1.0f + r->pan);
    fade_ = 1.0f;
    fadeStep_ = 0.0f;
    delay_ = (int64_t)(std::max(0.0f, r->delay) * sampleRate);
    useEnv_ = r->ampeg.enabled;
    envDelay_ = 0;
    if (useEnv_) {
        env_.setSampleRate(sampleRate);
        env_.set(std::max(0.0005f, r->ampeg.attack), std::max(0.001f, r->ampeg.decay), r->ampeg.sustain,
                 std::max(0.001f, r->ampeg.release));
        env_.setHold(std::max(0.0f, r->ampeg.hold));
        env_.reset();
        env_.gate(true);
        envDelay_ = (int64_t)(std::max(0.0f, r->ampeg.delay) * sampleRate);
    }
    if (r->filter != RegionFilter::None) {
        const float cents = r->filKeytrack * (float)(key - r->filKeycenter) + r->filVeltrack * velocity;
        const float fc = clampf(r->cutoff * std::exp2(cents / 1200.0f), 10.0f, sampleRate * 0.45f);
        // resonance dB -> SVF damping (0 dB = Butterworth)
        const float q = 0.7071f * dbToGain(std::max(0.0f, r->resonance));
        const float res = clampf((2.0f - 1.0f / q) / 1.98f, 0.0f, 1.0f);
        for (auto& f : filt_) { f.reset(); f.setCoefs(fc, res, sampleRate); }
        lp1a_ = 1.0f - std::exp(-kTwoPi * fc / sampleRate);
        lp1_[0] = lp1_[1] = 0.0f;
    }
}

void PcmPlayer::release() {
    if (!region_) return;
    released_ = true;
    if (region_->loopMode == LoopMode::Sustain) looping_ = false;
    if (useEnv_ && region_->loopMode != LoopMode::OneShot) env_.gate(false);
}

void PcmPlayer::cut() {
    if (!region_) return;
    if (region_->offFast || !useEnv_) {
        startFade(Voice::kSwapFadeSeconds);  // off_mode=fast: 6 ms, same as a sample set swap
    } else {
        env_.gate(false);
        looping_ = looping_ && region_->loopMode != LoopMode::Sustain;
    }
}

void PcmPlayer::startFade(float seconds) {
    const float step = 1.0f / (seconds * sr_);
    if (fadeStep_ < step) fadeStep_ = std::min(step, fade_);  // never slow down a faster fade
}

void PcmPlayer::fadeOut() {
    if (!region_) return;
    if (delay_ > 0) { finish(); return; }
    startFade(Voice::kSwapFadeSeconds);
}

void PcmPlayer::setPitch(float pitchSemis, float engineRate, int quality) {
    if (!region_) return;
    step_ = baseRatio_ * std::exp2((double)pitchSemis / 12.0) * region_->sample->sampleRate / (double)engineRate;
    if (quality <= 0 || step_ <= 1.0) {
        cut_ = -1;  // at or below the recorded pitch the cubic interpolator does not alias
        level_ = 0;
        return;
    }
    // pitched up: read a mip level that brings the step to <= 2, then a sinc
    // whose cutoff keeps the output below Nyquist
    double s = step_;
    int level = 0;
    while (level < region_->sample->mipLevels && s > 2.0) {
        s *= 0.5;
        ++level;
    }
    level_ = level;
    cut_ = resample::cutoffIndex(s);
}

inline const float* PcmPlayer::frames0(int c, int64_t first, int count) const {
    const Sample* s = region_->sample;
    if (first >= 0 && first + count <= s->preloadFrames) return s->channel(c) + first;
    if (!stream_) return nullptr;
    const float* p = stream_->window(c, first, count);
    if (!p && streamer_->offline() && first >= stream_->start && first + count <= s->frames) {
        streamer_->waitFor(*stream_, first + count);  // offline render: wait for the disk
        p = stream_->window(c, first, count);
    }
    return p;
}

inline float PcmPlayer::streamAt(int c, int64_t idx) const {
    if (idx < 0 || idx >= region_->sample->frames) return 0.0f;
    if (const float* p = frames0(c, idx, 1)) return *p;
    starved_ = true;
    return 0.0f;
}

inline float PcmPlayer::atLevel(int c, int64_t j) const {
    if (j < 0) return 0.0f;
    const Sample* s = region_->sample;
    if (s->streamed) return streamAt(c, j);
    if (looping_) {
        // loop points are level-0 frames: wrap in that domain
        int64_t j0 = j << level_;
        if (j0 >= region_->loopEnd) {
            j0 = region_->loopStart + (j0 - region_->loopEnd) % (region_->loopEnd - region_->loopStart);
            j = j0 >> level_;
        }
    }
    if (j >= s->levelFrames(level_) + Sample::kPadFrames) return 0.0f;
    return s->levelChannel(level_, c)[j];
}

float PcmPlayer::sincAt(int c, double pos) const {
    const double p = level_ ? std::ldexp(pos, -level_) : pos;
    const int64_t idx = (int64_t)p;
    const float fp = (float)(p - (double)idx) * (float)resample::kPhases;
    const int ph = std::min((int)fp, resample::kPhases - 1);
    const float pf = fp - (float)ph;
    const float* k0 = resample::kernel(cut_, ph);
    const float* k1 = k0 + resample::kTaps;
    const int64_t first = idx - (resample::kTaps / 2 - 1);
    const Sample* s = region_->sample;
    const int64_t last = first + resample::kTaps - 1;
    float acc = 0.0f;
    const bool inside = first >= 0 && last < s->levelFrames(level_) &&
                        (!looping_ || (last << level_) < region_->loopEnd);
    const float* d = !inside ? nullptr : s->streamed ? frames0(c, first, resample::kTaps) : s->levelChannel(level_, c) + first;
    if (d) {
        acc = simd::dotLerpKernel(d, k0, k1, pf, resample::kTaps);
    } else {
        for (int k = 0; k < resample::kTaps; ++k) acc += atLevel(c, first + k) * (k0[k] + pf * (k1[k] - k0[k]));
    }
    return acc;
}

void PcmPlayer::sincAt2(double pos, float& l, float& r) const {
    const double p = level_ ? std::ldexp(pos, -level_) : pos;
    const int64_t idx = (int64_t)p;
    const int64_t first = idx - (resample::kTaps / 2 - 1);
    const int64_t last = first + resample::kTaps - 1;
    const Sample* s = region_->sample;
    if (!(first >= 0 && last < s->levelFrames(level_) && (!looping_ || (last << level_) < region_->loopEnd))) {
        l = sincAt(0, pos);
        r = sincAt(1, pos);
        return;
    }
    const float* d0 = s->streamed ? frames0(0, first, resample::kTaps) : s->levelChannel(level_, 0) + first;
    const float* d1 = s->streamed ? frames0(1, first, resample::kTaps) : s->levelChannel(level_, 1) + first;
    if (!d0 || !d1) {
        l = sincAt(0, pos);
        r = sincAt(1, pos);
        return;
    }
    const float fp = (float)(p - (double)idx) * (float)resample::kPhases;
    const int ph = std::min((int)fp, resample::kPhases - 1);
    const float* k0 = resample::kernel(cut_, ph);
    simd::dotLerpKernel2(d0, d1, k0, k0 + resample::kTaps, fp - (float)ph, resample::kTaps, l, r);
}

inline float PcmPlayer::at(int c, int64_t idx) const {
    if (idx < 0) return 0.0f;
    if (region_->sample->streamed) return streamAt(c, idx);
    if (looping_ && idx >= region_->loopEnd) idx = region_->loopStart + (idx - region_->loopEnd) % (region_->loopEnd - region_->loopStart);
    const Sample* s = region_->sample;
    if (idx >= s->frames + Sample::kPadFrames) return 0.0f;
    return s->channel(c)[idx];
}

inline float PcmPlayer::sampleAt(int c, int64_t idx, float f) const {
    float xm1, x0, x1, x2;
    const int64_t lim = looping_ ? region_->loopEnd : region_->sample->frames + Sample::kPadFrames;
    const float* d = nullptr;
    if (region_->sample->streamed) d = frames0(c, idx - 1, 4);
    else if (idx >= 1 && idx + 2 < lim) d = region_->sample->channel(c) + idx - 1;  // common case: all four taps inside, no wrap
    if (d) {
        xm1 = d[0]; x0 = d[1]; x1 = d[2]; x2 = d[3];
    } else {
        xm1 = at(c, idx - 1); x0 = at(c, idx); x1 = at(c, idx + 1); x2 = at(c, idx + 2);
    }
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

void PcmPlayer::render(float* L, float* R, int n, float gain) {
    if (!region_) return;
    if (stream_) stream_->low.store((int64_t)pos_ - resample::kTaps, std::memory_order_release);
    struct StarveReport {  // one underrun per block that had to play silence
        const PcmPlayer& p;
        DiskStreamer* s;
        ~StarveReport() {
            if (p.starved_ && s) s->underrun();
            p.starved_ = false;
        }
    } starveReport{*this, streamer_};
    const bool stereo = region_->sample->channels == 2;
    const float gl = gainL_ * gain, gr = gainR_ * gain;
    const RegionFilter ft = region_->filter;
    for (int i = 0; i < n; ++i) {
        if (delay_ > 0) { --delay_; continue; }
        const int64_t idx = (int64_t)pos_;
        float l, r;
        if (stereo) value2(pos_, l, r);
        else r = l = value(0, pos_);
        if (xfLen_ > 0 && looping_ && idx >= region_->loopEnd - xfLen_) {
            // loop crossfade: blend in the material before the loop start
            const int64_t len = region_->loopEnd - region_->loopStart;
            const float t = (float)((pos_ - (double)(region_->loopEnd - xfLen_)) / (double)xfLen_);
            const float bl = value(0, pos_ - (double)len);
            l += (bl - l) * t;
            r = stereo ? r + (value(1, pos_ - (double)len) - r) * t : l;
        }
        if (ft != RegionFilter::None) {
            for (int c = 0; c < (stereo ? 2 : 1); ++c) {
                float& v = c == 0 ? l : r;
                switch (ft) {
                    case RegionFilter::Lp1: lp1_[c] += lp1a_ * (v - lp1_[c]); v = lp1_[c]; break;
                    case RegionFilter::Hp1: lp1_[c] += lp1a_ * (v - lp1_[c]); v -= lp1_[c]; break;
                    case RegionFilter::Lp2: v = filt_[c].tick(v).lp; break;
                    case RegionFilter::Hp2: v = filt_[c].tick(v).hp; break;
                    case RegionFilter::Bp2: v = filt_[c].tick(v).bp * filt_[c].k(); break;
                    case RegionFilter::Br2: { const Svf::Out o = filt_[c].tick(v); v = o.lp + o.hp; break; }
                    default: break;
                }
            }
            if (!stereo) r = l;
        }
        float e = 1.0f;
        if (useEnv_) {
            if (envDelay_ > 0) { --envDelay_; e = 0.0f; }
            else e = env_.process();
        }
        if (fadeStep_ > 0.0f) {
            fade_ -= fadeStep_;
            if (fade_ <= 0.0f) { finish(); return; }
            e *= fade_;
        }
        L[i] += l * gl * e;
        R[i] += r * gr * e;
        pos_ += step_;
        if (looping_) {
            if (pos_ >= (double)region_->loopEnd) pos_ -= (double)(region_->loopEnd - region_->loopStart);
        } else if (pos_ >= (double)end_) {
            finish();
            return;
        }
        if (useEnv_ && envDelay_ == 0 && !env_.active()) {
            finish();
            return;
        }
    }
}

// -------------------------------------------------------------------- Voice

// Multi-segment envelope: point 0 at t = 0, point i reached after the sum of
// the times of points 1..i. While the key is held and a loop point is set,
// time wraps from the last point back to the loop point.
void Voice::advanceMseg(const float* p, float dtSec, float tempo) {
    const int np = std::max(2, std::min(P::kMsegPoints, (int)p[P::MsegPoints]));
    const bool beats = p[P::MsegBeats] > 0.5f;
    float total = 0.0f, loopStart = 0.0f;
    const int loop = (int)p[P::MsegLoop];
    for (int i = 1; i < np; ++i) {
        if (i == loop) loopStart = total;
        total += std::max(0.0f, p[P::msegPt(i, P::MsegTime)]);
    }
    if (loop == 0) loopStart = -1.0f;
    msegT_ += beats ? dtSec * tempo / 60.0f : dtSec;
    if (!released_ && loopStart >= 0.0f && loop < np - 1 && total > loopStart + 1e-4f && msegT_ > total)
        msegT_ = loopStart + std::fmod(msegT_ - loopStart, total - loopStart);
    float t0 = 0.0f;
    msegValue_ = p[P::msegPt(np - 1, P::MsegLevel)];
    for (int i = 1; i < np; ++i) {
        const float d = std::max(0.0f, p[P::msegPt(i, P::MsegTime)]);
        if (msegT_ < t0 + d) {
            float x = d > 0.0f ? (msegT_ - t0) / d : 1.0f;
            const float c = p[P::msegPt(i, P::MsegCurve)];
            if (c > 0.0f) x = std::pow(x, 1.0f + 4.0f * c);                 // slow start
            else if (c < 0.0f) x = 1.0f - std::pow(1.0f - x, 1.0f - 4.0f * c);  // fast start
            const float a = p[P::msegPt(i - 1, P::MsegLevel)], b = p[P::msegPt(i, P::MsegLevel)];
            msegValue_ = a + (b - a) * x;
            break;
        }
        t0 += d;
    }
}

// Motion lane: tempo-synced step values (part parameters), optionally smoothed.
float Voice::motionValue(const VoiceContext& ctx, int lane) const {
    const float* p = params(ctx);
    const float stepBeats = fx::syncBeats((int)p[P::motion(lane, P::MotionRate)] + 1);
    if (stepBeats <= 0.0f) return 0.0f;
    const int steps = std::max(1, std::min(P::kMotionSteps, (int)p[P::motion(lane, P::MotionSteps)]));
    const double pos = ctx.beat / stepBeats;
    const double fl = std::floor(pos);
    const int idx = (int)((int64_t)fl % steps + steps) % steps;
    const float frac = (float)(pos - fl);
    const float v = p[P::motion(lane, P::MotionValue1 + idx)];
    const float sm = p[P::motion(lane, P::MotionSmooth)];
    if (sm <= 0.0f || frac < 1.0f - sm) return v;
    const float next = p[P::motion(lane, P::MotionValue1 + (idx + 1) % steps)];
    float x = (frac - (1.0f - sm)) / sm;
    x = x * x * (3.0f - 2.0f * x);
    return v + (next - v) * x;
}

void Voice::stopPlayers() {
    for (auto& ly : layers_)
        for (auto& pl : ly.pcm) pl.stop();
    for (auto& pl : relPcm_) pl.stop();
    relActive_ = false;
}

void Voice::deactivate() {
    active_ = false;
    stopPlayers();
}

void Voice::prepare(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.setSampleRate(sampleRate);
    env2_.setSampleRate(sampleRate);
    env3_.setSampleRate(sampleRate);
    for (auto& l : layers_) {
        l.fm.setSampleRate(sampleRate);
        l.organ.setSampleRate(sampleRate);
    }
    if (!combMem_) {
        combMem_.reset(new float[4 * Comb::kSize]());
        filterL_.setCombBuffer(combMem_.get());
        filterR_.setCombBuffer(combMem_.get() + Comb::kSize);
        filter2L_.setCombBuffer(combMem_.get() + 2 * Comb::kSize);
        filter2R_.setCombBuffer(combMem_.get() + 3 * Comb::kSize);
    }
    active_ = false;
}

void Voice::start(const Event& e, const VoiceContext& ctx, uint64_t age, float random, float glideFrom) {
    stopPlayers();  // a reused (stolen) voice: release the previous note's disk streams
    paramsOverride = nullptr;
    const float* p = ctx.params;
    noteId_ = e.noteId;
    key_ = e.key;
    pcmKey_ = e.key;
    glide_ = glideFrom >= 0.0f ? glideFrom - (float)e.key : 0.0f;
    velocity_ = clampf(e.value, 0.0f, 1.0f);
    random_ = random;
    age_ = age;
    active_ = true;
    released_ = stolen_ = sustained = hold = false;
    timeSinceOn_ = 0.0f;
    notePitchT_ = notePressureT_ = noteTimbreT_ = noteXT_ = 0.0f;
    notePitch_ = notePressure_ = noteTimbre_ = noteX_ = 0.0f;
    firstBlock_ = true;
    stereo_ = false;
    rng_ = Rng((uint32_t)(random * 4294967000.0f) + 1u);
    rng3_ = Rng(((uint32_t)(random * 4294967000.0f) ^ 0x9E3779B9u) | 1u);

    ampEnv_.reset();
    ampEnv_.set(p[P::AmpAttack], p[P::AmpDecay], p[P::AmpSustain], p[P::AmpRelease]);
    ampEnv_.gate(true);
    env2_.reset();
    env2_.set(p[P::Env2Attack], p[P::Env2Decay], p[P::Env2Sustain], p[P::Env2Release]);
    env2_.gate(true);
    lfo1_.reset(p[P::Lfo1KeySync] > 0.5f ? 0.0f : random);
    lfo1Value_ = 0.0f;
    env3_.reset();
    env3_.set(p[P::Env3Attack], p[P::Env3Decay], p[P::Env3Sustain], p[P::Env3Release]);
    env3_.gate(true);
    lfo3_.reset(p[P::Lfo3KeySync] > 0.5f ? 0.0f : std::fmod(random * 7.31f, 1.0f));
    lfo3Value_ = 0.0f;
    filterL_.reset();
    lfo1Depth_ = lfo3Depth_ = env2Scale_ = env3Scale_ = 1.0f;
    releaseVel_ = 0.5f;
    msegT_ = 0.0f;
    msegValue_ = p[P::msegPt(0, P::MsegLevel)];
    dampGain_ = 1.0f;
    startDelay = 0;
    releaseAt = -1;
    filterR_.reset();
    filter2L_.reset();
    filter2R_.reset();
    vfx_.reset();

    const int vel127 = std::max(1, std::min(127, (int)std::lround(velocity_ * 127.0f)));
    for (auto& pl : relPcm_) pl.stop();
    relActive_ = false;
    mainDone_ = false;
    for (int l = 0; l < kNumLayers; ++l) {
        sets_[l] = ctx.sampleSets[l];
        seq_[l] = ctx.seq[l];
        keyswitch_[l] = ctx.keyswitch[l];
    }
    for (int l = 0; l < kNumLayers; ++l) {
        Layer& ly = layers_[l];
        ly.type = (LayerType)(int)p[P::layer(l, P::LType)];
        ly.finished = ly.type == LayerType::Off;
        ly.pcmCount = 0;
        switch (ly.type) {
            case LayerType::VA:
                ly.osc1.reset(unisonDetune != 0.0f ? rng_.unipolar() : 0.0f);  // unison: free-running phases
                ly.osc2.reset(rng_.unipolar());
                ly.sub.reset(0.0f);
                for (auto& o : ly.ss1) o.reset(rng_.unipolar());
                for (auto& o : ly.ss2) o.reset(rng_.unipolar());
                break;
            case LayerType::FM:
                ly.fm.noteOn(velocity_, key_);
                ly.fm.control(p, l);
                break;
            case LayerType::Organ:
                ly.organ.noteOn(key_, rng_);
                ly.organ.control(p, l);
                break;
            case LayerType::WT:
                ly.wt.reset(rng_, (int)p[P::layer(l, P::WtUnison)]);
                break;
            case LayerType::Granular: {
                ly.grRegion = nullptr;
                for (auto& g : ly.grains) g.on = false;
                ly.grTimer = 0.0f;
                ly.grScan = 0.0;
                const SampleSet* set = ctx.sampleSets[l];
                const Region* regs[1];
                RegionQuery q;
                q.key = key_;
                q.vel127 = vel127;
                q.rand01 = random_;
                q.seq = ctx.seq[l];
                q.keyswitch = ctx.keyswitch[l];
                if (set && set->findRegions(q, regs, 1) > 0 && regs[0]->sample && regs[0]->sample->frames > 64) {
                    const Region* r = regs[0];
                    ly.grRegion = r;
                    ly.grBaseRatio = std::exp2(((double)(key_ - r->keycenter) * r->pitchKeytrack / 100.0 + r->transpose) / 12.0 +
                                               r->tune / 1200.0);
                } else {
                    ly.finished = true;
                }
                break;
            }
            case LayerType::PCM: {
                const SampleSet* set = ctx.sampleSets[l];
                const Region* regs[kMaxRegionsPerLayer];
                RegionQuery q;
                q.key = key_;
                q.vel127 = vel127;
                q.rand01 = random_;
                q.seq = ctx.seq[l];
                q.keyswitch = ctx.keyswitch[l];
                const int n = set ? set->findRegions(q, regs, kMaxRegionsPerLayer) : 0;
                for (int i = 0; i < n; ++i)
                    ly.pcm[i].start(regs[i], key_, velocity_, p[P::layer(l, P::PcmStart)], p[P::layer(l, P::PcmVelTrack)],
                                    sampleRate_);
                for (int i = n; i < kMaxRegionsPerLayer; ++i) ly.pcm[i].stop();
                ly.pcmCount = n;
                if (n == 0) ly.finished = true;
                break;
            }
            default: break;
        }
    }
}

void Voice::release() {
    if (!active_ || released_) return;
    released_ = true;
    sustained = false;
    // One-shot samples ignore note-off when they are the only sound source.
    bool allOneShot = true, anyLayer = false;
    for (auto& ly : layers_) {
        if (ly.type == LayerType::Off || ly.finished) continue;
        anyLayer = true;
        if (ly.type != LayerType::PCM) { allOneShot = false; continue; }
        for (int i = 0; i < ly.pcmCount; ++i)
            if (ly.pcm[i].active() && !ly.pcm[i].oneShot()) allOneShot = false;
    }
    if (!(anyLayer && allOneShot)) ampEnv_.gate(false);
    env2_.gate(false);
    for (auto& ly : layers_)
        if (ly.type == LayerType::Organ && !ly.finished) ly.organ.noteOff();
    env3_.gate(false);
    for (auto& ly : layers_) {
        if (ly.type == LayerType::FM) ly.fm.noteOff();
        if (ly.type == LayerType::PCM)
            for (int i = 0; i < ly.pcmCount; ++i) ly.pcm[i].release();
    }
    startReleaseRegions();
}

void Voice::startReleaseRegions() {
    int used = 0;
    const int vel127 = std::max(1, std::min(127, (int)std::lround(velocity_ * 127.0f)));
    for (int l = 0; l < kNumLayers && used < kMaxReleaseRegions; ++l) {
        const SampleSet* set = sets_[l];
        if (layers_[l].type != LayerType::PCM || !set || !set->hasReleaseRegions) continue;
        RegionQuery q;
        q.key = key_;
        q.vel127 = vel127;
        q.rand01 = random_;
        q.trigger = RegionTrigger::Release;
        q.seq = seq_[l];
        q.keyswitch = keyswitch_[l];
        const Region* regs[kMaxReleaseRegions];
        const int n = set->findRegions(q, regs, kMaxReleaseRegions - used);
        for (int i = 0; i < n; ++i) {
            relPcm_[used].start(regs[i], key_, velocity_, 0.0f, 1.0f, sampleRate_, -regs[i]->rtDecay * timeSinceOn_);
            relLayer_[used++] = l;
        }
    }
    if (used > 0) relActive_ = true;
}

void Voice::legatoTo(const Event& e, bool glide) {
    if (!active_) return;
    const float from = currentPitch();
    noteId_ = e.noteId;
    key_ = e.key;
    glide_ = glide ? from - (float)e.key : 0.0f;
    released_ = sustained = hold = false;
    notePitchT_ = notePressureT_ = noteTimbreT_ = noteXT_ = 0.0f;
}

int Voice::startedGroups(int* out, int max) const {
    int n = 0;
    for (const auto& ly : layers_) {
        if (ly.type != LayerType::PCM) continue;
        for (int i = 0; i < ly.pcmCount && n < max; ++i) {
            const Region* r = ly.pcm[i].region();
            if (!r || r->group == 0) continue;
            bool dup = false;
            for (int k = 0; k < n; ++k) dup = dup || out[k] == r->group;
            if (!dup) out[n++] = r->group;
        }
    }
    return n;
}

void Voice::choke(int group) {
    if (!active_ || group == 0) return;
    for (auto& ly : layers_) {
        if (ly.type != LayerType::PCM) continue;
        for (int i = 0; i < ly.pcmCount; ++i)
            if (ly.pcm[i].active() && ly.pcm[i].region()->offBy == group) ly.pcm[i].cut();
    }
    for (auto& pl : relPcm_)
        if (pl.active() && pl.region()->offBy == group) pl.cut();
}

void Voice::steal() {
    if (!active_) return;
    stolen_ = true;
    released_ = true;
    ampEnv_.kill(0.005f);
    for (auto& pl : relPcm_) {
        if (!pl.active()) continue;
        pl.release();
        pl.cut();
    }
}

void Voice::stopLayer(int l) {
    Layer& ly = layers_[l];
    ly.grRegion = nullptr;
    for (auto& pl : ly.pcm) pl.stop();
    for (int i = 0; i < kMaxReleaseRegions; ++i)
        if (relLayer_[i] == l) relPcm_[i].stop();
    sets_[l] = nullptr;
    if (ly.type == LayerType::PCM) ly.finished = true;
}

bool Voice::playsLayer(int l) const {
    if (!active_) return false;
    for (const auto& pl : layers_[l].pcm)
        if (pl.active()) return true;
    for (int i = 0; i < kMaxReleaseRegions; ++i)
        if (relLayer_[i] == l && relPcm_[i].active()) return true;
    return false;
}

bool Voice::usesSet(const SampleSet* set) const {
    if (!active_ || !set) return false;
    const Region* lo = set->regions.empty() ? nullptr : set->regions.data();
    const Region* hi = lo ? lo + set->regions.size() : nullptr;
    auto in = [&](const PcmPlayer& pl) { return lo && pl.active() && pl.region() >= lo && pl.region() < hi; };
    for (int l = 0; l < kNumLayers; ++l) {
        if (sets_[l] == set) return true;
        if (lo && layers_[l].grRegion >= lo && layers_[l].grRegion < hi) return true;
        for (const auto& pl : layers_[l].pcm)
            if (in(pl)) return true;
    }
    for (const auto& pl : relPcm_)
        if (in(pl)) return true;
    return false;
}

int Voice::dropSet(const SampleSet* set) {
    if (!set || set->regions.empty()) return 0;
    const Region* lo = set->regions.data();
    const Region* hi = lo + set->regions.size();
    auto in = [&](const PcmPlayer& pl) { return pl.active() && pl.region() >= lo && pl.region() < hi; };
    int n = 0;
    for (int l = 0; l < kNumLayers; ++l) {
        if (layers_[l].grRegion >= lo && layers_[l].grRegion < hi) {
            layers_[l].grRegion = nullptr;
            ++n;
        }
        for (auto& pl : layers_[l].pcm)
            if (in(pl)) { pl.stop(); ++n; }
        if (sets_[l] == set) sets_[l] = nullptr;
    }
    for (auto& pl : relPcm_)
        if (in(pl)) { pl.stop(); ++n; }
    return n;
}

void Voice::fadeLayer(int l) {
    layers_[l].grRegion = nullptr;  // grains stop at once (the set is retired)
    for (auto& pl : layers_[l].pcm) pl.fadeOut();
    for (int i = 0; i < kMaxReleaseRegions; ++i)
        if (relLayer_[i] == l) relPcm_[i].fadeOut();
    sets_[l] = nullptr;  // no new release regions from the outgoing set
}

void Voice::applyExpression(EventType type, float value) {
    switch (type) {
        case EventType::NotePitch: notePitchT_ = value; break;
        case EventType::NotePressure: notePressureT_ = clampf(value, 0.0f, 1.0f); break;
        case EventType::NoteTimbre: noteTimbreT_ = clampf(value, 0.0f, 1.0f); break;
        case EventType::NoteX: noteXT_ = clampf(value, -1.0f, 1.0f); break;
        default: break;
    }
}

float Voice::modSource(ModSource s, const VoiceContext& ctx) const {
    switch (s) {
        case ModSource::Velocity: return velocity_;
        case ModSource::Key: return (float)(key_ - 60) / 60.0f;
        case ModSource::AmpEnv: return ampEnv_.level();
        case ModSource::Env2: return env2_.level();
        case ModSource::Lfo1: return lfo1Value_ * lfo1Depth_;
        case ModSource::Lfo2: return ctx.lfo2;
        case ModSource::ModWheel: return ctx.modWheel;
        case ModSource::PitchBend: return ctx.pitchBendNorm;
        case ModSource::ChannelPressure: return ctx.chanPressure;
        case ModSource::NotePressure: return notePressure_;
        case ModSource::NoteTimbre: return noteTimbre_;
        case ModSource::NoteX: return noteX_;
        case ModSource::NotePitch: return notePitch_ / 12.0f;
        case ModSource::Random: return random_;
        case ModSource::Breath: return ctx.breath;
        case ModSource::Expression: return ctx.expression;
        case ModSource::Cc1:
        case ModSource::Cc2:
        case ModSource::Cc3:
        case ModSource::Cc4: return ctx.cc[(int)s - (int)ModSource::Cc1];
        case ModSource::Lfo3: return lfo3Value_ * lfo3Depth_;
        case ModSource::Env3: return env3_.level();
        case ModSource::Constant: return 1.0f;
        case ModSource::Super: return ctx.superKnob;
        case ModSource::ReleaseVelocity: return releaseVel_;
        case ModSource::NoteTimbreBi: return noteTimbre_ * 2.0f - 1.0f;
        case ModSource::Macro1:
        case ModSource::Macro2:
        case ModSource::Macro3:
        case ModSource::Macro4: return params(ctx)[P::Macro1 + ((int)s - (int)ModSource::Macro1)];
        case ModSource::Motion1:
        case ModSource::Motion2:
        case ModSource::Motion3:
        case ModSource::Motion4: return motionValue(ctx, (int)s - (int)ModSource::Motion1);
        case ModSource::Mseg: return msegValue_;
        default: return 0.0f;
    }
}

void Voice::render(float* L, float* R, int n, const VoiceContext& ctx) {
    if (!active_) return;
    const float* p = params(ctx);
    if (relActive_) {
        // release-trigger regions: own region envelope, layer level / pan / tuning, no amp env / filter
        bool any = false;
        for (int i = 0; i < kMaxReleaseRegions; ++i) {
            PcmPlayer& pl = relPcm_[i];
            if (!pl.active()) continue;
            const int l = relLayer_[i];
            float tl[kControlBlock] = {}, tr[kControlBlock] = {};
            pl.setPitch(notePitch_ + ctx.pitchBendSemis + p[P::layer(l, P::LCoarse)] + p[P::layer(l, P::LFine)] / 100.0f,
                        sampleRate_, ctx.quality);
            pl.render(tl, tr, n, p[P::layer(l, P::LLevel)]);
            const float lpan = p[P::layer(l, P::LPan)];
            const float lgl = std::min(1.0f, 1.0f - lpan), lgr = std::min(1.0f, 1.0f + lpan);
            for (int k = 0; k < n; ++k) {
                L[k] += tl[k] * lgl;
                R[k] += tr[k] * lgr;
            }
            any = any || pl.active();
        }
        relActive_ = any;
        if (mainDone_) {
            if (!relActive_) deactivate();
            return;
        }
    }
    const float sr = sampleRate_;
    const float dtSec = (float)n / sr;
    const float invN = 1.0f / (float)n;

    // Smooth per-note expression at control rate (~8 ms).
    {
        const float k = dtSec / 0.008f;
        const float a = firstBlock_ ? 1.0f : k / (1.0f + k);
        notePitch_ += a * (notePitchT_ - notePitch_);
        notePressure_ += a * (notePressureT_ - notePressure_);
        noteTimbre_ += a * (noteTimbreT_ - noteTimbre_);
        noteX_ += a * (noteXT_ - noteX_);
    }

    if (!stolen_) ampEnv_.set(p[P::AmpAttack], p[P::AmpDecay], p[P::AmpSustain], p[P::AmpRelease]);
    env2_.set(p[P::Env2Attack] * env2Scale_, p[P::Env2Decay] * env2Scale_, p[P::Env2Sustain], p[P::Env2Release] * env2Scale_);
    const float e2 = env2_.processBlock(n);
    env3_.set(p[P::Env3Attack] * env3Scale_, p[P::Env3Decay] * env3Scale_, p[P::Env3Sustain], p[P::Env3Release] * env3Scale_);
    env3_.processBlock(n);
    // half pedal: notes held only by a partly raised pedal fade out
    if (sustained && ctx.pedal < 0.85f) {
        const float tau = 0.1f + 8.0f * ctx.pedal * ctx.pedal;
        dampGain_ *= std::exp(-dtSec / tau);
    }

    // Modulation matrix: one scan collects the active slots, then pass 1
    // evaluates the destinations that scale other modulation (LFO depths,
    // amounts of slots 1-4; e.g. pressure sets the vibrato depth per note) and
    // pass 2 everything else.
    int aSrc[P::kModSlots], aVia[P::kModSlots], aDst[P::kModSlots], aSlot[P::kModSlots];
    float aAmt[P::kModSlots];
    int na = 0;
    bool anyMeta = false, usesMseg = false;
    auto isMeta = [](int dst) { return dst >= (int)ModDest::Lfo1Depth && dst <= (int)ModDest::Mod4Amt; };
    for (int s = 0; s < P::kModSlots; ++s) {
        const float amt = p[P::mod(s, P::ModAmt)];
        if (amt == 0.0f) continue;
        const int src = (int)p[P::mod(s, P::ModSrc)];
        const int dst = (int)p[P::mod(s, P::ModDst)];
        if (src <= 0 || dst <= 0 || dst >= (int)ModDest::Count) continue;
        const int via = (int)p[P::mod(s, P::ModVia)];
        aSrc[na] = src;
        aVia[na] = via;
        aDst[na] = dst;
        aAmt[na] = amt;
        aSlot[na++] = s;
        anyMeta = anyMeta || isMeta(dst);
        usesMseg = usesMseg || src == (int)ModSource::Mseg || via == (int)ModSource::Mseg;
    }
    if (usesMseg) advanceMseg(p, dtSec, ctx.tempo);
    float d[(int)ModDest::Count] = {};
    float slotScale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    lfo1Depth_ = lfo3Depth_ = 1.0f;
    auto evalSlot = [&](int i, float amt) {
        float v = modSource((ModSource)aSrc[i], ctx) * amt;
        if (aVia[i] > 0) v *= modSource((ModSource)aVia[i], ctx);
        d[aDst[i]] += v;
    };
    if (anyMeta) {
        for (int i = 0; i < na; ++i)
            if (isMeta(aDst[i])) evalSlot(i, aAmt[i]);
        lfo1Depth_ = clampf(1.0f + d[(int)ModDest::Lfo1Depth], 0.0f, 4.0f);
        lfo3Depth_ = clampf(1.0f + d[(int)ModDest::Lfo3Depth], 0.0f, 4.0f);
        for (int k = 0; k < 4; ++k) slotScale[k] = clampf(1.0f + d[(int)ModDest::Mod1Amt + k], 0.0f, 4.0f);
    }
    for (int i = 0; i < na; ++i)
        if (!isMeta(aDst[i])) evalSlot(i, aSlot[i] < 4 ? aAmt[i] * slotScale[aSlot[i]] : aAmt[i]);
    auto D = [&](ModDest x) { return d[(int)x]; };
    env2Scale_ = D(ModDest::Env2Time) != 0.0f ? fastExp2(clampf(D(ModDest::Env2Time), -2.0f, 2.0f) * 3.0f) : 1.0f;
    env3Scale_ = D(ModDest::Env3Time) != 0.0f ? fastExp2(clampf(D(ModDest::Env3Time), -2.0f, 2.0f) * 3.0f) : 1.0f;

    // amp envelope time modulation (x8 per unit), only when modulated
    if (!stolen_ && (D(ModDest::AmpAttack) != 0.0f || D(ModDest::AmpDecay) != 0.0f || D(ModDest::AmpRelease) != 0.0f)) {
        auto sc = [&](ModDest x) { return fastExp2(clampf(D(x), -2.0f, 2.0f) * 3.0f); };
        ampEnv_.set(p[P::AmpAttack] * sc(ModDest::AmpAttack), p[P::AmpDecay] * sc(ModDest::AmpDecay), p[P::AmpSustain],
                    p[P::AmpRelease] * sc(ModDest::AmpRelease));
    }

    // LFO1 (per voice) with delayed fade-in
    {
        const float syncB = fx::syncBeats((int)p[P::Lfo1Sync]);
        const float rate = (syncB > 0.0f ? ctx.tempo / 60.0f / syncB : p[P::Lfo1Rate]) *
                           fastExp2(clampf(D(ModDest::Lfo1Rate), -1.0f, 1.0f) * 4.0f);
        const float delay = p[P::Lfo1Delay];
        const float fade = delay <= 0.002f ? 1.0f : clampf(timeSinceOn_ / delay, 0.0f, 1.0f);
        lfo1Value_ = lfo1_.advance((LfoWave)(int)p[P::Lfo1Wave], rate, dtSec, rng_) * fade;
        timeSinceOn_ += dtSec;
        const float sync3 = fx::syncBeats((int)p[P::Lfo3Sync]);
        const float rate3 = (sync3 > 0.0f ? ctx.tempo / 60.0f / sync3 : p[P::Lfo3Rate]) *
                            fastExp2(clampf(D(ModDest::Lfo3Rate), -1.0f, 1.0f) * 4.0f);
        lfo3Value_ = lfo3_.advance((LfoWave)(int)p[P::Lfo3Wave], rate3, dtSec, rng3_);
    }

    float basePitch = (float)key_ + notePitch_ + ctx.pitchBendSemis + D(ModDest::Pitch) * 24.0f;
    {
        const float tune = ctx.masterTune + p[P::TuneC + key_ % 12] * 0.01f;  // master tune + scale tuning
        if (tune != 0.0f) basePitch += tune;
    }
    if (glide_ != 0.0f || unisonDetune != 0.0f) {
        basePitch += glide_ + unisonDetune * clampf(1.0f + D(ModDest::UnisonDetune), 0.0f, 4.0f);
        // exponential glide: ~99 % of the interval within voice.glide seconds
        const float t = p[P::Glide];
        glide_ = t <= 0.0005f ? 0.0f : glide_ * std::exp(-4.6f * dtSec / t);
        if (std::fabs(glide_) < 0.001f) glide_ = 0.0f;
    }

    // Filter
    const auto ftype = (FilterType)(int)p[P::FilterType];
    const float cutoffOct = p[P::FilterKeytrack] * (float)(key_ - 60) / 12.0f + (p[P::FilterEnvAmt] + D(ModDest::FilterEnvAmt)) * e2 * 6.0f +
                            p[P::FilterVelAmt] * 3.0f * (velocity_ - 1.0f) + D(ModDest::Cutoff) * 5.0f;
    const float cutoff = p[P::FilterCutoff] * fastExp2(clampf(cutoffOct, -12.0f, 12.0f));
    const float reso = clampf(p[P::FilterReso] + D(ModDest::Resonance), 0.0f, 1.0f);
    const float drive = clampf(p[P::FilterDrive] + D(ModDest::Drive), 0.0f, 1.0f);
    const float fmorph = clampf(p[P::FilterMorph] + D(ModDest::FilterMorph), 0.0f, 1.0f);
    const bool hq = ctx.quality >= 2;
    filterL_.set(ftype, cutoff, reso, drive, sr, fmorph, hq);
    if (stereo_) filterR_.copyCoefsFrom(filterL_);
    // second filter: serial after the first, or parallel beside it
    const auto ftype2 = (FilterType)(int)p[P::Filter2Type];
    const bool f2on = ftype2 != FilterType::Off;
    const bool f2parallel = p[P::FilterRouting] > 0.5f;
    const float f2bal = clampf(p[P::FilterBalance], 0.0f, 1.0f);
    if (f2on) {
        const float oct2 = p[P::Filter2Keytrack] * (float)(key_ - 60) / 12.0f + p[P::Filter2EnvAmt] * e2 * 6.0f +
                           D(ModDest::Cutoff2) * 5.0f;
        const float cutoff2 = p[P::Filter2Cutoff] * fastExp2(clampf(oct2, -12.0f, 12.0f));
        filter2L_.set(ftype2, cutoff2, clampf(p[P::Filter2Reso], 0.0f, 1.0f), clampf(p[P::Filter2Drive], 0.0f, 1.0f), sr, fmorph, hq);
        if (stereo_) filter2R_.copyCoefsFrom(filter2L_);
    }
    const auto vfxType = (VfxType)(int)p[P::VfxType];
    if (vfxType != VfxType::Off)
        vfx_.set(vfxType, p[P::VfxAmount] + D(ModDest::VfxAmount), p[P::VfxMix] + D(ModDest::VfxMix),
                 midiToHz(basePitch) * p[P::VfxRatio] * fastExp2(clampf(D(ModDest::VfxRatio), -1.0f, 1.0f) * 2.0f) / sr, hq);
    else
        vfx_.set(VfxType::Off, 0.0f, 0.0f, 0.0f);

    // Amp / pan
    const float vs = p[P::AmpVelSens];
    const float ampTarget = (1.0f - vs + vs * velocity_ * velocity_) * clampf(1.0f + D(ModDest::Amp), 0.0f, 2.0f) * dampGain_;
    const float pan = clampf(D(ModDest::Pan) + unisonPan, -1.0f, 1.0f);
    const float panL = std::min(1.0f, 1.0f - pan), panR = std::min(1.0f, 1.0f + pan);
    const float morph = clampf(p[P::Morph] + D(ModDest::Morph), -1.0f, 1.0f);

    float bufL[kControlBlock] = {};
    float bufR[kControlBlock] = {};
    float tmp[kControlBlock];
    float tmpR[kControlBlock];
    bool stereo = pan != 0.0f;
    bool anyLayerRunning = false;

    for (int l = 0; l < kNumLayers; ++l) {
        Layer& ly = layers_[l];
        if (ly.type == LayerType::Off || ly.finished) continue;
        const float lvs = p[P::layer(l, P::LVelSens)];
        float lvl = p[P::layer(l, P::LLevel)] * (1.0f - lvs + lvs * velocity_) *
                    clampf(1.0f + d[(int)ModDest::LayerALevel + l], 0.0f, 2.0f);
        if (l == 0) lvl *= clampf(1.0f - morph, 0.0f, 1.0f);
        if (l == 1) lvl *= clampf(1.0f + morph, 0.0f, 1.0f);
        const float lpitch = basePitch + p[P::layer(l, P::LCoarse)] + p[P::layer(l, P::LFine)] / 100.0f +
                             d[(int)ModDest::LayerAPitch + l] * 24.0f;
        const float lpanMod = d[(int)ModDest::LayerAPan + l];
        const float lpan = lpanMod != 0.0f ? clampf(p[P::layer(l, P::LPan)] + lpanMod, -1.0f, 1.0f) : p[P::layer(l, P::LPan)];
        const float lgl = std::min(1.0f, 1.0f - lpan), lgr = std::min(1.0f, 1.0f + lpan);
        if (lpan != 0.0f) stereo = true;
        const float g0 = firstBlock_ ? lvl : ly.gain;
        const float gStep = (lvl - g0) * invN;
        bool layerStereo = false;

        switch (ly.type) {
            case LayerType::VA: {
                const float dt1T = midiToHz(lpitch) / sr;
                const float dt2T = midiToHz(lpitch + p[P::layer(l, P::VaOsc2Semi)] + D(ModDest::VaOsc2Pitch) * 24.0f +
                                            (p[P::layer(l, P::VaDetune)] + D(ModDest::VaDetune) * 50.0f) / 100.0f) / sr;
                if (firstBlock_) { ly.dt1 = dt1T; ly.dt2 = dt2T; }
                const float s1 = (dt1T - ly.dt1) * invN, s2 = (dt2T - ly.dt2) * invN;
                const auto w1 = (VaWave)(int)p[P::layer(l, P::VaWave1)];
                const auto w2 = (VaWave)(int)p[P::layer(l, P::VaWave2)];
                const float pw = clampf(p[P::layer(l, P::VaPw)] + D(ModDest::VaPulseWidth) * 0.45f, 0.05f, 0.95f);
                const float mix = clampf(p[P::layer(l, P::VaMix)] + D(ModDest::VaOscMix), 0.0f, 1.0f);
                const float noise = clampf(p[P::layer(l, P::VaNoise)] + D(ModDest::VaNoise), 0.0f, 1.0f);
                const bool sync = p[P::layer(l, P::VaSync)] > 0.5f;
                const float xmod = p[P::layer(l, P::VaXmod)];
                const float subLvl = p[P::layer(l, P::VaSub)];
                const bool fancy = sync || xmod > 0.0f || subLvl > 0.0f || w1 == VaWave::SuperSaw || w2 == VaWave::SuperSaw;
                if (!fancy) {
                    for (int i = 0; i < n; ++i) {
                        ly.dt1 += s1;
                        ly.dt2 += s2;
                        float v = ly.osc1.tick(w1, ly.dt1, pw) * (1.0f - mix) + ly.osc2.tick(w2, ly.dt2, pw) * mix;
                        if (noise > 0.0f) v += noise * rng_.bipolar();
                        tmp[i] = v;
                    }
                } else {
                    // supersaw: centre + 6 detuned saws (detune curve of the classic design)
                    static const float kSs[6] = {-0.11002313f, -0.06288439f, -0.01952356f, 0.01991221f, 0.06216538f, 0.10745242f};
                    const float det = p[P::layer(l, P::VaSsDetune)];
                    const float ssDet = det * det * 0.9f + det * 0.1f;
                    const float ssMix = p[P::layer(l, P::VaSsMix)];
                    const float cg = 1.0f - 0.55f * ssMix, sg = 0.42f * ssMix;
                    auto osc = [&](VaOsc& o, VaOsc* side, VaWave w, float dt) {
                        if (w != VaWave::SuperSaw) return o.tick(w, dt, pw);
                        float v = o.tick(VaWave::Saw, dt, pw) * cg;
                        for (int k = 0; k < 6; ++k) v += side[k].tick(VaWave::Saw, dt * (1.0f + kSs[k] * ssDet), pw) * sg;
                        return v;
                    };
                    const int sw = (int)p[P::layer(l, P::VaSubWave)];
                    const float subRatio = sw == 2 ? 0.25f : 0.5f;
                    const VaWave subWave = sw == 1 ? VaWave::Sine : VaWave::Square;
                    for (int i = 0; i < n; ++i) {
                        ly.dt1 += s1;
                        ly.dt2 += s2;
                        const float o1 = osc(ly.osc1, ly.ss1, w1, ly.dt1);
                        float d2 = ly.dt2;
                        if (xmod > 0.0f) d2 = std::max(0.0f, d2 * (1.0f + 2.0f * xmod * o1));
                        if (sync && ly.osc1.wrapped()) ly.osc2.reset(ly.osc1.wrapFrac() * d2);
                        const float o2 = osc(ly.osc2, ly.ss2, w2, d2);
                        float v = o1 * (1.0f - mix) + o2 * mix;
                        if (subLvl > 0.0f) v += subLvl * ly.sub.tick(subWave, ly.dt1 * subRatio, 0.5f);
                        if (noise > 0.0f) v += noise * rng_.bipolar();
                        tmp[i] = v;
                    }
                }
                ly.dt1 = dt1T;
                ly.dt2 = dt2T;
                break;
            }
            case LayerType::FM: {
                const float opMod[P::kFmOps] = {D(ModDest::FmOp1Level), D(ModDest::FmOp2Level), D(ModDest::FmOp3Level),
                                                D(ModDest::FmOp4Level), D(ModDest::FmOp5Level), D(ModDest::FmOp6Level),
                                                D(ModDest::FmOp7Level), D(ModDest::FmOp8Level)};
                ly.fm.control(p, l, opMod, D(ModDest::FmModPitch) * 24.0f, lfo1Value_);
                const float pegAmt = p[P::layer(l, P::FmPegAmt)];
                const float peg = pegAmt != 0.0f ? pegAmt * std::exp(-timeSinceOn_ / std::max(0.001f, p[P::layer(l, P::FmPegTime)])) : 0.0f;
                const float dtT = midiToHz(lpitch + peg) / sr;
                if (firstBlock_) ly.dt1 = dtT;
                const float index = clampf(p[P::layer(l, P::FmIndex)] + D(ModDest::FmIndex), 0.0f, 1.0f) * 2.5f;
                const float fb = clampf(p[P::layer(l, P::FmFeedback)] + D(ModDest::FmFeedback), 0.0f, 1.0f);
                std::fill(tmp, tmp + n, 0.0f);
                ly.fm.render(tmp, n, ly.dt1, dtT, index, fb);
                ly.dt1 = dtT;
                if (!ly.fm.active()) ly.finished = true;
                break;
            }
            case LayerType::WT: {
                const int ti = (int)p[P::layer(l, P::WtTable)];
                const Wavetable* t = nullptr;
                if (ti < builtinWavetableCount()) t = &builtinWavetable(ti);
                else if (const SampleSet* ws = paramsOverride ? sets_[l] : ctx.sampleSets[l]; ws && ws->wavetable)
                    t = ws->wavetable.get();
                else t = &builtinWavetable(0);  // "user" selected but nothing loaded
                const float pos = p[P::layer(l, P::WtPos)] + p[P::layer(l, P::WtPosEnv)] * e2 + D(ModDest::WtPos);
                const float warpAmt = clampf(p[P::layer(l, P::WtWarpAmt)] + D(ModDest::WtWarp), 0.0f, 1.0f);
                layerStereo = ly.wt.render(*t, clampf(pos, 0.0f, 1.0f), midiToHz(lpitch) / sr, (WtWarp)(int)p[P::layer(l, P::WtWarpMode)],
                                           warpAmt, (int)p[P::layer(l, P::WtUnison)],
                                           p[P::layer(l, P::WtDetune)] * clampf(1.0f + D(ModDest::WtDetune), 0.0f, 4.0f),
                                           clampf(p[P::layer(l, P::WtSpread)] + D(ModDest::WtSpread), 0.0f, 1.0f), tmp, tmpR, n);
                break;
            }
            case LayerType::Organ: {
                ly.organ.control(p, l);
                std::fill(tmp, tmp + n, 0.0f);
                ly.organ.render(tmp, n, midiToHz(lpitch) / sr, rng_);
                break;
            }
            case LayerType::Granular: {
                std::fill(tmp, tmp + n, 0.0f);
                std::fill(tmpR, tmpR + n, 0.0f);
                const Region* r = ly.grRegion;
                if (!r) {
                    ly.finished = true;
                    break;
                }
                layerStereo = true;
                const Sample* s = r->sample;
                // a streamed sample has only its head in memory: grains stay inside it
                const int64_t avail = s->streamed ? std::min<int64_t>(s->frames, s->preloadFrames) : s->frames;
                const double framesD = (double)std::max<int64_t>(2, avail - 2);
                const float* c0 = s->channel(0);
                const float* c1 = s->channel(s->channels > 1 ? 1 : 0);
                const float size = std::max(0.002f, p[P::layer(l, P::GrSize)]);
                const float density = std::max(0.5f, p[P::layer(l, P::GrDensity)]);
                const float posRand = p[P::layer(l, P::GrPosRand)];
                const float pitchRand = p[P::layer(l, P::GrPitchRand)];
                const float spread = p[P::layer(l, P::GrSpread)];
                ly.grScan += (double)p[P::layer(l, P::GrScan)] * (double)n / sr * s->sampleRate / framesD;
                ly.grScan -= std::floor(ly.grScan);
                double pos = (double)p[P::layer(l, P::GrPos)] + (double)D(ModDest::GrainPos) + ly.grScan;
                pos -= std::floor(pos);
                const double ratio = ly.grBaseRatio * std::exp2((double)(lpitch - (float)key_) / 12.0) * s->sampleRate / sr;
                const float lenFrames = size * sr;
                const float norm = 1.0f / std::sqrt(std::max(1.0f, density * size));
                for (int i = 0; i < n; ++i) {
                    ly.grTimer -= 1.0f;
                    if (ly.grTimer <= 0.0f) {
                        for (auto& gr : ly.grains) {
                            if (gr.on) continue;
                            double start = (pos + (double)(rng_.bipolar() * posRand * 0.5f)) * framesD;
                            start = std::min(framesD - 1.0, std::max(0.0, start));
                            gr.pos = start;
                            gr.step = ratio * std::exp2((double)(rng_.bipolar() * pitchRand) / 12.0);
                            gr.age = 0.0f;
                            gr.len = lenFrames;
                            const float gp = rng_.bipolar() * spread;
                            gr.gl = norm * std::min(1.0f, 1.0f - gp);
                            gr.gr = norm * std::min(1.0f, 1.0f + gp);
                            gr.on = true;
                            break;
                        }
                        ly.grTimer += sr / density * (0.8f + 0.4f * rng_.unipolar());
                    }
                    float sl = 0.0f, sr2 = 0.0f;
                    for (auto& gr : ly.grains) {
                        if (!gr.on) continue;
                        const int64_t k = (int64_t)gr.pos;
                        const float fr = (float)(gr.pos - (double)k);
                        const float x = gr.age / gr.len;
                        const float w = 4.0f * x * (1.0f - x);  // parabolic window
                        sl += (c0[k] + (c0[k + 1] - c0[k]) * fr) * w * gr.gl;
                        sr2 += (c1[k] + (c1[k + 1] - c1[k]) * fr) * w * gr.gr;
                        gr.pos += gr.step;
                        gr.age += 1.0f;
                        if (gr.age >= gr.len || gr.pos >= framesD || gr.pos < 0.0) gr.on = false;
                    }
                    tmp[i] = sl;
                    tmpR[i] = sr2;
                }
                break;
            }
            case LayerType::PCM: {
                std::fill(tmp, tmp + n, 0.0f);
                std::fill(tmpR, tmpR + n, 0.0f);
                int running = 0;
                for (int i = 0; i < ly.pcmCount; ++i) {
                    PcmPlayer& pl = ly.pcm[i];
                    if (!pl.active()) continue;
                    if (pl.region()->sample->channels == 2 || pl.region()->pan != 0.0f) layerStereo = true;
                    pl.setPitch(lpitch - (float)pcmKey_, sr, ctx.quality);
                    pl.render(tmp, tmpR, n, 1.0f);
                    if (pl.active()) ++running;
                }
                if (running == 0) ly.finished = true;
                break;
            }
            default: break;
        }

        float g = g0;
        if (layerStereo) {
            stereo = true;
            for (int i = 0; i < n; ++i) {
                g += gStep;
                bufL[i] += tmp[i] * g * lgl;
                bufR[i] += tmpR[i] * g * lgr;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                g += gStep;
                const float v = tmp[i] * g;
                bufL[i] += v * lgl;
                bufR[i] += v * lgr;
            }
        }
        ly.gain = lvl;
        if (!ly.finished) anyLayerRunning = true;
    }

    if (stereo && !stereo_) {
        filterR_.copyFrom(filterL_);  // continue from the mono state without a click
        filter2R_.copyFrom(filter2L_);
        stereo_ = true;
    }

    const float a0 = firstBlock_ ? ampTarget : ampGain_;
    const float aStep = (ampTarget - a0) * invN;
    float a = a0;
    const float pl0 = firstBlock_ ? panL : panL_, pr0 = firstBlock_ ? panR : panR_;
    const float plStep = (panL - pl0) * invN, prStep = (panR - pr0) * invN;
    float gl = pl0, gr = pr0;
    for (int i = 0; i < n; ++i) {
        a += aStep;
        gl += plStep;
        gr += prStep;
        const float env = ampEnv_.process() * a;
        float l = filterL_.process(bufL[i]);
        float r = stereo_ ? filterR_.process(bufR[i]) : l;
        if (f2on) {
            if (f2parallel) {
                const float l2 = filter2L_.process(bufL[i]);
                const float r2 = stereo_ ? filter2R_.process(bufR[i]) : l2;
                l = l * (1.0f - f2bal) + l2 * f2bal;
                r = r * (1.0f - f2bal) + r2 * f2bal;
            } else {
                l = filter2L_.process(l);
                r = stereo_ ? filter2R_.process(r) : l;
            }
        }
        if (vfx_.active()) {
            vfx_.advance();
            l = vfx_.process(l, 0);
            r = stereo_ ? vfx_.process(r, 1) : l;
        }
        L[i] += l * env * gl;
        R[i] += r * env * gr;
    }
    ampGain_ = ampTarget;
    panL_ = panL;
    panR_ = panR;
    firstBlock_ = false;

    const bool envDone = !ampEnv_.active() || (ampEnv_.stage() == Adsr::Stage::Sustain && ampEnv_.level() <= 0.0f);
    if (envDone || !anyLayerRunning) {
        mainDone_ = true;
        if (!relActive_) deactivate();
    }
}

}  // namespace hsynth
