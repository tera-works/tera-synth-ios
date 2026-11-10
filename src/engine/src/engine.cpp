#include "hsynth/engine.h"


#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace hsynth {

namespace {
// Denormal floats (decaying filter / reverb states) are extremely slow on many
// CPUs. Enable flush-to-zero on the calling (audio) thread.
void enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    if (!(fpcr & (1ull << 24))) __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#elif defined(__SSE__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
#endif
}

void atomicMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void atomicMaxI(std::atomic<int>& a, int v) {
    int cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
}  // namespace

Engine::Engine()
    : sharedParams_(new std::atomic<float>[P::kNumParams]),
      partShared_(new std::atomic<float>[(size_t)(kNumParts - 1) * P::kPartParams]),
      partP_(new float[(size_t)(kNumParts - 1) * P::kPartParams]),
      partBus_(new float[(size_t)kNumParts * 2 * kMaxBlockSize]()),
      ghostParams_(new float[(size_t)kGhosts * P::kPartParams]()),
      inserts_(new fx::InsertChain[kNumParts]) {
    float defs[P::kNumParams];
    defaultParams(defs);
    for (int i = 0; i < P::kNumParams; ++i) sharedParams_[i].store(defs[i]);
    std::memcpy(p_, defs, sizeof p_);
    for (int k = 1; k < kNumParts; ++k)
        for (int i = 0; i < P::kPartParams; ++i) {
            const size_t idx = (size_t)(k - 1) * P::kPartParams + (size_t)i;
            partShared_[idx].store(defs[i]);
            partP_[idx] = defs[i];
        }
    for (int k = 0; k < kNumParts; ++k) {
        partVersion_[k].store(1);
        statPartVoices_[k].store(0);
        float mix[Mix::Count];
        defaultPartMix(k, mix);
        for (int i = 0; i < Mix::Count; ++i) {
            mixShared_[k][i].store(mix[i]);
            mix_[k][i] = mix[i];
        }
        parts_[k].p = k == 0 ? p_ : partP_.get() + (size_t)(k - 1) * P::kPartParams;
        parts_[k].rng = Rng(0x9E3779B9u * (uint32_t)(k + 1));
        for (auto& ps : pendingSets_[k]) ps.store(nullptr);
        for (auto& ks : keyswitch_[k]) ks = -1;
        stageIn_[k].store(nullptr);
        appliedProg_[k].store(-1);
        songProg_[k].store(-1);
        stagedProg_[k].store(-1);
    }
    for (int t = 0; t < 2; ++t) {
        pendingIr_[t].store(nullptr);
    }
    phrase_.reserve(kMaxPhraseEvents);
    snapBuf_.reserve(kMaxPhraseEvents);
    for (auto& s : statSeqStep_) s.store(-1);
    for (int k = 0; k < kNumParts; ++k) {
        meterPeak_[k].store(0.0f);
        for (auto& c : meterCc_[k]) c.store(0.0f);
        meterCc_[k][0].store(1.0f);
        meterCc_[k][1].store(1.0f);
        for (auto& n : meterNotes_[k]) n.store(0);
    }
    sendRev_ = fx::createBlock(fx::Reverb);
    sendDly_ = fx::createBlock(fx::Delay);
    sendRevP_.assign(defs + P::fxp(fx::Reverb, 0), defs + P::fxp(fx::Reverb, 0) + fx::reverb::Count);
    sendDlyP_.assign(defs + P::fxp(fx::Delay, 0), defs + P::fxp(fx::Delay, 0) + fx::delay::Count);
}

Engine::~Engine() {
    streamer_.stop();  // no file reads into samples that are deleted below
    delete pendingSong_.exchange(nullptr);
    delete song_;
    song_ = nullptr;
    for (int k = 0; k < kNumParts; ++k)
        for (int l = 0; l < kNumLayers; ++l) {
            delete pendingSets_[k][l].exchange(nullptr);
            delete currentSets_[k][l];
            currentSets_[k][l] = nullptr;
        }
    for (auto& r : retiring_) {
        delete r.set;
        r.set = nullptr;
    }
    for (int t = 0; t < 2; ++t) {
        fx::ConvIr* p = pendingIr_[t].exchange(nullptr);
        if (p != &unloadIr_) delete p;
        delete currentIr_[t];
        currentIr_[t] = nullptr;
    }
    for (int k = 0; k < kNumParts; ++k) {
        delete stageIn_[k].exchange(nullptr);
        delete stage_[k];
        stage_[k] = nullptr;
    }
    for (auto& h : held_) {
        delete h;
        h = nullptr;
    }
    delete pendingPhrase_.exchange(nullptr);
    collectGarbage();
}

void Engine::prepare(float sampleRate, int maxBlockSize) {
    (void)maxBlockSize;
    sampleRate_ = sampleRate;
    for (auto& v : voices_) v.prepare(sampleRate);
    builtinWavetable(0);  // generate the shared tables now, not on the audio thread
    fmAlgorithm(0);
    rack_.prepare(sampleRate);
    sendRev_->prepare(sampleRate);
    sendDly_->prepare(sampleRate);
    updateSends();
    for (int k = 0; k < kNumParts; ++k) inserts_[k].prepare(sampleRate);
    for (int t = 0; t < 2; ++t) rack_.setUserIr(t, currentIr_[t]);
    effectsQuality_ = -1;
    limiter_.prepare(sampleRate, kMaxBlockSize);
    masterGain_.setTime(0.02f, sampleRate);
    masterGain_.reset(p_[P::MasterVolume]);
    for (auto& ps : parts_) ps.ctx.sampleRate = sampleRate;
    gainStep_ = 1.0f / (0.01f * sampleRate);  // mix gain ramps: full scale in 10 ms
    seenParamVersion_ = 0;
    for (auto& v : seenPartVersion_) v = 0;
    seenMixVersion_ = 0;
    samplePos_ = 0;
    tickCount_ = 0;
    clockBpm_ = 0.0f;
    prepared_ = true;
}

// --------------------------------------------------------- non-audio API

bool Engine::sendEvent(const Event& e) {
    std::lock_guard<std::mutex> lock(producerMutex_);
    if (!events_.push(e)) {
        droppedEvents_.fetch_add(1);
        return false;
    }
    return true;
}

void Engine::setParam(int id, float value) {
    if (id < 0 || id >= P::kNumParams) return;
    sharedParams_[id].store(sanitizeParam(id, value), std::memory_order_relaxed);
    paramVersion_.fetch_add(1, std::memory_order_release);
}

float Engine::getParam(int id) const {
    if (id < 0 || id >= P::kNumParams) return 0.0f;
    return sharedParams_[id].load(std::memory_order_relaxed);
}

void Engine::setAllParams(const float* values) {
    for (int i = 0; i < P::kNumParams; ++i) sharedParams_[i].store(sanitizeParam(i, values[i]), std::memory_order_relaxed);
    paramVersion_.fetch_add(1, std::memory_order_release);
}

void Engine::getAllParams(float* out) const {
    for (int i = 0; i < P::kNumParams; ++i) out[i] = sharedParams_[i].load(std::memory_order_relaxed);
}

void Engine::setPartSampleSet(int part, int layer, std::unique_ptr<SampleSet> set) {
    if (part < 0 || part >= kNumParts || layer < 0 || layer >= kNumLayers) return;
    if (!set) set = std::make_unique<SampleSet>();
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        setNames_[part][layer] = set->name;
        setPaths_[part][layer] = set->sourcePath;
    }
    std::lock_guard<std::mutex> lock(loaderMutex_);
    if (set->streamedSamples() > 0) {
        streamer_.start();
        for (auto& s : set->samples)
            if (s->streamed) s->streamer = &streamer_;
    }
    SampleSet* old = pendingSets_[part][layer].exchange(set.release(), std::memory_order_acq_rel);
    delete old;  // never reached the audio thread
    if (!prepared_) {
        // No audio thread yet: publish immediately.
        SampleSet* s = pendingSets_[part][layer].exchange(nullptr);
        delete currentSets_[part][layer];
        currentSets_[part][layer] = s;
    }
}

std::string Engine::partSampleSetName(int part, int layer) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return part >= 0 && part < kNumParts && layer >= 0 && layer < kNumLayers ? setNames_[part][layer] : std::string();
}

std::string Engine::partSampleSetPath(int part, int layer) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return part >= 0 && part < kNumParts && layer >= 0 && layer < kNumLayers ? setPaths_[part][layer] : std::string();
}

void Engine::setPartParam(int part, int id, float value) {
    if (part < 0 || part >= kNumParts || id < 0 || id >= P::kNumParams) return;
    if (part == 0 || !isPartParam(id)) { setParam(id, value); return; }
    partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)id].store(sanitizeParam(id, value), std::memory_order_relaxed);
    partVersion_[part].fetch_add(1, std::memory_order_release);
}

float Engine::getPartParam(int part, int id) const {
    if (part < 0 || part >= kNumParts || id < 0 || id >= P::kNumParams) return 0.0f;
    if (part == 0 || !isPartParam(id)) return getParam(id);
    return partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)id].load(std::memory_order_relaxed);
}

void Engine::setPartParams(int part, const float* values) {
    if (part < 0 || part >= kNumParts) return;
    for (int i = 0; i < P::kPartParams; ++i) {
        if (!isPartParam(i)) continue;
        const float v = sanitizeParam(i, values[i]);
        if (part == 0) sharedParams_[i].store(v, std::memory_order_relaxed);
        else partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)i].store(v, std::memory_order_relaxed);
    }
    if (part == 0) paramVersion_.fetch_add(1, std::memory_order_release);
    else partVersion_[part].fetch_add(1, std::memory_order_release);
}

void Engine::getPartParams(int part, float* out) const {
    getAllParams(out);
    if (part <= 0 || part >= kNumParts) return;
    for (int i = 0; i < P::kPartParams; ++i)
        if (isPartParam(i)) out[i] = partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)i].load(std::memory_order_relaxed);
}

void Engine::setGlobalParams(const float* values) {
    for (int i = 0; i < P::kNumParams; ++i)
        if (!isPartParam(i)) sharedParams_[i].store(sanitizeParam(i, values[i]), std::memory_order_relaxed);
    paramVersion_.fetch_add(1, std::memory_order_release);
}

void Engine::setPartMix(int part, int id, float value) {
    if (part < 0 || part >= kNumParts || id < 0 || id >= Mix::Count) return;
    mixShared_[part][id].store(sanitizePartMix(id, value), std::memory_order_relaxed);
    mixVersion_.fetch_add(1, std::memory_order_release);
}

float Engine::getPartMix(int part, int id) const {
    if (part < 0 || part >= kNumParts || id < 0 || id >= Mix::Count) return 0.0f;
    return mixShared_[part][id].load(std::memory_order_relaxed);
}

void Engine::collectGarbage() {
    Song* song;
    while (retiredSongs_.pop(song)) delete song;
    SampleSet* s;
    while (retired_.pop(s)) {
        // the streamer may still be closing a slot of this set
        if (streamer_.waitUnused(*s)) delete s;
        // else: still referenced after the timeout - leaked rather than freed under the streamer
    }
    fx::ConvIr* ir;
    while (retiredIr_.pop(ir)) delete ir;
    PhraseSwap* ps;
    while (retiredPhrases_.pop(ps)) delete ps;
    StagedProgram* st;
    while (retiredStages_.pop(st)) {
        if (st->appliedPart >= 0 && !st->keepSound) {
            // applied: its sets now belong to the part (st->sets were cleared)
            std::lock_guard<std::mutex> lock(nameMutex_);
            for (int l = 0; l < kNumLayers; ++l) {
                setNames_[st->appliedPart][l] = st->setNames[l];
                setPaths_[st->appliedPart][l] = st->setPaths[l];
            }
        }
        delete st;
    }
}

void Engine::setFxImpulse(int target, std::unique_ptr<fx::ConvIr> ir) {
    if (target < 0 || target > 1) return;
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        irNames_[target] = ir ? ir->name : std::string();
        irPaths_[target] = ir ? ir->path : std::string();
    }
    std::lock_guard<std::mutex> lock(loaderMutex_);
    if (!prepared_) {
        delete currentIr_[target];
        currentIr_[target] = ir.release();
        return;
    }
    fx::ConvIr* next = ir ? ir.release() : &unloadIr_;
    fx::ConvIr* old = pendingIr_[target].exchange(next, std::memory_order_acq_rel);
    if (old != &unloadIr_) delete old;  // never reached the audio thread
}

std::string Engine::fxImpulseName(int target) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return target >= 0 && target < 2 ? irNames_[target] : std::string();
}

std::string Engine::fxImpulsePath(int target) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return target >= 0 && target < 2 ? irPaths_[target] : std::string();
}

void Engine::partMeters(PartMeter* out) const {
    for (int k = 0; k < kNumParts; ++k) {
        PartMeter& m = out[k];
        m.level = meterPeak_[k].exchange(0.0f, std::memory_order_relaxed);
        m.volume = meterCc_[k][0].load(std::memory_order_relaxed);
        m.expression = meterCc_[k][1].load(std::memory_order_relaxed);
        m.pan = meterCc_[k][2].load(std::memory_order_relaxed);
        m.bend = meterCc_[k][3].load(std::memory_order_relaxed);
        m.mod = meterCc_[k][4].load(std::memory_order_relaxed);
        m.reverb = meterCc_[k][5].load(std::memory_order_relaxed);
        m.chorus = meterCc_[k][6].load(std::memory_order_relaxed);
        m.voices = statPartVoices_[k].load(std::memory_order_relaxed);
        for (int w = 0; w < 4; ++w) m.notes[w] = meterNotes_[k][w].load(std::memory_order_relaxed);
    }
}

void Engine::setMaxVoices(int n) { maxVoices_.store(std::max(1, std::min(kMaxVoices, n))); }

