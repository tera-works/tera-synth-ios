#include "hsynth/dsp/wavetable.h"
#include "hsynth/dsp/simd.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "hsynth/fileio.h"
#include "hsynth/fx/convolver.h"
#include "hsynth/flac.h"
#include "hsynth/wav.h"

namespace hsynth {

size_t Wavetable::memoryBytes() const {
    size_t b = 0;
    for (auto& d : data) b += d.size() * sizeof(float);
    return b;
}

std::unique_ptr<Wavetable> Wavetable::fromFrames(const std::vector<float>& raw, int frameLen, int frames, const std::string& name,
                                                 bool perFrame) {
    auto wt = std::make_unique<Wavetable>();
    wt->name = name;
    wt->frames = frames;
    for (int k = 0; k < kLevels; ++k) {
        const int harm = std::max(1, 1024 >> k);
        wt->size[k] = std::min(4096, std::max(64, 8 * harm));
        wt->data[k].assign((size_t)frames * (size_t)(wt->size[k] + 1), 0.0f);
    }
    fx::Fft fwd;
    fwd.init(frameLen);
    std::vector<fx::Fft> inv(kLevels);
    for (int k = 0; k < kLevels; ++k) inv[(size_t)k].init(wt->size[k]);
    const int avail = std::min(frameLen / 2 - 1, 1024);
    std::vector<float> re((size_t)frameLen), im((size_t)frameLen);
    std::vector<float> ore(4096), oim(4096);
    for (int f = 0; f < frames; ++f) {
        std::copy(raw.begin() + (ptrdiff_t)f * frameLen, raw.begin() + (ptrdiff_t)(f + 1) * frameLen, re.begin());
        std::fill(im.begin(), im.end(), 0.0f);
        fwd.forward(re.data(), im.data());
        for (int k = 0; k < kLevels; ++k) {
            const int n = wt->size[k];
            const int harm = std::min(std::max(1, 1024 >> k), avail);
            std::fill(ore.begin(), ore.begin() + n, 0.0f);
            std::fill(oim.begin(), oim.begin() + n, 0.0f);
            const float scale = (float)n / (float)frameLen;  // keeps the amplitude across table sizes
            for (int h = 1; h <= harm && h < n / 2; ++h) {   // DC (h = 0) dropped
                ore[(size_t)h] = re[(size_t)h] * scale;
                oim[(size_t)h] = im[(size_t)h] * scale;
                ore[(size_t)(n - h)] = re[(size_t)h] * scale;
                oim[(size_t)(n - h)] = -im[(size_t)h] * scale;
            }
            inv[(size_t)k].inverse(ore.data(), oim.data());
            float* dst = wt->data[k].data() + (size_t)f * (size_t)(n + 1);
            for (int i = 0; i < n; ++i) dst[i] = ore[(size_t)i];
            dst[n] = dst[0];
        }
    }
    // normalise: per frame or one gain for the whole table (level 0 decides)
    auto framePeak = [&](int f) {
        const float* d = wt->frame(0, f);
        float pk = 0.0f;
        for (int i = 0; i < wt->size[0]; ++i) pk = std::max(pk, std::fabs(d[i]));
        return pk;
    };
    float tablePeak = 0.0f;
    for (int f = 0; f < frames; ++f) tablePeak = std::max(tablePeak, framePeak(f));
    for (int f = 0; f < frames; ++f) {
        const float pk = perFrame ? framePeak(f) : tablePeak;
        if (pk <= 0.0f) continue;
        const float g = 0.99f / pk;
        for (int k = 0; k < kLevels; ++k) {
            float* d = wt->data[k].data() + (size_t)f * (size_t)(wt->size[k] + 1);
            for (int i = 0; i <= wt->size[k]; ++i) d[i] *= g;
        }
    }
    return wt;
}

namespace {

constexpr int kBuiltinFrames = 64;
const int N = Wavetable::kFrameSize;

using Gen = float (*)(float x, float t);  // x: phase 0..1, t: frame 0..1

float saw(float x) { return 2.0f * x - 1.0f; }
float tri(float x) { return 1.0f - 4.0f * std::fabs(x - 0.5f); }
float sq(float x, float w) { return x < w ? 1.0f : -1.0f; }

float genBasic(float x, float t) {
    // sine -> triangle -> saw -> square -> narrow pulse
    const float s = std::sin(kTwoPi * x);
    const float seg = t * 4.0f;
    const int i = std::min(3, (int)seg);
    const float f = seg - (float)i;
    float a, b;
    switch (i) {
        case 0: a = s; b = tri(std::fmod(x + 0.25f, 1.0f)); break;
        case 1: a = tri(std::fmod(x + 0.25f, 1.0f)); b = saw(x); break;
        case 2: a = saw(x); b = sq(x, 0.5f); break;
        default: a = sq(x, 0.5f); b = sq(x, 0.1f); break;
    }
    return a + (b - a) * f;
}
float genPwm(float x, float t) { return sq(x, 0.5f - 0.48f * t); }
// Additive generators fill a whole frame: amplitudes depend only on the frame,
// sin(2 pi k i / N) comes from an exact table (k * i mod N).
const std::vector<float>& sinTableN() {
    static const std::vector<float> t = [] {
        std::vector<float> v((size_t)N);
        for (int i = 0; i < N; ++i) v[(size_t)i] = (float)std::sin(2.0 * 3.14159265358979323846 * i / N);
        return v;
    }();
    return t;
}
void additive(const float* amp, int count, float* out) {
    const auto& st = sinTableN();
    for (int i = 0; i < N; ++i) {
        float v = 0.0f;
        for (int k = 1; k <= count; ++k)
            if (amp[k] != 0.0f) v += amp[k] * st[(size_t)(((int64_t)k * i) & (N - 1))];
        out[i] = v;
    }
}
void fillHarmonics(float t, float* out) {
    const int count = 1 + (int)(t * 63.0f);
    float amp[65] = {};
    for (int k = 1; k <= count; ++k) amp[k] = 1.0f / (float)k;
    additive(amp, count, out);
}
void fillFormant(float t, float* out) {
    // vowels a e i o u as formant envelopes over harmonics of a 110 Hz voice
    static const float F[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {350, 2300, 3000}, {450, 800, 2830}, {325, 700, 2530}};
    const float seg = t * 4.0f;
    const int i = std::min(3, (int)seg);
    const float fr = seg - (float)i;
    float amp[61] = {};
    for (int k = 1; k <= 60; ++k) {
        const float hz = 110.0f * (float)k;
        for (int j = 0; j < 3; ++j) {
            const float c = F[i][j] + (F[i + 1][j] - F[i][j]) * fr;
            const float bw = 80.0f + 40.0f * (float)j;
            const float z = 0.5f * (hz - c) * (hz - c) / (bw * bw);
            if (z < 80.0f) amp[k] += std::exp(-z) / (float)(j + 1);  // skip underflowing terms (slow path in libm)
        }
    }
    additive(amp, 60, out);
}
float genSync(float x, float t) {
    const float r = 1.0f + 7.0f * t;
    const float y = x * r;
    return saw(y - std::floor(y)) * (0.6f + 0.4f * (1.0f - x));  // softened end of the cycle
}
float genFold(float x, float t) { return std::sin((1.0f + 7.0f * t) * std::sin(kTwoPi * x)); }
float genDigital(float x, float t) {
    const float steps = std::round(std::pow(2.0f, 5.0f - 4.0f * t));  // 32 .. 2 levels
    const float v = std::sin(kTwoPi * x) + 0.5f * saw(std::fmod(x * 3.0f, 1.0f)) * t;
    return std::round(v * steps) / steps;
}
float genOrgan(float x, float t) {
    // drawbar registrations on the integer-footage bars: 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'
    static const float H[7] = {1, 2, 3, 4, 5, 6, 8};
    static const float R[5][7] = {{8, 0, 0, 0, 0, 0, 0}, {8, 8, 0, 0, 0, 0, 0}, {8, 8, 8, 0, 0, 0, 8},
                                  {8, 8, 8, 8, 8, 8, 8}, {4, 8, 8, 8, 6, 8, 8}};
    const float seg = t * 4.0f;
    const int i = std::min(3, (int)seg);
    const float fr = seg - (float)i;
    float v = 0.0f;
    for (int k = 0; k < 7; ++k) {
        const float a = (R[i][k] + (R[i + 1][k] - R[i][k]) * fr) / 8.0f;
        if (a > 0.0f) v += a * std::sin(kTwoPi * H[k] * x);
    }
    return v;
}

using Fill = void (*)(float t, float* out);  // whole frame at once (additive generators)
struct Builtin {
    const char* name;
    Gen gen;
    Fill fill;
};
const Builtin kBuiltins[] = {{"basic", genBasic, nullptr}, {"pwm", genPwm, nullptr},     {"harmonics", nullptr, fillHarmonics},
                             {"formant", nullptr, fillFormant}, {"sync", genSync, nullptr}, {"fold", genFold, nullptr},
                             {"digital", genDigital, nullptr}, {"organ", genOrgan, nullptr}};
constexpr int kBuiltinCount = (int)(sizeof kBuiltins / sizeof kBuiltins[0]);

std::vector<std::unique_ptr<Wavetable>> buildAll() {
    std::vector<std::unique_ptr<Wavetable>> v;
    for (const auto& b : kBuiltins) {
        std::vector<float> raw((size_t)kBuiltinFrames * N);
        for (int f = 0; f < kBuiltinFrames; ++f) {
            const float t = (float)f / (float)(kBuiltinFrames - 1);
            if (b.fill) b.fill(t, raw.data() + (size_t)f * N);
            else
                for (int i = 0; i < N; ++i) raw[(size_t)f * N + (size_t)i] = b.gen((float)i / (float)N, t);
        }
        auto wt = Wavetable::fromFrames(raw, N, kBuiltinFrames, b.name, true);
        v.push_back(std::move(wt));
    }
    return v;
}

const std::vector<std::unique_ptr<Wavetable>>& builtins() {
    static const std::vector<std::unique_ptr<Wavetable>> tables = buildAll();
    return tables;
}

// Serum writes a "clm " chunk whose text starts with "<!>2048".
int clmFrameSize(const std::string& path) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (size_t i = 12; i + 8 < d.size();) {
        uint32_t sz;
        std::memcpy(&sz, &d[i + 4], 4);
        if (std::memcmp(&d[i], "clm ", 4) == 0) {
            const std::string text(d.begin() + (ptrdiff_t)i + 8, d.begin() + (ptrdiff_t)std::min(d.size(), i + 8 + sz));
            const size_t p = text.find("<!>");
            if (p != std::string::npos) return std::atoi(text.c_str() + p + 3);
        }
        i += 8 + sz + (sz & 1);
    }
    return 0;
}

}  // namespace

