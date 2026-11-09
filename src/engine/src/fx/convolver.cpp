#include "hsynth/fx/convolver.h"

#include <algorithm>
#include <cmath>

namespace hsynth {
namespace fx {

// ------------------------------------------------------------------ FFT

void Fft::init(int n) {
    n_ = n;
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    rev_.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev_[(size_t)i] = r;
    }
    cos_.resize((size_t)(n / 2));
    sin_.resize((size_t)(n / 2));
    for (int k = 0; k < n / 2; ++k) {
        const double a = 2.0 * 3.14159265358979323846 * k / n;
        cos_[(size_t)k] = (float)std::cos(a);
        sin_[(size_t)k] = (float)std::sin(a);
    }
}

void Fft::run(float* re, float* im, bool inv) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) {
        const int j = rev_[(size_t)i];
        if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    const float sgn = inv ? 1.0f : -1.0f;
    // first pass (len 2): twiddle 1
    for (int i = 0; i + 1 < n; i += 2) {
        const float tr = re[i + 1], ti = im[i + 1];
        re[i + 1] = re[i] - tr; im[i + 1] = im[i] - ti;
        re[i] += tr; im[i] += ti;
    }
    for (int len = 4; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        // twiddle outer, blocks inner: each twiddle is loaded once per pass
        for (int k = 0; k < half; ++k) {
            const float wr = cos_[(size_t)(k * step)];
            const float wi = sgn * sin_[(size_t)(k * step)];
            for (int a = k; a < n; a += len) {
                const int b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
            }
        }
    }
    if (inv) {
        const float s = 1.0f / (float)n;
        for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
    }
}

// ------------------------------------------------------------------ IR

namespace {
void buildStage(ConvStageIr& st, const float* hL, const float* hR, int len, int B, int first, int endTap,
                ConvIr::Layout layout) {
    st.block = B;
    st.first = first;
    const int start = first * B;
    const int end = std::min(len, endTap);
    st.parts = end > start ? (end - start + B - 1) / B : 0;
    st.re.assign((size_t)st.parts * 2 * B, 0.0f);
    st.im.assign((size_t)st.parts * 2 * B, 0.0f);
    Fft fft;
    fft.init(2 * B);
    for (int p = 0; p < st.parts; ++p) {
        float* re = &st.re[(size_t)p * 2 * B];
        float* im = &st.im[(size_t)p * 2 * B];
        for (int t = 0; t < B; ++t) {
            const int tap = start + p * B + t;
            if (tap >= end) break;
            re[t] = hL[tap];
            if (layout == ConvIr::StereoIrMonoIn) im[t] = hR ? hR[tap] : hL[tap];
        }
        fft.forward(re, im);
    }
}
}  // namespace

std::unique_ptr<ConvIr> ConvIr::build(const float* hL, const float* hR, int len, Layout layout, int maxLen) {
    auto ir = std::make_unique<ConvIr>();
    len = std::max(0, std::min(len, maxLen));
    ir->layout = layout;
    ir->length = len;
    ir->headL.assign(Convolver::kHead, 0.0f);
    ir->headR.assign(Convolver::kHead, 0.0f);
    for (int t = 0; t < std::min(len, Convolver::kHead); ++t) {
        ir->headL[(size_t)t] = hL[t];
        ir->headR[(size_t)t] = hR ? hR[t] : hL[t];
    }
    buildStage(ir->a, hL, hR, len, Convolver::kBlockA, 1, Convolver::kSplitB, layout);
    buildStage(ir->b, hL, hR, len, Convolver::kBlockB, Convolver::kSplitB / Convolver::kBlockB, len, layout);
    return ir;
}

// ------------------------------------------------------------------ runtime

void Convolver::initStage(Stage& s, int B, int maxParts) {
    s.B = B;
    s.maxParts = std::max(1, maxParts);
    s.inRe.assign((size_t)2 * B, 0.0f); s.inIm.assign((size_t)2 * B, 0.0f);
    s.fdlRe.assign((size_t)s.maxParts * 2 * B, 0.0f); s.fdlIm.assign((size_t)s.maxParts * 2 * B, 0.0f);
    s.accRe.assign((size_t)2 * B, 0.0f); s.accIm.assign((size_t)2 * B, 0.0f);
    s.outRe.assign((size_t)B, 0.0f); s.outIm.assign((size_t)B, 0.0f);
    s.nextRe.assign((size_t)B, 0.0f); s.nextIm.assign((size_t)B, 0.0f);
    s.fft.init(2 * B);
}

void Convolver::prepare(int maxLen) {
    initStage(a_, kBlockA, (kSplitB - kBlockA) / kBlockA);
    initStage(b_, kBlockB, std::max(1, (maxLen - kSplitB + kBlockB - 1) / kBlockB));
    histL_.assign(2 * kHead, 0.0f);
    histR_.assign(2 * kHead, 0.0f);
    reset();
}

void Convolver::clearStage(Stage& s) {
    std::fill(s.inRe.begin(), s.inRe.end(), 0.0f); std::fill(s.inIm.begin(), s.inIm.end(), 0.0f);
    std::fill(s.fdlRe.begin(), s.fdlRe.end(), 0.0f); std::fill(s.fdlIm.begin(), s.fdlIm.end(), 0.0f);
    std::fill(s.accRe.begin(), s.accRe.end(), 0.0f); std::fill(s.accIm.begin(), s.accIm.end(), 0.0f);
    std::fill(s.outRe.begin(), s.outRe.end(), 0.0f); std::fill(s.outIm.begin(), s.outIm.end(), 0.0f);
    std::fill(s.nextRe.begin(), s.nextRe.end(), 0.0f); std::fill(s.nextIm.begin(), s.nextIm.end(), 0.0f);
    s.pos = 0; s.fdlHead = 0; s.done = 0;
}

