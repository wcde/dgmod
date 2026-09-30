#include "dsp/Remez.h"

#include "dsp/Kernel.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numbers>
#include <numeric>

namespace dgmod::dsp {

namespace {

// Frequency grid point: angle w (radians per sample) kept as sin(w/2) and cos(w/2), so that
// cos(a) - cos(b) = -2 sin((a+b)/2) sin((a-b)/2) is formed without the cancellation of subtracting two cosines.
struct Node {
    double s = 0, c = 0;
};

Node MakeNode(double w) { return {std::sin(0.5 * w), std::cos(0.5 * w)}; }

double Diff(const Node& a, const Node& b) {
    return -2.0 * (a.s * b.c + a.c * b.s) * (a.s * b.c - a.c * b.s);
}

// Barycentric weights 1 / prod_{j != k} (x_k - x_j) of `nodes`, as mantissa and binary exponent (the products span
// far more than the double range for thousands of nodes).
void Weights(const std::vector<Node>& nodes, std::vector<double>& mant, std::vector<int>& expo) {
    const size_t n = nodes.size();
    mant.assign(n, 0.0);
    expo.assign(n, 0);
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), size_t{0});
    std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t k) {
        double p = 1.0;
        int e = 0;
        for (size_t j = 0; j < n; ++j) {
            if (j == k) continue;
            p *= Diff(nodes[k], nodes[j]);
            if ((j & 15) == 15) {
                int ex = 0;
                p = std::frexp(p, &ex);
                e += ex;
            }
        }
        int ex = 0;
        p = std::frexp(p, &ex);
        e += ex;
        mant[k] = 1.0 / p;
        expo[k] = -e;
    });
}

// Common scale for weights given as mantissa/exponent pairs (only their ratios matter).
std::vector<double> Scale(const std::vector<double>& mant, const std::vector<int>& expo) {
    const int top = *std::max_element(expo.begin(), expo.end());
    std::vector<double> w(mant.size());
    for (size_t k = 0; k < w.size(); ++k) w[k] = std::ldexp(mant[k], expo[k] - top);
    return w;
}

}  // namespace

