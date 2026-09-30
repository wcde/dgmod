#pragma once

#include <complex>
#include <cstdint>
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

// 1-bit delta-sigma modulator in error-feedback form: v = u + (NTF - 1) e, y = sign(v), e = y - v. The NTF runs as a
// cascade of second-order sections (numerically robust even with zeros and poles packed next to z = 1), so the
// output bit sequence follows y = u + NTF * e exactly as designed.
// Overload protection: the error fed back is taken against v clipped to +-clip (a level normal operation never
// reaches). While clipped the loop behaves as if its quantizer had more levels, which bounds the state and brings it
// back without a discontinuity: louder passages get briefly more noise instead of the run-away that an overloaded
// high-order loop falls into. Only a loop that still runs away (non-finite or far beyond the clip level) is reset.
class DeltaSigmaModulator {
public:
    void Configure(const NtfDesign& ntf);
    void Reset();

    // Modulates `n` samples read with stride `stride`; returns the bits, 1 = +1, first sample in the MSB of each
    // 16-bit word (DSD/DoP order). `n` must be a multiple of 16.
    void Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words);

    // Two modulators with the same NTF in one interleaved loop (about twice the throughput of two Process calls).
    static void ProcessPair(DeltaSigmaModulator& a, DeltaSigmaModulator& b, const float* inA, const float* inB,
                            uint32_t stride, uint32_t n, uint16_t* wordsA, uint16_t* wordsB);

    [[nodiscard]] uint64_t Resets() const { return resets_; }
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
    std::vector<Section> sections_;
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
    // `frameRate`: DoP frame rate (DSD rate / 16); `bandHz`: highest frequency the input can contain (source Nyquist).
    // Order 7 with |NTF| <= 1.25 out of band: in-band noise far below 24-bit PCM, and the 1-bit loop stays stable up
    // to about 0.75 of DSD full scale (PCM full scale maps to 0.5, the SACD reference level).
    // The noise-shaping band (NoiseBandHz) is the widest up to 50 kHz whose predicted noise stays below -160 dB in the
    // audio band (0-20 kHz) and below -140 dB over the whole band, but never narrower than the source band (at most
    // 22.05 kHz): DSD256 and above keep the rise of the shaped noise away from the audio band and clear hi-res content
    // above 20 kHz of it; DSD64/128 have no margin and stay at 22.05 kHz.
    bool Configure(uint32_t channels, uint32_t frameRate, double bandHz, uint32_t maxFrames, int order = 7,
                   double hinf = 1.25);
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
    void SetClipLevel(double clip) {  // for tests
        for (auto& m : modulators_) m.SetClipLevel(clip);
    }

    static constexpr uint32_t kFactor = 16;

private:
    uint32_t channels_ = 0, frameRate_ = 0, taps_ = 0, maxFrames_ = 0;
    double noiseBand_ = 0;
    NtfDesign ntf_;
    std::vector<double> coeffs_;  // [tap (history order, oldest first)][phase 0..15]
    std::vector<std::vector<float>> history_;    // per channel: taps_ - 1 previous frames + the current block
    std::vector<std::vector<float>> upsampled_;  // per channel: kFactor * frames samples
    std::vector<DeltaSigmaModulator> modulators_;
    std::vector<std::vector<uint16_t>> words_;
};

}  // namespace dgmod::dsp
