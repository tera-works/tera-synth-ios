// Effect rack parameter layout.
//
// The rack has kFxSlots serial slots; each slot names one effect block (or
// "none"). Every block owns a contiguous run of parameters in the global
// parameter array (P::fxp(block, local)), whether or not it is placed in a
// slot, so presets store each block's settings by name and moving a block to
// another slot keeps them.
#pragma once

namespace hsynth {
namespace fx {

enum Block : int {
    None = 0,
    // dynamics
    Gate, Comp, MbComp, Transient, Limiter,
    // drive / tone
    Drive, Amp, Cab, Crush, Lofi, Exciter,
    // eq / filter
    Peq, Geq, Filter, Wah,
    // modulation
    Chorus, Flanger, Phaser, Tremolo, Vibe, Rotary, RingMod,
    // pitch
    Pitch, Octaver,
    // time / space
    Delay, Reverb, Convo,
    // rhythm / utility
    Slicer, Stutter, Stereo, Vocoder,
    // v0.6
    DeEsser, Isolator, StepFilter, MultiTap, Doubler, Granular, TapeStop, Pump, Resonator, Swell, Talkbox, GatedVerb,
    // v0.7 (acoustic modelling)
    Sympathetic, Scanner,
    BlockCount
};

constexpr int kFxSlots = 12;
constexpr int kFxModSlots = 6;

// Local parameter indices. Every block starts with On.
namespace gate { enum { On, Thresh, Range, Attack, Hold, Release, Count }; }
namespace comp { enum { On, Thresh, Ratio, Attack, Release, Knee, Makeup, AutoMakeup, Detector, ScHpf, Mix, Count }; }
namespace mbcomp {
enum { On, XLow, XHigh, LowThresh, LowRatio, LowGain, MidThresh, MidRatio, MidGain, HighThresh, HighRatio, HighGain,
       Attack, Release, Mix, Count };
}
namespace transient { enum { On, Attack, Sustain, Speed, Output, Count }; }
namespace limiter { enum { On, Input, Ceiling, Release, Lookahead, Count }; }
namespace drive { enum { On, Type, Amount, Tight, Bias, Tone, Level, Mix, Count }; }
namespace amp { enum { On, Model, Gain, Bass, Middle, Treble, Presence, Depth, Level, Count }; }
namespace cab { enum { On, Model, Mic, LowCut, HighCut, Level, Mix, Count }; }
namespace crush { enum { On, Bits, Rate, Jitter, Tone, Mix, Level, Count }; }
namespace lofi { enum { On, Wow, Flutter, Saturation, Hiss, Crackle, Bandwidth, LowCut, Mix, Count }; }
namespace exciter { enum { On, Freq, Drive, Amount, BassAmount, BassFreq, Count }; }
namespace peq {
enum { On, LowCut, LsFreq, LsGain, P1Freq, P1Gain, P1Q, P2Freq, P2Gain, P2Q, P3Freq, P3Gain, P3Q, HsFreq, HsGain, HighCut,
       Output, Count };
}
namespace geq { enum { On, B31, B63, B125, B250, B500, B1k, B2k, B4k, B8k, B16k, Level, Count }; }
namespace filter {
enum { On, Type, Cutoff, Reso, Drive, LfoWave, LfoRate, LfoSync, LfoDepth, EnvAmount, EnvAttack, EnvRelease, Phase, Mix,
       Count };
}
namespace wah { enum { On, Mode, Position, Sens, Rate, Reso, Mix, Count }; }
namespace chorus { enum { On, Mode, Voices, Rate, Depth, Delay, Feedback, Width, Mix, Count }; }
namespace flanger { enum { On, Rate, Sync, Depth, Manual, Feedback, ThruZero, Phase, Mix, Count }; }
namespace phaser { enum { On, Stages, Rate, Sync, Depth, Center, Feedback, Phase, Mix, Count }; }
namespace tremolo { enum { On, Mode, Wave, Rate, Sync, Depth, Phase, Count }; }
namespace vibe { enum { On, Mode, Rate, Sync, Depth, Mix, Count }; }
namespace rotary { enum { On, Fast, Accel, Balance, Drive, Spread, Mix, Count }; }
namespace ringmod { enum { On, Mode, Freq, KeyTrack, LfoRate, LfoDepth, Spread, Mix, Count }; }
namespace pitch { enum { On, Semi1, Fine1, Pan1, Semi2, Fine2, Level2, Pan2, Window, Feedback, Mix, Count }; }
namespace octaver { enum { On, Sub1, Sub2, Up, Dry, Tone, Count }; }
namespace delay {
enum { On, Type, Mode, Time, Sync, Ratio, Feedback, LowCut, HighCut, ModDepth, ModRate, Ducking, Width, Mix, Count };
}
namespace reverb {
enum { On, Type, Size, Decay, PreDelay, Damp, LowCut, Diffusion, Modulation, Width, Early, Shimmer, Freeze, Mix,
       Count };
}
namespace convo { enum { On, Ir, PreDelay, LowCut, HighCut, Width, Mix, Count }; }
namespace slicer { enum { On, Division, Pattern, Duty, Depth, Smooth, Swing, Count }; }
namespace stutter { enum { On, Active, Division, Reverse, Gate, Mix, Count }; }
namespace stereo { enum { On, Width, Pan, Haas, MonoBass, Output, Count }; }
namespace vocoder {
enum { On, Bands, LowFreq, HighFreq, Q, Attack, Release, Formant, Unvoiced, InputGain, GateThresh, Mix, Count };
}

namespace deesser { enum { On, Freq, Thresh, Range, Listen, Count }; }
namespace isolator { enum { On, Low, Mid, High, XLow, XHigh, Count }; }
namespace stepfilter {
enum { On, Type, Division, Steps, Base, Depth, Reso, Glide, Mix, Step1, Count = Step1 + 16 };
}
namespace multitap {
enum { On, Sync, Time, Feedback, Tone, Mix, Tap1Time, Tap1Level, Tap1Pan, Count = Tap1Time + 4 * 3 };
}
namespace doubler { enum { On, Detune, Delay, Width, Mix, Count }; }
namespace granular { enum { On, Size, Density, Pitch, Spray, Spread, Reverse, Feedback, Freeze, Mix, Count }; }
namespace tapestop { enum { On, Stop, Time, Curve, Count }; }
namespace pump { enum { On, Division, Depth, Release, Curve, Count }; }
namespace resonator { enum { On, Chord, Root, KeyFollow, Decay, Tone, Mix, Count }; }
namespace swell { enum { On, Attack, Thresh, Curve, Count }; }
namespace talkbox { enum { On, Mode, Vowel, Rate, Sync, Depth, Sens, Reso, Mix, Count }; }
namespace gatedverb { enum { On, Mode, Size, PreDelay, Density, Damp, Width, Mix, Count }; }
namespace sympathetic { enum { On, Amount, Decay, PedalFollow, Tone, Count }; }
namespace scanner { enum { On, Mode, Rate, Count }; }

constexpr int kBlockParams[BlockCount] = {
    0,
    gate::Count, comp::Count, mbcomp::Count, transient::Count, limiter::Count,
    drive::Count, amp::Count, cab::Count, crush::Count, lofi::Count, exciter::Count,
    peq::Count, geq::Count, filter::Count, wah::Count,
    chorus::Count, flanger::Count, phaser::Count, tremolo::Count, vibe::Count, rotary::Count, ringmod::Count,
    pitch::Count, octaver::Count,
    delay::Count, reverb::Count, convo::Count,
    slicer::Count, stutter::Count, stereo::Count, vocoder::Count,
    deesser::Count, isolator::Count, stepfilter::Count, multitap::Count, doubler::Count, granular::Count,
    tapestop::Count, pump::Count, resonator::Count, swell::Count, talkbox::Count, gatedverb::Count,
    sympathetic::Count, scanner::Count,
};

constexpr int blockOffset(int b) {
    int s = 0;
    for (int i = 0; i < b; ++i) s += kBlockParams[i];
    return s;
}
constexpr int kBlockParamTotal = blockOffset(BlockCount);

// Part insert effects (multi-timbral parts / single patch): kInsertSlots
// serial slots per part choosing from this subset of light-weight blocks.
// Each part has its own copy of these blocks' parameters ("ins.<key>.<param>").
constexpr int kInsertSlots = 3;
constexpr int kInsertBlockList[] = {Comp, Drive, Crush, Peq, Filter, Wah, Chorus, Flanger, Phaser, Tremolo, RingMod, Stereo,
                                    DeEsser, Isolator, StepFilter, Pump, Talkbox, Doubler};
constexpr int kNumInsertBlocks = (int)(sizeof(kInsertBlockList) / sizeof(kInsertBlockList[0]));
constexpr int insertOffset(int i) {
    int s = 0;
    for (int k = 0; k < i; ++k) s += kBlockParams[kInsertBlockList[k]];
    return s;
}
constexpr int kInsertParamTotal = insertOffset(kNumInsertBlocks);
// index into kInsertBlockList, or -1
constexpr int insertIndex(int block) {
    for (int k = 0; k < kNumInsertBlocks; ++k)
        if (kInsertBlockList[k] == block) return k;
    return -1;
}

// Enumerations shared by several blocks.
enum class SyncDiv : int {
    Off = 0, D1_1, D1_2, D1_2D, D1_2T, D1_4, D1_4D, D1_4T, D1_8, D1_8D, D1_8T, D1_16, D1_16D, D1_16T, D1_32, Count
};
// Length of a sync division in beats (quarter notes). Off returns 0.
inline float syncBeats(int div) {
    static const float kBeats[] = {0.0f, 4.0f, 2.0f, 3.0f, 4.0f / 3.0f, 1.0f, 1.5f, 2.0f / 3.0f, 0.5f, 0.75f,
                                   1.0f / 3.0f, 0.25f, 0.375f, 1.0f / 6.0f, 0.125f};
    return div > 0 && div < (int)SyncDiv::Count ? kBeats[div] : 0.0f;
}

enum class FxWave : int { Sine = 0, Triangle, SawUp, SawDown, Square, SampleHold, Smooth, Count };

// Global sources for the FX modulation slots (effects are shared by all voices).
enum class FxModSource : int {
    None = 0, ModWheel, Lfo2, ChannelPressure, Expression, Breath, PitchBend, Velocity, LastNote, Envelope, NotesHeld,
    Macro1, Macro2, Count
};

// Stable short key ("reverb") used in parameter names / slot options, and a
// display title ("Reverb") used as the UI group "FX / <title>".
const char* blockKey(int block);
const char* blockTitle(int block);

}  // namespace fx
}  // namespace hsynth
