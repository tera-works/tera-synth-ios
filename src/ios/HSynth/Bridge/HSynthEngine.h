// Objective-C face of the C++ engine for the SwiftUI app: audio (miniaudio /
// CoreAudio), MIDI in (CoreMIDI), presets and parameters by name.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface HSynthEngine : NSObject

/// factoryDir holds "presets" and "library" (bundled); userDir is writable (Documents).
- (instancetype)initWithFactoryDir:(NSString*)factoryDir userDir:(NSString*)userDir;

- (BOOL)startAudio;
- (void)stopAudio;
- (void)setAudioInput:(BOOL)on;   // microphone for the vocoder / looper (asks for permission)

// playing
- (void)noteOn:(int)key velocity:(float)velocity NS_SWIFT_NAME(noteOn(_:velocity:));
- (void)noteOff:(int)key;
- (void)pitchBend:(float)value;   // -1..1
- (void)modWheel:(float)value;    // 0..1
- (void)sustain:(BOOL)on;
- (void)allSoundOff;

// presets
@property(nonatomic, readonly) NSArray<NSString*>* presetNames;
@property(nonatomic, readonly) NSArray<NSString*>* presetCategories;
@property(nonatomic, readonly) NSInteger currentPreset;
@property(nonatomic, readonly) NSString* currentPresetName;
- (void)rescanPresets;
- (BOOL)loadPreset:(NSInteger)index;
- (BOOL)savePresetNamed:(NSString*)name;

// parameters (names as in presets: "filter.cutoff", "amp.attack", ...)
- (int)paramId:(NSString*)name;
- (NSString*)paramLabel:(int)param;
- (NSString*)paramGroup:(int)param;
- (NSInteger)paramCount;
/// 0 float, 1 int, 2 enum, 3 bool
- (int)paramKind:(int)param;
- (NSArray<NSString*>*)paramOptions:(int)param;
- (float)paramValue:(int)param;
- (void)setParam:(int)param value:(float)value;
/// knob position 0..1 (exponential parameters map like the knobs on the other hosts)
- (float)paramNormalized:(int)param;
- (void)setParam:(int)param normalized:(float)t;
- (float)paramDefault:(int)param;
- (NSString*)formatParam:(int)param;

// songs (Standard MIDI Files): GM sounds from every preset (factory + user, e.g. GM test / SonicCell)
/// Loads the file and the sounds of its channels into the 16 parts (multi mode). nil on success, else the error.
- (nullable NSString*)loadSong:(NSString*)path;
@property(nonatomic, readonly) NSString* songName;
@property(nonatomic, readonly) double songLength;
@property(nonatomic, readonly) double songPosition;
@property(nonatomic, readonly) BOOL songPlaying;
/// Counts natural song ends (the player advances the playlist when it changes).
@property(nonatomic, readonly) NSInteger songEndCount;
- (void)songPlay:(BOOL)on;
- (void)songSeek:(double)seconds;
/// Tempo at a time (tempo map of the loaded song).
- (double)songTempoAt:(double)seconds;
/// Channel (= part) 0..15 used by the song; preset and GM names of its sound.
- (BOOL)songChannelUsed:(int)ch;
- (NSString*)songChannelPreset:(int)ch;
- (NSString*)songChannelGm:(int)ch;
/// Per part meters, packed: [level, volume, expression, pan, bend, mod, reverb, chorus, voices] x 16 (level resets on read).
- (NSArray<NSNumber*>*)partMeters;
/// Keys sounding per part: 16 x 4 words of 32 bits.
- (NSArray<NSNumber*>*)noteMasks;
/// Mix of a part: 0 volume, 1 pan, 2 mute.
- (void)setPartMix:(int)part id:(int)mixId value:(float)value;
- (float)partMix:(int)part id:(int)mixId;

// folders
/// Documents/library: user samples ("gm", "soniccell", ...); "factory" links to the bundle
@property(nonatomic, readonly) NSString* libraryDir;
/// Documents/midi
@property(nonatomic, readonly) NSString* midiDir;

