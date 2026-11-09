// ADSR envelope with analog-style exponential segments.
#pragma once

#include <cmath>

namespace hsynth {

class Adsr {
public:
    enum class Stage { Idle, Attack, Hold, Decay, Sustain, Release };

    void setSampleRate(float sr) { sampleRate_ = sr; }

    // Times in seconds, sustain 0..1. Cheap enough to call per control block.
    void set(float attack, float decay, float sustain, float release) {
        if (attack != a_ || decay != d_ || release != r_) {
            a_ = attack; d_ = decay; r_ = release;
            attackCoef_ = coef(attack, kAttackRatio);
            attackBase_ = (1.0f + kAttackRatio) * (1.0f - attackCoef_);
            decayCoef_ = coef(decay, kDecayRatio);
            releaseCoef_ = coef(release, kDecayRatio);
        }
        sustain_ = sustain;
        decayBase_ = (sustain_ - kDecayRatio) * (1.0f - decayCoef_);
        releaseBase_ = -kDecayRatio * (1.0f - releaseCoef_);
    }

    // Optional hold time at full level between attack and decay (SFZ ampeg_hold).
    void setHold(float seconds) { holdSamples_ = (int)(seconds * sampleRate_); }

    void gate(bool on) {
        if (on) {
            stage_ = Stage::Attack;
        } else if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
        }
    }
    // Start from zero (new note), as opposed to legato retrigger.
    void reset() { level_ = 0.0f; stage_ = Stage::Idle; }

    // Fast release used when a voice is stolen.
    void kill(float seconds) {
        releaseCoef_ = coef(seconds, kDecayRatio);
        releaseBase_ = -kDecayRatio * (1.0f - releaseCoef_);
        r_ = -1.0f;  // force recompute on next set()
        stage_ = Stage::Release;
    }

    float process() {
        switch (stage_) {
            case Stage::Idle: break;
            case Stage::Attack:
                level_ = attackBase_ + level_ * attackCoef_;
                if (level_ >= 1.0f) {
                    level_ = 1.0f;
                    stage_ = holdSamples_ > 0 ? Stage::Hold : Stage::Decay;
                    holdLeft_ = holdSamples_;
                }
                break;
            case Stage::Hold:
                if (--holdLeft_ <= 0) stage_ = Stage::Decay;
                break;
            case Stage::Decay:
                level_ = decayBase_ + level_ * decayCoef_;
                if (level_ <= sustain_) { level_ = sustain_; stage_ = Stage::Sustain; }
                break;
            case Stage::Sustain:
                level_ = sustain_;
                break;
            case Stage::Release:
                level_ = releaseBase_ + level_ * releaseCoef_;
                if (level_ <= 0.0f) { level_ = 0.0f; stage_ = Stage::Idle; }
                break;
        }
        return level_;
    }

    // Advance n samples, return final level (used for control-rate envelopes).
    float processBlock(int n) {
        float v = level_;
        for (int i = 0; i < n; ++i) v = process();
        return v;
    }

    float level() const { return level_; }
    Stage stage() const { return stage_; }
    bool active() const { return stage_ != Stage::Idle; }

private:
    static constexpr float kAttackRatio = 0.3f;
    static constexpr float kDecayRatio = 0.0001f;

    float coef(float seconds, float ratio) const {
        const float samples = std::fmax(seconds * sampleRate_, 1.0f);
        return std::exp(-std::log((1.0f + ratio) / ratio) / samples);
    }

    float sampleRate_ = 48000.0f;
    float a_ = -1, d_ = -1, r_ = -1;
    float sustain_ = 1.0f;
    float attackCoef_ = 0, attackBase_ = 1;
    float decayCoef_ = 0, decayBase_ = 0;
    float releaseCoef_ = 0, releaseBase_ = 0;
    float level_ = 0.0f;
    int holdSamples_ = 0, holdLeft_ = 0;
    Stage stage_ = Stage::Idle;
};

}  // namespace hsynth
