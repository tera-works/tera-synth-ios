#include "hsynth/dsp/resample.h"

#include <cmath>

namespace hsynth {
namespace resample {

namespace {
double bessel0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}
double kaiser(double t, double halfWidth, double beta) {
    const double r = t / halfWidth;
    if (r <= -1.0 || r >= 1.0) return 0.0;
    return bessel0(beta * std::sqrt(1.0 - r * r)) / bessel0(beta);
}
double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x); }

struct Table {
    std::vector<float> data;
    Table() {
        data.assign((size_t)kCutoffs * (kPhases + 1) * kTaps, 0.0f);
        const double half = kTaps / 2.0 + 0.5;  // window reaches just past the outer taps
        for (int c = 0; c < kCutoffs; ++c) {
            const double fc = kMinCut + (kMaxCut - kMinCut) * c / (kCutoffs - 1);
            for (int ph = 0; ph <= kPhases; ++ph) {
                const double f = (double)ph / kPhases;  // read position = idx + f
                double w[kTaps], sum = 0.0;
                for (int k = 0; k < kTaps; ++k) {
                    const double t = (double)(k - (kTaps / 2 - 1)) - f;  // tap offset -7..8 minus f
                    w[k] = fc * sinc(fc * t) * kaiser(t, half, 7.0);
                    sum += w[k];
                }
                float* row = data.data() + ((size_t)c * (kPhases + 1) + (size_t)ph) * kTaps;
                for (int k = 0; k < kTaps; ++k) row[k] = (float)(w[k] / sum);
            }
        }
    }
};
const Table g_table;  // built during static initialisation, never on the audio thread
}  // namespace

const float* kernelTable() { return g_table.data.data(); }

void decimate2(const std::vector<float>& in, size_t frames, std::vector<float>& out) {
    // 63-tap Kaiser low-pass at a quarter of the input rate (passband to ~0.21)
    constexpr int kN = 63;
    static const std::vector<double> h = [] {
        std::vector<double> c(kN);
        double sum = 0.0;
        for (int i = 0; i < kN; ++i) {
            const double t = i - (kN - 1) / 2.0;
            c[(size_t)i] = 0.45 * sinc(0.45 * t) * kaiser(t, (kN + 1) / 2.0, 9.0);
            sum += c[(size_t)i];
        }
        for (auto& v : c) v /= sum;
        return c;
    }();
    const size_t n = (frames + 1) / 2;
    out.assign(n + 8, 0.0f);  // padding like Sample::kPadFrames
    for (size_t o = 0; o < n; ++o) {
        const int64_t centre = (int64_t)(2 * o);
        double acc = 0.0;
        for (int k = 0; k < kN; ++k) {
            const int64_t i = centre + k - (kN - 1) / 2;
            if (i >= 0 && i < (int64_t)frames) acc += h[(size_t)k] * in[(size_t)i];
        }
        out[o] = (float)acc;
    }
}

}  // namespace resample
}  // namespace hsynth
