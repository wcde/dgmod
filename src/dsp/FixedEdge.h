#pragma once

#include "dsp/DeltaSigma.h"

#include <cstdint>

namespace dgmod::dsp {

// Cell shape of the fixed-edge code at one DSD rate.
struct FixedEdgeShape {
    uint32_t rate;           // DSD bit rate
    uint32_t cell, minRun;   // bits per cell; shortest run of ones or zeros
    double hinf;             // out-of-band NTF gain
};
// Both rates use 24-bit cells with runs of 2 bits or more (a 470.4 kHz grid at DSD256, 940.8 kHz at DSD512): the
// best noise at the same stable range (DSD256 cells of 16 / 20 / 24 / 28 / 32 bits: -120 / -123 / -124 / -123 /
// -117 dB). Longer cells at DSD512 (48 bits, runs of 4: the DSD256 grid in time) reach only -144 dB, and with runs of
// up to 44 equal bits the FiiO K13 R2R stayed silent (likely a detector for stuck DSD data); no run here is longer
// than 22 bits, as at DSD256, where it plays.
// The NTF gain trades the noise floor against the stable range; both keep 2.3 dB of margin above kLevel (stable up to
// 0.65 of full scale; measured with 16 paths: DSD256 1.056 -113 dB / 0.75, 1.07 -124 dB / 0.65, 1.09 -137 dB / 0.5;
// DSD512 1.05 -158 dB / 0.70, 1.06 -164 dB / 0.65, 1.07 -165 dB / 0.55).
inline constexpr FixedEdgeShape kFixedEdgeShapes[] = {{44100u * 256, 24, 2, 1.07}, {44100u * 512, 24, 2, 1.06}};
// The shape for a DSD bit rate, or nullptr: no fixed-edge code at that rate.
const FixedEdgeShape* FixedEdgeShapeFor(uint32_t dsdRate);

// Fixed-edge 1-bit modulator: the stream is cut into cells of `cell` bits, each a run of +1 of width d followed by a
// run of -1, both at least `minRun` bits long. Every rising edge then sits on the cell grid whatever the signal, and
// every cell has exactly one falling edge, which carries the information.
// Why: a 1-bit DAC adds a small error pulse at every edge (E_up at rising edges U, E_down at falling ones D), and the
// count of edges of a delta-sigma stream follows the music, so that error turns into distortion. With the bits q,
// Delta q = 2 (U - D) holds exactly, so D = U - Delta q / 2 and the output P q + E_up U + E_down D becomes
// (P - E_down Delta / 2) q + (E_up + E_down) U: a fixed linear response to q plus a periodic comb at the cell rate
// (the cell rate), whatever E_up and E_down are; no calibration of the DAC. The minimum run keeps two edges out of
// every window of minRun + 1 bits, which extends this to any edge error with that much memory (266 ns at DSD256,
// 133 ns at DSD512).
// The price is the modulator's own noise: about -124 dB in the audio band at DSD256 against far below -160 dB of the
// delta-sigma modulator (at DSD512 both are near -164 dB), so it pays off only where the DAC really decodes the bits
// (a "1-bit" DSD mode), not where it converts DSD to PCM first.
// Search: the noise-shaping loop runs on the real bits at the DSD rate (error-feedback form, as DeltaSigmaModulator);
// per cell every kept path is extended by every width, the kPaths cheapest of those (energy of the loop filter output
// over the cell, accumulated) are kept, and the cell kDepth cells back on the best path is committed. The loop is
// linear over a cell, so its cost and end state per width come from matrices built at configuration.
class FixedEdgeModulator {
public:
    static constexpr uint32_t kMaxCell = 24;
    static constexpr uint32_t kPaths = 16, kDepth = 8;  // kept paths; cells per path before commit
    static constexpr int kOrder = 8;                    // NTF order, band kBandHz
    static constexpr double kBandHz = 20000.0;
    static constexpr double kLevel = 0.5;  // PCM full scale -> 50 % modulation (the SACD reference)

