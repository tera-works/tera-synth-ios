// macOS / iOS MIDI input via CoreMIDI. Every source is connected to one input port:
// by default a MIDI 1.0 port (bytes, readProc -> onBytes), or with setUmpInput(true) a
// port that uses the MIDI 2.0 protocol: CoreMIDI then delivers Universal MIDI Packets
// (MIDI 2.0 devices with their full resolution and per-note controllers, MIDI 1.0
// devices converted), which go to the portable MidiTranslator through MidiIn::onUmp.
// SysEx (MIDI 1.0 bytes or UMP SysEx7 packets) is assembled per source and goes to
// the SysEx handler with the name of the destination of the same device (replies).
// A virtual destination + source named "HSynth" let other apps on this machine
// (and a PC through a network MIDI session) talk to the synth directly.
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <cstdio>

#include "midi_in.h"

namespace {
struct Sink : hsynth::EventSink {
    hsynth::Engine& engine;
    std::atomic<int>& program;
    std::atomic<int>& count;
    Sink(hsynth::Engine& e, std::atomic<int>& p, std::atomic<int>& c) : engine(e), program(p), count(c) {}
    void onEvent(const hsynth::Event& e) override {
        engine.sendEvent(e);
        count.fetch_add(1);
    }
    void onProgramChange(int, int p) override { program.store(p); }
};

std::string endpointName(MIDIEndpointRef e) {
    CFStringRef s = nullptr;
    if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) != noErr || !s) return "MIDI";
    char buf[256];
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

// The destination of the device a source belongs to (its first one), else a destination
// with the same name; "" if there is none.
std::string replyDestination(MIDIEndpointRef src) {
    MIDIEntityRef entity = 0;
    if (MIDIEndpointGetEntity(src, &entity) == noErr && entity && MIDIEntityGetNumberOfDestinations(entity) > 0)
        if (MIDIEndpointRef d = MIDIEntityGetDestination(entity, 0)) return endpointName(d);
    const std::string name = endpointName(src);
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i)
        if (MIDIEndpointRef d = MIDIGetDestination(i); d && endpointName(d) == name) return name;
    return "";
}

// Length of the channel / system message starting with `status` (SysEx excluded).
size_t messageLength(uint8_t status) {
    if (status < 0xF0) {
        const uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        default: return 1;
    }
}
}  // namespace

// Per connected source: where replies go and the SysEx being received.
struct Source {
    MidiIn* self = nullptr;
    MIDIEndpointRef endpoint = 0;
    std::string reply;           // destination name of the same device
    std::vector<uint8_t> sysex;
    void sysexBytes(const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const uint8_t c = b[i];
            if (c == 0xF0) sysex.clear();
            if (c == 0xF0 || !sysex.empty()) sysex.push_back(c);
            if (c == 0xF7 && !sysex.empty()) {
                self->onSysEx(sysex.data(), sysex.size(), reply);
                sysex.clear();
            }
        }
        if (sysex.size() > (1u << 20)) sysex.clear();
    }
    // UMP: SysEx7 packets (message type 3) -> bytes
    void ump(const uint32_t* w, size_t count) {
        for (size_t i = 0; i < count;) {
            const int mt = (int)(w[i] >> 28);
            const int len = hsynth::MidiTranslator::umpWordCount(w[i]);
            if (mt == 0x3 && i + 1 < count) {
                const int status = (int)((w[i] >> 20) & 0xF), nb = std::min(6, (int)((w[i] >> 16) & 0xF));
                const uint8_t d[6] = {(uint8_t)(w[i] >> 8), (uint8_t)w[i], (uint8_t)(w[i + 1] >> 24),
                                      (uint8_t)(w[i + 1] >> 16), (uint8_t)(w[i + 1] >> 8), (uint8_t)w[i + 1]};
                const uint8_t f0 = 0xF0, f7 = 0xF7;
                if (status == 0 || status == 1) sysexBytes(&f0, 1);  // complete / start
                for (int k = 0; k < nb && !sysex.empty(); ++k) sysex.push_back(d[k] & 0x7F);
                if (status == 0 || status == 3) sysexBytes(&f7, 1);  // complete / end
            }
            i += (size_t)std::max(1, len);
        }
    }
};

struct MidiIn::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    bool portIsUmp = false;
    OSStatus clientErr = 0, portErr = 0;
    std::vector<std::unique_ptr<Source>> sources;
    // disconnected sources stay allocated: CoreMIDI may still be delivering to them
    std::vector<std::unique_ptr<Source>> retired;
    MIDIEndpointRef virtualDest = 0, virtualSrc = 0;
    std::unique_ptr<Source> virtualIn;  // what arrives at the virtual destination
    uint8_t running = 0;  // running status
};

