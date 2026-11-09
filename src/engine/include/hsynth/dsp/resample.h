// Band-limited sample playback kernels.
//
// PCM regions pitched up (more than one source sample per output sample) fold
// everything above the output Nyquist frequency back into the audio band with a
// plain cubic interpolator. For those cases PcmPlayer uses a 16-tap polyphase
// windowed-sinc whose cutoff follows the playback step, plus pre-decimated
// copies of the sample (Sample::mip) for more than an octave up.
#pragma once

#include <cstdint>
#include <vector>

namespace hsynth {
namespace resample {

constexpr int kTaps = 16;     // taps per output sample (offsets -7 .. +8 around the read position)
constexpr int kPhases = 64;   // fractional positions (linear interpolation between neighbours)
constexpr int kCutoffs = 9;   // cutoff steps from kMinCut to kMaxCut (fraction of the source Nyquist)
constexpr float kMinCut = 0.45f, kMaxCut = 0.9f;

// [cutoff][phase 0..kPhases][tap], every row normalised to unity DC gain.
const float* kernelTable();
inline const float* kernel(int cut, int phase) {
    return kernelTable() + ((size_t)cut * (kPhases + 1) + (size_t)phase) * kTaps;
}
// cutoff row for a playback step (> 1): the highest cutoff that keeps the
// output alias-free, rounded down to the grid
inline int cutoffIndex(double step) {
    const double fc = kMaxCut / (step < 1.0 ? 1.0 : step);
    const double x = (fc - kMinCut) / (kMaxCut - kMinCut) * (kCutoffs - 1);
    return x <= 0.0 ? 0 : x >= kCutoffs - 1 ? kCutoffs - 1 : (int)x;
}

// Half-rate copy (anti-aliased) of a channel: out has (in.size() + 1) / 2 samples.
void decimate2(const std::vector<float>& in, size_t frames, std::vector<float>& out);

}  // namespace resample
}  // namespace hsynth
