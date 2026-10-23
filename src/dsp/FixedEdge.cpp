#include "dsp/FixedEdge.h"

#include "dsp/Dot.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dgmod::dsp {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
// Cost of a pruned path: far above any real cost.
constexpr double kDead = 1e300, kAlive = 1e299;
constexpr uint32_t kWidthBits = 6;
constexpr uint64_t kHistoryMask = (uint64_t{1} << (kWidthBits * FixedEdgeModulator::kDepth)) - 1;
constexpr uint32_t kOldestShift = kWidthBits * (FixedEdgeModulator::kDepth - 1);
// Committed cells wait in a 64-bit word: at most 16 bits before a cell is added (see Process).
static_assert(FixedEdgeModulator::kMaxCell + 16 <= 64);

// Bits of a cell of `cell` bits and width d, first bit in the MSB of the low `cell` bits.
constexpr uint64_t CellBits(uint32_t cell, uint32_t d) {
    return ((uint64_t{1} << d) - 1) << (cell - d);
}

constexpr uint32_t Lanes(uint32_t cell, uint32_t minRun) {
    return (cell - 2 * minRun + 1 + 7) / 8 * 8;
}

// The bit-level loop (DeltaSigmaModulator's error-feedback cascade without the clipping), for building the matrices:
// state s1 of the four sections, then their s2.
struct Loop {
    NtfSection sec[4];
    double s[8]{};

    // Returns the loop filter output w (the quantizer input is u + w), then takes bit y.
    double Step(double u, double y) {
        const double w = s[0] + s[1] + s[2] + s[3];
        double x = y - u - w;
        for (size_t k = 0; k < 4; ++k) {
            const double yk = x + s[k];
            const double s1 = sec[k].b1 * x - sec[k].a1 * yk + s[4 + k];
            s[4 + k] = sec[k].b2 * x - sec[k].a2 * yk;
            s[k] = s1;
            x = yk;
        }
        return w;
    }
};

double Sum4(__m256d v) {
    const __m128d h = _mm_add_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1));
    return _mm_cvtsd_f64(_mm_add_sd(h, _mm_unpackhi_pd(h, h)));
}

}  // namespace

const FixedEdgeShape* FixedEdgeShapeFor(uint32_t dsdRate) {
    for (const auto& s : kFixedEdgeShapes)
        if (s.rate == dsdRate) return &s;
    return nullptr;
}

bool FixedEdgeModulator::UsesAvx512() const {
    return allowAvx2_ && allowAvx512_ && Kernels().level == SimdLevel::Avx2 && CpuHasAvx512();
}

void FixedEdgeModulator::Selection::Insert(double c, uint32_t k) {
    uint32_t r = kPaths - 1;
    for (; r > 0 && cost[r - 1] > c; --r) {
        cost[r] = cost[r - 1];
        key[r] = key[r - 1];
    }
    cost[r] = c;
    key[r] = k;
}

