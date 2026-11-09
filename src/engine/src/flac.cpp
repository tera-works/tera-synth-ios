#include "hsynth/flac.h"

#include <algorithm>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#include <fstream>
#include <vector>

#include "hsynth/fileio.h"

namespace hsynth {

namespace {

// MSB-first bit reader over a byte buffer with a 64-bit refill buffer: bit
// fields, unary codes and Rice codes take a few operations instead of a loop
// per bit. Reads past the end return zero bits and set the overrun flag.
class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return !overrun_; }
    size_t bytePos() const { return bitPos() >> 3; }
    void alignByte() { seekBit((bitPos() + 7) & ~(size_t)7); }
    void seekByte(size_t b) { seekBit(b << 3); }
    bool atEnd() const { return bytePos() >= n_; }

    uint32_t bits(int count) {  // count <= 32
        if (count == 0) return 0;
        if (cnt_ < count) refill();
        const uint32_t v = (uint32_t)(buf_ >> (64 - count));
        consume(count);
        return v;
    }
    uint64_t bits64(int count) {  // count <= 64
        if (count <= 32) return bits(count);
        const uint64_t hi = bits(count - 32);
        return (hi << 32) | bits(32);
    }
    int64_t sbits(int count) {
        if (count == 0) return 0;
        const uint64_t v = bits64(count);
        const uint64_t sign = (uint64_t)1 << (count - 1);
        return (int64_t)(v ^ sign) - (int64_t)sign;
    }
    uint32_t bit() { return bits(1); }
    // number of 0 bits before the next 1 (which is consumed)
    uint32_t unary() {
        uint32_t q = 0;
        for (;;) {
            if (cnt_ < 57) refill();
            if (buf_) {
                const int z = clz64(buf_);
                if (z < cnt_) {
                    q += (uint32_t)z;
                    consume(z + 1);
                    return q;
                }
            }
            q += (uint32_t)cnt_;
            consume(cnt_);
            if (overrun_) return q;  // ran into the zero padding past the end
        }
    }
    int64_t rice(int param) {
        const uint64_t q = unary();
        const uint64_t u = (q << param) | (param ? bits(param) : 0);
        return (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
    }

private:
    static int clz64(uint64_t x) {
#if defined(_MSC_VER) && !defined(__clang__)
        unsigned long i;
        _BitScanReverse64(&i, x);
        return 63 - (int)i;
#else
        return __builtin_clzll(x);
#endif
    }
    size_t bitPos() const { return next_ * 8 + pad_ - (size_t)cnt_; }
    void seekBit(size_t pos) {
        next_ = pos >> 3;
        buf_ = 0;
        cnt_ = 0;
        pad_ = 0;
        const int skip = (int)(pos & 7);
        if (skip) {
            refill();
            consume(skip);
        }
    }
    // tops the buffer up to at least 57 bits (zero bits past the end, counted in pad_)
    void refill() {
        while (cnt_ <= 56) {
            uint64_t b = 0;
            if (next_ < n_) b = d_[next_++];
            else pad_ += 8;
            buf_ |= b << (56 - cnt_);
            cnt_ += 8;
        }
    }
    void consume(int count) {
        buf_ = count >= 64 ? 0 : buf_ << count;
        cnt_ -= count;
        if (pad_ > 0 && (size_t)cnt_ < pad_) overrun_ = true;  // consumed padding bits
    }

    const uint8_t* d_;
    size_t n_;
    size_t next_ = 0;  // next byte to load
    size_t pad_ = 0;   // zero bits appended past the end
    uint64_t buf_ = 0; // MSB-aligned
    int cnt_ = 0;      // valid bits in buf_
    bool overrun_ = false;
};

// Wrapping arithmetic: valid streams never overflow, corrupt ones must not be UB.
inline int64_t wadd(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
inline int64_t wmul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }
inline int64_t wneg(int64_t a) { return (int64_t)(0 - (uint64_t)a); }

bool decodeResidual(BitReader& br, int blockSize, int order, int64_t* out, std::string& err) {
    const int method = (int)br.bits(2);
    if (method > 1) { err = "reserved residual coding method"; return false; }
    const int paramBits = method == 0 ? 4 : 5;
    const uint32_t escape = method == 0 ? 15u : 31u;
    const int partOrder = (int)br.bits(4);
    const int parts = 1 << partOrder;
    if ((blockSize >> partOrder) < order || (blockSize & (parts - 1)) != 0) { err = "bad residual partition"; return false; }
    int i = order;
    for (int p = 0; p < parts; ++p) {
        const int count = (blockSize >> partOrder) - (p == 0 ? order : 0);
        const uint32_t param = br.bits(paramBits);
        if (param == escape) {
            const int nb = (int)br.bits(5);
            for (int k = 0; k < count; ++k) out[i++] = br.sbits(nb);
        } else {
            for (int k = 0; k < count; ++k) out[i++] = br.rice((int)param);
        }
        if (!br.ok()) { err = "truncated residual"; return false; }
    }
    return true;
}

bool decodeSubframe(BitReader& br, int blockSize, int bps, int64_t* out, std::string& err) {
    if (br.bit() != 0) { err = "bad subframe padding"; return false; }
    const int type = (int)br.bits(6);
    int wasted = 0;
    if (br.bit()) wasted = (int)br.unary() + 1;
    bps -= wasted;
    if (bps <= 0 || bps > 33) { err = "bad subframe bit depth"; return false; }
    if (type == 0) {  // constant
        const int64_t v = br.sbits(bps);
        std::fill(out, out + blockSize, v);
    } else if (type == 1) {  // verbatim
        for (int i = 0; i < blockSize; ++i) out[i] = br.sbits(bps);
    } else if (type >= 8 && type <= 12) {  // fixed predictor
        const int order = type - 8;
        if (order > blockSize) { err = "fixed order > block size"; return false; }
        for (int i = 0; i < order; ++i) out[i] = br.sbits(bps);
        if (!decodeResidual(br, blockSize, order, out, err)) return false;
        for (int i = order; i < blockSize; ++i) {
            static const int64_t kC[5][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, -1, 0, 0}, {3, -3, 1, 0}, {4, -6, 4, -1}};
            int64_t pred = 0;
            for (int k = 0; k < order; ++k) pred = wadd(pred, wmul(kC[order][k], out[i - 1 - k]));
            out[i] = wadd(out[i], pred);
        }
    } else if (type >= 32) {  // LPC
        const int order = type - 31;
        if (order > blockSize) { err = "lpc order > block size"; return false; }
        for (int i = 0; i < order; ++i) out[i] = br.sbits(bps);
        const int precision = (int)br.bits(4) + 1;
        if (precision == 16) { err = "bad lpc precision"; return false; }
        const int shift = (int)br.sbits(5);
        if (shift < 0) { err = "negative lpc shift"; return false; }
        int64_t coef[32];
        for (int i = 0; i < order; ++i) coef[i] = br.sbits(precision);
        if (!decodeResidual(br, blockSize, order, out, err)) return false;
        for (int i = order; i < blockSize; ++i) {
            int64_t sum = 0;
            for (int k = 0; k < order; ++k) sum = wadd(sum, wmul(coef[k], out[i - 1 - k]));
            out[i] = wadd(out[i], sum >> shift);
        }
    } else {
        err = "reserved subframe type";
        return false;
    }
    if (wasted)
        for (int i = 0; i < blockSize; ++i) out[i] = wmul(out[i], (int64_t)1 << wasted);
    return br.ok() || (err = "truncated subframe", false);
}

