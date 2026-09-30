#include "dsp/Kernel.h"

#include "dsp/Remez.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <map>
#include <mutex>
#include <numbers>
#include <tuple>

namespace dgmod::dsp {

namespace {

constexpr size_t kMaxTableFloats = size_t{4} << 20;  // 16 MiB of coefficients
constexpr uint32_t kMaxInterpolatedPhases = 4096;
constexpr uint32_t kMinInterpolatedPhases = 256;
constexpr uint32_t kMinTaps = 16;
constexpr uint32_t kMaxTaps = 16384;

// Reconstruction of a tabulated prototype between its grid samples: Kaiser-windowed sinc over +-interpHalf grid samples.
// On the fine grids the prototype occupies less than 1/16 cycle per grid sample and its first image starts at 15/16,
// so a short kernel is flat to well below the 150 dB design limit. The grid of a fixed-ratio kernel (two samples per
// input sample) holds up to 0.35 cycle per grid sample and needs the longer one (only for plots: its table phases fall
// on grid samples).
constexpr int kInterpHalf = 8;
constexpr int kInterpHalfCoarse = 24;
constexpr double kInterpAttenuationDb = 170.0;
// Pass-band edge of the Gaussian response: its -0.1 dB point.
constexpr double kGaussianEdgeDb = 0.1;

double Sinc(double x) {
    if (std::abs(x) < 1e-12) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

double KaiserWindow(double x, double beta, double i0Beta) {
    if (std::abs(x) >= 1.0) return std::abs(x) == 1.0 ? 1.0 / i0Beta : 0.0;
    return BesselI0(beta * std::sqrt(1.0 - x * x)) / i0Beta;
}

// In-place radix-2 complex FFT (size is a power of two). Not normalized.
void Fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<std::complex<double>> w(n / 2);
    const double sign = inverse ? 2.0 : -2.0;
    for (size_t k = 0; k < n / 2; ++k) w[k] = std::polar(1.0, sign * std::numbers::pi * double(k) / double(n));
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2, stride = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + half] * w[k * stride];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
        }
    }
}

// Inverse of the complementary error function for 0 < y < 1 (Newton iteration).
double ErfcInv(double y) {
    double x = y < 0.5 ? std::sqrt(-std::log(y)) : 0.0;
    for (int i = 0; i < 100; ++i) {
        const double step = (std::erfc(x) - y) / (-2.0 / std::sqrt(std::numbers::pi) * std::exp(-x * x));
        x -= step;
        if (std::abs(step) < 1e-14) break;
    }
    return x;
}

uint32_t RoundTaps(double n) { return std::clamp((static_cast<uint32_t>(std::ceil(n)) + 7u) & ~7u, kMinTaps, kMaxTaps); }

uint32_t TablePhases(uint32_t taps) {
    uint32_t p = kMaxInterpolatedPhases;
    while (p > kMinInterpolatedPhases && size_t(p + 3) * taps > kMaxTableFloats) p /= 2;
    return p;
}

}  // namespace