void FixedEdgeModulator::Configure(const NtfDesign& ntf, const FixedEdgeShape& shape) {
    shape_ = shape;
    const uint32_t cell = shape.cell, widths = cell - 2 * shape.minRun + 1;
    Loop proto;
    const auto sections = NtfSections(ntf);
    for (size_t k = 0; k < 4 && k < sections.size(); ++k) proto.sec[k] = sections[k];

    // Responses over one cell: w per bit and the end state, to a unit state, a unit input sample, and each width.
    double h[kMaxCell][kStates], g[kMaxCell][kMaxCell], rd[kMaxLanes][kMaxCell];
    m_ = {};
    for (uint32_t j = 0; j < kStates; ++j) {
        Loop l = proto;
        l.s[j] = 1.0;
        for (uint32_t i = 0; i < cell; ++i) h[i][j] = l.Step(0.0, 0.0);
        for (uint32_t i = 0; i < kStates; ++i) m_.b[j][i] = l.s[i];
    }
    for (uint32_t k = 0; k < cell; ++k) {
        Loop l = proto;
        for (uint32_t i = 0; i < cell; ++i) m_.gu[k][i] = g[i][k] = l.Step(i == k ? 1.0 : 0.0, 0.0);
        for (uint32_t i = 0; i < kStates; ++i) m_.ku[k][kStates + i] = l.s[i];
    }
    for (uint32_t w = 0; w < widths; ++w) {
        Loop l = proto;
        const uint32_t d = w + shape.minRun;
        for (uint32_t i = 0; i < cell; ++i) rd[w][i] = l.Step(0.0, i < d ? 1.0 : -1.0);
        for (uint32_t i = 0; i < kStates; ++i) m_.c[w][i] = l.s[i];
    }
    for (uint32_t a = 0; a < kStates; ++a) {
        for (uint32_t b = 0; b < kStates; ++b) {
            double q = 0;
            for (uint32_t i = 0; i < cell; ++i) q += h[i][a] * h[i][b];
            m_.qk[b][a] = q;
        }
        for (uint32_t k = 0; k < cell; ++k) {
            double v = 0;
            for (uint32_t i = 0; i < cell; ++i) v += h[i][a] * g[i][k];
            m_.ku[k][a] = 2.0 * v;
        }
        for (uint32_t w = 0; w < widths; ++w) {
            double v = 0;
            for (uint32_t i = 0; i < cell; ++i) v += h[i][a] * rd[w][i];
            m_.p[a][w] = 2.0 * v;
        }
    }
    for (uint32_t w = 0; w < kMaxLanes; ++w) {
        if (w >= widths) {
            m_.r[w] = kInf;  // never selected
            continue;
        }
        double r = 0;
        for (uint32_t i = 0; i < cell; ++i) r += rd[w][i] * rd[w][i];
        m_.r[w] = r;
        for (uint32_t k = 0; k < cell; ++k) {
            double v = 0;
            for (uint32_t i = 0; i < cell; ++i) v += g[i][k] * rd[w][i];
            m_.tu[k][w] = 2.0 * v;
        }
    }
    // The loop filter output stays within about +-2.1 at the highest levels encoded (0.65 of full scale, DC).
    limit_ = 32.0;
    Reset();
    resets_ = 0;
}

uint64_t FixedEdgeModulator::IdleHistory() const {
    uint64_t h = 0;
    for (uint32_t k = 0; k < kDepth; ++k) h = (h << kWidthBits) | (shape_.cell / 2);
    return h;
}

void FixedEdgeModulator::Reset() {
    ResetPaths(IdleHistory());
    pos_ = 0;
    // One idle cell ahead of the commits: the paths start with kDepth idle cells, so the first real cell is
    // committed after kDepth - 1 of them and comes out Latency() bits late.
    out_ = CellBits(shape_.cell, shape_.cell / 2);
    pending_ = shape_.cell;
    peakV_ = 0;
}

void FixedEdgeModulator::CopyStateFrom(const FixedEdgeModulator& other) {
    paths_[0] = other.paths_[0];
    paths_[1] = other.paths_[1];
    cur_ = other.cur_;
    std::copy(std::begin(other.cellIn_), std::end(other.cellIn_), cellIn_);
    pos_ = other.pos_;
    out_ = other.out_;
    pending_ = other.pending_;
    peakV_ = other.peakV_;
    resets_ = other.resets_;
}

void FixedEdgeModulator::ResetPaths(uint64_t history) {
    Paths& t = paths_[cur_];
    t = {};
    for (uint32_t p = 0; p < kPaths; ++p) {
        t.cost[p] = p == 0 ? 0.0 : kDead;
        t.history[p] = history;
    }
}

