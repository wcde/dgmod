#include "dsp/Dot.h"

#if defined(_M_X64) || defined(_M_IX86)
#define DGMOD_X86 1
#include <immintrin.h>
#include <intrin.h>
#endif

namespace dgmod::dsp {

namespace {

double DotScalar(const float* x, const float* c, uint32_t n) {
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (uint32_t i = 0; i < n; i += 4) {
        a0 += double(x[i]) * double(c[i]);
        a1 += double(x[i + 1]) * double(c[i + 1]);
        a2 += double(x[i + 2]) * double(c[i + 2]);
        a3 += double(x[i + 3]) * double(c[i + 3]);
    }
    return (a0 + a1) + (a2 + a3);
}

double DotLerpScalar(const float* x, const float* c0, const float* c1, float t, uint32_t n) {
    double a0 = 0, a1 = 0;
    for (uint32_t i = 0; i < n; i += 2) {
        a0 += double(x[i]) * double(c0[i] + t * (c1[i] - c0[i]));
        a1 += double(x[i + 1]) * double(c0[i + 1] + t * (c1[i + 1] - c0[i + 1]));
    }
    return a0 + a1;
}

double DotCubicScalar(const float* x, const float* const* c, const float* w, uint32_t n) {
    double a0 = 0, a1 = 0;
    for (uint32_t i = 0; i < n; i += 2) {
        const float k0 = w[0] * c[0][i] + w[1] * c[1][i] + w[2] * c[2][i] + w[3] * c[3][i];
        const float k1 = w[0] * c[0][i + 1] + w[1] * c[1][i + 1] + w[2] * c[2][i + 1] + w[3] * c[3][i + 1];
        a0 += double(x[i]) * double(k0);
        a1 += double(x[i + 1]) * double(k1);
    }
    return a0 + a1;
}

#if DGMOD_X86

double HorizontalSum(__m128d v) {
    return _mm_cvtsd_f64(_mm_add_sd(v, _mm_unpackhi_pd(v, v)));
}

double DotSse2(const float* x, const float* c, uint32_t n) {
    __m128d a0 = _mm_setzero_pd(), a1 = _mm_setzero_pd(), a2 = _mm_setzero_pd(), a3 = _mm_setzero_pd();
    for (uint32_t i = 0; i < n; i += 8) {
        const __m128 x0 = _mm_loadu_ps(x + i), x1 = _mm_loadu_ps(x + i + 4);
        const __m128 c0 = _mm_load_ps(c + i), c1 = _mm_load_ps(c + i + 4);
        a0 = _mm_add_pd(a0, _mm_mul_pd(_mm_cvtps_pd(x0), _mm_cvtps_pd(c0)));
        a1 = _mm_add_pd(a1, _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(x0, x0)), _mm_cvtps_pd(_mm_movehl_ps(c0, c0))));
        a2 = _mm_add_pd(a2, _mm_mul_pd(_mm_cvtps_pd(x1), _mm_cvtps_pd(c1)));
        a3 = _mm_add_pd(a3, _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(x1, x1)), _mm_cvtps_pd(_mm_movehl_ps(c1, c1))));
    }
    return HorizontalSum(_mm_add_pd(_mm_add_pd(a0, a1), _mm_add_pd(a2, a3)));
}

double DotLerpSse2(const float* x, const float* c0p, const float* c1p, float t, uint32_t n) {
    const __m128 tv = _mm_set1_ps(t);
    __m128d a0 = _mm_setzero_pd(), a1 = _mm_setzero_pd();
    for (uint32_t i = 0; i < n; i += 4) {
        const __m128 xv = _mm_loadu_ps(x + i);
        const __m128 c0 = _mm_load_ps(c0p + i), c1 = _mm_load_ps(c1p + i);
        const __m128 cv = _mm_add_ps(c0, _mm_mul_ps(tv, _mm_sub_ps(c1, c0)));
        a0 = _mm_add_pd(a0, _mm_mul_pd(_mm_cvtps_pd(xv), _mm_cvtps_pd(cv)));
        a1 = _mm_add_pd(a1, _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(xv, xv)), _mm_cvtps_pd(_mm_movehl_ps(cv, cv))));
    }
    return HorizontalSum(_mm_add_pd(a0, a1));
}

double DotCubicSse2(const float* x, const float* const* c, const float* w, uint32_t n) {
    const __m128 w0 = _mm_set1_ps(w[0]), w1 = _mm_set1_ps(w[1]), w2 = _mm_set1_ps(w[2]), w3 = _mm_set1_ps(w[3]);
    __m128d a0 = _mm_setzero_pd(), a1 = _mm_setzero_pd();
    for (uint32_t i = 0; i < n; i += 4) {
        const __m128 xv = _mm_loadu_ps(x + i);
        __m128 k = _mm_mul_ps(w0, _mm_load_ps(c[0] + i));
        k = _mm_add_ps(k, _mm_mul_ps(w1, _mm_load_ps(c[1] + i)));
        k = _mm_add_ps(k, _mm_mul_ps(w2, _mm_load_ps(c[2] + i)));
        k = _mm_add_ps(k, _mm_mul_ps(w3, _mm_load_ps(c[3] + i)));
        a0 = _mm_add_pd(a0, _mm_mul_pd(_mm_cvtps_pd(xv), _mm_cvtps_pd(k)));
        a1 = _mm_add_pd(a1, _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(xv, xv)), _mm_cvtps_pd(_mm_movehl_ps(k, k))));
    }
    return HorizontalSum(_mm_add_pd(a0, a1));
}