void Engine::setRenderThreads(int n) {
    if (n <= 0) n = autoRenderThreads();
    n = std::max(1, std::min(kMaxRenderThreads, n));
    if (n > 1) {
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (!poolOwner_) {
            // created once (the audio thread may hold the pointer at any time);
            // enough helpers for any later request on this machine
            const int hw = (int)std::thread::hardware_concurrency();
            const int helpers = std::min(WorkerPool::kMaxHelpers, std::max(n - 1, hw - 1));
            workBus_.reset(new float[(size_t)kMaxRenderThreads * kNumParts * 2 * kMaxBlockSize]());
            poolOwner_.reset(new WorkerPool(helpers));
            pool_.store(poolOwner_.get(), std::memory_order_release);
        }
    }
    renderThreads_.store(n);
}

EngineStats Engine::stats() const {
    EngineStats s;
    s.activeVoices = statActive_.load();
    s.peakVoices = statPeakVoices_.load();
    s.cpuLoad = statLoad_.load();
    s.cpuLoadPeak = statLoadPeak_.load();
    s.renderUsAvg = statUs_.load();
    s.renderUsPeak = statUsPeak_.load();
    s.blocks = statBlocks_.load();
    s.droppedEvents = droppedEvents_.load();
    s.lastBlockSize = statBlockSize_.load();
    s.streamVoices = streamer_.activeSlots();
    s.streamUnderruns = streamer_.underruns();
    s.streamBytesRead = streamer_.bytesRead();
    s.outputPeak = statOutPeak_.load();
    return s;
}

void Engine::peakProfile(float out[kProfPhases]) const {
    for (int i = 0; i < kProfPhases; ++i) out[i] = profPeak_[i].load(std::memory_order_relaxed);
}

void Engine::resetPeakStats() {
    profReset_.store(true);
    statPeakVoices_.store(0);
    statLoadPeak_.store(0);
    statUsPeak_.store(0);
    statOutPeak_.store(0);
}

// ------------------------------------------------------------ audio thread

Voice* Engine::allocateVoice(int part) {
    const int limit = maxVoices_.load(std::memory_order_relaxed);
    int playing = 0;
    int count[kNumParts] = {};
    for (auto& v : voices_)
        if (v.active() && !v.stolen()) { ++playing; ++count[v.part]; }
    if (playing >= limit) {
        // steal: a released voice first (lowest level), otherwise the oldest
        // held voice. Across parts the part that would hold the most voices
        // after this note-on loses one, so a busy part cannot starve the
        // others. With a single part this is the plain single-timbral rule.
        ++count[part];
        Voice* victim = nullptr;
        for (auto& v : voices_) {
            if (!v.active() || v.stolen()) continue;
            if (!victim) { victim = &v; continue; }
            const bool vr = v.released(), cr = victim->released();
            if (vr != cr) { if (vr) victim = &v; continue; }
            const int vc = count[v.part], cc = count[victim->part];
            if (vc != cc) { if (vc > cc) victim = &v; continue; }
            if (vr ? v.level() < victim->level() : v.age() < victim->age()) victim = &v;
        }
        if (victim) victim->steal();
    }
    for (auto& v : voices_)
        if (!v.active()) return &v;
    // pool exhausted: reuse the quietest fading voice
    Voice* q = nullptr;
    for (auto& v : voices_)
        if (v.stolen() && (!q || v.level() < q->level())) q = &v;
    if (!q) q = &voices_[0];
    q->hardStop();
    return q;
}

void Engine::noteOn(int part, const Event& in) {
    if (in.value <= 0.0f) {  // velocity 0 => note off
        noteOff(Event::noteOff(in.noteId, in.key), part);
        return;
    }
    Event e = in;
    float send = 1.0f;
    {
        const float curve = parts_[part].p[P::VelCurve];
        if (curve != 0.0f) e.value = std::pow(clampf(e.value, 0.0f, 1.0f), std::exp2(curve * 1.5f));  // >0: harder
    }
    if (multiActive_) {
        const float* m = mix_[part];
        if (m[Mix::Mute] > 0.5f || in.key < (int)m[Mix::KeyLow] || in.key > (int)m[Mix::KeyHigh]) return;
        e.key = (uint8_t)std::max(0, std::min(127, (int)in.key + (int)m[Mix::Transpose]));
        send = m[Mix::FxSend];
    }
    if (e.noteId != 0) {
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.noteId() == e.noteId && v.part == part) v.release();
    }
    bool held = false;
    for (auto& v : voices_)
        if (v.active() && !v.released()) { held = true; break; }
    if (send > 0.0f) rack_.noteOn(e.key, e.value, !held);
    if (!inserts_[part].empty()) {
        bool partHeld = false;
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.part == part) { partHeld = true; break; }
        inserts_[part].noteOn(e.key, !partHeld);
    }
    PartState& ps = parts_[part];
    const auto mode = (VoiceMode)(int)ps.p[P::VoiceMode];
    const bool glideOn = ps.p[P::Glide] > 0.0005f || ps.ccGlide > 0.0005f;
    const bool glideLegatoOnly = ps.p[P::GlideMode] > 0.5f;
    if (mode != VoiceMode::Poly) {
        // note stack: last note priority; a repeated key moves to the top
        int w = 0;
        for (int i = 0; i < ps.stackSize; ++i)
            if (ps.stack[i].key != e.key) ps.stack[w++] = ps.stack[i];
        ps.stackSize = w;
        if (ps.stackSize == PartState::kStack) {
            for (int i = 1; i < ps.stackSize; ++i) ps.stack[i - 1] = ps.stack[i];
            --ps.stackSize;
        }
        Event st = e;
        st.channel = in.key;  // remember the untransposed key for key-matched note-offs
        ps.stack[ps.stackSize++] = st;
        bool sounding = false;
        float from = ps.lastPitch;
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.part == part && v.noteId() == ps.monoId && v.matchKey == ps.monoKey) {
                sounding = true;
                from = v.currentPitch();  // key + glide (unison detune is not part of it)
                break;
            }
        const bool glide = glideOn && from >= 0.0f && (!glideLegatoOnly || sounding);
        if (mode == VoiceMode::Legato && sounding) {
            for (auto& v : voices_)
                if (v.active() && !v.released() && v.part == part && v.noteId() == ps.monoId && v.matchKey == ps.monoKey) {
                    v.legatoTo(e, glide);
                    v.matchKey = in.key;
                }
        } else {
            for (auto& v : voices_)
                if (v.active() && v.part == part && !v.stolen()) v.steal();  // mono: one note at a time
            startNote(part, e, glide ? from : -1.0f);
            for (auto& v : voices_)
                if (v.active() && v.part == part && v.noteId() == e.noteId && !v.stolen()) v.matchKey = in.key;
        }
        ps.monoId = e.noteId;
        ps.monoKey = in.key;
        ps.lastPitch = (float)e.key;
        return;
    }
    bool partHeld = false;
    for (auto& v : voices_)
        if (v.active() && !v.released() && v.part == part) { partHeld = true; break; }
    const bool glide = glideOn && ps.lastPitch >= 0.0f && (!glideLegatoOnly || partHeld);
    startNote(part, e, glide ? ps.lastPitch : -1.0f);
    for (auto& v : voices_)
        if (v.active() && v.part == part && v.noteId() == e.noteId && v.age() == ageCounter_) v.matchKey = in.key;
    ps.lastPitch = (float)e.key;
}

void Engine::startNote(int part, const Event& e, float glideFrom) {
    // SFZ keyswitches and round-robin counters (per part and layer)
    PartState& ps = parts_[part];
    for (int l = 0; l < kNumLayers; ++l) {
        const SampleSet* set = currentSets_[part][l];
        if (!set) continue;
        if (set->isKeyswitch(e.key)) keyswitch_[part][l] = e.key;
        ps.ctx.keyswitch[l] = keyswitch_[part][l];
        ps.ctx.seq[l] = seqCount_[part][l]++;
    }
    const int unison = std::max(1, std::min(8, (int)ps.p[P::Unison]));
    const uint64_t age = ++ageCounter_;  // unison voices share one age (stolen together, oldest first)
    for (int u = 0; u < unison; ++u) {
        const float x = unison > 1 ? (float)u / (float)(unison - 1) * 2.0f - 1.0f : 0.0f;  // -1..1
        Voice* v = allocateVoice(part);
        v->unisonDetune = x * 0.5f * ps.p[P::UnisonDetune] / 100.0f;
        v->unisonPan = x * ps.p[P::UnisonSpread];
        v->start(e, ps.ctx, age, rng_.unipolar(), glideFrom);
        v->startDelay = curOffset_;
        v->part = part;
        v->matchKey = e.key;
        int groups[8];
        const int ng = v->startedGroups(groups, 8);
        for (int g = 0; g < ng; ++g)
            for (auto& o : voices_)
                if (&o != v && o.active() && o.part == part && o.age() != age) o.choke(groups[g]);
    }
}

bool Engine::monoNoteOff(int part, const Event& e) {
    PartState& ps = parts_[part];
    if ((VoiceMode)(int)ps.p[P::VoiceMode] == VoiceMode::Poly || ps.stackSize == 0) return false;
    int found = -1;
    for (int i = 0; i < ps.stackSize; ++i) {
        const Event& s = ps.stack[i];
        if (e.noteId != 0 ? s.noteId == e.noteId : s.channel == e.key) { found = i; break; }
    }
    if (found < 0) return false;
    const bool wasTop = found == ps.stackSize - 1;
    for (int i = found + 1; i < ps.stackSize; ++i) ps.stack[i - 1] = ps.stack[i];
    --ps.stackSize;
    if (!wasTop) return true;  // a note under the sounding one: just forget it
    if (ps.stackSize > 0) {
        // return to the previous held note (legato in both modes, gliding if enabled)
        const Event& top = ps.stack[ps.stackSize - 1];
        const int rawKey = top.channel;  // the stack keeps the untransposed key in .channel
        Event again = top;
        again.channel = kOmniChannel;
        const bool glideOn = ps.p[P::Glide] > 0.0005f || ps.ccGlide > 0.0005f;
        bool any = false;
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.part == part && v.noteId() == ps.monoId) {
                v.legatoTo(again, glideOn);
                v.matchKey = rawKey;
                any = true;
            }
        ps.monoId = again.noteId;
        ps.monoKey = rawKey;
        ps.lastPitch = (float)again.key;
        if (!any) {
            // the voice was stolen meanwhile: start the returning note afresh
            startNote(part, again, -1.0f);
            for (auto& v : voices_)
                if (v.active() && v.part == part && v.noteId() == again.noteId && v.age() == ageCounter_) v.matchKey = rawKey;
        }
        return true;
    }
    // last key released: normal note-off for the sounding voices
    for (auto& v : voices_) {
        if (!v.active() || v.released() || v.sustained || v.part != part || v.noteId() != ps.monoId) continue;
        v.setReleaseVelocity(e.value);
        if (ps.sustain || v.hold) v.sustained = true;
        else releaseVoice(v);
    }
    return true;
}

void Engine::noteOff(const Event& e, int part) {
    bool mono[kNumParts] = {};  // part handled the note-off through its note stack
    for (int k = 0; k < kNumParts; ++k) {
        if (part >= 0 && k != part) continue;
        if ((VoiceMode)(int)parts_[k].p[P::VoiceMode] == VoiceMode::Poly) continue;
        // notes the stack does not know (started before a switch to mono) are
        // released by the normal path below
        mono[k] = monoNoteOff(k, e);
    }
    for (auto& v : voices_) {
        if (!v.active() || v.released() || v.sustained) continue;
        if (part >= 0 && v.part != part) continue;
        if (mono[v.part] && v.noteId() == parts_[v.part].monoId) continue;  // handled by the note stack
        const bool match = e.noteId != 0 ? v.noteId() == e.noteId : v.matchKey == e.key;
        if (!match) continue;
        v.setReleaseVelocity(e.value);
        if (parts_[v.part].sustain || v.hold) v.sustained = true;
        else releaseVoice(v);
    }
}

void Engine::perNote(const Event& e, int part) {
    for (auto& v : voices_) {
        if (!v.active() || v.stolen()) continue;
        if (part >= 0 && v.part != part) continue;
        if (e.noteId != 0 ? v.noteId() != e.noteId : v.matchKey != e.key) continue;
        if (e.type == EventType::NoteHold) {
            v.hold = e.value > 0.5f;
            if (!v.hold && v.sustained && !parts_[v.part].sustain) v.release();
        } else {
            v.applyExpression(e.type, e.value);
        }
    }
}

void Engine::releaseSustained(int part) {
    for (auto& v : voices_)
        if (v.active() && v.part == part && v.sustained && !v.hold) v.release();
}

namespace {
// scale lock: pitch classes of each ScaleType, as bit masks from the root
constexpr uint16_t kScaleMasks[] = {
    0xAB5,  // major 0 2 4 5 7 9 11
    0x5AD,  // minor 0 2 3 5 7 8 10
    0x6AD,  // dorian
    0x5AB,  // phrygian
    0xAD5,  // lydian
    0x6B5,  // mixolydian
    0x9AD,  // harmonic minor 0 2 3 5 7 8 11
    0xAAD,  // melodic minor 0 2 3 5 7 9 11
    0x295,  // major pentatonic 0 2 4 7 9
    0x4A9,  // minor pentatonic 0 3 5 7 10
    0x4E9,  // blues 0 3 5 6 7 10
    0x555,  // whole tone
    0xFFF,  // chromatic
};
// chord mode: intervals of each ChordType (index 0 = off)
struct ChordDef {
    int count;
    int iv[6];
};
constexpr ChordDef kChords[] = {
    {1, {0}},              {3, {0, 4, 7}},         {3, {0, 3, 7}},       {4, {0, 4, 7, 10}}, {4, {0, 4, 7, 11}},
    {4, {0, 3, 7, 10}},    {3, {0, 2, 7}},         {3, {0, 5, 7}},       {3, {0, 7, 12}},    {2, {0, 12}},
    {3, {0, 3, 6}},        {3, {0, 4, 8}},         {4, {0, 4, 7, 14}},   {5, {0, 3, 7, 10, 14}},
};

int quantizeToScale(int key, int root, int type) {
    const int t = std::max(0, std::min((int)(sizeof kScaleMasks / sizeof kScaleMasks[0]) - 1, type));
    const uint16_t mask = kScaleMasks[t];
    for (int d = 0; d <= 6; ++d)
        for (int s : {-d, d}) {  // the nearest scale note, the lower one on a tie
            const int k = key + s;
            const int pc = ((k - root) % 12 + 12) % 12;
            if (mask & (1u << pc)) return std::max(0, std::min(127, k));
        }
    return key;
}
}  // namespace

