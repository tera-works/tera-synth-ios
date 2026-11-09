#import "HSynthEngine.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/MIDINetworkSession.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "audio_out.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/gm_bank.h"
#include "hsynth/smf.h"
#include "hsynth/host_utils.h"
#include "hsynth/midi_learn.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"
#include "hsynth/preset_index.h"
#include "hsynth/wav.h"
#include "midi_in.h"
#include "midi_out.h"
#include "remote_link.h"

using namespace hsynth;

namespace {
NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::string str(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

// knob position <-> value (same mapping as ParamMeta on Android)
float fromNormalized(const ParamInfo& p, float t) {
    t = std::fmin(1.0f, std::fmax(0.0f, t));
    float v = (p.curve == ParamCurve::Exp && p.min > 0.0f) ? p.min * std::pow(p.max / p.min, t) : p.min + t * (p.max - p.min);
    if (p.kind != ParamKind::Float) v = std::round(v);
    return v;
}
float toNormalized(const ParamInfo& p, float v) {
    if (p.max <= p.min) return 0.0f;
    v = std::fmin(p.max, std::fmax(p.min, v));
    return (p.curve == ParamCurve::Exp && p.min > 0.0f) ? std::log(v / p.min) / std::log(p.max / p.min) : (v - p.min) / (p.max - p.min);
}
}  // namespace

@implementation HSynthEngine {
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<AudioOut> _audio;
    std::unique_ptr<MidiIn> _midi;
    PresetIndex _index;
    std::string _factoryDir, _userDir;
    NSInteger _current;
    NSString* _currentName;
    BOOL _input;
    std::string _libraryDir;              // Documents/library (factory -> bundle link + user samples)
    std::string _songLoaded[kNumParts];   // preset file per part (no reload between songs)
    std::vector<SongPart> _songParts;
    std::unique_ptr<SongProgramLoader> _programs;  // mid-song program changes (stopped before the engine goes)
    std::vector<std::pair<double, double>> _tempos;
    NSString* _songName;
    NSArray<NSString*>* _lastWarnings;   // preset load warnings / song report (autotest)
    MidiLearn _learn;
    std::unique_ptr<MidiOut> _midiOut;
    int _editPart;
    NSString* _recordPath;
    std::vector<float> _takeL, _takeR;    // the last sampling take
    int _takeRate;
    std::unique_ptr<RemoteLink> _remote;  // SysEx remote control (docs/SYSEX.md)
    std::atomic<int> _remoteSongs;
}

- (instancetype)initWithFactoryDir:(NSString*)factoryDir userDir:(NSString*)userDir {
    if ((self = [super init])) {
        _factoryDir = str(factoryDir);
        _userDir = str(userDir);
        NSFileManager* fm = [NSFileManager defaultManager];
        for (NSString* d in @[@"presets", @"library", @"midi"])
            [fm createDirectoryAtPath:[userDir stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
        // One library root for every preset: Documents/library/factory points into the app bundle (its path
        // changes with each install, so the link is renewed at start); the user's samples sit next to it.
        _libraryDir = _userDir + "/library";
        NSString* link = ns(_libraryDir + "/factory");
        [fm removeItemAtPath:link error:nil];
        [fm createSymbolicLinkAtPath:link withDestinationPath:ns(_factoryDir + "/library/factory") error:nil];
        _songName = @"";
        _engine = std::make_unique<Engine>();
        _engine->prepare(48000.0f);
        _engine->setMaxVoices(128);
        _engine->setRenderThreads(0);  // auto: helpers on the performance cores
        _audio = std::make_unique<AudioOut>(*_engine);
        _midi = std::make_unique<MidiIn>(*_engine);
        _midi->setLearn(&_learn);
        std::string learnErr;
        _learn.load(_userDir + "/midi_learn.json", learnErr);  // missing file: no mappings
        _midiOut = std::make_unique<MidiOut>();
        _editPart = 0;
        _current = -1;
        _currentName = @"Init";
        [self rescanPresets];
        [self startRemote];
    }
    return self;
}

- (void)startRemote {
    // a PC with rtpMIDI (Windows) or Audio MIDI Setup (Mac) can connect over Wi-Fi
    MIDINetworkSession* net = [MIDINetworkSession defaultSession];
    net.enabled = YES;
    net.connectionPolicy = MIDINetworkConnectionPolicy_Anyone;
    _remoteSongs.store(0);
    RemoteControl::Config cfg;
    cfg.deviceName = "HSynth iOS";
    cfg.version = str([[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"");
    cfg.libraryRoot = _libraryDir;
    cfg.presetDirs = {_factoryDir + "/presets", _userDir + "/presets"};
    cfg.userPresetDir = _userDir + "/presets";
    cfg.midiDir = _userDir + "/midi";
    cfg.learn = &_learn;
    RemoteControl::Hooks hooks;
    MidiIn* midi = _midi.get();
    hooks.setMultiTimbral = [midi](bool on) { midi->setMultiTimbral(on); };
    hooks.setMpe = [midi](bool on) { midi->setMpe(on); };
    __weak HSynthEngine* weakSelf = self;
    hooks.loadSong = [weakSelf](const std::string& path, Json& info) -> std::string {
        HSynthEngine* me = weakSelf;
        if (!me) return "gone";
        __block NSString* err = nil;
        dispatch_sync(dispatch_get_main_queue(), ^{ err = [me loadSong:ns(path)]; });  // the player state is main-thread
        if (err) return str(err);
        info.set("name", str(me.songName));
        info.set("length", me.songLength);
        me->_remoteSongs.fetch_add(1);
        return "";
    };
    hooks.presetLoaded = [weakSelf](int part, const std::string& name) {
        NSString* n = ns(name);
        dispatch_async(dispatch_get_main_queue(), ^{
            HSynthEngine* me = weakSelf;
            if (me && part <= 0) me->_currentName = n;
        });
    };
    _remote = std::make_unique<RemoteLink>(*_engine, *_midi, cfg, hooks);
}

- (NSInteger)remoteChanges { return _remote ? (NSInteger)_remote->control().changeCount() : 0; }
- (NSInteger)remoteSongCount { return _remoteSongs.load(); }
- (NSString*)remotePort { return _remote ? ns(_remote->replyPort()) : @""; }

- (void)configureSession:(BOOL)input {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    NSError* err = nil;
    if (input)
        [s setCategory:AVAudioSessionCategoryPlayAndRecord
                  mode:AVAudioSessionModeMeasurement
               options:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP
                 error:&err];
    else
        [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err];
    [s setPreferredIOBufferDuration:0.005 error:&err];  // ~256 frames at 48 kHz
    [s setPreferredSampleRate:48000 error:&err];
    [s setActive:YES error:&err];
}

- (BOOL)startAudio {
    [self configureSession:_input];
    std::string err;
    const bool ok = _audio->start(-1, 48000, 256, false, _input ? -1 : -2, err);
    if (!ok) NSLog(@"HSynth audio: %s", err.c_str());
    [self refreshMidi];
    return ok;
}

- (void)stopAudio { _audio->stop(); }

- (void)setAudioInput:(BOOL)on {
    if (on) {
        [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!granted) return;
                self->_input = YES;
                self->_audio->stop();
                [self startAudio];
            });
        }];
    } else {
        _input = NO;
        _audio->stop();
        [self startAudio];
    }
}

// ---------------------------------------------------------------- playing

- (void)noteOn:(int)key velocity:(float)velocity {
    _engine->sendEvent(Event::noteOn((uint32_t)(key + 1), key, velocity));
    if (_midiOut->isOpen())
        _midiOut->send(0x90, (uint8_t)std::max(0, std::min(127, key)), (uint8_t)std::max(1, std::min(127, (int)std::lround(velocity * 127.0f))));
}
- (void)noteOff:(int)key {
    _engine->sendEvent(Event::noteOff((uint32_t)(key + 1), key, 0.0f));
    if (_midiOut->isOpen()) _midiOut->send(0x80, (uint8_t)std::max(0, std::min(127, key)), 64);
}
- (void)pitchBend:(float)value { _engine->sendEvent(Event::control(EventType::PitchBend, value)); }
- (void)modWheel:(float)value { _engine->sendEvent(Event::control(EventType::ModWheel, value)); }
- (void)sustain:(BOOL)on { _engine->sendEvent(Event::control(EventType::Sustain, on ? 1.0f : 0.0f)); }
- (void)allSoundOff { _engine->sendEvent(Event::control(EventType::AllSoundOff, 0.0f)); }

// ---------------------------------------------------------------- presets

- (void)rescanPresets {
    _index.setFolders({{_factoryDir + "/presets", false}, {_userDir + "/presets", true}});
    _index.rescan();
}

- (NSArray<NSString*>*)presetNames {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& p : _index.all()) [a addObject:ns(p.name)];
    return a;
}

