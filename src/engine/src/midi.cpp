#include "hsynth/midi.h"

namespace hsynth {

namespace {
int dataBytesFor(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0: return 1;
        case 0xF0:
            switch (status) {
                case 0xF1: case 0xF3: return 1;
                case 0xF2: return 2;
                default: return 0;
            }
        default: return 2;
    }
}
float u32Norm(uint32_t v) { return (float)((double)v / 4294967295.0); }
float u32Bipolar(uint32_t v) { return (float)(((double)v - 2147483648.0) / 2147483648.0); }
// Stamps every event with the MIDI channel it came from (multi-timbral routing).
struct ChannelSink {
    EventSink& sink;
    int channel;
    void onEvent(Event e) {
        e.channel = (uint8_t)channel;
        sink.onEvent(e);
    }
    void onProgramChange(int ch, int program) { sink.onProgramChange(ch, program); }
};
}  // namespace

void MidiTranslator::processMidi1(const uint8_t* data, size_t len, EventSink& sink) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b >= 0xF8) {  // real-time messages may appear anywhere
            realtime(b, sink);
            continue;
        }
        if (b & 0x80) {
            if (b == 0xF0) { sysex_ = true; running_ = 0; continue; }
            if (b == 0xF7) { sysex_ = false; continue; }
            sysex_ = false;
            running_ = b;
            have_ = 0;
            if (dataBytesFor(b) == 0) {
                if (b >= 0xF0) running_ = 0;
            }
            continue;
        }
        if (sysex_ || running_ == 0) continue;
        data_[have_++] = b;
        if (have_ >= dataBytesFor(running_)) {
            midi1Message(0, running_, data_[0], have_ > 1 ? data_[1] : 0, sink);
            have_ = 0;
            if (running_ >= 0xF0) running_ = 0;  // system common: no running status
        }
    }
}

void MidiTranslator::midi1Message(int group, uint8_t status, uint8_t d1, uint8_t d2, EventSink& out) {
    const int ch = status & 0x0F;
    ChannelSink sink{out, ch};
    switch (status & 0xF0) {
        case 0x90:
            if (d2 > 0) {
                channelNote_[group][ch] = d1;
                sink.onEvent(Event::noteOn(noteId(group, ch, d1), d1, d2 / 127.0f));
                break;
            }
            [[fallthrough]];
        case 0x80:
            sink.onEvent(Event::noteOff(noteId(group, ch, d1), d1, d2 / 127.0f));
            break;
        case 0xA0:
            sink.onEvent(Event::perNote(EventType::NotePressure, noteId(group, ch, d1), d1, d2 / 127.0f));
            break;
        case 0xB0:
            if (isMpeMember(ch) && d1 == 74 && channelNote_[group][ch] >= 0) {
                const int n = channelNote_[group][ch];
                sink.onEvent(Event::perNote(EventType::NoteTimbre, noteId(group, ch, n), n, d2 / 127.0f));
                break;
            }
            if (d1 == 1) sink.onEvent(Event::control(EventType::ModWheel, d2 / 127.0f));
            else if (d1 == 64) sink.onEvent(Event::control(EventType::Sustain, d2 >= 64 ? 1.0f : 0.0f));
            else if (d1 == 120) sink.onEvent(Event::control(EventType::AllSoundOff, 0));
            else if (d1 == 123) sink.onEvent(Event::control(EventType::AllNotesOff, 0));
            else sink.onEvent(Event::control(EventType::Controller, d2 / 127.0f, d1));
            break;
        case 0xC0:
            sink.onProgramChange(ch, d1);
            break;
        case 0xD0:
            if (isMpeMember(ch) && channelNote_[group][ch] >= 0) {
                const int n = channelNote_[group][ch];
                sink.onEvent(Event::perNote(EventType::NotePressure, noteId(group, ch, n), n, d1 / 127.0f));
            } else {
                sink.onEvent(Event::control(EventType::ChannelPressure, d1 / 127.0f));
            }
            break;
        case 0xE0: {
            const int v = (d2 << 7) | d1;
            const float bend = (float)(v - 8192) / 8192.0f;
            if (isMpeMember(ch) && channelNote_[group][ch] >= 0) {
                const int n = channelNote_[group][ch];
                sink.onEvent(Event::perNote(EventType::NotePitch, noteId(group, ch, n), n, bend * mpeBendRange_));
            } else {
                sink.onEvent(Event::control(EventType::PitchBend, bend));
            }
            break;
        }
        default: break;
    }
}

void MidiTranslator::realtime(uint8_t status, EventSink& sink) {
    int msg = -1;
    switch (status) {
        case 0xF8: msg = (int)ClockMsg::Tick; break;
        case 0xFA: msg = (int)ClockMsg::Start; break;
        case 0xFB: msg = (int)ClockMsg::Continue; break;
        case 0xFC: msg = (int)ClockMsg::Stop; break;
        default: return;
    }
    sink.onEvent(Event::control(EventType::Clock, 0.0f, msg));
}

int MidiTranslator::umpWordCount(uint32_t w) {
    switch (w >> 28) {
        case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
        case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
        case 0xB: case 0xC: return 3;
        default: return 4;  // 0x5, 0xD, 0xE, 0xF
    }
}

