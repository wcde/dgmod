#pragma once

#include <vector>

namespace dgmod::dsp {

struct RemezResult {
    std::vector<double> taps;  // symmetric impulse response, DC gain about 1
    double passRipple = 0;     // achieved pass-band deviation (linear)
    double stopRipple = 0;     // achieved stop-band level (linear)
    int iterations = 0;
    bool converged = false;
};

// Linear-phase (type I) low-pass of odd `length` by the Parks-McClellan (Remez exchange) algorithm: equiripple in both
// bands, the stop-band error weighted `stopWeight` times the pass-band error. Band edges in cycles per sample
// (0 < pass < stop < 0.5). Differences of cosines are evaluated as products of sines, which keeps the barycentric
// interpolation accurate to far below the 150 dB stop bands used here even for several thousand taps.
RemezResult RemezLowpass(int length, double pass, double stop, double stopWeight);

}  // namespace dgmod::dsp
