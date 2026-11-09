// Effect block classes (internal to the engine; hosts use fx/rack.h).
#pragma once

#include <vector>

#include "hsynth/dsp/svf.h"
#include "hsynth/fx/block.h"
#include "hsynth/fx/convolver.h"

namespace hsynth {
namespace fx {

#define HS_FX_BLOCK_API                                           \
    void prepare(float sr) override;                              \
    void reset() override;                                        \
    void update(const float* p, const FxContext& c) override;     \
    void process(float* L, float* R, int n, const FxContext& c) override;

// v0.6 blocks (fx_extra.cpp); nullptr for other types.
std::unique_ptr<FxBlock> createExtraBlock(int type);

// Shared one-knob mix with click-free ramp.
struct MixRamp {
    Smoother s;
    float target = 1.0f;
    void prepare(float sr, float v) { s.setTime(0.02f, sr); s.reset(v); target = v; }
    float next() { return s.process(target); }
};

// ======================================================== dynamics

class GateFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, thrOpen_ = 0, thrClose_ = 0, floor_ = 0, att_ = 0, rel_ = 0, gain_ = 0;
    int hold_ = 0, holdLeft_ = 0;
    bool open_ = false;
    EnvFollower det_;
};

struct CompCore {
    float thresh = -20, ratio = 4, knee = 6, att = 0, rel = 0, grDb = 0;
    float computeGainDb(float levelDb) const;
    float process(float levelDb);
};

class CompFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, makeup_ = 1, rms_ = 0, rmsCoef_ = 0, mix_ = 1;
    bool rmsMode_ = true, useHpf_ = false;
    CompCore core_;
    StereoBiquad hpf_;
    Smoother mixS_;
};

class MbCompFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, gain_[3] = {1, 1, 1}, mix_ = 1;
    StereoBiquad lp1_[2], hp1_[2], lp2_[2], hp2_[2], ap_;
    CompCore core_[3];
};

class TransientFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, attack_ = 0, sustain_ = 0, out_ = 1, gainDb_ = 0, smooth_ = 0;
    EnvFollower fast_, slow_;
};

class LimiterFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, inGain_ = 1, ceil_ = 1, att_ = 0, rel_ = 0, gain_ = 1;
    int la_ = 1, winPos_ = 0;
    DelayLine dl_, dr_;
    std::vector<float> win_;
};

// ======================================================== drive / tone

class DriveFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float shape(float x) const;
    float sr_ = 48000, pre_ = 1, post_ = 1, bias_ = 0, biasOut_ = 0;
    int type_ = 1, os_ = 2;
    StereoBiquad tight_, emph_, tone_, fizz_;
    bool useEmph_ = false;
    Oversampler osL_, osR_;
    DcBlock dcL_, dcR_;
    DelayLine dryL_, dryR_;
    MixRamp mixR_;
};

class AmpFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    struct Stage { float hpA = 0, hpX = 0, hpY = 0, lpA = 1, lpY = 0; };
    float preamp(float x, Stage* st);
    void configure(int os);
    float sr_ = 48000, stageGain_ = 1, bias_ = 0.15f, outGain_ = 1, powerDrive_ = 1;
    int model_ = 1, stages_ = 2, os_ = 2, configuredOs_ = 0;
    float p_[16] = {};
    Stage stL_[4], stR_[4];
    StereoBiquad inHp_, bright_, bass_, mid_, treble_, presence_, depth_, outLp_;
    Oversampler osL_, osR_;
    DcBlock dcL_, dcR_;
};

class CabFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    void setUserIr(const ConvIr* ir) override { userIr_ = ir; }
private:
    float sr_ = 48000, level_ = 1, mix_ = 1;
    int model_ = -1;
    float mic_ = -1;
    StereoBiquad bq_[6], lowCut_, highCut_;
    const ConvIr* userIr_ = nullptr;
    bool useIr_ = false, useLow_ = true, useHigh_ = true;
    Convolver conv_;
    std::vector<float> wetL_, wetR_;
};

class CrushFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, levels_ = 128, step_ = 0.25f, jitter_ = 0, phase_ = 1, holdL_ = 0, holdR_ = 0, mix_ = 1, out_ = 1;
    StereoBiquad tone_;
    bool useTone_ = true;
    Rng rng_{0x9E3779B9u};
};

class LofiFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, wow_ = 0, flutter_ = 0, sat_ = 0, hiss_ = 0, crackle_ = 0, mix_ = 1;
    float wowPh_ = 0, flutPh_ = 0, wowTarget_ = 0, wowVal_ = 0, crackEnv_ = 0;
    DelayLine dl_, dr_;
    StereoBiquad band_, band2_, low_;
    OnePole hissLp_, crackLp_;
    Rng rng_{0x2545F491u};
};

class ExciterFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, drive_ = 1, amount_ = 0, bass_ = 0;
    StereoBiquad hp_, hp2_, bassLp_, bassBp_;
};

// ======================================================== eq / filter

class PeqFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, out_ = 1;
    StereoBiquad bq_[7];
    bool active_[7] = {};
    float last_[17] = {};
    bool init_ = false;
};

class GeqFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, level_ = 1;
    StereoBiquad bq_[10];
    int active_[10] = {};
    int numActive_ = 0;
    float last_[10] = {};
    bool init_ = false;
};

class FilterFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    void retrigger() override { lfo_.reset(); }
private:
    void setCoefs(float cutL, float cutR);
    float sr_ = 48000, cutoff_ = 1000, reso_ = 0, drive_ = 0, lfoDepth_ = 0, envAmt_ = 0, phase_ = 0;
    int type_ = 1, wave_ = 0, sync_ = 0;
    FxLfo lfo_;
    EnvFollower env_;
    Svf l1_, l2_, r1_, r2_;
    Svf fl_[3], fr_[3];
    Rng rng_{0x1234567u};
    int ctl_ = 0;
    MixRamp mixR_;
};

class WahFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, pos_ = 0.5f, sens_ = 0.5f, reso_ = 0.5f, mix_ = 1, posS_ = 0.5f;
    int mode_ = 1, ctl_ = 0;
    FxLfo lfo_;
    EnvFollower env_;
    Svf l_, r_;
    Rng rng_{0x777u};
};

// ======================================================== modulation

class ChorusFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, rate_ = 0.6f, depth_ = 0.5f, delay_ = 12, fb_ = 0, width_ = 1;
    int mode_ = 0, voices_ = 2;
    FxLfo lfo_, lfo2_;
    DelayLine dl_, dr_;
    float fbL_ = 0, fbR_ = 0;
    StereoBiquad bbd_;
    MixRamp mixR_;
    Rng rng_;
};

class FlangerFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, depth_ = 0.7f, manual_ = 2, fb_ = 0.5f, phase_ = 0.25f, fbL_ = 0, fbR_ = 0;
    int sync_ = 0;
    bool tz_ = false;
    FxLfo lfo_;
    DelayLine dl_, dr_;
    MixRamp mixR_;
    Rng rng_;
};

class PhaserFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, depth_ = 0.7f, center_ = 700, fb_ = 0.5f, phase_ = 0.25f, fbL_ = 0, fbR_ = 0;
    float aL_ = 0, aR_ = 0;
    int stages_ = 4, sync_ = 0, ctl_ = 0;
    float zl_[12] = {}, zr_[12] = {};
    FxLfo lfo_;
    MixRamp mixR_;
    Rng rng_;
};

class TremoloFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    void retrigger() override { lfo_.reset(); }
private:
    float sr_ = 48000, depth_ = 0.6f, phase_ = 0, gl_ = 1, gr_ = 1, smooth_ = 0;
    int mode_ = 0, wave_ = 0, sync_ = 0;
    FxLfo lfo_;
    StereoBiquad lp_, hp_;
    Rng rng_;
};

class VibeFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, depth_ = 0.6f;
    int mode_ = 0, sync_ = 0, ctl_ = 0;
    float zl_[4] = {}, zr_[4] = {}, a_[4] = {};
    FxLfo lfo_;
    MixRamp mixR_;
    Rng rng_;
};

class RotaryFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, hornRate_ = 0.8f, drumRate_ = 0.67f, hornPh_ = 0, drumPh_ = 0;
    float accel_ = 1, balance_ = 0.6f, drive_ = 0.2f, spread_ = 0.8f;
    bool fast_ = false;
    StereoBiquad split_, splitHp_;
    DelayLine hornL_, hornR_;
    MixRamp mixR_;
};

class RingModFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    struct Hilbert {
        float a[8] = {};
        float x1[8] = {}, x2[8] = {}, y1[8] = {}, y2[8] = {};
        float delayed = 0;
        void reset();
        void process(float x, float& re, float& im);
    };
    float sr_ = 48000, freq_ = 220, lfoDepth_ = 0, spread_ = 0, phL_ = 0, phR_ = 0.0f;
    int mode_ = 0;
    bool key_ = false;
    FxLfo lfo_;
    Hilbert hl_, hr_;
    MixRamp mixR_;
    Rng rng_;
};

// ======================================================== pitch

// Two-tap rotating-delay pitch shifter (one voice).
struct GrainShifter {
    DelayLine line;
    float phase = 0, window = 2000;
    void allocate(int maxWindow) { line.allocate(maxWindow + 16); }
    void clear() { line.clear(); phase = 0; }
    float process(float x, float ratio);
};

class PitchFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, ratio1_ = 2, ratio2_ = 0.5f, level2_ = 0, fb_ = 0, fb1_ = 0;
    float pan1L_ = 0.7f, pan1R_ = 0.7f, pan2L_ = 0.7f, pan2R_ = 0.7f;
    GrainShifter v1_, v2_;
    MixRamp mixR_;
};

class OctaverFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, sub1_ = 0, sub2_ = 0, up_ = 0, dry_ = 1;
    bool high_ = false;
    float ff1_ = 1, ff2_ = 1;
    StereoBiquad track_;  // only .l used (mono tracker)
    Biquad track2_;
    Biquad subLp_, subLp2_;
    OnePole upHp_;
    EnvFollower env_;
};

