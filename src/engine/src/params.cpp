#include "hsynth/params.h"

#include "hsynth/dsp/fm.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "hsynth/dsp/wavetable.h"

namespace hsynth {

namespace {

const std::vector<std::string> kLayerTypeNames = {"off", "va", "fm", "pcm", "wt", "organ", "granular"};
const std::vector<std::string> kVaWaveNames = {"saw", "square", "pulse", "triangle", "sine", "supersaw"};
const std::vector<std::string> kFilterNames = {"off", "lp12", "lp24", "bp", "hp", "notch", "ladder", "comb+", "comb-", "formant"};
const std::vector<std::string> kVfxNames = {"off", "saturate", "fold", "crush", "ring"};
const std::vector<std::string> kLfoWaveNames = {"sine", "triangle", "saw", "square", "s&h"};
const std::vector<std::string> kModSourceNames = {
    "none", "velocity", "key", "ampenv", "env2", "lfo1", "lfo2", "modwheel", "pitchbend",
    "chanpressure", "notepressure", "notetimbre", "notex", "notepitch", "random", "breath", "expression", "cc1", "cc2", "cc3", "cc4", "lfo3", "env3", "constant", "relvelocity", "notetimbre+-",
    "macro1", "macro2", "macro3", "macro4", "motion1", "motion2", "motion3", "motion4", "mseg", "super"};
const std::vector<std::string> kModDestNames = {
    "none", "pitch", "a.level", "b.level", "c.level", "morph", "a.pitch", "b.pitch", "c.pitch",
    "va.pw", "va.mix", "va.noise", "fm.index", "fm.feedback", "cutoff", "resonance", "drive",
    "amp", "pan", "lfo1.rate", "wt.pos", "wt.warp", "filter.morph", "vfx.amount",
    "a.pan", "b.pan", "c.pan", "fm.op1.level", "fm.op2.level", "fm.op3.level", "fm.op4.level",
    "amp.attack", "amp.decay", "amp.release", "unison.detune", "vfx.mix", "lfo3.rate", "fm.op5.level", "fm.op6.level",
    "fm.op7.level", "fm.op8.level", "fm.modpitch", "va.osc2pitch", "va.detune", "filter.envamt", "env2.time", "env3.time",
    "lfo1.depth", "lfo3.depth", "mod1.amt", "mod2.amt", "mod3.amt", "mod4.amt", "wt.detune", "wt.spread", "vfx.ratio",
    "cutoff2", "grain.pos"};
const std::vector<std::string> kScaleNames = {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian",
                                              "harmonic minor", "melodic minor", "major penta", "minor penta", "blues",
                                              "whole tone", "chromatic"};
const std::vector<std::string> kChordNames = {"off", "major", "minor", "7", "maj7", "m7", "sus2", "sus4", "power",
                                              "octave", "dim", "aug", "add9", "m9"};
const std::vector<std::string> kFmWaveNames = {"sine", "half", "abs", "quarter", "alt", "altabs", "square", "saw"};

struct Builder {
    std::vector<ParamInfo> t;
    Builder() { t.resize(P::kNumParams); }

