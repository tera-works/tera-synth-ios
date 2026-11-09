// Parameter layout and metadata.
//
// Every editable sound parameter is a float in one flat array. The array index
// is the parameter id; metadata (name, range, enum labels, group) is available
// at runtime so hosts can build generic editors and presets can be stored by
// name (robust against future re-ordering of ids).
#pragma once

#include <string>
#include <vector>

#include "hsynth/fx/fx_params.h"

namespace hsynth {

enum class LayerType : int { Off = 0, VA, FM, PCM, WT, Organ, Granular, Count };
enum class VaWave : int { Saw = 0, Square, Pulse, Triangle, Sine, SuperSaw, Count };
enum class FilterType : int { Off = 0, LP12, LP24, BP, HP, Notch, Ladder, CombPlus, CombMinus, Formant, Count };
// Voice FX: per-voice insert after the filter, before the amp envelope.
enum class VfxType : int { Off = 0, Saturate, Fold, Crush, Ring, Count };
enum class VoiceMode : int { Poly = 0, Mono, Legato, Count };
enum class ArpMode : int { Up = 0, Down, UpDown, Random, Order, Chord, Count };
enum class LfoWave : int { Sine = 0, Triangle, Saw, Square, SampleHold, Count };

enum class ModSource : int {
    None = 0,
    Velocity,
    Key,            // (note - 60) / 60
    AmpEnv,
    Env2,
    Lfo1,           // per-voice LFO
    Lfo2,           // global LFO
    ModWheel,
    PitchBend,
    ChannelPressure,
    NotePressure,   // per-note pressure (MPE / MIDI 2.0 / shoulder keyboard)
    NoteTimbre,     // per-note Y / CC74
    NoteX,          // per-note X position
    NotePitch,      // per-note pitch offset, normalised by 12 semitones
    Random,         // random value per note-on
    Breath,         // CC2
    Expression,     // CC11
    Cc1,            // assignable MIDI controllers (ctl.cc1..4: CC numbers), e.g. neck sensors
    Cc2,
    Cc3,
    Cc4,
    Lfo3,           // second per-voice LFO
    Env3,           // third envelope (per voice)
    Constant,       // 1.0 (offsets, or scaled by "via")
    ReleaseVelocity,  // note-off velocity (MPE "lift"); 0.5 until the key is released
    NoteTimbreBi,   // per-note timbre as -1..1 around the centre (CC74 = 64)
    Macro1,         // part macro knobs (macro.1..4), 0..1
    Macro2,
    Macro3,
    Macro4,
    Motion1,        // part motion lanes (tempo-synced step values), -1..1
    Motion2,
    Motion3,
    Motion4,
    Mseg,           // per-voice multi-segment envelope, 0..1
    Super,          // the Super Knob (perf.super, one for all parts), 0..1
    Count
};

enum class ModDest : int {
    None = 0,
    Pitch,          // +-24 semitones * amount
    LayerALevel,
    LayerBLevel,
    LayerCLevel,
    Morph,          // crossfade between layer A and B
    LayerAPitch,    // +-24 semitones
    LayerBPitch,
    LayerCPitch,
    VaPulseWidth,
    VaOscMix,
    VaNoise,
    FmIndex,
    FmFeedback,
    Cutoff,         // +-5 octaves * amount
    Resonance,
    Drive,
    Amp,
    Pan,
    Lfo1Rate,
    WtPos,          // wavetable position (all WT layers), +-1
    WtWarp,         // wavetable warp amount
    FilterMorph,    // formant vowel position, +-1
    VfxAmount,      // voice FX amount, +-1
    LayerAPan,      // +-1
    LayerBPan,
    LayerCPan,
    FmOp1Level,     // FM operator level (all FM layers), x(1 + amount), 0..2
    FmOp2Level,
    FmOp3Level,
    FmOp4Level,
    AmpAttack,      // amp envelope times, x8^amount
    AmpDecay,
    AmpRelease,
    UnisonDetune,   // x(1 + amount)
    VfxMix,         // +-1
    Lfo3Rate,       // +-4 octaves
    FmOp5Level,
    FmOp6Level,
    FmOp7Level,
    FmOp8Level,
    FmModPitch,     // pitch of all FM modulator operators, +-24 semitones (spectrum shift)
    VaOsc2Pitch,    // VA oscillator 2 pitch, +-24 semitones
    VaDetune,       // VA oscillator 2 detune, +-50 cents
    FilterEnvAmt,   // filter envelope amount, +-1
    Env2Time,       // all mod-envelope times, x8^amount
    Env3Time,       // all env3 times, x8^amount
    Lfo1Depth,      // LFO1 as a source, x(1 + amount): e.g. pressure -> vibrato depth
    Lfo3Depth,
    Mod1Amt,        // amount of mod slots 1-4, x(1 + amount)
    Mod2Amt,
    Mod3Amt,
    Mod4Amt,
    WtDetune,       // wavetable unison detune, x(1 + amount)
    WtSpread,       // wavetable unison stereo spread, +-1
    VfxRatio,       // ring modulator ratio, x4^amount
    Cutoff2,        // filter 2 cutoff, +-5 octaves * amount
    GrainPos,       // granular position (all granular layers), +-1
    Count
};

enum class ParamKind { Float, Int, Enum, Bool };
enum class ParamCurve { Linear, Exp };

struct ParamInfo {
    std::string name;    // stable id used in presets, e.g. "a.fm.op1.level"
    std::string group;   // UI grouping, e.g. "Layer A / FM"
    std::string label;   // short display label
    ParamKind kind = ParamKind::Float;
    ParamCurve curve = ParamCurve::Linear;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    std::string unit;
    std::vector<std::string> options;  // enum labels
};

namespace P {

enum Global : int {
    MasterVolume = 0,
    BendRange,
    Morph,
    FilterType,
    FilterCutoff,
    FilterReso,
    FilterDrive,
    FilterKeytrack,
    FilterEnvAmt,
    FilterVelAmt,
    AmpAttack,
    AmpDecay,
    AmpSustain,
    AmpRelease,
    AmpVelSens,
    Env2Attack,
    Env2Decay,
    Env2Sustain,
    Env2Release,
    Lfo1Wave,
    Lfo1Rate,
    Lfo1Delay,
    Lfo1KeySync,
    Lfo2Wave,
    Lfo2Rate,
    Limiter,
    FilterMorph,    // formant filter: vowel a-e-i-o-u (0..1)
    VfxType,
    VfxAmount,
    VfxRatio,       // ring modulator frequency / note frequency
    VfxMix,
    CtlCc1,         // CC numbers feeding the mod sources cc1..cc4
    CtlCc2,
    CtlCc3,
    CtlCc4,
    VoiceMode,      // poly / mono (retrigger) / legato
    Glide,          // portamento time, seconds (0 = off)
    GlideMode,      // always / legato (only between overlapping notes)
    Unison,         // voices per note 1..8
    UnisonDetune,   // cents, total spread
    UnisonSpread,   // stereo spread 0..1
    Lfo1Sync,       // tempo sync division (fx::SyncDiv, 0 = off: rate in Hz)
    Lfo2Sync,
    Lfo3Wave,
    Lfo3Rate,
    Lfo3Sync,
    Lfo3KeySync,
    Env3Attack,
    Env3Decay,
    Env3Sustain,
    Env3Release,
    MasterTune,     // global (master section): cents, A4 = 440 Hz * 2^(tune / 1200)
    VelCurve,       // velocity curve -1 (soft) .. 0 (linear) .. 1 (hard)
    TuneC,          // scale tuning: cents offset per pitch class C .. B (12 params)
    TuneLast = TuneC + 11,
    ArpOn,          // arpeggiator (per part)
    ArpMode,        // ArpMode
    ArpRate,        // division: option k = fx::SyncDiv k + 1 (1/1 .. 1/32, no "off")
    ArpOctaves,     // 1..4
    ArpGate,        // note length as a fraction of the step
    ArpSwing,       // delay of every second step, fraction of a step 0..0.5
    ArpLatch,       // keep playing after the keys are released (until a new chord)
    Macro1,         // macro knobs: mod sources macro1..4 (one knob -> several destinations)
    Macro2,
    Macro3,
    Macro4,
    SendA,          // post-insert send to the reverb bus (send / return, see engine.h)
    SendB,          // ... to the delay bus
    MsegPoints,     // multi-segment envelope: points used (2..8)
    MsegLoop,       // loop start point while the key is held (0 = no loop)
    MsegBeats,      // times in beats (tempo) instead of seconds
    MsegBase,       // 8 points x MsegPointCount (time, level, curve)
    MsegLast = MsegBase + 8 * 3 - 1,
    MotionBase,     // 4 motion lanes x MotionLaneCount (rate, steps, smooth, 16 values)
    MotionLast = MotionBase + 4 * 19 - 1,
    SeqOn,          // step sequencer (per part)
    SeqMode,        // free (runs with the transport) / key (runs while a key is held, transposed from C4)
    SeqRate,        // step length: option k = fx::SyncDiv k + 1 (like arp.rate)
    SeqSteps,       // 1..32
    SeqGate,        // gate scale 0.05..1
    SeqSwing,       // 0..0.5 of a step on every second step
    SeqTranspose,   // -24..24 semitones
    SeqStepBase,    // 32 steps x SeqStepCount (note, velocity (0 = rest), length in steps)
    SeqStepLast = SeqStepBase + 32 * 3 - 1,
    // second voice filter (after the first, or beside it)
    Filter2Type,
    Filter2Cutoff,
    Filter2Reso,
    Filter2Drive,
    Filter2Keytrack,
    Filter2EnvAmt,
    FilterRouting,  // serial (1 -> 2) / parallel (1 + 2)
    FilterBalance,  // parallel: 0 = filter 1 only .. 1 = filter 2 only
    // performance (per part)
    ScaleLock,      // notes are moved to the nearest note of the scale
    ScaleRoot,      // 0 = C .. 11 = B
    ScaleType,      // ScaleType
    ChordMode,      // one key plays a chord (ChordType, 0 = off)
    ChordInversion, // 0..3
    SuperKnob,      // global (not per part): mod source "super" in every part
    GlobalCount
};

constexpr int kMsegPoints = 8;
enum MsegPointParam : int { MsegTime = 0, MsegLevel, MsegCurve, MsegPointCount };
constexpr int kMotionLanes = 4, kMotionSteps = 16;
enum MotionParam : int { MotionRate = 0, MotionSteps, MotionSmooth, MotionValue1, MotionLaneCount = MotionValue1 + kMotionSteps };
constexpr int kSeqSteps = 32;
enum SeqStepParam : int { SeqNote = 0, SeqVel, SeqLen, SeqStepCount };
constexpr int msegPt(int i, int f) { return MsegBase + i * MsegPointCount + f; }
constexpr int motion(int lane, int f) { return MotionBase + lane * MotionLaneCount + f; }
constexpr int seqStep(int i, int f) { return SeqStepBase + i * SeqStepCount + f; }

constexpr int kModSlots = 16;
enum ModSlotParam : int { ModSrc = 0, ModVia, ModDst, ModAmt, ModSlotCount };
constexpr int kModBase = GlobalCount;

// FM: the operator count is a compile-time constant; algorithms are data
// tables (see dsp/fm.h).
constexpr int kFmOps = 8;  // algorithms 1-8 use ops 1-4, 9-16 ops 1-6, 17-88 all eight
enum FmOpParam : int {
    OpRatio = 0, OpDetune, OpLevel, OpAttack, OpDecay, OpSustain, OpRelease, OpVelSens,
    OpWave,      // FmWave
    OpFixed,     // fixed frequency in Hz (0 = follow the key with Ratio)
    OpKeyScale,  // keyboard level scaling, dB per octave from C4
    OpRateScale, // envelope rate scaling: times x 2^(-amount * octaves above C4)
    OpAms,       // amplitude modulation by the voice's LFO1, 0..1
    FmOpCount
};

enum LayerParam : int {
    LType = 0,
    LLevel,
    LPan,
    LCoarse,
    LFine,
    LVelSens,
    VaWave1,
    VaWave2,
    VaPw,
    VaOsc2Semi,
    VaDetune,
    VaMix,
    VaNoise,
    VaSync,         // osc 2 hard-synced to osc 1
    VaXmod,         // osc 1 -> osc 2 frequency modulation 0..1
    VaSub,          // sub oscillator level 0..1
    VaSubWave,      // sub: square -1 oct / sine -1 oct / square -2 oct
    VaSsDetune,     // supersaw detune 0..1
    VaSsMix,        // supersaw side oscillators 0..1
    FmAlgo,
    FmFeedback,
    FmIndex,
    FmPegAmt,       // FM pitch envelope: start offset (semitones), decays to 0
    FmPegTime,      // ... decay time
    FmOpBase,
    PcmStart = FmOpBase + kFmOps * FmOpCount,
    PcmVelTrack,
    WtTable,
    WtPos,
    WtPosEnv,
    WtWarpMode,
    WtWarpAmt,
    WtUnison,
    WtDetune,
    WtSpread,
    OrgDrawbar1,    // tonewheel organ: 9 drawbars 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' (0..8)
    OrgDrawbarLast = OrgDrawbar1 + 8,
    OrgPerc,        // percussion off / 2nd / 3rd harmonic
    OrgPercDecay,   // fast / slow
    OrgPercLevel,
    OrgClick,       // key click level
    OrgLeak,        // tonewheel leakage
    GrSize,         // granular: grain length, seconds
    GrDensity,      // grains per second
    GrPos,          // read position in the sample 0..1
    GrPosRand,      // position jitter 0..1 (of the sample)
    GrPitchRand,    // pitch jitter, semitones
    GrSpread,       // stereo spread of the grains 0..1
    GrScan,         // position movement while the note sounds, x real time (-2..2, 0 = frozen)
    LayerParamCount
};

constexpr int kLayerBase = kModBase + kModSlots * ModSlotCount;

// Effect rack (see fx/fx_params.h): chain settings, FX modulation slots, then
// the parameters of every effect block.
enum FxChainParam : int {
    FxTempo = 0, FxRetrig, FxMacro1, FxMacro2, LooperLevel, FxClockSync,
    // send / return buses: A = reverb, B = delay (fed by the parts' send.a / send.b)
    SendAType, SendASize, SendADecay, SendAPreDelay, SendADamp, SendALevel,
    SendBSync, SendBTime, SendBFeedback, SendBTone, SendBLevel,
    FxSlot1, FxChainCount = FxSlot1 + fx::kFxSlots
};
enum FxModParam : int { FxModSrc = 0, FxModDst, FxModAmt, FxModCount };
// Part insert effects: slot params (enum: 0 = none, k = kInsertBlockList[k-1])
// followed by the parameters of every insert block.
constexpr int kInsBase = kLayerBase + 3 * LayerParamCount;
constexpr int kInsBlockBase = kInsBase + fx::kInsertSlots;
constexpr int kFxChainBase = kInsBlockBase + fx::kInsertParamTotal;
constexpr int kFxModBase = kFxChainBase + FxChainCount;
constexpr int kFxBlockBase = kFxModBase + fx::kFxModSlots * FxModCount;
constexpr int kNumParams = kFxBlockBase + fx::kBlockParamTotal;

constexpr int layer(int l, int p) { return kLayerBase + l * LayerParamCount + p; }
constexpr int fmOp(int l, int op, int p) { return layer(l, FmOpBase + op * FmOpCount + p); }
constexpr int mod(int slot, int p) { return kModBase + slot * ModSlotCount + p; }
constexpr int fxChain(int p) { return kFxChainBase + p; }
constexpr int fxSlot(int slot) { return kFxChainBase + FxSlot1 + slot; }
constexpr int fxMod(int slot, int p) { return kFxModBase + slot * FxModCount + p; }
constexpr int fxp(int block, int local) { return kFxBlockBase + fx::blockOffset(block) + local; }
constexpr int insSlot(int slot) { return kInsBase + slot; }
// parameter of insert block `block` (must be in fx::kInsertBlockList)
constexpr int insp(int block, int local) { return kInsBlockBase + fx::insertOffset(fx::insertIndex(block)) + local; }

// Multi-timbral parts: ids [0, kPartParams) form one part's patch (voice,
// layers, mod matrix). MasterVolume and Limiter fall in that range but belong
// to the master section: they are only read from the global array.
constexpr int kPartParams = kFxChainBase;

}  // namespace P

constexpr int kNumParts = 16;

// True for ids stored per part (everything below the effect rack except the
// master volume, limiter switch and master tune).
constexpr bool isPartParam(int id) {
    return id >= 0 && id < P::kPartParams && id != P::MasterVolume && id != P::Limiter && id != P::MasterTune &&
           id != P::SuperKnob;
}

// Per-part mix settings (multi-timbral mode only; see engine.h).
namespace Mix {
enum : int {
    Volume = 0,  // 0..1, gain = volume^2 (same taper as master volume)
    Pan,         // -1..1, balance (centre = unity on both sides)
    Mute,        // bool: muted parts ignore note-ons and fade out
    Channel,     // MIDI channel 1..16 the part listens to (several parts may share one = layer)
    KeyLow,      // key range 0..127 (inclusive, before transpose)
    KeyHigh,
    Transpose,   // semitones -48..48
    FxSend,      // 0 = dry (bypasses the rack), 1 = all into the shared effect rack
    Count
};
}

// Metadata of the mix settings (names: "volume", "pan", "mute", "channel",
// "key.low", "key.high", "transpose", "fx.send"). def of "channel" is 1; use
// defaultPartMix() for the per-part default (part N -> channel N+1).
const std::vector<ParamInfo>& partMixTable();
void defaultPartMix(int part, float* values);  // Mix::Count values
float sanitizePartMix(int id, float value);

// Metadata for all parameters, indexed by id. Built once (thread-safe static).
const std::vector<ParamInfo>& paramTable();

// Look up id by name; returns -1 if unknown.
int findParam(const std::string& name);

// Parameter ids an FX modulation slot can target (index = enum value of
// "fxmodN.dst"; entry 0 is "none" = -1).
const std::vector<int>& fxModTargets();

// Fill array with default values (size kNumParams).
void defaultParams(float* values);

// Clamp and quantise a value according to the metadata.
float sanitizeParam(int id, float value);

}  // namespace hsynth
