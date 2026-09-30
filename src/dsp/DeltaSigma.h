#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dgmod::dsp {

// Noise transfer function of a lowpass delta-sigma modulator: zeros spread optimally over the signal band (Gauss-
// Legendre positions), poles placed (maximally flat) so that the out-of-band gain |NTF(-1)| equals `hinf` (Lee's
// criterion; lower is more stable). Port of synthesizeNTF (Schreier's Delta-Sigma Toolbox) for f0 = 0.
struct NtfDesign {
    int order = 0;
    double osr = 0;   // oversampling ratio relative to the signal band edge: fs / (2 * band)
    double hinf = 0;
    std::vector<std::complex<double>> zeros, poles;

    // |NTF(e^{jw})| at the normalized frequency f (cycles per sample).
    [[nodiscard]] double Magnitude(double f) const;
};
NtfDesign SynthesizeNtf(int order, double osr, double hinf);

// Quantization noise the modulator puts into [f0, f1] Hz at the modulator rate `fs` (linear model, uniform error of
// variance 1/3), in dB relative to a full-scale PCM sine (amplitude 0.5 of the DSD range).
double PredictedNoiseDb(const NtfDesign& ntf, double fs, double f0, double f1);

// Look-ahead of the modulator (pruned tree search): Off decides every bit on its own (sign of the quantizer input);
// the others keep the best `paths` bit sequences and commit a bit `depth` samples later, choosing the sequence whose
// noise-shaping filter output has the least energy. It extends the stable input range (DSD256: about 0.8 of full
// scale with Standard, 0.85 with High, against 0.75-0.8 without), at a CPU cost of roughly 0.3 (Standard) and 0.55
// (High) of a core per channel on a current desktop CPU.
enum class LookAhead : uint32_t { Off = 0, Standard = 1, High = 2 };
inline constexpr uint32_t kLookAheadLevels = 3;
// Highest DSD rate with look-ahead (DSD256): above it one channel alone would take most of a core.
inline constexpr uint32_t kLookAheadMaxRate = 44100u * 256;
const wchar_t* LookAheadName(LookAhead level);
struct LookAheadShape {
    uint32_t paths = 0, depth = 0;  // 0 = off
    friend bool operator==(const LookAheadShape&, const LookAheadShape&) = default;
};
// Paths and depth of a level at the DSD bit rate `dsdRate`: 4 (Standard) or 8 (High) paths at DSD256, twice as many
// per halving of the rate (up to kMaxPaths), depth kMaxDepth; off above kLookAheadMaxRate.
LookAheadShape LookAheadFor(LookAhead level, uint32_t dsdRate);

// 1-bit delta-sigma modulator in error-feedback form: v = u + (NTF - 1) e, y = sign(v), e = y - v. The NTF runs as a
// cascade of second-order sections (numerically robust even with zeros and poles packed next to z = 1), so the
// output bit sequence follows y = u + NTF * e exactly as designed.
// Overload protection: the error fed back is taken against v clipped to +-clip (a level normal operation never
// reaches). While clipped the loop behaves as if its quantizer had more levels, which bounds the state and brings it
// back without a discontinuity: louder passages get briefly more noise instead of the run-away that an overloaded
// high-order loop falls into. Only a loop that still runs away (non-finite or far beyond the clip level) is reset.
class DeltaSigmaModulator {
public:
    static constexpr uint32_t kMaxPaths = 32;
    static constexpr uint32_t kMaxDepth = 64;

    // `shape`: look-ahead paths (power of two, 4..kMaxPaths) and depth (multiple of 8, 8..kMaxDepth); 0 paths = off.
    // With look-ahead the output is `depth` bits late.
    void Configure(const NtfDesign& ntf, LookAheadShape shape = {});
    void Reset();

    // Modulates `n` samples read with stride `stride`; returns the bits, 1 = +1, first sample in the MSB of each
    // 16-bit word (DSD/DoP order). `n` must be a multiple of 16.
    void Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words);

    // Two modulators with the same NTF in one interleaved loop (about twice the throughput of two Process calls).
    static void ProcessPair(DeltaSigmaModulator& a, DeltaSigmaModulator& b, const float* inA, const float* inB,
                            uint32_t stride, uint32_t n, uint16_t* wordsA, uint16_t* wordsB);

    [[nodiscard]] uint64_t Resets() const { return resets_; }
    [[nodiscard]] LookAheadShape Shape() const { return {paths_, depth_}; }
    // Samples whose fed-back error was clipped (overload protection engaged).
    [[nodiscard]] uint64_t Clips() const { return clips_; }
    [[nodiscard]] double PeakQuantizerInput() const { return peakV_; }
    [[nodiscard]] double ClipLevel() const { return clip_; }
    void SetClipLevel(double clip) { clip_ = clip; }  // for tests

