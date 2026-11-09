// Standard MIDI File (format 0 / 1) reader for song playback.
//
// All tracks are merged into one time-ordered list of engine Events (in
// seconds, through the tempo map). Channel messages go through the regular
// MidiTranslator in multi-timbral mode, so every Event carries its MIDI
// channel and the engine routes it to the part listening on that channel.
// Program changes are kept per channel for the host (it loads a preset into
// the part; the engine itself never loads files).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hsynth/event.h"

namespace hsynth {

struct SongEvent {
    double time = 0.0;  // seconds from the start
    Event event;
};

struct Song {
    std::string name;
    std::string path;
    double length = 0.0;                 // seconds (last event)
    float initialTempo = 120.0f;         // bpm at the first tempo event (or 120)
    std::vector<SongEvent> events;       // sorted by time
    int initialProgram[16];              // first program change per channel, -1 = none
    int bankMsb[16];                     // CC0 before the first program change (GM2 / GS banks), -1 = none
    bool channelUsed[16] = {};           // channel has notes
    int noteCount = 0;
    struct Tempo { double time; float bpm; };
    std::vector<Tempo> tempos;           // tempo map (seconds -> bpm), first entry at 0
    float tempoAt(double t) const {
        float bpm = initialTempo;
        for (const auto& x : tempos) {
            if (x.time > t) break;
            bpm = x.bpm;
        }
        return bpm;
    }
    struct ProgramChange { double time; int channel; int program; };
    std::vector<ProgramChange> programChanges;  // all of them (the host may follow changes)
    Song() {
        for (auto& p : initialProgram) p = -1;
        for (auto& b : bankMsb) b = -1;
    }
};

bool loadSmf(const std::string& path, Song& out, std::string& error);
bool parseSmf(const uint8_t* data, size_t size, Song& out, std::string& error);

}  // namespace hsynth