- (NSArray<NSString*>*)presetCategories {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& p : _index.all()) [a addObject:ns(p.category.empty() ? (p.user ? "User" : "Factory") : p.category)];
    return a;
}

- (NSInteger)currentPreset { return _current; }
- (NSString*)currentPresetName { return _currentName; }

- (BOOL)loadPreset:(NSInteger)index {
    const auto& all = _index.all();
    if (index < 0 || index >= (NSInteger)all.size()) return NO;
    Preset p;
    std::string err;
    if (!loadPresetFile(all[(size_t)index].file, p, err)) {
        NSLog(@"HSynth preset: %s", err.c_str());
        return NO;
    }
    std::vector<std::string> warnings;
    applyPreset(*_engine, p, _libraryDir, warnings);
    _lastWarnings = [self strings:warnings];
    _engine->setMultiTimbral(false);  // a single preset: leave the song's multi mode
    _midi->setMultiTimbral(false);
    self.editPart = 0;
    _engine->collectGarbage();
    _current = index;
    _currentName = ns(p.name);
    return YES;
}

- (BOOL)savePresetNamed:(NSString*)name {
    Preset p = capturePreset(*_engine, str(name), _libraryDir);
    p.category = "User";
    p.author = "user";
    std::string safe;
    for (char c : str(name)) safe.push_back(std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' ? c : '_');
    std::string err;
    const bool ok = savePresetFile(_userDir + "/presets/" + (safe.empty() ? "untitled" : safe) + ".json", p, err);
    if (ok) [self rescanPresets];
    return ok;
}

// ---------------------------------------------------------------- parameters

- (int)paramId:(NSString*)name {
    const std::string n = str(name);
    const auto& t = paramTable();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].name == n) return (int)i;
    return -1;
}
- (NSInteger)paramCount { return (NSInteger)paramTable().size(); }
- (NSString*)paramLabel:(int)param { return param >= 0 ? ns(paramTable()[(size_t)param].label) : @""; }
- (NSString*)paramGroup:(int)param { return param >= 0 ? ns(paramTable()[(size_t)param].group) : @""; }
- (int)paramKind:(int)param { return param >= 0 ? (int)paramTable()[(size_t)param].kind : 0; }
- (NSArray<NSString*>*)paramOptions:(int)param {
    NSMutableArray* a = [NSMutableArray array];
    if (param >= 0) for (const auto& o : paramTable()[(size_t)param].options) [a addObject:ns(o)];
    return a;
}
// part parameters go to the edit part, the others are global (Engine::setPartParam)
- (float)paramValue:(int)param { return param >= 0 ? _engine->getPartParam(_editPart, param) : 0.0f; }
- (void)setParam:(int)param value:(float)value { if (param >= 0) _engine->setPartParam(_editPart, param, value); }
- (float)paramNormalized:(int)param {
    return param >= 0 ? toNormalized(paramTable()[(size_t)param], _engine->getPartParam(_editPart, param)) : 0.0f;
}
- (void)setParam:(int)param normalized:(float)t {
    if (param >= 0) _engine->setPartParam(_editPart, param, fromNormalized(paramTable()[(size_t)param], t));
}
- (float)paramDefault:(int)param { return param >= 0 ? paramTable()[(size_t)param].def : 0.0f; }

