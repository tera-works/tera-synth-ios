// PCM sample data and key/velocity mapping.
//
// A SampleSet is built entirely on a loader (non-audio) thread: all files are
// decoded into memory before the set is handed to the engine, so the audio
// thread never performs disk I/O. Sets are immutable once published.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hsynth/dsp/wavetable.h"
#include "hsynth/streaming.h"

namespace hsynth {

struct Sample {
    std::string path;
    int channels = 1;
    double sampleRate = 48000.0;
    int64_t frames = 0;               // valid frames (buffers are padded)
    std::vector<float> data[2];       // channel buffers, kPadFrames of zeros appended
    int64_t loopStart = -1;           // from smpl chunk
    int64_t loopEnd = -1;             // exclusive
    int unityNote = -1;

    // Disk streaming (streaming.h): data[] holds only the first preloadFrames
    // (+ padding); the rest is read by the engine's DiskStreamer.
    bool streamed = false;
    int64_t preloadFrames = 0;
    StreamSource stream;
    DiskStreamer* streamer = nullptr;  // set by the engine when the set is installed
    // Loads the frames that are on disk only (the sample is no longer streamed).
    bool loadFully(std::string& error);

    static constexpr int kPadFrames = 4;
    const float* channel(int c) const { return data[c < channels ? c : 0].data(); }

    // Anti-aliased 1/2, 1/4, 1/8 rate copies for playback more than one
    // octave above the recorded pitch (built by buildMips() on the loader
    // thread, only for samples whose regions can be pitched that far up).
    static constexpr int kMaxMips = 3;
    int mipLevels = 0;
    std::vector<float> mip[kMaxMips][2];
    int64_t mipFrames[kMaxMips] = {};
    // level 0 = the sample itself
    const float* levelChannel(int level, int c) const {
        return level == 0 ? channel(c) : mip[level - 1][c < channels ? c : 0].data();
    }
    int64_t levelFrames(int level) const { return level == 0 ? frames : mipFrames[level - 1]; }
    void buildMips(int levels);
};

enum class LoopMode : uint8_t { NoLoop, OneShot, Continuous, Sustain };

struct RegionEnv {
    bool enabled = false;
    float delay = 0.0f, attack = 0.0f, hold = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f;
};

enum class RegionTrigger : uint8_t { Attack, Release };
// Per-region static filter (SFZ fil_type / cutoff / resonance).
enum class RegionFilter : uint8_t { None, Lp1, Hp1, Lp2, Hp2, Bp2, Br2 };

struct Region {
    const Sample* sample = nullptr;
    int lokey = 0, hikey = 127;
    int lovel = 1, hivel = 127;
    float lorand = 0.0f, hirand = 1.0f;
    int keycenter = 60;
    float pitchKeytrack = 100.0f;  // cents per key
    float tune = 0.0f;             // cents
    int transpose = 0;
    float volumeDb = 0.0f;
    float pan = 0.0f;              // -1..1
    float ampVeltrack = 100.0f;    // percent
    LoopMode loopMode = LoopMode::NoLoop;
    int64_t loopStart = -1, loopEnd = -1;  // exclusive end
    int64_t offset = 0, end = -1;          // end exclusive, -1 = sample end
    RegionEnv ampeg;
    float delay = 0.0f;              // seconds before the region starts (sfz delay)
    RegionTrigger trigger = RegionTrigger::Attack;
    float rtDecay = 0.0f;            // release trigger: dB attenuation per second the note was held
    int group = 0, offBy = 0;        // exclusive groups: a new region of group G cuts regions with off_by = G
    bool offFast = true;             // off_mode: fast (6 ms) or normal (region release)
    int seqLength = 1, seqPosition = 1;  // round robin: plays when (counter % length) == position - 1
    float loopCrossfade = 0.0f;      // seconds
    RegionFilter filter = RegionFilter::None;
    float cutoff = 0.0f;             // Hz
    float resonance = 0.0f;          // dB
    float filKeytrack = 0.0f;        // cents per key
    int filKeycenter = 60;
    float filVeltrack = 0.0f;        // cents at velocity 127
    float ampKeytrack = 0.0f;        // dB per key
    int ampKeycenter = 60;
    int swLast = -1;                 // keyswitch: region plays only while this switch key is active
    // crossfades (SFZ xfin_* / xfout_*): gain 0 -> 1 over the "in" range and
    // 1 -> 0 over the "out" range; equal-power (xf_*curve=power) or linear
    int xfinLoVel = 0, xfinHiVel = 0, xfoutLoVel = 127, xfoutHiVel = 127;
    int xfinLoKey = 0, xfinHiKey = 0, xfoutLoKey = 127, xfoutHiKey = 127;
    bool xfVelPower = true, xfKeyPower = true;
    // crossfade gain for a note (1 when no crossfade applies)
    float crossfadeGain(int key, int vel127) const;
};

// Region lookup context (per note).
struct RegionQuery {
    int key = 60;
    int vel127 = 100;
    float rand01 = 0.0f;
    RegionTrigger trigger = RegionTrigger::Attack;
    uint32_t seq = 0;                // round-robin counter of the layer
    int keyswitch = -1;              // active keyswitch, -1 = the set's default
};

struct SampleSet {
    std::string name;
    std::string sourcePath;
    std::vector<std::unique_ptr<Sample>> samples;
    std::vector<Region> regions;
    std::vector<std::string> warnings;
    std::unique_ptr<Wavetable> wavetable;  // set instead of samples for wavetable layers ("user" table)
    // keyswitch range (sw_lokey / sw_hikey, -1 = none) and default switch
    int swLo = -1, swHi = -1, swDefault = -1;
    bool hasReleaseRegions = false;
    int streamedSamples() const;
    int64_t diskBytes() const;  // bytes left on disk (streamed samples)
    bool isKeyswitch(int key) const { return swLo >= 0 && key >= swLo && key <= swHi; }
    size_t memoryBytes() const;

    // Builds the sample mip levels each sample needs for its regions' key
    // ranges (plus a bend headroom). Loader thread.
    void prepareMips();
    // Samples that cannot be streamed (looped, pitched up more than an octave
    // or started far into the file) are loaded fully. Loader thread, after
    // the regions are known and before prepareMips().
    void finalizeStreaming();

    // Returns number of matching regions written to out (at most max).
    int findRegions(const RegionQuery& q, const Region** out, int max) const;
    int findRegions(int key, int vel127, float rand01, const Region** out, int max) const {
        RegionQuery q;
        q.key = key;
        q.vel127 = vel127;
        q.rand01 = rand01;
        return findRegions(q, out, max);
    }
};

// Load a single WAV / FLAC as a set with one region spanning all keys.
std::unique_ptr<SampleSet> loadWavSampleSet(const std::string& path, std::string& error);

// Load an SFZ instrument. Unsupported opcodes are reported in warnings.
std::unique_ptr<SampleSet> loadSfzSampleSet(const std::string& path, std::string& error);

// Dispatch on file extension (.wav / .flac / .sfz).
std::unique_ptr<SampleSet> loadSampleSet(const std::string& path, std::string& error);

// Opcodes understood by the SFZ loader (for documentation / UI).
const std::vector<std::string>& supportedSfzOpcodes();

}  // namespace hsynth