private:
    struct Section {
        double b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double s1 = 0, s2 = 0;
    };
    template <size_t N, size_t L>
    static void RunLanes(DeltaSigmaModulator* const* mods, const float* const* in, uint32_t stride, uint32_t n,
                         uint16_t* const* words);
    template <size_t L>
    static void Dispatch(DeltaSigmaModulator* const* mods, const float* const* in, uint32_t stride, uint32_t n,
                         uint16_t* const* words);
    template <size_t N, size_t M>
    void RunLookAhead(const float* in, uint32_t stride, uint32_t n, uint16_t* words);
    template <size_t N, size_t M>
    void RunLookAheadAvx2(const float* in, uint32_t stride, uint32_t n, uint16_t* words);
    template <size_t N>
    void DispatchLookAhead(const float* in, uint32_t stride, uint32_t n, uint16_t* words);
    void ResetPaths(uint64_t history);

    // Look-ahead paths (structure of arrays over the paths): section states without the latest fed-back error x (the
    // loop adds x * (q1, q2) when it advances the path), accumulated cost relative to the best path (huge = pruned),
    // bit history (newest bit in bit 0).
    struct Paths {
        alignas(64) double s1[4][kMaxPaths];
        alignas(64) double s2[4][kMaxPaths];
        alignas(64) double x[kMaxPaths];
        alignas(64) double cost[kMaxPaths];
        alignas(64) uint64_t history[kMaxPaths];
    };

    std::vector<Section> sections_;
    uint32_t paths_ = 0, depth_ = 0;
    double q1_[4]{}, q2_[4]{};  // state change per unit of fed-back error (the loop is affine in it)
    Paths trellis_{};
    double clip_ = 0;   // |v| clip level of the fed-back error
    double limit_ = 0;  // |v| above this means the loop ran away despite the clipping
    double peakV_ = 0;
    uint64_t resets_ = 0, clips_ = 0;
};

// DSD encoder: interpolates PCM at the DoP frame rate by 16 to the DSD rate (short linear-phase polyphase filter; the
// input carries nothing above the source Nyquist) and modulates each channel to one DSD bit per sample, producing one
// 16-bit DSD word per channel and DoP frame.
class DsdEncoder {
public:
    DsdEncoder();
    ~DsdEncoder();
    DsdEncoder(const DsdEncoder&) = delete;
    DsdEncoder& operator=(const DsdEncoder&) = delete;

    // Runs the body of the look-ahead helper thread (for example inside an MMCSS scope); set before Configure. By
    // default the body runs as is (denormals are flushed either way).
    using ThreadWrapper = std::function<void(const std::function<void()>& body)>;
    void SetThreadWrapper(ThreadWrapper wrapper) { wrapper_ = std::move(wrapper); }

    // `frameRate`: DoP frame rate (DSD rate / 16); `bandHz`: highest frequency the input can contain (source Nyquist).
    // Order 7 with |NTF| <= 1.25 out of band: in-band noise far below 24-bit PCM, and the 1-bit loop stays stable up
    // to about 0.75 of DSD full scale (PCM full scale maps to 0.5, the SACD reference level).
    // The noise-shaping band (NoiseBandHz) is the widest up to 50 kHz whose predicted noise stays below -160 dB in the
    // audio band (0-20 kHz) and below -140 dB over the whole band, but never narrower than the source band (at most
    // 22.05 kHz): DSD256 and above keep the rise of the shaped noise away from the audio band and clear hi-res content
    // above 20 kHz of it; DSD64/128 have no margin and stay at 22.05 kHz.
    // With look-ahead the channels are split between the calling thread and a helper thread (see SetThreadWrapper).
    bool Configure(uint32_t channels, uint32_t frameRate, double bandHz, uint32_t maxFrames,
                   LookAhead lookAhead = LookAhead::Off, int order = 7, double hinf = 1.25);
    void Reset();
    // Takes over the filter history and modulator states of `other` (configured identically) without allocating, so
    // a spare encoder can render an alternative continuation of the same stream.
    void CopyStateFrom(const DsdEncoder& other);

    // `in`: interleaved PCM frames (|x| up to about 0.6 of DSD full scale), `out`: interleaved words.
    void Process(const float* in, uint32_t frames, uint16_t* out);

    [[nodiscard]] uint32_t Taps() const { return taps_; }
    [[nodiscard]] uint32_t DsdRate() const { return frameRate_ * 16; }
    [[nodiscard]] uint64_t Resets() const;
    [[nodiscard]] uint64_t Clips() const;
    [[nodiscard]] double PeakQuantizerInput() const;
    [[nodiscard]] const NtfDesign& Ntf() const { return ntf_; }
    [[nodiscard]] double NoiseBandHz() const { return noiseBand_; }
    [[nodiscard]] LookAheadShape LookAheadUsed() const { return shape_; }
    void SetClipLevel(double clip) {  // for tests
        for (auto& m : modulators_) m.SetClipLevel(clip);
    }

    static constexpr uint32_t kFactor = 16;

private:
    struct Helper;
    void Interpolate(uint32_t c, const float* in, uint32_t n);
    void EncodeChannels(uint32_t first, uint32_t last, const float* in, uint32_t n);

    uint32_t channels_ = 0, frameRate_ = 0, taps_ = 0, maxFrames_ = 0;
    LookAheadShape shape_;
    ThreadWrapper wrapper_;
    std::unique_ptr<Helper> helper_;
    double noiseBand_ = 0;
    NtfDesign ntf_;
    std::vector<double> coeffs_;  // [tap (history order, oldest first)][phase 0..15]
    std::vector<std::vector<float>> history_;    // per channel: taps_ - 1 previous frames + the current block
    std::vector<std::vector<float>> upsampled_;  // per channel: kFactor * frames samples
    std::vector<DeltaSigmaModulator> modulators_;
    std::vector<std::vector<uint16_t>> words_;
};

}  // namespace dgmod::dsp
