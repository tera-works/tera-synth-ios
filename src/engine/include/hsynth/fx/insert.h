// Part insert effects: up to fx::kInsertSlots serial blocks (from
// fx::kInsertBlockList) processed on one part's bus, before its volume / pan /
// FX send. Every part owns one chain; parameters come from the part's own
// parameter array (P::insSlot / P::insp), so they are stored with the patch.
// Audio thread only, except the constructor and prepare().
#pragma once

#include <memory>
#include <vector>

#include "hsynth/fx/block.h"
#include "hsynth/params.h"

namespace hsynth {
namespace fx {

class InsertChain {
public:
    InsertChain();
    ~InsertChain();
    void prepare(float sampleRate);
    void reset();

    // p = part parameter array (at least P::kPartParams values).
    void paramsChanged(const float* p, int oversample, bool highQuality);
    void setTempo(float bpm, double beat);
    void noteOn(int key, bool firstNote);
    bool empty() const { return chainLen_ == 0; }
    // true while some block still produces output for silent input (chorus /
    // flanger tails, crossfades); the engine keeps feeding silence meanwhile.
    bool busy() const { return chainLen_ > 0 && quietSamples_ < quietLimit_; }
    void process(float* L, float* R, int n);

private:
    float sr_ = 48000;
    std::unique_ptr<FxBlock> blocks_[kNumInsertBlocks];
    int chain_[kInsertSlots] = {};  // insert indices
    int chainLen_ = 0;
    bool inChain_[kNumInsertBlocks] = {};
    float xfade_[kNumInsertBlocks] = {};
    float on_[kNumInsertBlocks] = {};
    std::vector<float> params_;  // kInsertParamTotal, last seen values
    FxContext ctx_;
    std::vector<float> tmpL_, tmpR_;
    int quietSamples_ = 0, quietLimit_ = 0;
    bool first_ = true;
};

}  // namespace fx
}  // namespace hsynth
