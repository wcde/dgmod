#include "dsp/DeltaSigma.h"

#include "dsp/Dot.h"
#include "dsp/Kernel.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <numbers>

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

double Sinc(double x) {
    if (std::abs(x) < 1e-12) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

}  // namespace

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

void DeltaSigmaModulator::Configure(const NtfDesign& ntf) {
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
    Reset();
    resets_ = clips_ = 0;
}

void DeltaSigmaModulator::Reset() {
    for (auto& s : sections_) s.s1 = s.s2 = 0.0;
    peakV_ = 0;
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

void DeltaSigmaModulator::Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words) {
    DeltaSigmaModulator* mods[] = {this};
    Dispatch<1>(mods, &in, stride, n, &words);
}

void DeltaSigmaModulator::ProcessPair(DeltaSigmaModulator& a, DeltaSigmaModulator& b, const float* inA, const float* inB,
                                      uint32_t stride, uint32_t n, uint16_t* wordsA, uint16_t* wordsB) {
    DeltaSigmaModulator* mods[] = {&a, &b};
    const float* in[] = {inA, inB};
    uint16_t* words[] = {wordsA, wordsB};
    Dispatch<2>(mods, in, stride, n, words);
}

bool DsdEncoder::Configure(uint32_t channels, uint32_t frameRate, double bandHz, uint32_t maxFrames, int order, double hinf) {
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
    for (auto& m : modulators_) m.Configure(ntf_);
    history_.assign(channels, std::vector<float>(size_t(taps_) - 1 + maxFrames, 0.0f));
    upsampled_.assign(channels, std::vector<float>(size_t(kFactor) * maxFrames, 0.0f));
    words_.assign(channels, std::vector<uint16_t>(maxFrames, 0));
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

void DsdEncoder::Process(const float* in, uint32_t frames, uint16_t* out) {
    while (frames > 0) {
        const uint32_t n = std::min(frames, maxFrames_);
        for (uint32_t c = 0; c < channels_; ++c) {
            float* h = history_[c].data();
            float* cur = h + taps_ - 1;
            for (uint32_t i = 0; i < n; ++i) cur[i] = in[size_t(i) * channels_ + c];
            // All 16 phases at once: each history sample is broadcast against one row of 16 coefficients.
            float* up = upsampled_[c].data();
            const auto interpolate = Kernels().level == SimdLevel::Avx2 ? Interpolate16Avx2 : Interpolate16Scalar;
            for (uint32_t i = 0; i < n; ++i) interpolate(h + i, coeffs_.data(), taps_, up + size_t(i) * kFactor);
            std::copy(h + n, h + n + taps_ - 1, h);
        }
        uint32_t c = 0;
        for (; c + 1 < channels_; c += 2)
            DeltaSigmaModulator::ProcessPair(modulators_[c], modulators_[c + 1], upsampled_[c].data(),
                                             upsampled_[c + 1].data(), 1, n * kFactor, words_[c].data(), words_[c + 1].data());
        if (c < channels_) modulators_[c].Process(upsampled_[c].data(), 1, n * kFactor, words_[c].data());
        for (uint32_t ch = 0; ch < channels_; ++ch)
            for (uint32_t i = 0; i < n; ++i) out[size_t(i) * channels_ + ch] = words_[ch][i];
        in += size_t(n) * channels_;
        out += size_t(n) * channels_;
        frames -= n;
    }
}

}  // namespace dgmod::dsp