- (NSString*)formatParam:(int)param {
    if (param < 0) return @"";
    const ParamInfo& p = paramTable()[(size_t)param];
    const float v = _engine->getPartParam(_editPart, param);
    char buf[64];
    switch (p.kind) {
        case ParamKind::Enum: {
            const int i = (int)std::lround(v);
            return i >= 0 && i < (int)p.options.size() ? ns(p.options[(size_t)i]) : ns(std::to_string(i));
        }
        case ParamKind::Bool: return v > 0.5f ? @"on" : @"off";
        case ParamKind::Int: snprintf(buf, sizeof buf, "%d %s", (int)std::lround(v), p.unit.c_str()); break;
        default:
            if (p.unit == "Hz" && v >= 1000.0f) snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.0f);
            else if (p.unit == "s" && v < 1.0f) snprintf(buf, sizeof buf, "%.0f ms", v * 1000.0f);
            else if (std::fabs(v) >= 100.0f) snprintf(buf, sizeof buf, "%.0f %s", v, p.unit.c_str());
            else if (std::fabs(v) >= 10.0f) snprintf(buf, sizeof buf, "%.1f %s", v, p.unit.c_str());
            else snprintf(buf, sizeof buf, "%.3f %s", v, p.unit.c_str());
    }
    return [ns(buf) stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
}

