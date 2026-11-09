// Disk streaming of large samples (pianos and other multi-gigabyte libraries).
//
// A streamed Sample keeps only its first preloadFrames in memory. When a voice
// starts it, the audio thread claims a StreamSlot (lock-free) and the
// DiskStreamer thread reads the rest of the sample into the slot's ring buffer
// ahead of the play position. The audio thread never touches files and never
// waits: frames that have not arrived yet play as silence and are counted as
// underruns (EngineStats::streamUnderruns).
//
// Streaming is used for samples that are played forward without loops and
// without mip levels (a sample pitched up more than an octave is kept in
// memory), which is what large multi-sampled instruments consist of.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hsynth {

struct Sample;
struct SampleSet;

// Where the frames of a streamed sample are on disk: interleaved PCM in a WAV
// file, or a FLAC file that the streamer decodes frame by frame (flac = true;
// the PCM fields are then unused).
struct StreamSource {
    std::string path;
    bool flac = false;
    int64_t dataOffset = 0;   // byte offset of frame 0
    int format = 1;           // 1 = integer PCM, 3 = IEEE float
    int bits = 16;            // bits per sample
    int blockAlign = 2;       // bytes per frame (all file channels)
    int fileChannels = 1;     // channels in the file (the first two are used)
};

// Reads frames [start, start + count) of channel 0 / 1 (right may be null).
// scratch is reused between calls. Returns false on I/O errors (the output is
// zero-filled from the failing frame on).
bool readStreamFrames(FILE* f, const StreamSource& src, int64_t start, int64_t count, float* left, float* right,
                      std::vector<uint8_t>& scratch);

// Global loader options (any thread; read when a sample set is loaded).
enum class StreamMode : int { Off = 0, Auto = 1, Always = 2 };
struct SampleLoadOptions {
    StreamMode mode = StreamMode::Auto;
    int64_t autoThresholdBytes = 32ll << 20;  // Auto: stream sets whose sample files exceed this
    int64_t preloadFrames = 16384;            // frames kept in memory per streamed sample (~0.34 s)
    std::string cacheDir;                     // unused (FLAC used to be decoded into a cache; it streams directly now)
};
void setSampleLoadOptions(const SampleLoadOptions& o);
SampleLoadOptions sampleLoadOptions();

struct StreamFlacState;  // streaming.cpp

struct StreamSlot {
    static constexpr int kRing = 32768;   // frames per channel in the ring
    static constexpr int kMirror = 32;    // ring[0..kMirror) repeated after the end: any window <= kMirror is contiguous
    enum : int { kFree = 0, kActive = 1, kReleased = 2 };

    std::atomic<int> state{kFree};
    // set by the audio thread while the slot is free, read by the streamer
    const Sample* sample = nullptr;
    int64_t start = 0;                    // first streamed frame
    std::atomic<int64_t> written{0};      // frames [start, written) have arrived
    std::atomic<int64_t> low{0};          // the player no longer reads frames below this
    std::unique_ptr<float[]> ring[2];
    // streamer thread only
    FILE* file = nullptr;
    std::shared_ptr<StreamFlacState> flac;  // FLAC: compressed data + decoder (kept for reuse)

    // Audio thread: pointer to frames [first, first + count) of channel c
    // (count <= kMirror), nullptr if they are not in the ring (yet).
    const float* window(int c, int64_t first, int count) const {
        if (first < start) return nullptr;
        const int64_t w = written.load(std::memory_order_acquire);
        if (first + count > w || first < w - kRing) return nullptr;
        return ring[c].get() + (first % kRing);
    }
};

class DiskStreamer {
public:
    static constexpr int kSlots = 384;

    DiskStreamer() = default;
    ~DiskStreamer() { stop(); }
    DiskStreamer(const DiskStreamer&) = delete;
    DiskStreamer& operator=(const DiskStreamer&) = delete;

    // Non-audio thread: allocates the slots and starts the thread (idempotent).
    void start();
    void stop();
    bool running() const { return running_.load(); }

    // Audio thread. nullptr if every slot is in use.
    StreamSlot* claim(const Sample* s, int64_t startFrame);
    void release(StreamSlot* slot);
    void underrun() { underruns_.fetch_add(1, std::memory_order_relaxed); }

    // Non-audio thread: waits until no slot refers to a sample of `set`
    // (before the set is deleted). false: still in use after 2 s.
    bool waitUnused(const SampleSet& set);

    // Offline rendering (export, tests): players wait for the disk instead of
    // playing silence. Never enable while a real-time audio callback runs.
    void setOffline(bool on) { offline_.store(on); }
    bool offline() const { return offline_.load(std::memory_order_relaxed); }
    void waitFor(const StreamSlot& s, int64_t frameEnd) const;

    uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    int activeSlots() const { return active_.load(std::memory_order_relaxed); }
    uint64_t bytesRead() const { return bytesRead_.load(std::memory_order_relaxed); }

private:
    void threadMain();
    bool fill(StreamSlot& s);  // one chunk; true if something was read
    bool fillFlac(StreamSlot& s, int64_t w, int64_t n);  // decodes frames [w, w + n) into tmp_

    std::unique_ptr<StreamSlot[]> slots_;
    std::thread thread_;
    std::atomic<bool> running_{false}, quit_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> offline_{false};
    int claimHint_ = 0;                       // audio thread
    std::atomic<uint32_t> underruns_{0};
    std::atomic<int> active_{0};
    std::atomic<uint64_t> bytesRead_{0};
    std::vector<uint8_t> scratch_;            // streamer thread
    std::vector<float> tmp_[2];
};

}  // namespace hsynth