void Engine::performanceNote(int part, const Event& e) {
    PartState& ps = parts_[part];
    if (e.type == EventType::NoteOn && e.value > 0.0f) {
        PartState::PerfNote* slot = nullptr;
        for (auto& pn : ps.perf)
            if (!pn.used) { slot = &pn; break; }
        if (!slot) slot = &ps.perf[0];  // all in use: reuse (its notes keep going until a note-off finds them)
        const bool lock = ps.p[P::ScaleLock] > 0.5f;
        const int root = (int)ps.p[P::ScaleRoot], type = (int)ps.p[P::ScaleType];
        const int ct = std::max(0, std::min((int)(sizeof kChords / sizeof kChords[0]) - 1, (int)ps.p[P::ChordMode]));
        int iv[6];
        const int n = kChords[ct].count;
        for (int k = 0; k < n; ++k) iv[k] = kChords[ct].iv[k];
        for (int inv = 0; inv < (int)ps.p[P::ChordInversion] && n > 1; ++inv) {  // lowest note up an octave
            int lo = 0;
            for (int k = 1; k < n; ++k) if (iv[k] < iv[lo]) lo = k;
            iv[lo] += 12;
        }
        slot->used = true;
        slot->id = e.noteId;
        slot->key = e.key;
        slot->count = 0;
        // the root moves into the scale first, then the chord is built on it (and kept in the
        // scale: diatonic chords)
        const int base = lock ? quantizeToScale(e.key, root, type) : (int)e.key;
        for (int k = 0; k < n; ++k) {
            int key = std::max(0, std::min(127, base + iv[k]));
            if (lock) key = quantizeToScale(key, root, type);
            bool dup = false;
            for (int j = 0; j < slot->count; ++j) dup = dup || slot->keys[j] == key;
            if (dup) continue;
            Event c = e;
            c.key = (uint8_t)key;
            c.noteId = e.noteId == 0 ? 0 : (slot->count == 0 ? e.noteId : ((e.noteId << 3) | (uint32_t)slot->count | 0x80000000u));
            slot->keys[slot->count] = (uint8_t)key;
            slot->ids[slot->count] = c.noteId;
            ++slot->count;
            partEvent(part, c);
        }
        return;
    }
    // note-off: the notes this key started
    for (auto& pn : ps.perf) {
        if (!pn.used || (e.noteId != 0 ? pn.id != e.noteId : pn.key != e.key)) continue;
        for (int j = 0; j < pn.count; ++j) {
            Event c = e;
            c.type = EventType::NoteOff;
            c.key = pn.keys[j];
            c.noteId = pn.ids[j];
            partEvent(part, c);
        }
        pn.used = false;
        return;
    }
    partEvent(part, e);  // a key pressed before the mode was switched on
}

void Engine::partEvent(int part, const Event& e) {
    PartState& ps = parts_[part];
    if ((e.type == EventType::NoteOn || e.type == EventType::NoteOff) && !ps.inPerf) {
        bool active = ps.p[P::ScaleLock] > 0.5f || ps.p[P::ChordMode] > 0.5f;
        if (!active && e.type == EventType::NoteOff)
            for (const auto& pn : ps.perf) active = active || pn.used;  // switched off while keys were held
        if (active) {
            ps.inPerf = true;
            performanceNote(part, e);
            ps.inPerf = false;
            return;
        }
    }
    if ((e.type == EventType::NoteOn || e.type == EventType::NoteOff) && (seqInput(part, e) || arpInput(part, e))) return;
    switch (e.type) {
        case EventType::NoteOn: noteOn(part, e); break;
        case EventType::NoteOff: noteOff(e, part); break;
        case EventType::NotePitch:
        case EventType::NotePressure:
        case EventType::NoteTimbre:
        case EventType::NoteX:
        case EventType::NoteHold: perNote(e, part); break;
        case EventType::PitchBend: ps.pitchBend = clampf(e.value, -1.0f, 1.0f); break;
        case EventType::ModWheel:
            ps.ctx.modWheel = clampf(e.value, 0.0f, 1.0f);
            for (int i = 0; i < 4; ++i)
                if ((int)ps.p[P::CtlCc1 + i] == 1) ps.ctx.cc[i] = ps.ctx.modWheel;
            break;
        case EventType::ChannelPressure: ps.ctx.chanPressure = clampf(e.value, 0.0f, 1.0f); break;
        case EventType::Sustain: {
            // continuous pedal (half pedal): down from 10 %, notes held below
            // ~85 % fade out (Voice, ctx.pedal)
            ps.pedal = clampf(e.value, 0.0f, 1.0f);
            const bool on = e.value >= 0.1f;
            if (ps.sustain && !on) { ps.sustain = false; releaseSustained(part); }
            ps.sustain = on;
            break;
        }
        case EventType::Controller:
            for (int i = 0; i < 4; ++i)
                if ((int)ps.p[P::CtlCc1 + i] == (int)e.controller) ps.ctx.cc[i] = clampf(e.value, 0.0f, 1.0f);
            switch (e.controller) {
                case 1: ps.ctx.modWheel = clampf(e.value, 0.0f, 1.0f); break;
                case 2: ps.ctx.breath = clampf(e.value, 0.0f, 1.0f); break;
                case 7: if (multiActive_) ps.ccVolume = clampf(e.value, 0.0f, 1.0f); break;
                case 10: if (multiActive_) ps.ccPan = clampf(e.value * 2.0f - 1.0f, -1.0f, 1.0f); break;
                case 11:
                    ps.ctx.expression = clampf(e.value, 0.0f, 1.0f);
                    ps.ccExpr = ps.ctx.expression;  // part level, both modes (unless the preset routes it)
                    break;
                // standard controllers that work on every preset (GM2-style, centre = no change)
                case 5: {  // portamento time: 0 = the preset's glide, then ~5 ms .. 2 s
                    const float v = clampf(e.value, 0.0f, 1.0f);
                    ps.ccGlide = v <= 0.0f ? -1.0f : 0.005f * std::pow(400.0f, v);
                    break;
                }
                case 71: ps.ccReso = clampf((e.value * 127.0f - 64.0f) / 63.0f, -1.0f, 1.0f) * 0.9f; break;
                case 74: ps.ccCutoffOct = clampf((e.value * 127.0f - 64.0f) / 63.0f, -1.0f, 1.0f) * 4.5f; break;
                case 94: ps.ccDly = clampf(e.value, 0.0f, 1.0f); break;
                case 64: partEvent(part, Event::control(EventType::Sustain, e.value)); break;
                case 91: ps.ccRev = clampf(e.value, 0.0f, 1.0f); break;
                case 93: ps.ccCho = clampf(e.value, 0.0f, 1.0f); break;
                // RPN 0 = pitch bend sensitivity (CC101/100 select, CC6 semitones, CC38 cents)
                case 101: ps.rpnMsb = (uint8_t)std::lround(clampf(e.value, 0.0f, 1.0f) * 127.0f); break;
                case 100: ps.rpnLsb = (uint8_t)std::lround(clampf(e.value, 0.0f, 1.0f) * 127.0f); break;
                case 98: case 99: ps.rpnMsb = ps.rpnLsb = 127; break;  // an NRPN deselects the RPN
                case 6:
                case 38:
                    if (ps.rpnMsb == 0 && ps.rpnLsb == 0) {
                        const float v = std::round(clampf(e.value, 0.0f, 1.0f) * 127.0f);
                        ps.knobBend = ps.p[P::BendRange];  // the knob's value now; moving it later wins
                        const float cur = ps.rpnBend >= 0.0f ? ps.rpnBend : ps.p[P::BendRange];
                        // CC6 sets the semitones (and clears the cents), CC38 the cents
                        ps.rpnBend = e.controller == 6 ? std::min(v, 48.0f)
                                                       : std::floor(cur) + std::min(v, 99.0f) / 100.0f;
                    }
                    break;
                case 121:  // reset all controllers (RP-015: not volume, pan, sends or RPN values)
                    ps.pitchBend = 0.0f;
                    ps.ctx.modWheel = 0.0f;
                    ps.ctx.chanPressure = 0.0f;
                    ps.ctx.expression = 1.0f;
                    ps.ccExpr = 1.0f;
                    if (ps.sustain) { ps.sustain = false; releaseSustained(part); }
                    ps.pedal = 0.0f;
                    ps.rpnMsb = ps.rpnLsb = 127;
                    break;
                case 120: arpStop(part); seqStop(part); ps.stackSize = 0; for (auto& v : voices_) if (v.part == part) v.hardStop(); break;
                case 123: arpStop(part); seqStop(part); ps.stackSize = 0; for (auto& v : voices_) if (v.active() && v.part == part) v.release(); break;
                default: break;
            }
            break;
        case EventType::AllNotesOff:
            arpStop(part); seqStop(part);
            ps.sustain = false;
            ps.stackSize = 0;
            for (auto& pn : ps.perf) pn.used = false;
            for (auto& v : voices_) if (v.active() && v.part == part) v.release();
            break;
        case EventType::AllSoundOff:
            arpStop(part); seqStop(part);
            ps.stackSize = 0;
            for (auto& pn : ps.perf) pn.used = false;
            for (auto& v : voices_) if (v.part == part) v.hardStop();
            break;
        case EventType::ProgramChange:  // handled by the host
        default: break;
    }
}

void Engine::handleEvent(const Event& e) {
    if (e.type == EventType::Looper) { rack_.looperCommand((fx::LooperCmd)e.controller); return; }
    if (e.type == EventType::Clock) { clockEvent((ClockMsg)e.controller); return; }
    if (!multiActive_) { partEvent(0, e); return; }
    const bool panic = e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff;
    const bool omni = e.channel == kOmniChannel;
    // Note-id matched note-offs / expression find their voices in any part, so
    // notes end correctly even if the part's channel (or the selected part for
    // omni input) changed while they were held.
    const bool byId = e.noteId != 0 && e.type != EventType::NoteOn && e.type <= EventType::NoteHold;
    if (byId) {
        if (e.type == EventType::NoteOff) {
            for (int k = 0; k < kNumParts; ++k)
                if (omni || (int)mix_[k][Mix::Channel] - 1 == (e.channel & 15)) {
                    seqInput(k, e);
                    arpInput(k, e);
                }
            noteOff(e, -1);
        } else {
            perNote(e, -1);
        }
        return;
    }
    if (omni) {
        if (panic) for (int k = 0; k < kNumParts; ++k) partEvent(k, e);
        else partEvent(selectedPart_.load(std::memory_order_relaxed), e);
        return;
    }
    for (int k = 0; k < kNumParts; ++k)
        if ((int)mix_[k][Mix::Channel] - 1 == (e.channel & 15)) partEvent(k, e);
}

// ------------------------------------------------------------ arpeggiator

bool Engine::arpInput(int part, const Event& e) {
    PartState& ps = parts_[part];
    PartState::Arp& a = ps.arp;
    if (ps.p[P::ArpOn] <= 0.5f) {
        if (a.running || a.heldCount) arpStop(part);
        return false;
    }
    const bool latch = ps.p[P::ArpLatch] > 0.5f;
    if (e.type == EventType::NoteOn && e.value > 0.0f) {
        if (latch && a.physical == 0) a.heldCount = 0;  // new chord replaces the latched one
        ++a.physical;
        int w = 0;
        for (int i = 0; i < a.heldCount; ++i)
            if (a.held[i].key != e.key) a.held[w++] = a.held[i];
        a.heldCount = w;
        if (a.heldCount < PartState::Arp::kMax) a.held[a.heldCount++] = e;
        return true;
    }
    // note-off (or velocity 0)
    bool found = false;
    for (int i = 0; i < a.heldCount; ++i) {
        const Event& h = a.held[i];
        if (e.noteId != 0 ? h.noteId == e.noteId : h.key == e.key) { found = true; break; }
    }
    if (!found) return false;  // a note from before the arp was switched on
    a.physical = std::max(0, a.physical - 1);
    if (!latch) {
        int w = 0;
        for (int i = 0; i < a.heldCount; ++i) {
            const Event& h = a.held[i];
            if (!(e.noteId != 0 ? h.noteId == e.noteId : h.key == e.key)) a.held[w++] = h;
        }
        a.heldCount = w;
    }
    return true;
}

void Engine::arpStop(int part) {
    PartState::Arp& a = parts_[part].arp;
    for (int i = 0; i < a.pendCount; ++i) noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), part);
    a.pendCount = 0;
    a.heldCount = a.physical = 0;
    a.running = false;
}