// RIFF "smpl" inside an APPLICATION "riff" block (flac --keep-foreign-metadata).
void parseForeignRiff(const uint8_t* d, size_t n, WavData& out) {
    for (size_t i = 0; i + 8 <= n; ++i) {
        if (d[i] != 's' || d[i + 1] != 'm' || d[i + 2] != 'p' || d[i + 3] != 'l') continue;
        auto u32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
        const size_t body = i + 8;
        if (body + 36 > n) return;
        out.unityNote = (int)u32(body + 12);
        if (u32(body + 28) > 0 && body + 36 + 24 <= n) {
            out.loopStart = u32(body + 36 + 8);
            out.loopEnd = u32(body + 36 + 12);
        }
        return;
    }
}

}  // namespace

bool FlacFrameDecoder::open(const uint8_t* data, size_t size, std::string& error) {
    *this = FlacFrameDecoder();
    if (size < 8 || data[0] != 'f' || data[1] != 'L' || data[2] != 'a' || data[3] != 'C') {
        error = "not a FLAC file";
        return false;
    }
    WavData meta;  // loop / root key from a RIFF smpl chunk
    size_t pos = 4;
    bool last = false, haveInfo = false;
    while (!last) {
        if (pos + 4 > size) { error = "truncated metadata"; return false; }
        last = (data[pos] & 0x80) != 0;
        const int type = data[pos] & 0x7F;
        const size_t len = (size_t)data[pos + 1] << 16 | (size_t)data[pos + 2] << 8 | data[pos + 3];
        pos += 4;
        if (pos + len > size) { error = "truncated metadata block"; return false; }
        if (type == 0 && len >= 34) {
            BitReader br(data + pos, len);
            br.bits(16);  // min block
            br.bits(16);  // max block
            br.bits(24);
            br.bits(24);
            rate_ = (int)br.bits(20);
            fileChannels_ = (int)br.bits(3) + 1;
            bps_ = (int)br.bits(5) + 1;
            total_ = (int64_t)br.bits64(36);
            haveInfo = true;
        } else if (type == 2 && len > 4 && std::equal(data + pos, data + pos + 4, (const uint8_t*)"riff")) {
            parseForeignRiff(data + pos + 4, len - 4, meta);
        }
        pos += len;
    }
    if (!haveInfo || rate_ <= 0) { error = "missing STREAMINFO"; return false; }
    if (bps_ < 4 || bps_ > 32) { error = "unsupported bit depth " + std::to_string(bps_); return false; }
    d_ = data;
    n_ = size;
    pos_ = pos;
    outChannels_ = std::min(2, fileChannels_);
    unity_ = meta.unityNote;
    loopStart_ = meta.loopStart;
    loopEnd_ = meta.loopEnd;
    return true;
}

