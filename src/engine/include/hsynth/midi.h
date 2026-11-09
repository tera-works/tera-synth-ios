// Translators from external protocols to engine Events.
//
// MidiTranslator understands
//   * MIDI 1.0 byte streams (running status, SysEx skipped), with optional MPE
//     (lower zone: channel 1 master, channels 2-16 members)
//   * MIDI 2.0 Universal MIDI Packets (MIDI 1.0 and MIDI 2.0 channel voice
//     messages, including per-note pitch bend and per-note controllers)
//
// MIDI clock (F8 / FA / FB / FC, also as UMP system messages) becomes
// EventType::Clock; the engine derives tempo and beat position from it.
//
// Every emitted Event carries its MIDI channel (Event::channel, 0..15; the UMP
// group is ignored). The engine uses it to route events to parts in
// multi-timbral mode and ignores it in single-part mode. Multi-timbral mode
// and MPE exclude each other: with setMultiTimbral(true) the MPE member-channel
// handling is off and every channel is an independent GM-style channel.
//
// Any other controller protocol (e.g. the planned RP2350 shoulder keyboard
// over a vendor USB interface) should implement its own translator that emits
// the same Events into an EventSink; the DSP code never sees protocol details.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hsynth/event.h"

namespace hsynth {

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void onEvent(const Event& e) = 0;
    virtual void onProgramChange(int /*channel*/, int /*program*/) {}
};

class MidiTranslator {
public:
    void setMpeEnabled(bool on) { mpe_ = on; }
    bool mpeEnabled() const { return mpe_; }
    // Mirror Engine::setMultiTimbral(); disables MPE interpretation while on.
    void setMultiTimbral(bool on) { multi_ = on; }
    bool multiTimbral() const { return multi_; }
    void setMpeBendRange(float semis) { mpeBendRange_ = semis; }
    void setPerNoteBendRange(float semis) { perNoteBendRange_ = semis; }

    void processMidi1(const uint8_t* data, size_t len, EventSink& sink);
    // Words in host order.
    void processUmp(const uint32_t* words, size_t count, EventSink& sink);
    // Android delivers UMP as a byte stream of big-endian 32-bit words.
    void processUmpBytes(const uint8_t* data, size_t len, EventSink& sink);

    static uint32_t noteId(int group, int channel, int note) {
        return ((uint32_t)((group & 15) << 4 | (channel & 15)) << 8 | (uint32_t)(note & 127)) + 1u;
    }
    static int umpWordCount(uint32_t firstWord);

private:
    void midi1Message(int group, uint8_t status, uint8_t d1, uint8_t d2, EventSink& sink);
    void midi2Message(uint32_t w0, uint32_t w1, EventSink& sink);
    void realtime(uint8_t status, EventSink& sink);  // clock / start / continue / stop
    bool isMpeMember(int channel) const { return mpe_ && !multi_ && channel >= 1; }

    bool mpe_ = false;
    bool multi_ = false;
    float mpeBendRange_ = 48.0f;
    float perNoteBendRange_ = 48.0f;
    // MIDI 1.0 parser state
    uint8_t running_ = 0;
    uint8_t data_[2] = {};
    int have_ = 0;
    bool sysex_ = false;
    // MPE: last note started on each channel (per group)
    int16_t channelNote_[16][16];
    uint32_t umpPartial_ = 0;
    int umpPartialBytes_ = 0;
    uint32_t umpWords_[4] = {};
    int umpHave_ = 0;

public:
    MidiTranslator() {
        for (auto& g : channelNote_)
            for (auto& c : g) c = -1;
    }
};

}  // namespace hsynth