void Engine::runArps(int frameOffset, int frames) {
    const float tempo = rack_.tempo();
    const double beatsPerFrame = (double)tempo / 60.0 / (double)sampleRate_;
    const bool clock = rack_.clockSyncEnabled() && statClockBpm_.load(std::memory_order_relaxed) > 0.0f;
    const double now = clock ? rack_.beat() + beatsPerFrame * frameOffset : arpBeat_;
    const int parts = multiActive_ ? kNumParts : 1;
    for (int k = 0; k < parts; ++k) {
        PartState& ps = parts_[k];
        PartState::Arp& a = ps.arp;
        if (ps.p[P::ArpOn] <= 0.5f) {
            if (a.running || a.heldCount || a.pendCount) arpStop(k);
            continue;
        }
        // gate ends
        for (int i = 0; i < a.pendCount;) {
            if (now >= a.pend[i].offBeat) {
                noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), k);
                a.pend[i] = a.pend[--a.pendCount];
            } else {
                ++i;
            }
        }
        // latch switched off while nothing is held: drop the latched notes
        if (ps.p[P::ArpLatch] <= 0.5f && a.physical == 0) a.heldCount = 0;
        // MIDI clock transport stopped: the beat stands still, so stop playing
        if (clock && !clockRunning_) {
            for (int i = 0; i < a.pendCount; ++i) noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), k);
            a.pendCount = 0;
            a.running = false;
            continue;
        }
        if (a.heldCount == 0) { a.running = false; continue; }
        const float stepBeats = fx::syncBeats((int)ps.p[P::ArpRate] + 1);  // arp.rate has no "off" entry
        if (a.running) {
            // time base jumped (clock start / appearing / lost): restart the pattern grid
            const double next = a.origin + (double)a.step * stepBeats;
            if (now < next - 2.0 * stepBeats || now > next + 4.0 * stepBeats) {
                a.running = false;
                // gate ends refer to the old time base: end those notes now
                for (int i = 0; i < a.pendCount; ++i) noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), k);
                a.pendCount = 0;
            }
        }
        if (!a.running) {
            a.running = true;
            a.origin = clock ? std::floor(now / stepBeats) * stepBeats : now;  // MIDI clock: stay on the grid
            if (clock && now - a.origin > 1e-6) a.origin += stepBeats;          // ... from the next step
            a.step = 0;
            a.index = 0;
            a.dir = 1;
        }
        const double swing = ps.p[P::ArpSwing];
        for (int guard = 0; guard < 8; ++guard) {  // a few steps per control block at most
            const double t = a.origin + (double)a.step * stepBeats + ((a.step & 1) ? swing * stepBeats : 0.0);
            if (now + 1e-9 < t) break;
            // pattern: held notes x octaves
            int keys[PartState::Arp::kMax * 4];
            float vels[PartState::Arp::kMax * 4];
            int n = 0;
            const auto mode = (ArpMode)(int)ps.p[P::ArpMode];
            Event sorted[PartState::Arp::kMax];
            for (int i = 0; i < a.heldCount; ++i) sorted[i] = a.held[i];
            if (mode != ArpMode::Order)
                std::sort(sorted, sorted + a.heldCount, [](const Event& x, const Event& y) { return x.key < y.key; });
            const int oct = std::max(1, std::min(4, (int)ps.p[P::ArpOctaves]));
            for (int o = 0; o < oct; ++o)
                for (int i = 0; i < a.heldCount; ++i) {
                    const int key = sorted[i].key + 12 * o;
                    if (key > 127) continue;
                    keys[n] = key;
                    vels[n++] = sorted[i].value;
                }
            int chosen[PartState::Arp::kMax * 4];
            int nc = 0;
            if (n > 0) {
                switch (mode) {
                    case ArpMode::Down: chosen[nc++] = n - 1 - (a.index % n); ++a.index; break;
                    case ArpMode::UpDown: {
                        if (n == 1) { chosen[nc++] = 0; break; }
                        const int period = 2 * n - 2;
                        const int ph = a.index % period;
                        chosen[nc++] = ph < n ? ph : period - ph;
                        ++a.index;
                        break;
                    }
                    case ArpMode::Random: chosen[nc++] = (int)(a.rng.unipolar() * (float)n) % n; break;
                    case ArpMode::Chord:
                        for (int i = 0; i < n && i < PartState::Arp::kMax; ++i) chosen[nc++] = i;
                        break;
                    default: chosen[nc++] = a.index % n; ++a.index; break;  // up / order
                }
            }
            const double len = std::max(0.02, (double)ps.p[P::ArpGate]) * stepBeats;
            for (int c = 0; c < nc && a.pendCount < PartState::Arp::kMax; ++c) {
                const uint32_t id = 0xA0000000u | ((uint32_t)k << 24) | (a.nextId++ & 0xFFFFFFu);
                Event on = Event::noteOn(id, keys[chosen[c]], vels[chosen[c]]);
                on.channel = kOmniChannel;
                noteOn(k, on);
                a.pend[a.pendCount++] = {id, (uint8_t)keys[chosen[c]], t + len};
            }
            ++a.step;
        }
    }
    if (!clock) arpBeat_ += beatsPerFrame * frames;
}

// ------------------------------------------------------------ song playback

void Engine::setSong(std::unique_ptr<Song> song) {
    songPlay_.store(false);
    songLen_.store(song ? song->length : 0.0);
    if (!song) songUnload_.store(true);
    delete pendingSong_.exchange(song.release(), std::memory_order_acq_rel);  // a set never picked up
    if (!prepared_) songUpdate();
}

void Engine::songPlay(bool on) { songPlay_.store(on); }

void Engine::songSeek(double seconds) {
    songSeekReq_.store((int64_t)(std::max(0.0, seconds) * sampleRate_));
}

// ------------------------------------------------------------ song program changes

namespace {
// patch key of a song's program change (smf.h): the value carries the bank, if any
int songPatch(const Event& e) { return e.value >= 128.0f ? (int)e.value : e.controller & 127; }
}  // namespace


void Engine::stagePartProgram(int part, std::unique_ptr<StagedProgram> staged) {
    if (!validPart(part) || !staged) return;
    {
        std::lock_guard<std::mutex> lock(loaderMutex_);
        for (SampleSet* set : staged->sets) {
            if (!set || set->streamedSamples() == 0) continue;
            streamer_.start();
            for (auto& smp : set->samples)
                if (smp->streamed) smp->streamer = &streamer_;
        }
    }
    delete stageIn_[part].exchange(staged.release(), std::memory_order_acq_rel);  // never reached the audio thread
    collectGarbage();
}

void Engine::setPartProgram(int part, int program) {
    if (!validPart(part)) return;
    appliedProg_[part].store(program, std::memory_order_relaxed);
    songProg_[part].store(program, std::memory_order_relaxed);
}

void Engine::takeStagedPrograms() {
    for (int k = 0; k < kNumParts; ++k) {
        if (!stageIn_[k].load(std::memory_order_acquire)) continue;
        if (stage_[k]) {
            if (!retiredStages_.push(stage_[k])) continue;  // garbage queue full: retry next block
            stage_[k] = nullptr;
        }
        stage_[k] = stageIn_[k].exchange(nullptr, std::memory_order_acq_rel);
        stagedProg_[k].store(stage_[k] ? stage_[k]->program : -1, std::memory_order_relaxed);
        // late: the song already switched to this program
        if (stage_[k] && song_ && songProg_[k].load(std::memory_order_relaxed) == stage_[k]->program &&
            appliedProg_[k].load(std::memory_order_relaxed) != stage_[k]->program)
            applyStage(k);
    }
}

void Engine::songProgramChange(int part, int program) {
    songProg_[part].store(program, std::memory_order_relaxed);
    if (appliedProg_[part].load(std::memory_order_relaxed) == program) return;
    if (stage_[part] && stage_[part]->program == program) applyStage(part);
    // else: not loaded yet; takeStagedPrograms() applies it when it arrives
}

void Engine::applyStage(int part) {
    StagedProgram* st = stage_[part];
    if (!st || retiredStages_.full()) return;  // garbage queue full: apply next block
    stage_[part] = nullptr;
    stagedProg_[part].store(-1, std::memory_order_relaxed);
    appliedProg_[part].store(st->program, std::memory_order_relaxed);
    st->appliedPart = part;
    if (st->keepSound) {
        retiredStages_.push(st);
        return;
    }
    // notes sounding now keep the old sound (MIDI: a program change affects new notes)
    float* ghost = nullptr;
    bool sounding = false;
    for (const auto& v : voices_) sounding = sounding || (v.active() && v.part == part && !v.paramsOverride);
    if (sounding)
        for (int g = 0; g < kGhosts; ++g)
            if (!ghostUsed_[g]) {
                ghostUsed_[g] = true;
                ghost = ghostParams_.get() + (size_t)g * P::kPartParams;
                std::memcpy(ghost, parts_[part].p, sizeof(float) * P::kPartParams);
                for (auto& v : voices_)
                    if (v.active() && v.part == part && !v.paramsOverride) v.paramsOverride = ghost;
                break;
            }
    // parameters: the audio-thread copy and the shared values (what the UI reads)
    if (part == 0) {
        for (int i = 0; i < P::kPartParams; ++i) {
            if (!isPartParam(i)) continue;
            const float v = sanitizeParam(i, st->params[i]);
            p_[i] = v;
            sharedParams_[i].store(v, std::memory_order_relaxed);
        }
    } else {
        float* dst = partP_.get() + (size_t)(part - 1) * P::kPartParams;
        std::atomic<float>* shared = partShared_.get() + (size_t)(part - 1) * P::kPartParams;
        for (int i = 0; i < P::kPartParams; ++i) {
            if (!isPartParam(i)) continue;
            const float v = sanitizeParam(i, st->params[i]);
            dst[i] = v;
            shared[i].store(v, std::memory_order_relaxed);
        }
    }
    updateInserts(part);
    // samples: notes that keep their old parameters keep their old sets too (held
    // until they end); without a ghost (all in use) voices fade out as on a swap
    for (int l = 0; l < kNumLayers; ++l) {
        SampleSet* set = st->sets[l];
        if (!set) continue;  // the loader passes an empty set to unload a layer
        st->sets[l] = nullptr;
        SampleSet* old = currentSets_[part][l];
        int slot = -1;
        if (ghost && old)
            for (int h = 0; h < kHeld; ++h)
                if (!held_[h]) { slot = h; break; }
        if (slot >= 0) {
            held_[slot] = old;
            currentSets_[part][l] = set;
            keyswitch_[part][l] = -1;
            seqCount_[part][l] = 0;
        } else {
            swapSampleSet(part, l, set, true);
        }
        // notes later in this block start on the new set (the context is filled at block start)
        parts_[part].ctx.sampleSets[l] = currentSets_[part][l];
    }
    retiredStages_.push(st);  // last: the garbage thread may free it at once (its sets are moved out)
}

void Engine::songNotesOff() {
    for (int k = 0; k < kNumParts; ++k) {
        partEvent(k, Event::control(EventType::AllNotesOff, 0.0f));
        PartState& ps = parts_[k];
        ps.pitchBend = 0.0f;
        ps.ctx.modWheel = 0.0f;
        ps.ctx.expression = 1.0f;
        ps.ccExpr = 1.0f;
        ps.ccVolume = 1.0f;
        ps.ccPan = 0.0f;
        ps.pedal = 0.0f;
        ps.ccRev = 0.0f;
        ps.ccCho = 0.0f;
        ps.ccDly = 0.0f;
        ps.ccGlide = -1.0f;
        ps.ccCutoffOct = 0.0f;
        ps.ccReso = 0.0f;
        ps.rpnBend = -1.0f;
        ps.rpnMsb = ps.rpnLsb = 127;
    }
}

void Engine::songChase() {
    // the latest value of every controller / bend / pressure before songIdx_,
    // so volumes, pans and bends are right after a jump or a pause
    static constexpr int kSlots = 131;  // 128 CCs, bend, channel pressure, (spare)
    bool seen[16][kSlots] = {};
    for (size_t i = songIdx_; i-- > 0;) {
        const Event& ev = song_->events[i].event;
        int slot = -1;
        if (ev.type == EventType::Controller) slot = ev.controller & 127;
        else if (ev.type == EventType::PitchBend) slot = 128;
        else if (ev.type == EventType::ChannelPressure) slot = 129;
        else if (ev.type == EventType::ModWheel) slot = 1;
        else if (ev.type == EventType::Sustain) slot = 64;
        const int ch = ev.channel & 15;
        if (slot == 121) {  // a reset: what came before it on these controllers no longer counts
            for (int s : {1, 11, 64, 128, 129}) seen[ch][s] = true;
            continue;
        }
        // RPN / NRPN messages only mean something in order (below); 120-127 are commands
        if (slot < 0 || slot == 6 || slot == 38 || (slot >= 98 && slot <= 101) || (slot >= 120 && slot < 128)) continue;
        if (seen[ch][slot]) continue;
        seen[ch][slot] = true;
        if (slot != 64) handleEvent(ev);  // no sustain: nothing is held after a jump
    }
    // programs: the latest program change on each channel (the loader stages it if needed)
    int prog[16];
    for (int c = 0; c < 16; ++c) prog[c] = song_->initialProgram[c] < 0 ? 0 : song_->initialProgram[c];
    for (size_t i = 0; i < songIdx_; ++i) {
        const Event& ev = song_->events[i].event;
        if (ev.type == EventType::ProgramChange) prog[ev.channel & 15] = songPatch(ev);
    }
    for (int c = 0; c < 16 && c < kNumParts; ++c)
        if (song_->channelUsed[c]) songProgramChange(c, prog[c]);
    // bend ranges: replay the RPN selection in order
    int msb[16], lsb[16];
    float range[16];
    for (int c = 0; c < 16; ++c) { msb[c] = lsb[c] = 127; range[c] = -1.0f; }
    for (size_t i = 0; i < songIdx_; ++i) {
        const Event& ev = song_->events[i].event;
        if (ev.type != EventType::Controller) continue;
        const int ch = ev.channel & 15;
        const int v = (int)std::lround(clampf(ev.value, 0.0f, 1.0f) * 127.0f);
        switch (ev.controller) {
            case 101: msb[ch] = v; break;
            case 100: lsb[ch] = v; break;
            case 98: case 99: case 121: msb[ch] = lsb[ch] = 127; break;
            case 6: if (msb[ch] == 0 && lsb[ch] == 0) range[ch] = (float)std::min(v, 48); break;
            case 38:
                if (msb[ch] == 0 && lsb[ch] == 0) range[ch] = std::floor(std::max(range[ch], 2.0f)) + std::min(v, 99) / 100.0f;
                break;
            default: break;
        }
    }
    auto cc = [&](int c, int num, float v) {
        Event e = Event::control(EventType::Controller, v / 127.0f, num);
        e.channel = (uint8_t)c;
        handleEvent(e);
    };
    for (int c = 0; c < 16; ++c) {
        if (range[c] >= 0.0f) {
            const float semis = std::floor(range[c]);
            cc(c, 101, 0);
            cc(c, 100, 0);
            cc(c, 6, semis);
            cc(c, 38, std::round((range[c] - semis) * 100.0f));
        }
        if (range[c] >= 0.0f || msb[c] != 127 || lsb[c] != 127) {  // the selection the song left
            cc(c, 101, (float)msb[c]);
            cc(c, 100, (float)lsb[c]);
        }
    }
}