// One cell of L input samples, widths R .. L - R. The AVX-512 kernel performs the AVX2 kernel's operations lane by
// lane (and its two sums in the same order), so both produce the same bits.
template <uint32_t L, uint32_t R, FixedEdgeModulator::Isa I>
void FixedEdgeModulator::RunCell() {
    constexpr uint32_t kWidths = L - 2 * R + 1, kLanes = Lanes(L, R);
    constexpr uint32_t V = kLanes / 4, VG = L / 4, Z = kLanes / 8, ZG = L / 8;
    // Keys carried in the low mantissa bits of the AVX2 / AVX-512 search.
    constexpr uint64_t kKeyMask = std::bit_ceil(uint64_t{kPaths} * kLanes) - 1;
    static_assert(L % 8 == 0 && kLanes <= kMaxLanes && L <= kMaxCell && kKeyMask < 1024);
    static_assert(kPaths == 16);

    const double* u = cellIn_;
    const Paths& now = paths_[cur_];
    Paths& next = paths_[cur_ ^ 1];
    peakV_ = std::max(peakV_, std::abs(u[0] + now.s[0][0] + now.s[0][1] + now.s[0][2] + now.s[0][3]));

    // Shared by all paths: 2K u and Bu u, and per width 2 T_d u + |R_d|^2 + |G u|^2 + 1: every cost is then the
    // energy of w plus one (relative path costs are never negative), positive, so the vector search can carry the
    // candidate in its low mantissa bits.
    alignas(64) double ku[2 * kStates], t[kLanes];
    if constexpr (I == Isa::Avx512) {
        __m512d k0 = _mm512_setzero_pd(), k1 = k0, tv[Z], g[ZG];
        for (auto& v : g) v = _mm512_setzero_pd();
        for (uint32_t v = 0; v < Z; ++v) tv[v] = _mm512_load_pd(m_.r + 8 * v);
        for (uint32_t i = 0; i < L; ++i) {
            const __m512d ui = _mm512_set1_pd(u[i]);
            k0 = _mm512_fmadd_pd(ui, _mm512_load_pd(m_.ku[i]), k0);
            k1 = _mm512_fmadd_pd(ui, _mm512_load_pd(m_.ku[i] + 8), k1);
            for (uint32_t v = 0; v < Z; ++v) tv[v] = _mm512_fmadd_pd(ui, _mm512_load_pd(m_.tu[i] + 8 * v), tv[v]);
            for (uint32_t v = 0; v < ZG; ++v) g[v] = _mm512_fmadd_pd(ui, _mm512_load_pd(m_.gu[i] + 8 * v), g[v]);
        }
        // |G u|^2 summed as the AVX2 kernel does: 4 lanes, its vectors in order.
        const __m256d g0 = _mm512_castpd512_pd256(g[0]);
        __m256d gg = _mm256_fmadd_pd(_mm512_extractf64x4_pd(g[0], 1), _mm512_extractf64x4_pd(g[0], 1), _mm256_mul_pd(g0, g0));
        for (uint32_t v = 1; v < ZG; ++v) {
            const __m256d lo = _mm512_castpd512_pd256(g[v]), hi = _mm512_extractf64x4_pd(g[v], 1);
            gg = _mm256_fmadd_pd(hi, hi, _mm256_fmadd_pd(lo, lo, gg));
        }
        const __m512d offset = _mm512_set1_pd(Sum4(gg) + 1.0);
        _mm512_store_pd(ku, k0);
        _mm512_store_pd(ku + 8, k1);
        for (uint32_t v = 0; v < Z; ++v) _mm512_store_pd(t + 8 * v, _mm512_add_pd(tv[v], offset));
    } else if constexpr (I == Isa::Avx2) {
        __m256d k[4], tv[V], g[VG];
        for (auto& v : k) v = _mm256_setzero_pd();
        for (auto& v : g) v = _mm256_setzero_pd();
        for (uint32_t v = 0; v < V; ++v) tv[v] = _mm256_load_pd(m_.r + 4 * v);
        for (uint32_t i = 0; i < L; ++i) {
            const __m256d ui = _mm256_set1_pd(u[i]);
            for (uint32_t v = 0; v < 4; ++v) k[v] = _mm256_fmadd_pd(ui, _mm256_load_pd(m_.ku[i] + 4 * v), k[v]);
            for (uint32_t v = 0; v < V; ++v) tv[v] = _mm256_fmadd_pd(ui, _mm256_load_pd(m_.tu[i] + 4 * v), tv[v]);
            for (uint32_t v = 0; v < VG; ++v) g[v] = _mm256_fmadd_pd(ui, _mm256_load_pd(m_.gu[i] + 4 * v), g[v]);
        }
        __m256d gg = _mm256_mul_pd(g[0], g[0]);
        for (uint32_t v = 1; v < VG; ++v) gg = _mm256_fmadd_pd(g[v], g[v], gg);
        const __m256d offset = _mm256_set1_pd(Sum4(gg) + 1.0);
        for (uint32_t v = 0; v < 4; ++v) _mm256_store_pd(ku + 4 * v, k[v]);
        for (uint32_t v = 0; v < V; ++v) _mm256_store_pd(t + 4 * v, _mm256_add_pd(tv[v], offset));
    } else {
        double g[L]{};
        std::fill(std::begin(ku), std::end(ku), 0.0);
        std::copy_n(m_.r, kLanes, t);
        for (uint32_t i = 0; i < L; ++i) {
            for (uint32_t j = 0; j < 2 * kStates; ++j) ku[j] += u[i] * m_.ku[i][j];
            for (uint32_t d = 0; d < kLanes; ++d) t[d] += u[i] * m_.tu[i][d];
            for (uint32_t j = 0; j < L; ++j) g[j] += u[i] * m_.gu[i][j];
        }
        double offset = 1.0;
        for (const double v : g) offset += v * v;
        for (double& v : t) v += offset;
    }

    // Every live path extended by every width; the kPaths cheapest kept (the best path comes first, so the
    // threshold drops early).
    // Vector kernels: the kPaths cheapest so far sorted ascending (4 AVX2 or 2 AVX-512 vectors), the key in the low
    // mantissa bits (distinct values in the order of the costs). Inserting x: A[i] becomes min(A[i], max(A[i - 1], x)),
    // which leaves A as it is for x at or above its last element; no branches.
    Selection sel;
    std::fill(std::begin(sel.cost), std::end(sel.cost), kInf);
    std::fill(std::begin(sel.key), std::end(sel.key), 0u);
    [[maybe_unused]] __m256d top[4];
    [[maybe_unused]] __m512d top0, top1;
    if constexpr (I == Isa::Avx2)
        for (auto& v : top) v = _mm256_set1_pd(kInf);
    if constexpr (I == Isa::Avx512) top0 = top1 = _mm512_set1_pd(kInf);
    for (uint32_t p = 0; p < kPaths; ++p) {
        if (!(now.cost[p] < kAlive)) continue;
        const double* s = now.s[p];
        alignas(64) double cost[kLanes];
        if constexpr (I == Isa::Avx512) {
            __m512d z = _mm512_load_pd(ku), c[Z];
            for (uint32_t v = 0; v < Z; ++v) c[v] = _mm512_load_pd(t + 8 * v);
            for (uint32_t j = 0; j < kStates; ++j) {
                const __m512d sj = _mm512_set1_pd(s[j]);
                z = _mm512_fmadd_pd(sj, _mm512_load_pd(m_.qk[j]), z);
                for (uint32_t v = 0; v < Z; ++v) c[v] = _mm512_fmadd_pd(sj, _mm512_load_pd(m_.p[j] + 8 * v), c[v]);
            }
            const __m256d sz = _mm256_fmadd_pd(_mm256_load_pd(s), _mm512_castpd512_pd256(z),
                                               _mm256_mul_pd(_mm256_load_pd(s + 4), _mm512_extractf64x4_pd(z, 1)));
            const __m512d base = _mm512_set1_pd(now.cost[p] + Sum4(sz));
            const __m512d threshold = _mm512_permutexvar_pd(_mm512_set1_epi64(7), top1);
            const __m512i keyMask = _mm512_set1_epi64(~int64_t(kKeyMask));
            const __m512i lane = _mm512_setr_epi64(0, 1, 2, 3, 4, 5, 6, 7);
            uint64_t below = 0;
            for (uint32_t v = 0; v < Z; ++v) {
                const __m512i key = _mm512_add_epi64(_mm512_set1_epi64(int64_t(p * kLanes + 8 * v)), lane);
                // Padding lanes (+inf) turn into NaN: never below the threshold.
                const __m512d packed = _mm512_castsi512_pd(
                    _mm512_or_si512(_mm512_and_si512(_mm512_castpd_si512(_mm512_add_pd(c[v], base)), keyMask), key));
                _mm512_store_pd(cost + 8 * v, packed);
                below |= uint64_t(_mm512_cmp_pd_mask(packed, threshold, _CMP_LT_OQ)) << (8 * v);
            }
            const __m512d negInf = _mm512_set1_pd(-kInf);
            const __m512i shift0 = _mm512_setr_epi64(8, 0, 1, 2, 3, 4, 5, 6), shift1 = _mm512_setr_epi64(7, 8, 9, 10, 11, 12, 13, 14);
            for (; below; below &= below - 1) {
                const __m512d x = _mm512_set1_pd(cost[std::countr_zero(below)]);
                const __m512d prev0 = _mm512_permutex2var_pd(top0, shift0, negInf);
                const __m512d prev1 = _mm512_permutex2var_pd(top0, shift1, top1);
                top0 = _mm512_min_pd(top0, _mm512_max_pd(prev0, x));
                top1 = _mm512_min_pd(top1, _mm512_max_pd(prev1, x));
            }
        } else if constexpr (I == Isa::Avx2) {
            __m256d z0 = _mm256_load_pd(ku), z1 = _mm256_load_pd(ku + 4), c[V];
            for (uint32_t v = 0; v < V; ++v) c[v] = _mm256_load_pd(t + 4 * v);
            for (uint32_t j = 0; j < kStates; ++j) {
                const __m256d sj = _mm256_set1_pd(s[j]);
                z0 = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.qk[j]), z0);
                z1 = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.qk[j] + 4), z1);
                for (uint32_t v = 0; v < V; ++v) c[v] = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.p[j] + 4 * v), c[v]);
            }
            const __m256d sz = _mm256_fmadd_pd(_mm256_load_pd(s), z0, _mm256_mul_pd(_mm256_load_pd(s + 4), z1));
            const __m256d base = _mm256_set1_pd(now.cost[p] + Sum4(sz));
            const __m256d threshold = _mm256_permute4x64_pd(top[3], 0xFF);
            const __m256i keyMask = _mm256_set1_epi64x(~int64_t(kKeyMask));
            uint64_t below = 0;
            for (uint32_t v = 0; v < V; ++v) {
                const __m256i key =
                    _mm256_add_epi64(_mm256_set1_epi64x(int64_t(p * kLanes + 4 * v)), _mm256_setr_epi64x(0, 1, 2, 3));
                const __m256d packed = _mm256_castsi256_pd(
                    _mm256_or_si256(_mm256_and_si256(_mm256_castpd_si256(_mm256_add_pd(c[v], base)), keyMask), key));
                _mm256_store_pd(cost + 4 * v, packed);
                below |= uint64_t(_mm256_movemask_pd(_mm256_cmp_pd(packed, threshold, _CMP_LT_OQ))) << (4 * v);
            }
            const __m256d negInf = _mm256_set1_pd(-kInf);
            for (; below; below &= below - 1) {
                const __m256d x = _mm256_set1_pd(cost[std::countr_zero(below)]);
                __m256d prev[4];
                prev[0] = _mm256_blend_pd(_mm256_permute4x64_pd(top[0], 0x90), negInf, 1);
                for (uint32_t k = 1; k < 4; ++k)
                    prev[k] = _mm256_blend_pd(_mm256_permute4x64_pd(top[k], 0x90), _mm256_permute4x64_pd(top[k - 1], 0xFF), 1);
                for (uint32_t k = 0; k < 4; ++k) top[k] = _mm256_min_pd(top[k], _mm256_max_pd(prev[k], x));
            }
        } else {
            double z[kStates];
            for (uint32_t i = 0; i < kStates; ++i) z[i] = ku[i];
            for (uint32_t j = 0; j < kStates; ++j)
                for (uint32_t i = 0; i < kStates; ++i) z[i] += s[j] * m_.qk[j][i];
            double base = now.cost[p];
            for (uint32_t i = 0; i < kStates; ++i) base += s[i] * z[i];
            for (uint32_t d = 0; d < kLanes; ++d) cost[d] = t[d];
            for (uint32_t j = 0; j < kStates; ++j)
                for (uint32_t d = 0; d < kLanes; ++d) cost[d] += s[j] * m_.p[j][d];
            for (uint32_t d = 0; d < kWidths; ++d) {
                const double c = cost[d] + base;
                if (c < sel.cost[kPaths - 1]) sel.Insert(c, p * kLanes + d);
            }
        }
    }
    if constexpr (I == Isa::Avx512) {
        _mm512_store_pd(sel.cost, top0);
        _mm512_store_pd(sel.cost + 8, top1);
    } else if constexpr (I == Isa::Avx2) {
        for (uint32_t k = 0; k < 4; ++k) _mm256_store_pd(sel.cost + 4 * k, top[k]);
    }
    if constexpr (I != Isa::Scalar)
        for (uint32_t r = 0; r < kPaths; ++r) sel.key[r] = uint32_t(std::bit_cast<uint64_t>(sel.cost[r]) & kKeyMask);

    uint32_t commit = L / 2;
    if (sel.cost[0] < kAlive) {
        for (uint32_t r = 0; r < kPaths; ++r) {
            const uint32_t p = sel.key[r] / kLanes, w = sel.key[r] % kLanes;
            const double* s = now.s[p];
            double* ns = next.s[r];
            if constexpr (I == Isa::Avx512) {
                __m512d a = _mm512_add_pd(_mm512_load_pd(ku + kStates), _mm512_load_pd(m_.c[w]));
                for (uint32_t j = 0; j < kStates; ++j) a = _mm512_fmadd_pd(_mm512_set1_pd(s[j]), _mm512_load_pd(m_.b[j]), a);
                _mm512_store_pd(ns, a);
            } else if constexpr (I == Isa::Avx2) {
                __m256d a0 = _mm256_add_pd(_mm256_load_pd(ku + kStates), _mm256_load_pd(m_.c[w]));
                __m256d a1 = _mm256_add_pd(_mm256_load_pd(ku + kStates + 4), _mm256_load_pd(m_.c[w] + 4));
                for (uint32_t j = 0; j < kStates; ++j) {
                    const __m256d sj = _mm256_set1_pd(s[j]);
                    a0 = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.b[j]), a0);
                    a1 = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.b[j] + 4), a1);
                }
                _mm256_store_pd(ns, a0);
                _mm256_store_pd(ns + 4, a1);
            } else {
                for (uint32_t i = 0; i < kStates; ++i) ns[i] = ku[kStates + i] + m_.c[w][i];
                for (uint32_t j = 0; j < kStates; ++j)
                    for (uint32_t i = 0; i < kStates; ++i) ns[i] += s[j] * m_.b[j][i];
            }
            next.history[r] = ((now.history[p] << kWidthBits) | (w + R)) & kHistoryMask;
        }
        // Commit the oldest cell of the best path; paths that disagree with it, and run-away ones, are pruned.
        commit = uint32_t(next.history[0] >> kOldestShift) & 63u;
        bool alive = false;
        for (uint32_t r = 0; r < kPaths; ++r) {
            const double* ns = next.s[r];
            const bool ok = (uint32_t(next.history[r] >> kOldestShift) & 63u) == commit && sel.cost[r] < kAlive &&
                            std::abs(ns[0] + ns[1] + ns[2] + ns[3]) <= limit_;
            next.cost[r] = ok ? sel.cost[r] - sel.cost[0] : kDead;
            alive |= ok;
        }
        if (alive && !(next.cost[0] < kAlive)) {
            // The best path itself ran away: the cheapest live one moves to the front.
            uint32_t r = 1;
            while (!(next.cost[r] < kAlive)) ++r;
            std::swap(next.s[0], next.s[r]);
            std::swap(next.cost[0], next.cost[r]);
            std::swap(next.history[0], next.history[r]);
        }
        cur_ ^= 1;
        if (!alive) {
            ResetPaths(next.history[0]);
            ++resets_;
            peakV_ = 0;
        }
    } else {  // every path ran away (non-finite costs)
        ResetPaths(IdleHistory());
        ++resets_;
        peakV_ = 0;
    }
    out_ = (out_ << L) | CellBits(L, commit);
    pending_ += L;
}

