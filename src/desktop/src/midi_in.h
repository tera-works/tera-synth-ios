// Desktop MIDI input. Windows: winmm (MIDI 1.0); macOS / iOS: CoreMIDI, MIDI 1.0 bytes
// by default, or the MIDI 2.0 protocol (Universal MIDI Packets; MIDI 1.0 devices are
// converted by CoreMIDI) with setUmpInput(true). Everything goes to the engine's portable
// MidiTranslator, like on Android.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/midi.h"
#include "hsynth/midi_learn.h"

class MidiIn {
public:
    explicit MidiIn(hsynth::Engine& engine);
    ~MidiIn();

    // (Re)open all available input ports. Returns the port names.
    std::vector<std::string> openAll();
    void closeAll();
    std::vector<std::string> openPorts() const;

    void setMpe(bool on);
    void setMultiTimbral(bool on);
    void setLearn(hsynth::MidiLearn* l) { learn_ = l; }
    int takeProgramChange() { return pendingProgram_.exchange(-1); }
    int eventCount() const { return eventCount_.load(); }
    std::string lastMessage() const;
    // CoreMIDI: receive with the MIDI 2.0 protocol (UMP) instead of MIDI 1.0 bytes.
    // Takes effect at once (the input port is made again). No-op on Windows.
    void setUmpInput(bool on);
    bool umpInput() const { return umpInput_; }
    // One line for the user: port state, sources, messages received ("" on Windows).
    std::string status() const;

    // System Exclusive messages (complete F0..F7, driver thread) with the name of the
    // output port that reaches the sender ("" = unknown, kVirtualPort = our virtual
    // source). Used by the SysEx remote control (remote_link.h).
    using SysExHandler = std::function<void(const uint8_t* data, size_t len, const std::string& replyPort)>;
    void setSysExHandler(SysExHandler h);
    static constexpr const char* kVirtualPort = "<virtual>";
    // Sends through the virtual "HSynth" source (CoreMIDI); false where there is none.
    bool sendVirtual(const uint8_t* data, size_t len);

    // Called from the driver thread with one complete MIDI message.
    void onBytes(const uint8_t* data, size_t len);
    void onSysEx(const uint8_t* data, size_t len, const std::string& replyPort);
    // ... or with Universal MIDI Packets (words in host order).
    void onUmp(const uint32_t* words, size_t count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    hsynth::Engine& engine_;
    hsynth::MidiTranslator translator_;
    mutable std::mutex mutex_;
    std::atomic<int> pendingProgram_{-1};
    std::atomic<int> eventCount_{0};
    std::atomic<int> rawCount_{0};  // messages received from the driver (before the translator)
    bool umpInput_ = false;
    std::string last_;
    std::vector<std::string> names_;
    hsynth::MidiLearn* learn_ = nullptr;
    std::mutex sysexMutex_;
    SysExHandler sysex_;
};
