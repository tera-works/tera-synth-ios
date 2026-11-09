#include "hsynth/dsp/oscillators.h"

namespace hsynth {

namespace {
float g_table[kSineTableSize + 1];
struct SineInit {
    SineInit() {
        for (int i = 0; i <= kSineTableSize; ++i) g_table[i] = (float)std::sin(2.0 * 3.14159265358979323846 * i / kSineTableSize);
    }
} g_sineInit;
}  // namespace

const float* const g_sineTable = g_table;

}  // namespace hsynth