FixedEdgeModulator::CellFn FixedEdgeModulator::Kernel() const {
    const Isa isa = UsesAvx512()                                              ? Isa::Avx512
                    : allowAvx2_ && Kernels().level == SimdLevel::Avx2 ? Isa::Avx2
                                                                            : Isa::Scalar;
    // Every shape is 24 / 2 (kFixedEdgeShapes); the kernels take other cells and runs as template parameters.
    return isa == Isa::Avx512 ? &FixedEdgeModulator::RunCell<24, 2, Isa::Avx512>
           : isa == Isa::Avx2 ? &FixedEdgeModulator::RunCell<24, 2, Isa::Avx2>
                              : &FixedEdgeModulator::RunCell<24, 2, Isa::Scalar>;
}

void FixedEdgeModulator::Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    const CellFn run = Kernel();
    const uint32_t cell = shape_.cell;
    for (uint32_t i = 0; i < n; ++i) {
        cellIn_[pos_] = double(in[size_t(i) * stride]);
        if (++pos_ == cell) {
            (this->*run)();
            pos_ = 0;
        }
        if ((i & 15) == 15) {
            // Never short: one idle cell ahead plus a cell per `cell` samples always covers the samples taken (and
            // at most 16 bits wait when a cell is added).
            pending_ -= 16;
            words[i >> 4] = static_cast<uint16_t>(out_ >> pending_);
        }
    }
}

}  // namespace dgmod::dsp
