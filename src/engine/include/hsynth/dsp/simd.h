// Minimal 4-lane float SIMD wrapper: NEON (ARM, the Android target), SSE2
// (x86-64 desktop) or plain scalar code (HSYNTH_SIMD off / other CPUs).
// Only what the DSP hot loops need; every operation has the same meaning on
// all back ends (no fused multiply-add, so results match within rounding of
// the summation order).
#pragma once

#include <cstdint>

#if defined(HSYNTH_SIMD) && HSYNTH_SIMD && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define HSYNTH_SIMD_NEON 1
#elif defined(HSYNTH_SIMD) && HSYNTH_SIMD && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define HSYNTH_SIMD_SSE 1
#endif

namespace hsynth {
namespace simd {

#if defined(HSYNTH_SIMD_NEON)
struct f4 { float32x4_t v; };
struct i4 { int32x4_t v; };
inline f4 load(const float* p) { return {vld1q_f32(p)}; }
inline void store(float* p, f4 a) { vst1q_f32(p, a.v); }
inline f4 set1(float x) { return {vdupq_n_f32(x)}; }
inline f4 set(float a, float b, float c, float d) {
    const float t[4] = {a, b, c, d};
    return {vld1q_f32(t)};
}
inline f4 operator+(f4 a, f4 b) { return {vaddq_f32(a.v, b.v)}; }
inline f4 operator-(f4 a, f4 b) { return {vsubq_f32(a.v, b.v)}; }
inline f4 operator*(f4 a, f4 b) { return {vmulq_f32(a.v, b.v)}; }
inline f4 min(f4 a, f4 b) { return {vminq_f32(a.v, b.v)}; }
inline f4 max(f4 a, f4 b) { return {vmaxq_f32(a.v, b.v)}; }
// a >= b ? x - b : x   (phase wrap)
inline f4 subIfGe(f4 x, f4 a, f4 b) {
    const uint32x4_t m = vcgeq_f32(a.v, b.v);
    return {vbslq_f32(m, vsubq_f32(x.v, b.v), x.v)};
}
inline i4 truncInt(f4 a) { return {vcvtq_s32_f32(a.v)}; }
inline f4 toFloat(i4 a) { return {vcvtq_f32_s32(a.v)}; }
inline void store(int32_t* p, i4 a) { vst1q_s32(p, a.v); }
inline float hsum(f4 a) {
#if defined(__aarch64__)
    return vaddvq_f32(a.v);
#else
    const float32x2_t s = vadd_f32(vget_low_f32(a.v), vget_high_f32(a.v));
    return vget_lane_f32(vpadd_f32(s, s), 0);
#endif
}
#elif defined(HSYNTH_SIMD_SSE)
struct f4 { __m128 v; };
struct i4 { __m128i v; };
inline f4 load(const float* p) { return {_mm_loadu_ps(p)}; }
inline void store(float* p, f4 a) { _mm_storeu_ps(p, a.v); }
inline f4 set1(float x) { return {_mm_set1_ps(x)}; }
inline f4 set(float a, float b, float c, float d) { return {_mm_setr_ps(a, b, c, d)}; }
inline f4 operator+(f4 a, f4 b) { return {_mm_add_ps(a.v, b.v)}; }
inline f4 operator-(f4 a, f4 b) { return {_mm_sub_ps(a.v, b.v)}; }
inline f4 operator*(f4 a, f4 b) { return {_mm_mul_ps(a.v, b.v)}; }
inline f4 min(f4 a, f4 b) { return {_mm_min_ps(a.v, b.v)}; }
inline f4 max(f4 a, f4 b) { return {_mm_max_ps(a.v, b.v)}; }
inline f4 subIfGe(f4 x, f4 a, f4 b) {
    const __m128 m = _mm_cmpge_ps(a.v, b.v);
    return {_mm_sub_ps(x.v, _mm_and_ps(m, b.v))};
}
inline i4 truncInt(f4 a) { return {_mm_cvttps_epi32(a.v)}; }
inline f4 toFloat(i4 a) { return {_mm_cvtepi32_ps(a.v)}; }
inline void store(int32_t* p, i4 a) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), a.v); }
inline float hsum(f4 a) {
    __m128 s = _mm_add_ps(a.v, _mm_movehl_ps(a.v, a.v));        // (0+2, 1+3, ..)
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(s);
}
#else
struct f4 { float v[4]; };
struct i4 { int32_t v[4]; };
inline f4 load(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline void store(float* p, f4 a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline f4 set1(float x) { return {{x, x, x, x}}; }
inline f4 set(float a, float b, float c, float d) { return {{a, b, c, d}}; }
#define HSYNTH_F4_OP(op)                                       \
    inline f4 operator op(f4 a, f4 b) {                        \
        return {{a.v[0] op b.v[0], a.v[1] op b.v[1], a.v[2] op b.v[2], a.v[3] op b.v[3]}}; \
    }
HSYNTH_F4_OP(+)
HSYNTH_F4_OP(-)
HSYNTH_F4_OP(*)
#undef HSYNTH_F4_OP
inline f4 min(f4 a, f4 b) { f4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return r; }
inline f4 max(f4 a, f4 b) { f4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r; }
inline f4 subIfGe(f4 x, f4 a, f4 b) { f4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] >= b.v[i] ? x.v[i] - b.v[i] : x.v[i]; return r; }
inline i4 truncInt(f4 a) { i4 r; for (int i = 0; i < 4; ++i) r.v[i] = (int32_t)a.v[i]; return r; }
inline f4 toFloat(i4 a) { f4 r; for (int i = 0; i < 4; ++i) r.v[i] = (float)a.v[i]; return r; }
inline void store(int32_t* p, i4 a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline float hsum(f4 a) { return (a.v[0] + a.v[2]) + (a.v[1] + a.v[3]); }
#endif

// Dot product of n (multiple of 4) floats with an interpolated kernel:
// sum d[k] * (k0[k] + f * (k1[k] - k0[k])).
inline float dotLerpKernel(const float* d, const float* k0, const float* k1, float f, int n) {
    const f4 vf = set1(f);
    f4 acc = set1(0.0f);
    for (int k = 0; k < n; k += 4) {
        const f4 a = load(k0 + k);
        acc = acc + load(d + k) * (a + vf * (load(k1 + k) - a));
    }
    return hsum(acc);
}

// Same kernel applied to two channels (stereo samples share the kernel).
inline void dotLerpKernel2(const float* d0, const float* d1, const float* k0, const float* k1, float f, int n, float& out0,
                           float& out1) {
    const f4 vf = set1(f);
    f4 acc0 = set1(0.0f), acc1 = set1(0.0f);
    for (int k = 0; k < n; k += 4) {
        const f4 a = load(k0 + k);
        const f4 w = a + vf * (load(k1 + k) - a);
        acc0 = acc0 + load(d0 + k) * w;
        acc1 = acc1 + load(d1 + k) * w;
    }
    out0 = hsum(acc0);
    out1 = hsum(acc1);
}

}  // namespace simd
}  // namespace hsynth