// status
@property(nonatomic, readonly) int activeVoices;
@property(nonatomic, readonly) float cpuLoad;
@property(nonatomic, readonly) NSArray<NSString*>* midiInputs;
- (void)refreshMidi;
/// MIDI input state for the user: port, sources, messages received, the last message.
@property(nonatomic, readonly) NSString* midiStatus;
/// Receive with the MIDI 2.0 protocol (UMP) instead of MIDI 1.0 (remembered).
@property(nonatomic) BOOL umpInput;

// SysEx remote control (docs/SYSEX.md): a controller / PC reads and changes everything over MIDI SysEx
@property(nonatomic, readonly) NSInteger remoteChanges;    // bumps when a remote request changed something
@property(nonatomic, readonly) NSInteger remoteSongCount;  // songs loaded remotely
@property(nonatomic, readonly) NSString* remotePort;       // where replies go ("" until the first request)

// audio timing (autotest): peaks since resetPeaks
@property(nonatomic) BOOL muted;
@property(nonatomic, readonly) NSString* audioDevice;   // "backend, rate Hz, period x periods"
@property(nonatomic, readonly) int peakVoices;
@property(nonatomic, readonly) float cpuLoadPeak;       // engine process() time / block time
@property(nonatomic, readonly) float callbackLoadMax;   // whole callback time / callback period
@property(nonatomic, readonly) float callbackUsAvg;
@property(nonatomic, readonly) int lateCallbacks;       // callback came > 2.5 periods late (likely audible)
@property(nonatomic, readonly) int callbackFrames;
- (void)resetPeaks;
/// Warnings of the last loadPreset (missing samples, ...) or the report of the last loadSong.
@property(nonatomic, readonly) NSArray<NSString*>* lastWarnings;

// parts (multi mode): the edit part is what the parameters above and loadPreset address
@property(nonatomic) BOOL multiTimbral;
@property(nonatomic) int editPart;                       // 0..15
/// Loads a preset into a part (multi mode). The part's sound name is kept by the caller.
- (BOOL)loadPreset:(NSInteger)index intoPart:(int)part NS_SWIFT_NAME(loadPreset(_:intoPart:));
/// All parameters of the edit part (undo / compare snapshots).
- (NSArray<NSNumber*>*)allParams;
- (void)setAllParams:(NSArray<NSNumber*>*)values;

// samples: SFZ / WAV / FLAC into a layer (0..2) of the edit part; nil on success, else the error
- (nullable NSString*)loadSamples:(NSString*)path layer:(int)layer NS_SWIFT_NAME(loadSamples(_:layer:));
- (void)unloadSamples:(int)layer;
- (NSString*)sampleSetName:(int)layer;

// settings
@property(nonatomic) int maxVoices;
- (void)setQuality:(int)quality;                         // 0 low, 1 normal, 2 high
@property(nonatomic, readonly) BOOL audioInputOn;

// recording (output after the effects, written to Documents/recordings while it runs, any length)
- (void)startRecording;
/// Seconds recorded so far, -1 when not recording.
@property(nonatomic, readonly) double recordSeconds;
/// Stops and writes a WAV; returns its path (nil on failure / nothing recorded).
- (nullable NSString*)stopRecording;

// MIDI learn (mappings saved in Documents/midi_learn.json)
- (void)learnArm:(int)param;
- (void)learnCancel;
@property(nonatomic, readonly) int learnArmed;            // -1 = not armed
- (void)learnRemove:(int)param;
- (NSString*)learnDescribe:(int)param;                    // "CC 74 ch 1" / ""
- (void)learnSave;

// MIDI out (the notes of the on-screen keyboard)
@property(nonatomic, readonly) NSArray<NSString*>* midiOutputs;
@property(nonatomic, readonly) NSString* midiOutput;      // "" = off
- (BOOL)openMidiOutput:(NSString*)name;                   // "" closes