void Engine::songUpdate() {
    if (pendingSong_.load(std::memory_order_relaxed) || songUnload_.load(std::memory_order_relaxed)) {
        Song* s = pendingSong_.exchange(nullptr, std::memory_order_acq_rel);
        songUnload_.store(false);
        if (s || song_) {
            if (songWasPlaying_) songNotesOff();
            if (song_ && !retiredSongs_.push(song_)) {
                // garbage queue full: keep the old song until the next block
                if (s) {
                    Song* expected = nullptr;
                    if (!pendingSong_.compare_exchange_strong(expected, s)) delete s;  // never on a real-time path in practice
                }
                return;
            }
            song_ = s;
            songFrames_ = 0;
            songIdx_ = 0;
            songWasPlaying_ = false;
            statSongPos_.store(0.0, std::memory_order_relaxed);
            // staged programs stay: the next song's loader may already have staged one
            // (a stale one is harmless: same GM program, same sound, replaced when needed)
            for (int k = 0; k < kNumParts; ++k)
                if (s) songProg_[k].store(s->initialProgram[k] < 0 ? 0 : s->initialProgram[k], std::memory_order_relaxed);
        }
    }
    takeStagedPrograms();
    if (!song_) {
        songWasPlaying_ = false;
        return;
    }
    const int64_t seek = songSeekReq_.exchange(-1);
    if (seek >= 0) {
        if (songWasPlaying_) songNotesOff();
        songFrames_ = seek;
        const double t = (double)seek / sampleRate_;
        songIdx_ = (size_t)(std::lower_bound(song_->events.begin(), song_->events.end(), t,
                                             [](const SongEvent& a, double v) { return a.time < v; }) -
                            song_->events.begin());
        statSongPos_.store(t, std::memory_order_relaxed);
        songChase();
    }
    const bool play = songPlay_.load(std::memory_order_relaxed);
    // resuming in the middle: stopping reset the controllers (songNotesOff)
    if (play && !songWasPlaying_ && seek < 0 && songIdx_ > 0) songChase();
    if (songWasPlaying_ && !play) songNotesOff();
    songWasPlaying_ = play;
}

void Engine::runSong(int frameOffset, int frames) {
    const double end = (double)(songFrames_ + frameOffset + frames) / sampleRate_;
    const auto& ev = song_->events;
    int budget = 256;  // per control block (dense passages: the rest follows in the next one)
    while (songIdx_ < ev.size() && ev[songIdx_].time < end && budget-- > 0) {
        const Event& e = ev[songIdx_++].event;
        if (e.type == EventType::ProgramChange) songProgramChange(e.channel & 15, songPatch(e));
        else handleEvent(e);
    }
}

// ------------------------------------------------------------ step sequencer

bool Engine::seqActive() const {
    const int parts = multiActive_ ? kNumParts : 1;
    for (int k = 0; k < parts; ++k) {
        const PartState& ps = parts_[k];
        if (ps.p[P::SeqOn] > 0.5f || ps.seq.pendCount > 0 || ps.seq.running) return true;
    }
    return false;
}

void Engine::scheduleControl(int n) {
    jobBeat_ = rack_.beat();
    jobBpf_ = rack_.tempo() / 60.0f / sampleRate_;
    const int parts = multiActive_ ? kNumParts : 1;
    bool arp = false;
    for (int k = 0; k < parts; ++k) arp = arp || parts_[k].p[P::ArpOn] > 0.5f || parts_[k].arp.pendCount > 0;
    const bool seq = seqActive();
    const bool song = song_ && songWasPlaying_;
    const bool phrase = phraseState_ != PhraseState::Empty && phraseState_ != PhraseState::Stopped;
    if (!arp && !seq && !song && !phrase) return;
    for (int off = 0; off < n; off += kControlBlock) {
        const int m = std::min(kControlBlock, n - off);
        curOffset_ = off;
        if (song) runSong(off, m);
        if (phrase) runPhrase(m);
        if (arp) runArps(off, m);
        if (seq) runSeqs(off, m);
    }
    curOffset_ = 0;
    if (song) {
        songFrames_ += n;
        statSongPos_.store((double)songFrames_ / sampleRate_, std::memory_order_relaxed);
        // the end: let the last notes ring for a moment, then stop
        if (songIdx_ >= song_->events.size() && (double)songFrames_ / sampleRate_ > song_->length + 3.0) {
            songEnds_.fetch_add(1, std::memory_order_relaxed);
            songPlay_.store(false);
            songWasPlaying_ = false;
            songNotesOff();
            songFrames_ = 0;
            songIdx_ = 0;
            statSongPos_.store(0.0, std::memory_order_relaxed);
        }
    }
}

bool Engine::seqInput(int part, const Event& e) {
    PartState& ps = parts_[part];
    PartState::Seq& s = ps.seq;
    if (ps.p[P::SeqOn] <= 0.5f || (int)ps.p[P::SeqMode] != 1) {
        s.heldCount = 0;
        return false;
    }
    auto same = [&](const Event& h) { return e.noteId != 0 ? h.noteId == e.noteId : h.key == e.key; };
    if (e.type == EventType::NoteOn && e.value > 0.0f) {
        int w = 0;
        for (int i = 0; i < s.heldCount; ++i)
            if (s.held[i].key != e.key) s.held[w++] = s.held[i];
        s.heldCount = w;
        if (s.heldCount == 8) {  // full: forget the oldest
            for (int i = 1; i < 8; ++i) s.held[i - 1] = s.held[i];
            --s.heldCount;
        }
        s.held[s.heldCount++] = e;
        return true;
    }
    bool found = false;
    int w = 0;
    for (int i = 0; i < s.heldCount; ++i) {
        if (same(s.held[i])) { found = true; continue; }
        s.held[w++] = s.held[i];
    }
    s.heldCount = w;
    return found;
}

void Engine::seqStop(int part) {
    PartState::Seq& s = parts_[part].seq;
    for (int i = 0; i < s.pendCount; ++i) noteOff(Event::noteOff(s.pend[i].id, s.pend[i].key), part);
    s.pendCount = 0;
    s.heldCount = 0;
    s.running = false;
    statSeqStep_[part].store(-1, std::memory_order_relaxed);
}

void Engine::runSeqs(int frameOffset, int frames) {
    (void)frames;
    const double now = jobBeat_ + (double)jobBpf_ * frameOffset;
    const bool transport = transport_.load(std::memory_order_relaxed);
    const int parts = multiActive_ ? kNumParts : 1;
    for (int k = 0; k < parts; ++k) {
        PartState& ps = parts_[k];
        PartState::Seq& s = ps.seq;
        // gate ends
        for (int i = 0; i < s.pendCount;) {
            if (now >= s.pend[i].offBeat) {
                noteOff(Event::noteOff(s.pend[i].id, s.pend[i].key), k);
                s.pend[i] = s.pend[--s.pendCount];
            } else {
                ++i;
            }
        }
        const bool on = ps.p[P::SeqOn] > 0.5f;
        const bool keyMode = (int)ps.p[P::SeqMode] == 1;
        if (!on || (keyMode ? s.heldCount == 0 : !transport)) {
            if (s.running) {
                s.running = false;
                statSeqStep_[k].store(-1, std::memory_order_relaxed);
            }
            continue;
        }
        const float stepBeats = fx::syncBeats((int)ps.p[P::SeqRate] + 1);
        if (stepBeats <= 0.0f) continue;
        if (s.running) {  // the time base jumped (tempo source changed): restart on the grid
            const double next = s.origin + (double)s.step * stepBeats;
            if (now < next - 2.0 * stepBeats || now > next + 4.0 * stepBeats) s.running = false;
        }
        if (!s.running) {
            s.running = true;
            s.step = 0;
            // key mode starts at once; free mode waits for the next step of the grid
            s.origin = keyMode ? now : std::ceil(now / stepBeats - 1e-9) * stepBeats;
        }
        const int steps = std::max(1, std::min(P::kSeqSteps, (int)ps.p[P::SeqSteps]));
        const double swing = ps.p[P::SeqSwing];
        for (int guard = 0; guard < 8; ++guard) {
            const double t = s.origin + (double)s.step * stepBeats + ((s.step & 1) ? swing * stepBeats : 0.0);
            if (now + 1e-9 < t) break;
            const int idx = (int)(s.step % steps);
            statSeqStep_[k].store(idx, std::memory_order_relaxed);
            const float vel = ps.p[P::seqStep(idx, P::SeqVel)];
            if (vel > 0.0f && s.pendCount < PartState::Seq::kMax) {
                int key = (int)ps.p[P::seqStep(idx, P::SeqNote)] + (int)ps.p[P::SeqTranspose];
                float v = vel;
                if (keyMode) {
                    const Event& h = s.held[s.heldCount - 1];
                    key += (int)h.key - 60;
                    v *= 0.5f + 0.5f * h.value;
                }
                key = std::max(0, std::min(127, key));
                const double len = std::max(0.05, (double)ps.p[P::seqStep(idx, P::SeqLen)] * ps.p[P::SeqGate]) * stepBeats;
                const uint32_t id = 0xB0000000u | ((uint32_t)k << 24) | (s.nextId++ & 0xFFFFFFu);
                Event ev = Event::noteOn(id, key, clampf(v, 0.0f, 1.0f));
                ev.channel = kOmniChannel;
                noteOn(k, ev);
                s.pend[s.pendCount++] = {id, (uint8_t)key, t + len};
            }
            ++s.step;
        }
    }
}

void Engine::clockEvent(ClockMsg m) {
    switch (m) {
        case ClockMsg::Tick:
            tickTimes_[tickHead_] = samplePos_;
            tickHead_ = (tickHead_ + 1) % 25;
            tickCount_ = std::min(25, tickCount_ + 1);
            if (clockRunning_) ++ticksSinceStart_;
            if (tickCount_ == 25) {
                // 24 ticks = one quarter note; timestamps are block-quantised, so
                // measure over a whole beat and smooth
                const uint64_t newest = tickTimes_[(tickHead_ + 24) % 25], oldest = tickTimes_[tickHead_];
                if (newest > oldest) {
                    const float bpm = 60.0f * sampleRate_ / (float)(newest - oldest);
                    clockBpm_ = clockBpm_ <= 0.0f || std::fabs(bpm - clockBpm_) > 0.1f * clockBpm_ ? bpm
                                                                                                  : clockBpm_ + 0.2f * (bpm - clockBpm_);
                }
            }
            break;
        case ClockMsg::Start: ticksSinceStart_ = 0; clockRunning_ = true; transport_.store(true); break;
        case ClockMsg::Continue: clockRunning_ = true; break;
        case ClockMsg::Stop: clockRunning_ = false; transport_.store(false); break;
    }
}

void Engine::updateClock(int n) {
    const uint64_t last = tickCount_ ? tickTimes_[(tickHead_ + 24) % 25] : 0;
    const bool active = tickCount_ == 25 && clockBpm_ > 0.0f && samplePos_ - last < (uint64_t)(0.5f * sampleRate_);
    if (!active && tickCount_ > 0 && samplePos_ - last >= (uint64_t)(0.5f * sampleRate_)) {
        tickCount_ = 0;  // clock lost: measure afresh when it comes back
        clockBpm_ = 0.0f;
    }
    // beat position: whole ticks since start plus the time since the last tick
    const double tickLen = active ? 60.0 * sampleRate_ / (clockBpm_ * 24.0) : 1.0;
    const double frac = active ? std::min(0.999, (double)(samplePos_ - last) / tickLen) : 0.0;
    rack_.setExternalClock(active, clockBpm_, ((double)ticksSinceStart_ + frac) / 24.0);
    statClockBpm_.store(active ? clockBpm_ : 0.0f, std::memory_order_relaxed);
    samplePos_ += (uint64_t)n;
}

void Engine::updateEffects() {
    const int q = quality_.load(std::memory_order_relaxed);
    effectsQuality_ = q;
    // Low: no oversampling, Normal: 2x, High: 4x (drive / amp stages)
    rack_.paramsChanged(p_, q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2, q == (int)Quality::High);
    limiter_.enabled = p_[P::Limiter] > 0.5f;
    for (int k = 0; k < kNumParts; ++k) updateInserts(k);
    updateSends();
}

// The send buses use full reverb / delay blocks driven by a compact set of
// global parameters (sendfx.*); they always run 100 % wet.
void Engine::updateSends() {
    if (!sendRev_ || !sendDly_) return;
    float* r = sendRevP_.data();
    r[fx::reverb::On] = 1.0f;
    r[fx::reverb::Type] = p_[P::fxChain(P::SendAType)];
    r[fx::reverb::Size] = p_[P::fxChain(P::SendASize)];
    r[fx::reverb::Decay] = p_[P::fxChain(P::SendADecay)];
    r[fx::reverb::PreDelay] = p_[P::fxChain(P::SendAPreDelay)];
    r[fx::reverb::Damp] = p_[P::fxChain(P::SendADamp)];
    r[fx::reverb::Mix] = 1.0f;
    float* d = sendDlyP_.data();
    d[fx::delay::On] = 1.0f;
    d[fx::delay::Mode] = 1.0f;  // ping-pong
    d[fx::delay::Sync] = p_[P::fxChain(P::SendBSync)];
    d[fx::delay::Time] = p_[P::fxChain(P::SendBTime)];
    d[fx::delay::Feedback] = p_[P::fxChain(P::SendBFeedback)];
    d[fx::delay::HighCut] = p_[P::fxChain(P::SendBTone)];
    d[fx::delay::Mix] = 1.0f;
    fx::FxContext c;
    c.sampleRate = sampleRate_;
    c.tempo = rack_.tempo();
    sendRev_->update(r, c);
    sendDly_->update(d, c);
}

