// Channel vocoder. Modulator = external audio input (microphone / line, see
// Engine::setInputBuffer); carrier = the synth signal entering the block.
#include "fx_blocks.h"

namespace hsynth {
namespace fx {

void VocoderFx::prepare(float sr) {
    sr_ = sr;
    gateEnv_.set(0.001f, 0.1f, sr);
    hfEnv_.set(0.001f, 0.03f, sr);
    hfHp_.set(Biquad::HP, 5000.0f, 0.707f, 0, sr);
    noiseHp_.set(Biquad::HP, 6000.0f, 0.707f, 0, sr);
    mixR_.prepare(sr, 1.0f);
    init_ = false;
    reset();
}
void VocoderFx::reset() {
    for (int k = 0; k < kMaxBands; ++k)
        for (int s = 0; s < 2; ++s) { ana_[k][s].reset(); synL_[k][s].reset(); synR_[k][s].reset(); }
    for (auto& e : env_) e.reset();
    gateEnv_.reset(); hfEnv_.reset(); hfHp_.reset(); noiseHp_.reset();
    gateG_ = 0;
}
void VocoderFx::update(const float* p, const FxContext&) {
    inGain_ = dbToLin(p[vocoder::InputGain]);
    gateThr_ = dbToLin(p[vocoder::GateThresh]);
    unvoiced_ = p[vocoder::Unvoiced];
    mixR_.target = p[vocoder::Mix];
    bool changed = !init_;
    for (int k = 0; k < vocoder::Count; ++k)
        if (p[k] != last_[k]) { changed = true; last_[k] = p[k]; }
    if (!changed) return;
    init_ = true;
    bands_ = (int)clampf(p[vocoder::Bands], 4, kMaxBands);
    const float lo = p[vocoder::LowFreq];
    const float hi = std::max(p[vocoder::HighFreq], lo * 2.0f);
    // band spacing in octaves -> Q of a filter one band wide, scaled by the Q knob
    const float span = std::log2(hi / lo) / (float)(bands_ - 1);
    const float qBand = 1.0f / (std::pow(2.0f, span * 0.5f) - std::pow(2.0f, -span * 0.5f));
    const float q = qBand * (0.5f + 1.5f * p[vocoder::Q]);
    const float shift = fastExp2(p[vocoder::Formant] / 12.0f);
    for (int k = 0; k < bands_; ++k) {
        const float f = lo * std::pow(hi / lo, (float)k / (float)(bands_ - 1));
        for (int s = 0; s < 2; ++s) {
            ana_[k][s].set(Biquad::BP, f, q, 0, sr_);
            synL_[k][s].set(Biquad::BP, f * shift, q, 0, sr_);
            synR_[k][s].copyCoefs(synL_[k][s]);
        }
        env_[k].set(p[vocoder::Attack] * 0.001f, p[vocoder::Release] * 0.001f, sr_);
    }
    // two cascaded band-passes lose level with narrow bands: compensate
    makeup_ = 4.0f * std::sqrt(q) * std::sqrt(16.0f / (float)bands_);
}
void VocoderFx::process(float* L, float* R, int n, const FxContext& c) {
    const float gStep = 1.0f - tauCoef(0.005f, sr_);
    for (int i = 0; i < n; ++i) {
        float mod = 0.0f;
        if (c.inL) mod = 0.5f * (c.inL[i] + (c.inR ? c.inR[i] : c.inL[i])) * inGain_;
        const float ge = gateEnv_.process(mod);
        gateG_ += gStep * ((ge > gateThr_ ? 1.0f : 0.0f) - gateG_);
        mod *= gateG_;
        float yl = 0, yr = 0;
        for (int k = 0; k < bands_; ++k) {
            const float a = ana_[k][1].process(ana_[k][0].process(mod));
            const float e = env_[k].process(a);
            yl += synL_[k][1].process(synL_[k][0].process(L[i])) * e;
            yr += synR_[k][1].process(synR_[k][0].process(R[i])) * e;
        }
        yl *= makeup_;
        yr *= makeup_;
        if (unvoiced_ > 0.001f) {
            // sibilants: high-passed noise following the modulator's top band
            const float he = hfEnv_.process(hfHp_.process(mod));
            const float noise = noiseHp_.process(rng_.bipolar()) * he * unvoiced_ * 4.0f;
            yl += noise;
            yr += noise;
        }
        const float m = mixR_.next();
        L[i] += (yl - L[i]) * m;
        R[i] += (yr - R[i]) * m;
    }
}

}  // namespace fx
}  // namespace hsynth