double BesselI0(double x) {
    // Power series: sum_k ((x/2)^k / k!)^2, converges quickly for the beta range used here (< 20).
    const double half = x * 0.5;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 500; ++k) {
        const double r = half / k;
        term *= r * r;
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

double KaiserBeta(double a) {
    if (a > 50.0) return 0.1102 * (a - 8.7);
    if (a > 21.0) return 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    return 0.0;
}

KernelInfo Kernel::Plan(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t fixedPhases) {
    spec = ClampSpec(spec);
    // NOS holds each source sample for its period: only meaningful when oversampling. Otherwise the slow roll-off,
    // whose stop band then ends at the output Nyquist (anything beyond would alias); so does the Gaussian one.
    if (spec.response == FilterResponse::Nos && outRate <= inRate) spec = ShapeSpec(spec, spec.phase, false, FilterResponse::Slow);
    if ((spec.response == FilterResponse::Slow || spec.response == FilterResponse::Gaussian) && outRate <= inRate)
        spec.stopband = std::min(spec.stopband, 1.0);
    // Exact phases only for the integer ratio they describe; NOS needs its fine grid.
    if (fixedPhases && (uint64_t{outRate} != uint64_t{inRate} * fixedPhases || spec.response == FilterResponse::Nos))
        fixedPhases = 0;
    if (!fixedPhases) spec.design = FilterDesign::Kaiser;
    KernelInfo k;
    k.inRate = inRate;
    k.outRate = outRate;
    k.spec = spec;
    k.fixedPhases = fixedPhases;
    if (inRate == 0 || outRate == 0) return k;

    const double fmin = std::min(inRate, outRate);
    if (spec.response == FilterResponse::Nos) {
        // The pass/stop edges describe the anti-alias filter at the output Nyquist that band-limits the hold steps.
        k.stopbandHz = outRate * 0.5;
        k.passbandHz = std::max(kNosFilterStart * k.stopbandHz, kMaxPassband * inRate * 0.5);
    } else {
        k.passbandHz = spec.passband * fmin * 0.5;
        k.stopbandHz = spec.stopband * fmin * 0.5;
    }
    k.cutoffHz = 0.5 * (k.passbandHz + k.stopbandHz);
    const double transition = (k.stopbandHz - k.passbandHz) / inRate;  // cycles per input sample
    k.beta = KaiserBeta(spec.attenuationDb);

    uint32_t taps = 0;
    if (spec.response == FilterResponse::Gaussian) {
        // H(f) = erfc((f - fc) / (sqrt(2) sf)) / 2 (an ideal low-pass under a Gaussian window of sigma 1 / (2 pi sf)):
        // -0.1 dB at the pass-band edge, the attenuation at the stop-band edge. The window is cut where it has fallen
        // to the attenuation.
        const double xp = ErfcInv(2.0 * (1.0 - std::pow(10.0, -kGaussianEdgeDb / 20.0)));
        const double xs = ErfcInv(2.0 * std::pow(10.0, -spec.attenuationDb / 20.0));
        const double s2 = (k.stopbandHz - k.passbandHz) / (xp + xs);  // sqrt(2) sf, Hz
        k.cutoffHz = k.passbandHz + xp * s2;
        k.gaussSigma = inRate / (2.0 * std::numbers::pi * (s2 / std::numbers::sqrt2));
        const double reach = k.gaussSigma * std::sqrt(2.0 * spec.attenuationDb / 20.0 * std::numbers::ln10);
        taps = RoundTaps(2.0 * reach);
    } else if (spec.design == FilterDesign::Equiripple) {
        // Parks-McClellan length estimate (Kaiser's formula for unequal ripples) on the grid of the fixed phases.
        const double ds = std::pow(10.0, -spec.attenuationDb / 20.0);
        const double n =
            (-20.0 * std::log10(std::sqrt(kEquirippleRipple * ds)) - 13.0) / (14.6 * transition / fixedPhases) + 1.0;
        taps = RoundTaps(n / fixedPhases * 1.03);
    } else {
        // Kaiser length estimate (in input samples), with a small safety margin, rounded up to a multiple of 8.
        const double n = (spec.attenuationDb - 7.95) / (14.36 * transition) * 1.04 + 1.0;
        // NOS: plus the one input sample of the hold itself.
        taps = RoundTaps(n + (spec.response == FilterResponse::Nos ? 1.0 : 0.0));
    }
    k.protoTaps = taps;
    if (spec.phase == FilterPhase::Intermediate) taps = RoundTaps(taps * kIntermediateLengthFactor);
    k.taps = taps;

    k.tablePhases = fixedPhases ? fixedPhases : TablePhases(taps);
    k.storedPhases = k.tablePhases + 3;
    // Minimum phase: the exact group delay is known once the filter is designed (Create); a few input samples.
    k.latencyInputFrames = spec.phase == FilterPhase::Minimum        ? kMinPhaseLatencyEstimate
                           : spec.phase == FilterPhase::Intermediate ? k.protoTaps * 0.5 * kIntermediateLatencyFraction
                                                                     : taps * 0.5;
    return k;
}

KernelInfo Kernel::PlanInterstage(uint32_t inRate, uint32_t outRate, double passHz, double stopHz, double attenuationDb) {
    KernelInfo k;
    k.inRate = inRate;
    k.outRate = outRate;
    k.spec.attenuationDb = attenuationDb;
    if (inRate == 0 || outRate == 0 || !(stopHz > passHz)) return k;
    k.spec.passband = passHz / (inRate * 0.5);
    k.spec.stopband = stopHz / (inRate * 0.5);
    k.passbandHz = passHz;
    k.stopbandHz = stopHz;
    k.cutoffHz = 0.5 * (passHz + stopHz);
    k.beta = KaiserBeta(attenuationDb);
    k.taps = k.protoTaps = RoundTaps((attenuationDb - 7.95) / (14.36 * (stopHz - passHz) / inRate) * 1.04 + 1.0);
    k.tablePhases = TablePhases(k.taps);
    k.storedPhases = k.tablePhases + 3;
    k.latencyInputFrames = k.taps * 0.5;
    return k;
}

double Kernel::LinearPrototype(double t) const {
    const double half = info_.protoTaps * 0.5;
    if (!linearGrid_.empty()) return GridValue(linearGrid_, t + half);  // tabulated from -half
    const double x = t / half;
    if (std::abs(x) > 1.0) return 0.0;
    const double w = info_.gaussSigma > 0 ? std::exp(-0.5 * (t / info_.gaussSigma) * (t / info_.gaussSigma))
                                          : BesselI0(info_.beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / BesselI0(info_.beta);
    const double fc2 = 2.0 * cutoffNorm_;
    return fc2 * Sinc(fc2 * t) * w;
}

double Kernel::GridValue(const std::vector<double>& grid, double t) const {
    const double u = t * gridDensity_;
    const auto base = static_cast<int64_t>(std::floor(u));
    const double beta = KaiserBeta(kInterpAttenuationDb), i0 = BesselI0(beta);
    double acc = 0.0;
    for (int64_t k = base - interpHalf_ + 1; k <= base + interpHalf_; ++k) {
        if (k < 0 || k >= static_cast<int64_t>(grid.size())) continue;
        const double x = u - double(k);
        acc += grid[size_t(k)] * Sinc(x) * KaiserWindow(x / interpHalf_, beta, i0);
    }
    return acc * gridDensity_;
}

void Kernel::DesignNos() {
    // One-sample hold (a centred box of width 1) convolved with a Kaiser-windowed sinc low-pass g at the cutoff, so
    // H(f) = sin(pi f) / (pi f) * G(f) with G flat in its pass band: h(t) = integral of g from t - 1/2 to t + 1/2.
    // Tabulated from -half on a grid fine enough for the interpolation of GridValue (the response occupies less
    // than 1/16 cycle per grid sample up to the stop band); like the shaped grid, values are divided by the density.
    const int d = 16 * std::max(1, static_cast<int>(std::ceil(info_.stopbandHz / info_.inRate)));
    gridDensity_ = d;
    interpHalf_ = kInterpHalf;
    const double half = info_.protoTaps * 0.5, reach = half - 0.5;  // g spans +-reach
    const double fc2 = 2.0 * cutoffNorm_, i0 = BesselI0(info_.beta);
    const auto g = [&](double u) { return fc2 * Sinc(fc2 * u) * KaiserWindow(u / reach, info_.beta, i0); };
    // Running integral of g at u_m = m / d - half, 8-point Gauss-Legendre per grid interval (g changes by less than
    // 1/16 cycle over one; the window edges +-reach fall on grid points since d is even).
    static constexpr double kNodes[4] = {0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363};
    static constexpr double kWeights[4] = {0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};
    const size_t n = size_t(info_.protoTaps) * size_t(d);
    const double r = 0.5 / d;
    std::vector<double> running(n + 1, 0.0);
    for (size_t m = 1; m <= n; ++m) {
        const double mid = (double(m) - 0.5) / d - half;
        double acc = 0.0;
        for (int i = 0; i < 4; ++i) acc += kWeights[i] * (g(mid - r * kNodes[i]) + g(mid + r * kNodes[i]));
        running[m] = running[m - 1] + acc * r;
    }
    const size_t off = size_t(d) / 2;  // half an input sample
    linearGrid_.resize(n + 1);
    for (size_t k = 0; k <= n; ++k)
        linearGrid_[k] = (running[std::min(k + off, n)] - (k >= off ? running[k - off] : 0.0)) / d;
    peakTime_ = 0.0;
}

bool Kernel::DesignEquiripple() {
    // Type I Remez design at the rate of the fixed phases, taps * d - 1 long (the grid then runs from -half to +half
    // with zero ends). The design grid resolves the error between its points only approximately: aim 3 dB below the
    // attenuation, and lengthen until the result meets it.
    const int d = gridDensity_;
    const double rate = double(info_.inRate) * d;
    const double pass = info_.passbandHz / rate, stop = info_.stopbandHz / rate;
    const double target = std::pow(10.0, -info_.spec.attenuationDb / 20.0) * 0.7;
    uint32_t taps = info_.protoTaps;
    for (int attempt = 0; attempt < 8; ++attempt, taps = RoundTaps(taps * 1.06)) {
        const RemezResult r = RemezLowpass(static_cast<int>(taps) * d - 1, pass, stop, kEquirippleRipple / target);
        if (!r.converged || r.stopRipple > target * 1.05 || r.passRipple > kEquirippleRipple * 1.05) continue;
        info_.taps = info_.protoTaps = taps;
        linearGrid_.assign(size_t(taps) * d + 1, 0.0);
        for (size_t k = 0; k < r.taps.size(); ++k) linearGrid_[k + 1] = r.taps[k];
        info_.latencyInputFrames = taps * 0.5;
        return true;
    }
    return false;
}

double Kernel::Prototype(double t) const {
    return info_.spec.phase != FilterPhase::Linear ? GridValue(shapedGrid_, t) : LinearPrototype(t);
}

void Kernel::DesignShapedPhase(double fraction) {
    // Homomorphic (real cepstrum) design on the grid of gridDensity_ points per input sample: the linear prototype's
    // magnitude is kept, its phase replaced by `fraction` of the minimum phase (1 = minimum phase, which has the same
    // length as the linear filter; 0.5 = intermediate).
    const int d = gridDensity_;
    const size_t n = size_t(info_.taps) * d;         // result window
    const size_t np = size_t(info_.protoTaps) * d;   // linear prototype
    const size_t size = std::clamp<size_t>(std::bit_ceil(n * 32), size_t{1} << 16, size_t{1} << 22);
    std::vector<std::complex<double>> a(size);
    for (size_t k = 0; k <= np; ++k)
        a[k] = linearGrid_.size() == np + 1 ? linearGrid_[k] : LinearPrototype((double(k) - double(np) * 0.5) / d) / d;
    Fft(a, false);
    double peak = 0.0;
    for (const auto& v : a) peak = std::max(peak, std::abs(v));
    const double floor = peak * 1e-10;  // -200 dB: the stop-band zeros would make the logarithm diverge
    for (auto& v : a) v = std::log(std::max(std::abs(v), floor));
    Fft(a, true);
    // Weight the (even) real cepstrum by 1 + fraction * sign(quefrency): its odd part carries the phase, so this
    // scales the minimum-phase phase response; fraction 1 folds everything onto positive quefrencies.
    const double scale = 1.0 / double(size);
    a[0] *= scale;
    for (size_t k = 1; k < size / 2; ++k) {
        a[k] *= (1.0 + fraction) * scale;
        a[size - k] *= (1.0 - fraction) * scale;
    }
    a[size / 2] *= scale;
    Fft(a, false);
    for (auto& v : a) v = std::exp(v);
    Fft(a, true);
    // Negative times wrap to the end of the buffer. The window starts where the response rises above -150 dB (energy
    // before it is negligible), which keeps the delay as short as the pre-ringing allows.
    const auto at = [&](int64_t m) { return a[size_t((m + int64_t(size)) % int64_t(size))].real() * scale; };
    int64_t start = 0;
    if (fraction < 1.0) {
        const int64_t len = int64_t(n);
        double total = 0.0;
        for (int64_t m = -len; m <= len; ++m) total += at(m) * at(m);
        double before = 0.0;
        start = -len;
        while (start < 0 && before + at(start) * at(start) <= total * 1e-15) before += at(start) * at(start), ++start;
    }
    shapedGrid_.resize(n + 1);
    for (size_t k = 0; k <= n; ++k) shapedGrid_[k] = at(start + int64_t(k));

    // Group delay at DC (first moment) and the main peak.
    double sum = 0.0, moment = 0.0, best = 0.0;
    for (size_t k = 0; k <= n; ++k) {
        sum += shapedGrid_[k];
        moment += shapedGrid_[k] * double(k);
        if (std::abs(shapedGrid_[k]) > best) {
            best = std::abs(shapedGrid_[k]);
            peakTime_ = double(k) / d;
        }
    }
    const double delay = sum != 0.0 ? moment / sum / d : 0.0;
    // The table places the prototype start (t = 0) half - 1 input samples before the linear-phase centre.
    info_.latencyInputFrames = 1.0 + delay;
}

std::shared_ptr<const Kernel> Kernel::Create(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t fixedPhases) {
    return Build(Plan(inRate, outRate, spec, fixedPhases));
}

std::shared_ptr<const Kernel> Kernel::Build(const KernelInfo& info) {
    if (info.taps == 0) return nullptr;
    auto kernel = std::shared_ptr<Kernel>(new Kernel());
    kernel->info_ = info;
    kernel->cutoffNorm_ = info.cutoffHz / info.inRate;
    kernel->gridDensity_ = info.fixedPhases ? static_cast<int>(info.fixedPhases) : kMinPhaseGrid;
    kernel->interpHalf_ = kernel->gridDensity_ >= kMinPhaseGrid ? kInterpHalf : kInterpHalfCoarse;
    if (info.spec.design == FilterDesign::Equiripple && !kernel->DesignEquiripple())
        kernel->info_.spec.design = FilterDesign::Kaiser;  // attenuation out of reach: keep the Kaiser design
    const KernelInfo& k = kernel->info_;
    const bool nos = k.spec.response == FilterResponse::Nos;
    const bool minimum = k.spec.phase != FilterPhase::Linear;  // shaped (minimum or intermediate) phase
    if (minimum) {
        // The intermediate-phase window is twice the (possibly lengthened) prototype.
        if (k.spec.phase == FilterPhase::Intermediate) kernel->info_.taps = RoundTaps(k.protoTaps * kIntermediateLengthFactor);
        else kernel->info_.taps = k.protoTaps;
        kernel->DesignShapedPhase(PhaseFraction(k.spec.phase));
    }
    if (nos) kernel->DesignNos();
    if (!info.fixedPhases) kernel->info_.tablePhases = TablePhases(k.taps);
    kernel->info_.storedPhases = k.tablePhases + 3;
    const int d = kernel->gridDensity_;
    const int ih = kernel->interpHalf_;

    const uint32_t taps = k.taps;
    const double half = taps * 0.5;
    const double i0Beta = BesselI0(k.beta);
    const double fc2 = 2.0 * kernel->cutoffNorm_;
    const double interpBeta = KaiserBeta(kInterpAttenuationDb), interpI0 = BesselI0(interpBeta);
    const std::vector<double>& grid = minimum ? kernel->shapedGrid_ : kernel->linearGrid_;
    const bool gridBased = minimum || !kernel->linearGrid_.empty();
    kernel->coeffs_.Allocate(size_t(k.storedPhases) * taps);

    std::vector<double> row(taps);
    for (uint32_t index = 0; index < k.storedPhases; ++index) {
        const double frac = double(int64_t(index) - 1) / double(k.tablePhases);
        if (gridBased) {
            // Tap j sees the prototype at frac + taps - 2 - j (its start half - 1 samples before the linear-phase
            // centre; linear grids start half samples before it, frac + taps - 1 - j). The fractional grid offset
            // is the same for every tap of a phase (zero for fixed phases: exact grid samples).
            const double u0 = (frac + double(taps) - (minimum ? 2.0 : 1.0)) * d;
            const auto base = static_cast<int64_t>(std::floor(u0));
            const double phi = u0 - double(base);
            double w[2 * kInterpHalfCoarse];
            for (int m = 0; m < 2 * ih; ++m) {
                const double x = phi - double(m - ih + 1);
                w[m] = phi == 0.0 ? (m == ih - 1 ? 1.0 : 0.0) : Sinc(x) * KaiserWindow(x / ih, interpBeta, interpI0);
            }
            for (uint32_t j = 0; j < taps; ++j) {
                const int64_t center = base - int64_t(j) * d;
                double acc = 0.0;
                for (int m = 0; m < 2 * ih; ++m) {
                    const int64_t g = center + m - ih + 1;
                    if (g >= 0 && g < static_cast<int64_t>(grid.size())) acc += grid[size_t(g)] * w[m];
                }
                row[j] = acc;
            }
        } else {
            for (uint32_t j = 0; j < taps; ++j) {
                // Coefficient j multiplies history sample (pos + j); the output instant sits at pos + half - 1 + frac.
                const double t = frac + half - 1.0 - double(j);
                row[j] = k.gaussSigma > 0 ? (std::abs(t) <= half ? fc2 * Sinc(fc2 * t) * std::exp(-0.5 * (t / k.gaussSigma) *
                                                                                                    (t / k.gaussSigma))
                                                                  : 0.0)
                                          : fc2 * Sinc(fc2 * t) * KaiserWindow(t / half, k.beta, i0Beta);
            }
        }
        double sum = 0.0;
        for (uint32_t j = 0; j < taps; ++j) sum += row[j];
        // Unity DC gain per phase: no phase-dependent ripple at DC.
        const double norm = sum != 0.0 ? 1.0 / sum : 1.0;
        float* dst = kernel->coeffs_.data() + size_t(index) * taps;
        for (uint32_t j = 0; j < taps; ++j) dst[j] = static_cast<float>(row[j] * norm);
    }
    return kernel;
}

std::shared_ptr<const Kernel> Kernel::Get(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t fixedPhases) {
    spec = ClampSpec(spec);
    using Key = std::tuple<uint32_t, uint32_t, int64_t, int64_t, int64_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<const Kernel>> cache;

    const Key key{inRate,
                  outRate,
                  std::llround(spec.attenuationDb * 1000.0),
                  std::llround(spec.passband * 1e6),
                  std::llround(spec.stopband * 1e6),
                  static_cast<uint32_t>(spec.phase),
                  static_cast<uint32_t>(spec.response),
                  static_cast<uint32_t>(spec.design),
                  fixedPhases};
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end())
            if (auto live = it->second.lock()) return live;
    }
    auto kernel = Create(inRate, outRate, spec, fixedPhases);
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](const auto& e) { return e.second.expired(); });
    if (auto it = cache.find(key); it != cache.end())
        if (auto live = it->second.lock()) return live;  // built concurrently by another thread
    cache[key] = kernel;
    return kernel;
}