RemezResult RemezLowpass(int length, double pass, double stop, double stopWeight) {
    RemezResult res;
    if (length < 5 || (length & 1) == 0 || !(pass > 0 && pass < stop && stop < 0.5)) return res;
    const int m = (length - 1) / 2;
    const int r = m + 1;  // cosine terms; r + 1 extremal frequencies

    // Dense grid, uniform in each band, proportional to the band widths.
    constexpr int density = 16;
    const double total = pass + (0.5 - stop);
    const int nPass = std::max(8, static_cast<int>(std::lround(double(density) * r * pass / total)));
    const int nStop = std::max(8, static_cast<int>(std::lround(double(density) * r * (0.5 - stop) / total)));
    const int g = nPass + nStop;
    std::vector<Node> grid(static_cast<size_t>(g));
    std::vector<double> want(static_cast<size_t>(g)), weight(static_cast<size_t>(g));
    for (int i = 0; i < nPass; ++i) {
        grid[size_t(i)] = MakeNode(2.0 * std::numbers::pi * pass * i / (nPass - 1));
        want[size_t(i)] = 1.0;
        weight[size_t(i)] = 1.0;
    }
    for (int i = 0; i < nStop; ++i) {
        grid[size_t(nPass + i)] = MakeNode(2.0 * std::numbers::pi * (stop + (0.5 - stop) * i / (nStop - 1)));
        want[size_t(nPass + i)] = 0.0;
        weight[size_t(nPass + i)] = stopWeight;
    }

    std::vector<double> err(static_cast<size_t>(g)), interpW, value;
    std::vector<size_t> gridIdx(static_cast<size_t>(g));
    std::iota(gridIdx.begin(), gridIdx.end(), size_t{0});

    // Alternating local extrema of the signed error `err` of at least `floor`, band edges included, reduced to r + 1.
    // The previous extremals always compete (in exact arithmetic their error is exactly |delta|; rounding must not
    // make one drop out and break the alternation).
    std::vector<char> previous(static_cast<size_t>(g), 0);
    auto extrema = [&](double floor) {
        std::vector<int> alt;
        auto consider = [&](int i, int lo, int hi) {
            const double e = err[size_t(i)];
            if (!previous[size_t(i)]) {
                if (std::abs(e) < floor) return;
                const bool geL = i == lo || (e > 0 ? e >= err[size_t(i - 1)] : e <= err[size_t(i - 1)]);
                const bool geR = i == hi || (e > 0 ? e > err[size_t(i + 1)] : e < err[size_t(i + 1)]);
                if (!geL || !geR) return;
            }
            if (!alt.empty() && (err[size_t(alt.back())] > 0) == (e > 0)) {
                if (std::abs(e) > std::abs(err[size_t(alt.back())])) alt.back() = i;
            } else {
                alt.push_back(i);
            }
        };
        for (int i = 0; i < nPass; ++i) consider(i, 0, nPass - 1);
        for (int i = nPass; i < g; ++i) consider(i, nPass, g - 1);
        while (alt.size() > size_t(r) + 1) {
            if (alt.size() == size_t(r) + 2) {
                if (std::abs(err[size_t(alt.front())]) < std::abs(err[size_t(alt.back())])) alt.erase(alt.begin());
                else alt.pop_back();
                continue;
            }
            size_t worst = 0;
            for (size_t k = 1; k < alt.size(); ++k)
                if (std::abs(err[size_t(alt[k])]) < std::abs(err[size_t(alt[worst])])) worst = k;
            alt.erase(alt.begin() + static_cast<std::ptrdiff_t>(worst));
            // The neighbours of the removed one now share a sign: keep the larger.
            if (worst > 0 && worst < alt.size()) {
                const size_t a = worst - 1, b = worst;
                alt.erase(alt.begin() + static_cast<std::ptrdiff_t>(
                                            std::abs(err[size_t(alt[a])]) < std::abs(err[size_t(alt[b])]) ? a : b));
            }
        }
        return alt;
    };

    // Initial extremals: the ripple peaks of a Kaiser-windowed design of the same length (close to the optimum set;
    // evenly spaced ones make the first exchanges ill-conditioned for long filters).
    std::vector<int> ext;
    {
        const double fc = 0.5 * (pass + stop), beta = KaiserBeta(20.0 * std::log10(stopWeight) + 100.0);
        const double i0 = BesselI0(beta);
        std::vector<double> h(size_t(m) + 1);
        for (int n = 0; n <= m; ++n) {
            const double x = double(n) / (m + 1);
            const double sinc = n == 0 ? 1.0 : std::sin(2.0 * std::numbers::pi * fc * n) / (2.0 * std::numbers::pi * fc * n);
            h[size_t(n)] = 2.0 * fc * sinc * BesselI0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / i0 * (n ? 2.0 : 1.0);
        }
        // A(w) = sum h_n cos(n w) = sum h_n T_n(cos w): Clenshaw recurrence.
        std::for_each(std::execution::par, gridIdx.begin(), gridIdx.end(), [&](size_t i) {
            const double x = 1.0 - 2.0 * grid[i].s * grid[i].s;
            double b1 = 0, b2 = 0;
            for (int n = m; n >= 1; --n) {
                const double b0 = h[size_t(n)] + 2.0 * x * b1 - b2;
                b2 = b1;
                b1 = b0;
            }
            err[i] = weight[i] * (want[i] - (h[0] + x * b1 - b2));
        });
        ext = extrema(0.0);
        if (ext.size() != size_t(r) + 1) {
            ext.resize(size_t(r) + 1);
            for (int k = 0; k <= r; ++k) ext[size_t(k)] = static_cast<int>(std::lround(double(k) * (g - 1) / r));
        }
    }

    std::vector<Node> interpNodes;
    std::vector<int> isExt(size_t(g), -1);
    double delta = 0;
    std::vector<double> mant;
    std::vector<int> expo;

    // A(w) through the barycentric form over the first r extremals.
    auto evaluate = [&](const Node& x, int exact) {
        if (exact >= 0 && exact < r) return value[size_t(exact)];
        double num = 0, den = 0;
        for (int k = 0; k < r; ++k) {
            const double d = Diff(x, interpNodes[size_t(k)]);
            if (d == 0.0) return value[size_t(k)];
            const double t = interpW[size_t(k)] / d;
            num += t * value[size_t(k)];
            den += t;
        }
        return num / den;
    };

    for (int iter = 1; iter <= 80; ++iter) {
        res.iterations = iter;
        std::vector<Node> nodes(size_t(r) + 1);
        for (int k = 0; k <= r; ++k) nodes[size_t(k)] = grid[size_t(ext[size_t(k)])];
        Weights(nodes, mant, expo);
        const std::vector<double> b = Scale(mant, expo);
        double num = 0, den = 0;
        for (int k = 0; k <= r; ++k) {
            const size_t e = size_t(ext[size_t(k)]);
            num += b[size_t(k)] * want[e];
            den += b[size_t(k)] * ((k & 1) ? -1.0 : 1.0) / weight[e];
        }
        delta = num / den;
        // Weights over the first r nodes: b_k * (x_k - x_r).
        interpNodes.assign(nodes.begin(), nodes.end() - 1);
        interpW.resize(size_t(r));
        value.resize(size_t(r));
        for (int k = 0; k < r; ++k) {
            const size_t e = size_t(ext[size_t(k)]);
            interpW[size_t(k)] = b[size_t(k)] * Diff(nodes[size_t(k)], nodes[size_t(r)]);
            value[size_t(k)] = want[e] - ((k & 1) ? -1.0 : 1.0) * delta / weight[e];
        }
        std::fill(isExt.begin(), isExt.end(), -1);
        for (int k = 0; k < r; ++k) isExt[size_t(ext[size_t(k)])] = k;
        std::for_each(std::execution::par, gridIdx.begin(), gridIdx.end(), [&](size_t i) {
            err[i] = weight[i] * (want[i] - evaluate(grid[i], isExt[i]));
        });

        // New extremal set: local extrema of the signed error at least |delta|, alternating.
        std::fill(previous.begin(), previous.end(), char{0});
        for (const int i : ext) previous[size_t(i)] = 1;
        std::vector<int> alt = extrema(std::abs(delta) - std::max(std::abs(delta) * 1e-6, 1e-14));
        if (alt.size() < size_t(r) + 1) break;  // lost alternation (numerical limit): keep the last solution

        double lo = INFINITY, hi = 0;
        for (const int i : alt) {
            lo = std::min(lo, std::abs(err[size_t(i)]));
            hi = std::max(hi, std::abs(err[size_t(i)]));
        }
        const bool same = std::equal(alt.begin(), alt.end(), ext.begin());
        ext = std::move(alt);
        if (same || (hi - lo) <= 1e-4 * hi) {
            res.converged = true;
            break;
        }
    }

    // Achieved deviations on the grid.
    for (int i = 0; i < g; ++i) {
        if (i < nPass) res.passRipple = std::max(res.passRipple, std::abs(err[size_t(i)]));
        else res.stopRipple = std::max(res.stopRipple, std::abs(err[size_t(i)]) / stopWeight);
    }

    // Impulse response from A at w_j = 2 pi j / length (inverse DFT of the zero-phase response).
    std::vector<double> a(size_t(m) + 1);
    for (int j = 0; j <= m; ++j) a[size_t(j)] = evaluate(MakeNode(2.0 * std::numbers::pi * j / length), -1);
    std::vector<double> cosTable(static_cast<size_t>(length));
    for (int t = 0; t < length; ++t) cosTable[size_t(t)] = std::cos(2.0 * std::numbers::pi * t / length);
    res.taps.assign(size_t(length), 0.0);
    std::vector<size_t> tapIdx(size_t(m) + 1);
    std::iota(tapIdx.begin(), tapIdx.end(), size_t{0});
    std::for_each(std::execution::par, tapIdx.begin(), tapIdx.end(), [&](size_t n) {
        double acc = a[0];
        size_t t = 0;
        for (int j = 1; j <= m; ++j) {
            t += n;
            if (t >= size_t(length)) t -= size_t(length);
            acc += 2.0 * a[size_t(j)] * cosTable[t];
        }
        acc /= length;
        res.taps[size_t(m) + n] = acc;
        res.taps[size_t(m) - n] = acc;
    });
    return res;
}

}  // namespace dgmod::dsp
