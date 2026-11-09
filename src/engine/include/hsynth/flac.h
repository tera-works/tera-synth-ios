// FLAC decoder (no external dependencies). readFlac() decodes a whole file into
// memory (loader thread only), like the WAV reader; FlacFrameDecoder decodes
// it frame by frame. Supports every encoding the
// reference encoder produces: 4..32 bits, 1..8 channels (downmixed to at most
// 2 like WAV), fixed / LPC / verbatim / constant subframes, all stereo
// decorrelation modes, variable block sizes. Loop points / root key are read
// from a RIFF "smpl" chunk stored in an APPLICATION "riff" block (flac
// --keep-foreign-metadata) when present.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hsynth/wav.h"

namespace hsynth {

bool readFlac(const std::string& path, WavData& out, std::string& error);
bool readFlacMemory(const uint8_t* data, size_t size, WavData& out, std::string& error);

// Frame-by-frame decoding of FLAC data in memory (the caller keeps the data
// alive). Used by readFlacMemory() and by disk streaming, which decodes a
// sample a few frames at a time on the streamer thread instead of keeping it
// decoded anywhere.
class FlacFrameDecoder {
public:
    // Parses the metadata. false: not FLAC / no STREAMINFO / unsupported.
    bool open(const uint8_t* data, size_t size, std::string& error);
    int sampleRate() const { return rate_; }
    int channels() const { return outChannels_; }   // 1 or 2 (more are reduced to front left / right)
    int64_t totalFrames() const { return total_; }   // from STREAMINFO, 0 = unknown
    int unityNote() const { return unity_; }          // from an embedded RIFF smpl chunk, -1 = none
    int64_t loopStart() const { return loopStart_; }  // -1 = none
    int64_t loopEnd() const { return loopEnd_; }      // inclusive, like smpl
    int64_t position() const { return decoded_; }     // frames decoded so far
    // Decodes the next frame into left / right (resized to its length; right
    // stays empty for mono). Returns the frame count, 0 at the end of the
    // data, -1 on corrupt data (error set). Frames past totalFrames() are cut.
    int next(std::vector<float>& left, std::vector<float>& right, std::string& error);

private:
    const uint8_t* d_ = nullptr;
    size_t n_ = 0, pos_ = 0;
    int rate_ = 0, fileChannels_ = 0, outChannels_ = 0, bps_ = 0, unity_ = -1;
    int64_t total_ = 0, decoded_ = 0, loopStart_ = -1, loopEnd_ = -1;
    std::vector<int64_t> sub_[8];
};

}  // namespace hsynth
