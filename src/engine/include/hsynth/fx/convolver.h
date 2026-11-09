// Zero-latency partitioned convolution.
//
// The impulse response is split into
//   head    taps [0, 128)              direct-form FIR (no latency)
//   stage A taps [128, 4096)           uniform partitions of 128, FFT 256
//   stage B taps [4096, len)           uniform partitions of 2048, FFT 4096
// Stage B results are needed one 2048-block later than they could be
// computed, so its multiply-accumulate work is spread evenly over that block
// (no CPU spike every 2048 samples).
//
// Two stereo layouts, both evaluated with one complex FFT:
//   MonoIrStereoIn  : yL = xL * h,  yR = xR * h   (packs L + iR, h real)
//   StereoIrMonoIn  : yL = x * hL,  yR = x * hR   (packs h = hL + i hR)
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace hsynth {
namespace fx {

class Fft {
public:
    void init(int n);
    int size() const { return n_; }
    void forward(float* re, float* im) const { run(re, im, false); }
    void inverse(float* re, float* im) const { run(re, im, true); }  // scaled by 1/n

private:
    void run(float* re, float* im, bool inv) const;
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

struct ConvStageIr {
    int block = 0;      // partition size B
    int first = 1;      // first partition index (start tap = first * B)
    int parts = 0;
    std::vector<float> re, im;  // parts * 2B spectra
};

// Immutable, pre-transformed impulse response. Built off the audio thread.
struct ConvIr {
    enum Layout { MonoIrStereoIn, StereoIrMonoIn };
    Layout layout = MonoIrStereoIn;
    int length = 0;
    std::string name;
    std::string path;  // source file (for presets), empty for built-ins
    std::vector<float> headL, headR;  // headR used by StereoIrMonoIn
    ConvStageIr a, b;

    // hR may be null (mono IR). For StereoIrMonoIn a null hR duplicates hL.
    static std::unique_ptr<ConvIr> build(const float* hL, const float* hR, int len, Layout layout,
                                         int maxLen = 1 << 19);
};

class Convolver {
public:
    static constexpr int kHead = 128;
    static constexpr int kBlockA = 128;
    static constexpr int kBlockB = 2048;
    static constexpr int kSplitB = 4096;  // stage B starts here (2 * kBlockB)

    // Allocates state for IRs up to maxLen samples.
    void prepare(int maxLen);
    void reset();
    // Audio thread: switch IR (state is cleared). nullptr = silence.
    void setIr(const ConvIr* ir);
    const ConvIr* ir() const { return ir_; }
    // In-place; for StereoIrMonoIn the input is (L + R) / 2.
    void process(float* L, float* R, int n);

private:
    struct Stage {
        int B = 0, parts = 0, maxParts = 0, pos = 0;
        int fdlHead = 0, done = 0;
        std::vector<float> inRe, inIm;       // 2B input frame
        std::vector<float> fdlRe, fdlIm;     // maxParts * 2B spectra
        std::vector<float> accRe, accIm;     // 2B accumulator
        std::vector<float> outRe, outIm;     // B samples being played
        std::vector<float> nextRe, nextIm;   // B samples for the next block (stage B)
        Fft fft;
    };
    void initStage(Stage& s, int B, int maxParts);
    void clearStage(Stage& s);
    void blockDone(Stage& s, const ConvStageIr& h, bool distributed);
    void accumulate(Stage& s, const ConvStageIr& h, int from, int to);

    const ConvIr* ir_ = nullptr;
    Stage a_, b_;
    std::vector<float> histL_, histR_;  // mirrored head FIR history
    int histPos_ = 0;
};

}  // namespace fx
}  // namespace hsynth