    ParamInfo& set(int id, const std::string& name, const std::string& group, const std::string& label,
                   float mn, float mx, float def, const std::string& unit = "") {
        ParamInfo& p = t[id];
        p.name = name; p.group = group; p.label = label;
        p.kind = ParamKind::Float; p.curve = ParamCurve::Linear;
        p.min = mn; p.max = mx; p.def = def; p.unit = unit;
        return p;
    }
    void fl(int id, const std::string& n, const std::string& g, const std::string& l, float mn, float mx, float d,
            const std::string& unit = "") {
        set(id, n, g, l, mn, mx, d, unit);
    }
    void ex(int id, const std::string& n, const std::string& g, const std::string& l, float mn, float mx, float d,
            const std::string& unit) {
        set(id, n, g, l, mn, mx, d, unit).curve = ParamCurve::Exp;
    }
    void in(int id, const std::string& n, const std::string& g, const std::string& l, float mn, float mx, float d,
            const std::string& unit = "") {
        set(id, n, g, l, mn, mx, d, unit).kind = ParamKind::Int;
    }
    void bo(int id, const std::string& n, const std::string& g, const std::string& l, bool d) {
        set(id, n, g, l, 0, 1, d ? 1.0f : 0.0f).kind = ParamKind::Bool;
    }
    void en(int id, const std::string& n, const std::string& g, const std::string& l,
            const std::vector<std::string>& opts, int d) {
        ParamInfo& p = set(id, n, g, l, 0, (float)opts.size() - 1, (float)d);
        p.kind = ParamKind::Enum;
        p.options = opts;
    }
};

#include "params_fx.inc"

std::vector<ParamInfo> build() {
    using namespace P;
    Builder b;
    b.fl(MasterVolume, "master.volume", "Master", "Volume", 0, 1, 0.7f);
    b.in(BendRange, "master.bendrange", "Master", "Bend Range", 0, 24, 2, "st");
    b.fl(Morph, "mix.morph", "Master", "Morph A<>B", -1, 1, 0);

    b.en(P::FilterType, "filter.type", "Filter", "Type", kFilterNames, (int)hsynth::FilterType::LP24);
    b.ex(FilterCutoff, "filter.cutoff", "Filter", "Cutoff", 20, 20000, 8000, "Hz");
    b.fl(FilterReso, "filter.reso", "Filter", "Resonance", 0, 1, 0.15f);
    b.fl(FilterDrive, "filter.drive", "Filter", "Drive", 0, 1, 0);
    b.fl(FilterKeytrack, "filter.keytrack", "Filter", "Key Track", 0, 1, 0.5f);
    b.fl(FilterEnvAmt, "filter.envamt", "Filter", "Env2 Amount", -1, 1, 0.2f);
    b.fl(FilterVelAmt, "filter.velamt", "Filter", "Vel Amount", 0, 1, 0.2f);

    b.ex(AmpAttack, "amp.attack", "Amp Env", "Attack", 0.0005f, 10, 0.003f, "s");
    b.ex(AmpDecay, "amp.decay", "Amp Env", "Decay", 0.001f, 20, 0.4f, "s");
    b.fl(AmpSustain, "amp.sustain", "Amp Env", "Sustain", 0, 1, 0.8f);
    b.ex(AmpRelease, "amp.release", "Amp Env", "Release", 0.001f, 20, 0.3f, "s");
    b.fl(AmpVelSens, "amp.velsens", "Amp Env", "Vel Sens", 0, 1, 0.6f);

    b.ex(Env2Attack, "env2.attack", "Mod Env", "Attack", 0.0005f, 10, 0.003f, "s");
    b.ex(Env2Decay, "env2.decay", "Mod Env", "Decay", 0.001f, 20, 0.5f, "s");
    b.fl(Env2Sustain, "env2.sustain", "Mod Env", "Sustain", 0, 1, 0.3f);
    b.ex(Env2Release, "env2.release", "Mod Env", "Release", 0.001f, 20, 0.4f, "s");

    b.en(Lfo1Wave, "lfo1.wave", "LFO", "LFO1 Wave", kLfoWaveNames, 0);
    b.ex(Lfo1Rate, "lfo1.rate", "LFO", "LFO1 Rate", 0.01f, 40, 5.5f, "Hz");
    b.ex(Lfo1Delay, "lfo1.delay", "LFO", "LFO1 Delay", 0.001f, 5, 0.001f, "s");
    b.bo(Lfo1KeySync, "lfo1.keysync", "LFO", "LFO1 Key Sync", true);
    b.en(Lfo2Wave, "lfo2.wave", "LFO", "LFO2 Wave", kLfoWaveNames, 1);
    b.ex(Lfo2Rate, "lfo2.rate", "LFO", "LFO2 Rate", 0.01f, 40, 0.3f, "Hz");

    b.bo(Limiter, "master.limiter", "Master", "Limiter", true);
    b.fl(P::FilterMorph, "filter.morph", "Filter", "Vowel", 0, 1, 0);
    b.en(P::VfxType, "vfx.type", "Voice FX", "Type", kVfxNames, 0);
    b.fl(P::VfxAmount, "vfx.amount", "Voice FX", "Amount", 0, 1, 0.5f);
    b.ex(P::VfxRatio, "vfx.ratio", "Voice FX", "Ring Ratio", 0.25f, 16, 2, "x");
    b.fl(P::VfxMix, "vfx.mix", "Voice FX", "Mix", 0, 1, 1);
    b.en(P::VoiceMode, "voice.mode", "Voice", "Mode", {"poly", "mono", "legato"}, 0);
    b.fl(P::Glide, "voice.glide", "Voice", "Glide", 0, 2, 0, "s");
    b.en(P::GlideMode, "voice.glidemode", "Voice", "Glide Mode", {"always", "legato"}, 0);
    b.in(P::Unison, "voice.unison", "Voice", "Unison", 1, 8, 1);
    b.fl(P::UnisonDetune, "voice.unidetune", "Voice", "Unison Detune", 0, 100, 15, "ct");
    b.fl(P::UnisonSpread, "voice.unispread", "Voice", "Unison Spread", 0, 1, 0.5f);
    const std::vector<std::string>& syncNames = kSyncNames;
    b.en(P::Lfo1Sync, "lfo1.sync", "LFO", "LFO1 Sync", syncNames, 0);
    b.en(P::Lfo2Sync, "lfo2.sync", "LFO", "LFO2 Sync", syncNames, 0);
    b.en(P::Lfo3Wave, "lfo3.wave", "LFO", "LFO3 Wave", kLfoWaveNames, 0);
    b.ex(P::Lfo3Rate, "lfo3.rate", "LFO", "LFO3 Rate", 0.01f, 40, 2.0f, "Hz");
    b.en(P::Lfo3Sync, "lfo3.sync", "LFO", "LFO3 Sync", syncNames, 0);
    b.bo(P::Lfo3KeySync, "lfo3.keysync", "LFO", "LFO3 Key Sync", true);
    b.ex(P::Env3Attack, "env3.attack", "Env 3", "Attack", 0.0005f, 10, 0.003f, "s");
    b.ex(P::Env3Decay, "env3.decay", "Env 3", "Decay", 0.001f, 20, 0.5f, "s");
    b.fl(P::Env3Sustain, "env3.sustain", "Env 3", "Sustain", 0, 1, 0.0f);
    b.ex(P::Env3Release, "env3.release", "Env 3", "Release", 0.001f, 20, 0.3f, "s");
    b.bo(P::ArpOn, "arp.on", "Arpeggiator", "Arp On", false);
    b.en(P::ArpMode, "arp.mode", "Arpeggiator", "Mode", {"up", "down", "updown", "random", "order", "chord"}, 0);
    b.en(P::ArpRate, "arp.rate", "Arpeggiator", "Rate", std::vector<std::string>(kSyncNames.begin() + 1, kSyncNames.end()),
         10);  // 1/16 (option k = fx::SyncDiv k + 1)
    b.in(P::ArpOctaves, "arp.octaves", "Arpeggiator", "Octaves", 1, 4, 1);
    b.fl(P::ArpGate, "arp.gate", "Arpeggiator", "Gate", 0.05f, 1.0f, 0.5f);
    b.fl(P::ArpSwing, "arp.swing", "Arpeggiator", "Swing", 0, 0.5f, 0);
    b.bo(P::ArpLatch, "arp.latch", "Arpeggiator", "Latch", false);
    b.fl(P::MasterTune, "master.tune", "Master", "Master Tune", -100, 100, 0, "ct");
    b.fl(P::VelCurve, "voice.velcurve", "Voice", "Velocity Curve", -1, 1, 0);
    {
        static const char* kPc[12] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
        static const char* kPcLabel[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        for (int i = 0; i < 12; ++i)
            b.fl(P::TuneC + i, std::string("tune.") + kPc[i], "Tuning", kPcLabel[i], -100, 100, 0, "ct");
    }
    for (int i = 0; i < 4; ++i)
        b.in(P::CtlCc1 + i, "ctl.cc" + std::to_string(i + 1), "Controllers", "CC" + std::to_string(i + 1) + " #", 0, 127,
             (float)(16 + i));
    for (int i = 0; i < 4; ++i)
        b.fl(P::Macro1 + i, "macro." + std::to_string(i + 1), "Macros", "Macro " + std::to_string(i + 1), 0, 1, 0);
    b.fl(P::SendA, "send.a", "Sends", "Send A (Reverb)", 0, 1, 0);
    b.fl(P::SendB, "send.b", "Sends", "Send B (Delay)", 0, 1, 0);
    // multi-segment envelope (mod source "mseg")
    b.in(P::MsegPoints, "mseg.points", "MSEG", "Points", 2, kMsegPoints, 4);
    b.in(P::MsegLoop, "mseg.loop", "MSEG", "Loop From Point (0 = off)", 0, kMsegPoints - 1, 0);
    b.bo(P::MsegBeats, "mseg.beats", "MSEG", "Times in Beats", false);
    {
        static const float kT[8] = {0, 0.05f, 0.3f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
        static const float kL[8] = {0, 1, 0.5f, 0, 0, 0, 0, 0};
        for (int i = 0; i < kMsegPoints; ++i) {
            const std::string n = "mseg.p" + std::to_string(i + 1), l = "P" + std::to_string(i + 1) + " ";
            b.fl(msegPt(i, MsegTime), n + ".time", "MSEG", l + "Time (from previous)", 0, 10, kT[i] - (i ? kT[i - 1] : 0), "s");
            b.fl(msegPt(i, MsegLevel), n + ".level", "MSEG", l + "Level", 0, 1, kL[i]);
            b.fl(msegPt(i, MsegCurve), n + ".curve", "MSEG", l + "Curve", -1, 1, 0);
        }
    }
    // motion lanes (mod sources "motion1..4")
    for (int ln = 0; ln < kMotionLanes; ++ln) {
        const std::string n = "motion" + std::to_string(ln + 1), g = "Motion " + std::to_string(ln + 1);
        b.en(motion(ln, MotionRate), n + ".rate", g, "Step", std::vector<std::string>(kSyncNames.begin() + 1, kSyncNames.end()), 10);
        b.in(motion(ln, MotionSteps), n + ".steps", g, "Steps", 1, kMotionSteps, 16);
        b.fl(motion(ln, MotionSmooth), n + ".smooth", g, "Smooth", 0, 1, 0);
        for (int s = 0; s < kMotionSteps; ++s)
            b.fl(motion(ln, MotionValue1 + s), n + ".v" + std::to_string(s + 1), g, "Step " + std::to_string(s + 1), -1, 1, 0);
    }
    // step sequencer
    b.bo(P::SeqOn, "seq.on", "Sequencer", "Sequencer On", false);
    b.en(P::SeqMode, "seq.mode", "Sequencer", "Mode", {"free", "key"}, 0);
    b.en(P::SeqRate, "seq.rate", "Sequencer", "Step", std::vector<std::string>(kSyncNames.begin() + 1, kSyncNames.end()), 10);
    b.in(P::SeqSteps, "seq.steps", "Sequencer", "Steps", 1, kSeqSteps, 16);
    b.fl(P::SeqGate, "seq.gate", "Sequencer", "Gate", 0.05f, 1, 0.8f);
    b.fl(P::SeqSwing, "seq.swing", "Sequencer", "Swing", 0, 0.5f, 0);
    b.in(P::SeqTranspose, "seq.transpose", "Sequencer", "Transpose", -24, 24, 0, "st");
    // second filter
    b.en(P::Filter2Type, "filter2.type", "Filter 2", "Type", kFilterNames, 0);
    b.ex(P::Filter2Cutoff, "filter2.cutoff", "Filter 2", "Cutoff", 20, 20000, 2000, "Hz");
    b.fl(P::Filter2Reso, "filter2.reso", "Filter 2", "Resonance", 0, 1, 0.15f);
    b.fl(P::Filter2Drive, "filter2.drive", "Filter 2", "Drive", 0, 1, 0);
    b.fl(P::Filter2Keytrack, "filter2.keytrack", "Filter 2", "Key Track", 0, 1, 0.5f);
    b.fl(P::Filter2EnvAmt, "filter2.envamt", "Filter 2", "Env2 Amount", -1, 1, 0);
    b.en(P::FilterRouting, "filter.routing", "Filter 2", "Routing", {"serial", "parallel"}, 0);
    b.fl(P::FilterBalance, "filter.balance", "Filter 2", "Parallel Balance", 0, 1, 0.5f);
    // performance
    b.bo(P::ScaleLock, "scale.lock", "Performance", "Scale Lock", false);
    b.en(P::ScaleRoot, "scale.root", "Performance", "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0);
    b.en(P::ScaleType, "scale.type", "Performance", "Scale", kScaleNames, 0);
    b.en(P::ChordMode, "chord.mode", "Performance", "Chord", kChordNames, 0);
    b.in(P::ChordInversion, "chord.inversion", "Performance", "Inversion", 0, 3, 0);
    b.fl(P::SuperKnob, "perf.super", "Performance", "Super Knob", 0, 1, 0);
    for (int s = 0; s < kSeqSteps; ++s) {
        const std::string n = "seq.s" + std::to_string(s + 1), l = "Step " + std::to_string(s + 1) + " ";
        b.in(seqStep(s, SeqNote), n + ".note", "Sequencer", l + "Note", 0, 127, 60);
        b.fl(seqStep(s, SeqVel), n + ".vel", "Sequencer", l + "Velocity (0 = rest)", 0, 1, s < 16 && s % 4 == 0 ? 0.8f : 0.0f);
        b.fl(seqStep(s, SeqLen), n + ".len", "Sequencer", l + "Length (steps)", 0.1f, 8, 1);
    }

    buildFx(b);

    for (int s = 0; s < kModSlots; ++s) {
        const std::string n = "mod" + std::to_string(s + 1);
        const std::string g = "Mod Matrix";
        const std::string l = "#" + std::to_string(s + 1) + " ";
        b.en(mod(s, ModSrc), n + ".src", g, l + "Source", kModSourceNames, 0);
        b.en(mod(s, ModVia), n + ".via", g, l + "Via", kModSourceNames, 0);
        b.en(mod(s, ModDst), n + ".dst", g, l + "Dest", kModDestNames, 0);
        b.fl(mod(s, ModAmt), n + ".amt", g, l + "Amount", -1, 1, 0);
    }

    const char* layerNames[3] = {"a", "b", "c"};
    const char* layerLabels[3] = {"Layer A", "Layer B", "Layer C"};
    for (int l = 0; l < 3; ++l) {
        const std::string n = layerNames[l];
        const std::string g = layerLabels[l];
        b.en(layer(l, LType), n + ".type", g, "Type", kLayerTypeNames,
             l == 0 ? (int)LayerType::VA : (int)LayerType::Off);
        b.fl(layer(l, LLevel), n + ".level", g, "Level", 0, 1, 0.8f);
        b.fl(layer(l, LPan), n + ".pan", g, "Pan", -1, 1, 0);
        b.in(layer(l, LCoarse), n + ".coarse", g, "Coarse", -24, 24, 0, "st");
        b.fl(layer(l, LFine), n + ".fine", g, "Fine", -100, 100, 0, "ct");
        b.fl(layer(l, LVelSens), n + ".velsens", g, "Vel->Level", 0, 1, 0);

        const std::string gv = g + " / VA";
        b.en(layer(l, VaWave1), n + ".va.wave1", gv, "Osc1 Wave", kVaWaveNames, 0);
        b.en(layer(l, VaWave2), n + ".va.wave2", gv, "Osc2 Wave", kVaWaveNames, 0);
        b.fl(layer(l, VaPw), n + ".va.pw", gv, "Pulse Width", 0.05f, 0.95f, 0.5f);
        b.in(layer(l, VaOsc2Semi), n + ".va.osc2semi", gv, "Osc2 Semi", -24, 24, 0, "st");
        b.fl(layer(l, VaDetune), n + ".va.detune", gv, "Detune", 0, 50, 7, "ct");
        b.fl(layer(l, VaMix), n + ".va.mix", gv, "Osc1<>Osc2", 0, 1, 0.5f);
        b.fl(layer(l, VaNoise), n + ".va.noise", gv, "Noise", 0, 1, 0);
        b.bo(layer(l, VaSync), n + ".va.sync", gv, "Osc2 Hard Sync", false);
        b.fl(layer(l, VaXmod), n + ".va.xmod", gv, "Cross Mod (Osc1>2)", 0, 1, 0);
        b.fl(layer(l, VaSub), n + ".va.sub", gv, "Sub Osc", 0, 1, 0);
        b.en(layer(l, VaSubWave), n + ".va.subwave", gv, "Sub Wave", {"square -1", "sine -1", "square -2"}, 0);
        b.fl(layer(l, VaSsDetune), n + ".va.ssdetune", gv, "Supersaw Detune", 0, 1, 0.4f);
        b.fl(layer(l, VaSsMix), n + ".va.ssmix", gv, "Supersaw Mix", 0, 1, 0.6f);

        const std::string gf = g + " / FM";
        b.en(layer(l, FmAlgo), n + ".fm.algo", gf, "Algorithm", fmAlgorithmNames(), 0);
        b.fl(layer(l, FmFeedback), n + ".fm.feedback", gf, "Feedback", 0, 1, 0);
        b.fl(layer(l, FmIndex), n + ".fm.index", gf, "Mod Index", 0, 1, 0.5f);
        b.fl(layer(l, FmPegAmt), n + ".fm.peg.amt", gf, "Pitch Env Amount", -24, 24, 0, "st");
        b.ex(layer(l, FmPegTime), n + ".fm.peg.time", gf, "Pitch Env Time", 0.001f, 10, 0.1f, "s");
        for (int op = 0; op < kFmOps; ++op) {
            const std::string on = n + ".fm.op" + std::to_string(op + 1);
            const std::string ol = "Op" + std::to_string(op + 1) + " ";
            b.in(fmOp(l, op, OpRatio), on + ".ratio", gf, ol + "Ratio", 0, 32, 1, "x");
            b.fl(fmOp(l, op, OpDetune), on + ".detune", gf, ol + "Detune", -50, 50, 0, "ct");
            b.fl(fmOp(l, op, OpLevel), on + ".level", gf, ol + "Level", 0, 1, op == 0 ? 1.0f : 0.5f);
            b.ex(fmOp(l, op, OpAttack), on + ".attack", gf, ol + "Attack", 0.0005f, 10, 0.002f, "s");
            b.ex(fmOp(l, op, OpDecay), on + ".decay", gf, ol + "Decay", 0.001f, 20, 1.0f, "s");
            b.fl(fmOp(l, op, OpSustain), on + ".sustain", gf, ol + "Sustain", 0, 1, op == 0 ? 0.8f : 0.4f);
            b.ex(fmOp(l, op, OpRelease), on + ".release", gf, ol + "Release", 0.001f, 20, 0.3f, "s");
            b.fl(fmOp(l, op, OpVelSens), on + ".velsens", gf, ol + "Vel Sens", 0, 1, op == 0 ? 0.0f : 0.5f);
            b.en(fmOp(l, op, OpWave), on + ".wave", gf, ol + "Wave", kFmWaveNames, 0);
            b.fl(fmOp(l, op, OpFixed), on + ".fixed", gf, ol + "Fixed Freq (0 = ratio)", 0, 8000, 0, "Hz");
            b.fl(fmOp(l, op, OpKeyScale), on + ".keyscale", gf, ol + "Key Scaling", -12, 12, 0, "dB/oct");
            b.fl(fmOp(l, op, OpRateScale), on + ".ratescale", gf, ol + "Rate Scaling", 0, 1, 0);
            b.fl(fmOp(l, op, OpAms), on + ".ams", gf, ol + "LFO1 AM Sens", 0, 1, 0);
        }

        const std::string gp = g + " / PCM";
        b.fl(layer(l, PcmStart), n + ".pcm.start", gp, "Start Offset", 0, 1, 0);
        b.fl(layer(l, PcmVelTrack), n + ".pcm.veltrack", gp, "SFZ Vel Track", 0, 1, 1.0f);

        const std::string gw = g + " / WT";
        std::vector<std::string> tables = builtinWavetableNames();
        tables.push_back("user");
        b.en(layer(l, WtTable), n + ".wt.table", gw, "Table", tables, 0);
        b.fl(layer(l, WtPos), n + ".wt.pos", gw, "Position", 0, 1, 0);
        b.fl(layer(l, WtPosEnv), n + ".wt.posenv", gw, "Env2 > Position", -1, 1, 0);
        b.en(layer(l, WtWarpMode), n + ".wt.warp", gw, "Warp", {"none", "sync", "bend", "mirror", "squeeze"}, 0);
        b.fl(layer(l, WtWarpAmt), n + ".wt.warpamt", gw, "Warp Amount", 0, 1, 0);
        b.in(layer(l, WtUnison), n + ".wt.unison", gw, "Unison", 1, 7, 1);
        b.fl(layer(l, WtDetune), n + ".wt.detune", gw, "Unison Detune", 0, 100, 12, "ct");
        b.fl(layer(l, WtSpread), n + ".wt.spread", gw, "Unison Spread", 0, 1, 0.7f);

        const std::string go = g + " / Organ";
        static const char* kFeet[9] = {"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};
        static const float kBars[9] = {8, 8, 8, 0, 0, 0, 0, 0, 0};  // "888000000"
        for (int k = 0; k < 9; ++k)
            b.fl(layer(l, OrgDrawbar1 + k), n + ".organ.bar" + std::to_string(k + 1), go, std::string("Drawbar ") + kFeet[k], 0, 8,
                 kBars[k]);
        b.en(layer(l, OrgPerc), n + ".organ.perc", go, "Percussion", {"off", "2nd", "3rd"}, 0);
        b.en(layer(l, OrgPercDecay), n + ".organ.percdecay", go, "Perc Decay", {"fast", "slow"}, 0);
        b.fl(layer(l, OrgPercLevel), n + ".organ.perclevel", go, "Perc Level", 0, 1, 0.6f);
        b.fl(layer(l, OrgClick), n + ".organ.click", go, "Key Click", 0, 1, 0.3f);
        b.fl(layer(l, OrgLeak), n + ".organ.leak", go, "Leakage", 0, 1, 0.2f);

        const std::string gg = g + " / Granular";
        b.ex(layer(l, GrSize), n + ".grain.size", gg, "Grain Size", 0.005f, 0.5f, 0.08f, "s");
        b.ex(layer(l, GrDensity), n + ".grain.density", gg, "Density", 1, 120, 25, "/s");
        b.fl(layer(l, GrPos), n + ".grain.pos", gg, "Position", 0, 1, 0.3f);
        b.fl(layer(l, GrPosRand), n + ".grain.posrand", gg, "Pos Jitter", 0, 1, 0.05f);
        b.fl(layer(l, GrPitchRand), n + ".grain.pitchrand", gg, "Pitch Jitter", 0, 12, 0, "st");
        b.fl(layer(l, GrSpread), n + ".grain.spread", gg, "Stereo Spread", 0, 1, 0.5f);
        b.fl(layer(l, GrScan), n + ".grain.scan", gg, "Scan", -2, 2, 0, "x");
    }

    // Part insert effects: copies of the rack metadata of the insert blocks,
    // renamed "ins.<key>.<param>", group "Part FX / <title>". On defaults to
    // true: choosing a block for a slot is what switches it in.
    {
        std::vector<std::string> opts = {"none"};
        for (int k = 0; k < fx::kNumInsertBlocks; ++k) opts.push_back(kBlockKeys[fx::kInsertBlockList[k]]);
        for (int s = 0; s < fx::kInsertSlots; ++s)
            b.en(insSlot(s), "ins.slot" + std::to_string(s + 1), "Part FX", "Insert " + std::to_string(s + 1), opts, 0);
        for (int k = 0; k < fx::kNumInsertBlocks; ++k) {
            const int blk = fx::kInsertBlockList[k];
            for (int l = 0; l < fx::kBlockParams[blk]; ++l) {
                ParamInfo info = b.t[(size_t)fxp(blk, l)];
                info.name = "ins." + info.name.substr(3);  // "fx.chorus.mix" -> "ins.chorus.mix"
                info.group = std::string("Part FX / ") + kBlockTitles[blk];
                if (l == 0) info.def = 1.0f;
                b.t[(size_t)insp(blk, l)] = info;
            }
        }
    }

    // FX modulation destinations: every continuous or switch parameter of the
    // effect blocks, by name (stable in presets).
    std::vector<std::string> dstNames = {"none"};
    fxTargets().assign(1, -1);
    for (int id = kFxBlockBase; id < kNumParams; ++id) {
        const ParamInfo& p = b.t[(size_t)id];
        if (p.kind == ParamKind::Float || p.kind == ParamKind::Bool) {
            dstNames.push_back(p.name);
            fxTargets().push_back(id);
        }
    }
    for (int s = 0; s < fx::kFxModSlots; ++s) {
        const std::string n = "fxmod" + std::to_string(s + 1);
        const std::string l = "#" + std::to_string(s + 1) + " ";
        b.en(fxMod(s, FxModSrc), n + ".src", "FX Mod", l + "Source", kFxModSourceNames, 0);
        b.en(fxMod(s, FxModDst), n + ".dst", "FX Mod", l + "Target", dstNames, 0);
        b.fl(fxMod(s, FxModAmt), n + ".amt", "FX Mod", l + "Amount", -1, 1, 0);
    }

    // Default modulation: mod wheel adds LFO1 vibrato, per-note timbre opens
    // the filter, per-note pressure adds level.
    b.t[mod(0, ModSrc)].def = (float)ModSource::Lfo1;
    b.t[mod(0, ModVia)].def = (float)ModSource::ModWheel;
    b.t[mod(0, ModDst)].def = (float)ModDest::Pitch;
    b.t[mod(0, ModAmt)].def = 0.02f;
    b.t[mod(1, ModSrc)].def = (float)ModSource::NoteTimbre;
    b.t[mod(1, ModDst)].def = (float)ModDest::Cutoff;
    b.t[mod(1, ModAmt)].def = 0.3f;
    b.t[mod(2, ModSrc)].def = (float)ModSource::ChannelPressure;
    b.t[mod(2, ModDst)].def = (float)ModDest::Cutoff;
    b.t[mod(2, ModAmt)].def = 0.2f;
    return b.t;
}

}  // namespace

const std::vector<ParamInfo>& paramTable() {
    static const std::vector<ParamInfo> table = build();
    return table;
}

const std::vector<int>& fxModTargets() {
    paramTable();  // filled while the table is built
    return fxTargets();
}

namespace fx {
const char* blockKey(int block) { return block >= 0 && block < BlockCount ? kBlockKeys[block] : "none"; }
const char* blockTitle(int block) { return block >= 0 && block < BlockCount ? kBlockTitles[block] : "None"; }
}  // namespace fx

int findParam(const std::string& name) {
    static const std::unordered_map<std::string, int> index = [] {
        std::unordered_map<std::string, int> m;
        const auto& t = paramTable();
        for (int i = 0; i < (int)t.size(); ++i) m[t[i].name] = i;
        return m;
    }();
    auto it = index.find(name);
    return it == index.end() ? -1 : it->second;
}

void defaultParams(float* values) {
    const auto& t = paramTable();
    for (int i = 0; i < P::kNumParams; ++i) values[i] = t[i].def;
}

float sanitizeParam(int id, float value) {
    const auto& t = paramTable();
    if (id < 0 || id >= (int)t.size()) return value;
    const ParamInfo& p = t[id];
    if (!std::isfinite(value)) value = p.def;
    if (value < p.min) value = p.min;
    if (value > p.max) value = p.max;
    if (p.kind != ParamKind::Float) value = std::round(value);
    return value;
}

const std::vector<ParamInfo>& partMixTable() {
    static const std::vector<ParamInfo> table = [] {
        std::vector<ParamInfo> t((size_t)Mix::Count);
        auto set = [&](int id, const char* name, const char* label, ParamKind kind, float mn, float mx, float def,
                       const char* unit = "") {
            ParamInfo& p = t[(size_t)id];
            p.name = name; p.group = "Part"; p.label = label; p.kind = kind;
            p.min = mn; p.max = mx; p.def = def; p.unit = unit;
        };
        set(Mix::Volume, "volume", "Volume", ParamKind::Float, 0, 1, 1);
        set(Mix::Pan, "pan", "Pan", ParamKind::Float, -1, 1, 0);
        set(Mix::Mute, "mute", "Mute", ParamKind::Bool, 0, 1, 0);
        set(Mix::Channel, "channel", "MIDI Ch", ParamKind::Int, 1, 16, 1);
        set(Mix::KeyLow, "key.low", "Key Lo", ParamKind::Int, 0, 127, 0);
        set(Mix::KeyHigh, "key.high", "Key Hi", ParamKind::Int, 0, 127, 127);
        set(Mix::Transpose, "transpose", "Transpose", ParamKind::Int, -48, 48, 0, "st");
        set(Mix::FxSend, "fx.send", "FX Send", ParamKind::Float, 0, 1, 1);
        return t;
    }();
    return table;
}

void defaultPartMix(int part, float* values) {
    const auto& t = partMixTable();
    for (int i = 0; i < Mix::Count; ++i) values[i] = t[(size_t)i].def;
    values[Mix::Channel] = (float)(1 + std::max(0, std::min(kNumParts - 1, part)) % 16);
}

float sanitizePartMix(int id, float value) {
    if (id < 0 || id >= Mix::Count) return 0.0f;
    const ParamInfo& p = partMixTable()[(size_t)id];
    if (!std::isfinite(value)) value = p.def;
    value = std::min(p.max, std::max(p.min, value));
    if (p.kind == ParamKind::Bool) return value > 0.5f ? 1.0f : 0.0f;
    if (p.kind == ParamKind::Int) return std::round(value);
    return value;
}

}  // namespace hsynth