// ---------------------------------------------------------------- songs

- (void)dealloc {
    _remote.reset();  // its worker uses the engine and the MIDI ports
    if (_programs) _programs->stop();  // its thread uses the engine
}

- (NSString*)libraryDir { return ns(_libraryDir); }
- (NSString*)midiDir { return ns(_userDir + "/midi"); }

- (nullable NSString*)loadSong:(NSString*)path {
    auto song = std::make_unique<Song>();
    std::string err;
    if (!loadSmf(str(path), *song, err)) return ns(err);
    if (_programs) _programs->stop();  // before prepareSong: it must not stage into the parts being loaded
    [self rescanPresets];      // samples / presets copied in since the last scan
    GmBank bank;
    bank.build(_index.all());  // factory + user presets (GM test, SonicCell, ...)
    _engine->songPlay(false);
    std::vector<std::string> report;
    _songParts.clear();
    prepareSong(*_engine, *song, bank, _libraryDir, _songLoaded, report, &_songParts);
    _lastWarnings = [self strings:report];
    _midi->setMultiTimbral(true);
    _tempos.clear();
    for (const auto& t : song->tempos) _tempos.push_back({t.time, t.bpm});
    if (_tempos.empty()) _tempos.push_back({0.0, song->initialTempo});
    _songName = ns(song->name);
    if (!_programs) _programs = std::make_unique<SongProgramLoader>();
    _programs->start(*_engine, *song, bank, _libraryDir);
    _engine->setSong(std::move(song));
    _engine->songSeek(0.0);
    return nil;
}
- (NSString*)songName { return _songName; }
- (double)songLength { return _engine->songLength(); }
- (double)songPosition { return _engine->songPosition(); }
- (BOOL)songPlaying { return _engine->songPlaying(); }
- (NSInteger)songEndCount { return (NSInteger)_engine->songEndCount(); }
- (void)songPlay:(BOOL)on { _engine->songPlay(on); }
- (void)songSeek:(double)seconds { _engine->songSeek(seconds); }
- (double)songTempoAt:(double)t {
    double b = _tempos.empty() ? 120.0 : _tempos.front().second;
    for (const auto& x : _tempos) {
        if (x.first > t) break;
        b = x.second;
    }
    return b;
}
- (const SongPart*)songPart:(int)ch {
    for (const auto& p : _songParts)
        if (p.channel == ch) return &p;
    return nullptr;
}
- (BOOL)songChannelUsed:(int)ch { return [self songPart:ch] != nullptr; }
- (NSString*)songChannelPreset:(int)ch {
    const SongPart* p = [self songPart:ch];
    if (!p) return @"";
    const std::string now = _programs ? _programs->presetName(ch) : std::string();  // after a program change
    return ns(now.empty() ? p->presetName : now);
}
- (NSString*)songChannelGm:(int)ch {
    const SongPart* p = [self songPart:ch];
    return p ? (p->drum ? @"Drums" : ns(p->gmName)) : @"";
}
- (NSArray<NSNumber*>*)partMeters {
    Engine::PartMeter m[kNumParts];
    _engine->partMeters(m);
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:kNumParts * 9];
    for (int k = 0; k < kNumParts; ++k) {
        const float v[9] = {m[k].level, m[k].volume, m[k].expression, m[k].pan, m[k].bend,
                            m[k].mod, m[k].reverb, m[k].chorus, (float)m[k].voices};
        for (float x : v) [a addObject:@(x)];
    }
    return a;
}
- (NSArray<NSNumber*>*)noteMasks {
    Engine::PartMeter m[kNumParts];
    _engine->partMeters(m);
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:kNumParts * 4];
    for (int k = 0; k < kNumParts; ++k)
        for (int w = 0; w < 4; ++w) [a addObject:@(m[k].notes[w])];
    return a;
}
- (void)setPartMix:(int)part id:(int)mixId value:(float)value { _engine->setPartMix(part, mixId, value); }
- (float)partMix:(int)part id:(int)mixId { return _engine->getPartMix(part, mixId); }