void Engine::processSends(float* L, float* R, int n) {
    if (sendUsed_) sendTail_ = (int)(12.0f * sampleRate_);  // reverb / delay tails ring out after the last send
    sendUsed_ = false;
    if (sendTail_ <= 0) return;
    sendTail_ -= n;
    fx::FxContext c;
    c.sampleRate = sampleRate_;
    c.tempo = rack_.tempo();
    c.beat = rack_.beat();
    if (std::fabs(c.tempo - sendTempo_) > 1e-3f) {  // tempo-synced delay follows the tempo
        sendTempo_ = c.tempo;
        sendDly_->update(sendDlyP_.data(), c);
    }
    const float la = p_[P::fxChain(P::SendALevel)], lb = p_[P::fxChain(P::SendBLevel)];
    sendRev_->process(sendA_[0], sendA_[1], n, c);
    sendDly_->process(sendB_[0], sendB_[1], n, c);
    for (int i = 0; i < n; ++i) {
        L[i] += la * sendA_[0][i] + lb * sendB_[0][i];
        R[i] += la * sendA_[1][i] + lb * sendB_[1][i];
    }
}

void Engine::updateInserts(int part) {
    const int q = effectsQuality_;
    inserts_[part].paramsChanged(parts_[part].p, q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2,
                                 q == (int)Quality::High);
}

int Engine::standardCcIndex(int cc) {
    for (int i = 0; i < 7; ++i)
        if (kStandardCcs[i] == cc) return i;
    return -1;
}

void Engine::setStandardCcOverride(int part, int cc, bool on) {
    const int i = standardCcIndex(cc);
    if (i < 0 || part < 0 || part >= kNumParts) return;
    if (on) ccOverride_[part].fetch_or(1u << i, std::memory_order_relaxed);
    else ccOverride_[part].fetch_and(~(1u << i), std::memory_order_relaxed);
}

bool Engine::standardCcOverridden(int part, int cc) const {
    const int i = standardCcIndex(cc);
    return i >= 0 && part >= 0 && part < kNumParts && (ccOverride_[part].load(std::memory_order_relaxed) >> i) & 1u;
}

void Engine::standardControllers(int part, int n) {
    PartState& ps = parts_[part];
    const uint32_t ovr = ccOverride_[part].load(std::memory_order_relaxed);
    auto learned = [ovr](int cc) { return ((ovr >> standardCcIndex(cc)) & 1u) != 0; };
    // Does the preset route the mod wheel / expression itself (mod matrix)? Then it keeps
    // its own response and the standard one (vibrato / part level) stays out of the way.
    bool wheel = false, expr = false;
    if (ps.ctx.modWheel > 0.0f || ps.ccExpr < 1.0f) {
        for (int s = 0; s < P::kModSlots; ++s) {
            if (ps.p[P::mod(s, P::ModAmt)] == 0.0f || (int)ps.p[P::mod(s, P::ModDst)] <= 0) continue;
            const int src = (int)ps.p[P::mod(s, P::ModSrc)], via = (int)ps.p[P::mod(s, P::ModVia)];
            wheel = wheel || src == (int)ModSource::ModWheel || via == (int)ModSource::ModWheel;
            expr = expr || src == (int)ModSource::Expression || via == (int)ModSource::Expression;
        }
    }
    ps.exprLevel = expr || learned(11) ? 1.0f : ps.ccExpr;
    // CC1: vibrato 5.5 Hz, up to +-0.5 semitone at full wheel
    if (!wheel && !learned(1) && ps.ctx.modWheel > 0.0f) {
        ps.vibPhase += 2.0f * 3.14159265f * 5.5f * (float)n / sampleRate_;
        if (ps.vibPhase > 2.0f * 3.14159265f) ps.vibPhase -= 2.0f * 3.14159265f;
        ps.ctx.vibrato = ps.ctx.modWheel * 0.5f * std::sin(ps.vibPhase);
    } else {
        ps.ctx.vibrato = 0.0f;
    }
    ps.ctx.ccCutoffOct = learned(74) ? 0.0f : ps.ccCutoffOct;
    ps.ctx.ccReso = learned(71) ? 0.0f : ps.ccReso;
    ps.ctx.ccGlide = learned(5) ? -1.0f : ps.ccGlide;
    ps.revSend = learned(91) ? 0.0f : ps.ccRev * 0.6f;
    ps.dlySend = learned(94) ? 0.0f : ps.ccDly * 0.6f;
}

void Engine::updatePartTargets() {
    for (int k = 0; k < kNumParts; ++k) {
        PartState& ps = parts_[k];
        const float* m = mix_[k];
        const float vol = m[Mix::Mute] > 0.5f ? 0.0f
                                               : m[Mix::Volume] * m[Mix::Volume] * ps.ccVolume * ps.ccVolume * ps.exprLevel * ps.exprLevel;
        const float pan = clampf(m[Mix::Pan] + ps.ccPan, -1.0f, 1.0f);
        const float gl = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
        const float gr = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
        const float send = m[Mix::FxSend];
        ps.target[0] = gl * send;
        ps.target[1] = gr * send;
        ps.target[2] = gl * (1.0f - send);
        ps.target[3] = gr * (1.0f - send);
    }
}

// playing: a voice of the part still sounds the layer (found by the caller in
// one pass over the voices for every pending swap: a song switch replaces up to
// 16 x 3 sets in one block, and a pass per set took ~1 ms on a phone)
void Engine::swapSampleSet(int part, int layer, SampleSet* s, bool playing) {
    SampleSet* old = currentSets_[part][layer];
    keyswitch_[part][layer] = -1;
    seqCount_[part][layer] = 0;
    if (!old) {
        currentSets_[part][layer] = s;
        return;
    }
    // Voices still playing the old set fade out (no click) while it stays alive
    // in retiring_; with nothing playing it, or no free slot, it goes to the
    // garbage queue right away (sounding voices then stop at once).
    int slot = -1;
    for (int i = 0; i < kRetiring && playing; ++i)
        if (!retiring_[i].set) { slot = i; break; }
    if (slot >= 0) {
        for (auto& v : voices_)
            if (v.active() && v.part == part) v.fadeLayer(layer);
        retiring_[slot].set = old;
        retiring_[slot].frames = (int)(2.0f * Voice::kSwapFadeSeconds * sampleRate_) + kControlBlock;
        currentSets_[part][layer] = s;
        return;
    }
    if (!retired_.push(old)) {
        // garbage queue full (collectGarbage not called): put the new set back
        // and retry next block
        SampleSet* expected = nullptr;
        if (!pendingSets_[part][layer].compare_exchange_strong(expected, s)) retired_.push(s);
        return;
    }
    if (playing)
        for (auto& v : voices_)
            if (v.active() && v.part == part) v.stopLayer(layer);
    currentSets_[part][layer] = s;
}

void Engine::releaseHeld() {
    for (int g = 0; g < kGhosts; ++g) {
        if (!ghostUsed_[g]) continue;
        const float* buf = ghostParams_.get() + (size_t)g * P::kPartParams;
        bool used = false;
        for (const auto& v : voices_) used = used || (v.active() && v.paramsOverride == buf);
        if (!used) ghostUsed_[g] = false;
    }
    for (auto& h : held_) {
        if (!h) continue;
        bool used = false;
        for (const auto& v : voices_) used = used || v.usesSet(h);
        if (!used && retired_.push(h)) h = nullptr;  // queue full: next block
    }
}

void Engine::advanceRetiring(int n) {
    for (auto& r : retiring_) {
        if (!r.set) continue;
        r.frames -= n;
        if (r.frames > 0) continue;
        // normally every voice has faded out of it by now; a voice that was not
        // rendered meanwhile must not keep reading it after it is freed
        for (auto& v : voices_)
            if (v.active()) v.dropSet(r.set);
        if (retired_.push(r.set)) r.set = nullptr;  // queue full: retry next block
    }
}

// ------------------------------------------------------------ multi-core
namespace {
// Fewer active voices: render on the audio thread alone. Waiting for a helper
// that the OS preempted costs more than a few voices (measured on a phone:
// 1-3 ms stalls with 6-18 voices in parallel, ~15 us per voice serially).
constexpr int kParallelMinVoices = 24;
constexpr int kVoicesPerThread = 6;  // at least this many voices per render thread
}  // namespace

void Engine::renderJob(void* self, int worker, int item) {
    Engine& e = *static_cast<Engine*>(self);
    enableFlushToZero();  // per thread (helpers)
    Voice& v = e.voices_[e.jobVoices_[item]];
    const int k = e.jobMulti_ ? v.part : 0;
    const int n = e.jobN_;
    float* bl = e.workBus(worker, k, 0);
    float* br = e.workBus(worker, k, 1);
    const uint32_t bit = 1u << k;
    if (!(e.workUsed_[worker] & bit)) {
        e.workUsed_[worker] |= bit;
        std::memset(bl, 0, sizeof(float) * n);
        std::memset(br, 0, sizeof(float) * n);
    }
    VoiceContext c = e.parts_[k].ctx;
    for (int off = 0, b = 0; off < n && v.active(); off += kControlBlock, ++b) {
        if (v.startDelay > off) continue;
        if (v.releaseAt >= 0 && v.releaseAt <= off) { v.release(); v.releaseAt = -1; }
        c.lfo2 = e.jobLfo2_[k][b];
        c.beat = e.jobBeat_ + (double)e.jobBpf_ * off;
        v.render(bl + off, br + off, std::min(kControlBlock, n - off), c);
    }
}

void Engine::insertJob(void* self, int worker, int item) {
    Engine& e = *static_cast<Engine*>(self);
    if (worker > 0) enableFlushToZero();
    const int k = e.jobInsParts_[item];
    e.inserts_[k].process(e.bus(k, 0), e.bus(k, 1), e.jobN_);
}

bool Engine::renderParallel(float* L, float* R, int n, bool multi, bool* used) {
    WorkerPool* pool = pool_.load(std::memory_order_acquire);
    const int threads = renderThreads_.load(std::memory_order_relaxed);
    if (!pool || threads < 2 || n > kMaxBlockSize) return false;
    int count = 0;
    for (int i = 0; i < VoicePool::kSize; ++i)
        if (voices_[i].active()) jobVoices_[count++] = i;
    if (count < kParallelMinVoices) return false;

    // LFO2 of every control block, in the same order as the serial loop (each
    // part's LFO has its own state); the context keeps the last value, which
    // the effect rack reads
    const int sub = (n + kControlBlock - 1) / kControlBlock;
    for (int k = 0; k < (multi ? kNumParts : 1); ++k) {
        PartState& ps = parts_[k];
        for (int b = 0; b < sub; ++b) {
            const int m = std::min(kControlBlock, n - b * kControlBlock);
            jobLfo2_[k][b] = ps.lfo2.advance((LfoWave)(int)ps.p[P::Lfo2Wave], lfo2Rate(ps.p), (float)m / sampleRate_,
                                             k == 0 ? rng_ : ps.rng);
        }
        ps.ctx.lfo2 = jobLfo2_[k][sub - 1];
    }
    jobN_ = n;
    jobMulti_ = multi;
    const int helpers = std::max(0, std::min(std::min(threads - 1, pool->helpers()), count / kVoicesPerThread - 1));
    for (int w = 0; w <= helpers; ++w) workUsed_[w] = 0;
    pool->run(&Engine::renderJob, this, count, helpers);

    // sum the per-thread buses (fixed worker order)
    for (int w = 0; w <= helpers; ++w) {
        const uint32_t mask = workUsed_[w];
        if (!mask) continue;
        if (!multi) {
            const float* wl = workBus(w, 0, 0);
            const float* wr = workBus(w, 0, 1);
            for (int i = 0; i < n; ++i) {
                L[i] += wl[i];
                R[i] += wr[i];
            }
            continue;
        }
        for (int k = 0; k < kNumParts; ++k) {
            if (!(mask & (1u << k))) continue;
            const float* wl = workBus(w, k, 0);
            const float* wr = workBus(w, k, 1);
            float* bl = bus(k, 0);
            float* br = bus(k, 1);
            if (!used[k]) {
                used[k] = true;
                std::memcpy(bl, wl, sizeof(float) * n);
                std::memcpy(br, wr, sizeof(float) * n);
                continue;
            }
            for (int i = 0; i < n; ++i) {
                bl[i] += wl[i];
                br[i] += wr[i];
            }
        }
    }
    return true;
}

