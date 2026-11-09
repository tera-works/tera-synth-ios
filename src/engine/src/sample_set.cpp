#include "hsynth/sample_set.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>

#include "hsynth/fileio.h"

#include "hsynth/dsp/resample.h"
#include "hsynth/flac.h"
#include "hsynth/wav.h"

namespace hsynth {

int SampleSet::streamedSamples() const {
    int n = 0;
    for (const auto& s : samples) n += s->streamed ? 1 : 0;
    return n;
}

int64_t SampleSet::diskBytes() const {
    int64_t n = 0;
    for (const auto& s : samples)
        if (s->streamed) n += (s->frames - s->preloadFrames) * s->stream.blockAlign;
    return n;
}

bool Sample::loadFully(std::string& error) {
    if (!streamed) return true;
    if (stream.flac) {
        WavData w;
        if (!readFlac(stream.path, w, error)) return false;
        data[0] = std::move(w.left);
        data[0].resize((size_t)(frames + kPadFrames), 0.0f);
        if (channels == 2) {
            data[1] = std::move(w.right);
            data[1].resize((size_t)(frames + kPadFrames), 0.0f);
        }
        streamed = false;
        preloadFrames = 0;
        return true;
    }
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(stream.path, "rb"), &std::fclose);
    if (!f) { error = "cannot open " + stream.path; return false; }
    std::vector<float> d[2];
    for (int c = 0; c < channels; ++c) d[c].assign((size_t)(frames + kPadFrames), 0.0f);
    std::vector<uint8_t> scratch;
    if (!readStreamFrames(f.get(), stream, 0, frames, d[0].data(), channels == 2 ? d[1].data() : nullptr, scratch)) {
        error = "read error " + stream.path;
        return false;
    }
    for (int c = 0; c < channels; ++c) data[c] = std::move(d[c]);
    streamed = false;
    preloadFrames = 0;
    return true;
}

void SampleSet::finalizeStreaming() {
    for (auto& sp : samples) {
        Sample* s = sp.get();
        if (!s->streamed) continue;
        bool keep = true;
        for (const Region& r : regions) {
            if (r.sample != s) continue;
            const bool loops = (r.loopMode == LoopMode::Continuous || r.loopMode == LoopMode::Sustain) && r.loopStart >= 0 &&
                               r.loopEnd > r.loopStart + 1;
            // same pitch-up estimate as prepareMips(): a mip level is needed above step 2
            const double semis = (double)(r.hikey - r.keycenter) * r.pitchKeytrack / 100.0 + r.transpose + r.tune / 100.0;
            const double step = std::exp2((semis + 2.0) / 12.0) * s->sampleRate / 44100.0;
            if (loops || step > 2.0 || r.offset > s->preloadFrames / 2) keep = false;
        }
        if (keep) continue;
        std::string err;
        if (!s->loadFully(err)) warnings.push_back("streaming: " + err);
    }
}

size_t SampleSet::memoryBytes() const {
    if (wavetable) return wavetable->memoryBytes();
    size_t n = 0;
    for (const auto& s : samples) {
        n += (s->data[0].size() + s->data[1].size()) * sizeof(float);
        for (int l = 0; l < s->mipLevels; ++l) n += (s->mip[l][0].size() + s->mip[l][1].size()) * sizeof(float);
    }
    return n;
}

void Sample::buildMips(int levels) {
    levels = std::max(0, std::min(kMaxMips, levels));
    for (int l = mipLevels; l < levels; ++l) {
        const int64_t inFrames = levelFrames(l);
        for (int c = 0; c < channels; ++c) {
            const std::vector<float>& src = l == 0 ? data[c] : mip[l - 1][c];
            resample::decimate2(src, (size_t)inFrames, mip[l][c]);
        }
        mipFrames[l] = (inFrames + 1) / 2;
    }
    mipLevels = std::max(mipLevels, levels);
}

