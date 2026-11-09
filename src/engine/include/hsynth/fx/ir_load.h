// Loading user impulse responses (cabinet / IR reverb) from WAV files.
// Not real-time: call on a loader thread, then Engine::setFxImpulse().
#pragma once

#include <memory>
#include <string>

#include "hsynth/fx/convolver.h"

namespace hsynth {
namespace fx {

// target 0 = cabinet (mono IR, max 0.68 s), 1 = IR reverb (stereo IR, max 6 s).
// Resamples to sampleRate, trims the silent tail, normalises to unit energy.
std::unique_ptr<ConvIr> loadImpulseResponse(const std::string& path, float sampleRate, int target, std::string& error);

// Same from samples already in memory (right may be null).
std::unique_ptr<ConvIr> makeImpulseResponse(const float* left, const float* right, int frames, float fileRate,
                                            float sampleRate, int target, const std::string& name);

}  // namespace fx
}  // namespace hsynth
