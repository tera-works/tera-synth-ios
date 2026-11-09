// macOS / iOS MIDI output via CoreMIDI (destination endpoints by name).
#include "midi_out.h"

#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace {
struct Out {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef dest = 0;
};

std::string endpointName(MIDIEndpointRef e) {
    CFStringRef s = nullptr;
    if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) != noErr || !s) return "MIDI";
    char buf[256];
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

void sendBytes(Out* o, const uint8_t* b, int n) {
    Byte buffer[64];
    auto* list = reinterpret_cast<MIDIPacketList*>(buffer);
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, sizeof buffer, p, 0, (ByteCount)n, b);
    if (p) MIDISend(o->port, o->dest, list);
}

void sendLong(Out* o, const uint8_t* b, size_t n) {
    std::vector<Byte> buffer(n + 256);
    auto* list = reinterpret_cast<MIDIPacketList*>(buffer.data());
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, buffer.size(), p, 0, (ByteCount)n, b);
    if (p) MIDISend(o->port, o->dest, list);
}
}  // namespace

MidiOut::~MidiOut() {
    setClock(false, nullptr);
    close();
}

std::vector<std::string> MidiOut::ports() {
    std::vector<std::string> out;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i)
        if (MIDIEndpointRef d = MIDIGetDestination(i)) out.push_back(endpointName(d));
    return out;
}

bool MidiOut::open(const std::string& name, std::string& error) {
    close();
    if (name.empty()) return true;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef d = MIDIGetDestination(i);
        if (!d || endpointName(d) != name) continue;
        auto* o = new Out();
        if (MIDIClientCreate(CFSTR("HSynth Out"), nullptr, nullptr, &o->client) != noErr ||
            MIDIOutputPortCreate(o->client, CFSTR("HSynth Out"), &o->port) != noErr) {
            if (o->client) MIDIClientDispose(o->client);
            delete o;
            error = "cannot open MIDI output " + name;
            return false;
        }
        o->dest = d;
        std::lock_guard<std::mutex> lock(mutex_);
        handle_ = o;
        name_ = name;
        return true;
    }
    error = "MIDI output not found: " + name;
    return false;
}

void MidiOut::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* o = static_cast<Out*>(handle_)) {
        for (uint8_t ch = 0; ch < 16; ++ch) {  // all notes off
            const uint8_t m[3] = {(uint8_t)(0xB0 | ch), 123, 0};
            sendBytes(o, m, 3);
        }
        MIDIPortDispose(o->port);
        MIDIClientDispose(o->client);
        delete o;
    }
    handle_ = nullptr;
    name_.clear();
}

void MidiOut::send(uint8_t status, uint8_t d1, uint8_t d2) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* o = static_cast<Out*>(handle_);
    if (!o) return;
    const uint8_t hi = status & 0xF0;
    const int len = (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    const uint8_t m[3] = {status, d1, d2};
    sendBytes(o, m, len);
}

void MidiOut::sendRealtime(uint8_t status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* o = static_cast<Out*>(handle_)) sendBytes(o, &status, 1);
}

void MidiOut::sendSysEx(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* o = static_cast<Out*>(handle_); o && len) sendLong(o, data, len);
}

void MidiOut::setClock(bool on, std::function<float()> tempo) {
    if (clockRun_.load()) {
        clockRun_.store(false);
        if (clock_.joinable()) clock_.join();
    }
    if (!on || !tempo) return;
    clockRun_.store(true);
    clock_ = std::thread([this, tempo] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        auto next = std::chrono::steady_clock::now();
        while (clockRun_.load()) {
            const double bpm = std::max(20.0f, std::min(300.0f, tempo()));
            next += std::chrono::nanoseconds((long long)(60.0e9 / (bpm * 24.0)));
            std::this_thread::sleep_until(next - std::chrono::milliseconds(1));
            while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
            sendRealtime(0xF8);
        }
    });
}
