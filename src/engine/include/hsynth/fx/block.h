// Effect block interface and factory.
#pragma once

#include <memory>

#include "hsynth/fx/dsp.h"

namespace hsynth {
namespace fx {

struct ConvIr;

class FxBlock {
public:
    virtual ~FxBlock() = default;
    // Not real-time: allocate everything the block can ever need.
    virtual void prepare(float sampleRate) = 0;
    virtual void reset() = 0;
    // Audio thread. p points at the block's first parameter (p[0] = On).
    // Called when parameters change and, while FX modulation is active, once
    // per rack sub-block; must be cheap and allocation-free.
    virtual void update(const float* p, const FxContext& c) = 0;
    // Audio thread, in place.
    virtual void process(float* L, float* R, int n, const FxContext& c) = 0;

    // Spillover blocks (delay, reverbs) handle On themselves: when switched
    // off they stop taking input but keep playing their tail. Other blocks are
    // bypassed by the rack with a short crossfade.
    virtual bool spillover() const { return false; }
    virtual bool tailActive() const { return false; }
    // Restart LFOs / rhythmic patterns (key retrigger).
    virtual void retrigger() {}
    // User impulse response (cabinet / IR reverb). Audio thread.
    virtual void setUserIr(const ConvIr*) {}
};

std::unique_ptr<FxBlock> createBlock(int type);

}  // namespace fx
}  // namespace hsynth