// ---------------------------------------------------------------- status

- (int)activeVoices { return _engine->stats().activeVoices; }
- (float)cpuLoad { return _engine->stats().cpuLoad; }

- (NSArray<NSString*>*)midiInputs {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& n : _midi->openPorts()) [a addObject:ns(n)];
    return a;
}
- (void)refreshMidi { _midi->openAll(); }

- (BOOL)muted { return _audio->muted(); }
- (void)setMuted:(BOOL)m { _audio->setMuted(m); }
- (NSString*)audioDevice {
    const AudioOutStats s = _audio->stats();
    AVAudioSession* a = [AVAudioSession sharedInstance];
    return [NSString stringWithFormat:@"%s, %d Hz, period %d x %d (session %.1f ms, output latency %.1f ms)",
                                      s.backend.c_str(), s.sampleRate, s.periodFrames, s.periods,
                                      a.IOBufferDuration * 1000.0, a.outputLatency * 1000.0];
}
- (int)peakVoices { return _engine->stats().peakVoices; }
- (float)cpuLoadPeak { return _engine->stats().cpuLoadPeak; }
- (float)callbackLoadMax { return _audio->stats().callbackLoadMax; }
- (float)callbackUsAvg { return _audio->stats().callbackUsAvg; }
- (int)lateCallbacks { return _audio->stats().lateCallbacks; }
- (int)callbackFrames { return _audio->stats().lastCallbackFrames; }
- (NSArray<NSString*>*)strings:(const std::vector<std::string>&)v {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& x : v) [a addObject:ns(x)];
    return a;
}
- (NSArray<NSString*>*)lastWarnings { return _lastWarnings ?: @[]; }

// ---------------------------------------------------------------- parts

- (BOOL)multiTimbral { return _engine->multiTimbral(); }
- (void)setMultiTimbral:(BOOL)on {
    _engine->setMultiTimbral(on);
    _midi->setMultiTimbral(on);
    if (!on) self.editPart = 0;
}
- (int)editPart { return _editPart; }
- (void)setEditPart:(int)part {
    _editPart = std::max(0, std::min(kNumParts - 1, part));
    _engine->selectPart(_editPart);
}

- (BOOL)loadPreset:(NSInteger)index intoPart:(int)part {
    const auto& all = _index.all();
    if (index < 0 || index >= (NSInteger)all.size() || part < 0 || part >= kNumParts) return NO;
    Preset p;
    std::string err;
    if (!loadPresetFile(all[(size_t)index].file, p, err)) {
        _lastWarnings = @[ ns(err) ];
        return NO;
    }
    std::vector<std::string> warnings;
    applyPartPreset(*_engine, part, p, _libraryDir, warnings);
    _songLoaded[part].clear();  // the part no longer holds a song's sound
    _lastWarnings = [self strings:warnings];
    _engine->collectGarbage();
    return YES;
}