// arpeggiator / sequencer transport (Run), sequencer step of the edit part (-1 = stopped)
@property(nonatomic) BOOL transportRunning;
@property(nonatomic, readonly) int seqStep;
/// FM algorithm for the diagram: [operators, carrier mask, feedback operator, modulator mask of op 1..8]
- (NSArray<NSNumber*>*)fmAlgorithm:(int)index;

// scenes (whole multis): the part names are kept in the file
- (BOOL)saveScene:(NSString*)path name:(NSString*)name partNames:(NSArray<NSString*>*)names
    NS_SWIFT_NAME(saveScene(_:name:partNames:));
/// [scene name, part 1 name, ..., part 16 name] or nil
- (nullable NSArray<NSString*>*)loadScene:(NSString*)path;

// setup files (engine setup.h): parts, effects, MIDI learn and app settings in one file,
// shared with the desktop and Android apps. host = JSON object of the app settings.
- (NSString*)setupJson:(NSString*)name partNames:(NSArray<NSString*>*)names host:(NSString*)host
    NS_SWIFT_NAME(setupJson(_:partNames:host:));
/// Applies a setup file's text: {"name", "state" (bool), "multi" (bool), "parts" [16 names],
/// "host" (JSON text)}, or nil (the error is in lastWarnings).
- (nullable NSDictionary*)applySetup:(NSString*)text;

// sound snapshots (set lists): the single-mode sound to / from a preset file anywhere
- (BOOL)saveSoundFile:(NSString*)path name:(NSString*)name NS_SWIFT_NAME(saveSoundFile(_:name:));
/// Loads it as the whole sound (single mode). The preset's name, or nil on failure.
- (nullable NSString*)loadSoundFile:(NSString*)path;

// phrase recorder: what is played live, as a loop (Engine::phraseRecord ...)
- (void)phraseRecord;
- (void)phrasePlay;
- (void)phraseStop;
- (void)phraseClear;
- (void)phraseUndo;
@property(nonatomic) int phraseBars;                      // 0 = the length is set by Play
@property(nonatomic, readonly) int phraseState;           // 0 empty, 1 recording, 2 playing, 3 overdub, 4 stopped
@property(nonatomic, readonly) double phraseLength;       // beats
@property(nonatomic, readonly) double phrasePosition;     // beats
@property(nonatomic, readonly) int phraseEventCount;
/// Moves the notes to the grid (beats, 0.25 = 1/16). NO if the audio is not running.
- (BOOL)phraseQuantize:(double)grid;
/// Writes the phrase as a MIDI file (repeated). nil on success, else the error.
- (nullable NSString*)phraseExport:(NSString*)path repeats:(int)repeats NS_SWIFT_NAME(phraseExport(_:repeats:));

// sampling: record the audio input (or the output) and make an instrument of it
- (void)startSampling:(BOOL)fromInput;                    // at most 30 s
@property(nonatomic, readonly) double samplingSeconds;    // -1 when not sampling
- (BOOL)stopSampling;                                     // keeps the take
@property(nonatomic, readonly) long long takeFrames;
@property(nonatomic, readonly) int takeRate;
/// Peak of each of `points` slices of the take (0..1).
- (NSArray<NSNumber*>*)takeWaveform:(int)points;
/// [start, end] in frames: where the sound is (within 40 dB of the peak).
- (NSArray<NSNumber*>*)takeBounds;
/// [loop start, loop end] in frames inside the range, or nil if no seamless loop was found.
- (nullable NSArray<NSNumber*>*)takeFindLoopFrom:(long long)start to:(long long)end NS_SWIFT_NAME(takeFindLoop(from:to:));
/// Saves the range as library/user/sampled/<name> (normalised if asked, loop if loopEnd > loopStart)
/// and loads it into a layer of the edit part. nil on success, else the error.
- (nullable NSString*)saveTake:(NSString*)name start:(long long)start end:(long long)end normalize:(BOOL)normalize
                     loopStart:(long long)loopStart loopEnd:(long long)loopEnd rootKey:(int)rootKey layer:(int)layer
    NS_SWIFT_NAME(saveTake(_:start:end:normalize:loopStart:loopEnd:rootKey:layer:));

@end

NS_ASSUME_NONNULL_END
