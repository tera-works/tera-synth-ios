// Setup files: the whole playing setup in one JSON file, to export and import it on
// any host (desktop, Android, iOS, SysEx remote).
//
//   { "format": "hsynth-setup", "version": 1, "name": "...", "comment": "...",
//     "state":     { an hsynth-multi (multi-timbral: one part per MIDI channel)
//                    or an hsynth-preset (single part) },
//     "midiLearn": { an hsynth-midimap: CC -> parameter assignments } (optional),
//     "host":      { host settings, e.g. "voices", "mpe", "quality" } (optional) }
//
// Sample paths in "state" are relative to the library root, so a setup moves between
// devices that have the same library (e.g. library/user/soniccell/...).
// Missing parts of a file keep the current state (a file with only "midiLearn"
// replaces just the assignments).
#pragma once

#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/json.h"

namespace hsynth {

class MidiLearn;

// The current state (multi when the engine is multi-timbral) + MIDI learn + host settings.
// partNames: the preset names the host shows per part (stored with the parts).
std::string makeSetupJson(const Engine& engine, const std::string& name, const std::string& libraryRoot,
                          const MidiLearn* learn, const Json& host = Json::object(), const std::string& comment = "",
                          const std::vector<std::string>& partNames = {});

struct SetupResult {
    std::string name;
    bool hasState = false;
    bool multi = false;          // the state was a multi (the engine is now multi-timbral)
    std::vector<std::string> partNames;  // preset name per part (1 entry for a single preset)
    bool hasMidiLearn = false;
    Json host = Json::object();  // the file's host settings, for the host to apply
    std::vector<std::string> warnings;
};

// Applies a setup: switches single / multi mode, loads the parts and their samples,
// replaces the MIDI learn assignments (when the file has them). Hosts apply out.host
// themselves and keep their own multi / MPE switches in step with out.multi.
bool applySetupJson(Engine& engine, const std::string& text, const std::string& libraryRoot, MidiLearn* learn,
                    SetupResult& out, std::string& error);

}  // namespace hsynth
