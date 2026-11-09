// A single polyphonic voice: up to kNumLayers sound generators (VA / FM / PCM)
// mixed into a stereo multimode filter and amp envelope. Each voice carries its
// own per-note expression state (pitch, pressure, timbre, X) so that every key
// can be modulated independently.
#pragma once

#include <cstdint>
#include <memory>

#include "hsynth/common.h"
#include "hsynth/dsp/envelope.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/dsp/lfo.h"
#include "hsynth/dsp/organ.h"
#include "hsynth/dsp/oscillators.h"
#include "hsynth/dsp/svf.h"
#include "hsynth/dsp/voice_fx.h"
#include "hsynth/fx/fx_params.h"
#include "hsynth/dsp/wavetable.h"
#include "hsynth/event.h"
#include "hsynth/params.h"
#include "hsynth/sample_set.h"

namespace hsynth {

// Shared, per-control-block state supplied by the engine.
struct VoiceContext {
    const float* params = nullptr;
    float sampleRate = 48000.0f;
    float pitchBendSemis = 0.0f;   // already scaled by bend range
    float pitchBendNorm = 0.0f;    // -1..1
    float modWheel = 0.0f;
    float chanPressure = 0.0f;
    float breath = 0.0f;
    float expression = 1.0f;
    float lfo2 = 0.0f;
    int quality = 1;               // Engine Quality (0 low, 1 normal, 2 high)
    float cc[4] = {};              // assignable controllers (ctl.cc1..4)
    float tempo = 120.0f;          // bpm (effect rack tempo / MIDI clock) for synced LFOs
    float masterTune = 0.0f;       // semitones (master.tune)
    double beat = 0.0;             // tempo position (beats) at the start of this control block (motion lanes)
    float pedal = 0.0f;            // sustain pedal 0..1 of the part (half pedal)
    float superKnob = 0.0f;        // perf.super (same for every part), mod source "super"
    const SampleSet* sampleSets[kNumLayers] = {};
    uint32_t seq[kNumLayers] = {};    // round-robin counters (SFZ seq_position)
    int keyswitch[kNumLayers] = {-1, -1, -1};  // active SFZ keyswitch per layer (-1 = default)
};

// Plays one SFZ region / WAV sample.
class PcmPlayer {
public:
    // gainDb: extra gain (release-trigger decay)
    void start(const Region* r, int key, float velocity, float startFrac, float velTrackScale, float sampleRate,
               float gainDb = 0.0f);
    void release();
    void stop() { finish(); }
    // Exclusive-group cut: fast fade (6 ms) or the region's normal release.
    void cut();
    // 6 ms fade regardless of the region (sample set swap); a player still in
    // its start delay stops at once (it is silent anyway).
    void fadeOut();
    bool active() const { return region_ != nullptr; }
    bool oneShot() const { return region_ && region_->loopMode == LoopMode::OneShot; }
    const Region* region() const { return region_; }
    // pitchSemis: extra transposition (bend, modulation, layer coarse/fine).
    // quality 0 = cubic only (cheap), >= 1 = band-limited when pitched up.
    void setPitch(float pitchSemis, float engineRate, int quality = 1);
    void render(float* L, float* R, int n, float gain);

private:
    void finish() {                                  // the player stops (releases its disk stream)
        region_ = nullptr;
        detachStream();
    }
    void detachStream() {
        if (stream_) streamer_->release(stream_);
        stream_ = nullptr;
    }
    // streamed samples (streaming.h): level 0, never looping
    const float* frames0(int c, int64_t first, int count) const;  // contiguous frames or nullptr
    float streamAt(int c, int64_t idx) const;
    StreamSlot* stream_ = nullptr;
    DiskStreamer* streamer_ = nullptr;
    mutable bool starved_ = false;                   // a needed frame had not arrived from disk
    void startFade(float seconds);                   // linear fade to silence, then the player stops
    float at(int c, int64_t idx) const;
    float atLevel(int c, int64_t j) const;           // sample j of the current mip level, loop-aware
    float sampleAt(int c, int64_t idx, float f) const;  // 4-point Hermite (level 0)
    float sincAt(int c, double pos) const;           // polyphase windowed sinc (current level / cutoff)
    void sincAt2(double pos, float& l, float& r) const;  // both channels, one kernel (SIMD)
    float value(int c, double pos) const {
        if (cut_ < 0) {
            const int64_t idx = (int64_t)pos;
            return sampleAt(c, idx, (float)(pos - (double)idx));
        }
        return sincAt(c, pos);
    }
    void value2(double pos, float& l, float& r) const {
        if (cut_ < 0) {
            const int64_t idx = (int64_t)pos;
            const float f = (float)(pos - (double)idx);
            l = sampleAt(0, idx, f);
            r = sampleAt(1, idx, f);
            return;
        }
        sincAt2(pos, l, r);
    }
    int cut_ = -1;     // sinc cutoff row, -1 = Hermite
    int level_ = 0;    // mip level read by the sinc path
    const Region* region_ = nullptr;
    double pos_ = 0.0;
    double step_ = 1.0;
    double baseRatio_ = 1.0;
    int64_t end_ = 0;
    int64_t delay_ = 0;       // frames until playback starts
    int64_t envDelay_ = 0;    // frames until the region envelope starts
    int64_t xfLen_ = 0;       // loop crossfade length (sample frames)
    bool released_ = false;
    bool looping_ = false;
    float gainL_ = 1.0f, gainR_ = 1.0f;
    float fade_ = 1.0f, fadeStep_ = 0.0f;
    float sr_ = 48000.0f;
    Adsr env_;
    bool useEnv_ = false;
    Svf filt_[2];
    float lp1_[2] = {}, lp1a_ = 0.0f;
};

class Voice {
public:
    static constexpr int kMaxRegionsPerLayer = 4;

