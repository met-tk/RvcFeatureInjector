#include "../src/dsp/RvcFeatureInjector.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace rvc_vst;
namespace {
int synthesisCalls = 0;
bool reverseSecondSpan = true;
double secondSpanPhase = 0;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void feed(RvcFeatureInjector& dsp, int64_t start, int count, int block) {
    std::vector<float> input(block, 0.2f), output(block);
    for (int n = 0; n < count; n += block)
        dsp.processBlock(input.data(), nullptr, output.data(), nullptr, std::min(block, count-n), 1, start+n, true);
}
double rms(const std::vector<float>& audio, size_t first, size_t count) {
    double energy = 0;
    for (size_t i = first; i < first + count; ++i) energy += audio[i] * audio[i];
    return std::sqrt(energy/count);
}
}

namespace rvc_vst {
struct RvcModelEngine::Impl {};
RvcModelEngine::RvcModelEngine() = default;
RvcModelEngine::~RvcModelEngine() = default;
void RvcModelEngine::cancelPendingWork() noexcept {}
void RvcModelEngine::resumePendingWork() noexcept {}
bool RvcModelEngine::syncDaemonSlotsIfNeeded() { return true; }
RvcModelSlotInfo RvcModelEngine::getSlotInfo(int) const { RvcModelSlotInfo info; info.isLoaded = true; return info; }
std::string RvcModelEngine::getLoadingStatusText() const { return {}; }
bool RvcModelEngine::synthesizeRvcChunk(const float*, size_t count, const RvcChunkSynthRequest& request,
    RvcChunkSynthResult& result, const std::function<void(float,const std::string&)>&) {
    // Two valid network results can have opposite polarity. Their overlap still carries voice.
    const bool subsequent = ++synthesisCalls > 1;
    const float sign = (subsequent && reverseSecondSpan) ? -1.0f : 1.0f;
    const size_t n = static_cast<size_t>(std::llround(count * request.targetSampleRate / 16000.0));
    result.slotAAudioSamples.resize(n);
    for (size_t i = 0; i < n; ++i)
        result.slotAAudioSamples[i] = sign * 0.3f * std::cos(2 * 3.141592653589793 * 220 * i / request.targetSampleRate
            + (subsequent ? secondSpanPhase : 0));
    result.slotBAudioSamples = result.slotAAudioSamples;
    return true;
}
}

int main() {
    try {
        for (int sr : {44100, 48000, 96000}) {
            for (int phaseCase : {0, 1, 2, 3}) {
                const bool reversed = phaseCase == 0;
                reverseSecondSpan = reversed;
                secondSpanPhase = phaseCase == 2 ? 3.141592653589793/2 : (phaseCase == 3 ? 3.131592653589793 : 0);
                synthesisCalls = 0;
                RvcModelEngine engine; RvcFeatureInjector dsp(engine); dsp.prepare(sr, 128);
                auto params = dsp.getParameters(); params.rmsMixRate = 0; dsp.updateParameters(params);
                const int64_t origin = 17LL * sr + 137;
                dsp.setTimelineState(TimelineState::kCapture);
                feed(dsp, origin, 11 * sr, 128);
                dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
                feed(dsp, origin + 11 * sr, 3 * sr + 79, 128);
                dsp.setTimelineState(TimelineState::kIdle); dsp.triggerGenerate();
                require(dsp.waitForSynthesisIdle(10000), "seam synthesis did not complete");
                require(synthesisCalls == 2, "test did not exercise two synthesis spans");
                dsp.reset();
                std::vector<float> input(128, 0), output(14 * sr + 79);
                for (int n = 0; n < static_cast<int>(output.size()); n += 128)
                    dsp.processBlock(input.data(), nullptr, output.data()+n, nullptr,
                        std::min(128, static_cast<int>(output.size())-n), 1, origin+n, true);
                const double baseline = rms(output, static_cast<size_t>(11.8*sr), sr/10);
                double quietest = baseline;
                for (int n = static_cast<int>(11.94*sr); n < 12*sr; n += sr/1000)
                    quietest = std::min(quietest, rms(output, n, sr/250));
                std::cout << "seam sr=" << sr << " phase_case=" << phaseCase
                          << " minimum/baseline=" << quietest/baseline << std::endl;
                require(quietest > baseline*0.65, "crossfade cancelled valid voice before chunk boundary");
            }
        }
        std::cout << "PASS appended neural spans preserve voice through the overlap\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << std::endl; return 1; }
}
