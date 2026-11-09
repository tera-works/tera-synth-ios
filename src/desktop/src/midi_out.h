// Desktop MIDI output (Windows: winmm; macOS / iOS: CoreMIDI). Sends the notes played on the screen
// / PC keyboard to external gear (zone control) and, optionally, MIDI clock
// (24 ppqn) with Start / Stop following the transport.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class MidiOut {
public:
    ~MidiOut();
    static std::vector<std::string> ports();
    bool open(const std::string& name, std::string& error);  // empty name = close
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    std::string name() const { return name_; }

    void send(uint8_t status, uint8_t d1, uint8_t d2);
    void sendRealtime(uint8_t status);  // F8 / FA / FC
    void sendSysEx(const uint8_t* data, size_t len);  // one complete F0..F7 message

    // Clock thread: 24 ticks per quarter note at tempo() bpm while on.
    void setClock(bool on, std::function<float()> tempo);
    bool clockOn() const { return clockRun_.load(); }

private:
    void* handle_ = nullptr;
    std::string name_;
    std::mutex mutex_;
    std::thread clock_;
    std::atomic<bool> clockRun_{false};
};