// Multi-timbral render: voices go into per-part buses, which run through the
// part's insert effects and are mixed (volume / pan / FX send) into the rack
// bus (L/R) and the dry bus.
void Engine::processMulti(float* L, float* R, int n) {
    std::memset(sendA_, 0, sizeof sendA_);
    std::memset(sendB_, 0, sizeof sendB_);
    std::memset(dryL_, 0, sizeof(float) * n);
    std::memset(dryR_, 0, sizeof(float) * n);
    bool used[kNumParts] = {};
    scheduleControl(n);
    const bool parallel = renderParallel(nullptr, nullptr, n, true, used);
    profParallel_ = parallel;
    for (int off = 0; !parallel && off < n; off += kControlBlock) {
        const int m = std::min(kControlBlock, n - off);
        for (int k = 0; k < kNumParts; ++k) {
            PartState& ps = parts_[k];
            ps.ctx.lfo2 = ps.lfo2.advance((LfoWave)(int)ps.p[P::Lfo2Wave], lfo2Rate(ps.p), (float)m / sampleRate_,
                                          k == 0 ? rng_ : ps.rng);
            ps.ctx.beat = jobBeat_ + (double)jobBpf_ * off;
        }
        for (auto& v : voices_) {
            if (!v.active() || v.startDelay > off) continue;
            if (v.releaseAt >= 0 && v.releaseAt <= off) { v.release(); v.releaseAt = -1; }
            const int k = v.part;
            if (!used[k]) {
                used[k] = true;
                std::memset(bus(k, 0), 0, sizeof(float) * n);
                std::memset(bus(k, 1), 0, sizeof(float) * n);
            }
            v.render(bus(k, 0) + off, bus(k, 1) + off, m, parts_[k].ctx);
        }
    }
    profMark(3);
    const float step = gainStep_;
    // part insert effects (independent per part: in parallel when enabled)
    int insParts = 0;
    for (int k = 0; k < kNumParts; ++k) {
        fx::InsertChain& ins = inserts_[k];
        if (!used[k] && ins.busy()) {  // let insert tails ring out
            used[k] = true;
            std::memset(bus(k, 0), 0, sizeof(float) * n);
            std::memset(bus(k, 1), 0, sizeof(float) * n);
        }
        if (used[k] && !ins.empty()) {
            ins.setTempo(rack_.tempo(), rack_.beat());
            jobInsParts_[insParts++] = k;
        }
    }
    jobN_ = n;
    WorkerPool* pool = pool_.load(std::memory_order_acquire);
    const int threads = renderThreads_.load(std::memory_order_relaxed);
    if (pool && threads > 1 && insParts >= 2) {
        pool->run(&Engine::insertJob, this, insParts, std::min(threads - 1, insParts - 1));
    } else {
        for (int i = 0; i < insParts; ++i) insertJob(this, 0, i);
    }
    profMark(4);
    for (int k = 0; k < kNumParts; ++k) {
        PartState& ps = parts_[k];
        if (!used[k]) {
            for (int c = 0; c < 4; ++c) ps.gain[c] += clampf(ps.target[c] - ps.gain[c], -step * n, step * n);
            continue;
        }
        float* bl = bus(k, 0);
        float* br = bus(k, 1);
        {
            const float sa = std::min(1.0f, ps.p[P::SendA] + ps.revSend), sb = std::min(1.0f, ps.p[P::SendB] + ps.dlySend);
            {
                float pk = 0.0f;
                for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(bl[i]), std::fabs(br[i])));
                blockPeak_[k] = pk * std::max(ps.gain[0] + ps.gain[2], ps.gain[1] + ps.gain[3]);
            }
            if (sa > 0.0f || sb > 0.0f) {
                sendUsed_ = true;
                const float gl = ps.gain[0] + ps.gain[2], gr = ps.gain[1] + ps.gain[3];
                for (int i = 0; i < n; ++i) {
                    sendA_[0][i] += bl[i] * gl * sa;
                    sendA_[1][i] += br[i] * gr * sa;
                    sendB_[0][i] += bl[i] * gl * sb;
                    sendB_[1][i] += br[i] * gr * sb;
                }
            }
        }
        bool steady = true;
        for (int c = 0; c < 4; ++c) steady = steady && ps.gain[c] == ps.target[c];
        if (steady) {
            const float* g = ps.gain;
            for (int i = 0; i < n; ++i) {
                L[i] += bl[i] * g[0];
                R[i] += br[i] * g[1];
                dryL_[i] += bl[i] * g[2];
                dryR_[i] += br[i] * g[3];
            }
            continue;
        }
        float* g = ps.gain;
        const float* t = ps.target;
        for (int i = 0; i < n; ++i) {  // linear ramps toward the targets
            for (int c = 0; c < 4; ++c) g[c] += clampf(t[c] - g[c], -step, step);
            L[i] += bl[i] * g[0];
            R[i] += br[i] * g[1];
            dryL_[i] += bl[i] * g[2];
            dryR_[i] += br[i] * g[3];
        }
    }
}

void Engine::process(float* L, float* R, int n) {
    const auto t0 = std::chrono::steady_clock::now();
    enableFlushToZero();
    if (!prepared_ || n <= 0) {
        if (n > 0) { std::memset(L, 0, sizeof(float) * n); std::memset(R, 0, sizeof(float) * n); }
        return;
    }
    if (n > kMaxBlockSize && (multiActive_ || multi_.load(std::memory_order_relaxed))) {
        // the dry bus holds kMaxBlockSize frames
        for (int off = 0; off < n; off += kMaxBlockSize) process(L + off, R + off, std::min(kMaxBlockSize, n - off));
        return;
    }

    profMark_[0] = t0;
    // 1. parameters
    const uint32_t ver = paramVersion_.load(std::memory_order_acquire);
    if (ver != seenParamVersion_) {
        seenParamVersion_ = ver;
        for (int i = 0; i < P::kNumParams; ++i) p_[i] = sharedParams_[i].load(std::memory_order_relaxed);
        updateEffects();
    } else if (effectsQuality_ != quality_.load(std::memory_order_relaxed)) {
        updateEffects();
    }
    for (int k = 1; k < kNumParts; ++k) {
        const uint32_t pv = partVersion_[k].load(std::memory_order_acquire);
        if (pv == seenPartVersion_[k]) continue;
        seenPartVersion_[k] = pv;
        float* dst = partP_.get() + (size_t)(k - 1) * P::kPartParams;
        const std::atomic<float>* src = partShared_.get() + (size_t)(k - 1) * P::kPartParams;
        for (int i = 0; i < P::kPartParams; ++i) dst[i] = src[i].load(std::memory_order_relaxed);
        updateInserts(k);
    }
    const uint32_t mv = mixVersion_.load(std::memory_order_acquire);
    if (mv != seenMixVersion_) {
        seenMixVersion_ = mv;
        for (int k = 0; k < kNumParts; ++k)
            for (int i = 0; i < Mix::Count; ++i) mix_[k][i] = mixShared_[k][i].load(std::memory_order_relaxed);
    }
    const bool multi = multi_.load(std::memory_order_relaxed);
    if (multi != multiActive_) {
        multiActive_ = multi;
        if (!multi) {
            // back to single-part mode: only part 0 may sound
            for (auto& v : voices_) if (v.part != 0) v.hardStop();
        }
        for (auto& ps : parts_) {
            ps.ccVolume = 1.0f;
            ps.ccPan = 0.0f;
            ps.ccExpr = 1.0f;
        }
        updatePartTargets();
        for (auto& ps : parts_) std::memcpy(ps.gain, ps.target, sizeof ps.gain);
    }
    for (int t = 0; t < 2; ++t) {
        if (!pendingIr_[t].load(std::memory_order_acquire)) continue;
        if (currentIr_[t] && !retiredIr_.push(currentIr_[t])) continue;  // garbage queue full: retry next block
        fx::ConvIr* p = pendingIr_[t].exchange(nullptr, std::memory_order_acq_rel);
        currentIr_[t] = p == &unloadIr_ ? nullptr : p;
        rack_.setUserIr(t, currentIr_[t]);
    }

    profMark(1);
    // 2. sample set swaps (one pass over the voices for all of them)
    {
        SampleSet* incoming[kNumParts][kNumLayers] = {};
        bool any = false;
        for (int k = 0; k < kNumParts; ++k)
            for (int l = 0; l < kNumLayers; ++l) {
                if (!pendingSets_[k][l].load(std::memory_order_relaxed)) continue;
                incoming[k][l] = pendingSets_[k][l].exchange(nullptr, std::memory_order_acq_rel);
                any = any || incoming[k][l];
            }
        if (any) {
            bool playing[kNumParts][kNumLayers] = {};
            for (auto& v : voices_) {
                if (!v.active()) continue;
                const int k = v.part & (kNumParts - 1);
                for (int l = 0; l < kNumLayers; ++l)
                    if (incoming[k][l] && !playing[k][l] && v.playsLayer(l)) playing[k][l] = true;
            }
            for (int k = 0; k < kNumParts; ++k)
                for (int l = 0; l < kNumLayers; ++l)
                    if (incoming[k][l]) swapSampleSet(k, l, incoming[k][l], playing[k][l]);
        }
    }

    // 3. events (voices started here read the part contexts)
    for (int k = 0; k < kNumParts; ++k) {
        parts_[k].ctx.params = parts_[k].p;
        for (int l = 0; l < kNumLayers; ++l) parts_[k].ctx.sampleSets[l] = currentSets_[k][l];
    }
    phraseBlockStart();
    Event e;
    int budget = 512;
    while (budget-- > 0 && events_.pop(e)) {
        handleEvent(e);
        phraseCapture(e);
    }
    songUpdate();

    updateClock(n);

    profMark(2);
    // 4. render
    for (auto& ps : parts_) {
        ps.ctx.tempo = rack_.tempo();
        ps.ctx.quality = quality_.load(std::memory_order_relaxed);
        ps.ctx.masterTune = p_[P::MasterTune] * 0.01f;
        ps.ctx.pitchBendNorm = ps.pitchBend;
        if (ps.p[P::BendRange] != ps.knobBend) {  // the user moved the knob (or a preset loaded)
            ps.knobBend = ps.p[P::BendRange];
            ps.rpnBend = -1.0f;
        }
        ps.ctx.pitchBendSemis = ps.pitchBend * (ps.rpnBend >= 0.0f ? ps.rpnBend : ps.p[P::BendRange]);
        ps.ctx.pedal = ps.pedal;
        ps.ctx.superKnob = p_[P::SuperKnob];  // one Super Knob for every part
        standardControllers((int)(&ps - parts_), n);
    }
    VoiceContext& ctx0 = parts_[0].ctx;

    std::memset(L, 0, sizeof(float) * n);
    std::memset(R, 0, sizeof(float) * n);
    if (multiActive_) {
        updatePartTargets();
        processMulti(L, R, n);
    } else {
        scheduleControl(n);
        const bool parallel = renderParallel(L, R, n, false, nullptr);
        for (int off = 0; !parallel && off < n; off += kControlBlock) {
            const int m = std::min(kControlBlock, n - off);
            ctx0.lfo2 = parts_[0].lfo2.advance((LfoWave)(int)p_[P::Lfo2Wave], lfo2Rate(p_), (float)m / sampleRate_, rng_);
            ctx0.beat = jobBeat_ + (double)jobBpf_ * off;
            for (auto& v : voices_) {
                if (!v.active() || v.startDelay > off) continue;
                if (v.releaseAt >= 0 && v.releaseAt <= off) { v.release(); v.releaseAt = -1; }
                v.render(L + off, R + off, m, ctx0);
            }
        }
        if (!inserts_[0].empty()) {
            inserts_[0].setTempo(rack_.tempo(), rack_.beat());
            inserts_[0].process(L, R, n);
        }
        {  // CC11 expression as the level (ramped over the block)
            PartState& p0 = parts_[0];
            const float target = p0.exprLevel * p0.exprLevel;
            if (target != 1.0f || p0.singleGain != 1.0f) {
                const float g0 = p0.singleGain, step = (target - g0) / (float)n;
                for (int i = 0; i < n; ++i) {
                    const float g = g0 + step * (float)(i + 1);
                    L[i] *= g;
                    R[i] *= g;
                }
                p0.singleGain = target;
            }
        }
        // send buses (post insert)
        const float sa = std::min(1.0f, p_[P::SendA] + parts_[0].revSend),
                    sb = std::min(1.0f, p_[P::SendB] + parts_[0].dlySend);
        if (sa > 0.0f || sb > 0.0f) {
            sendUsed_ = true;
            for (int i = 0; i < n; ++i) {
                sendA_[0][i] = L[i] * sa;
                sendA_[1][i] = R[i] * sa;
                sendB_[0][i] = L[i] * sb;
                sendB_[1][i] = R[i] * sb;
            }
        } else {
            std::memset(sendA_, 0, sizeof sendA_);
            std::memset(sendB_, 0, sizeof sendB_);
        }
    }

    if (!multiActive_) {  // single part: voices + inserts are one phase
        profMark(3);
        profMark(4);
    }
    // 5. effect rack, then master. The rack's FX modulation follows part 0's
    // controllers (mod wheel, LFO2, ...).
    bool held = false;
    for (auto& v : voices_)
        if (v.active() && !v.released()) { held = true; break; }
    fx::FxGlobals g;
    g.modWheel = ctx0.modWheel;
    g.lfo2 = ctx0.lfo2;
    g.chanPressure = ctx0.chanPressure;
    g.expression = ctx0.expression;
    g.breath = ctx0.breath;
    g.pitchBend = parts_[0].pitchBend;
    g.notesHeld = held;
    g.sustain = parts_[0].pedal;
    rack_.process(L, R, n, g, inL_, inR_);
    inL_ = inR_ = nullptr;
    if (multiActive_) {
        for (int i = 0; i < n; ++i) {
            L[i] += dryL_[i];
            R[i] += dryR_[i];
        }
    }
    processSends(L, R, n);
    for (auto& v : voices_) {  // block done: every scheduled start / release has happened
        v.startDelay = 0;
        if (v.releaseAt >= 0) { v.release(); v.releaseAt = -1; }
    }
    const float target = p_[P::MasterVolume] * p_[P::MasterVolume];  // perceptual taper
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float mg = masterGain_.process(target);
        L[i] *= mg;
        R[i] *= mg;
    }
    limiter_.process(L, R, n);
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));

    advanceRetiring(n);
    releaseHeld();
    profMark(5);

    // 6. stats
    int active = 0;
    int perPart[kNumParts] = {};
    for (auto& v : voices_)
        if (v.active()) { ++active; ++perPart[v.part]; }
    for (int k = 0; k < kNumParts; ++k) statPartVoices_[k].store(perPart[k], std::memory_order_relaxed);
    {
        uint32_t notes[kNumParts][4] = {};
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.key() >= 0 && v.key() < 128)
                notes[v.part & (kNumParts - 1)][v.key() >> 5] |= 1u << (v.key() & 31);
        if (!multiActive_) {
            float pk = 0.0f;
            for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
            blockPeak_[0] = pk;
        }
        for (int k = 0; k < kNumParts; ++k) {
            const PartState& ps = parts_[k];
            atomicMax(meterPeak_[k], blockPeak_[k]);
            blockPeak_[k] = 0.0f;
            const float cc[7] = {ps.ccVolume, ps.ccExpr, ps.ccPan, ps.pitchBend, ps.ctx.modWheel, ps.ccRev, ps.ccCho};
            for (int c = 0; c < 7; ++c) meterCc_[k][c].store(cc[c], std::memory_order_relaxed);
            for (int w = 0; w < 4; ++w) meterNotes_[k][w].store(notes[k][w], std::memory_order_relaxed);
        }
    }
    statActive_.store(active, std::memory_order_relaxed);
    atomicMaxI(statPeakVoices_, active);
    atomicMax(statOutPeak_, peak);
    statBlockSize_.store(n, std::memory_order_relaxed);
    const float us = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
    const float load = us / (1e6f * (float)n / sampleRate_);
    statUs_.store(statUs_.load(std::memory_order_relaxed) * 0.95f + us * 0.05f, std::memory_order_relaxed);
    statLoad_.store(statLoad_.load(std::memory_order_relaxed) * 0.95f + load * 0.05f, std::memory_order_relaxed);
    atomicMax(statUsPeak_, us);
    atomicMax(statLoadPeak_, load);
    if (profReset_.exchange(false, std::memory_order_relaxed)) profPeakTotal_ = 0.0f;
    if (us > profPeakTotal_) {
        // single-part mode has no separate insert phase: marks 3 / 4 stay from an
        // earlier block, so clamp each phase at >= 0
        profMark(6);
        profPeakTotal_ = us;
        profPeak_[6].store((float)active, std::memory_order_relaxed);
        profPeak_[7].store((float)profParallel_, std::memory_order_relaxed);
        for (int i = 0; i < 6; ++i)
            profPeak_[i].store(std::max(0.0f, std::chrono::duration<float, std::micro>(profMark_[i + 1] - profMark_[i]).count()),
                               std::memory_order_relaxed);
    }
    statBlocks_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace hsynth

