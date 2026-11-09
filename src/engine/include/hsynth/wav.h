// Minimal RIFF/WAVE reader and writer (no external dependencies).
// Supports PCM 8/16/24/32-bit integer, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE,
// any channel count (decoded to at most 2 channels) and the "smpl" chunk
// (loop points and unity note).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hsynth {

struct WavData {
    int channels = 0;        // 1 or 2 after decoding
    double sampleRate = 0;
    int64_t frames = 0;
    std::vector<float> left;   // always filled
    std::vector<float> right;  // empty for mono
    // From smpl chunk (-1 if absent)
    int unityNote = -1;
    int64_t loopStart = -1;
    int64_t loopEnd = -1;  // inclusive end as stored in the file
};

// Returns true on success, otherwise fills error.
bool readWav(const std::string& path, WavData& out, std::string& error);
bool readWavMemory(const uint8_t* data, size_t size, WavData& out, std::string& error);

// Write 16-bit PCM or 32-bit float WAV, interleaving left/right.
bool writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>* right,
              int sampleRate, bool float32, std::string& error, int unityNote = -1, int64_t loopStart = -1,
              int64_t loopEnd = -1);

}  // namespace hsynth