// ======================================================== time / space

class DelayFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    bool spillover() const override { return true; }
    bool tailActive() const override { return tail_ > 1e-6f; }
private:
    float sr_ = 48000, timeL_ = 0.35f, timeR_ = 0.35f, fb_ = 0.35f, mix_ = 0.3f, duck_ = 0, width_ = 1;
    float modDepth_ = 0, tail_ = 0;
    int type_ = 0, mode_ = 1;
    bool on_ = true, timeInit_ = false;
    DelayLine dl_, dr_;
    Smoother tL_, tR_, feed_, dryS_, wetS_;
    StereoBiquad hc_, lc_;
    FxLfo lfo_;
    float flutPh_ = 0;
    EnvFollower duckEnv_;
    float duckG_ = 1;
    // reverse
    float revPos_ = 0;
    // diffuse
    DelayLine apL_[4], apR_[4];
    int apLen_[4] = {};
    Rng rng_;
};

class ReverbFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    bool spillover() const override { return true; }
    bool tailActive() const override { return tail_ > 1e-6f; }
private:
    static constexpr int kLines = 8;
    float sr_ = 48000, mix_ = 0.15f, early_ = 0.3f, width_ = 1, shimmer_ = 0, tail_ = 0, diff_ = 0.7f;
    int type_ = 1;
    bool on_ = true, freeze_ = false;
    float lineLen_[kLines] = {}, g_[kLines] = {}, dampA_ = 0.5f, modDepth_ = 0;
    float lp_[kLines] = {}, modPh_[kLines] = {}, modInc_[kLines] = {};
    DelayLine line_[kLines], ap_[kLines];
    int apLen_[kLines] = {};
    DelayLine pre_, inAp_[4], erL_, erR_;
    int inApLen_[4] = {};
    float preDelay_ = 0;
    float erTapL_[8] = {}, erTapR_[8] = {}, erGain_[8] = {};
    StereoBiquad lowCut_;
    Smoother feed_, dryS_, wetS_;
    GrainShifter shimL_;
    float shimFb_ = 0;
    // spring
    DelayLine springL_, springR_;
    float springLen_ = 0, springG_ = 0;
    float sapL_[16] = {}, sapR_[16] = {};
    OnePole springLpL_, springLpR_;
};

class ConvoFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    bool spillover() const override { return true; }
    bool tailActive() const override { return tail_ > 1e-6f; }
    void setUserIr(const ConvIr* ir) override { user_ = ir; }
private:
    float sr_ = 48000, mix_ = 0.3f, width_ = 1, tail_ = 0, preDelay_ = 0;
    int ir_ = 1;
    bool on_ = true;
    std::unique_ptr<ConvIr> builtIn_[3];
    const ConvIr* user_ = nullptr;
    Convolver conv_;
    DelayLine preL_, preR_;
    StereoBiquad lc_, hc_;
    bool useLc_ = false, useHc_ = false;
    Smoother feed_, dryS_, wetS_;
    std::vector<float> wetL_, wetR_;
};

class SlicerFx : public FxBlock {
public:
    HS_FX_BLOCK_API
    void retrigger() override { lastStep_ = -1; }
private:
    float sr_ = 48000, stepBeats_ = 0.25f, duty_ = 0.6f, depth_ = 1, swing_ = 0, gain_ = 1, att_ = 0, rel_ = 0;
    int pattern_ = 0, lastStep_ = -1;
    bool stepOn_ = true;
    Rng rng_{0xBEEFu};
};

class StutterFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, mix_ = 1, gate_ = 1, fade_ = 0;
    float divBeats_ = 0.25f;
    bool active_ = false, reverse_ = false, capturing_ = false;
    std::vector<float> bufL_, bufR_;
    int write_ = 0, loopStart_ = 0, loopLen_ = 1, loopPos_ = 0;
};

class StereoFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    float sr_ = 48000, width_ = 1, panL_ = 1, panR_ = 1, haas_ = 0, out_ = 1;
    bool monoBass_ = false;
    StereoBiquad sideHp_;
    Biquad sideHp2_;
    DelayLine dr_;
};

class VocoderFx : public FxBlock {
public:
    HS_FX_BLOCK_API
private:
    static constexpr int kMaxBands = 32;
    float sr_ = 48000, inGain_ = 1, gateThr_ = 0, unvoiced_ = 0, makeup_ = 1;
    int bands_ = 16;
    Biquad ana_[kMaxBands][2];
    Biquad synL_[kMaxBands][2], synR_[kMaxBands][2];
    EnvFollower env_[kMaxBands];
    EnvFollower gateEnv_, hfEnv_;
    Biquad hfHp_, noiseHp_;
    float gateG_ = 0;
    Rng rng_{0xACE1u};
    MixRamp mixR_;
    float last_[16] = {};
    bool init_ = false;
};

#undef HS_FX_BLOCK_API

}  // namespace fx
}  // namespace hsynth