namespace {
void readProc(const MIDIPacketList* list, void* refCon, void* srcRef) {
    auto* self = static_cast<MidiIn*>(refCon);
    auto* src = static_cast<Source*>(srcRef);
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 n = 0; n < list->numPackets; ++n) {
        size_t i = 0;
        uint8_t running = 0;
        // the continuation of a SysEx that started in an earlier packet
        if (src && !src->sysex.empty()) {
            size_t k = 0;
            while (k < p->length && p->data[k] < 0x80) ++k;
            if (k < p->length && p->data[k] == 0xF7) ++k;
            src->sysexBytes(p->data, k);
            i = k;
        }
        while (i < p->length) {
            uint8_t b = p->data[i];
            if (b == 0xF0) {  // SysEx: to the handler (it may continue in the next packet)
                size_t k = i;
                while (k < p->length && p->data[k] != 0xF7) ++k;
                if (k < p->length) ++k;
                if (src) src->sysexBytes(p->data + i, k - i);
                i = k;
                continue;
            }
            uint8_t msg[3];
            size_t len;
            if (b & 0x80) {
                if (b < 0xF0) running = b;
                len = messageLength(b);
                msg[0] = b;
                ++i;
            } else {
                if (!running) { ++i; continue; }
                len = messageLength(running);
                msg[0] = running;
            }
            size_t k = 1;
            while (k < len && i < p->length) msg[k++] = p->data[i++];
            if (k == len) self->onBytes(msg, len);
        }
        p = MIDIPacketNext(p);
    }
}

// Devices came or went (USB, Bluetooth LE MIDI connected in the system's Bluetooth
// screen, network sessions): reconnect every source. Runs on the client's run loop.
void notifyProc(const MIDINotification* n, void* refCon) {
    if (!n || !refCon) return;
    if (n->messageID == kMIDIMsgSetupChanged || n->messageID == kMIDIMsgObjectAdded || n->messageID == kMIDIMsgObjectRemoved)
        static_cast<MidiIn*>(refCon)->openAll();
}
}  // namespace

MidiIn::MidiIn(hsynth::Engine& engine) : impl_(std::make_unique<Impl>()), engine_(engine) {}
MidiIn::~MidiIn() {
    closeAll();
    if (impl_->virtualDest) MIDIEndpointDispose(impl_->virtualDest);
    if (impl_->virtualSrc) MIDIEndpointDispose(impl_->virtualSrc);
    if (impl_->port) MIDIPortDispose(impl_->port);
    if (impl_->client) MIDIClientDispose(impl_->client);
}

std::vector<std::string> MidiIn::openAll() {
    closeAll();
    if (impl_->port && impl_->portIsUmp != umpInput_) {  // the protocol changed: make the port again
        MIDIPortDispose(impl_->port);
        impl_->port = 0;
    }
    if (!impl_->client) {
        impl_->clientErr = MIDIClientCreate(CFSTR("HSynth"), notifyProc, this, &impl_->client);
        if (impl_->clientErr != noErr) {
            impl_->client = 0;
            return {};
        }
    }
    if (!impl_->port) {
        OSStatus st = -1;
        impl_->portIsUmp = false;
        if (!umpInput_) {
            // MIDI 1.0 bytes (readProc below)
        } else if (__builtin_available(macOS 11.0, iOS 14.0, *)) {
            st = MIDIInputPortCreateWithProtocol(impl_->client, CFSTR("HSynth In"), kMIDIProtocol_2_0, &impl_->port,
                                                 ^(const MIDIEventList* list, void* srcRef) {
                                                     auto* src = static_cast<Source*>(srcRef);
                                                     const MIDIEventPacket* p = &list->packet[0];
                                                     for (UInt32 k = 0; k < list->numPackets; ++k) {
                                                         if (src) src->ump(p->words, p->wordCount);
                                                         onUmp(p->words, p->wordCount);
                                                         p = MIDIEventPacketNext(p);
                                                     }
                                                 });
        }
        if (st == noErr) impl_->portIsUmp = true;
        else st = MIDIInputPortCreate(impl_->client, CFSTR("HSynth In"), readProc, this, &impl_->port);
        impl_->portErr = st;
        if (st != noErr) {
            impl_->port = 0;
            return {};
        }
    }
    if (!impl_->virtualIn) {
        // virtual endpoints: other apps / a PC (network session) can address the synth directly
        impl_->virtualIn = std::make_unique<Source>();
        impl_->virtualIn->self = this;
        impl_->virtualIn->reply = kVirtualPort;
        Source* vin = impl_->virtualIn.get();
        if (__builtin_available(macOS 11.0, iOS 14.0, *)) {
            MIDIDestinationCreateWithProtocol(impl_->client, CFSTR("HSynth"), kMIDIProtocol_2_0, &impl_->virtualDest,
                                              ^(const MIDIEventList* list, void*) {
                                                  const MIDIEventPacket* p = &list->packet[0];
                                                  for (UInt32 k = 0; k < list->numPackets; ++k) {
                                                      vin->ump(p->words, p->wordCount);
                                                      onUmp(p->words, p->wordCount);
                                                      p = MIDIEventPacketNext(p);
                                                  }
                                              });
            MIDISourceCreateWithProtocol(impl_->client, CFSTR("HSynth"), kMIDIProtocol_1_0, &impl_->virtualSrc);
        }
    }
    std::vector<std::string> names;
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef src = MIDIGetSource(i);
        if (!src || src == impl_->virtualSrc) continue;  // not our own output
        auto s = std::make_unique<Source>();
        s->self = this;
        s->endpoint = src;
        s->reply = replyDestination(src);
        if (MIDIPortConnectSource(impl_->port, src, s.get()) == noErr) {
            names.push_back(endpointName(src));
            impl_->sources.push_back(std::move(s));
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    names_ = names;
    return names;
}

void MidiIn::closeAll() {
    for (auto& s : impl_->sources) {
        if (impl_->port) MIDIPortDisconnectSource(impl_->port, s->endpoint);
        impl_->retired.push_back(std::move(s));
    }
    impl_->sources.clear();
    if (impl_->retired.size() > 256) impl_->retired.erase(impl_->retired.begin(), impl_->retired.begin() + 128);
    std::lock_guard<std::mutex> lock(mutex_);
    names_.clear();
}

std::vector<std::string> MidiIn::openPorts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return names_;
}

