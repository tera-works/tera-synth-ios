// Performance events used inside the engine.
// These are deliberately independent of any MIDI version: MIDI 1.0, MIDI 2.0
// UMP, the touch keyboard and the future RP2350 shoulder keyboard protocol are
// all translated into this representation before they reach the DSP code.
#pragma once

#include <cstdint>

namespace hsynth {

enum class EventType : uint8_t {
    NoteOn,          // key, value = velocity 0..1, noteId
    NoteOff,         // key, value = release velocity 0..1, noteId
    // Per-note expression (targets noteId, or key if noteId == 0)
    NotePitch,       // value = pitch offset in semitones (relative to key)
    NotePressure,    // value = 0..1
    NoteTimbre,      // value = 0..1 (Y position on key / MPE CC74 / MIDI2 per-note ctrl)
    NoteX,           // value = -1..1 (X position on key surface)
    NoteHold,        // value = 0/1 (hard-press hold)
    // Channel-wide controls
    PitchBend,       // value = -1..1 (scaled by bend range)
    ModWheel,        // value = 0..1
    ChannelPressure, // value = 0..1
    Sustain,         // value = 0/1
    Controller,      // controller = CC number 0..127, value = 0..1
    ProgramChange,   // controller = program number (handled by host, forwarded as notification)
    AllNotesOff,
    AllSoundOff,
    Looper,          // controller = fx::LooperCmd
    Clock,           // MIDI clock: controller = ClockMsg (tick 24 ppqn / start / continue / stop)
};

enum class ClockMsg : uint16_t { Tick = 0, Start, Continue, Stop };

// Event::channel value for events that are not tied to a MIDI channel (touch
// keyboard, host UI, offline renders). In multi-timbral mode they go to the
// engine's selected part; in single-part mode the channel is ignored.
constexpr uint8_t kOmniChannel = 0xFF;

struct Event {
    EventType type = EventType::NoteOn;
    uint8_t key = 60;         // MIDI-style note number used for pitch & key range
    uint16_t controller = 0;  // controller / program number
    uint32_t noteId = 0;      // unique id of a sounding note (0 = match by key)
    float value = 0.0f;
    uint32_t frame = 0;       // reserved for sample-accurate scheduling
    uint8_t channel = kOmniChannel;  // MIDI channel 0..15 (set by MidiTranslator) or kOmniChannel

    static Event noteOn(uint32_t id, int key, float vel) {
        Event e; e.type = EventType::NoteOn; e.noteId = id; e.key = (uint8_t)key; e.value = vel; return e;
    }
    static Event noteOff(uint32_t id, int key, float vel = 0.5f) {
        Event e; e.type = EventType::NoteOff; e.noteId = id; e.key = (uint8_t)key; e.value = vel; return e;
    }
    static Event perNote(EventType t, uint32_t id, int key, float v) {
        Event e; e.type = t; e.noteId = id; e.key = (uint8_t)key; e.value = v; return e;
    }
    static Event control(EventType t, float v, int ctrl = 0) {
        Event e; e.type = t; e.value = v; e.controller = (uint16_t)ctrl; return e;
    }
};

}  // namespace hsynth
