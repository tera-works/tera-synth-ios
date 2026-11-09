#include "hsynth/fx/ir_load.h"

#include <cmath>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/wav.h"

namespace hsynth {
namespace fx {

namespace {
std::vector<float> resample(const float* x, int n, float ratio) {
    // ratio = out rate / in rate; 4-point Hermite, low-passed implicitly by the
    // IR's own content (IRs rarely contain energy near Nyquist)
    if (std::fabs(ratio - 1.0f) < 1e-6f) return std::vector<float>(x, x + n);
    const int m = (int)((float)n * ratio);
    std::vector<float> y((size_t)std::max(0, m));
    auto at = [&](int i) { return i < 0 || i >= n ? 0.0f : x[i]; };
    for (int j = 0; j < m; ++j) {
        const float pos = (float)j / ratio;
        const int i = (int)pos;
        const float f = pos - (float)i;
        const float xm = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
        y[(size_t)j] = ((c3 * f + c2) * f + c1) * f + x0;
    }
    return y;
}
}  // namespace

std::unique_ptr<ConvIr> makeImpulseResponse(const float* left, const float* right, int frames, float fileRate,
                                            float sampleRate, int target, const std::string& name) {
    if (!left || frames <= 0) return nullptr;
    const float ratio = sampleRate / fileRate;
    std::vector<float> l = resample(left, frames, ratio);
    std::vector<float> r = right ? resample(right, frames, ratio) : l;
    if (target == 0) {
        // cabinet: mono IR (average of the channels)
        for (size_t i = 0; i < l.size(); ++i) l[i] = 0.5f * (l[i] + r[i]);
        r = l;
    }
    const int maxLen = target == 0 ? (1 << 15) : (int)(6.0f * sampleRate);
    int len = std::min((int)l.size(), maxLen);
    const bool truncated = len < (int)l.size();
    // trim the tail below -90 dB of the peak
    float peak = 0.0f;
    for (int i = 0; i < len; ++i) peak = std::max(peak, std::max(std::fabs(l[(size_t)i]), std::fabs(r[(size_t)i])));
    if (peak <= 0.0f) return nullptr;
    const float floor = peak * 3.16e-5f;
    while (len > 1 && std::fabs(l[(size_t)len - 1]) < floor && std::fabs(r[(size_t)len - 1]) < floor) --len;
    // short fade-out where the IR was cut short by maxLen
    const int fade = truncated ? std::min(len / 4, (int)(0.01f * sampleRate)) : 0;
    for (int i = 0; i < fade; ++i) {
        const float g = (float)i / (float)fade;
        l[(size_t)(len - 1 - i)] *= g;
        r[(size_t)(len - 1 - i)] *= g;
    }
    double e = 0;
    for (int i = 0; i < len; ++i) e += (double)l[(size_t)i] * l[(size_t)i] + (double)r[(size_t)i] * r[(size_t)i];
    const float g = (float)std::sqrt((target == 0 ? 2.0 : 1.0) / (e + 1e-20));
    for (int i = 0; i < len; ++i) { l[(size_t)i] *= g; r[(size_t)i] *= g; }
    auto ir = ConvIr::build(l.data(), r.data(), len, target == 0 ? ConvIr::MonoIrStereoIn : ConvIr::StereoIrMonoIn, maxLen);
    ir->name = name;
    return ir;
}

std::unique_ptr<ConvIr> loadImpulseResponse(const std::string& path, float sampleRate, int target, std::string& error) {
    WavData w;
    if (!readWav(path, w, error)) return nullptr;
    std::string name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto ir = makeImpulseResponse(w.left.data(), w.right.empty() ? nullptr : w.right.data(), (int)w.frames,
                                  (float)w.sampleRate, sampleRate, target, name);
    if (!ir) error = path + ": impulse response is empty or silent";
    else ir->path = path;
    return ir;
}

}  // namespace fx
}  // namespace hsynth
