// General MIDI sound selection for song (SMF) playback: maps GM program
// numbers (and the drum channel) to the presets the user has, then loads the
// presets of a song's channels into the matching parts.
//
// Presets are recognised by name: SonicCell "GM2 Capital NNN" captures
// (program NNN-1), the "GM Test" sampled instruments, drum kits, and a few
// factory presets by keyword. Missing programs fall back to the nearest one
// in the same GM family, then to a related family.
//
// Bank select reaches every SonicCell capture by its own numbers, as on the
// SonicCell itself: GM2 Capital = MSB 121 LSB 0, GM2 Var #k = 121 / k,
// PRESET A..G = 87 / 64..70, PRESET Rhythm = 86 / 64, GM2 Rhythm Sets = 120 / 0.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/preset_index.h"
#include "hsynth/smf.h"

namespace hsynth {

const char* gmProgramName(int program);  // "Acoustic Grand Piano" ... (0..127)

class GmBank {
public:
    struct Entry {
        int program = -1;      // 0..127, -1 for drum kits
        bool drum = false;
        int quality = 0;       // higher wins among equals
        int msb = -1, lsb = -1;  // SonicCell bank of the capture (bank select), -1 = none
        int bankProgram = -1;  // its program number in that bank (0..127)
        bool bankOnly = false; // reached only by bank select (SonicCell PRESET A-G, GM2 variations)
        std::string file, name;
    };
    void build(const std::vector<PresetInfo>& presets);
    // Preset file for a program or patch key (smf.h; with a bank: the capture of exactly that
    // bank / program, else as the plain program) or the drum kit; nullptr if the bank is empty.
    const Entry* pick(int program, bool drum) const;
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

// Puts the engine in multi-timbral mode and loads, for every channel the song
// uses, the preset of its first program into part = channel (channel 10 =
// drums). loaded[16] caches the preset file per part between songs. Lines
// for the user ("ch 1: Piano 1 <- Acoustic Grand Piano") go to report.
struct SongPart {
    int channel = 0;          // 0..15 (= part)
    bool drum = false;
    int program = 0;
    std::string gmName;       // "Acoustic Grand Piano" / "Drum Kit"
    std::string presetName;   // "" if nothing could be loaded
};
bool prepareSong(Engine& engine, const Song& song, const GmBank& bank, const std::string& libraryRoot,
                 std::string loaded[kNumParts], std::vector<std::string>& report, std::vector<SongPart>* parts = nullptr);

// Follows a song's mid-song program changes while it plays: a background
// thread looks a few seconds ahead of the song position and loads the preset
// of each channel's next program (Engine::stagePartProgram), so the engine can
// switch at the exact moment of the program change. After a seek it loads the
// program in force at the new position first. Channel 10 switches drum kits
// (GM2: 0 Standard, 8 Room, 16 Power, 24 Electric, 25 Analog, 32 Jazz,
// 40 Brush, 48 Orchestra, 56 SFX; numbers in between use the kit below). Start it after prepareSong();
// stop() (or the destructor) before loading another song.
class SongProgramLoader {
public:
    static constexpr double kLookahead = 15.0;  // seconds
    SongProgramLoader() = default;
    SongProgramLoader(const SongProgramLoader&) = delete;
    SongProgramLoader& operator=(const SongProgramLoader&) = delete;
    ~SongProgramLoader() { stop(); }

    void start(Engine& engine, const Song& song, const GmBank& bank, const std::string& libraryRoot);
    void stop();
    // without the thread (tests, offline renders): prepare() once (false: the song never
    // switches programs), then step() stages at most one program per call (false: nothing due)
    bool prepare(const Song& song, const GmBank& bank, const std::string& libraryRoot);
    bool step(Engine& engine);
    // name of the preset playing on a channel ("" = unchanged since prepareSong); any thread
    std::string presetName(int channel) const;

private:
    struct Change { double time; int program; };
    std::vector<Change> changes_[16];
    int initial_[16] = {};
    GmBank bank_;
    std::string root_;
    int stagedProg_[16] = {};    // program staged per channel and not applied yet (-1 none)
    std::string stagedName_[16];
    mutable std::mutex nameMutex_;
    std::string names_[16];      // preset playing after a switch (presetName)
    std::atomic<bool> run_{false};
    std::thread thread_;
    const GmBank::Entry* entryFor(int program, int channel) const;
};

}  // namespace hsynth