void Convolver::reset() {
    clearStage(a_);
    clearStage(b_);
    std::fill(histL_.begin(), histL_.end(), 0.0f);
    std::fill(histR_.begin(), histR_.end(), 0.0f);
    histPos_ = 0;
}

void Convolver::setIr(const ConvIr* ir) {
    if (ir == ir_) return;
    ir_ = ir;
    reset();
    if (ir_) {
        a_.parts = std::min(ir_->a.parts, a_.maxParts);
        b_.parts = std::min(ir_->b.parts, b_.maxParts);
    }
}

void Convolver::accumulate(Stage& s, const ConvStageIr& h, int from, int to) {
    const int N = 2 * s.B;
    for (int p = from; p < to; ++p) {
        const int slot = (s.fdlHead - p + s.maxParts) % s.maxParts;
        const float* xr = &s.fdlRe[(size_t)slot * N];
        const float* xi = &s.fdlIm[(size_t)slot * N];
        const float* hr = &h.re[(size_t)p * N];
        const float* hi = &h.im[(size_t)p * N];
        float* ar = s.accRe.data();
        float* ai = s.accIm.data();
        for (int k = 0; k < N; ++k) {
            ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
            ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
        }
    }
}

void Convolver::blockDone(Stage& s, const ConvStageIr& h, bool distributed) {
    const int B = s.B, N = 2 * B;
    if (distributed) {
        // finish the block whose output is due now, publish it
        accumulate(s, h, s.done, s.parts);
        s.fft.inverse(s.accRe.data(), s.accIm.data());
        std::copy(s.accRe.begin() + B, s.accRe.end(), s.outRe.begin());
        std::copy(s.accIm.begin() + B, s.accIm.end(), s.outIm.begin());
    }
    // transform the input frame [previous block, this block]
    s.fdlHead = (s.fdlHead + 1) % s.maxParts;
    float* xr = &s.fdlRe[(size_t)s.fdlHead * N];
    float* xi = &s.fdlIm[(size_t)s.fdlHead * N];
    std::copy(s.inRe.begin(), s.inRe.end(), xr);
    std::copy(s.inIm.begin(), s.inIm.end(), xi);
    s.fft.forward(xr, xi);
    std::copy(s.inRe.begin() + B, s.inRe.end(), s.inRe.begin());
    std::copy(s.inIm.begin() + B, s.inIm.end(), s.inIm.begin());
    std::fill(s.accRe.begin(), s.accRe.end(), 0.0f);
    std::fill(s.accIm.begin(), s.accIm.end(), 0.0f);
    s.done = 0;
    s.pos = 0;
    if (!distributed) {
        accumulate(s, h, 0, s.parts);
        s.fft.inverse(s.accRe.data(), s.accIm.data());
        std::copy(s.accRe.begin() + B, s.accRe.end(), s.outRe.begin());
        std::copy(s.accIm.begin() + B, s.accIm.end(), s.outIm.begin());
    }
}

void Convolver::process(float* L, float* R, int n) {
    if (!ir_) {
        std::fill(L, L + n, 0.0f);
        std::fill(R, R + n, 0.0f);
        return;
    }
    const bool stereoIr = ir_->layout == ConvIr::StereoIrMonoIn;
    const float* hL = ir_->headL.data();
    const float* hR = ir_->headR.data();
    const int headLen = std::min(kHead, ir_->length);
    const int stepB = kBlockB / 16;
    for (int i = 0; i < n; ++i) {
        float xr, xi;
        if (stereoIr) { xr = 0.5f * (L[i] + R[i]); xi = 0.0f; }
        else { xr = L[i]; xi = R[i]; }

        // head FIR
        histPos_ = (histPos_ + kHead - 1) % kHead;
        histL_[(size_t)histPos_] = histL_[(size_t)histPos_ + kHead] = xr;
        histR_[(size_t)histPos_] = histR_[(size_t)histPos_ + kHead] = xi;
        const float* zl = &histL_[(size_t)histPos_];
        const float* zr = &histR_[(size_t)histPos_];
        float yl = 0, yr = 0;
        if (stereoIr) {
            for (int t = 0; t < headLen; ++t) { yl += hL[t] * zl[t]; yr += hR[t] * zl[t]; }
        } else {
            for (int t = 0; t < headLen; ++t) { yl += hL[t] * zl[t]; yr += hL[t] * zr[t]; }
        }

        if (a_.parts > 0) {
            a_.inRe[(size_t)(a_.B + a_.pos)] = xr;
            a_.inIm[(size_t)(a_.B + a_.pos)] = xi;
            yl += a_.outRe[(size_t)a_.pos];
            yr += a_.outIm[(size_t)a_.pos];
            if (++a_.pos == a_.B) blockDone(a_, ir_->a, false);
        }
        if (b_.parts > 0) {
            b_.inRe[(size_t)(b_.B + b_.pos)] = xr;
            b_.inIm[(size_t)(b_.B + b_.pos)] = xi;
            yl += b_.outRe[(size_t)b_.pos];
            yr += b_.outIm[(size_t)b_.pos];
            ++b_.pos;
            if (b_.pos == b_.B) blockDone(b_, ir_->b, true);
            else if (b_.pos % stepB == 0) {
                const int target = b_.parts * (b_.pos / stepB) / 16;
                accumulate(b_, ir_->b, b_.done, target);
                b_.done = target;
            }
        }
        L[i] = yl;
        R[i] = yr;
    }
}

}  // namespace fx
}  // namespace hsynth
