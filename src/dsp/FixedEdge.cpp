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
constexpr uint32_t kWidthBits = 5;
constexpr uint64_t kHistoryMask = (uint64_t{1} << (kWidthBits * FixedEdgeModulator::kDepth)) - 1;
constexpr uint32_t kOldestShift = kWidthBits * (FixedEdgeModulator::kDepth - 1);
constexpr uint32_t kIdleWidth = FixedEdgeModulator::kCell / 2;

// Cell bits of width d, first bit in the MSB of the low kCell bits.
constexpr uint64_t CellBits(uint32_t d) {
    return ((uint64_t{1} << d) - 1) << (FixedEdgeModulator::kCell - d);
}

constexpr uint64_t IdleHistory() {
    uint64_t h = 0;
    for (uint32_t k = 0; k < FixedEdgeModulator::kDepth; ++k) h = (h << kWidthBits) | kIdleWidth;
    return h;
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

void FixedEdgeModulator::Selection::Insert(double c, uint32_t k) {
    uint32_t r = kPaths - 1;
    for (; r > 0 && cost[r - 1] > c; --r) {
        cost[r] = cost[r - 1];
        key[r] = key[r - 1];
    }
    cost[r] = c;
    key[r] = k;
}

void FixedEdgeModulator::Configure(const NtfDesign& ntf) {
    Loop proto;
    const auto sections = NtfSections(ntf);
    for (size_t k = 0; k < 4 && k < sections.size(); ++k) proto.sec[k] = sections[k];

    // Responses over one cell: w per bit and the end state, to a unit state, a unit input sample, and each width.
    double h[kCell][kStates], g[kCell][kCell], rd[kWidths][kCell];
    m_ = {};
    for (uint32_t j = 0; j < kStates; ++j) {
        Loop l = proto;
        l.s[j] = 1.0;
        for (uint32_t i = 0; i < kCell; ++i) h[i][j] = l.Step(0.0, 0.0);
        for (uint32_t i = 0; i < kStates; ++i) m_.b[j][i] = l.s[i];
    }
    for (uint32_t k = 0; k < kCell; ++k) {
        Loop l = proto;
        for (uint32_t i = 0; i < kCell; ++i) m_.gu[k][i] = g[i][k] = l.Step(i == k ? 1.0 : 0.0, 0.0);
        for (uint32_t i = 0; i < kStates; ++i) m_.ku[k][kStates + i] = l.s[i];
    }
    for (uint32_t w = 0; w < kWidths; ++w) {
        Loop l = proto;
        const uint32_t d = w + kMinRun;
        for (uint32_t i = 0; i < kCell; ++i) rd[w][i] = l.Step(0.0, i < d ? 1.0 : -1.0);
        for (uint32_t i = 0; i < kStates; ++i) m_.c[w][i] = l.s[i];
    }
    for (uint32_t a = 0; a < kStates; ++a) {
        for (uint32_t b = 0; b < kStates; ++b) {
            double q = 0;
            for (uint32_t i = 0; i < kCell; ++i) q += h[i][a] * h[i][b];
            m_.qk[b][a] = q;
        }
        for (uint32_t k = 0; k < kCell; ++k) {
            double v = 0;
            for (uint32_t i = 0; i < kCell; ++i) v += h[i][a] * g[i][k];
            m_.ku[k][a] = 2.0 * v;
        }
        for (uint32_t w = 0; w < kWidths; ++w) {
            double v = 0;
            for (uint32_t i = 0; i < kCell; ++i) v += h[i][a] * rd[w][i];
            m_.p[a][w] = 2.0 * v;
        }
    }
    for (uint32_t w = 0; w < kLanes; ++w) {
        if (w >= kWidths) {
            m_.r[w] = kInf;  // never selected
            continue;
        }
        double r = 0;
        for (uint32_t i = 0; i < kCell; ++i) r += rd[w][i] * rd[w][i];
        m_.r[w] = r;
        for (uint32_t k = 0; k < kCell; ++k) {
            double v = 0;
            for (uint32_t i = 0; i < kCell; ++i) v += g[i][k] * rd[w][i];
            m_.tu[k][w] = 2.0 * v;
        }
    }
    // The loop filter output stays within about +-2.1 at the highest levels encoded (0.65 of full scale, DC).
    limit_ = 32.0;
    Reset();
    resets_ = 0;
}

void FixedEdgeModulator::Reset() {
    ResetPaths(IdleHistory());
    pos_ = 0;
    // One idle cell ahead of the commits: the paths start with kDepth idle cells, so the first real cell is
    // committed after kDepth - 1 of them and comes out kLatency bits late.
    out_ = CellBits(kIdleWidth);
    pending_ = kCell;
    peakV_ = 0;
}

void FixedEdgeModulator::ResetPaths(uint64_t history) {
    Paths& t = paths_[cur_];
    t = {};
    for (uint32_t p = 0; p < kPaths; ++p) {
        t.cost[p] = p == 0 ? 0.0 : kDead;
        t.history[p] = history;
    }
}

template <bool Avx2>
void FixedEdgeModulator::RunCell() {
    const double* u = cellIn_;
    const Paths& now = paths_[cur_];
    Paths& next = paths_[cur_ ^ 1];
    peakV_ = std::max(peakV_, std::abs(u[0] + now.s[0][0] + now.s[0][1] + now.s[0][2] + now.s[0][3]));

    // Shared by all paths: 2K u and Bu u, and per width 2 T_d u + |R_d|^2 + |G u|^2 + 1: every cost is then the
    // energy of w plus one (relative path costs are never negative), positive, so the AVX2 search can carry the
    // candidate in its low mantissa bits.
    alignas(64) double ku[2 * kStates], t[kLanes];
    if constexpr (Avx2) {
        __m256d k[4], tv[6], g[6];
        for (auto& v : k) v = _mm256_setzero_pd();
        for (auto& v : g) v = _mm256_setzero_pd();
        for (uint32_t v = 0; v < 6; ++v) tv[v] = _mm256_load_pd(m_.r + 4 * v);
        for (uint32_t i = 0; i < kCell; ++i) {
            const __m256d ui = _mm256_set1_pd(u[i]);
            for (uint32_t v = 0; v < 4; ++v) k[v] = _mm256_fmadd_pd(ui, _mm256_load_pd(m_.ku[i] + 4 * v), k[v]);
            for (uint32_t v = 0; v < 6; ++v) {
                tv[v] = _mm256_fmadd_pd(ui, _mm256_load_pd(m_.tu[i] + 4 * v), tv[v]);
                g[v] = _mm256_fmadd_pd(ui, _mm256_load_pd(m_.gu[i] + 4 * v), g[v]);
            }
        }
        __m256d gg = _mm256_mul_pd(g[0], g[0]);
        for (uint32_t v = 1; v < 6; ++v) gg = _mm256_fmadd_pd(g[v], g[v], gg);
        const __m256d offset = _mm256_set1_pd(Sum4(gg) + 1.0);
        for (uint32_t v = 0; v < 4; ++v) _mm256_store_pd(ku + 4 * v, k[v]);
        for (uint32_t v = 0; v < 6; ++v) _mm256_store_pd(t + 4 * v, _mm256_add_pd(tv[v], offset));
    } else {
        double g[kLanes]{};
        std::fill(std::begin(ku), std::end(ku), 0.0);
        std::copy(std::begin(m_.r), std::end(m_.r), t);
        for (uint32_t i = 0; i < kCell; ++i) {
            for (uint32_t j = 0; j < 2 * kStates; ++j) ku[j] += u[i] * m_.ku[i][j];
            for (uint32_t d = 0; d < kLanes; ++d) t[d] += u[i] * m_.tu[i][d];
            for (uint32_t j = 0; j < kLanes; ++j) g[j] += u[i] * m_.gu[i][j];
        }
        double offset = 1.0;
        for (const double v : g) offset += v * v;
        for (double& v : t) v += offset;
    }

    // Every live path extended by every width; the kPaths cheapest kept (the best path comes first, so the
    // threshold drops early).
    Selection sel;
    std::fill(std::begin(sel.cost), std::end(sel.cost), kInf);
    std::fill(std::begin(sel.key), std::end(sel.key), 0u);
    // AVX2: the kPaths cheapest so far sorted ascending in 4 vectors, the key in the low 9 mantissa bits (distinct
    // values in the order of the costs). Inserting x: A[i] becomes min(A[i], max(A[i - 1], x)), which leaves A as it
    // is for x at or above its last element; no branches.
    static_assert(kPaths == 16 && kPaths * kLanes <= 512);
    [[maybe_unused]] __m256d top[4];
    if constexpr (Avx2)
        for (auto& v : top) v = _mm256_set1_pd(kInf);
    for (uint32_t p = 0; p < kPaths; ++p) {
        if (!(now.cost[p] < kAlive)) continue;
        const double* s = now.s[p];
        alignas(64) double cost[kLanes];
        if constexpr (Avx2) {
            __m256d z0 = _mm256_load_pd(ku), z1 = _mm256_load_pd(ku + 4), c[6];
            for (uint32_t v = 0; v < 6; ++v) c[v] = _mm256_load_pd(t + 4 * v);
            for (uint32_t j = 0; j < kStates; ++j) {
                const __m256d sj = _mm256_set1_pd(s[j]);
                z0 = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.qk[j]), z0);
                z1 = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.qk[j] + 4), z1);
                for (uint32_t v = 0; v < 6; ++v) c[v] = _mm256_fmadd_pd(sj, _mm256_load_pd(m_.p[j] + 4 * v), c[v]);
            }
            const __m256d sz = _mm256_fmadd_pd(_mm256_load_pd(s), z0, _mm256_mul_pd(_mm256_load_pd(s + 4), z1));
            const __m256d base = _mm256_set1_pd(now.cost[p] + Sum4(sz));
            const __m256d threshold = _mm256_permute4x64_pd(top[3], 0xFF);
            const __m256i keyMask = _mm256_set1_epi64x(~int64_t{511});
            uint32_t below = 0;
            for (uint32_t v = 0; v < 6; ++v) {
                const __m256i key =
                    _mm256_add_epi64(_mm256_set1_epi64x(int64_t(p * kLanes + 4 * v)), _mm256_setr_epi64x(0, 1, 2, 3));
                // Padding lanes (+inf) turn into NaN: never below the threshold.
                const __m256d packed = _mm256_castsi256_pd(
                    _mm256_or_si256(_mm256_and_si256(_mm256_castpd_si256(_mm256_add_pd(c[v], base)), keyMask), key));
                _mm256_store_pd(cost + 4 * v, packed);
                below |= uint32_t(_mm256_movemask_pd(_mm256_cmp_pd(packed, threshold, _CMP_LT_OQ))) << (4 * v);
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

    if constexpr (Avx2) {
        for (uint32_t k = 0; k < 4; ++k) _mm256_storeu_pd(sel.cost + 4 * k, top[k]);
        for (uint32_t r = 0; r < kPaths; ++r) sel.key[r] = uint32_t(std::bit_cast<uint64_t>(sel.cost[r]) & 511u);
    }

    uint32_t commit = kIdleWidth;
    if (sel.cost[0] < kAlive) {
        for (uint32_t r = 0; r < kPaths; ++r) {
            const uint32_t p = sel.key[r] / kLanes, w = sel.key[r] % kLanes;
            const double* s = now.s[p];
            double* ns = next.s[r];
            if constexpr (Avx2) {
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
            next.history[r] = ((now.history[p] << kWidthBits) | (w + kMinRun)) & kHistoryMask;
        }
        // Commit the oldest cell of the best path; paths that disagree with it, and run-away ones, are pruned.
        commit = uint32_t(next.history[0] >> kOldestShift) & 31u;
        bool alive = false;
        for (uint32_t r = 0; r < kPaths; ++r) {
            const double* ns = next.s[r];
            const bool ok = (uint32_t(next.history[r] >> kOldestShift) & 31u) == commit && sel.cost[r] < kAlive &&
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
    out_ = (out_ << kCell) | CellBits(commit);
    pending_ += kCell;
}

void FixedEdgeModulator::Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    const bool avx2 = allowAvx2_ && Kernels().level == SimdLevel::Avx2;
    for (uint32_t i = 0; i < n; ++i) {
        cellIn_[pos_] = double(in[size_t(i) * stride]);
        if (++pos_ == kCell) {
            if (avx2) RunCell<true>();
            else RunCell<false>();
            pos_ = 0;
        }
        if ((i & 15) == 15) {
            // Never short: one idle cell ahead plus a cell per kCell samples always covers the samples taken.
            pending_ -= 16;
            words[i >> 4] = static_cast<uint16_t>(out_ >> pending_);
        }
    }
}

}  // namespace dgmod::dsp
