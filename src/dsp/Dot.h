#pragma once

#include <cstdint>

namespace dgmod::dsp {

// Inner products used by the resampler. Inputs are float32, accumulation is double precision.
// `n` must be a multiple of 8; `c` (and `c1`) must be 32-byte aligned, `x` may be unaligned.
using DotFn = double (*)(const float* x, const float* c, uint32_t n);
// Sum of x[i] * (c0[i] + t * (c1[i] - c0[i])), used between two stored filter phases.
using DotLerpFn = double (*)(const float* x, const float* c0, const float* c1, float t, uint32_t n);
// Sum of x[i] * (w[0]*c[0][i] + w[1]*c[1][i] + w[2]*c[2][i] + w[3]*c[3][i]): cubic interpolation between four
// consecutive stored phases (-1, 0, +1, +2 around the target position).
using DotCubicFn = double (*)(const float* x, const float* const* c, const float* w, uint32_t n);

// Cubic Lagrange weights for points -1, 0, 1, 2 at fractional position t in [0, 1).
inline void CubicWeights(float t, float* w) {
    const float tm1 = t - 1.0f, tm2 = t - 2.0f, tp1 = t + 1.0f;
    w[0] = -t * tm1 * tm2 * (1.0f / 6.0f);
    w[1] = tp1 * tm1 * tm2 * 0.5f;
    w[2] = -tp1 * t * tm2 * 0.5f;
    w[3] = tp1 * t * tm1 * (1.0f / 6.0f);
}

enum class SimdLevel { Scalar, Sse2, Avx2 };

struct DotKernels {
    DotFn dot;
    DotLerpFn dotLerp;
    DotCubicFn dotCubic;
    SimdLevel level;
};

// Best implementation for the current CPU (detected once, thread-safe).
const DotKernels& Kernels();
// Specific implementation (for tests); falls back to the best available one if `level` is unsupported.
DotKernels KernelsFor(SimdLevel level);
const wchar_t* SimdLevelName(SimdLevel level);

}  // namespace dgmod::dsp