int builtinWavetableCount() { return kBuiltinCount; }

const std::vector<std::string>& builtinWavetableNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& b : kBuiltins) n.push_back(b.name);
        return n;
    }();
    return names;
}

const Wavetable& builtinWavetable(int index) {
    const auto& t = builtins();
    return *t[(size_t)std::max(0, std::min(index, (int)t.size() - 1))];
}

std::unique_ptr<Wavetable> loadWavetableFile(const std::string& path, std::string& error) {
    WavData w;
    const bool flac = path.size() > 5 && (path.compare(path.size() - 5, 5, ".flac") == 0 || path.compare(path.size() - 5, 5, ".FLAC") == 0);
    if (!(flac ? readFlac(path, w, error) : readWav(path, w, error))) return nullptr;
    std::vector<float> mono(w.left);
    if (!w.right.empty())
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (mono[i] + w.right[i]);
    const int len = (int)mono.size();
    if (len < 16) { error = path + ": too short for a wavetable"; return nullptr; }
    int frameLen = clmFrameSize(path);
    std::vector<float> raw;
    int frames = 0;
    auto isPow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
    if (frameLen > 0 && len % frameLen == 0 && isPow2(frameLen)) {
        frames = len / frameLen;
        raw = mono;
    } else if (len % N == 0) {
        frameLen = N;
        frames = len / N;
        raw = mono;
    } else if (len <= 8192) {
        // one cycle of arbitrary length (e.g. AKWF 600-sample waves): periodic resample to N
        frameLen = N;
        frames = 1;
        raw.resize((size_t)N);
        for (int i = 0; i < N; ++i) {
            const float pos = (float)i * (float)len / (float)N;
            const int a = (int)pos;
            const float t = pos - (float)a;
            raw[(size_t)i] = mono[(size_t)(a % len)] * (1.0f - t) + mono[(size_t)((a + 1) % len)] * t;
        }
    } else {
        frameLen = N;
        frames = len / N;
        raw.assign(mono.begin(), mono.begin() + (ptrdiff_t)frames * N);
    }
    frames = std::min(frames, 256);
    raw.resize((size_t)frames * (size_t)frameLen);
    std::string name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto wt = Wavetable::fromFrames(raw, frameLen, frames, name);
    wt->sourcePath = path;
    return wt;
}

