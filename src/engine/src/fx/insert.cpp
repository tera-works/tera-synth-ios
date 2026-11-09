#include "hsynth/fx/insert.h"

#include <cstring>

namespace hsynth {
namespace fx {

InsertChain::InsertChain() {
    for (int k = 0; k < kNumInsertBlocks; ++k) blocks_[k] = createBlock(kInsertBlockList[k]);
    params_.assign((size_t)kInsertParamTotal, -1e30f);
}

InsertChain::~InsertChain() = default;

void InsertChain::prepare(float sr) {
    sr_ = sr;
    ctx_.sampleRate = sr;
    for (auto& b : blocks_) b->prepare(sr);
    tmpL_.assign(kMaxBlockSize, 0.0f);
    tmpR_.assign(kMaxBlockSize, 0.0f);
    quietLimit_ = (int)(0.5f * sr);
    quietSamples_ = quietLimit_;
    first_ = true;
    std::fill(params_.begin(), params_.end(), -1e30f);
    reset();
}

void InsertChain::reset() {
    for (auto& b : blocks_) b->reset();
}

void InsertChain::paramsChanged(const float* p, int oversample, bool highQuality) {
    const bool ctxChanged = first_ || ctx_.oversample != oversample || ctx_.highQuality != highQuality;
    ctx_.oversample = oversample;
    ctx_.highQuality = highQuality;
    bool now[kNumInsertBlocks] = {};
    chainLen_ = 0;
    for (int s = 0; s < kInsertSlots; ++s) {
        const int k = (int)p[P::insSlot(s)] - 1;
        if (k < 0 || k >= kNumInsertBlocks || now[k]) continue;
        now[k] = true;
        chain_[chainLen_++] = k;
    }
    for (int k = 0; k < kNumInsertBlocks; ++k) {
        const float* bp = p + P::kInsBlockBase + insertOffset(k);
        const int cnt = kBlockParams[kInsertBlockList[k]];
        if (now[k] && !inChain_[k]) {
            blocks_[k]->reset();
            xfade_[k] = first_ && bp[0] > 0.5f ? 1.0f : 0.0f;
            quietSamples_ = 0;
        }
        inChain_[k] = now[k];
        on_[k] = bp[0];
        float* last = &params_[(size_t)insertOffset(k)];
        bool changed = ctxChanged;
        for (int i = 0; i < cnt && !changed; ++i) changed = last[i] != bp[i];
        if (!changed) continue;
        std::memcpy(last, bp, sizeof(float) * (size_t)cnt);
        blocks_[k]->update(bp, ctx_);
        quietSamples_ = 0;
    }
    first_ = false;
}

void InsertChain::setTempo(float bpm, double beat) {
    ctx_.beat = beat;
    if (bpm == ctx_.tempo) return;
    ctx_.tempo = bpm;
    for (int k = 0; k < kNumInsertBlocks; ++k)
        if (inChain_[k]) blocks_[k]->update(&params_[(size_t)insertOffset(k)], ctx_);
}

void InsertChain::noteOn(int key, bool firstNote) {
    ctx_.lastNoteHz = midiToHz((float)key);
    quietSamples_ = 0;
    if (firstNote)
        for (int i = 0; i < chainLen_; ++i) blocks_[chain_[i]]->retrigger();
}

void InsertChain::process(float* L, float* R, int n) {
    if (chainLen_ == 0) return;
    constexpr int kSub = 64;  // blocks assume buffers <= kMaxBlockSize; small sub-blocks keep crossfades smooth
    if (n > kSub) {
        for (int off = 0; off < n; off += kSub) process(L + off, R + off, std::min(kSub, n - off));
        return;
    }
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    quietSamples_ = pk > 1e-6f ? 0 : std::min(quietLimit_, quietSamples_ + n);
    const float xfStep = (float)n / (0.01f * sr_);
    for (int i = 0; i < chainLen_; ++i) {
        const int k = chain_[i];
        FxBlock* blk = blocks_[k].get();
        float& xf = xfade_[k];
        const float target = on_[k] > 0.5f ? 1.0f : 0.0f;
        if (xf == 0.0f && target == 0.0f) continue;
        if (xf == 1.0f && target == 1.0f) {
            blk->process(L, R, n, ctx_);
            continue;
        }
        // crossfade dry <-> processed over 10 ms
        std::memcpy(tmpL_.data(), L, sizeof(float) * (size_t)n);
        std::memcpy(tmpR_.data(), R, sizeof(float) * (size_t)n);
        if (xf == 0.0f) blk->reset();
        blk->process(tmpL_.data(), tmpR_.data(), n, ctx_);
        const float x1 = target > xf ? std::min(1.0f, xf + xfStep) : std::max(0.0f, xf - xfStep);
        for (int j = 0; j < n; ++j) {
            const float g = xf + (x1 - xf) * ((float)(j + 1) / (float)n);
            L[j] += (tmpL_[(size_t)j] - L[j]) * g;
            R[j] += (tmpR_[(size_t)j] - R[j]) * g;
        }
        xf = x1;
        quietSamples_ = 0;
    }
}

}  // namespace fx
}  // namespace hsynth