std::shared_ptr<const Kernel> Kernel::GetInterstage(uint32_t inRate, uint32_t outRate, double passHz, double stopHz,
                                                    double attenuationDb) {
    using Key = std::tuple<uint32_t, uint32_t, int64_t, int64_t, int64_t>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<const Kernel>> cache;
    const Key key{inRate, outRate, std::llround(passHz * 1000.0), std::llround(stopHz * 1000.0),
                  std::llround(attenuationDb * 1000.0)};
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end())
            if (auto live = it->second.lock()) return live;
    }
    auto kernel = Build(PlanInterstage(inRate, outRate, passHz, stopHz, attenuationDb));
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](const auto& e) { return e.second.expired(); });
    if (auto it = cache.find(key); it != cache.end())
        if (auto live = it->second.lock()) return live;
    cache[key] = kernel;
    return kernel;
}

std::vector<double> Kernel::MagnitudeResponseDb(const std::vector<double>& freqsHz) const {
    // Sample the continuous prototype on a fine grid (at least 8 points per input sample) and evaluate its
    // symmetric Fourier sum. The grid Nyquist lies above the stop band (NOS: the output Nyquist), so the grid
    // aliasing is negligible. The minimum-phase filter has the same magnitude by construction, so the linear
    // prototype serves both.
    const int kOversample = std::max(8, 2 * static_cast<int>(std::ceil(info_.stopbandHz / info_.inRate)) + 2);
    const double step = 1.0 / kOversample;
    const int count = static_cast<int>(std::ceil(info_.protoTaps * 0.5 * kOversample));
    std::vector<double> h(static_cast<size_t>(count) + 1);
    for (int i = 0; i <= count; ++i) h[size_t(i)] = LinearPrototype(i * step);

    auto response = [&](double f) {
        const double w = 2.0 * std::numbers::pi * f / info_.inRate * step;
        double acc = h[0];
        for (int i = 1; i <= count; ++i) acc += 2.0 * h[size_t(i)] * std::cos(w * i);
        return acc * step;
    };
    const double dc = response(0.0);
    std::vector<double> out;
    out.reserve(freqsHz.size());
    for (const double f : freqsHz) {
        const double m = std::abs(response(f) / dc);
        out.push_back(20.0 * std::log10(std::max(m, 1e-12)));
    }
    return out;
}

}  // namespace dgmod::dsp