void SampleSet::prepareMips() {
    for (auto& sp : samples) {
        Sample* s = sp.get();
        if (s->streamed) continue;  // finalizeStreaming() keeps only samples without mips streamed
        double maxSemis = -1e9;
        for (const Region& r : regions) {
            if (r.sample != s) continue;
            const double semis = (double)(r.hikey - r.keycenter) * r.pitchKeytrack / 100.0 + r.transpose + r.tune / 100.0;
            maxSemis = std::max(maxSemis, semis);
        }
        if (maxSemis < -1e8) continue;
        // +2 semitones pitch bend headroom; rate relative to the lowest common engine rate
        const double step = std::exp2((maxSemis + 2.0) / 12.0) * s->sampleRate / 44100.0;
        s->buildMips(step > 8.0 ? 3 : step > 4.0 ? 2 : step > 2.0 ? 1 : 0);
    }
}

namespace {
// position in a crossfade range [lo, hi] (callers ensure hi > lo)
float xfPos(int v, int lo, int hi) { return std::min(1.0f, std::max(0.0f, (float)(v - lo) / (float)(hi - lo))); }
float xfCurve(float t, bool power) { return power ? std::sqrt(t) : t; }
}  // namespace

float Region::crossfadeGain(int key, int vel) const {
    float g = 1.0f;
    if (xfinHiVel > xfinLoVel) g *= xfCurve(xfPos(vel, xfinLoVel, xfinHiVel), xfVelPower);
    if (xfoutHiVel > xfoutLoVel) g *= xfCurve(1.0f - xfPos(vel, xfoutLoVel, xfoutHiVel), xfVelPower);
    if (xfinHiKey > xfinLoKey) g *= xfCurve(xfPos(key, xfinLoKey, xfinHiKey), xfKeyPower);
    if (xfoutHiKey > xfoutLoKey) g *= xfCurve(1.0f - xfPos(key, xfoutLoKey, xfoutHiKey), xfKeyPower);
    return g;
}

int SampleSet::findRegions(const RegionQuery& q, const Region** out, int max) const {
    int n = 0;
    const int sw = q.keyswitch >= 0 ? q.keyswitch : swDefault;
    for (const Region& r : regions) {
        if (r.trigger != q.trigger) continue;
        if (q.key < r.lokey || q.key > r.hikey) continue;
        if (q.vel127 < r.lovel || q.vel127 > r.hivel) continue;
        if (q.rand01 < r.lorand || q.rand01 >= r.hirand) continue;
        if (r.seqLength > 1 && (int)(q.seq % (uint32_t)r.seqLength) != r.seqPosition - 1) continue;
        if (r.swLast >= 0 && r.swLast != sw) continue;
        if (r.crossfadeGain(q.key, q.vel127) <= 0.0f) continue;  // fully faded out: not started at all
        if (!r.sample) continue;
        out[n++] = &r;
        if (n >= max) break;
    }
    return n;
}

namespace {
bool endsWithFlac(const std::string& p) {
    if (p.size() < 5) return false;
    std::string e = p.substr(p.size() - 5);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".flac";
}
}  // namespace

