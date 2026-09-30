#include "dsp/DeltaSigma.h"

#include "dsp/Dot.h"
#include "dsp/Kernel.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <semaphore>
#include <thread>
#include <utility>

namespace dgmod::dsp {

namespace {

// Optimal NTF zero positions, normalized to the band edge (roots of the Legendre polynomial of the order; ds_optzeros
// with opt = 1). Only the non-negative ones; 0 is present for odd orders.
std::vector<double> OptimalZeros(int order) {
    switch (order) {
        case 1: return {0.0};
        case 2: return {0.5773502691896258};
        case 3: return {0.0, 0.7745966692414834};
        case 4: return {0.3399810435848563, 0.8611363115940526};
        case 5: return {0.0, 0.5384693101056831, 0.9061798459386640};
        case 6: return {0.2386191860831969, 0.6612093864662645, 0.9324695142031521};
        case 7: return {0.0, 0.4058451513773972, 0.7415311855993945, 0.9491079123427585};
        default: return {0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363};  // 8
    }
}

std::complex<double> Evaluate(const NtfDesign& d, std::complex<double> z) {
    std::complex<double> num = 1.0, den = 1.0;
    for (const auto& q : d.zeros) num *= z - q;
    for (const auto& p : d.poles) den *= z - p;
    return num / den;
}

// One input frame through all 16 phases: out[k] = sum_j coeffs[j][k] * x[j].
void Interpolate16Scalar(const float* x, const double* coeffs, uint32_t taps, float* out) {
    double acc[16]{};
    for (uint32_t j = 0; j < taps; ++j) {
        const double xv = x[j];
        const double* row = coeffs + size_t(j) * 16;
        for (uint32_t k = 0; k < 16; ++k) acc[k] += row[k] * xv;
    }
    for (uint32_t k = 0; k < 16; ++k) out[k] = static_cast<float>(acc[k]);
}

void Interpolate16Avx2(const float* x, const double* coeffs, uint32_t taps, float* out) {
    __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd(), a2 = _mm256_setzero_pd(), a3 = _mm256_setzero_pd();
    for (uint32_t j = 0; j < taps; ++j) {
        const __m256d xv = _mm256_set1_pd(double(x[j]));
        const double* row = coeffs + size_t(j) * 16;
        a0 = _mm256_fmadd_pd(_mm256_loadu_pd(row), xv, a0);
        a1 = _mm256_fmadd_pd(_mm256_loadu_pd(row + 4), xv, a1);
        a2 = _mm256_fmadd_pd(_mm256_loadu_pd(row + 8), xv, a2);
        a3 = _mm256_fmadd_pd(_mm256_loadu_pd(row + 12), xv, a3);
    }
    _mm_storeu_ps(out, _mm256_cvtpd_ps(a0));
    _mm_storeu_ps(out + 4, _mm256_cvtpd_ps(a1));
    _mm_storeu_ps(out + 8, _mm256_cvtpd_ps(a2));
    _mm_storeu_ps(out + 12, _mm256_cvtpd_ps(a3));
}

// Output of a freshly reset look-ahead modulator until its first decision is committed: the DSD idle pattern.
constexpr uint64_t kIdleHistory = 0x6969696969696969ull;
// Look-ahead cost of a quantizer input beyond kClipKnee: (kClipPenalty * excess)^2. Without it the search steers the
// loop to the clip level (the cheapest filter output is often found there) and clipping adds unshaped error; with it
// the search keeps away from the edge and extends the stable input range (measured at DSD256, depth 64: 0.8 of full
// scale clean with 4 paths and about 0.85 with 8, where the plain loop overloads from about 0.8).
constexpr double kClipKnee = 0.85, kClipPenalty = 1e3;
// Cost of a pruned path: finite (the AVX2 search keys carry bits in the mantissa), far above any real cost.
constexpr double kDead = 1e300, kAlive = 1e299;

template <size_t V>
size_t ArgMinAvx2(const double* a) {
    __m256d m = _mm256_load_pd(a);
    for (size_t j = 1; j < V; ++j) m = _mm256_min_pd(m, _mm256_load_pd(a + 4 * j));
    m = _mm256_min_pd(m, _mm256_permute4x64_pd(m, 0x4E));
    m = _mm256_min_pd(m, _mm256_permute_pd(m, 0x5));
    for (size_t j = 0; j < V; ++j)
        if (const int mask = _mm256_movemask_pd(_mm256_cmp_pd(_mm256_load_pd(a + 4 * j), m, _CMP_EQ_OQ)))
            return 4 * j + size_t(std::countr_zero(unsigned(mask)));
    return 0;
}

// One compare-exchange pass (block size K, distance J) of a bitonic sorting network over the 4V elements of `a`
// (element 4v + l in lane l of a[v]); the whole network sorts ascending, or descending with Desc.
template <size_t V, bool Desc, size_t K, size_t J, size_t Vi>
inline void BitonicExchange(__m256d* a) {
    if constexpr (J >= 4) {
        constexpr size_t W = Vi ^ (J / 4);
        if constexpr (W > Vi) {
            constexpr bool asc = (((4 * Vi) & K) == 0) != Desc;
            const __m256d lo = _mm256_min_pd(a[Vi], a[W]), hi = _mm256_max_pd(a[Vi], a[W]);
            a[Vi] = asc ? lo : hi;
            a[W] = asc ? hi : lo;
        }
    } else {
        constexpr int kMaxLanes = [] {
            int mask = 0;
            for (size_t l = 0; l < 4; ++l) {
                const bool asc = (((4 * Vi + l) & K) == 0) != Desc;
                if (((l & J) == 0) != asc) mask |= 1 << l;
            }
            return mask;
        }();
        const __m256d p = J == 1 ? _mm256_permute_pd(a[Vi], 0x5) : _mm256_permute4x64_pd(a[Vi], 0x4E);
        a[Vi] = _mm256_blend_pd(_mm256_min_pd(a[Vi], p), _mm256_max_pd(a[Vi], p), kMaxLanes);
    }
}

template <size_t V, bool Desc, size_t K = 2, size_t J = 1>
inline void BitonicSort(__m256d* a) {
    if constexpr (K <= 4 * V) {
        [a]<size_t... Vs>(std::index_sequence<Vs...>) {
            (BitonicExchange<V, Desc, K, J, Vs>(a), ...);
        }(std::make_index_sequence<V>{});
        if constexpr (J > 1) BitonicSort<V, Desc, K, J / 2>(a);
        else BitonicSort<V, Desc, K * 2, K>(a);
    }
}

// The 4 largest (Desc) or smallest of the 4V elements of `a`, sorted: each vector sorted, then pairs merged (the
// elementwise max/min of one and the other reversed holds the 4 extremes of both as a bitonic sequence).
template <size_t V, bool Desc>
inline __m256d Extremes4(__m256d* a) {
    for (size_t j = 0; j < V; ++j) BitonicSort<1, Desc>(a + j);
    for (size_t w = 1; w < V; w *= 2) {
        for (size_t j = 0; j + w < V; j += 2 * w) {
            const __m256d r = _mm256_permute4x64_pd(a[j + w], 0x1B);
            a[j] = Desc ? _mm256_max_pd(a[j], r) : _mm256_min_pd(a[j], r);
            BitonicExchange<1, Desc, 8, 2, 0>(a + j);
            BitonicExchange<1, Desc, 8, 1, 0>(a + j);
        }
    }
    return a[0];
}

void FlushDenormals() {
#if defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
#endif
}

double Sinc(double x) {
    if (std::abs(x) < 1e-12) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

}  // namespace

const wchar_t* LookAheadName(LookAhead level) {
    switch (level) {
        case LookAhead::Standard: return L"standard";
        case LookAhead::High: return L"high";
        default: return L"off";
    }
}

LookAheadShape LookAheadFor(LookAhead level, uint32_t dsdRate) {
    if (level == LookAhead::Off || dsdRate == 0 || dsdRate > kLookAheadMaxRate) return {};
    // Paths per level at DSD256; twice as many per halving of the rate (the cost per second stays the same).
    uint32_t paths = level == LookAhead::High ? 8u : 4u;
    for (uint64_t rate = dsdRate; rate * 2 <= kLookAheadMaxRate && paths < DeltaSigmaModulator::kMaxPaths; rate *= 2)
        paths *= 2;
    return {paths, DeltaSigmaModulator::kMaxDepth};
}

double NtfDesign::Magnitude(double f) const { return std::abs(Evaluate(*this, std::polar(1.0, 2.0 * std::numbers::pi * f))); }

double PredictedNoiseDb(const NtfDesign& ntf, double fs, double f0, double f1) {
    // Two-sided density 1/3 over fs: power in [f0, f1] = 2/3 * integral of |NTF|^2 over normalized frequency.
    constexpr int kSteps = 4000;
    const double a = f0 / fs, b = f1 / fs, h = (b - a) / kSteps;
    double sum = 0;
    for (int i = 0; i <= kSteps; ++i) {
        const double m = ntf.Magnitude(a + h * i);
        sum += (i == 0 || i == kSteps ? 0.5 : 1.0) * m * m;
    }
    return 10.0 * std::log10(std::max(2.0 / 3.0 * sum * h, 1e-300) / 0.125);
}

NtfDesign SynthesizeNtf(int order, double osr, double hinf) {
    NtfDesign d;
    d.order = order = std::clamp(order, 1, 8);
    d.osr = osr;
    d.hinf = hinf;
    const double dw = std::numbers::pi / osr;
    for (const double x : OptimalZeros(order)) {
        if (x == 0.0) {
            d.zeros.emplace_back(1.0, 0.0);
        } else {
            d.zeros.push_back(std::polar(1.0, x * dw));
            d.zeros.push_back(std::polar(1.0, -x * dw));
        }
    }
    // Poles: maximally flat high-pass family, parameter x adjusted by a secant search until |NTF(-1)| = hinf.
    double x = std::pow(0.3, order - 1), deltaX = 0, fPrev = 0;
    for (int itn = 1; itn <= 100; ++itn) {
        const double me2 = -0.5 * std::pow(x, 2.0 / order);
        d.poles.clear();
        for (int k = 1; k <= order; ++k) {
            const double w = (2.0 * k + 1.0) * std::numbers::pi / order;
            const std::complex<double> mb2 = 1.0 + me2 * std::polar(1.0, w);
            std::complex<double> p = mb2 - std::sqrt(mb2 * mb2 - 1.0);
            if (std::abs(p) > 1.0) p = 1.0 / p;
            d.poles.push_back(p);
        }
        const double f = Evaluate(d, -1.0).real() - hinf;
        deltaX = itn == 1 ? -f / 100.0 : -f * deltaX / (f - fPrev);
        const double next = x + deltaX;
        x = next > 0 ? next : x * 0.1;
        fPrev = f;
        if (std::abs(f) < 1e-10 || std::abs(deltaX) < 1e-10) break;
    }
    return d;
}

void DeltaSigmaModulator::Configure(const NtfDesign& ntf, LookAheadShape shape) {
    sections_.clear();
    std::vector<std::complex<double>> zp, pp;  // upper half-plane members of conjugate pairs
    double realZero = 0, realPole = 0;
    bool hasReal = false;
    for (const auto& z : ntf.zeros) {
        if (std::abs(z.imag()) < 1e-15) realZero = z.real();
        else if (z.imag() > 0) zp.push_back(z);
    }
    for (const auto& p : ntf.poles) {
        if (std::abs(p.imag()) < 1e-12) {
            realPole = p.real();
            hasReal = true;
        } else if (p.imag() > 0) {
            pp.push_back(p);
        }
    }
    auto byAngle = [](const std::complex<double>& a, const std::complex<double>& b) { return std::arg(a) < std::arg(b); };
    std::sort(zp.begin(), zp.end(), byAngle);
    std::sort(pp.begin(), pp.end(), byAngle);
    if (hasReal) sections_.push_back({-realZero, 0.0, -realPole, 0.0});
    for (size_t i = 0; i < std::min(zp.size(), pp.size()); ++i) {
        // 1 - 2cos(w) z^-1 + z^-2 with -2cos(w) = -2 + 4 sin^2(w/2), exact for the tiny band angles.
        const double s = std::sin(std::arg(zp[i]) * 0.5);
        sections_.push_back({-2.0 + 4.0 * s * s, std::norm(zp[i]), -2.0 * pp[i].real(), std::norm(pp[i])});
    }
    // A stable loop keeps its quantizer input within about +-0.8 at the highest levels it encodes (+-0.72 at 0.707);
    // clipping at 1.0 never touches that, and past the stable range it keeps the most signal (measured at 0.8 of
    // full scale: 93 dB SNR against 47 dB with resets). Far beyond the clip level the loop has run away.
    clip_ = 1.0;
    limit_ = 4.0 * std::pow(2.0, ntf.order * 0.5) + 8.0;
    paths_ = shape.paths && std::has_single_bit(shape.paths) ? std::clamp(shape.paths, 4u, kMaxPaths) : 0u;
    depth_ = paths_ ? std::clamp(shape.depth & ~7u, 8u, kMaxDepth) : 0u;
    // The section cascade is affine in the fed-back error x: every section sees x plus the first states of the
    // sections before it, so x adds (b1 - a1) x and (b2 - a2) x to the states of every section.
    for (size_t k = 0; k < 4; ++k) {
        q1_[k] = k < sections_.size() ? sections_[k].b1 - sections_[k].a1 : 0.0;
        q2_[k] = k < sections_.size() ? sections_[k].b2 - sections_[k].a2 : 0.0;
    }
    Reset();
    resets_ = clips_ = 0;
}

void DeltaSigmaModulator::Reset() {
    for (auto& s : sections_) s.s1 = s.s2 = 0.0;
    // The committed bit is bit depth - 1 after the shift: the first depth - 1 bits out are the idle pattern in phase.
    ResetPaths(depth_ ? kIdleHistory >> (65 - depth_) : 0);
    peakV_ = 0;
}

void DeltaSigmaModulator::ResetPaths(uint64_t history) {
    // One live path (copies of it would only produce identical children); the tree fills up within log2(paths)
    // samples.
    for (size_t k = 0; k < 4; ++k)
        for (uint32_t p = 0; p < kMaxPaths; ++p) trellis_.s1[k][p] = trellis_.s2[k][p] = 0.0;
    for (uint32_t p = 0; p < kMaxPaths; ++p) {
        trellis_.x[p] = 0.0;
        trellis_.cost[p] = p == 0 ? 0.0 : kDead;
        trellis_.history[p] = history;
    }
}

// Runs L independent modulators (lanes) with N sections in one loop: their feedback chains interleave, which hides
// the latency of the strictly serial per-sample recursion.
template <size_t N, size_t L>
void DeltaSigmaModulator::RunLanes(DeltaSigmaModulator* const* mods, const float* const* in, uint32_t stride, uint32_t n,
                                   uint16_t* const* words) {
    double b1[N], b2[N], a1[N], a2[N], s1[L][N], s2[L][N], peak[L], limit[L], clip[L];
    uint64_t clips[L]{};
    for (size_t k = 0; k < N; ++k) {
        const Section& c = mods[0]->sections_[k];
        b1[k] = c.b1, b2[k] = c.b2, a1[k] = c.a1, a2[k] = c.a2;
    }
    for (size_t l = 0; l < L; ++l) {
        for (size_t k = 0; k < N; ++k) s1[l][k] = mods[l]->sections_[k].s1, s2[l][k] = mods[l]->sections_[k].s2;
        peak[l] = mods[l]->peakV_;
        limit[l] = mods[l]->limit_;
        clip[l] = mods[l]->clip_;
    }
    uint32_t word[L]{};
    for (uint32_t i = 0; i < n; ++i) {
        for (size_t l = 0; l < L; ++l) {
            double v = double(in[l][size_t(i) * stride]);
            for (size_t k = 0; k < N; ++k) v += s1[l][k];
            // Branch-free quantizer: the output bits are random, a conditional jump would mispredict half the time.
            const bool bit = !std::signbit(v);
            // Quantization error e, filtered by the NTF sections; taken against the clipped v (overload protection).
            const double vc = std::clamp(v, -clip[l], clip[l]);
            clips[l] += vc != v;
            double x = std::copysign(1.0, v) - vc;
            for (size_t k = 0; k < N; ++k) {
                const double y = x + s1[l][k];
                s1[l][k] = b1[k] * x - a1[k] * y + s2[l][k];
                s2[l][k] = b2[k] * x - a2[k] * y;
                x = y;
            }
            const double av = std::abs(v);
            peak[l] = std::max(peak[l], av);
            if (!(av <= limit[l])) {  // run away (also NaN)
                for (size_t k = 0; k < N; ++k) s1[l][k] = s2[l][k] = 0.0;
                ++mods[l]->resets_;
                peak[l] = 0;
            }
            word[l] = (word[l] << 1) | (bit ? 1u : 0u);
            if ((i & 15) == 15) words[l][i >> 4] = static_cast<uint16_t>(word[l]);
        }
    }
    for (size_t l = 0; l < L; ++l) {
        for (size_t k = 0; k < N; ++k) mods[l]->sections_[k].s1 = s1[l][k], mods[l]->sections_[k].s2 = s2[l][k];
        mods[l]->peakV_ = peak[l];
        mods[l]->clips_ += clips[l];
    }
}

template <size_t L>
void DeltaSigmaModulator::Dispatch(DeltaSigmaModulator* const* mods, const float* const* in, uint32_t stride, uint32_t n,
                                   uint16_t* const* words) {
    switch (mods[0]->sections_.size()) {
        case 1: RunLanes<1, L>(mods, in, stride, n, words); break;
        case 2: RunLanes<2, L>(mods, in, stride, n, words); break;
        case 3: RunLanes<3, L>(mods, in, stride, n, words); break;
        default: RunLanes<4, L>(mods, in, stride, n, words); break;
    }
}

// Pruned tree search over M paths (Janssen & Reefman). The cost of a path is the energy of the noise-shaping filter
// output v - u, which is the quantization error weighted by 1/NTF (the in-band error dominates it), and which keeps
// the loop states small. Every sample advances each path with zero error (in place), costs both children of each path
// by the filter output they lead to and keeps the M cheapest children: every path's cheaper child, except where the
// dearer child of another path is cheaper than the worst of those. A survivor's state is its parent's zero-error
// state plus its fed-back error times (q1, q2); that error is kept per path and added when the path is advanced next.
// The bit `depth` samples back on the best path is committed; paths that disagree with it are pruned.
template <size_t N, size_t M>
void DeltaSigmaModulator::RunLookAhead(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    constexpr double kInf = std::numeric_limits<double>::infinity();
    double b1[N], b2[N], a1[N], a2[N], q1[N], q2[N];
    double qsum = 0;  // change of the next filter output per unit of fed-back error
    for (size_t k = 0; k < N; ++k) {
        const Section& c = sections_[k];
        b1[k] = c.b1, b2[k] = c.b2, a1[k] = c.a1, a2[k] = c.a2, q1[k] = q1_[k], q2[k] = q2_[k];
        qsum += q1[k];
    }
    Paths& t = trellis_;
    const uint32_t shift = depth_ - 1;
    const double clip = clip_, limit = limit_;
    double peak = peakV_;
    uint64_t clips = 0;
    uint32_t word = 0;
    alignas(64) double sum[M], next[M], v[M], good[M], bad[M], cost[M];
    alignas(64) uint64_t hist[M];
    uint32_t src[M];
    bool goodUp[M];
    for (uint32_t i = 0; i < n; ++i) {
        const double u = double(in[size_t(i) * stride]);
        for (size_t p = 0; p < M; ++p) sum[p] = next[p] = 0.0;
        for (size_t k = 0; k < N; ++k) {
            for (size_t p = 0; p < M; ++p) {
                const double y = sum[p] + (t.s1[k][p] + t.x[p] * q1[k]);
                const double s1 = b1[k] * sum[p] - a1[k] * y + (t.s2[k][p] + t.x[p] * q2[k]);
                t.s2[k][p] = b2[k] * sum[p] - a2[k] * y;
                t.s1[k][p] = s1;
                next[p] += s1;
                sum[p] = y;
            }
        }
        for (size_t p = 0; p < M; ++p) {
            const double vp = u + sum[p];
            const double vc = std::clamp(vp, -clip, clip);
            const double fUp = next[p] + (1.0 - vc) * qsum, fDown = next[p] + (-1.0 - vc) * qsum;
            // Clipping feeds unshaped error to the output: paths that approach the clip level are penalized.
            const double over = std::max(std::abs(vp) - kClipKnee, 0.0) * kClipPenalty;
            const double c = t.cost[p] + over * over;
            const double cUp = c + fUp * fUp, cDown = c + fDown * fDown;
            const bool ok = std::abs(vp) <= limit;  // false for a run-away path (also NaN): pruned
            v[p] = vp;
            goodUp[p] = cUp <= cDown;
            good[p] = ok ? std::min(cUp, cDown) : kDead;
            bad[p] = ok ? std::max(cUp, cDown) : kDead;
            cost[p] = good[p];
            src[p] = static_cast<uint32_t>(p);
        }
        // A bad child is never cheaper than its own path's good child: a path whose bad child survives keeps its good
        // child too, so a replaced slot is never a parent.
        uint32_t flipped = 0;
        for (;;) {
            size_t w = 0, b = 0;
            for (size_t p = 1; p < M; ++p) {
                if (good[p] > good[w]) w = p;
                if (bad[p] < bad[b]) b = p;
            }
            if (!(bad[b] < good[w])) break;
            cost[w] = bad[b];
            src[w] = static_cast<uint32_t>(b);
            flipped |= 1u << w;
            good[w] = -kInf;
            bad[b] = kInf;
        }
        for (uint32_t f = flipped; f; f &= f - 1) {
            const size_t p = size_t(std::countr_zero(f)), s = src[p];
            for (size_t k = 0; k < N; ++k) t.s1[k][p] = t.s1[k][s], t.s2[k][p] = t.s2[k][s];
        }
        for (size_t p = 0; p < M; ++p) {
            const uint32_t s = src[p];
            const bool up = goodUp[s] != (((flipped >> p) & 1u) != 0);
            t.x[p] = (up ? 1.0 : -1.0) - std::clamp(v[s], -clip, clip);
            hist[p] = (t.history[s] << 1) | (up ? 1u : 0u);
        }
        size_t best = 0;
        for (size_t p = 1; p < M; ++p)
            if (cost[p] < cost[best]) best = p;
        const uint64_t bit = (hist[best] >> shift) & 1u;
        const double base = cost[best];
        if (base < kAlive) {
            for (size_t p = 0; p < M; ++p) {
                t.cost[p] = ((hist[p] >> shift) & 1u) == bit ? cost[p] - base : kDead;
                t.history[p] = hist[p];
            }
            const double av = std::abs(v[src[best]]);
            peak = std::max(peak, av);
            clips += av > clip;
        } else {  // every path ran away
            ResetPaths(hist[best]);
            ++resets_;
            peak = 0;
        }
        word = (word << 1) | static_cast<uint32_t>(bit);
        if ((i & 15) == 15) words[i >> 4] = static_cast<uint16_t>(word);
    }
    peakV_ = peak;
    clips_ += clips;
}

// RunLookAhead with the paths in AVX2 vectors (the same search, with FMA rounding).
template <size_t N, size_t M>
void DeltaSigmaModulator::RunLookAheadAvx2(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    static_assert(M % 4 == 0);
    constexpr size_t V = M / 4;
    __m256d b1[N], b2[N], a1[N], a2[N], q1[N], q2[N];
    double qsum = 0;
    for (size_t k = 0; k < N; ++k) {
        const Section& c = sections_[k];
        b1[k] = _mm256_set1_pd(c.b1), b2[k] = _mm256_set1_pd(c.b2), a1[k] = _mm256_set1_pd(c.a1);
        a2[k] = _mm256_set1_pd(c.a2), q1[k] = _mm256_set1_pd(q1_[k]), q2[k] = _mm256_set1_pd(q2_[k]);
        qsum += q1_[k];
    }
    Paths& t = trellis_;
    const uint32_t shift = depth_ - 1;
    const double clip = clip_;
    const __m256d dead = _mm256_set1_pd(kDead), one = _mm256_set1_pd(1.0), minusOne = _mm256_set1_pd(-1.0),
                  hi = _mm256_set1_pd(clip), lo = _mm256_set1_pd(-clip), knee = _mm256_set1_pd(kClipKnee),
                  penalty = _mm256_set1_pd(kClipPenalty), limit = _mm256_set1_pd(limit_), vq = _mm256_set1_pd(qsum),
                  twoQ = _mm256_set1_pd(2.0 * qsum), zero = _mm256_setzero_pd(),
                  absMask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
    const __m256i lsb = _mm256_set1_epi64x(1), keyMask = _mm256_set1_epi64x(~int64_t{63});
    const __m128i count = _mm_cvtsi32_si128(int(shift));
    __m256i slotKey[V];  // slot p as key bits: p << 1 (child flag in bit 0)
    for (size_t j = 0; j < V; ++j)
        slotKey[j] = _mm256_setr_epi64x(int64_t(8 * j), int64_t(8 * j + 2), int64_t(8 * j + 4), int64_t(8 * j + 6));
    double peak = peakV_;
    uint64_t clips = 0;
    uint32_t word = 0;
    alignas(64) double v[M], vcs[M], bad[M], cost[M];
    alignas(64) uint64_t hist[M], goodKeys[4], badKeys[4];
    uint32_t src[M];
    for (uint32_t i = 0; i < n; ++i) {
        const __m256d u = _mm256_set1_pd(double(in[size_t(i) * stride]));
        __m256d sum[V], next[V], up[V], gk[V], bk[V];
        for (size_t j = 0; j < V; ++j) sum[j] = next[j] = zero;
        __m256d xs[V];
        for (size_t j = 0; j < V; ++j) xs[j] = _mm256_load_pd(t.x + 4 * j);
        for (size_t k = 0; k < N; ++k) {
            for (size_t j = 0; j < V; ++j) {
                const __m256d s1 = _mm256_fmadd_pd(xs[j], q1[k], _mm256_load_pd(t.s1[k] + 4 * j));
                const __m256d s2 = _mm256_fmadd_pd(xs[j], q2[k], _mm256_load_pd(t.s2[k] + 4 * j));
                const __m256d y = _mm256_add_pd(sum[j], s1);
                const __m256d n1 = _mm256_fmadd_pd(b1[k], sum[j], _mm256_fnmadd_pd(a1[k], y, s2));
                const __m256d n2 = _mm256_fnmadd_pd(a2[k], y, _mm256_mul_pd(b2[k], sum[j]));
                _mm256_store_pd(t.s1[k] + 4 * j, n1);
                _mm256_store_pd(t.s2[k] + 4 * j, n2);
                next[j] = _mm256_add_pd(next[j], n1);
                sum[j] = y;
            }
        }
        uint32_t goodUp = 0;
        for (size_t j = 0; j < V; ++j) {
            const __m256d vp = _mm256_add_pd(u, sum[j]);
            const __m256d vc = _mm256_min_pd(_mm256_max_pd(vp, lo), hi);
            const __m256d fUp = _mm256_fmadd_pd(_mm256_sub_pd(one, vc), vq, next[j]);
            const __m256d fDown = _mm256_sub_pd(fUp, twoQ);
            const __m256d av = _mm256_and_pd(vp, absMask);
            const __m256d over = _mm256_mul_pd(_mm256_max_pd(_mm256_sub_pd(av, knee), zero), penalty);
            const __m256d c = _mm256_fmadd_pd(over, over, _mm256_load_pd(t.cost + 4 * j));
            const __m256d cUp = _mm256_fmadd_pd(fUp, fUp, c), cDown = _mm256_fmadd_pd(fDown, fDown, c);
            const __m256d ok = _mm256_cmp_pd(av, limit, _CMP_LE_OQ);  // false for a run-away path (also NaN)
            up[j] = _mm256_cmp_pd(cUp, cDown, _CMP_LE_OQ);
            const __m256d g = _mm256_blendv_pd(dead, _mm256_min_pd(cUp, cDown), ok);
            const __m256d b = _mm256_blendv_pd(dead, _mm256_max_pd(cUp, cDown), ok);
            _mm256_store_pd(v + 4 * j, vp);
            _mm256_store_pd(vcs + 4 * j, vc);
            _mm256_store_pd(cost + 4 * j, g);
            _mm256_store_pd(bad + 4 * j, b);
            goodUp |= uint32_t(_mm256_movemask_pd(up[j])) << (4 * j);
            // Sort keys: cost + 1 (a normal number, never flushed as a denormal) with the slot and the child in the
            // low mantissa bits: distinct, ordered like the costs, and a path's good child always before its bad one.
            gk[j] = _mm256_castsi256_pd(
                _mm256_or_si256(_mm256_and_si256(_mm256_castpd_si256(_mm256_add_pd(g, one)), keyMask), slotKey[j]));
            bk[j] = _mm256_castsi256_pd(_mm256_or_si256(
                _mm256_and_si256(_mm256_castpd_si256(_mm256_add_pd(b, one)), keyMask), _mm256_or_si256(slotKey[j], lsb)));
        }
        // The M cheapest of the 2M children: with the good children sorted descending and the bad ones ascending, the
        // elementwise minimum. Its first k places take bad children: those slots' (the k worst) good children drop
        // out, and the k cheapest bad children move in (their paths keep their good children, so no parent is lost).
        // At most 4 per sample (the extremes only, not full sorts): more is rare, and slots left over (pruned paths)
        // are filled a sample later.
        const __m256d worstGood = Extremes4<V, true>(gk), bestBad = Extremes4<V, false>(bk);
        const uint32_t lower = uint32_t(_mm256_movemask_pd(_mm256_cmp_pd(bestBad, worstGood, _CMP_LT_OQ)));
        _mm256_store_si256(reinterpret_cast<__m256i*>(goodKeys), _mm256_castpd_si256(worstGood));
        _mm256_store_si256(reinterpret_cast<__m256i*>(badKeys), _mm256_castpd_si256(bestBad));
        for (size_t j = 0; j < V; ++j) {
            _mm256_store_pd(t.x + 4 * j, _mm256_sub_pd(_mm256_blendv_pd(minusOne, one, up[j]), _mm256_load_pd(vcs + 4 * j)));
            const __m256i h = _mm256_slli_epi64(_mm256_load_si256(reinterpret_cast<const __m256i*>(t.history + 4 * j)), 1);
            _mm256_store_si256(reinterpret_cast<__m256i*>(hist + 4 * j),
                               _mm256_or_si256(h, _mm256_and_si256(_mm256_castpd_si256(up[j]), lsb)));
        }
        // Slots taken over by a bad child get its parent's state, error, history and cost.
        uint32_t flipped = 0;
        for (uint32_t r = 0, k = uint32_t(std::popcount(lower)); r < k; ++r) {
            const uint32_t p = uint32_t(goodKeys[r] & 63) >> 1, b = uint32_t(badKeys[r] & 63) >> 1;
            const bool upB = ((goodUp >> b) & 1u) == 0;
            src[p] = b;
            flipped |= 1u << p;
            cost[p] = bad[b];
            t.x[p] = (upB ? 1.0 : -1.0) - vcs[b];
            hist[p] = (t.history[b] << 1) | (upB ? 1u : 0u);
            for (size_t k2 = 0; k2 < N; ++k2) t.s1[k2][p] = t.s1[k2][b], t.s2[k2][p] = t.s2[k2][b];
        }
        const size_t best = ArgMinAvx2<V>(cost);
        const uint64_t bit = (hist[best] >> shift) & 1u;
        const double base = cost[best];
        if (base < kAlive) {
            const __m256d vb = _mm256_set1_pd(base);
            const __m256i vbit = _mm256_set1_epi64x(int64_t(bit));
            for (size_t j = 0; j < V; ++j) {
                const __m256i h = _mm256_load_si256(reinterpret_cast<const __m256i*>(hist + 4 * j));
                const __m256i keep = _mm256_cmpeq_epi64(_mm256_and_si256(_mm256_srl_epi64(h, count), lsb), vbit);
                _mm256_store_pd(t.cost + 4 * j, _mm256_blendv_pd(dead, _mm256_sub_pd(_mm256_load_pd(cost + 4 * j), vb),
                                                                 _mm256_castsi256_pd(keep)));
                _mm256_store_si256(reinterpret_cast<__m256i*>(t.history + 4 * j), h);
            }
            const double av = std::abs(v[(flipped >> best) & 1u ? src[best] : best]);
            peak = std::max(peak, av);
            clips += av > clip;
        } else {  // every path ran away
            ResetPaths(hist[best]);
            ++resets_;
            peak = 0;
        }
        word = (word << 1) | static_cast<uint32_t>(bit);
        if ((i & 15) == 15) words[i >> 4] = static_cast<uint16_t>(word);
    }
    peakV_ = peak;
    clips_ += clips;
}

template <size_t N>
void DeltaSigmaModulator::DispatchLookAhead(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    if (Kernels().level == SimdLevel::Avx2) {
        switch (paths_) {
            case 4: RunLookAheadAvx2<N, 4>(in, stride, n, words); break;
            case 8: RunLookAheadAvx2<N, 8>(in, stride, n, words); break;
            case 16: RunLookAheadAvx2<N, 16>(in, stride, n, words); break;
            default: RunLookAheadAvx2<N, 32>(in, stride, n, words); break;
        }
        return;
    }
    switch (paths_) {
        case 4: RunLookAhead<N, 4>(in, stride, n, words); break;
        case 8: RunLookAhead<N, 8>(in, stride, n, words); break;
        case 16: RunLookAhead<N, 16>(in, stride, n, words); break;
        default: RunLookAhead<N, 32>(in, stride, n, words); break;
    }
}

void DeltaSigmaModulator::Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    if (paths_) {
        switch (sections_.size()) {
            case 1: DispatchLookAhead<1>(in, stride, n, words); break;
            case 2: DispatchLookAhead<2>(in, stride, n, words); break;
            case 3: DispatchLookAhead<3>(in, stride, n, words); break;
            default: DispatchLookAhead<4>(in, stride, n, words); break;
        }
        return;
    }
    DeltaSigmaModulator* mods[] = {this};
    Dispatch<1>(mods, &in, stride, n, &words);
}

void DeltaSigmaModulator::ProcessPair(DeltaSigmaModulator& a, DeltaSigmaModulator& b, const float* inA, const float* inB,
                                      uint32_t stride, uint32_t n, uint16_t* wordsA, uint16_t* wordsB) {
    if (a.paths_ || b.paths_) {
        a.Process(inA, stride, n, wordsA);
        b.Process(inB, stride, n, wordsB);
        return;
    }
    DeltaSigmaModulator* mods[] = {&a, &b};
    const float* in[] = {inA, inB};
    uint16_t* words[] = {wordsA, wordsB};
    Dispatch<2>(mods, in, stride, n, words);
}

// Look-ahead helper thread: encodes the channels [first, last) of each block while the caller encodes the others.
struct DsdEncoder::Helper {
    DsdEncoder* owner = nullptr;
    const float* in = nullptr;
    uint32_t frames = 0, first = 0, last = 0;
    bool quit = false;
    std::binary_semaphore start{0}, done{0};
    std::thread thread;