- (NSArray<NSNumber*>*)allParams {
    std::vector<float> v((size_t)P::kNumParams);
    _engine->getPartParams(_editPart, v.data());
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:v.size()];
    for (float x : v) [a addObject:@(x)];
    return a;
}
- (void)setAllParams:(NSArray<NSNumber*>*)values {
    if ((int)values.count != P::kNumParams) return;
    for (int i = 0; i < P::kNumParams; ++i) _engine->setPartParam(_editPart, i, values[(NSUInteger)i].floatValue);
}

// ---------------------------------------------------------------- samples

- (nullable NSString*)loadSamples:(NSString*)path layer:(int)layer {
    if (layer < 0 || layer >= kNumLayers) return @"no such layer";
    std::string err;
    std::vector<std::string> warnings;
    const std::string ext = [[path pathExtension] lowercaseString].UTF8String;
    const bool ok = ext == "wav" && _engine->getPartParam(_editPart, P::layer(layer, P::LType)) == (float)LayerType::WT
                        ? loadPartWavetable(*_engine, _editPart, layer, str(path), err)
                        : loadPartSamples(*_engine, _editPart, layer, str(path), err, warnings);
    _lastWarnings = [self strings:warnings];
    if (!ok) return ns(err);
    // a PCM layer to hear it
    if (_engine->getPartParam(_editPart, P::layer(layer, P::LType)) != (float)LayerType::WT)
        _engine->setPartParam(_editPart, P::layer(layer, P::LType), (float)LayerType::PCM);
    return nil;
}
- (void)unloadSamples:(int)layer {
    if (layer < 0 || layer >= kNumLayers) return;
    _engine->setPartSampleSet(_editPart, layer, nullptr);
    _engine->collectGarbage();
}
- (NSString*)sampleSetName:(int)layer { return ns(_engine->partSampleSetName(_editPart, layer)); }

// ---------------------------------------------------------------- settings

- (int)maxVoices { return _engine->maxVoices(); }
- (void)setMaxVoices:(int)n { _engine->setMaxVoices(n); }
- (void)setQuality:(int)quality { _engine->setQuality((Quality)std::max(0, std::min(2, quality))); }
- (BOOL)audioInputOn { return _input; }

// ---------------------------------------------------------------- recording

- (void)startRecording {
    NSString* dir = ns(_userDir + "/recordings");
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter* f = [[NSDateFormatter alloc] init];
    f.dateFormat = @"yyyyMMdd-HHmmss";
    _recordPath = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"HSynth %@.wav", [f stringFromDate:[NSDate date]]]];
    std::string err;
    if (!_audio->startFileRecording(str(_recordPath), err)) {
        NSLog(@"HSynth recording: %s", err.c_str());
        _recordPath = nil;
    }
}
- (double)recordSeconds { return _audio->fileRecording() ? _audio->fileRecordSeconds() : -1.0; }
- (nullable NSString*)stopRecording {
    if (!_audio->fileRecording()) return nil;
    const double sec = _audio->stopFileRecording();
    NSString* path = _recordPath;
    _recordPath = nil;
    if (sec <= 0.0 && path) {
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
        return nil;
    }
    return path;
}

// ---------------------------------------------------------------- transport / diagrams / scenes

- (BOOL)transportRunning { return _engine->transportPlaying(); }
- (void)setTransportRunning:(BOOL)on { _engine->setTransport(on); }
- (int)seqStep { return _engine->seqPosition(_editPart); }

- (NSArray<NSNumber*>*)fmAlgorithm:(int)index {
    const FmAlgorithm& a = hsynth::fmAlgorithm(std::max(0, std::min(kNumFmAlgorithms - 1, index)));
    NSMutableArray* r = [NSMutableArray arrayWithObjects:@(a.numOps), @(a.carriers), @(a.feedbackOp), nil];
    for (int i = 0; i < P::kFmOps; ++i) [r addObject:@(a.modulators[i])];
    return r;
}

- (BOOL)saveScene:(NSString*)path name:(NSString*)name partNames:(NSArray<NSString*>*)names {
    MultiPreset m = captureMulti(*_engine, str(name), _libraryDir);
    for (int k = 0; k < kNumParts && k < (int)names.count; ++k) m.parts[k].name = str(names[(NSUInteger)k]);
    std::string err;
    if (!saveMultiFile(str(path), m, err)) {
        NSLog(@"HSynth scene: %s", err.c_str());
        return NO;
    }
    return YES;
}

