// MIDI learn: control-change -> parameter assignments, shared by the hosts.
//
// The host calls arm(param) (e.g. from a parameter's context menu); the next
// CC that arrives is assigned to it. onMidi() is called with every incoming
// MIDI 1.0 message on the MIDI thread and sets the mapped parameters through
// the engine's thread-safe setters (it never runs on the audio thread). The
// messages still go to the engine as well (mod wheel etc. keep working).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hsynth {

class Engine;

struct MidiMapping {
    int channel = -1;      // 0..15, -1 = any channel
    int cc = 1;            // 0..127
    int param = 0;         // parameter id
    int part = -1;         // part for part parameters, -1 = the selected part
    float min = 0.0f;      // parameter value at CC 0
    float max = 1.0f;      // ... at CC 127 (min > max inverts)
};

class MidiLearn {
public:
    // Next CC is assigned to `param` (range = the parameter's full range).
    void arm(int param, int part = -1);
    void cancel();
    int armedParam() const;  // -1 if not armed

    // MIDI thread. Returns true if the message was a learned / mapped CC.
    bool onMidi(const uint8_t* data, size_t len, Engine& engine);
    // The same for Universal MIDI Packets (words in host order): the control changes
    // in them (MIDI 1.0 or MIDI 2.0 channel voice) are learned / mapped like MIDI 1.0 CCs.
    void onUmp(const uint32_t* words, size_t count, Engine& engine);

    std::vector<MidiMapping> mappings() const;
    void setMappings(std::vector<MidiMapping> m);
    void removeParam(int param);              // every mapping of the parameter
    std::string describe(int param) const;    // "CC 74 ch 1" / "" (UI tooltips)
    int lastLearned() const;                  // parameter of the last completed learn (UI feedback), -1 none

    std::string toJson() const;               // parameter names, so files survive parameter-id changes
    bool fromJson(const std::string& json, std::string& error);
    bool load(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error) const;

private:
    mutable std::mutex mutex_;
    std::vector<MidiMapping> maps_;
    int armed_ = -1, armedPart_ = -1, lastLearned_ = -1;
};

}  // namespace hsynth