void MidiTranslator::processUmp(const uint32_t* words, size_t count, EventSink& sink) {
    size_t i = 0;
    while (i < count) {
        const uint32_t w0 = words[i];
        const int n = umpWordCount(w0);
        if (i + (size_t)n > count) break;  // incomplete packet
        const int mt = (int)(w0 >> 28);
        const int group = (int)((w0 >> 24) & 0xF);
        if (mt == 0x1) {
            realtime((uint8_t)(w0 >> 16), sink);
        } else if (mt == 0x2) {
            const uint8_t status = (uint8_t)(w0 >> 16);
            midi1Message(group, status, (uint8_t)((w0 >> 8) & 0x7F), (uint8_t)(w0 & 0x7F), sink);
        } else if (mt == 0x4) {
            midi2Message(w0, words[i + 1], sink);
        }
        i += (size_t)n;
    }
}

void MidiTranslator::processUmpBytes(const uint8_t* data, size_t len, EventSink& sink) {
    for (size_t i = 0; i < len; ++i) {
        umpPartial_ = (umpPartial_ << 8) | data[i];
        if (++umpPartialBytes_ < 4) continue;
        umpWords_[umpHave_++] = umpPartial_;
        umpPartial_ = 0;
        umpPartialBytes_ = 0;
        if (umpHave_ >= umpWordCount(umpWords_[0])) {
            processUmp(umpWords_, (size_t)umpHave_, sink);
            umpHave_ = 0;
        }
    }
}

void MidiTranslator::midi2Message(uint32_t w0, uint32_t w1, EventSink& out) {
    const int group = (int)((w0 >> 24) & 0xF);
    const int status = (int)((w0 >> 20) & 0xF);
    const int ch = (int)((w0 >> 16) & 0xF);
    ChannelSink sink{out, ch};
    const int idx = (int)((w0 >> 8) & 0x7F);  // note number or controller index
    const int sub = (int)(w0 & 0xFF);         // attribute type / per-note ctrl index
    const uint32_t id = noteId(group, ch, idx);
    switch (status) {
        case 0x9: {
            float vel = (float)(w1 >> 16) / 65535.0f;
            if (vel <= 0.0f) vel = 1.0f / 65535.0f;  // MIDI 2.0: velocity 0 is still a note-on
            channelNote_[group][ch] = (int16_t)idx;
            sink.onEvent(Event::noteOn(id, idx, vel));
            break;
        }
        case 0x8:
            sink.onEvent(Event::noteOff(id, idx, (float)(w1 >> 16) / 65535.0f));
            break;
        case 0xA:
            sink.onEvent(Event::perNote(EventType::NotePressure, id, idx, u32Norm(w1)));
            break;
        case 0x0:  // registered per-note controller
            if (sub == 74) sink.onEvent(Event::perNote(EventType::NoteTimbre, id, idx, u32Norm(w1)));
            else if (sub == 3) {
                // absolute pitch 7.25 fixed point (semitones)
                const float pitch = (float)((double)w1 / 33554432.0);
                sink.onEvent(Event::perNote(EventType::NotePitch, id, idx, pitch - (float)idx));
            }
            break;
        case 0x1:  // assignable per-note controller (HSynth convention)
            if (sub == 1) sink.onEvent(Event::perNote(EventType::NoteX, id, idx, u32Bipolar(w1)));
            else if (sub == 2) sink.onEvent(Event::perNote(EventType::NoteHold, id, idx, u32Norm(w1)));
            else if (sub == 3) sink.onEvent(Event::perNote(EventType::NoteTimbre, id, idx, u32Norm(w1)));
            break;
        case 0x6:  // per-note pitch bend
            sink.onEvent(Event::perNote(EventType::NotePitch, id, idx, u32Bipolar(w1) * perNoteBendRange_));
            break;
        case 0x2:  // registered controller (RPN), atomic in MIDI 2.0
            if (idx == 0 && sub == 0) {  // pitch bend sensitivity: semitones in the top 7 bits, cents in the next 7
                const int semis = (int)(w1 >> 25), cents = (int)((w1 >> 18) & 0x7F);
                // the engine's RPN handling (select, data entry, deselect)
                sink.onEvent(Event::control(EventType::Controller, 0.0f, 101));
                sink.onEvent(Event::control(EventType::Controller, 0.0f, 100));
                sink.onEvent(Event::control(EventType::Controller, (float)semis / 127.0f, 6));
                sink.onEvent(Event::control(EventType::Controller, (float)cents / 127.0f, 38));
                sink.onEvent(Event::control(EventType::Controller, 1.0f, 101));
                sink.onEvent(Event::control(EventType::Controller, 1.0f, 100));
            }
            break;
        case 0xF:  // per-note management: reset per-note controllers if S flag
            if (w0 & 0x1) sink.onEvent(Event::perNote(EventType::NotePitch, id, idx, 0.0f));
            break;
        case 0xB: {
            const float v = u32Norm(w1);
            if (idx == 1) sink.onEvent(Event::control(EventType::ModWheel, v));
            else if (idx == 64) sink.onEvent(Event::control(EventType::Sustain, v >= 0.5f ? 1.0f : 0.0f));
            else if (idx == 120) sink.onEvent(Event::control(EventType::AllSoundOff, 0));
            else if (idx == 123) sink.onEvent(Event::control(EventType::AllNotesOff, 0));
            else sink.onEvent(Event::control(EventType::Controller, v, idx));
            break;
        }
        case 0xC:
            sink.onProgramChange(ch, (int)((w1 >> 24) & 0x7F));
            break;
        case 0xD:
            sink.onEvent(Event::control(EventType::ChannelPressure, u32Norm(w1)));
            break;
        case 0xE:
            sink.onEvent(Event::control(EventType::PitchBend, u32Bipolar(w1)));
            break;
        default: break;
    }
}

}  // namespace hsynth