namespace {

int64_t tell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
void seekTo(FILE* f, int64_t pos) {
#ifdef _WIN32
    _fseeki64(f, pos, SEEK_SET);
#else
    fseeko(f, (off_t)pos, SEEK_SET);
#endif
}
uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Header of a WAV file, without reading the sample data.
struct WavInfo {
    int format = 0, bits = 0, fileChannels = 0, blockAlign = 0;
    double rate = 0;
    int64_t dataOffset = -1, frames = 0;
    int unity = -1;
    bool hasLoop = false;
};

bool readWavInfo(FILE* f, WavInfo& w) {
    uint8_t h[12];
    if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    std::fseek(f, 0, SEEK_END);
    const int64_t size = tell64(f);
    int64_t pos = 12, dataSize = 0;
    while (pos + 8 <= size) {
        seekTo(f, pos);
        uint8_t ck[8];
        if (std::fread(ck, 1, 8, f) != 8) break;
        const int64_t ckSize = (int64_t)le32(ck + 4);
        const int64_t body = std::min(ckSize, size - pos - 8);
        if (std::memcmp(ck, "fmt ", 4) == 0 && body >= 16) {
            uint8_t b[40] = {};
            if (std::fread(b, 1, (size_t)std::min<int64_t>(body, 40), f) < 16) return false;
            w.format = b[0] | b[1] << 8;
            w.fileChannels = b[2] | b[3] << 8;
            w.rate = (double)le32(b + 4);
            w.blockAlign = b[12] | b[13] << 8;
            w.bits = b[14] | b[15] << 8;
            if (w.format == 0xFFFE && body >= 40) w.format = b[24] | b[25] << 8;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            w.dataOffset = pos + 8;
            dataSize = body;
        } else if (std::memcmp(ck, "smpl", 4) == 0 && body >= 36) {
            uint8_t b[36];
            if (std::fread(b, 1, 36, f) == 36) {
                w.unity = (int)le32(b + 12);
                w.hasLoop = le32(b + 28) > 0;
            }
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    const bool fmtOk = (w.format == 1 && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32)) ||
                       (w.format == 3 && (w.bits == 32 || w.bits == 64));
    if (w.dataOffset < 0 || !fmtOk || w.fileChannels <= 0 || w.rate <= 0 || w.blockAlign < w.bits / 8 * w.fileChannels)
        return false;
    w.frames = dataSize / w.blockAlign;
    return true;
}

// A streamed sample whose head is read from src.
std::unique_ptr<Sample> headFromSource(FILE* f, const std::string& path, const StreamSource& src, int channels, double rate,
                                       int64_t frames, int unity, int64_t preload) {
    auto s = std::make_unique<Sample>();
    s->path = path;
    s->channels = channels;
    s->sampleRate = rate;
    s->frames = frames;
    s->unityNote = unity;
    for (int c = 0; c < channels; ++c) s->data[c].assign((size_t)(preload + Sample::kPadFrames), 0.0f);
    std::vector<uint8_t> scratch;
    if (!readStreamFrames(f, src, 0, preload, s->data[0].data(), channels == 2 ? s->data[1].data() : nullptr, scratch))
        return nullptr;
    s->streamed = true;
    s->preloadFrames = preload;
    s->stream = src;
    return s;
}

std::unique_ptr<Sample> streamedWav(const std::string& path, int64_t preload) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) return nullptr;
    WavInfo w;
    if (!readWavInfo(f.get(), w) || w.hasLoop || w.frames <= 2 * preload) return nullptr;
    StreamSource src;
    src.path = path;
    src.dataOffset = w.dataOffset;
    src.format = w.format;
    src.bits = w.bits;
    src.blockAlign = w.blockAlign;
    src.fileChannels = w.fileChannels;
    return headFromSource(f.get(), path, src, w.fileChannels >= 2 ? 2 : 1, w.rate, w.frames, w.unity, preload);
}