- (BOOL)saveSoundFile:(NSString*)path name:(NSString*)name {
    Preset p = capturePreset(*_engine, str(name), _libraryDir);
    p.category = "Set List";
    p.author = "user";
    std::string err;
    if (!hsynth::savePresetFile(str(path), p, err)) {
        NSLog(@"HSynth sound: %s", err.c_str());
        return NO;
    }
    return YES;
}

- (nullable NSString*)loadSoundFile:(NSString*)path {
    Preset p;
    std::string err;
    if (!hsynth::loadPresetFile(str(path), p, err)) {
        _lastWarnings = @[ ns(err) ];
        return nil;
    }
    std::vector<std::string> warnings;
    applyPreset(*_engine, p, _libraryDir, warnings);
    _lastWarnings = [self strings:warnings];
    _engine->setMultiTimbral(false);
    _midi->setMultiTimbral(false);
    self.editPart = 0;
    _songLoaded[0].clear();
    _engine->collectGarbage();
    _current = -1;
    _currentName = ns(p.name);
    return _currentName;
}

// ---------------------------------------------------------------- phrase recorder

- (void)phraseRecord { _engine->phraseRecord(); }
- (void)phrasePlay { _engine->phrasePlay(); }
- (void)phraseStop { _engine->phraseStop(); }
- (void)phraseClear { _engine->phraseClear(); }
- (void)phraseUndo { _engine->phraseUndo(); }
- (int)phraseBars { return _engine->phraseBars(); }
- (void)setPhraseBars:(int)bars { _engine->setPhraseBars(bars); }
- (int)phraseState { return (int)_engine->phraseState(); }
- (double)phraseLength { return _engine->phraseLength(); }
- (double)phrasePosition { return _engine->phrasePosition(); }
- (int)phraseEventCount { return _engine->phraseEventCount(); }

// a copy of the phrase from the audio thread (it answers within a block)
- (BOOL)phraseSnapshot:(std::vector<Engine::PhraseEvent>&)events length:(double&)len {
    _engine->requestPhraseSnapshot();
    for (int i = 0; i < 100; ++i) {
        if (_engine->takePhraseSnapshot(events, len)) return YES;
        [NSThread sleepForTimeInterval:0.005];
    }
    return NO;
}

- (BOOL)phraseQuantize:(double)grid {
    std::vector<Engine::PhraseEvent> ev;
    double len = 0.0;
    if (![self phraseSnapshot:ev length:len] || len <= 0.0) return NO;
    quantizePhrase(ev, len, grid);
    _engine->replacePhrase(ev, len);
    return YES;
}

- (nullable NSString*)phraseExport:(NSString*)path repeats:(int)repeats {
    std::vector<Engine::PhraseEvent> ev;
    double len = 0.0;
    if (![self phraseSnapshot:ev length:len]) return @"the audio is not running";
    std::string err;
    const double bpm = _engine->getParam(P::fxChain(P::FxTempo));
    if (!writePhraseSmf(str(path), ev, len, bpm, repeats, err)) return ns(err);
    return nil;
}

// ---------------------------------------------------------------- sampling