void MidiIn::setMpe(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    translator_.setMpeEnabled(on);
}

void MidiIn::setMultiTimbral(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    translator_.setMultiTimbral(on);
}

void MidiIn::setUmpInput(bool on) {
    if (on == umpInput_) return;
    umpInput_ = on;
    if (impl_->client) openAll();
}

std::string MidiIn::status() const {
    char buf[200];
    if (!impl_->client)
        std::snprintf(buf, sizeof buf, "CoreMIDI client error %d", (int)impl_->clientErr);
    else if (!impl_->port)
        std::snprintf(buf, sizeof buf, "input port error %d", (int)impl_->portErr);
    else
        std::snprintf(buf, sizeof buf, "%s input, %d source(s) of %d, %d message(s) received, %d event(s)",
                      impl_->portIsUmp ? "MIDI 2.0 (UMP)" : "MIDI 1.0", (int)impl_->sources.size(),
                      (int)MIDIGetNumberOfSources(), rawCount_.load(), eventCount_.load());
    return buf;
}

std::string MidiIn::lastMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

void MidiIn::setSysExHandler(SysExHandler h) {
    std::lock_guard<std::mutex> lock(sysexMutex_);
    sysex_ = std::move(h);
}

void MidiIn::onSysEx(const uint8_t* data, size_t len, const std::string& replyPort) {
    std::lock_guard<std::mutex> lock(sysexMutex_);
    if (sysex_) sysex_(data, len, replyPort);
}

bool MidiIn::sendVirtual(const uint8_t* data, size_t len) {
    if (!impl_->virtualSrc || len == 0) return false;
    std::vector<Byte> buffer(len + 256);
    auto* list = reinterpret_cast<MIDIPacketList*>(buffer.data());
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, buffer.size(), p, 0, (ByteCount)len, data);
    return p && MIDIReceived(impl_->virtualSrc, list) == noErr;
}

void MidiIn::onUmp(const uint32_t* words, size_t count) {
    if (count == 0) return;
    rawCount_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    Sink sink(engine_, pendingProgram_, eventCount_);
    if (learn_) learn_->onUmp(words, count, engine_);
    translator_.processUmp(words, count, sink);
    char buf[40];
    std::snprintf(buf, sizeof buf, "UMP %08X%s", (unsigned)words[0], count > 1 ? " ..." : "");
    last_ = buf;
}

void MidiIn::onBytes(const uint8_t* data, size_t len) {
    rawCount_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    Sink sink(engine_, pendingProgram_, eventCount_);
    if (learn_) learn_->onMidi(data, len, engine_);
    translator_.processMidi1(data, len, sink);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02X %02X %02X", data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0);
    last_ = buf;
}
