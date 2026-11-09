// SysEx remote control for the desktop / iOS hosts (docs/SYSEX.md).
//
// Connects the engine's RemoteControl to MIDI: SysEx from any input goes to it,
// and its replies / notifications go back to the device the last request came
// from (the output port of the same device, or the virtual "HSynth" source on
// macOS / iOS). Works with a PC over USB MIDI, a loopback port (loopMIDI on
// Windows: one port is enough, echoes are ignored) or a network MIDI session.
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "hsynth/remote.h"
#include "midi_in.h"
#include "midi_out.h"

class RemoteLink {
public:
    RemoteLink(hsynth::Engine& engine, MidiIn& in, hsynth::RemoteControl::Config config,
               hsynth::RemoteControl::Hooks hooks = {});
    ~RemoteLink();
    RemoteLink(const RemoteLink&) = delete;
    RemoteLink& operator=(const RemoteLink&) = delete;

    hsynth::RemoteControl& control() { return *control_; }
    // The port replies go to ("" until the first request).
    std::string replyPort() const;
    uint32_t requests() const { return requests_.load(); }

private:
    void send(const std::vector<uint8_t>& msg);

    MidiIn& in_;
    MidiOut out_;
    mutable std::mutex mutex_;
    std::string wanted_, opened_;
    std::atomic<uint32_t> requests_{0};
    std::unique_ptr<hsynth::RemoteControl> control_;
};