    void Configure(const NtfDesign& ntf, const FixedEdgeShape& shape);
    void Reset();
    // Takes over the state of `other` (configured identically): the stream continues bit-exactly.
    void CopyStateFrom(const FixedEdgeModulator& other);

    // Modulates `n` samples read with stride `stride` (n a multiple of 16) into 16-bit words, MSB first. The output
    // starts with Latency() bits of the idle cell (width cell / 2), cells aligned to the start of the stream.
    void Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words);

    [[nodiscard]] const FixedEdgeShape& Shape() const { return shape_; }
    [[nodiscard]] uint32_t Latency() const { return shape_.cell * kDepth; }  // output delay in bits
    [[nodiscard]] uint64_t Resets() const { return resets_; }
    [[nodiscard]] double PeakQuantizerInput() const { return peakV_; }
    // The search runs on the AVX-512 kernel (CPU with AVX-512F; the same output as the AVX2 one).
    [[nodiscard]] bool UsesAvx512() const;
    void AllowAvx2(bool allow) { allowAvx2_ = allow; }      // for tests (AVX-512 needs AVX2 allowed too)
    void AllowAvx512(bool allow) { allowAvx512_ = allow; }  // for tests

private:
    static constexpr uint32_t kStates = 8;     // s1 of up to 4 sections, then their s2
    static constexpr uint32_t kMaxLanes = 24;  // widths padded to a multiple of 8

    // Per cell, for the state s at its start, the inputs u and the width d (index d - minRun), with the loop filter
    // output over the cell w = H s + G u + R_d (R_d here its response to the width's bits):
    // cost = |w|^2 = s'Qs + 2 s'K u + 2 s'P_d + 2 T_d u + |R_d|^2 + |G u|^2, end state = B s + Bu u + C_d.
    struct Matrices {
        alignas(64) double qk[kStates][kStates];       // column j of Q
        alignas(64) double ku[kMaxCell][2 * kStates];  // per input sample: column of 2K, then of Bu
        alignas(64) double tu[kMaxCell][kMaxLanes];    // per input sample: 2 T_d over d
        alignas(64) double gu[kMaxCell][kMaxCell];     // per input sample: its column of G
        alignas(64) double p[kStates][kMaxLanes];      // 2 P_d over d, per state
        alignas(64) double r[kMaxLanes];               // |R_d|^2 (padding: +inf)
        alignas(64) double b[kStates][kStates];        // column j of B
        alignas(64) double c[kMaxLanes][kStates];      // C_d
    };
    struct Paths {
        alignas(64) double s[kPaths][kStates];
        alignas(64) double cost[kPaths];  // relative to the best path (kept first); huge = pruned
        uint64_t history[kPaths];         // widths of the last kDepth cells, 6 bits each, newest in the low bits
    };
    // The kPaths cheapest extensions of a cell, sorted: cost and key = path * lanes + width index.
    struct Selection {
        alignas(64) double cost[kPaths];
        uint32_t key[kPaths];
        void Insert(double c, uint32_t k);
    };

    enum class Isa { Scalar, Avx2, Avx512 };
    template <uint32_t L, uint32_t R, Isa I>
    void RunCell();
    using CellFn = void (FixedEdgeModulator::*)();
    [[nodiscard]] CellFn Kernel() const;
    void ResetPaths(uint64_t history);
    [[nodiscard]] uint64_t IdleHistory() const;

    FixedEdgeShape shape_{};
    Matrices m_{};
    Paths paths_[2]{};  // current and next
    uint32_t cur_ = 0;
    alignas(64) double cellIn_[kMaxCell]{};
    uint32_t pos_ = 0;   // samples of the current cell received
    uint64_t out_ = 0;   // committed bits not yet written, oldest first, in the low `pending_` bits
    uint32_t pending_ = 0;
    double limit_ = 0;   // |loop filter output| beyond this means the loop ran away
    double idleState_[kStates]{};  // loop state on a steady run of idle cells (every reset starts from it)
    double peakV_ = 0;
    uint64_t resets_ = 0;
    bool allowAvx2_ = true, allowAvx512_ = true;
};

}  // namespace dgmod::dsp