double HorizontalSum256(__m256d v) {
    const __m128d lo = _mm256_castpd256_pd128(v), hi = _mm256_extractf128_pd(v, 1);
    return HorizontalSum(_mm_add_pd(lo, hi));
}

double DotAvx2(const float* x, const float* c, uint32_t n) {
    __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd(), a2 = _mm256_setzero_pd(), a3 = _mm256_setzero_pd();
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m256 x0 = _mm256_loadu_ps(x + i), x1 = _mm256_loadu_ps(x + i + 8);
        const __m256 c0 = _mm256_load_ps(c + i), c1 = _mm256_load_ps(c + i + 8);
        a0 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x0)), _mm256_cvtps_pd(_mm256_castps256_ps128(c0)), a0);
        a1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x0, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(c0, 1)), a1);
        a2 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x1)), _mm256_cvtps_pd(_mm256_castps256_ps128(c1)), a2);
        a3 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x1, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(c1, 1)), a3);
    }
    if (i < n) {  // n is a multiple of 8
        const __m256 x0 = _mm256_loadu_ps(x + i);
        const __m256 c0 = _mm256_load_ps(c + i);
        a0 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x0)), _mm256_cvtps_pd(_mm256_castps256_ps128(c0)), a0);
        a1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x0, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(c0, 1)), a1);
    }
    const double r = HorizontalSum256(_mm256_add_pd(_mm256_add_pd(a0, a1), _mm256_add_pd(a2, a3)));
    _mm256_zeroupper();
    return r;
}

double DotLerpAvx2(const float* x, const float* c0p, const float* c1p, float t, uint32_t n) {
    const __m256 tv = _mm256_set1_ps(t);
    __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
    for (uint32_t i = 0; i < n; i += 8) {
        const __m256 xv = _mm256_loadu_ps(x + i);
        const __m256 c0 = _mm256_load_ps(c0p + i), c1 = _mm256_load_ps(c1p + i);
        const __m256 cv = _mm256_fmadd_ps(tv, _mm256_sub_ps(c1, c0), c0);
        a0 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(xv)), _mm256_cvtps_pd(_mm256_castps256_ps128(cv)), a0);
        a1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(xv, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(cv, 1)), a1);
    }
    const double r = HorizontalSum256(_mm256_add_pd(a0, a1));
    _mm256_zeroupper();
    return r;
}

double DotCubicAvx2(const float* x, const float* const* c, const float* w, uint32_t n) {
    const __m256 w0 = _mm256_set1_ps(w[0]), w1 = _mm256_set1_ps(w[1]), w2 = _mm256_set1_ps(w[2]), w3 = _mm256_set1_ps(w[3]);
    __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
    for (uint32_t i = 0; i < n; i += 8) {
        const __m256 xv = _mm256_loadu_ps(x + i);
        __m256 k = _mm256_mul_ps(w0, _mm256_load_ps(c[0] + i));
        k = _mm256_fmadd_ps(w1, _mm256_load_ps(c[1] + i), k);
        k = _mm256_fmadd_ps(w2, _mm256_load_ps(c[2] + i), k);
        k = _mm256_fmadd_ps(w3, _mm256_load_ps(c[3] + i), k);
        a0 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(xv)), _mm256_cvtps_pd(_mm256_castps256_ps128(k)), a0);
        a1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(xv, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(k, 1)), a1);
    }
    const double r = HorizontalSum256(_mm256_add_pd(a0, a1));
    _mm256_zeroupper();
    return r;
}

bool CpuHasAvx2Fma() {
    int info[4]{};
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool avx = (info[2] & (1 << 28)) != 0;
    const bool fma = (info[2] & (1 << 12)) != 0;
    if (!osxsave || !avx || !fma) return false;
    if ((_xgetbv(0) & 0x6) != 0x6) return false;  // OS saves XMM and YMM state
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
}

#endif

DotKernels Make(SimdLevel level) {
    switch (level) {
#if DGMOD_X86
        case SimdLevel::Avx2: return {DotAvx2, DotLerpAvx2, DotCubicAvx2, SimdLevel::Avx2};
        case SimdLevel::Sse2: return {DotSse2, DotLerpSse2, DotCubicSse2, SimdLevel::Sse2};
#endif
        default: return {DotScalar, DotLerpScalar, DotCubicScalar, SimdLevel::Scalar};
    }
}

SimdLevel BestLevel() {
#if DGMOD_X86
    return CpuHasAvx2Fma() ? SimdLevel::Avx2 : SimdLevel::Sse2;
#else
    return SimdLevel::Scalar;
#endif
}

}  // namespace

const DotKernels& Kernels() {
    static const DotKernels kernels = Make(BestLevel());
    return kernels;
}

DotKernels KernelsFor(SimdLevel level) {
    const SimdLevel best = BestLevel();
    if (static_cast<int>(level) > static_cast<int>(best)) level = best;
    return Make(level);
}

bool CpuHasAvx512() {
#if DGMOD_X86
    static const bool has = [] {
        if (!CpuHasAvx2Fma()) return false;
        if ((_xgetbv(0) & 0xE6) != 0xE6) return false;  // XMM, YMM, opmask and both ZMM parts
        int info[4]{};
        __cpuidex(info, 7, 0);
        return (info[1] & (1 << 16)) != 0;
    }();
    return has;
#else
    return false;
#endif
}

const wchar_t* SimdLevelName(SimdLevel level) {
    switch (level) {
        case SimdLevel::Avx2: return L"AVX2+FMA";
        case SimdLevel::Sse2: return L"SSE2";
        default: return L"Scalar";
    }
}

}  // namespace dgmod::dsp
