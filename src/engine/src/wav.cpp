#include "hsynth/wav.h"
#include "hsynth/fileio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace hsynth {

namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
void wr32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i)));
}
void wrTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

}  // namespace

bool readWavMemory(const uint8_t* data, size_t size, WavData& out, std::string& error) {
    out = WavData{};
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    int format = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmSize = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* ck = data + pos;
        const uint32_t ckSize = rd32(ck + 4);
        const uint8_t* body = ck + 8;
        const size_t avail = size - pos - 8;
        const size_t bodySize = ckSize > avail ? avail : ckSize;
        if (std::memcmp(ck, "fmt ", 4) == 0 && bodySize >= 16) {
            format = rd16(body);
            channels = rd16(body + 2);
            rate = rd32(body + 4);
            blockAlign = rd16(body + 12);
            bits = rd16(body + 14);
            if (format == 0xFFFE && bodySize >= 40) format = rd16(body + 24);  // extensible subformat
        } else if (std::memcmp(ck, "data", 4) == 0) {
            pcm = body;
            pcmSize = bodySize;
        } else if (std::memcmp(ck, "smpl", 4) == 0 && bodySize >= 36) {
            out.unityNote = (int)rd32(body + 12);
            const uint32_t numLoops = rd32(body + 28);
            if (numLoops > 0 && bodySize >= 36 + 24) {
                out.loopStart = rd32(body + 36 + 8);
                out.loopEnd = rd32(body + 36 + 12);
            }
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    if (!pcm) { error = "missing data chunk"; return false; }
    if (channels <= 0 || rate == 0 || blockAlign <= 0) { error = "missing or invalid fmt chunk"; return false; }
    const bool isFloat = format == 3;
    if (!(format == 1 || format == 3)) { error = "unsupported WAV encoding (format " + std::to_string(format) + ")"; return false; }
    if (!isFloat && !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) { error = "unsupported bit depth " + std::to_string(bits); return false; }
    if (isFloat && !(bits == 32 || bits == 64)) { error = "unsupported float bit depth " + std::to_string(bits); return false; }

    const int bytes = bits / 8;
    // the frame stride must hold every channel that is decoded (a smaller
    // blockAlign used to read past the end of the data chunk)
    if (blockAlign < bytes * channels) { error = "inconsistent fmt chunk (block align " + std::to_string(blockAlign) + ")"; return false; }
    const int64_t frames = (int64_t)(pcmSize / (size_t)blockAlign);
    out.channels = channels >= 2 ? 2 : 1;
    out.sampleRate = rate;
    out.frames = frames;
    out.left.resize((size_t)frames);
    if (out.channels == 2) out.right.resize((size_t)frames);

    auto decode = [&](const uint8_t* p) -> float {
        if (isFloat) {
            if (bits == 32) { float f; std::memcpy(&f, p, 4); return f; }
            double d; std::memcpy(&d, p, 8); return (float)d;
        }
        switch (bits) {
            case 8: return ((int)p[0] - 128) / 128.0f;
            case 16: return (int16_t)rd16(p) / 32768.0f;
            case 24: {
                int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
                return v / 8388608.0f;
            }
            default: return (int32_t)rd32(p) / 2147483648.0f;
        }
    };
    for (int64_t i = 0; i < frames; ++i) {
        const uint8_t* f = pcm + i * blockAlign;
        out.left[(size_t)i] = decode(f);
        if (out.channels == 2) out.right[(size_t)i] = decode(f + bytes);
    }
    if (out.loopStart >= frames || out.loopEnd >= frames || out.loopEnd <= out.loopStart) {
        out.loopStart = out.loopEnd = -1;
    }
    return true;
}

bool readWav(const std::string& path, WavData& out, std::string& error) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) { error = "cannot open " + path; return false; }
    std::fseek(f.get(), 0, SEEK_END);
    const long len = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    if (len <= 0) { error = "empty file " + path; return false; }
    // a directory opens on POSIX and reports a bogus size; RIFF cannot exceed 4 GB
    if (len > 0xFFFFFFFFL) { error = "not a WAV file (directory or too large): " + path; return false; }
    std::vector<uint8_t> buf((size_t)len);
    if (std::fread(buf.data(), 1, buf.size(), f.get()) != buf.size()) { error = "read error " + path; return false; }
    if (!readWavMemory(buf.data(), buf.size(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>* right,
              int sampleRate, bool float32, std::string& error, int unityNote, int64_t loopStart, int64_t loopEnd) {
    const int ch = right ? 2 : 1;
    const int bytes = float32 ? 4 : 2;
    // channels of different length: the file is as long as the longer one, the
    // shorter one is padded with silence (it used to be read out of bounds)
    const uint32_t frames = (uint32_t)std::max(left.size(), right ? right->size() : (size_t)0);
    std::vector<uint8_t> v;
    v.reserve(64 + frames * ch * bytes);
    wrTag(v, "RIFF"); wr32(v, 0); wrTag(v, "WAVE");
    wrTag(v, "fmt "); wr32(v, 16);
    wr16(v, float32 ? 3 : 1); wr16(v, (uint16_t)ch); wr32(v, (uint32_t)sampleRate);
    wr32(v, (uint32_t)(sampleRate * ch * bytes)); wr16(v, (uint16_t)(ch * bytes)); wr16(v, (uint16_t)(bytes * 8));
    if (unityNote >= 0 || loopStart >= 0) {
        wrTag(v, "smpl");
        const bool loop = loopStart >= 0 && loopEnd > loopStart;
        wr32(v, 36 + (loop ? 24 : 0));
        wr32(v, 0); wr32(v, 0);
        wr32(v, (uint32_t)(1000000000.0 / sampleRate));
        wr32(v, (uint32_t)(unityNote >= 0 ? unityNote : 60));
        wr32(v, 0); wr32(v, 0); wr32(v, 0);
        wr32(v, loop ? 1 : 0); wr32(v, 0);
        if (loop) {
            wr32(v, 0); wr32(v, 0);
            wr32(v, (uint32_t)loopStart); wr32(v, (uint32_t)loopEnd);
            wr32(v, 0); wr32(v, 0);
        }
    }
    wrTag(v, "data"); wr32(v, frames * ch * bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        for (int c = 0; c < ch; ++c) {
            const std::vector<float>& src = c == 0 ? left : *right;
            float s = i < src.size() ? src[i] : 0.0f;
            if (float32) {
                uint32_t u; std::memcpy(&u, &s, 4); wr32(v, u);
            } else {
                s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
                wr16(v, (uint16_t)(int16_t)std::lround(s * 32767.0f));
            }
        }
    }
    const uint32_t riffSize = (uint32_t)v.size() - 8;
    std::memcpy(&v[4], &riffSize, 4);  // little-endian hosts (all supported targets)
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "wb"), &std::fclose);
    if (!f) { error = "cannot create " + path; return false; }
    if (std::fwrite(v.data(), 1, v.size(), f.get()) != v.size()) { error = "write error " + path; return false; }
    return true;
}

}  // namespace hsynth