    void Loop() {
        FlushDenormals();
        for (;;) {
            start.acquire();
            if (quit) return;
            owner->EncodeChannels(first, last, in, frames);
            done.release();
        }
    }
    ~Helper() {
        quit = true;
        start.release();
        if (thread.joinable()) thread.join();
    }
};

DsdEncoder::DsdEncoder() = default;
DsdEncoder::~DsdEncoder() = default;

bool DsdEncoder::Configure(uint32_t channels, uint32_t frameRate, double bandHz, uint32_t maxFrames, LookAhead lookAhead,
                           int order, double hinf) {
    helper_.reset();
    if (channels == 0 || frameRate == 0 || maxFrames == 0) return false;
    channels_ = channels;
    frameRate_ = frameRate;
    maxFrames_ = maxFrames;

    // Interpolation by 16: pass band up to `bandHz`, first image from frameRate - bandHz; 150 dB Kaiser design.
    constexpr double kAttenuationDb = 150.0;
    const double transition = std::max(0.1, (double(frameRate) - 2.0 * bandHz) / double(frameRate));
    auto taps = static_cast<uint32_t>(std::ceil((kAttenuationDb - 7.95) / (14.36 * transition) + 1.0));
    taps_ = std::clamp((taps + 7u) & ~7u, 8u, 64u);
    const double length = double(taps_) * kFactor;
    const double center = (length - 1.0) * 0.5;
    const double beta = KaiserBeta(kAttenuationDb), i0 = BesselI0(beta);
    const double fc2 = 1.0 / kFactor;  // cutoff at frameRate / 2
    coeffs_.assign(size_t(kFactor) * taps_, 0.0);
    for (uint32_t k = 0; k < kFactor; ++k) {
        std::vector<double> row(taps_);
        double sum = 0;
        for (uint32_t jj = 0; jj < taps_; ++jj) {
            // History index jj (oldest first) is input n - (taps - 1 - jj); output 16n + k.
            const double t = double(kFactor) * double(taps_ - 1 - jj) + double(k) - center;
            const double xw = t / (length * 0.5);
            const double w = std::abs(xw) >= 1.0 ? 0.0 : BesselI0(beta * std::sqrt(1.0 - xw * xw)) / i0;
            row[jj] = fc2 * Sinc(fc2 * t) * w;
            sum += row[jj];
        }
        for (uint32_t jj = 0; jj < taps_; ++jj) coeffs_[size_t(jj) * kFactor + k] = row[jj] / sum;  // unity DC per phase
    }

    const double fs = DsdRate();
    const double floorBand = std::min(bandHz, 22050.0);
    noiseBand_ = floorBand;
    ntf_ = SynthesizeNtf(order, fs / (2.0 * floorBand), hinf);
    for (double band = 50000.0; band > floorBand; band -= 1000.0) {
        const NtfDesign wide = SynthesizeNtf(order, fs / (2.0 * band), hinf);
        if (PredictedNoiseDb(wide, fs, 0.0, 20000.0) <= -160.0 && PredictedNoiseDb(wide, fs, 0.0, band) <= -140.0) {
            noiseBand_ = band;
            ntf_ = wide;
            break;
        }
    }
    modulators_.assign(channels, {});
    for (auto& m : modulators_) m.Configure(ntf_, LookAheadFor(lookAhead, DsdRate()));
    shape_ = modulators_[0].Shape();
    history_.assign(channels, std::vector<float>(size_t(taps_) - 1 + maxFrames, 0.0f));
    upsampled_.assign(channels, std::vector<float>(size_t(kFactor) * maxFrames, 0.0f));
    words_.assign(channels, std::vector<uint16_t>(maxFrames, 0));
    if (shape_.paths && channels > 1) {
        helper_ = std::make_unique<Helper>();
        Helper* h = helper_.get();
        h->owner = this;
        h->thread = std::thread([h, wrapper = wrapper_] {
            if (wrapper) wrapper([h] { h->Loop(); });
            else h->Loop();
        });
    }
    return true;
}

void DsdEncoder::Reset() {
    for (auto& h : history_) std::fill(h.begin(), h.end(), 0.0f);
    for (auto& m : modulators_) m.Reset();
}

void DsdEncoder::CopyStateFrom(const DsdEncoder& other) {
    for (uint32_t c = 0; c < channels_ && c < other.channels_; ++c) {
        std::copy_n(other.history_[c].begin(), taps_ - 1, history_[c].begin());
        modulators_[c] = other.modulators_[c];  // same section count: no allocation
    }
}

uint64_t DsdEncoder::Resets() const {
    uint64_t n = 0;
    for (const auto& m : modulators_) n += m.Resets();
    return n;
}

uint64_t DsdEncoder::Clips() const {
    uint64_t n = 0;
    for (const auto& m : modulators_) n += m.Clips();
    return n;
}

double DsdEncoder::PeakQuantizerInput() const {
    double v = 0;
    for (const auto& m : modulators_) v = std::max(v, m.PeakQuantizerInput());
    return v;
}

void DsdEncoder::Interpolate(uint32_t c, const float* in, uint32_t n) {
    float* h = history_[c].data();
    float* cur = h + taps_ - 1;
    for (uint32_t i = 0; i < n; ++i) cur[i] = in[size_t(i) * channels_ + c];
    // All 16 phases at once: each history sample is broadcast against one row of 16 coefficients.
    float* up = upsampled_[c].data();
    const auto interpolate = Kernels().level == SimdLevel::Avx2 ? Interpolate16Avx2 : Interpolate16Scalar;
    for (uint32_t i = 0; i < n; ++i) interpolate(h + i, coeffs_.data(), taps_, up + size_t(i) * kFactor);
    std::copy(h + n, h + n + taps_ - 1, h);
}

void DsdEncoder::EncodeChannels(uint32_t first, uint32_t last, const float* in, uint32_t n) {
    for (uint32_t c = first; c < last; ++c) {
        Interpolate(c, in, n);
        modulators_[c].Process(upsampled_[c].data(), 1, n * kFactor, words_[c].data());
    }
}

void DsdEncoder::Process(const float* in, uint32_t frames, uint16_t* out) {
    while (frames > 0) {
        const uint32_t n = std::min(frames, maxFrames_);
        if (helper_) {
            // Look-ahead: the upper half of the channels on the helper thread.
            const uint32_t split = (channels_ + 1) / 2;
            helper_->in = in;
            helper_->frames = n;
            helper_->first = split;
            helper_->last = channels_;
            helper_->start.release();
            EncodeChannels(0, split, in, n);
            helper_->done.acquire();
        } else if (shape_.paths) {
            EncodeChannels(0, channels_, in, n);
        } else {
            for (uint32_t c = 0; c < channels_; ++c) Interpolate(c, in, n);
            uint32_t c = 0;
            for (; c + 1 < channels_; c += 2)
                DeltaSigmaModulator::ProcessPair(modulators_[c], modulators_[c + 1], upsampled_[c].data(),
                                                 upsampled_[c + 1].data(), 1, n * kFactor, words_[c].data(),
                                                 words_[c + 1].data());
            if (c < channels_) modulators_[c].Process(upsampled_[c].data(), 1, n * kFactor, words_[c].data());
        }
        for (uint32_t ch = 0; ch < channels_; ++ch)
            for (uint32_t i = 0; i < n; ++i) out[size_t(i) * channels_ + ch] = words_[ch][i];
        in += size_t(n) * channels_;
        out += size_t(n) * channels_;
        frames -= n;
    }
}

}  // namespace dgmod::dsp
