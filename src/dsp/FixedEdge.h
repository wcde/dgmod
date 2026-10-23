#pragma once

#include "dsp/DeltaSigma.h"

#include <cstdint>

namespace dgmod::dsp {

// Fixed-edge 1-bit modulator for DSD256: the stream is cut into cells of kCell bits, each a run of +1 of width d
// followed by a run of -1, both at least kMinRun bits long. Every rising edge then sits on the cell grid whatever the
// signal, and every cell has exactly one falling edge, which carries the information.
// Why: a 1-bit DAC adds a small error pulse at every edge (E_up at rising edges U, E_down at falling ones D), and the
// count of edges of a delta-sigma stream follows the music, so that error turns into distortion. With the bits q,
// Delta q = 2 (U - D) holds exactly, so D = U - Delta q / 2 and the output P q + E_up U + E_down D becomes
// (P - E_down Delta / 2) q + (E_up + E_down) U: a fixed linear response to q plus a periodic comb at the cell rate
// (470.4 kHz), whatever E_up and E_down are; no calibration of the DAC. The minimum run keeps two edges out of every
// three-bit window, which extends this to any edge error with up to three bits of memory.
// The price is the modulator's own noise: about -124 dB in the audio band against far below -160 dB of the
// delta-sigma modulator, so it pays off only where the DAC really decodes the bits (a "1-bit" DSD mode), not where it
// converts DSD to PCM first.
// Search: the noise-shaping loop runs on the real bits at the DSD rate (error-feedback form, as DeltaSigmaModulator);
// per cell every kept path is extended by all kWidths widths, the kPaths cheapest of those (energy of the loop filter
// output over the cell, accumulated) are kept, and the cell kDepth cells back on the best path is committed. The loop
// is linear over a cell, so its cost and end state per width come from matrices built at configuration.
class FixedEdgeModulator {
public:
    static constexpr uint32_t kCell = 24, kMinRun = 2;
    static constexpr uint32_t kWidths = kCell - 2 * kMinRun + 1;  // d = kMinRun .. kCell - kMinRun
    static constexpr uint32_t kPaths = 16, kDepth = 8;              // kept paths; cells per path before commit
    static constexpr uint32_t kLatency = kCell * kDepth;            // output delay in bits
    // NTF: order 8, band 20 kHz, |NTF| <= 1.07 out of band. The out-of-band gain trades the noise floor against the
    // stable range (measured with 16 paths: 1.056 gives -113 dB and holds 0.75 of full scale, 1.07 -124 dB and 0.65,
    // 1.09 -137 dB and only 0.5): 1.07 keeps 2.3 dB of margin above the level the bridge sends (kLevel).
    static constexpr int kOrder = 8;
    static constexpr double kHinf = 1.07, kBandHz = 20000.0;
    static constexpr double kLevel = 0.5;            // PCM full scale -> 50 % modulation (the SACD reference)
    static constexpr uint32_t kRate = 44100u * 256;  // DSD256 only: the cell rate is then 470.4 kHz

    void Configure(const NtfDesign& ntf);
    void Reset();

    // Modulates `n` samples read with stride `stride` (n a multiple of 16) into 16-bit words, MSB first. The output
    // starts with kLatency bits of the idle cell (width kCell / 2), cells aligned to the start of the stream.
    void Process(const float* in, uint32_t stride, uint32_t n, uint16_t* words);

    [[nodiscard]] uint64_t Resets() const { return resets_; }
    [[nodiscard]] double PeakQuantizerInput() const { return peakV_; }
    void AllowAvx2(bool allow) { allowAvx2_ = allow; }  // for tests

private:
    static constexpr uint32_t kStates = 8;  // s1 of up to 4 sections, then their s2
    static constexpr uint32_t kLanes = 24;  // widths padded to a multiple of 4

    // Per cell, for the state s at its start, the inputs u and the width d (index d - kMinRun), with the loop filter
    // output over the cell w = H s + G u + R_d (R_d here its response to the width's bits):
    // cost = |w|^2 = s'Qs + 2 s'K u + 2 s'P_d + 2 T_d u + |R_d|^2 + |G u|^2, end state = B s + Bu u + C_d.
    struct Matrices {
        alignas(64) double qk[kStates][kStates];    // column j of Q
        alignas(64) double ku[kCell][2 * kStates];  // per input sample: column of 2K, then of Bu
        alignas(64) double tu[kCell][kLanes];       // per input sample: 2 T_d over d
        alignas(64) double gu[kCell][kLanes];       // per input sample: its column of G (padding: 0)
        alignas(64) double p[kStates][kLanes];      // 2 P_d over d, per state
        alignas(64) double r[kLanes];               // |R_d|^2 (padding: +inf)
        alignas(64) double b[kStates][kStates];     // column j of B
        alignas(64) double c[kLanes][kStates];      // C_d
    };
    struct Paths {
        alignas(64) double s[kPaths][kStates];
        alignas(64) double cost[kPaths];  // relative to the best path (kept first); huge = pruned
        uint64_t history[kPaths];         // widths of the last kDepth cells, 5 bits each, newest in the low bits
    };
    // The kPaths cheapest extensions of a cell, sorted: cost and key = path * kLanes + width index.
    struct Selection {
        double cost[kPaths];
        uint32_t key[kPaths];
        void Insert(double c, uint32_t k);
    };

    template <bool Avx2>
    void RunCell();
    void ResetPaths(uint64_t history);

    Matrices m_{};
    Paths paths_[2]{};  // current and next
    uint32_t cur_ = 0;
    alignas(64) double cellIn_[kCell]{};
    uint32_t pos_ = 0;   // samples of the current cell received
    uint64_t out_ = 0;   // committed bits not yet written, oldest first, in the low `pending_` bits
    uint32_t pending_ = 0;
    double limit_ = 0;   // |loop filter output| beyond this means the loop ran away
    double peakV_ = 0;
    uint64_t resets_ = 0;
    bool allowAvx2_ = true;
};

}  // namespace dgmod::dsp
