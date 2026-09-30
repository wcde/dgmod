#include "dsp/Oversampler.h"

#include <algorithm>

namespace dgmod::dsp {

namespace {

constexpr uint32_t kFactor = 2;  // stage-1 ratio

// Stage 2: flat over everything stage 1 lets through, full attenuation from its first image at the doubled rate.
KernelInfo PlanInterp(const KernelInfo& filter, uint32_t outRate) {
    const uint32_t mid = filter.outRate;
    return Kernel::PlanInterstage(mid, outRate, filter.stopbandHz, mid - filter.stopbandHz, filter.spec.attenuationDb);
}

}  // namespace

bool Oversampler::UseCascade(uint32_t inRate, uint32_t outRate, FilterSpec spec, KernelInfo& filter) {
    spec = ClampSpec(spec);
    if (spec.response == FilterResponse::Nos || inRate == 0 || outRate < inRate) return false;
    filter = Kernel::Plan(inRate, inRate * kFactor, spec, kFactor);
    // The output must carry everything the filter passes (a slow or Gaussian roll-off at the source rate: no).
    return filter.fixedPhases == kFactor && filter.stopbandHz <= outRate * 0.5;
}

OversamplerPlan Oversampler::Plan(uint32_t inRate, uint32_t outRate, FilterSpec spec) {
    OversamplerPlan p;
    if (UseCascade(inRate, outRate, spec, p.filter)) {
        p.interp = PlanInterp(p.filter, outRate);
    } else {
        p.filter = Kernel::Plan(inRate, outRate, spec);
    }
    return p;
}

std::shared_ptr<const Kernel> Oversampler::FilterKernel(uint32_t inRate, uint32_t outRate, FilterSpec spec) {
    KernelInfo filter;
    return UseCascade(inRate, outRate, spec, filter) ? Kernel::Get(inRate, inRate * kFactor, spec, kFactor)
                                                     : Kernel::Get(inRate, outRate, spec);
}

bool Oversampler::Configure(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t channels, uint32_t maxInputFrames) {
    if (channels == 0 || maxInputFrames == 0) return false;
    channels_ = channels;
    plan_ = {};
    KernelInfo planned;
    cascade_ = UseCascade(inRate, outRate, spec, planned);
    if (!cascade_) {
        auto kernel = Kernel::Get(inRate, outRate, spec);
        if (!kernel || !last_.Configure(kernel, channels, maxInputFrames)) return false;
        plan_.filter = kernel->Info();
        mid_.clear();
        return true;
    }
    auto filter = Kernel::Get(inRate, inRate * kFactor, spec, kFactor);
    if (!filter || !first_.Configure(filter, channels, maxInputFrames)) return false;
    plan_.filter = filter->Info();
    plan_.interp = PlanInterp(plan_.filter, outRate);
    auto interp = Kernel::GetInterstage(plan_.interp.inRate, outRate, plan_.interp.passbandHz, plan_.interp.stopbandHz,
                                        plan_.interp.spec.attenuationDb);
    // One call of stage 1 yields at most two frames per frame of its history.
    const uint32_t midFrames = kFactor * (2 * plan_.filter.taps + 2 * maxInputFrames + 64);
    if (!interp || !last_.Configure(interp, channels, midFrames)) return false;
    plan_.interp = interp->Info();
    mid_.assign(size_t(midFrames) * channels, 0.0f);
    return true;
}

void Oversampler::Reset() {
    if (cascade_) first_.Reset();
    last_.Reset();
}

uint32_t Oversampler::InputFramesFor(uint32_t outFrames) const {
    const uint32_t n = last_.InputFramesFor(outFrames);
    return cascade_ ? first_.InputFramesFor(n) : n;
}

uint32_t Oversampler::OutputFramesFor(uint32_t inFrames) const {
    return last_.OutputFramesFor(cascade_ ? first_.OutputFramesFor(inFrames) : inFrames);
}

VariableResampler::ProcessResult Oversampler::Process(const float* in, uint32_t inFrames, float* out, uint32_t maxOut,
                                                      float gain) {
    if (!cascade_) return last_.Process(in, inFrames, out, maxOut, gain);
    const auto r1 = first_.Process(in, inFrames, mid_.data(), static_cast<uint32_t>(mid_.size() / channels_), gain);
    auto r2 = last_.Process(mid_.data(), r1.produced, out, maxOut);
    r2.dropped += r1.dropped;
    return r2;
}

}  // namespace dgmod::dsp