    void prepare(float sampleRate);

    // glideFrom: pitch (key, may be fractional) to glide from, < 0 = no glide
    void start(const Event& e, const VoiceContext& ctx, uint64_t age, float random, float glideFrom = -1.0f);
    // Legato: move to a new note without retriggering (gliding if glide > 0).
    void legatoTo(const Event& e, bool glide);
    float currentPitch() const { return (float)key_ + glide_; }
    void release();
    void steal();            // fast fade-out
    void stopLayer(int l);   // immediately stop PCM playback of a layer (sample set swap)
    void fadeLayer(int l);   // fade PCM playback of a layer out within kSwapFadeSeconds (sample set swap)
    bool playsLayer(int l) const;  // PCM / release players of layer l still sounding
    // stop every player still reading `set` (it is about to be freed); true if one was
    int dropSet(const SampleSet* set);
    bool usesSet(const SampleSet* set) const;  // a player or a release region still reads it

    // A note that keeps the sound it started with after its part switched to
    // another program (song program changes): the old part parameters (the
    // engine owns the copy) and the voice's own sample sets. Cleared by start().
    const float* paramsOverride = nullptr;
    const float* params(const VoiceContext& ctx) const { return paramsOverride ? paramsOverride : ctx.params; }
    static constexpr float kSwapFadeSeconds = 0.006f;
    // SFZ exclusive groups: groups started by this voice (returns count), and
    // cutting this voice's regions with off_by == group.
    int startedGroups(int* out, int max) const;
    void choke(int group);
    void hardStop() { deactivate(); }
    void applyExpression(EventType type, float value);
    void setReleaseVelocity(float v) { releaseVel_ = v; }  // before release(): "relvelocity" source

    // Render n <= kControlBlock samples, adding into L/R.
    void render(float* L, float* R, int n, const VoiceContext& ctx);