int FlacFrameDecoder::next(std::vector<float>& left, std::vector<float>& right, std::string& error) {
    if (!d_) { error = "not open"; return -1; }
    static const int kBps[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    BitReader br(d_, n_);
    br.seekByte(pos_);
    while (!br.atEnd()) {
        if (total_ > 0 && decoded_ >= total_) break;
        const size_t frameStart = br.bytePos();
        if (br.bits(14) != 0x3FFE) {  // resync: scan for the next frame header
            br.seekByte(frameStart + 1);
            if (frameStart + 2 >= n_) break;
            continue;
        }
        br.bit();  // reserved
        br.bit();  // blocking strategy
        const int bsCode = (int)br.bits(4);
        const int srCode = (int)br.bits(4);
        const int chCode = (int)br.bits(4);
        const int bpsCode = (int)br.bits(3);
        br.bit();
        // UTF-8-style coded frame / sample number
        const uint32_t first = br.bits(8);
        int extra = 0;
        for (uint32_t m = 0x80; m && (first & m); m >>= 1) ++extra;
        if (extra > 0) --extra;
        for (int i = 0; i < extra; ++i) br.bits(8);
        int blockSize = 0;
        if (bsCode == 1) blockSize = 192;
        else if (bsCode >= 2 && bsCode <= 5) blockSize = 576 << (bsCode - 2);
        else if (bsCode == 6) blockSize = (int)br.bits(8) + 1;
        else if (bsCode == 7) blockSize = (int)br.bits(16) + 1;
        else if (bsCode >= 8) blockSize = 256 << (bsCode - 8);
        if (srCode == 12) br.bits(8);
        else if (srCode == 13 || srCode == 14) br.bits(16);
        br.bits(8);  // CRC-8
        const int bps = bpsCode == 0 ? bps_ : kBps[bpsCode];
        const int nch = chCode < 8 ? chCode + 1 : 2;
        if (blockSize <= 0 || bps == 0 || chCode > 10 || nch != fileChannels_ || !br.ok()) {
            br.seekByte(frameStart + 1);  // false sync
            continue;
        }
        for (int c = 0; c < nch; ++c) {
            sub_[c].resize((size_t)blockSize);
            // the side channel carries one extra bit
            const bool side = (chCode == 8 && c == 1) || (chCode == 9 && c == 0) || (chCode == 10 && c == 1);
            if (!decodeSubframe(br, blockSize, bps + (side ? 1 : 0), sub_[c].data(), error)) {
                error = "frame at byte " + std::to_string(frameStart) + ": " + error;
                pos_ = n_;
                return -1;
            }
        }
        br.alignByte();
        br.bits(16);  // CRC-16
        pos_ = br.bytePos();
        int64_t* a = sub_[0].data();
        int64_t* b = nch > 1 ? sub_[1].data() : nullptr;
        for (int i = 0; i < blockSize; ++i) {
            switch (chCode) {
                case 8: b[i] = wadd(a[i], wneg(b[i])); break;  // left / side
                case 9: a[i] = wadd(a[i], b[i]); break;        // side / right
                case 10: {                                     // mid / side
                    const int64_t mid = wmul(a[i], 2) | (b[i] & 1);
                    a[i] = wadd(mid, b[i]) >> 1;
                    b[i] = wadd(mid, wneg(b[i])) >> 1;
                    break;
                }
                default: break;
            }
        }
        int frames = blockSize;
        if (total_ > 0) frames = (int)std::min<int64_t>(frames, total_ - decoded_);
        const float scale = 1.0f / (float)((int64_t)1 << (bps - 1));
        left.resize((size_t)frames);
        if (outChannels_ == 2) right.resize((size_t)frames);
        else right.clear();
        for (int i = 0; i < frames; ++i) {
            // >2 channels: front left / right only (same as the WAV reader keeping two)
            left[(size_t)i] = (float)a[i] * scale;
            if (outChannels_ == 2) right[(size_t)i] = (float)b[i] * scale;
        }
        decoded_ += frames;
        return frames;
    }
    pos_ = n_;
    return 0;
}

bool readFlacMemory(const uint8_t* data, size_t size, WavData& out, std::string& error) {
    FlacFrameDecoder dec;
    if (!dec.open(data, size, error)) return false;
    std::vector<float> ch[2], l, r;
    const int64_t total = dec.totalFrames();
    if (total > 0 && total < (1ll << 31)) {
        ch[0].reserve((size_t)total);
        if (dec.channels() == 2) ch[1].reserve((size_t)total);
    }
    for (;;) {
        const int n = dec.next(l, r, error);
        if (n < 0) return false;
        if (n == 0) break;
        ch[0].insert(ch[0].end(), l.begin(), l.end());
        if (dec.channels() == 2) ch[1].insert(ch[1].end(), r.begin(), r.end());
    }
    if (ch[0].empty()) { error = "no audio frames"; return false; }
    WavData w;
    w.channels = dec.channels();
    w.sampleRate = dec.sampleRate();
    w.frames = (int64_t)ch[0].size();
    w.unityNote = dec.unityNote();
    w.loopStart = dec.loopStart();
    w.loopEnd = dec.loopEnd();
    w.left = std::move(ch[0]);
    if (w.channels == 2) w.right = std::move(ch[1]);
    out = std::move(w);
    return true;
}

bool readFlac(const std::string& path, WavData& out, std::string& error) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!readFlacMemory(buf.data(), buf.size(), out, error)) { error = path + ": " + error; return false; }
    return true;
}

}  // namespace hsynth
