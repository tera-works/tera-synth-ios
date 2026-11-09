// Voice FX: a per-voice insert between the filter and the amp envelope.
// Every voice has its own instance, so per-note modulation (pressure, timbre,
// ...) of vfx.amount shapes each key independently. No oversampling (cheap by
// design; the effect rack has the oversampled drive / amp blocks).
#pragma once

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

class VoiceFx {
public:
    void reset() {
        phase_ = 0.0f;
        hold_[0] = hold_[1] = 0.0f;
        holdCount_ = 0;
        adaa_[0].reset();
        adaa_[1].reset();
    }

    // Once per control block. ringDt: ring carrier phase increment (cycles / sample).
    // hq: anti-aliased saturator (Quality::High)
    void set(VfxType type, float amount, float mix, float ringDt, bool hq = false) {
        type_ = type;
        hq_ = hq;
        if (!(hq && type == VfxType::Saturate)) {  // restart the ADAA history when it comes back
            adaa_[0].reset();
            adaa_[1].reset();
        }
        amount_ = clampf(amount, 0.0f, 1.0f);
        mix_ = clampf(mix, 0.0f, 1.0f);
        // shapers are fully neutral at amount 0 and fade in over the first 20 %
        if (type == VfxType::Saturate || type == VfxType::Fold) mix_ *= std::min(1.0f, amount_ * 5.0f);
        ringDt_ = clampf(ringDt, 0.0f, 0.45f);
        switch (type) {
            case VfxType::Saturate:
                gain_ = 1.0f + amount_ * 24.0f;
                comp_ = 1.0f / fastTanh(std::min(3.0f, gain_ * 0.5f)) * 0.5f;
                break;
            case VfxType::Fold: gain_ = 1.0f + amount_ * 7.0f; break;
            case VfxType::Crush: {
                const float bits = 16.0f - amount_ * 13.0f;  // 16 .. 3 bits
                levels_ = std::exp2(bits - 1.0f);
                holdLen_ = 1 + (int)(amount_ * amount_ * 23.0f);  // sample-rate reduction up to 1/24
                break;
            }
            default: break;
        }
    }

    bool active() const { return type_ != VfxType::Off; }

    // Call once per sample before process() of the channels.
    void advance() {
        if (type_ == VfxType::Ring) {
            phase_ += ringDt_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            carrier_ = std::sin(kTwoPi * phase_);
        } else if (type_ == VfxType::Crush) {
            update_ = holdCount_ == 0;
            if (++holdCount_ >= holdLen_) holdCount_ = 0;
        }
    }

    float process(float x, int ch) {
        float y = x;
        switch (type_) {
            case VfxType::Saturate: y = (hq_ ? adaa_[ch].process(x * gain_) : fastTanh(x * gain_)) * comp_; break;
            case VfxType::Fold: y = std::sin(x * gain_ * (kPi * 0.5f)) * 0.7f; break;
            case VfxType::Crush:
                if (update_) hold_[ch] = std::round(x * levels_) / levels_;
                y = hold_[ch];
                break;
            case VfxType::Ring: y = x * (1.0f - amount_ + amount_ * carrier_); break;
            default: return x;
        }
        return x + (y - x) * mix_;
    }

private:
    VfxType type_ = VfxType::Off;
    float amount_ = 0.0f, mix_ = 1.0f, gain_ = 1.0f, comp_ = 1.0f, levels_ = 32768.0f;
    float ringDt_ = 0.0f, phase_ = 0.0f, carrier_ = 0.0f;
    float hold_[2] = {};
    int holdLen_ = 1, holdCount_ = 0;
    bool update_ = true;
    bool hq_ = false;
    AdaaTanh adaa_[2];
};

}  // namespace hsynth
