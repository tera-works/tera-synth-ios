// Helpers shared by hosts (Android app, offline tools, tests). Blocking:
// call from a loader / UI thread, never from the audio callback.
#pragma once

#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/preset.h"

namespace hsynth {

// Part 0 + global: apply parameters and load the preset's sample sets (paths are relative to
// libraryRoot unless absolute). Returns false only if nothing could be applied;
// non-fatal problems are appended to warnings.
bool applyPreset(Engine& engine, const Preset& preset, const std::string& libraryRoot,
                 std::vector<std::string>& warnings);

// Snapshot the engine state into a preset. Sample paths are made relative to
// libraryRoot when they are inside it.
Preset capturePreset(const Engine& engine, const std::string& name, const std::string& libraryRoot);

// ---- multi-timbral (see engine.h) ----
// Part parameters + sample sets of `preset` into part N (0..15). The effect
// rack, impulses, master volume and limiter of the preset are ignored.
bool applyPartPreset(Engine& engine, int part, const Preset& preset, const std::string& libraryRoot,
                     std::vector<std::string>& warnings);
// Loads a part preset's samples into a program staged for a mid-song program
// change (Engine::stagePartProgram); nothing is sent to the engine.
// Sampling helpers (a recording made on the device -> an instrument).
// Where the sound starts / ends: the first / last frame within thresholdDb of the peak.
void sampleBounds(const std::vector<float>& l, const std::vector<float>& r, float thresholdDb, int64_t& start, int64_t& end);
// Gain that brings the peak of [start, end) to targetDb (1 if silent).
float normalizeGain(const std::vector<float>& l, const std::vector<float>& r, int64_t start, int64_t end, float targetDb = -1.0f);
// A seamless loop inside [start, end): the end of the loop is near the end of the
// sound, its start where the waveform matches best (in the second half). false: none.
bool findLoop(const std::vector<float>& l, const std::vector<float>& r, int64_t start, int64_t end, int64_t& loopStart,
              int64_t& loopEnd);
// Writes <dir>/<name>/<name>.wav (16-bit, root key and loop in smpl) and <name>.sfz
// (one region over the keyboard), trimmed to [start, end) with gain and a 5 ms
// fade at both ends. loopStart < 0: no loop. sfzPath: the SFZ to load.
bool saveSampledInstrument(const std::string& dir, const std::string& name, const std::vector<float>& l,
                           const std::vector<float>& r, int sampleRate, int64_t start, int64_t end, float gain,
                           int64_t loopStart, int64_t loopEnd, int rootKey, std::string& sfzPath, std::string& error);

// Phrase recorder helpers (non-audio thread; Engine::takePhraseSnapshot / replacePhrase).
// Moves note-ons to the nearest grid line (beats, e.g. 0.25 = 1/16) by `strength`
// (0..1); each note-off moves with its note-on, so lengths stay.
void quantizePhrase(std::vector<Engine::PhraseEvent>& events, double lengthBeats, double grid, double strength = 1.0);
// Standard MIDI File (format 0, 480 ppq) of the phrase, `repeats` times at `bpm`.
// Events without a channel (on-screen keyboard) go to channel 1.
bool writePhraseSmf(const std::string& path, const std::vector<Engine::PhraseEvent>& events, double lengthBeats, double bpm,
                    int repeats, std::string& error);

std::unique_ptr<Engine::StagedProgram> loadStagedProgram(const Preset& preset, const std::string& libraryRoot,
                                                         std::vector<std::string>& warnings);
// Part N as a regular preset (global params / impulses are the current global ones).
Preset capturePartPreset(const Engine& engine, int part, const std::string& name, const std::string& libraryRoot);
// Switches the engine to multi-timbral mode and loads all 16 parts, their mix
// settings, the rack / master parameters and the impulses.
bool applyMulti(Engine& engine, const MultiPreset& multi, const std::string& libraryRoot,
                std::vector<std::string>& warnings);
MultiPreset captureMulti(const Engine& engine, const std::string& name, const std::string& libraryRoot);
bool loadPartSamples(Engine& engine, int part, int layer, const std::string& path, std::string& error,
                     std::vector<std::string>& warnings);
bool loadPartWavetable(Engine& engine, int part, int layer, const std::string& path, std::string& error);

// Scala (.scl) scale -> tune.c .. tune.b of a part (scale degree 0 on pitch
// class `root`, 0 = C). Non-12-note scales fail with an error.
bool applyScalaTuning(Engine& engine, int part, const std::string& sclPath, int root, std::string& error);

// Load a WAV/SFZ file into a layer. Returns false on failure (error set);
// warnings receives non-fatal SFZ messages.
bool loadLayerSamples(Engine& engine, int layer, const std::string& path, std::string& error,
                      std::vector<std::string>& warnings);

// Load a WAV wavetable as the layer's "user" table (layer type WT).
bool loadLayerWavetable(Engine& engine, int layer, const std::string& path, std::string& error);

// Offline rendering ---------------------------------------------------------

struct ScheduledEvent {
    double time = 0;  // seconds
    Event event;
};

struct RenderJob {
    float sampleRate = 48000.0f;
    int blockSize = 128;
    double seconds = 2.0;
    std::vector<ScheduledEvent> events;  // must be sorted by time
};

// Renders job into left/right (resized). Events are quantised to block starts.
void renderOffline(Engine& engine, const RenderJob& job, std::vector<float>& left, std::vector<float>& right);

// Helper to schedule a note.
void addNote(RenderJob& job, double time, double duration, int key, float velocity, uint32_t id = 0);

}  // namespace hsynth
