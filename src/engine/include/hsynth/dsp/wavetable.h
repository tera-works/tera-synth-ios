// Wavetable synthesis: a table is a sequence of single-cycle frames; the
// oscillator scans through them (position) and reads band-limited copies so
// that no harmonic exceeds Nyquist at any pitch.
//
// Every frame is stored at kLevels resolutions: level k keeps the first
// 1024 >> k harmonics (built with an FFT). The oscillator picks the level from
// its phase increment and crossfades with the next duller level, so sweeping
// the pitch never steps or aliases.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "hsynth/common.h"

namespace hsynth {

struct Wavetable {
    static constexpr int kFrameSize = 2048;  // samples per single cycle (level 0 resolution)
    static constexpr int kLevels = 11;       // 1024, 512, ... 1 harmonics

    std::string name;
    std::string sourcePath;
    int frames = 0;
    int size[kLevels] = {};                 // samples per frame at each level (power of two)
    std::vector<float> data[kLevels];       // frames * (size + 1): one guard sample per frame

    const float* frame(int level, int f) const { return data[level].data() + (size_t)f * (size_t)(size[level] + 1); }
    size_t memoryBytes() const;

    // raw: frames * frameLen samples, frameLen a power of two. DC is removed.
    // perFrame: every frame peaks at 0.99 (built-ins: even loudness across the
    // sweep); otherwise one gain for the whole table (keeps imported levels).
    static std::unique_ptr<Wavetable> fromFrames(const std::vector<float>& raw, int frameLen, int frames, const std::string& name,
                                                 bool perFrame = false);
};

// Built-in tables (generated once, shared by all voices).
int builtinWavetableCount();
const std::vector<std::string>& builtinWavetableNames();
const Wavetable& builtinWavetable(int index);

// WAV wavetable: frame size from a Serum "clm " chunk ("<!>2048 ..."), else 2048
// if the length is a multiple of it, else the whole file is one cycle
// (resampled). Up to 256 frames.
std::unique_ptr<Wavetable> loadWavetableFile(const std::string& path, std::string& error);

// Band-limited wavetable oscillator with unison and phase warping.
enum class WtWarp : int { None = 0, Sync, Bend, Mirror, Squeeze, Count };

class WtOsc {
public:
    static constexpr int kMaxUnison = 7;
    void reset(Rng& rng, int unison);
    // Renders n samples into L / R (overwrites). pos 0..1 across the frames,
    // dt = cycles per sample of the centre voice. Returns true if the output is
    // stereo (unison spread).
    bool render(const Wavetable& wt, float pos, float dt, WtWarp warp, float warpAmt, int unison, float detuneCents,
                float spread, float* L, float* R, int n);

private:
    float phase_[kMaxUnison] = {};
};

}  // namespace hsynth
