// Remote control over MIDI System Exclusive (see docs/SYSEX.md).
//
// Every setting and operation of the synthesizer can be read and changed by
// another device (a controller, a PC, the host of an embedded build): the
// whole sound state, each parameter, the part mix, presets, samples, songs,
// the phrase recorder, the looper, the engine settings and MIDI learn.
//
// Messages are JSON requests / replies carried in SysEx frames:
//   F0 7D 48 53 <ver> <dev> <type> <seq> <idx lo hi> <total lo hi> <ASCII JSON chunk> F7
// 7D = non-commercial manufacturer id, 48 53 = "HS". Long JSON is split into
// chunks of at most kChunkBytes; idx / total are 14-bit (two 7-bit bytes, low first).
//
// Threads: onSysEx() may be called from any MIDI thread with complete F0..F7
// messages. Requests run on the RemoteControl's own worker thread (never the
// audio thread); replies and change notifications go to the output callback
// from that thread, or wait in a queue for poll().
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/json.h"

namespace hsynth {

class MidiLearn;
class PresetIndex;
class SongProgramLoader;

namespace sysex {

constexpr uint8_t kManufacturer = 0x7D;  // non-commercial / educational use
constexpr uint8_t kId1 = 0x48, kId2 = 0x53;  // "HS"
constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kAllDevices = 0x7F;
constexpr size_t kChunkBytes = 240;      // JSON bytes per message (a frame is at most 256 bytes)
constexpr size_t kHeaderBytes = 12;      // F0 .. total hi

enum Type : uint8_t { Request = 0x01, Reply = 0x02, Notify = 0x03 };

struct Frame {
    int device = 0, type = 0, seq = 0, index = 0, total = 0;
    std::string payload;  // this chunk's JSON text
};

// JSON text with every non-ASCII character escaped (\uXXXX), compact.
std::string asciiJson(const Json& j);
// One or more complete F0..F7 messages carrying `json` (already ASCII).
std::vector<std::vector<uint8_t>> encode(int device, int type, int seq, const std::string& json);
// Parses one F0..F7 message of this protocol. false: another manufacturer / malformed.
bool parseFrame(const uint8_t* data, size_t len, Frame& out);

// Joins chunks back into whole JSON texts (per type + seq; incomplete ones expire).
class Assembler {
public:
    // true when `f` completed a message: json = the whole text.
    bool add(const Frame& f, std::string& json);

private:
    struct Partial {
        int total = 0, received = 0;
        std::vector<std::string> chunks;
        std::chrono::steady_clock::time_point started;
    };
    std::map<int, Partial> partial_;  // key: type << 8 | seq
};

}  // namespace sysex

class RemoteControl {
public:
    struct Config {
        std::string deviceName = "HSynth";
        std::string version;                  // app version shown by "hello"
        int deviceId = 0;                     // 0..126; requests to 0x7F reach every device
        std::string libraryRoot;              // sample paths are relative to it
        std::vector<std::string> presetDirs;  // factory + user preset folders (scanned for "presets")
        std::string userPresetDir;            // "save_preset" writes here
        std::string midiDir;                  // MIDI files ("songs", phrase export)
        MidiLearn* learn = nullptr;           // the host's MIDI learn (optional)
    };
    // Things only the host can do (all optional; called on the worker thread).
    struct Hooks {
        std::function<void(bool on)> setMultiTimbral;  // the host's MIDI translator too
        std::function<void(bool on)> setMpe;
        // Loads a MIDI file as the song (the host's player follows it). "" = ok, else the error.
        // Without it RemoteControl loads the song itself.
        std::function<std::string(const std::string& path, Json& info)> loadSong;
        std::function<void(int part, const std::string& name)> presetLoaded;  // a preset came in remotely
        std::function<void(Json& status)> status;   // extra fields for "status" (song name, ports, ...)
        // Host-specific operations ("host.*" ops): true if handled (reply filled).
        std::function<bool(const std::string& op, const Json& request, Json& reply)> extra;
    };
    using Output = std::function<void(const std::vector<uint8_t>& message)>;

    RemoteControl(Engine& engine, Config config, Hooks hooks = {});
    ~RemoteControl();
    RemoteControl(const RemoteControl&) = delete;
    RemoteControl& operator=(const RemoteControl&) = delete;

    // Replies / notifications go here (worker thread). Not set: they queue for poll().
    void setOutput(Output out);
    // A complete F0..F7 message from any source (any thread). Messages of other
    // manufacturers and our own replies (echoes on loopback ports) are ignored.
    void onSysEx(const uint8_t* data, size_t len);
    bool poll(std::vector<uint8_t>& message);
    // Bumped whenever a request changed something (the host refreshes its UI).
    uint32_t changeCount() const { return changes_.load(std::memory_order_relaxed); }
    int deviceId() const { return config_.deviceId; }

    // Executes one request synchronously (the worker uses it; also for tests).
    Json handle(const Json& request);
    // The change notifications that are due now (diff against the last call); for tests.
    std::vector<Json> collectChanges();

private:
    struct Pending {
        int device = 0, seq = 0;
        std::string json;
    };
    void run();
    void emit(int type, int seq, const Json& body);
    void snapshot();
    int partOf(const Json& req) const;
    Json valueJson(int id, float v) const;
    bool valueFrom(int id, const Json& j, float& out, std::string& error) const;
    Json paramsJson(int part, const Json* names, bool changedOnly) const;
    Json mixJson(int part) const;
    Json statusJson();
    Json stateJson(bool compact);
    bool setState(const Json& state, Json& reply);
    Json presetsJson(const Json& req);
    Json filesJson(const Json& req);
    std::string resolve(const std::string& path, const std::string& root) const;
    std::string songLoad(const std::string& path, Json& info);
    void changed() { changes_.fetch_add(1, std::memory_order_relaxed); }

    Engine& engine_;
    Config config_;
    Hooks hooks_;
    Output output_;
    std::mutex outMutex_;
    std::deque<std::vector<uint8_t>> outQueue_;

    std::mutex inMutex_;
    std::condition_variable inCv_;
    sysex::Assembler assembler_;
    std::deque<Pending> requests_;
    std::atomic<bool> run_{true};
    std::thread worker_;
    std::atomic<uint32_t> changes_{0};
    int notifySeq_ = 0;

    // subscriptions ("subscribe")
    bool subParams_ = false, subStatus_ = false, subMeters_ = false;
    int intervalMs_ = 50;
    std::chrono::steady_clock::time_point lastStatus_;
    // last values seen (for change notifications)
    std::vector<float> snap_;           // kNumParts * kNumParams
    std::vector<float> snapMix_;        // kNumParts * Mix::Count
    bool snapMulti_ = false;
    int snapSelected_ = 0;
    std::vector<std::string> snapSamples_;  // kNumParts * kNumLayers

    std::unique_ptr<PresetIndex> index_;
    std::unique_ptr<SongProgramLoader> programs_;
    std::string songLoaded_[kNumParts];
    std::string songName_;
};

}  // namespace hsynth
