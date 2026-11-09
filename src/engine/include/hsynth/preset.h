// JSON preset format.
//
// {
//   "format": "hsynth-preset", "version": 1,
//   "name": "...", "category": "...", "author": "...", "comment": "...",
//   "tags": ["warm", "pad"],                                   (optional)
//   "samples": { "a": "factory/piano/piano.sfz", "b": "", "c": "" },
//   "impulses": { "cab": "user/ir/4x12.wav", "reverb": "" },   (optional)
//   "params": { "master.volume": 0.7, "a.type": "fm", ... }
// }
//
// Parameters are stored by name; enum values as their label. Missing params
// take their default value, unknown params are reported as warnings, so old
// presets keep loading as the engine grows.
#pragma once

#include <string>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

struct Preset {
    std::string name = "Init";
    std::string category;
    std::string author;
    std::string comment;
    std::vector<std::string> tags;      // free-form search tags
    std::vector<float> params;          // kNumParams values
    std::string samples[kNumLayers];    // library-relative sample/SFZ paths
    std::string impulses[2];            // user IRs: cabinet, IR reverb (library-relative)
    std::vector<std::string> warnings;  // filled by fromJson

    Preset();
};

std::string presetToJson(const Preset& p);
bool presetFromJson(const std::string& json, Preset& out, std::string& error);
bool loadPresetFile(const std::string& path, Preset& out, std::string& error);
bool savePresetFile(const std::string& path, const Preset& p, std::string& error);

// Multi-timbral setup ("multi"):
// {
//   "format": "hsynth-multi", "version": 1, "name": "...", "comment": "...",
//   "selected": 1,                                  (part receiving omni events, 1..16)
//   "global": { "impulses": {...}, "params": { "master.volume": 0.7, "fx.slot1": ..., ... } },
//   "parts": [                                      (16 entries; missing ones = defaults)
//     { "mix": { "volume": 1, "pan": 0, "mute": false, "channel": 1, "key.low": 0,
//                "key.high": 127, "transpose": 0, "fx.send": 1 },
//       "preset": { a regular hsynth-preset object with only part parameters } },
//     ...
//   ]
// }
// "global.params" holds the non-part parameters (rack, master volume,
// limiter); each part preset holds isPartParam() parameters and its samples.
struct MultiPreset {
    std::string name = "Init Multi";
    std::string comment;
    int selectedPart = 0;
    Preset global;                          // only non-part params + impulses are used
    Preset parts[kNumParts];                // only part params + samples are used
    float mix[kNumParts][Mix::Count];       // per-part mix settings
    std::vector<std::string> warnings;      // filled by multiFromJson

    MultiPreset();
};

std::string multiToJson(const MultiPreset& m);
bool multiFromJson(const std::string& json, MultiPreset& out, std::string& error);
bool loadMultiFile(const std::string& path, MultiPreset& out, std::string& error);
bool saveMultiFile(const std::string& path, const MultiPreset& m, std::string& error);

// Converts a parameter name from before the effect rack (chorus.*, delay.*,
// reverb.*, eq.low/high) into rack settings. Returns false if unknown.
bool applyLegacyParam(Preset& p, const std::string& name, float value);

// Parameter metadata as JSON (for generic editors on the host side).
std::string paramMetadataJson();

}  // namespace hsynth