- (void)startSampling:(BOOL)fromInput { _audio->startRecording(30.0, fromInput); }
- (double)samplingSeconds { return _audio->recording() ? _audio->recordedSeconds() : -1.0; }
- (BOOL)stopSampling {
    std::vector<float> inter;
    const size_t frames = _audio->stopRecording(inter);
    if (frames == 0) return NO;
    _takeL.resize(frames);
    _takeR.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        _takeL[i] = inter[i * 2];
        _takeR[i] = inter[i * 2 + 1];
    }
    _takeRate = _audio->stats().sampleRate > 0 ? _audio->stats().sampleRate : 48000;
    return YES;
}
- (long long)takeFrames { return (long long)_takeL.size(); }
- (int)takeRate { return _takeRate > 0 ? _takeRate : 48000; }
- (NSArray<NSNumber*>*)takeWaveform:(int)points {
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:(NSUInteger)std::max(0, points)];
    const size_t n = _takeL.size();
    for (int p = 0; p < points; ++p) {
        const size_t a0 = n * (size_t)p / (size_t)points, a1 = std::max(a0 + 1, n * (size_t)(p + 1) / (size_t)points);
        float pk = 0.0f;
        for (size_t i = a0; i < a1 && i < n; ++i) pk = std::max(pk, std::max(std::fabs(_takeL[i]), std::fabs(_takeR[i])));
        [a addObject:@(std::min(1.0f, pk))];
    }
    return a;
}
- (NSArray<NSNumber*>*)takeBounds {
    int64_t s = 0, e = 0;
    sampleBounds(_takeL, _takeR, -40.0f, s, e);
    return @[ @(s), @(e) ];
}
- (nullable NSArray<NSNumber*>*)takeFindLoopFrom:(long long)start to:(long long)end {
    int64_t ls = 0, le = 0;
    if (!findLoop(_takeL, _takeR, start, end, ls, le)) return nil;
    return @[ @(ls), @(le) ];
}
- (nullable NSString*)saveTake:(NSString*)name start:(long long)start end:(long long)end normalize:(BOOL)normalize
                     loopStart:(long long)loopStart loopEnd:(long long)loopEnd rootKey:(int)rootKey layer:(int)layer {
    if (_takeL.empty()) return @"nothing recorded";
    std::string safe;
    for (char c : str(name)) safe.push_back(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' ? c : '_');
    if (safe.empty()) safe = "sample";
    const float gain = normalize ? normalizeGain(_takeL, _takeR, start, end, -1.0f) : 1.0f;
    std::string sfz, err;
    if (!saveSampledInstrument(_libraryDir + "/user/sampled", safe, _takeL, _takeR, [self takeRate], start, end, gain,
                               loopEnd > loopStart ? loopStart : -1, loopEnd, rootKey, sfz, err))
        return ns(err);
    return [self loadSamples:ns(sfz) layer:layer];
}

- (nullable NSArray<NSString*>*)loadScene:(NSString*)path {
    MultiPreset m;
    std::string err;
    if (!loadMultiFile(str(path), m, err)) {
        _lastWarnings = @[ ns(err) ];
        return nil;
    }
    std::vector<std::string> warnings;
    applyMulti(*_engine, m, _libraryDir, warnings);
    _midi->setMultiTimbral(true);
    for (auto& s : _songLoaded) s.clear();  // the parts hold the scene's sounds now
    _editPart = _engine->selectedPart();
    _lastWarnings = [self strings:warnings];
    NSMutableArray* r = [NSMutableArray arrayWithObject:ns(m.name)];
    for (int k = 0; k < kNumParts; ++k) [r addObject:ns(m.parts[k].name)];
    return r;
}

// ---------------------------------------------------------------- MIDI learn / out

- (void)learnArm:(int)param { _learn.arm(param, _editPart); }
- (void)learnCancel { _learn.cancel(); }
- (int)learnArmed { return _learn.armedParam(); }
- (void)learnRemove:(int)param {
    _learn.removeParam(param);
    [self learnSave];
}
- (NSString*)learnDescribe:(int)param { return ns(_learn.describe(param)); }
- (void)learnSave {
    std::string err;
    if (!_learn.save(_userDir + "/midi_learn.json", err)) NSLog(@"HSynth learn: %s", err.c_str());
}

- (NSArray<NSString*>*)midiOutputs {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& n : MidiOut::ports()) [a addObject:ns(n)];
    return a;
}
- (NSString*)midiOutput { return _midiOut->isOpen() ? ns(_midiOut->name()) : @""; }
- (BOOL)openMidiOutput:(NSString*)name {
    std::string err;
    const bool ok = _midiOut->open(str(name), err);
    if (!ok) NSLog(@"HSynth MIDI out: %s", err.c_str());
    return ok;
}
- (void)resetPeaks {
    _engine->resetPeakStats();
    _audio->resetPeaks();
}

@end