namespace hsynth {

// ------------------------------------------------------------ phrase recorder

void Engine::phraseRecord() { phraseCmds_.push(CmdRecord); }
void Engine::phrasePlay() { phraseCmds_.push(CmdPlay); }
void Engine::phraseStop() { phraseCmds_.push(CmdStop); }
void Engine::phraseClear() { phraseCmds_.push(CmdClear); }
void Engine::phraseUndo() { phraseCmds_.push(CmdUndo); }

bool Engine::takePhraseSnapshot(std::vector<PhraseEvent>& out, double& lengthBeats) {
    if (!snapReady_.load(std::memory_order_acquire)) return false;
    out.assign(snapBuf_.begin(), snapBuf_.end());
    lengthBeats = snapLen_;
    snapReady_.store(false, std::memory_order_release);
    return true;
}

void Engine::replacePhrase(const std::vector<PhraseEvent>& events, double lengthBeats) {
    auto* s = new PhraseSwap();
    s->events.reserve(kMaxPhraseEvents);
    for (const auto& e : events)
        if (s->events.size() < (size_t)kMaxPhraseEvents) s->events.push_back(e);
    std::stable_sort(s->events.begin(), s->events.end(), [](const PhraseEvent& a, const PhraseEvent& b) { return a.beat < b.beat; });
    s->length = lengthBeats;
    delete pendingPhrase_.exchange(s, std::memory_order_acq_rel);  // a previous one never reached the audio thread
}

void Engine::phraseDispatch(const Event& e) {
    if (e.type == EventType::NoteOn && e.value > 0.0f) {
        if (playHeldCount_ < kPhraseHeld) playHeld_[playHeldCount_++] = {e.noteId, e.key, e.channel};
    } else if (e.type == EventType::NoteOff || e.type == EventType::NoteOn) {
        for (int i = 0; i < playHeldCount_; ++i)
            if (playHeld_[i].id == e.noteId && playHeld_[i].key == e.key) {
                playHeld_[i] = playHeld_[--playHeldCount_];
                break;
            }
    }
    handleEvent(e);
}

void Engine::phraseSilence() {
    for (int i = 0; i < playHeldCount_; ++i) {
        Event off = Event::noteOff(playHeld_[i].id, playHeld_[i].key);
        off.channel = playHeld_[i].channel;
        handleEvent(off);
    }
    playHeldCount_ = 0;
}

void Engine::phraseInsert(const PhraseEvent& pe, bool atPlayhead) {
    if (phrase_.size() >= (size_t)kMaxPhraseEvents) return;  // full: the event is not recorded
    if (!atPlayhead) {
        phrase_.push_back(pe);  // first pass: in time order already
        return;
    }
    // overdub: right at the playhead, before the events still to come at this
    // position, and counted as played (it already sounded live)
    const size_t i = std::min(phraseIdx_, phrase_.size());
    phrase_.insert(phrase_.begin() + (std::ptrdiff_t)i, pe);
    phraseIdx_ = i + 1;
}

void Engine::phraseCapture(const Event& e) {
    if (phraseState_ != PhraseState::Recording && phraseState_ != PhraseState::Overdub) return;
    switch (e.type) {
        case EventType::NoteOn: case EventType::NoteOff: case EventType::Controller: case EventType::PitchBend:
        case EventType::ModWheel: case EventType::ChannelPressure: case EventType::Sustain: break;
        default: return;  // per-note expression, clock, looper... are not recorded
    }
    PhraseEvent pe;
    pe.event = e;
    pe.pass = phrasePass_;
    pe.beat = phrasePos_;
    // own note ids per pass: a loop note never collides with the same key played live
    if (e.type == EventType::NoteOn || e.type == EventType::NoteOff)
        pe.event.noteId = e.noteId == 0 ? 0 : ((e.noteId & 0xFFFFFu) | ((uint32_t)(phrasePass_ & 0x3FF) << 20) | 0x40000000u);
    if (e.type == EventType::NoteOn && e.value > 0.0f) {
        if (recHeldCount_ < kPhraseHeld) recHeld_[recHeldCount_++] = {pe.event.noteId, e.key, e.channel};
    } else if (e.type == EventType::NoteOff) {
        for (int i = 0; i < recHeldCount_; ++i)
            if (recHeld_[i].id == pe.event.noteId && recHeld_[i].key == e.key) {
                recHeld_[i] = recHeld_[--recHeldCount_];
                break;
            }
    }
    phraseInsert(pe, phraseState_ == PhraseState::Overdub);
}

void Engine::phraseClosePass(double offBeat) {
    for (int i = 0; i < recHeldCount_; ++i) {
        PhraseEvent pe;
        pe.beat = offBeat;
        pe.pass = phrasePass_;
        pe.event = Event::noteOff(recHeld_[i].id, recHeld_[i].key);
        pe.event.channel = recHeld_[i].channel;
        if (phrase_.size() >= (size_t)kMaxPhraseEvents) break;
        // in order: after every event at or before offBeat
        auto it = std::upper_bound(phrase_.begin(), phrase_.end(), offBeat,
                                   [](double b, const PhraseEvent& x) { return b < x.beat; });
        const size_t at = (size_t)(it - phrase_.begin());
        phrase_.insert(it, pe);
        if (at < phraseIdx_) ++phraseIdx_;
    }
    recHeldCount_ = 0;
}

void Engine::phraseBlockStart() {
    // a replaced phrase (quantized): same position
    if (PhraseSwap* s = pendingPhrase_.load(std::memory_order_acquire)) {
        if (retiredPhrases_.push(s)) {
            pendingPhrase_.store(nullptr, std::memory_order_release);
            phraseSilence();
            phrase_.swap(s->events);  // s now holds the old events (freed by collectGarbage)
            if (s->length > 0.0) phraseLen_ = s->length;
            if (phraseLen_ > 0.0) phrasePos_ = std::fmod(phrasePos_, phraseLen_);
            phraseIdx_ = (size_t)(std::lower_bound(phrase_.begin(), phrase_.end(), phrasePos_,
                                                   [](const PhraseEvent& x, double b) { return x.beat < b; }) - phrase_.begin());
            if (phraseState_ == PhraseState::Empty && !phrase_.empty()) phraseState_ = PhraseState::Stopped;
        }
    }
    int cmd;
    while (phraseCmds_.pop(cmd)) {
        const int bars = phraseBars_.load(std::memory_order_relaxed);
        switch (cmd) {
            case CmdRecord:
                if (phraseState_ == PhraseState::Empty || (phraseState_ == PhraseState::Stopped && phrase_.empty())) {
                    phrase_.clear();
                    phraseLen_ = bars > 0 ? 4.0 * bars : 0.0;
                    phrasePos_ = 0.0;
                    phraseIdx_ = 0;
                    phrasePass_ = 1;
                    recHeldCount_ = 0;
                    phraseState_ = PhraseState::Recording;
                } else if (phraseState_ == PhraseState::Playing || phraseState_ == PhraseState::Stopped) {
                    if (phraseState_ == PhraseState::Stopped) {
                        phrasePos_ = 0.0;
                        phraseIdx_ = 0;
                    }
                    ++phrasePass_;
                    recHeldCount_ = 0;
                    phraseState_ = PhraseState::Overdub;
                } else if (phraseState_ == PhraseState::Overdub) {
                    phraseClosePass(phrasePos_);
                    phraseState_ = PhraseState::Playing;
                }
                break;
            case CmdPlay:
                if (phraseState_ == PhraseState::Recording) {
                    // the first pass ends: its length in whole bars (at least one)
                    if (phraseLen_ <= 0.0) phraseLen_ = std::max(4.0, std::round(phrasePos_ / 4.0) * 4.0);
                    phraseClosePass(phraseLen_ - 1e-4);
                    phrasePos_ = std::fmod(phrasePos_, phraseLen_);
                    phraseIdx_ = (size_t)(std::lower_bound(phrase_.begin(), phrase_.end(), phrasePos_,
                                                           [](const PhraseEvent& x, double b) { return x.beat < b; }) -
                                          phrase_.begin());
                    phraseState_ = phrase_.empty() ? PhraseState::Empty : PhraseState::Playing;
                } else if (phraseState_ == PhraseState::Stopped && !phrase_.empty()) {
                    phrasePos_ = 0.0;
                    phraseIdx_ = 0;
                    phraseState_ = PhraseState::Playing;
                }
                break;
            case CmdStop:
                if (phraseState_ == PhraseState::Recording) {
                    if (phraseLen_ <= 0.0) phraseLen_ = std::max(4.0, std::round(phrasePos_ / 4.0) * 4.0);
                    phraseClosePass(phraseLen_ - 1e-4);
                } else if (phraseState_ == PhraseState::Overdub) {
                    phraseClosePass(phrasePos_);
                }
                phraseSilence();
                phrasePos_ = 0.0;
                phraseIdx_ = 0;
                phraseState_ = phrase_.empty() ? PhraseState::Empty : PhraseState::Stopped;
                break;
            case CmdClear:
                phraseSilence();
                phrase_.clear();
                phraseLen_ = phrasePos_ = 0.0;
                phraseIdx_ = 0;
                phrasePass_ = 0;
                recHeldCount_ = 0;
                phraseState_ = PhraseState::Empty;
                break;
            case CmdUndo: {
                if (phrase_.empty()) break;
                uint16_t last = 0;
                for (const auto& pe : phrase_) last = std::max(last, pe.pass);
                phraseSilence();
                phrase_.erase(std::remove_if(phrase_.begin(), phrase_.end(), [last](const PhraseEvent& pe) { return pe.pass == last; }),
                              phrase_.end());
                recHeldCount_ = 0;
                phrasePass_ = last > 0 ? (uint16_t)(last - 1) : 0;
                phraseIdx_ = (size_t)(std::lower_bound(phrase_.begin(), phrase_.end(), phrasePos_,
                                                       [](const PhraseEvent& x, double b) { return x.beat < b; }) - phrase_.begin());
                if (phrase_.empty()) {
                    phraseLen_ = phrasePos_ = 0.0;
                    phraseState_ = PhraseState::Empty;
                } else if (phraseState_ == PhraseState::Overdub) {
                    phraseState_ = PhraseState::Playing;
                }
                break;
            }
            default: break;
        }
    }
    if (snapReq_.load(std::memory_order_acquire) && !snapReady_.load(std::memory_order_acquire)) {
        snapBuf_.clear();
        snapBuf_.insert(snapBuf_.end(), phrase_.begin(), phrase_.end());  // within the reserved capacity
        snapLen_ = phraseLen_;
        snapReq_.store(false, std::memory_order_relaxed);
        snapReady_.store(true, std::memory_order_release);
    }
    phraseStateA_.store((int)phraseState_, std::memory_order_relaxed);
    phraseLenA_.store(phraseLen_, std::memory_order_relaxed);
    phrasePosA_.store(phrasePos_, std::memory_order_relaxed);
    phraseCountA_.store((int)phrase_.size(), std::memory_order_relaxed);
}

void Engine::runPhrase(int frames) {
    const double db = (double)jobBpf_ * frames;
    if (phraseState_ == PhraseState::Recording) {
        phrasePos_ += db;
        if (phraseLen_ > 0.0 && phrasePos_ >= phraseLen_) {  // a set length: the first pass closes itself
            phraseClosePass(phraseLen_ - 1e-4);
            phrasePos_ = std::fmod(phrasePos_, phraseLen_);
            phraseIdx_ = 0;
            phraseState_ = PhraseState::Playing;
        }
        return;
    }
    if (phraseLen_ <= 0.0) return;
    double end = phrasePos_ + db;
    for (int wrap = 0; wrap < 4; ++wrap) {
        const double stop = std::min(end, phraseLen_);
        while (phraseIdx_ < phrase_.size() && phrase_[phraseIdx_].beat < stop) phraseDispatch(phrase_[phraseIdx_++].event);
        if (end < phraseLen_) break;
        end -= phraseLen_;  // the loop starts again
        phraseIdx_ = 0;
        if (end <= 0.0) break;
    }
    phrasePos_ = end < phraseLen_ ? end : std::fmod(end, phraseLen_);
}

}  // namespace hsynth
