#include "hsynth/fx/rack.h"

#include <cstring>

#include "fx_blocks.h"

namespace hsynth {
namespace fx {

std::unique_ptr<FxBlock> createBlock(int type) {
    switch (type) {
        case Gate: return std::make_unique<GateFx>();
        case Comp: return std::make_unique<CompFx>();
        case MbComp: return std::make_unique<MbCompFx>();
        case Transient: return std::make_unique<TransientFx>();
        case Limiter: return std::make_unique<LimiterFx>();
        case Drive: return std::make_unique<DriveFx>();
        case Amp: return std::make_unique<AmpFx>();
        case Cab: return std::make_unique<CabFx>();
        case Crush: return std::make_unique<CrushFx>();
        case Lofi: return std::make_unique<LofiFx>();
        case Exciter: return std::make_unique<ExciterFx>();
        case Peq: return std::make_unique<PeqFx>();
        case Geq: return std::make_unique<GeqFx>();
        case Filter: return std::make_unique<FilterFx>();
        case Wah: return std::make_unique<WahFx>();
        case Chorus: return std::make_unique<ChorusFx>();
        case Flanger: return std::make_unique<FlangerFx>();
        case Phaser: return std::make_unique<PhaserFx>();
        case Tremolo: return std::make_unique<TremoloFx>();
        case Vibe: return std::make_unique<VibeFx>();
        case Rotary: return std::make_unique<RotaryFx>();
        case RingMod: return std::make_unique<RingModFx>();
        case Pitch: return std::make_unique<PitchFx>();
        case Octaver: return std::make_unique<OctaverFx>();
        case Delay: return std::make_unique<DelayFx>();
        case Reverb: return std::make_unique<ReverbFx>();
        case Convo: return std::make_unique<ConvoFx>();
        case Slicer: return std::make_unique<SlicerFx>();
        case Stutter: return std::make_unique<StutterFx>();
        case Stereo: return std::make_unique<StereoFx>();
        case Vocoder: return std::make_unique<VocoderFx>();
        default: return createExtraBlock(type);
    }
}

// ================================================================ looper

void Looper::prepare(float sr) {
    sr_ = sr;
    cap_ = (int)(kMaxSeconds * sr);
    buf_.assign((size_t)cap_ * 2, 0);
    undo_.assign((size_t)cap_ * 2, 0);
    reset();
}
void Looper::reset() {
    state_ = LooperState::Empty;
    len_ = pos_ = 0;
    undoValid_ = false;
    undoLeft_ = 0;
    fade_ = 0;
}

void Looper::command(LooperCmd c) {
    switch (c) {
        case LooperCmd::Tap:
            switch (state_) {
                case LooperState::Empty: state_ = LooperState::Recording; pos_ = 0; len_ = 0; undoValid_ = false; break;
                case LooperState::Recording:
                    if (pos_ < (int)(0.05f * sr_)) { state_ = LooperState::Empty; break; }
                    len_ = pos_; pos_ = 0; state_ = LooperState::Playing;
                    break;
                case LooperState::Playing:
                    state_ = LooperState::Overdubbing; dubStart_ = pos_; dubCount_ = 0; undoValid_ = false;
                    break;
                case LooperState::Overdubbing: state_ = LooperState::Playing; undoValid_ = dubCount_ > 0; break;
                case LooperState::Stopped: state_ = LooperState::Playing; pos_ = 0; break;
            }
            break;
        case LooperCmd::Play:
            if (state_ == LooperState::Stopped) { state_ = LooperState::Playing; pos_ = 0; }
            break;
        case LooperCmd::Stop:
            if (state_ == LooperState::Recording) {
                if (pos_ < (int)(0.05f * sr_)) { state_ = LooperState::Empty; break; }
                len_ = pos_;
            }
            if (state_ == LooperState::Overdubbing) undoValid_ = dubCount_ > 0;
            if (state_ != LooperState::Empty) state_ = LooperState::Stopped;
            pos_ = 0;
            break;
        case LooperCmd::Clear: reset(); break;
        case LooperCmd::Undo:
            // swapping the saved layer back also works as redo
            if (undoValid_ && state_ != LooperState::Recording && state_ != LooperState::Overdubbing) {
                undoLeft_ = std::min(dubCount_, len_);
                undoAt_ = dubStart_;
            }
            break;
        default: break;
    }
}

void Looper::undoStep(int budget) {
    while (undoLeft_ > 0 && budget-- > 0) {
        const size_t i = (size_t)undoAt_ * 2;
        std::swap(buf_[i], undo_[i]);
        std::swap(buf_[i + 1], undo_[i + 1]);
        undoAt_ = (undoAt_ + 1) % len_;
        --undoLeft_;
    }
}

void Looper::process(float* L, float* R, int n, float level) {
    if (undoLeft_ > 0) undoStep(16384);
    const float fadeStep = 1.0f / (0.005f * sr_);
    auto toI16 = [](float v) { return (int16_t)std::lround(clampf(v, -1.0f, 1.0f) * 32767.0f); };
    const float k = 1.0f / 32767.0f;
    for (int i = 0; i < n; ++i) {
        const bool playing = state_ == LooperState::Playing || state_ == LooperState::Overdubbing;
        fade_ = clampf(fade_ + (playing ? fadeStep : -fadeStep), 0.0f, 1.0f);
        if (state_ == LooperState::Recording) {
            if (pos_ >= cap_) { len_ = pos_; pos_ = 0; state_ = LooperState::Playing; }
            else {
                buf_[(size_t)pos_ * 2] = toI16(L[i]);
                buf_[(size_t)pos_ * 2 + 1] = toI16(R[i]);
                ++pos_;
                continue;
            }
        }
        if (len_ <= 0 || state_ == LooperState::Empty) continue;
        const size_t at = (size_t)pos_ * 2;
        const float ll = buf_[at] * k, lr = buf_[at + 1] * k;
        if (state_ == LooperState::Overdubbing) {
            undo_[at] = buf_[at];
            undo_[at + 1] = buf_[at + 1];
            buf_[at] = toI16(ll + L[i]);
            buf_[at + 1] = toI16(lr + R[i]);
            if (dubCount_ < len_) ++dubCount_;
        }
        if (fade_ > 0.0f) {
            L[i] += ll * level * fade_;
            R[i] += lr * level * fade_;
        }
        if (state_ != LooperState::Stopped && ++pos_ >= len_) pos_ = 0;
    }
    stState_.store((int)state_, std::memory_order_relaxed);
    stLen_.store(len_, std::memory_order_relaxed);
    stPos_.store(pos_, std::memory_order_relaxed);
    stUndo_.store(undoValid_, std::memory_order_relaxed);
}

LooperStatus Looper::status() const {
    LooperStatus s;
    s.state = (LooperState)stState_.load(std::memory_order_relaxed);
    s.lengthSec = (float)stLen_.load(std::memory_order_relaxed) / sr_;
    s.positionSec = (float)stPos_.load(std::memory_order_relaxed) / sr_;
    s.canUndo = stUndo_.load(std::memory_order_relaxed);
    return s;
}

// ================================================================ rack

FxRack::FxRack() {
    for (int b = 1; b < BlockCount; ++b) blocks_[b] = createBlock(b);
    base_.assign((size_t)kBlockParamTotal, 0.0f);
    cur_.assign((size_t)kBlockParamTotal, 0.0f);
    blockOfLocal_.assign((size_t)kBlockParamTotal, 0);
    for (int b = 1; b < BlockCount; ++b)
        for (int k = 0; k < kBlockParams[b]; ++k) blockOfLocal_[(size_t)(blockOffset(b) + k)] = b;
    for (auto& s : snap_) s.store(0);
}

FxRack::~FxRack() = default;

void FxRack::prepare(float sr) {
    sr_ = sr;
    ctx_.sampleRate = sr;
    for (int b = 1; b < BlockCount; ++b) blocks_[b]->prepare(sr);
    tmpL_.assign(kMaxBlockSize, 0.0f);
    tmpR_.assign(kMaxBlockSize, 0.0f);
    looper_.prepare(sr);
    first_ = true;
    reset();
}

void FxRack::reset() {
    for (int b = 1; b < BlockCount; ++b) blocks_[b]->reset();
    beat_ = 0;
    env_ = 0;
}

void FxRack::setUserIr(int target, const ConvIr* ir) {
    if (target == 0) blocks_[Cab]->setUserIr(ir);
    else blocks_[Convo]->setUserIr(ir);
}

int FxRack::chainSnapshot(int* out) const {
    const int n = snapLen_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) out[i] = snap_[i].load(std::memory_order_relaxed);
    return n;
}

void FxRack::paramsChanged(const float* p, int oversample, bool highQuality) {
    paramTempo_ = p[P::fxChain(P::FxTempo)];
    clockSync_ = p[P::fxChain(P::FxClockSync)] > 0.5f;
    const float tempo = extActive_ && clockSync_ ? tempo_ : paramTempo_;
    const bool ctxChanged = first_ || ctx_.oversample != oversample || ctx_.highQuality != highQuality || tempo_ != tempo;
    ctx_.oversample = oversample;
    ctx_.highQuality = highQuality;
    tempo_ = tempo;
    ctx_.tempo = tempo_;
    retrig_ = p[P::fxChain(P::FxRetrig)] > 0.5f;
    macro1_ = p[P::fxChain(P::FxMacro1)];
    macro2_ = p[P::fxChain(P::FxMacro2)];
    looperLevel_ = p[P::fxChain(P::LooperLevel)];

    // chain order (first occurrence of a block wins)
    bool now[BlockCount] = {};
    chainLen_ = 0;
    for (int s = 0; s < kFxSlots; ++s) {
        const int b = (int)p[P::fxSlot(s)];
        if (b <= None || b >= BlockCount || now[b]) continue;
        now[b] = true;
        chain_[chainLen_++] = b;
    }
    for (int b = 1; b < BlockCount; ++b) {
        if (now[b] && !inChain_[b]) {
            blocks_[b]->reset();  // no stale state from an earlier insertion
            xfade_[b] = 0.0f;
        }
        inChain_[b] = now[b];
    }
    for (int i = 0; i < chainLen_; ++i) snap_[i].store(chain_[i], std::memory_order_relaxed);
    snapLen_.store(chainLen_, std::memory_order_release);

    // modulation slots
    const auto& targets = fxModTargets();
    numMods_ = 0;
    for (int s = 0; s < kFxModSlots; ++s) {
        const int src = (int)p[P::fxMod(s, P::FxModSrc)];
        const int dst = (int)p[P::fxMod(s, P::FxModDst)];
        const float amt = p[P::fxMod(s, P::FxModAmt)];
        if (src <= 0 || dst <= 0 || dst >= (int)targets.size() || amt == 0.0f) continue;
        ModSlot& m = mods_[numMods_++];
        m.src = src;
        m.local = targets[(size_t)dst] - P::kFxBlockBase;
        m.block = blockOfLocal_[(size_t)m.local];
        m.amt = amt;
    }

    // block parameters: update blocks whose values changed
    const float* bp = p + P::kFxBlockBase;
    for (int b = 1; b < BlockCount; ++b) {
        const int off = blockOffset(b), cnt = kBlockParams[b];
        bool changed = ctxChanged;
        for (int k = 0; k < cnt && !changed; ++k) changed = base_[(size_t)(off + k)] != bp[off + k];
        if (!changed) continue;
        std::memcpy(&base_[(size_t)off], bp + off, sizeof(float) * (size_t)cnt);
        std::memcpy(&cur_[(size_t)off], bp + off, sizeof(float) * (size_t)cnt);
        dirty_[b] = true;
    }
    for (int i = 0; i < numMods_; ++i) dirty_[mods_[i].block] = true;  // re-apply modulation on fresh values
    if (first_) {
        for (int b = 1; b < BlockCount; ++b) xfade_[b] = inChain_[b] && cur_[(size_t)blockOffset(b)] > 0.5f ? 1.0f : 0.0f;
        first_ = false;
    }
    for (int b = 1; b < BlockCount; ++b) {
        if (!dirty_[b] || numMods_ > 0) continue;  // with modulation, update happens per sub-block
        blocks_[b]->update(&cur_[(size_t)blockOffset(b)], ctx_);
        dirty_[b] = false;
    }
}

void FxRack::tempoChanged() {
    ctx_.tempo = tempo_;
    for (int b = 1; b < BlockCount; ++b) dirty_[b] = true;  // re-derive synced times / rates
}

void FxRack::setExternalClock(bool active, float bpm, double beat) {
    const bool use = active && clockSync_;
    extActive_ = active;
    if (use) {
        beat_ = beat;
        if (std::fabs(bpm - tempo_) > 0.05f) {
            tempo_ = bpm;
            tempoChanged();
        }
    } else if (tempo_ != paramTempo_) {
        tempo_ = paramTempo_;
        tempoChanged();
    }
}

void FxRack::noteOn(int key, float velocity, bool firstNote) {
    ctx_.lastNoteHz = midiToHz((float)key);
    lastKey_ = (float)key;
    velocity_ = velocity;
    if (firstNote && retrig_) {
        if (!(extActive_ && clockSync_)) beat_ = 0.0;  // with MIDI clock the beat follows the clock
        for (int i = 0; i < chainLen_; ++i) blocks_[chain_[i]]->retrigger();
    }
}

float FxRack::sourceValue(int src, const FxGlobals& g) const {
    switch ((FxModSource)src) {
        case FxModSource::ModWheel: return g.modWheel;
        case FxModSource::Lfo2: return g.lfo2;
        case FxModSource::ChannelPressure: return g.chanPressure;
        case FxModSource::Expression: return g.expression;
        case FxModSource::Breath: return g.breath;
        case FxModSource::PitchBend: return g.pitchBend;
        case FxModSource::Velocity: return velocity_;
        case FxModSource::LastNote: return (lastKey_ - 60.0f) / 60.0f;
        case FxModSource::Envelope: return env_;
        case FxModSource::NotesHeld: return held_;
        case FxModSource::Macro1: return macro1_;
        case FxModSource::Macro2: return macro2_;
        default: return 0.0f;
    }
}

void FxRack::applyModulation(const FxGlobals& g) {
    const auto& table = paramTable();
    // restore base values of modulated params, then accumulate every slot
    for (int i = 0; i < numMods_; ++i) cur_[(size_t)mods_[i].local] = base_[(size_t)mods_[i].local];
    for (int i = 0; i < numMods_; ++i) {
        const ModSlot& m = mods_[i];
        const ParamInfo& info = table[(size_t)(P::kFxBlockBase + m.local)];
        const float v = sourceValue(m.src, g) * m.amt;
        float& c = cur_[(size_t)m.local];
        if (info.kind == ParamKind::Bool) c = (c + v) > 0.5f ? 1.0f : 0.0f;
        else if (info.curve == ParamCurve::Exp && info.min > 0.0f) c = clampf(c * std::pow(info.max / info.min, v), info.min, info.max);
        else c = clampf(c + v * (info.max - info.min), info.min, info.max);
        dirty_[m.block] = true;
    }
}

void FxRack::runChain(float* L, float* R, int n) {
    const float xfStep = (float)n / (0.01f * sr_);
    for (int i = 0; i < chainLen_; ++i) {
        const int b = chain_[i];
        FxBlock* blk = blocks_[b].get();
        const bool on = cur_[(size_t)blockOffset(b)] > 0.5f;
        if (blk->spillover()) {
            if (on || blk->tailActive()) blk->process(L, R, n, ctx_);
            continue;
        }
        float& xf = xfade_[b];
        const float target = on ? 1.0f : 0.0f;
        if (xf == 0.0f && target == 0.0f) continue;
        if (xf == 1.0f && target == 1.0f) {
            blk->process(L, R, n, ctx_);
            continue;
        }
        // crossfade dry <-> processed over 10 ms
        std::memcpy(tmpL_.data(), L, sizeof(float) * (size_t)n);
        std::memcpy(tmpR_.data(), R, sizeof(float) * (size_t)n);
        if (xf == 0.0f) blk->reset();
        blk->process(tmpL_.data(), tmpR_.data(), n, ctx_);
        const float x1 = target > xf ? std::min(1.0f, xf + xfStep) : std::max(0.0f, xf - xfStep);
        for (int k = 0; k < n; ++k) {
            const float g = xf + (x1 - xf) * ((float)(k + 1) / (float)n);
            L[k] += (tmpL_[(size_t)k] - L[k]) * g;
            R[k] += (tmpR_[(size_t)k] - R[k]) * g;
        }
        xf = x1;
    }
}

void FxRack::process(float* L, float* R, int n, const FxGlobals& g, const float* inL, const float* inR) {
    constexpr int kSub = 32;
    // Idle: skip the effect chain while the input is silent and the output has
    // been silent for 4 s (reverb / delay tails ended; blocks that make sound
    // on their own - lo-fi hiss, frozen reverb, looper - never go silent). The
    // beat keeps running for tempo-synced effects and the sequencers.
    float inPk = 0.0f;
    for (int i = 0; i < n; ++i) inPk = std::max(inPk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    if (inL && inR)
        for (int i = 0; i < n; ++i) inPk = std::max(inPk, std::max(std::fabs(inL[i]), std::fabs(inR[i])));
    const bool silentIn = inPk < 1e-7f;
    if (silentIn && idle_ && looper_.status().state != LooperState::Playing && looper_.status().state != LooperState::Overdubbing) {
        beat_ += (double)tempo_ / 60.0 * (double)n / (double)sr_;
        env_ = 0.0f;
        held_ = clampf(held_ - (float)n / (0.02f * sr_), 0.0f, 1.0f);
        return;
    }
    if (idle_) {
        // waking up: every tail is silent, so start the chain from a clean
        // state (time glides / LFOs would otherwise resume from where they
        // stopped, e.g. a tempo-synced delay after the tempo changed)
        for (int i = 0; i < chainLen_; ++i) {
            blocks_[chain_[i]]->reset();
            dirty_[chain_[i]] = true;
        }
        idle_ = false;
    }
    const float envRel = std::exp(-(float)kSub / (0.15f * sr_));
    const float heldStep = (float)kSub / (0.02f * sr_);
    for (int off = 0; off < n; off += kSub) {
        const int m = std::min(kSub, n - off);
        float* l = L + off;
        float* r = R + off;
        // envelope source: peak level of the signal entering the rack
        float pk = 0.0f;
        for (int i = 0; i < m; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        const float e = clampf((fastLinToDb(pk + 1e-9f) + 48.0f) / 48.0f, 0.0f, 1.0f);
        env_ = e > env_ ? e : e + (env_ - e) * envRel;
        held_ = clampf(held_ + (g.notesHeld ? heldStep : -heldStep), 0.0f, 1.0f);
        ctx_.notesHeld = g.notesHeld;
        ctx_.sustain = g.sustain;
        ctx_.beat = beat_;
        ctx_.inL = inL ? inL + off : nullptr;
        ctx_.inR = inR ? inR + off : nullptr;
        if (numMods_ > 0) applyModulation(g);
        for (int b = 1; b < BlockCount; ++b) {
            if (!dirty_[b]) continue;
            blocks_[b]->update(&cur_[(size_t)blockOffset(b)], ctx_);
            dirty_[b] = false;
        }
        runChain(l, r, m);
        beat_ += (double)tempo_ / 60.0 * (double)m / (double)sr_;
    }
    looper_.process(L, R, n, looperLevel_);
    float outPk = 0.0f;
    for (int i = 0; i < n; ++i) outPk = std::max(outPk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    if (silentIn && outPk < 1e-7f) {
        silentFrames_ += n;
        // longer than any gap a tail can have before it sounds again (delay /
        // multi-tap up to 2 s, granular reaching ~1.5 s into the past)
        if (silentFrames_ > (int64_t)(4.0f * sr_)) idle_ = true;
    } else {
        silentFrames_ = 0;
    }
}

}  // namespace fx
}  // namespace hsynth