    bool active() const { return active_; }
    bool released() const { return released_; }
    bool stolen() const { return stolen_; }
    uint32_t noteId() const { return noteId_; }
    int key() const { return key_; }
    uint64_t age() const { return age_; }
    float level() const { return ampEnv_.level(); }
    bool sustained = false;  // note-off arrived while sustain pedal / hold active
    bool hold = false;       // per-note hard-press hold (NoteHold event)
    int part = 0;            // owning part (multi-timbral), set by the engine
    // sample-accurate scheduling inside one process() block (engine): the
    // voice renders from startDelay frames on and is released at releaseAt
    int startDelay = 0;
    int releaseAt = -1;
    int matchKey = 60;       // key as received (before part transpose), for key-matched events
    float unisonDetune = 0;  // semitones (set by the engine before start)
    float unisonPan = 0;     // -1..1

private:
    void deactivate();       // voice ends: every PCM player stops (disk streams are released)
    void stopPlayers();
    static constexpr int kMaxReleaseRegions = 4;
    struct Layer {
        LayerType type = LayerType::Off;
        VaOsc osc1, osc2, sub;
        VaOsc ss1[6], ss2[6];        // supersaw side oscillators
        OrganVoice organ;
        FmVoice fm;
        WtOsc wt;
        PcmPlayer pcm[kMaxRegionsPerLayer];
        int pcmCount = 0;
        // granular: grains read from the layer's sample (the region for the key)
        struct Grain {
            double pos = 0.0, step = 1.0;
            float age = 0.0f, len = 1.0f, gl = 0.0f, gr = 0.0f;
            bool on = false;
        };
        static constexpr int kGrains = 24;
        Grain grains[kGrains];
        const Region* grRegion = nullptr;
        double grBaseRatio = 1.0;    // pitch of the region at the voice's key
        float grTimer = 0.0f;        // samples to the next grain
        double grScan = 0.0;         // position moved by scanning (fraction of the sample)
        float dt1 = 0, dt2 = 0;      // current oscillator increments (ramped)
        float gain = 0;              // smoothed layer gain
        bool finished = false;
    };
    void startReleaseRegions();
    // SFZ release-trigger regions: played after note-off, outside the amp
    // envelope / filter (they carry their own region envelope).
    PcmPlayer relPcm_[kMaxReleaseRegions];
    int relLayer_[kMaxReleaseRegions] = {};
    bool relActive_ = false;
    bool mainDone_ = false;  // layers / amp envelope finished, only release regions left
    const SampleSet* sets_[kNumLayers] = {};
    uint32_t seq_[kNumLayers] = {};
    int keyswitch_[kNumLayers] = {-1, -1, -1};

    float modSource(ModSource s, const VoiceContext& ctx) const;

    float sampleRate_ = 48000.0f;
    bool active_ = false, released_ = false, stolen_ = false;
    uint32_t noteId_ = 0;
    int key_ = 60;
    int pcmKey_ = 60;        // key the PCM regions were started for (legato keeps the samples)
    float glide_ = 0.0f;     // portamento offset in semitones, decays to 0
    float velocity_ = 1.0f;
    float random_ = 0.0f;
    uint64_t age_ = 0;
    float timeSinceOn_ = 0.0f;

    // per-note expression (targets and smoothed values)
    float notePitchT_ = 0, notePressureT_ = 0, noteTimbreT_ = 0, noteXT_ = 0;
    float notePitch_ = 0, notePressure_ = 0, noteTimbre_ = 0, noteX_ = 0;
    bool firstBlock_ = true;
    bool stereo_ = false;

    Adsr ampEnv_, env2_, env3_;
    Lfo lfo1_, lfo3_;
    float lfo1Value_ = 0.0f, lfo3Value_ = 0.0f;
    float lfo1Depth_ = 1.0f, lfo3Depth_ = 1.0f;   // lfo1.depth / lfo3.depth destinations
    float env2Scale_ = 1.0f, env3Scale_ = 1.0f;   // env2.time / env3.time (applied from the next block)
    float releaseVel_ = 0.5f;
    float msegT_ = 0.0f, msegValue_ = 0.0f;  // multi-segment envelope position / value
    float dampGain_ = 1.0f;                   // half pedal: sustained notes fade
    void advanceMseg(const float* p, float dtSec, float tempo);
    float motionValue(const VoiceContext& ctx, int lane) const;
    Rng rng_;
    Rng rng3_{0x5EEDu};  // LFO3 sample & hold (separate stream: the main one feeds noise / phases)
    VoiceFilter filterL_, filterR_;
    VoiceFilter filter2L_, filter2R_;   // second filter (filter2.*)
    std::unique_ptr<float[]> combMem_;  // comb filter delay lines (L, R, filter 2 L, R), allocated in prepare()
    VoiceFx vfx_;
    float ampGain_ = 0.0f;           // smoothed
    float panL_ = 1.0f, panR_ = 1.0f;
    Layer layers_[kNumLayers];
};

}  // namespace hsynth
