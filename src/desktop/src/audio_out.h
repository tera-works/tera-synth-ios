// Desktop audio output (Windows WASAPI today; ALSA / PulseAudio / CoreAudio via
// the same code on Linux, Raspberry Pi and macOS) using miniaudio.
// Plays the role that android/app/src/main/cpp/audio_engine.* plays on Android.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsynth/engine.h"

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
};

struct AudioOutStats {
    bool running = false;
    std::string backend;
    std::string deviceName;
    int sampleRate = 0;
    int periodFrames = 0;       // requested / actual callback size
    int periods = 0;
    bool exclusive = false;
    int lastCallbackFrames = 0;
    float callbackUsAvg = 0, callbackUsMax = 0, callbackLoadMax = 0;
    int lateCallbacks = 0;      // callback arrived much later than expected (likely audible glitch)
    uint64_t callbacks = 0;
    bool input = false;         // capture device open (vocoder modulator)
    std::string inputName;
    float inputPeak = 0;        // since last resetPeaks
};

class AudioOut {
public:
    explicit AudioOut(hsynth::Engine& engine);
    ~AudioOut();

    std::vector<AudioDeviceInfo> listDevices();
    std::vector<AudioDeviceInfo> listInputDevices();
    // deviceIndex -1 = system default. periodFrames 0 = backend default.
    // inputDevice: -2 = no input, -1 = default capture device, else index from
    // listInputDevices(). The input feeds Engine::setInputBuffer (vocoder).
    bool start(int deviceIndex, int sampleRate, int periodFrames, bool exclusive, int inputDevice, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    AudioOutStats stats();
    void resetPeaks();
    // Muted: the device gets silence; the scope, the recording and the
    // timing statistics still see the engine output (silent tests).
    void setMuted(bool m) { muted_.store(m); }
    bool muted() const { return muted_.load(); }

    // Oscilloscope tap (written by the audio thread, read by UI; tearing is fine).
    static constexpr int kScopeSize = 4096;
    void copyScope(float* left, float* right, int n) const;

    // Recording to memory (UI thread starts/stops, audio thread writes).
    void startRecording(double seconds, bool fromInput = false);  // fromInput: the audio input (sampling)
    // Returns recorded frames (interleaved stereo in out) and stops recording.
    size_t stopRecording(std::vector<float>& out);
    bool recording() const { return recording_.load(); }
    double recordedSeconds() const;

    // Recording straight to a WAV file (32-bit float stereo), any length: the
    // audio thread fills a ring, a writer thread appends it to the file.
    bool startFileRecording(const std::string& path, std::string& error);
    double stopFileRecording();  // seconds written (the header is completed)
    bool fileRecording() const { return fileRec_.load(); }
    double fileRecordSeconds() const;

    void audioCallback(float* out, const float* in, uint32_t frames, uint32_t channels, uint32_t inChannels);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    hsynth::Engine& engine_;
    std::mutex lock_;
    std::atomic<bool> running_{false};

    float left_[hsynth::kMaxBlockSize];
    float right_[hsynth::kMaxBlockSize];
    float inL_[hsynth::kMaxBlockSize];
    float inR_[hsynth::kMaxBlockSize];
    std::atomic<float> inPeak_{0};

    float scopeL_[kScopeSize] = {}, scopeR_[kScopeSize] = {};
    std::atomic<int> scopePos_{0};

    std::vector<float> recBuf_;
    std::atomic<size_t> recPos_{0};
    std::atomic<bool> recording_{false};
    std::atomic<bool> recInput_{false};
    // file recording
    void fileWriter();
    std::vector<float> fileRing_;              // interleaved stereo, power of two
    std::atomic<size_t> ringWrite_{0}, ringRead_{0};
    std::atomic<bool> fileRec_{false}, fileQuit_{false};
    std::atomic<uint64_t> fileFrames_{0};
    std::thread fileThread_;
    void* fileHandle_ = nullptr;               // FILE*
    std::atomic<bool> muted_{false};

    std::atomic<float> cbUsAvg_{0}, cbUsMax_{0}, cbLoadMax_{0};
    std::atomic<int> lastFrames_{0}, late_{0};
    std::atomic<uint64_t> callbacks_{0};
    int64_t lastCallbackNs_ = 0;
    int sampleRate_ = 48000;
};