// ------------------------------------------------------------------ oscillator

void WtOsc::reset(Rng& rng, int unison) {
    // single voice starts at phase 0 (repeatable attack); unison voices are free-running
    for (int v = 0; v < kMaxUnison; ++v) phase_[v] = unison > 1 ? rng.unipolar() : 0.0f;
}

namespace {
inline float warpPhase(float p, WtWarp w, float amt) {
    switch (w) {
        case WtWarp::Sync: {
            const float y = p * (1.0f + amt * 7.0f);
            return y - fastFloor(y);
        }
        case WtWarp::Bend: {
            const float k = fastExp2(6.0f * (amt - 0.5f));
            return p * k / (1.0f + p * (k - 1.0f));
        }
        case WtWarp::Mirror: {
            const float m = p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p;
            const float y = p + (m - p) * amt;
            return y >= 1.0f ? 0.0f : y;
        }
        case WtWarp::Squeeze: {
            const float s = 0.5f - 0.49f * amt;
            return p < s ? 0.5f * p / s : 0.5f + 0.5f * (p - s) / (1.0f - s);
        }
        default: return p;
    }
}
// 4-point Hermite interpolation (size is a power of two; indices wrap). Linear
// interpolation left the first images of the top harmonics only ~21 dB down,
// which folded back as inharmonic spurs (about -45 dB on bright frames).
inline float readTable(const float* t, int size, float phase) {
    const float idx = phase * (float)size;
    int i = (int)idx;
    const float f = idx - (float)i;
    const int m = size - 1;
    i &= m;
    const float xm1 = t[(i - 1) & m], x0 = t[i], x1 = t[(i + 1) & m], x2 = t[(i + 2) & m];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

// readTable() for four phases at once (four unison voices): the table reads
// are scalar (no gather on NEON), the Hermite interpolation is vectorised.
inline simd::f4 readTable4(const float* t, int size, simd::f4 phase) {
    using namespace simd;
    const f4 idx = phase * set1((float)size);
    const i4 ii = truncInt(idx);
    const f4 f = idx - toFloat(ii);
    alignas(16) int32_t iv[4];
    store(iv, ii);
    const int m = size - 1;
    alignas(16) float xm1[4], x0[4], x1[4], x2[4];
    for (int k = 0; k < 4; ++k) {
        const int i = iv[k] & m;
        xm1[k] = t[(i - 1) & m];
        x0[k] = t[i];
        x1[k] = t[(i + 1) & m];
        x2[k] = t[(i + 2) & m];
    }
    const f4 a = load(xm1), b = load(x0), c = load(x1), d = load(x2);
    const f4 c1 = set1(0.5f) * (c - a);
    const f4 c2 = a - set1(2.5f) * b + set1(2.0f) * c - set1(0.5f) * d;
    const f4 c3 = set1(0.5f) * (d - a) + set1(1.5f) * (b - c);
    return ((c3 * f + c2) * f + c1) * f + b;
}
}  // namespace

bool WtOsc::render(const Wavetable& wt, float pos, float dt, WtWarp warp, float warpAmt, int unison, float detuneCents,
                   float spread, float* L, float* R, int n) {
    const int U = std::max(1, std::min(kMaxUnison, unison));
    dt = clampf(dt, 1e-7f, 0.49f);
    // mip level: level k holds 1024 >> k harmonics, alias-free while (1024 >> k) * dt <= 0.5
    const float x = std::log2(2048.0f * dt * (U > 1 ? fastExp2(detuneCents / 1200.0f) : 1.0f));
    int k0;
    float w0;
    if (x <= 0.0f) { k0 = 0; w0 = 1.0f; }
    else {
        // level k0 is alias-free for x <= k0; over the last half octave before
        // the switch it crossfades into the duller k0 + 1 so that sweeping the
        // pitch never steps (the rest of the time only one level is read)
        k0 = (int)std::ceil(x);
        w0 = clampf(2.0f * ((float)k0 - x), 0.0f, 1.0f);
    }
    if (k0 >= Wavetable::kLevels - 1) { k0 = Wavetable::kLevels - 1; w0 = 1.0f; }
    const int k1 = std::min(k0 + 1, Wavetable::kLevels - 1);
    const float w1 = 1.0f - w0;
    const float fp = clampf(pos, 0.0f, 1.0f) * (float)(wt.frames - 1);
    const int f0 = (int)fp, f1 = std::min(f0 + 1, wt.frames - 1);
    const float ff = fp - (float)f0;
    const float* a0 = wt.frame(k0, f0);
    const float* a1 = wt.frame(k0, f1);
    const float* b0 = wt.frame(k1, f0);
    const float* b1 = wt.frame(k1, f1);
    const int sa = wt.size[k0], sb = wt.size[k1];
    std::fill(L, L + n, 0.0f);
    std::fill(R, R + n, 0.0f);
    const bool stereo = U > 1 && spread > 0.0f;
    const float norm = 1.0f / std::sqrt((float)U);
    if (U >= 3) {
        // unison: four voices per SIMD vector (unused lanes have zero gain)
        using namespace simd;
        for (int v0 = 0; v0 < U; v0 += 4) {
            alignas(16) float ph4[4] = {}, dt4[4] = {}, gl4[4] = {}, gr4[4] = {};
            for (int j = 0; j < 4 && v0 + j < U; ++j) {
                const int v = v0 + j;
                const float off = ((float)v - 0.5f * (float)(U - 1)) / (0.5f * (float)(U - 1));
                const float pan = off * spread;
                ph4[j] = phase_[v];
                dt4[j] = std::min(0.49f, dt * fastExp2(off * detuneCents / 1200.0f));
                gl4[j] = norm * std::min(1.0f, 1.0f - pan);
                gr4[j] = norm * std::min(1.0f, 1.0f + pan);
            }
            f4 ph = load(ph4);
            const f4 vdt = load(dt4), gl = load(gl4), gr = load(gr4), one = set1(1.0f);
            const f4 vw0 = set1(w0), vw1 = set1(w1), vff = set1(ff);
            for (int i = 0; i < n; ++i) {
                ph = ph + vdt;
                ph = subIfGe(ph, ph, one);
                f4 p = ph;
                if (warp != WtWarp::None) {
                    alignas(16) float t[4];
                    store(t, ph);
                    for (float& x : t) x = warpPhase(x, warp, warpAmt);
                    p = load(t);
                }
                f4 sv = set1(0.0f);
                if (w0 > 0.0f) {
                    const f4 ra = readTable4(a0, sa, p);
                    sv = vw0 * (ff > 0.0f ? ra + vff * (readTable4(a1, sa, p) - ra) : ra);
                }
                if (w1 > 0.0f) {
                    const f4 rb = readTable4(b0, sb, p);
                    sv = sv + vw1 * (ff > 0.0f ? rb + vff * (readTable4(b1, sb, p) - rb) : rb);
                }
                L[i] += hsum(sv * gl);
                R[i] += hsum(sv * gr);
            }
            store(ph4, ph);
            for (int j = 0; j < 4 && v0 + j < U; ++j) phase_[v0 + j] = ph4[j];
        }
        return stereo;
    }
    for (int v = 0; v < U; ++v) {
        const float off = U > 1 ? ((float)v - 0.5f * (float)(U - 1)) / (0.5f * (float)(U - 1)) : 0.0f;  // -1..1
        const float vdt = std::min(0.49f, dt * fastExp2(off * detuneCents / 1200.0f));
        const float pan = off * spread;
        const float gl = norm * std::min(1.0f, 1.0f - pan), gr = norm * std::min(1.0f, 1.0f + pan);
        float ph = phase_[v];
        for (int i = 0; i < n; ++i) {
            ph += vdt;
            if (ph >= 1.0f) ph -= 1.0f;
            const float p = warp == WtWarp::None ? ph : warpPhase(ph, warp, warpAmt);
            float s = 0.0f;
            if (w0 > 0.0f) {
                const float ra = readTable(a0, sa, p);
                s = w0 * (ff > 0.0f ? ra + ff * (readTable(a1, sa, p) - ra) : ra);
            }
            if (w1 > 0.0f) {
                const float rb = readTable(b0, sb, p);
                s += w1 * (ff > 0.0f ? rb + ff * (readTable(b1, sb, p) - rb) : rb);
            }
            L[i] += s * gl;
            R[i] += s * gr;
        }
        phase_[v] = ph;
    }
    return stereo;
}

}  // namespace hsynth