// FLAC: only the head is decoded now; the streamer decodes the rest frame by
// frame from the compressed file (nothing decoded is kept on disk).
std::unique_ptr<Sample> streamedFlac(const std::string& path, int64_t preload) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) return nullptr;
    std::fseek(f.get(), 0, SEEK_END);
    const long size = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    if (size <= 0) return nullptr;
    // the head is in the first part of the file (a whole read if it is not)
    for (size_t want = std::min<size_t>((size_t)size, 256u << 10);; want = (size_t)size) {
        std::vector<uint8_t> bytes(want);
        std::fseek(f.get(), 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, want, f.get()) != want) return nullptr;
        FlacFrameDecoder dec;
        std::string err;
        if (!dec.open(bytes.data(), bytes.size(), err)) return nullptr;
        // unknown length or a loop in the file: decoded fully by the caller
        if (dec.totalFrames() <= 2 * preload || dec.loopStart() >= 0) return nullptr;
        auto s = std::make_unique<Sample>();
        s->path = path;
        s->channels = dec.channels();
        s->sampleRate = dec.sampleRate();
        s->frames = dec.totalFrames();
        s->unityNote = dec.unityNote();
        for (int c = 0; c < s->channels; ++c) s->data[c].assign((size_t)(preload + Sample::kPadFrames), 0.0f);
        std::vector<float> l, r;
        int64_t got = 0;
        bool ok = true;
        while (got < preload) {
            const int n = dec.next(l, r, err);
            if (n <= 0) { ok = false; break; }
            const int64_t k = std::min<int64_t>(n, preload - got);
            std::memcpy(s->data[0].data() + got, l.data(), sizeof(float) * (size_t)k);
            if (s->channels == 2) std::memcpy(s->data[1].data() + got, r.data(), sizeof(float) * (size_t)k);
            got += k;
        }
        if (!ok) {
            if (want == (size_t)size) return nullptr;
            continue;  // the head runs past the part read: read it all
        }
        s->streamed = true;
        s->preloadFrames = preload;
        s->stream.path = path;
        s->stream.flac = true;
        s->stream.blockAlign = 2 * s->channels;  // diskBytes(): counted like 16-bit PCM
        s->stream.fileChannels = s->channels;
        return s;
    }
}

}  // namespace

// WAV or FLAC (by extension). stream: keep only the head in memory when the
// sample is long enough and has no loop in the file (see streaming.h).
std::unique_ptr<Sample> sampleFromWav(const std::string& path, std::string& error, bool stream) {
    if (stream) {
        const int64_t preload = sampleLoadOptions().preloadFrames;
        if (auto s = endsWithFlac(path) ? streamedFlac(path, preload) : streamedWav(path, preload)) return s;
    }
    WavData w;
    if (!(endsWithFlac(path) ? readFlac(path, w, error) : readWav(path, w, error))) return nullptr;
    auto s = std::make_unique<Sample>();
    s->path = path;
    s->channels = w.channels;
    s->sampleRate = w.sampleRate;
    s->frames = w.frames;
    s->unityNote = w.unityNote;
    if (w.loopStart >= 0) {
        s->loopStart = w.loopStart;
        s->loopEnd = w.loopEnd + 1;  // smpl end is inclusive
    }
    s->data[0] = std::move(w.left);
    s->data[0].resize((size_t)w.frames + Sample::kPadFrames, 0.0f);
    if (w.channels == 2) {
        s->data[1] = std::move(w.right);
        s->data[1].resize((size_t)w.frames + Sample::kPadFrames, 0.0f);
    }
    return s;
}

namespace {

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

}  // namespace

std::unique_ptr<SampleSet> loadWavSampleSet(const std::string& path, std::string& error) {
    auto s = sampleFromWav(path, error, false);  // one sample over the whole keyboard: needs mips, kept in memory
    if (!s) return nullptr;
    auto set = std::make_unique<SampleSet>();
    set->name = baseName(path);
    set->sourcePath = path;
    Region r;
    r.sample = s.get();
    r.keycenter = s->unityNote >= 0 && s->unityNote < 128 ? s->unityNote : 60;
    if (s->loopStart >= 0) {
        r.loopMode = LoopMode::Continuous;
        r.loopStart = s->loopStart;
        r.loopEnd = s->loopEnd;
    }
    set->regions.push_back(r);
    set->samples.push_back(std::move(s));
    set->prepareMips();
    return set;
}

std::unique_ptr<SampleSet> loadSampleSet(const std::string& path, std::string& error) {
    const std::string ext = lowerExt(path);
    if (ext == "wav" || ext == "wave" || ext == "flac") return loadWavSampleSet(path, error);
    if (ext == "sfz") return loadSfzSampleSet(path, error);
    error = "unsupported file type: " + path;
    return nullptr;
}

}  // namespace hsynth
