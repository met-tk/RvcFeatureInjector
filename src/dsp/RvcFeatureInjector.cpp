#include "RvcFeatureInjector.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;

namespace rvc_vst {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEps = 1e-8f;

uint64_t computeBaseParameterHash(
    float slotAIndex,
    float slotBIndex,
    float protect,
    uint64_t modelRevision,
    uint64_t manualCounter) noexcept {

    // Neural retrieval/protection changes are pending until the next explicit Generate.
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };

    auto mixFloat = [&](float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };
    mixFloat(slotAIndex);
    mixFloat(slotBIndex);
    mixFloat(protect);
    mix(modelRevision);
    mix(manualCounter);
    return h ? h : 1ULL;
}

void downsampleTo16kHz(
    const float* src,
    size_t srcLen,
    double srcSr,
    std::vector<float>& dst16k) {

    if (!src || srcLen == 0 || srcSr <= 1000.0) {
        dst16k.clear();
        return;
    }

    const double ratio = srcSr / 16000.0;
    const size_t dstLen = std::max<size_t>(1, static_cast<size_t>(std::llround(static_cast<double>(srcLen) / ratio)));
    dst16k.resize(dstLen);

    if (std::abs(ratio - 1.0) < 1e-4) {
        for (size_t i = 0; i < dstLen; ++i) {
            dst16k[i] = src[std::min(i, srcLen - 1)];
        }
        return;
    }

    // Symmetric zero-phase Blackman-windowed Sinc anti-aliasing low-pass filter (fc = 7600 Hz):
    // Prevents >8 kHz sibilants/harmonics from aliasing into HuBERT's 0-8 kHz phoneme band
    // while maintaining strictly 0.0-sample group delay (zero pronunciation/timing drift).
    const double cutoffNorm = 2.0 * (7600.0 / srcSr); // Normalized to Nyquist = 1.0 (fc / (srcSr / 2))
    const int halfSupport = std::clamp(static_cast<int>(std::ceil(8.0 * ratio)), 4, 48);
    const double invHalfSupport = 1.0 / static_cast<double>(halfSupport);
    const int64_t maxIdx = static_cast<int64_t>(srcLen) - 1;

    for (size_t i = 0; i < dstLen; ++i) {
        const double center = static_cast<double>(i) * ratio;
        const int64_t cInt = static_cast<int64_t>(std::llround(center));
        double acc = 0.0;
        double wSum = 0.0;
        for (int k = -halfSupport; k <= halfSupport; ++k) {
            const int64_t idx = cInt + k;
            const double d = static_cast<double>(idx) - center;
            const double u = d * invHalfSupport;
            if (std::abs(u) >= 1.0) continue;

            // Blackman window symmetric around d = 0
            const double win = 0.42 + 0.50 * std::cos(3.14159265358979323846 * u)
                                    + 0.08 * std::cos(2.0 * 3.14159265358979323846 * u);
            const double x = 3.14159265358979323846 * cutoffNorm * d;
            const double sinc = (std::abs(x) < 1e-8) ? 1.0 : (std::sin(x) / x);
            const double w = win * sinc;
            const int64_t clampedIdx = std::clamp<int64_t>(idx, 0, maxIdx);
            acc += static_cast<double>(src[clampedIdx]) * w;
            wSum += w;
        }
        dst16k[i] = (wSum > 1e-9) ? static_cast<float>(acc / wSum) : src[std::clamp<int64_t>(cInt, 0, maxIdx)];
    }
}

struct SvfCoeffs {
    float g = 0.0f;
    float k = 0.33f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;
};

inline SvfCoeffs makeSvfBandpassCoeffs(float freqHz, float qVal, double sampleRate) noexcept {
    SvfCoeffs c{};
    const float maxFc = static_cast<float>(sampleRate * 0.42);
    const float fc = std::clamp(freqHz, 80.0f, std::max(100.0f, maxFc));
    c.g = std::tan(kPi * fc / static_cast<float>(sampleRate));
    c.k = 1.0f / std::max(0.5f, qVal);
    c.a1 = 1.0f / (1.0f + c.g * (c.g + c.k));
    c.a2 = c.g * c.a1;
    c.a3 = c.g * c.a2;
    return c;
}

inline float processSvfUnityBandpass(
    float x,
    float& ic1eq,
    float& ic2eq,
    const SvfCoeffs& c) noexcept {
    const float v3 = x - ic2eq;
    const float v1 = c.a1 * ic1eq + c.a2 * v3;
    const float v2 = ic2eq + c.a2 * ic1eq + c.a3 * v3;
    ic1eq = 2.0f * v1 - ic1eq;
    ic2eq = 2.0f * v2 - ic2eq;
    return c.k * v1;
}

} // namespace

RvcFeatureInjector::RvcFeatureInjector(RvcModelEngine& modelEngine)
    : modelEngine_(modelEngine) {
    prepare(48000.0, 512);
    modelEngine_.resumePendingWork();
    workerRunning_.store(true, std::memory_order_release);
    workerThread_ = std::thread(&RvcFeatureInjector::inferenceWorkerLoop, this);
}

RvcFeatureInjector::~RvcFeatureInjector() {
    workerRunning_.store(false, std::memory_order_release);
    modelEngine_.cancelPendingWork();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void RvcFeatureInjector::prepare(double sampleRate, int maxBlockSize) {
    const double newSampleRate = (sampleRate > 8000.0) ? sampleRate : 48000.0;

    // Allocate lock-free full-track dual-basis timeline audio cache (240s @ host sample rate, 6.0s segments)
    const size_t newChunkLen = std::max<size_t>(48000, static_cast<size_t>(std::llround(newSampleRate * kChunkSeconds)));
    const size_t newMaxCache = newChunkLen * kMaxChunks;
    if (newSampleRate != sampleRate_ || newMaxCache != maxCacheSamples_ || !dryTimelineCache_ || !wetSlotACache_ || !wetSlotBCache_ || !automationTimeline_) {
        latestStoppedProjectSample_.store(-1, std::memory_order_relaxed);
        captureArmStartFloor_.store(-1, std::memory_order_relaxed);
        captureArmRevision_.store(0, std::memory_order_relaxed);
        capturePassStartFloor_ = -1;
        const bool wasRunning = workerRunning_.exchange(false, std::memory_order_acq_rel);
        if (wasRunning && workerThread_.joinable()) {
            modelEngine_.cancelPendingWork();
            workerThread_.join();
        }

        sampleRate_ = newSampleRate;
        chunkLenSamples_ = newChunkLen;

        maxCacheSamples_ = newMaxCache;
        dryTimelineCache_ = std::make_unique<std::atomic<float>[]>(maxCacheSamples_);
        wetSlotACache_ = std::make_unique<std::atomic<float>[]>(maxCacheSamples_);
        wetSlotBCache_ = std::make_unique<std::atomic<float>[]>(maxCacheSamples_);
        captureValidity_ = std::make_unique<CaptureValidity[]>((maxCacheSamples_ + 63) / 64);
        for (size_t i = 0; i < maxCacheSamples_; ++i) {
            dryTimelineCache_[i].store(0.0f, std::memory_order_relaxed);
            wetSlotACache_[i].store(0.0f, std::memory_order_relaxed);
            wetSlotBCache_[i].store(0.0f, std::memory_order_relaxed);
        }

        automationTimeline_ = std::make_unique<AutomationTimelineFrame[]>(kAutomationCacheFrames);
        clearCapture();

        if (wasRunning) {
            modelEngine_.resumePendingWork();
            workerRunning_.store(true, std::memory_order_release);
            workerThread_ = std::thread(&RvcFeatureInjector::inferenceWorkerLoop, this);
        }
    }

    const size_t automationCapacity = static_cast<size_t>(std::max(16384, maxBlockSize));
    if (automationCapacity != blockAutomationCapacity_) {
        blockAutomation_ = std::make_unique<BlockAutomationFrame[]>(automationCapacity);
        blockAutomationCapacity_ = automationCapacity;
    }

    // Initialize default telemetry envelopes
    for (int b = 0; b < 2; ++b) {
        auto& snap = telemetryBuffers_[b];
        snap.sampleRate = sampleRate_;
        snap.latencySamples = getLatencySamples();
        for (size_t k = 0; k < kScopeBins; ++k) {
            const float fNorm = static_cast<float>(k) / static_cast<float>(kScopeBins - 1);
            snap.sourceEnvelopeDb[k] = -38.0f - 10.0f * fNorm;
            snap.slotAEnvelopeDb[k] = -34.0f - 8.0f * fNorm;
            snap.slotBEnvelopeDb[k] = -36.0f - 9.0f * fNorm;
            snap.injectedOutputDb[k] = -34.0f - 8.0f * fNorm;
        }
        std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText), "IDLE: CLICK [CAPTURE] AND PLAY DAW TO RECORD");
    }

    reset();
}

void RvcFeatureInjector::reset() {
    dryEnvFollower_ = 0.0f;
    wetEnvFollower_ = 0.0f;
    smoothEnvGain_ = 1.0f;
    neuralCacheCrossfade_ = 0.0f;
    lastTimelineSamplePos_ = -1;
    lastTransportWasPlaying_ = false;
    expectedTimelineSamplePos_ = -1;
    lastActiveRelEnd_ = -1;
    transportPlayGate_ = 0.0f;
    dspStateFlushedOnPause_ = true;

    smoothAbMorph_ = paramAbMorph_.load(std::memory_order_relaxed);
    smoothInjectDepth_ = paramInjectDepth_.load(std::memory_order_relaxed);
    smoothPitchSt_ = paramPitchShiftSt_.load(std::memory_order_relaxed);
    smoothThroatSt_ = paramThroatFormantSt_.load(std::memory_order_relaxed);
    smoothPresenceDb_ = paramPresenceAirDb_.load(std::memory_order_relaxed);
    smoothProtect_ = paramConsonantProtect_.load(std::memory_order_relaxed);
    smoothRmsMix_ = paramRmsMixRate_.load(std::memory_order_relaxed);
    smoothDryWet_ = paramDryWetMix_.load(std::memory_order_relaxed);
    smoothSlotAIndex_ = paramSlotAIndexRate_.load(std::memory_order_relaxed);
    smoothSlotBIndex_ = paramSlotBIndexRate_.load(std::memory_order_relaxed);
    smoothOutputGainDb_ = paramOutputGainDb_.load(std::memory_order_relaxed);

    pitchLpBuf_.fill(0.0f);
    pitchWritePos_ = 0;
    pitchTotalSamples_ = 0;
    const float srScale = std::clamp(static_cast<float>(sampleRate_ / 48000.0), 0.5f, 4.0f);
    const float defaultPeriod = 200.0f * srScale;
    pdaLpfState1_ = 0.0f;
    pdaLpfState2_ = 0.0f;
    pdaMedianTable_.fill(defaultPeriod);
    pdaMedianIdx_ = 0;
    trackedPeriodSamples_ = defaultPeriod;
    pitchLpfState_ = defaultPeriod;
    pdaTrackCount_ = 0;
    pdaVoiced_ = false;
    pdaChunkCounter_ = 0;
    goertzelRe_ = 0.0f;
    goertzelIm_ = 0.0f;
    prevGoertzelRe_ = 0.0f;
    epochTable_.fill(0.0);
    epochWriteIdx_ = 0;
    epochCount_ = 0;
    currentEpochLoc_ = 0.0;
    prevEpochLoc_ = -static_cast<double>(defaultPeriod);
    epochSpacing_ = defaultPeriod;
    pitchShiftCrossfade_ = 0.0f;
    smoothPeriodRatio_ = std::pow(2.0f, -std::clamp(smoothPitchSt_, -12.0f, 12.0f) / 12.0f);
    motif1Delay_ = 0.0f;
    motif2Delay_ = 0.0f;
    motif1Age_ = 0.0f;
    motif2Age_ = 0.0f;
    splicePhase_ = 1.0f;
    spliceStep_ = 1.0f / (48.0f * srScale);
    epochTimer_ = 0.0f;
    prevVoicedState_ = false;
    dcBlockX1_ = 0.0f;
    dcBlockY1_ = 0.0f;
    pitchInEnv_ = 0.001f;
    pitchOutEnv_ = 0.001f;
    pitchSilentSamples_ = 1000000;
    pitchTransientSamples_ = 0;

    for (auto& s : svfBase_) s = SvfBandState{};
    for (auto& s : svfShifted_) s = SvfBandState{};
    formantTiltLp_ = 0.0f;
    presenceShelfLp_ = 0.0f;
    presenceShelfLp2_ = 0.0f;
    drySibilanceLp_.fill(0.0f);
    wetSibilanceLp_.fill(0.0f);
    dryHfEnv_ = 0.0f;
    dryLowEnv_ = 0.0f;
}

void RvcFeatureInjector::lockCaptureControl() noexcept {
    while (captureControlBusy_.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
}

void RvcFeatureInjector::configureCaptureArm(bool resetCapture) noexcept {
    // Publish before the Capture command so its very first callback cannot miss the floor.
    const uint64_t control = timelineControl_.load(std::memory_order_acquire);
    const uint32_t generation = static_cast<uint32_t>(control >> 32) + (resetCapture ? 1u : 0u);
    const uint32_t revision = (static_cast<uint32_t>(control) & ~uint32_t{3}) + 4u;
    const uint64_t next = (static_cast<uint64_t>(generation) << 32) | (revision & ~uint32_t{3});
    captureArmStartFloor_.store(latestStoppedProjectSample_.load(std::memory_order_acquire), std::memory_order_relaxed);
    captureArmRevision_.store(next >> 2, std::memory_order_release);
}

void RvcFeatureInjector::setTimelineCommand(TimelineState state, bool invalidate, bool resetCapture) noexcept {
    uint64_t old = timelineControl_.load(std::memory_order_acquire);
    uint64_t next;
    do {
        const uint32_t generation = static_cast<uint32_t>(old >> 32) + (resetCapture ? 1u : 0u);
        const uint32_t revision = (static_cast<uint32_t>(old) & ~uint32_t{3}) + (invalidate ? 4u : 0u);
        next = (static_cast<uint64_t>(generation) << 32) | (revision & ~uint32_t{3}) |
            static_cast<uint64_t>(state);
    } while (!timelineControl_.compare_exchange_weak(old, next, std::memory_order_seq_cst));
}

void RvcFeatureInjector::applyCaptureGeneration(uint32_t generation) noexcept {
    if (captureGeneration_.load(std::memory_order_relaxed) == generation) return;
    // UI commands invalidate logically; only this audio-thread owner resets capture metadata.
    captureGeneration_.store(generation, std::memory_order_release);
    if (captureBoundsGeneration_.load(std::memory_order_acquire) != generation) {
        captureStartSample_.store(-1, std::memory_order_relaxed);
        captureEndSample_.store(-1, std::memory_order_relaxed);
    }
    timelineOriginSample_.store(0, std::memory_order_relaxed);
    internalTimelineCursor_.store(0, std::memory_order_relaxed);
    latestPlayheadRelSample_.store(0, std::memory_order_relaxed);
    for (auto& chunk : chunkStates_) {
        chunk.capturedCount.store(0, std::memory_order_relaxed);
        chunk.maxAbsPeak.store(0.0f, std::memory_order_relaxed);
    }
}

void RvcFeatureInjector::triggerResynthesize() noexcept {
    lockCaptureControl();
    const auto state = getTimelineState();
    for (auto& chunk : chunkStates_) chunk.wetReady.store(false, std::memory_order_release);
    manualResynthCounter_.fetch_add(1, std::memory_order_release);
    setTimelineCommand(state == TimelineState::kActive ? TimelineState::kGenerating : state, true);
    captureControlBusy_.clear(std::memory_order_release);
}

void RvcFeatureInjector::clearTimelineCacheLocked() noexcept {
    for (auto& chunk : chunkStates_) {
        chunk.synthesizedGen.store(0, std::memory_order_release);
        chunk.wetReady.store(false, std::memory_order_release);
    }
    currentCachedChunks_.store(0, std::memory_order_release);
    currentTotalActiveChunks_.store(0, std::memory_order_release);
    currentCacheCoverage_.store(0.0f, std::memory_order_release);
    currentSynthProgress_.store(1.0f, std::memory_order_release);
    pendingRegenerateNotice_.store(false, std::memory_order_release);
    appendCapture_.store(false, std::memory_order_release);
    appendCacheReusable_.store(false, std::memory_order_release);
    appendReusedCache_.store(false, std::memory_order_release);
    appendComparedSamples_.store(0, std::memory_order_release);
    appendChangedSamples_.store(0, std::memory_order_release);
    captureCapacityExceeded_.store(false, std::memory_order_release);
}

void RvcFeatureInjector::clearTimelineCache() noexcept {
    lockCaptureControl();
    setTimelineCommand(TimelineState::kIdle, true, true);
    clearTimelineCacheLocked();
    captureControlBusy_.clear(std::memory_order_release);
}

bool RvcFeatureInjector::hasCapturedSample(size_t index, uint32_t generation, int64_t startSample) const noexcept {
    if (index >= maxCacheSamples_ || !captureValidity_ || startSample < 0) return false;
    // Absolute project addressing makes prepending O(1), without moving the 240s allocation.
    const size_t physical = (static_cast<size_t>(startSample % static_cast<int64_t>(maxCacheSamples_)) + index) % maxCacheSamples_;
    const auto& valid = captureValidity_[physical / 64];
    return valid.generation.load(std::memory_order_acquire) == generation &&
        (valid.mask.load(std::memory_order_acquire) & (uint64_t{1} << (physical % 64))) != 0;
}

float RvcFeatureInjector::readCapturedSample(size_t index, uint32_t generation, int64_t startSample) const noexcept {
    if (!hasCapturedSample(index, generation, startSample)) return 0.0f;
    const size_t physical = (static_cast<size_t>(startSample % static_cast<int64_t>(maxCacheSamples_)) + index) % maxCacheSamples_;
    return dryTimelineCache_[physical].load(std::memory_order_relaxed);
}

int64_t RvcFeatureInjector::automationFrameAt(int64_t sample) const noexcept {
    const int64_t sr = static_cast<int64_t>(std::llround(sampleRate_));
    return (sample / sr) * kAutoFrameHz + ((sample % sr) * kAutoFrameHz) / sr;
}

bool RvcFeatureInjector::waitForSynthesisIdle(uint32_t timeoutMs) {
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    while (true) {
        const uint32_t active = currentTotalActiveChunks_.load(std::memory_order_acquire);
        const uint32_t cached = currentCachedChunks_.load(std::memory_order_acquire);
        const bool busy = currentlySynthesizing_.load(std::memory_order_acquire) ||
                          modelEngine_.isDaemonBusy();
        if (getTimelineState() == TimelineState::kActive && active > 0 && cached >= active && !busy) {
            return true;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= static_cast<int64_t>(timeoutMs)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void RvcFeatureInjector::updateParameters(const RvcVoiceParameters& params) noexcept {
    paramAbMorph_.store(std::clamp(params.abMorph, 0.0f, 1.0f), std::memory_order_relaxed);
    paramInjectDepth_.store(std::clamp(params.injectDepth, 0.0f, 1.0f), std::memory_order_relaxed);
    paramPitchShiftSt_.store(std::clamp(params.pitchShiftSemitones, -12.0f, 12.0f), std::memory_order_relaxed);
    paramThroatFormantSt_.store(std::clamp(params.throatFormantSemitones, -6.0f, 6.0f), std::memory_order_relaxed);
    paramPresenceAirDb_.store(std::clamp(params.presenceAirDb, -6.0f, 6.0f), std::memory_order_relaxed);
    paramConsonantProtect_.store(std::clamp(params.consonantProtect, 0.0f, 0.50f), std::memory_order_relaxed);
    paramRmsMixRate_.store(std::clamp(params.rmsMixRate, 0.0f, 1.0f), std::memory_order_relaxed);
    paramDryWetMix_.store(std::clamp(params.dryWetMix, 0.0f, 1.0f), std::memory_order_relaxed);
    paramSlotAIndexRate_.store(std::clamp(params.slotAIndexRate, 0.0f, 1.0f), std::memory_order_relaxed);
    paramSlotBIndexRate_.store(std::clamp(params.slotBIndexRate, 0.0f, 1.0f), std::memory_order_relaxed);
    paramOutputGainDb_.store(std::clamp(params.outputGainDb, -12.0f, 12.0f), std::memory_order_relaxed);
    paramBypass_.store(params.bypass, std::memory_order_relaxed);
}

RvcVoiceParameters RvcFeatureInjector::getParameters() const noexcept {
    RvcVoiceParameters p;
    p.abMorph = paramAbMorph_.load(std::memory_order_relaxed);
    p.injectDepth = paramInjectDepth_.load(std::memory_order_relaxed);
    p.pitchShiftSemitones = paramPitchShiftSt_.load(std::memory_order_relaxed);
    p.throatFormantSemitones = paramThroatFormantSt_.load(std::memory_order_relaxed);
    p.presenceAirDb = paramPresenceAirDb_.load(std::memory_order_relaxed);
    p.consonantProtect = paramConsonantProtect_.load(std::memory_order_relaxed);
    p.rmsMixRate = paramRmsMixRate_.load(std::memory_order_relaxed);
    p.dryWetMix = paramDryWetMix_.load(std::memory_order_relaxed);
    p.slotAIndexRate = paramSlotAIndexRate_.load(std::memory_order_relaxed);
    p.slotBIndexRate = paramSlotBIndexRate_.load(std::memory_order_relaxed);
    p.outputGainDb = paramOutputGainDb_.load(std::memory_order_relaxed);
    p.bypass = paramBypass_.load(std::memory_order_relaxed);
    return p;
}

void RvcFeatureInjector::pushAutomationPoint(uint32_t paramId, int32_t sampleOffset, float plainValue) noexcept {
    if (paramId >= 12 || !std::isfinite(plainValue) || !blockAutomation_) return;
    const size_t offset = std::min(static_cast<size_t>(std::max(0, sampleOffset)), blockAutomationCapacity_ - 1);
    auto& frame = blockAutomation_[offset];
    frame.values[paramId] = plainValue;
    frame.mask |= static_cast<uint16_t>(1u << paramId);
}

void RvcFeatureInjector::applyAutomationPoint(uint32_t paramId, float plainValue) noexcept {
    switch (paramId) {
        case 0: paramAbMorph_.store(std::clamp(plainValue, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 1:
        case 7:
            paramInjectDepth_.store(std::clamp(plainValue, 0.0f, 1.0f), std::memory_order_relaxed);
            paramDryWetMix_.store(std::clamp(plainValue, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 2: paramPitchShiftSt_.store(std::clamp(plainValue, -12.0f, 12.0f), std::memory_order_relaxed); break;
        case 3: paramThroatFormantSt_.store(std::clamp(plainValue, -6.0f, 6.0f), std::memory_order_relaxed); break;
        case 4: paramPresenceAirDb_.store(std::clamp(plainValue, -6.0f, 6.0f), std::memory_order_relaxed); break;
        case 5: paramConsonantProtect_.store(std::clamp(plainValue, 0.0f, 0.50f), std::memory_order_relaxed); break;
        case 6: paramRmsMixRate_.store(std::clamp(plainValue, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 8: paramSlotAIndexRate_.store(std::clamp(plainValue, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 9: paramSlotBIndexRate_.store(std::clamp(plainValue, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 10: paramOutputGainDb_.store(std::clamp(plainValue, -12.0f, 12.0f), std::memory_order_relaxed); break;
        case 11: paramBypass_.store(plainValue >= 0.5f, std::memory_order_relaxed); break;
        default: break;
    }
}

void RvcFeatureInjector::setHostTrackContext(const std::wstring& trackName, int32_t channelIndex) {
    std::lock_guard<std::mutex> lock(hostContextMutex_);
    hostTrackName_ = trackName;
    hostChannelIndex_ = channelIndex;
}

std::wstring RvcFeatureInjector::getHostTrackName() const {
    std::lock_guard<std::mutex> lock(hostContextMutex_);
    return hostTrackName_;
}

void RvcFeatureInjector::setTimelineState(TimelineState state) noexcept {
    lockCaptureControl();
    if (state == TimelineState::kActive || state == TimelineState::kGenerating) {
        if (capturedAudioGeneration_.load(std::memory_order_acquire) !=
            static_cast<uint32_t>(timelineControl_.load(std::memory_order_acquire) >> 32)) state = TimelineState::kIdle;
    }
    if (state == TimelineState::kCapture) {
        clearTimelineCacheLocked();
        currentSynthProgress_.store(0.0f, std::memory_order_release);
    }
    if (state == TimelineState::kActive) {
        const uint64_t revision = timelineControl_.load(std::memory_order_acquire) >> 2;
        int64_t start, end;
        getCaptureRange(start, end);
        const size_t count = start >= 0 && end > start ? std::min<size_t>(kMaxChunks,
            (static_cast<size_t>(end - start) + chunkLenSamples_ - 1) / chunkLenSamples_) : 0;
        for (size_t c = 0; c < count; ++c) {
            if (!chunkStates_[c].wetReady.load(std::memory_order_acquire) ||
                chunkStates_[c].wetGeneration.load(std::memory_order_acquire) != revision) state = TimelineState::kIdle;
        }
    }
    if (state == TimelineState::kCapture) configureCaptureArm(true);
    setTimelineCommand(state, state == TimelineState::kCapture || state == TimelineState::kGenerating,
        state == TimelineState::kCapture);
    if (state != TimelineState::kCapture) isCapturingNow_.store(false, std::memory_order_release);
    captureControlBusy_.clear(std::memory_order_release);
}

void RvcFeatureInjector::beginAppendCapture() noexcept {
    lockCaptureControl();
    const bool pendingUnchanged = appendCacheReusable_.load(std::memory_order_acquire) &&
        appendChangedSamples_.load(std::memory_order_acquire) == 0 && !appendAutomationChanged_.load();
    if (!pendingUnchanged) {
        getCaptureRange(appendBaseStart_, appendBaseEnd_);
        appendBaseRevision_ = timelineControl_.load(std::memory_order_acquire) >> 2;
        appendBaseParameterHash_ = computeBaseParameterHash(paramSlotAIndexRate_.load(), paramSlotBIndexRate_.load(),
            paramConsonantProtect_.load(), modelEngine_.getModelRevision(), manualResynthCounter_.load());
    }
    bool reusable = (getTimelineState() == TimelineState::kActive || getTimelineState() == TimelineState::kIdle) &&
        appendBaseStart_ >= 0 && appendBaseEnd_ > appendBaseStart_;
    const size_t count = reusable ? std::min<size_t>(kMaxChunks,
        (static_cast<size_t>(appendBaseEnd_ - appendBaseStart_) + chunkLenSamples_ - 1) / chunkLenSamples_) : 0;
    for (size_t c = 0; c < count; ++c) {
        reusable = reusable && chunkStates_[c].wetReady.load(std::memory_order_acquire) &&
            chunkStates_[c].wetGeneration.load(std::memory_order_acquire) == appendBaseRevision_ &&
            chunkStates_[c].synthesizedGen.load(std::memory_order_acquire) == appendBaseParameterHash_;
    }
    appendCacheReusable_.store(reusable, std::memory_order_release);
    appendAutomationChanged_.store(false, std::memory_order_release);
    appendComparedSamples_.store(0, std::memory_order_release);
    appendChangedSamples_.store(0, std::memory_order_release);
    appendReusedCache_.store(false, std::memory_order_release);
    appendCapture_.store(true, std::memory_order_release);
    captureCapacityExceeded_.store(false, std::memory_order_release);
    isCapturingNow_.store(false, std::memory_order_release);
    // Keep the last successful samples until Generate knows whether the recording changed.
    currentCachedChunks_.store(0, std::memory_order_release);
    currentCacheCoverage_.store(0.0f, std::memory_order_release);
    currentSynthProgress_.store(0.0f, std::memory_order_release);
    configureCaptureArm(false);
    setTimelineCommand(TimelineState::kCapture, true);
    captureControlBusy_.clear(std::memory_order_release);
}

TimelineState RvcFeatureInjector::getTimelineState() const noexcept {
    return static_cast<TimelineState>(timelineControl_.load(std::memory_order_acquire) & 3);
}

void RvcFeatureInjector::getCaptureRange(int64_t& startSample, int64_t& endSample) const noexcept {
    const uint32_t generation = static_cast<uint32_t>(timelineControl_.load(std::memory_order_acquire) >> 32);
    startSample = endSample = -1;
    if (captureBoundsGeneration_.load(std::memory_order_acquire) != generation) return;
    startSample = captureStartSample_.load(std::memory_order_acquire);
    endSample = captureEndSample_.load(std::memory_order_acquire);
    if (captureBoundsGeneration_.load(std::memory_order_acquire) != generation ||
        static_cast<uint32_t>(timelineControl_.load(std::memory_order_acquire) >> 32) != generation)
        startSample = endSample = -1;
}

void RvcFeatureInjector::setCaptureRange(int64_t startSample, int64_t endSample) noexcept {
    lockCaptureControl();
    setTimelineCommand(getTimelineState(), true, true);
    captureStartSample_.store(startSample, std::memory_order_release);
    captureEndSample_.store(endSample, std::memory_order_release);
    captureBoundsGeneration_.store(static_cast<uint32_t>(timelineControl_.load(std::memory_order_acquire) >> 32),
        std::memory_order_release);
    captureControlBusy_.clear(std::memory_order_release);
}

void RvcFeatureInjector::clearCapture() noexcept {
    lockCaptureControl();
    setTimelineCommand(TimelineState::kIdle, true, true);
    isCapturingNow_.store(false, std::memory_order_release);
    clearTimelineCacheLocked();
    captureControlBusy_.clear(std::memory_order_release);
}

void RvcFeatureInjector::triggerGenerate() noexcept {
    lockCaptureControl();
    if (getTimelineState() == TimelineState::kCapture) {
        // Close recording before examining it; another callback must not extend an inspected take.
        setTimelineCommand(TimelineState::kIdle, false);
        while (audioBlockInFlight_.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    int64_t capStart, capEnd;
    getCaptureRange(capStart, capEnd);
    const bool hasCapturedAudio = capturedAudioGeneration_.load(std::memory_order_acquire) ==
        static_cast<uint32_t>(timelineControl_.load(std::memory_order_acquire) >> 32);
    if (capStart < 0 || capEnd <= capStart || chunkLenSamples_ == 0 || maxCacheSamples_ == 0 || !hasCapturedAudio) {
        captureControlBusy_.clear(std::memory_order_release);
        return;
    }
    const int64_t totalCap = std::min<int64_t>(capEnd - capStart, static_cast<int64_t>(maxCacheSamples_));
    const size_t numChunks = static_cast<size_t>(std::min<int64_t>(
        kMaxChunks,
        (totalCap + static_cast<int64_t>(chunkLenSamples_) - 1) / static_cast<int64_t>(chunkLenSamples_)));
    const uint64_t parameters = computeBaseParameterHash(paramSlotAIndexRate_.load(), paramSlotBIndexRate_.load(),
        paramConsonantProtect_.load(), modelEngine_.getModelRevision(), manualResynthCounter_.load());
    bool reuse = appendCacheReusable_.exchange(false, std::memory_order_acq_rel) &&
        appendChangedSamples_.load(std::memory_order_acquire) == 0 &&
        !appendAutomationChanged_.load(std::memory_order_acquire) &&
        capStart == appendBaseStart_ && capEnd == appendBaseEnd_ && parameters == appendBaseParameterHash_;
    for (size_t c = 0; c < numChunks && reuse; ++c)
        reuse = chunkStates_[c].wetReady.load(std::memory_order_acquire) &&
            chunkStates_[c].wetGeneration.load(std::memory_order_acquire) == appendBaseRevision_;
    if (reuse) {
        const uint64_t control = timelineControl_.load(std::memory_order_acquire);
        const uint32_t revision = (static_cast<uint32_t>(control) & ~uint32_t{3}) + 4u;
        const uint64_t readyControl = (control & 0xffffffff00000000ULL) |
            (revision & ~uint32_t{3}) | static_cast<uint64_t>(TimelineState::kActive);
        for (size_t c = 0; c < numChunks; ++c)
            chunkStates_[c].wetGeneration.store(readyControl >> 2, std::memory_order_release);
        currentTotalActiveChunks_.store(static_cast<uint32_t>(numChunks), std::memory_order_release);
        currentCachedChunks_.store(static_cast<uint32_t>(numChunks), std::memory_order_release);
        currentCacheCoverage_.store(1.0f, std::memory_order_release);
        currentSynthProgress_.store(1.0f, std::memory_order_release);
        pendingRegenerateNotice_.store(false, std::memory_order_release);
        paramBypass_.store(false, std::memory_order_release);
        isCapturingNow_.store(false, std::memory_order_release);
        appendReusedCache_.store(true, std::memory_order_release);
        timelineControl_.store(readyControl, std::memory_order_seq_cst);
        captureControlBusy_.clear(std::memory_order_release);
        return;
    }
    appendReusedCache_.store(false, std::memory_order_release);
    for (size_t c = 0; c < numChunks; ++c) chunkStates_[c].wetReady.store(false, std::memory_order_release);
    currentCachedChunks_.store(0, std::memory_order_release);
    currentTotalActiveChunks_.store(static_cast<uint32_t>(numChunks), std::memory_order_release);
    currentCacheCoverage_.store(0.0f, std::memory_order_release);
    currentSynthProgress_.store(0.0f, std::memory_order_release);
    pendingRegenerateNotice_.store(false, std::memory_order_release);
    paramBypass_.store(false, std::memory_order_release);
    isCapturingNow_.store(false, std::memory_order_release);
    manualResynthCounter_.fetch_add(1, std::memory_order_release);
    setTimelineCommand(TimelineState::kGenerating, true);
    captureControlBusy_.clear(std::memory_order_release);
}

VisualTelemetrySnapshot RvcFeatureInjector::getVisualTelemetry() const noexcept {
    const int idx = activeTelemetryIdx_.load(std::memory_order_acquire) & 1;
    VisualTelemetrySnapshot snap = telemetryBuffers_[idx];

    const uint64_t telemetryControl = timelineControl_.load(std::memory_order_acquire);
    snap.timelineStateVal = static_cast<uint32_t>(telemetryControl & 3);
    getCaptureRange(snap.captureStartSample, snap.captureEndSample);
    snap.isCapturingNow = isCapturingNow_.load(std::memory_order_relaxed);
    snap.isAppendCapture = appendCapture_.load(std::memory_order_relaxed);
    snap.appendComparedSamples = appendComparedSamples_.load(std::memory_order_relaxed);
    snap.appendChangedSamples = appendChangedSamples_.load(std::memory_order_relaxed);
    snap.appendReusedCache = appendReusedCache_.load(std::memory_order_relaxed);
    snap.captureCapacityExceeded = captureCapacityExceeded_.load(std::memory_order_relaxed);
    snap.slotALoaded = slotALoaded_.load(std::memory_order_relaxed);
    snap.slotBLoaded = slotBLoaded_.load(std::memory_order_relaxed);
    const bool isSynth = currentlySynthesizing_.load(std::memory_order_acquire) || modelEngine_.isDaemonBusy();
    snap.isSynthesizing = isSynth;
    snap.isDaemonReady = modelEngine_.isNativeDaemonReady();
    snap.isCacheLocked = true;
    snap.isPendingRegenerate = pendingRegenerateNotice_.load(std::memory_order_relaxed);
    snap.isPlayingFromNeuralCache = playingFromNeuralCache_.load(std::memory_order_relaxed);

    const double sr = (sampleRate_ > 8000.0) ? sampleRate_ : 48000.0;
    const int64_t rawCapSamples = (snap.captureStartSample >= 0 && snap.captureEndSample > snap.captureStartSample)
                                      ? std::min<int64_t>(snap.captureEndSample - snap.captureStartSample,
                                                          static_cast<int64_t>(maxCacheSamples_))
                                      : 0;
    snap.totalActiveChunks = rawCapSamples > 0 && chunkLenSamples_ > 0
        ? static_cast<uint32_t>(std::min<size_t>(kMaxChunks,
            (static_cast<size_t>(rawCapSamples) + chunkLenSamples_ - 1) / chunkLenSamples_)) : 0;
    snap.cachedChunksCount = 0;
    for (size_t c = 0; c < snap.totalActiveChunks; ++c) {
        if (chunkStates_[c].wetReady.load(std::memory_order_acquire) &&
            chunkStates_[c].wetGeneration.load(std::memory_order_acquire) == (telemetryControl >> 2))
            ++snap.cachedChunksCount;
    }
    // A command can arrive while the UI reads; never present its predecessor's completed cache.
    if (timelineControl_.load(std::memory_order_acquire) != telemetryControl) snap.cachedChunksCount = 0;
    snap.cacheCoverageRatio = snap.totalActiveChunks > 0
        ? static_cast<float>(snap.cachedChunksCount) / snap.totalActiveChunks : 0.0f;
    snap.synthesisProgress = isSynth
        ? modelEngine_.getLoadingProgress() * 0.35f + currentSynthProgress_.load(std::memory_order_acquire) * 0.65f
        : snap.cacheCoverageRatio;
    snap.capturedDurationSec = static_cast<float>(static_cast<double>(rawCapSamples) / sr);
    const int64_t relPlayhead = std::max<int64_t>(0, latestPlayheadRelSample_.load(std::memory_order_relaxed));
    snap.playheadRelSec = static_cast<float>(static_cast<double>(relPlayhead) / sr);

    // Downsample captured dry timeline cache into kWaveformBins (256) peak & RMS bins for the Time-Domain Waveform View
    if (rawCapSamples > 0 && dryTimelineCache_) {
        const size_t totalS = static_cast<size_t>(rawCapSamples);
        for (size_t b = 0; b < kWaveformBins; ++b) {
            const size_t b0 = (b * totalS) / kWaveformBins;
            const size_t b1 = std::max<size_t>(b0 + 1, ((b + 1) * totalS) / kWaveformBins);
            const size_t span = b1 - b0;
            const size_t step = std::max<size_t>(1, span / 24);
            float maxP = 0.0f;
            float sumSq = 0.0f;
            size_t cnt = 0;
            for (size_t s = b0; s < b1 && s < maxCacheSamples_; s += step) {
                const float v = std::abs(readCapturedSample(s, captureGeneration_.load(std::memory_order_acquire), snap.captureStartSample));
                if (v > maxP) maxP = v;
                sumSq += v * v;
                ++cnt;
            }
            snap.capturedWaveformPeak[b] = std::min(1.0f, maxP);
            snap.capturedWaveformRms[b] = (cnt > 0) ? std::min(1.0f, std::sqrt(sumSq / static_cast<float>(cnt))) : 0.0f;
        }
    } else {
        snap.capturedWaveformPeak.fill(0.0f);
        snap.capturedWaveformRms.fill(0.0f);
    }

    std::memset(snap.statusBannerText, 0, sizeof(snap.statusBannerText));
    try {
        const std::string statusStr = modelEngine_.getLoadingStatusText();
        if (!statusStr.empty()) {
            std::strncpy(snap.statusBannerText, statusStr.c_str(), sizeof(snap.statusBannerText) - 1);
        }
    } catch (...) {
    }

    const auto state = static_cast<TimelineState>(snap.timelineStateVal);
    if (snap.captureCapacityExceeded) {
        std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                      "240S SPAN LIMIT: EXISTING CAPTURE KEPT");
    } else if (state == TimelineState::kCapture) {
        if (snap.isCapturingNow) {
            std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                          snap.isAppendCapture ? "APPENDING TIMELINE AUDIO (%.1fS TOTAL)..." :
                                                 "CAPTURING TIMELINE AUDIO (%.1fS RECORDED)...", snap.capturedDurationSec);
        } else {
            std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                          snap.isAppendCapture ? "APPEND ARMED: PLAY DAW; EXISTING AUDIO KEPT" :
                                                 "CAPTURE ARMED: PLAY DAW TO RECORD PHRASE");
        }
    } else if (state == TimelineState::kGenerating) {
        std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                      "GENERATING RVC NEURAL CACHE (%.0f%% // %u/%u SEGS)...",
                      snap.synthesisProgress * 100.0f, snap.cachedChunksCount, std::max(1u, snap.totalActiveChunks));
    } else if (state == TimelineState::kActive) {
        if (snap.isPendingRegenerate) {
            std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                          "MODIFIED: CLICK [GENERATE] TO UPDATE");
        } else {
            std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                          "ACTIVE CACHE LOCKED (%.1fS READY)", snap.capturedDurationSec);
        }
    } else {
        if (rawCapSamples > 0) {
            std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                          snap.isAppendCapture ? "APPEND COMPLETE (%.1fS): GENERATE OR APPEND AGAIN" :
                                                 "CAPTURED %.1fS: CLICK [GENERATE] TO CONVERT", snap.capturedDurationSec);
        } else {
            std::snprintf(snap.statusBannerText, sizeof(snap.statusBannerText),
                          "IDLE: CLICK [CAPTURE] AND PLAY DAW TO RECORD");
        }
    }
    return snap;
}

void RvcFeatureInjector::processBlock(
    const float* inLeft,
    const float* inRight,
    float* outLeft,
    float* outRight,
    int numSamples,
    int numChannels,
    int64_t timelineSamplePos,
    bool isTransportPlaying) noexcept {

    if (numSamples <= 0 || !inLeft || !outLeft) {
        return;
    }

    // Cubase may send playing pre-roll before the stopped cursor while upstream audio is muted.
    // One atomic encodes an explicit stopped position; playing/missing context invalidates it.
    latestStoppedProjectSample_.store(!isTransportPlaying && timelineSamplePos >= 0 ? timelineSamplePos : -1,
        std::memory_order_release);

    // Pair the command read with worker handoff; a release store permits Store -> Load reordering.
    audioBlockInFlight_.store(true, std::memory_order_seq_cst);
    struct EndAudioBlock {
        std::atomic<bool>& inFlight;
        ~EndAudioBlock() { inFlight.store(false, std::memory_order_release); }
    } endAudioBlock{audioBlockInFlight_};
    const uint64_t blockControl = timelineControl_.load(std::memory_order_seq_cst);
    const uint64_t captureRevision = blockControl >> 2;
    const uint32_t captureGeneration = static_cast<uint32_t>(blockControl >> 32);
    const TimelineState curTimelineState = static_cast<TimelineState>(blockControl & 3);
    applyCaptureGeneration(captureGeneration);
    const bool captureBegan = curTimelineState == TimelineState::kCapture && blockControl != lastAudioTimelineControl_;
    if (captureBegan) {
        expectedTimelineSamplePos_ = -1;
        capturePassStarted_ = false;
        capturePassExpectedSamplePos_ = -1;
        capturePassStartFloor_ = captureArmRevision_.load(std::memory_order_acquire) == captureRevision
            ? captureArmStartFloor_.load(std::memory_order_relaxed) : -1;
    }
    if (curTimelineState == TimelineState::kCapture && !capturePassStarted_) {
        if (timelineSamplePos < 0) capturePassStartFloor_ = -1;
        else if (!isTransportPlaying) capturePassStartFloor_ = timelineSamplePos;
    }
    lastAudioTimelineControl_ = blockControl;
    const auto tStart = std::chrono::high_resolution_clock::now();

    // A take ends before a stop/return block can overwrite its beginning, even if kPlaying
    // arrives one block late. Arming while stopped still waits for its first playing block.
    const bool finishCapturePass = curTimelineState == TimelineState::kCapture && capturePassStarted_ &&
        (!isTransportPlaying || (appendCapture_.load(std::memory_order_acquire) && timelineSamplePos >= 0 &&
         capturePassExpectedSamplePos_ >= 0 && timelineSamplePos != capturePassExpectedSamplePos_));
    if (finishCapturePass) {
        uint64_t expected = blockControl;
        timelineControl_.compare_exchange_strong(expected, (blockControl & ~uint64_t{3}) |
            static_cast<uint64_t>(TimelineState::kIdle), std::memory_order_acq_rel);
    }

    // Detect genuine transport advancement: if the host pauses (isTransportPlaying == false)
    // or freezes projectTimeSamples at a stationary value across consecutive audio blocks,
    // never loop the same cached audio slice (which would create an fs/N wavetable buzz).
    const bool isStationaryTimeline =
        (!captureBegan && lastTransportWasPlaying_ && timelineSamplePos >= 0 && timelineSamplePos == lastTimelineSamplePos_);
    lastTransportWasPlaying_ = isTransportPlaying;
    lastTimelineSamplePos_ = timelineSamplePos;
    const bool isEffectivelyPlaying = isTransportPlaying && !isStationaryTimeline;
    if (isEffectivelyPlaying && timelineSamplePos >= 0 && expectedTimelineSamplePos_ >= 0 &&
        timelineSamplePos != expectedTimelineSamplePos_) {
        const float playingGate = transportPlayGate_;
        const float cacheBlend = neuralCacheCrossfade_;
        const bool wasFlushed = dspStateFlushedOnPause_;
        reset();
        // A seek invalidates pitch/filter history, but it does not stop playback or make a
        // ready cache unavailable. Restarting these fades would leak live dry at every seek.
        transportPlayGate_ = playingGate;
        neuralCacheCrossfade_ = cacheBlend;
        dspStateFlushedOnPause_ = wasFlushed;
        lastTimelineSamplePos_ = timelineSamplePos;
        lastTransportWasPlaying_ = isTransportPlaying;
    }
    expectedTimelineSamplePos_ = (isEffectivelyPlaying && timelineSamplePos >= 0)
                                    ? timelineSamplePos + numSamples : -1;
    hostIsCurrentlyPlaying_.store(isEffectivelyPlaying, std::memory_order_relaxed);

    int64_t capStart, capEnd;
    getCaptureRange(capStart, capEnd);
    const int64_t fadeLen = std::max<int64_t>(48, static_cast<int64_t>(sampleRate_ * 0.005));

    // Resolve host timeline position (or internal fallback cursor if host omits projectTimeSamples)
    int64_t hostPos = timelineSamplePos;
    if (isEffectivelyPlaying && hostPos < 0 && maxCacheSamples_ > 0) {
        hostPos = internalTimelineCursor_.load(std::memory_order_relaxed);
        internalTimelineCursor_.store(
            hostPos <= std::numeric_limits<int64_t>::max() - numSamples ? hostPos + numSamples : hostPos,
            std::memory_order_relaxed);
    }

    // Commit only samples at/after the armed cursor, and never evict old audio for pre-roll.
    bool captureBlockAccepted = false;
    int captureFirstSample = 0;
    if (curTimelineState == TimelineState::kCapture && !finishCapturePass &&
        isEffectivelyPlaying && hostPos >= 0 && maxCacheSamples_ > 0) {
        if (!capturePassStarted_ && capturePassStartFloor_ > hostPos)
            captureFirstSample = static_cast<int>(std::min<int64_t>(numSamples, capturePassStartFloor_ - hostPos));
        if (captureFirstSample < numSamples && hostPos <= std::numeric_limits<int64_t>::max() - numSamples) {
            const int64_t acceptedStart = hostPos + captureFirstSample;
            const int64_t mergedStart = capStart < 0 ? acceptedStart : std::min(capStart, acceptedStart);
            const int64_t mergedEnd = std::max(capEnd, hostPos + numSamples);
            if (mergedEnd - mergedStart <= static_cast<int64_t>(maxCacheSamples_)) {
                capStart = mergedStart;
                capEnd = mergedEnd;
                captureStartSample_.store(capStart, std::memory_order_release);
                captureEndSample_.store(capEnd, std::memory_order_release);
                timelineOriginSample_.store(capStart, std::memory_order_release);
                captureBlockAccepted = true;
            }
        }
        if (!captureBlockAccepted && captureFirstSample < numSamples)
            captureCapacityExceeded_.store(true, std::memory_order_release);
    }
    isCapturingNow_.store(captureBlockAccepted, std::memory_order_relaxed);

    const bool hasStereo = (numChannels >= 2 && inRight != nullptr && outRight != nullptr);
    float blockInPeak = 1e-6f;
    float blockOutPeak = 1e-6f;
    for (int i = 0; i < numSamples; ++i) {
        const float l = std::abs(inLeft[i]);
        const float r = hasStereo ? std::abs(inRight[i]) : l;
        blockInPeak = std::max(blockInPeak, std::max(l, r));
    }

    // 2. Compute Relative Cache Offset (`blockRelStart`) anchored to `capStart` ONLY when transport is advancing.
    //    When transport pauses while `transportPlayGate_ > 0`, advance smoothly from `lastActiveRelEnd_` during
    //    the 3ms anti-click fade-out so the paused block never repeats the previous buffer slice.
    int64_t blockRelStart = -1;
    bool hasValidRelBlock = false;
    if (isEffectivelyPlaying && hostPos >= 0 && maxCacheSamples_ > 0 && chunkLenSamples_ > 0) {
        int64_t origin = timelineOriginSample_.load(std::memory_order_relaxed);
        if (capStart >= 0) {
            origin = capStart;
            if (timelineOriginSample_.load(std::memory_order_relaxed) != capStart) {
                timelineOriginSample_.store(capStart, std::memory_order_release);
            }
        } else if (hostPos >= 0 && hostPos + numSamples <= static_cast<int64_t>(maxCacheSamples_)) {
            if (origin != 0) {
                timelineOriginSample_.store(0, std::memory_order_release);
                origin = 0;
            }
        } else if (hostPos >= static_cast<int64_t>(maxCacheSamples_)) {
            const int64_t alignedOrigin = (hostPos / static_cast<int64_t>(chunkLenSamples_)) *
                                          static_cast<int64_t>(chunkLenSamples_);
            if (hostPos < origin || hostPos - origin + numSamples > static_cast<int64_t>(maxCacheSamples_)) {
                timelineOriginSample_.store(alignedOrigin, std::memory_order_release);
                origin = alignedOrigin;
            }
        }

        const int64_t rel = hostPos - origin;
        if (rel + numSamples > 0 && rel < static_cast<int64_t>(maxCacheSamples_)) {
            blockRelStart = rel;
            lastActiveRelEnd_ = rel + numSamples;
            hasValidRelBlock = true;
            latestPlayheadRelSample_.store(
                std::clamp<int64_t>(rel, 0, static_cast<int64_t>(maxCacheSamples_) - 1),
                std::memory_order_relaxed);
        }
    } else if (!isEffectivelyPlaying && curTimelineState == TimelineState::kActive &&
               transportPlayGate_ > 1e-4f && lastActiveRelEnd_ >= 0 &&
               lastActiveRelEnd_ < static_cast<int64_t>(maxCacheSamples_)) {
        blockRelStart = lastActiveRelEnd_;
        lastActiveRelEnd_ += numSamples;
        hasValidRelBlock = true;
    }

    // Parameter smoothing filter coefficients (~15ms time constant)
    constexpr float kSmoothCoeff = 0.0035f;
    // 60ms macro-dynamic RMS energy window coefficient (prevents phoneme-level jitter dips)
    const float kRmsWindowCoeff = std::clamp(1.0f - std::exp(-1.0f / static_cast<float>(0.060 * sampleRate_)), 0.0001f, 0.005f);
    bool blockUsedNeuralCache = false;
    uint64_t appendCompared = 0, appendChanged = 0;

    // Precompute real-time filter coefficients for this block
    const float srScale = std::clamp(static_cast<float>(sampleRate_ / 48000.0), 0.5f, 4.0f);
    const float airCoeff = std::clamp(static_cast<float>(2.0 * kPi * 7200.0 / sampleRate_), 0.05f, 0.78f);
    const float tiltCoeff = std::clamp(static_cast<float>(2.0 * kPi * 1000.0 / sampleRate_), 0.02f, 0.45f);
    const float pdaLpfCoef = std::clamp(static_cast<float>(1.0 - std::exp(-2.0 * kPi * 600.0 / sampleRate_)), 0.01f, 0.35f);

    // Precompute 4-Band P.02 Throat Formant Warping SVF coefficients (F1=520Hz, F2=1420Hz, F3=2550Hz, F4=3750Hz)
    static constexpr float kBaseFormantHz[4] = {520.0f, 1420.0f, 2550.0f, 3750.0f};
    static constexpr float kFormantQ[4] = {2.8f, 3.2f, 3.5f, 3.8f};
    float coefficientThroatSt = paramThroatFormantSt_.load(std::memory_order_relaxed);
    const float throatRatio = std::pow(2.0f, std::clamp(coefficientThroatSt, -6.0f, 6.0f) / 12.0f);
    SvfCoeffs svfBaseCoeffs[4];
    SvfCoeffs svfShiftCoeffs[4];
    for (int b = 0; b < 4; ++b) {
        svfBaseCoeffs[b] = makeSvfBandpassCoeffs(kBaseFormantHz[b], kFormantQ[b], sampleRate_);
        svfShiftCoeffs[b] = makeSvfBandpassCoeffs(kBaseFormantHz[b] * throatRatio, kFormantQ[b], sampleRate_);
    }

    const bool hasSlotA = slotALoaded_.load(std::memory_order_relaxed);
    const bool hasSlotB = slotBLoaded_.load(std::memory_order_relaxed);
    const int64_t totalCapSamples = (capStart >= 0 && capEnd > capStart)
                                        ? std::min<int64_t>(capEnd - capStart, static_cast<int64_t>(maxCacheSamples_))
                                        : -1;

    // Stage 1 Helper: Soundtoys Little AlterBoy `wmPitchDetect_ProcessChunk`
    // Decimated low-pass autocorrelation / CMNDF with track continuity boost + 5-Point Median Filter (`medianTableArray[5]`)
    auto runWmPitchDetectChunk = [&]() noexcept {
        const int minLag = std::max(20, static_cast<int>(36.0f * srScale));     // ~1333 Hz max F0
        const int maxLag = std::min(1200, static_cast<int>(800.0f * srScale));  // ~60 Hz min F0
        const int tapStep = std::max(2, static_cast<int>(4.0f * srScale));      // 64 taps * 4 = 256 samples (~5.33 ms)
        const int probeBaseInt = static_cast<int>(pitchWritePos_) - static_cast<int>(16.0f * srScale);

        float rxxEnergy = 0.0f;
        float probeSamples[64];
        for (int k = 0; k < 64; ++k) {
            const float s = pitchLpBuf_[static_cast<size_t>(probeBaseInt - k * tapStep) & kPitchBufMask];
            probeSamples[k] = s;
            rxxEnergy += s * s;
        }

        if (rxxEnergy < 1e-6f) {
            pdaTrackCount_ = 0;
            pdaVoiced_ = false;
            return;
        }

        const int coarseStep = std::max(1, static_cast<int>(2.0f * srScale));
        static constexpr int kMaxCoarseLags = 600;
        float cmndVals[kMaxCoarseLags];
        int lagVals[kMaxCoarseLags];
        int numLags = 0;
        float runningSum = 0.0f;
        float minCmnd = 1e9f;
        int bestLagIdx = 0;

        for (int lag = minLag; lag <= maxLag && numLags < kMaxCoarseLags; lag += coarseStep) {
            float diffSq = 0.0f;
            const int candStart = probeBaseInt - lag;
            for (int k = 0; k < 64; ++k) {
                const float c = pitchLpBuf_[static_cast<size_t>(candStart - k * tapStep) & kPitchBufMask];
                const float diff = probeSamples[k] - c;
                diffSq += diff * diff;
            }
            runningSum += diffSq;
            float cmnd = (numLags > 0)
                             ? (diffSq * static_cast<float>(numLags + 1) / std::max(1e-9f, runningSum))
                             : 1.0f;

            // `trackPeakLocation` / `trackBoost` continuity bias toward `trackedPeriodSamples_`
            if (pdaTrackCount_ > 0) {
                const float relDiff = std::abs(static_cast<float>(lag) - trackedPeriodSamples_) /
                                      std::max(32.0f * srScale, trackedPeriodSamples_);
                if (relDiff < 0.18f) {
                    cmnd *= (0.82f + 0.18f * (relDiff / 0.18f));
                }
            }

            cmndVals[numLags] = cmnd;
            lagVals[numLags] = lag;
            if (cmnd < minCmnd) {
                minCmnd = cmnd;
                bestLagIdx = numLags;
            }
            ++numLags;
        }

        // First significant peak selection (`sigPeakLocation` / `sigPeakCoef = 0.6f`)
        const float sigThresh = std::max(0.02f, std::min(0.22f, 2.4f * minCmnd));
        int chosenIdx = bestLagIdx;
        for (int idx = 1; idx < numLags - 1; ++idx) {
            if (cmndVals[idx] < sigThresh &&
                cmndVals[idx] <= cmndVals[idx - 1] &&
                cmndVals[idx] <= cmndVals[idx + 1]) {
                chosenIdx = idx;
                break;
            }
        }

        const int coarseLag = lagVals[chosenIdx];
        int bestFineLag = coarseLag;
        float minFineDiff = 1e30f;
        for (int dOff = -coarseStep; dOff <= coarseStep; ++dOff) {
            const int lag = std::clamp(coarseLag + dOff, minLag, maxLag);
            float diffSq = 0.0f;
            const int candStart = probeBaseInt - lag;
            for (int k = 0; k < 64; ++k) {
                const float c = pitchLpBuf_[static_cast<size_t>(candStart - k * tapStep) & kPitchBufMask];
                const float diff = probeSamples[k] - c;
                diffSq += diff * diff;
            }
            if (diffSq < minFineDiff) {
                minFineDiff = diffSq;
                bestFineLag = lag;
            }
        }

        // Parabolic sub-sample refinement (`pitchInfo_period`)
        float refinedPeriod = static_cast<float>(bestFineLag);
        if (bestFineLag > minLag && bestFineLag < maxLag) {
            auto evalDiffSq = [&](int lag) noexcept -> float {
                float s = 0.0f;
                const int candStart = probeBaseInt - lag;
                for (int k = 0; k < 64; ++k) {
                    const float c = pitchLpBuf_[static_cast<size_t>(candStart - k * tapStep) & kPitchBufMask];
                    const float diff = probeSamples[k] - c;
                    s += diff * diff;
                }
                return s;
            };
            const float ym1 = evalDiffSq(bestFineLag - 1);
            const float y0 = minFineDiff;
            const float yp1 = evalDiffSq(bestFineLag + 1);
            const float denom = 2.0f * (ym1 - 2.0f * y0 + yp1);
            if (std::abs(denom) > 1e-9f) {
                refinedPeriod += std::clamp((ym1 - yp1) / denom, -0.5f, 0.5f);
            }
        }

        if (cmndVals[chosenIdx] < 0.45f) {
            // 5-Point Median Filter (`medianTableArray[5]` -> `medianPeriod`)
            pdaMedianTable_[static_cast<size_t>(pdaMedianIdx_)] = refinedPeriod;
            pdaMedianIdx_ = (pdaMedianIdx_ + 1) % 5;
            std::array<float, 5> sortedMed = pdaMedianTable_;
            std::sort(sortedMed.begin(), sortedMed.end());
            trackedPeriodSamples_ = std::clamp(
                sortedMed[2], static_cast<float>(minLag), static_cast<float>(maxLag));
            pdaTrackCount_ = std::min(pdaTrackCount_ + 1, 100);
            pdaVoiced_ = true;
        } else {
            pdaTrackCount_ = std::max(0, pdaTrackCount_ - 1);
            if (pdaTrackCount_ == 0) {
                pdaVoiced_ = false;
            }
        }
    };

    for (int i = 0; i < numSamples; ++i) {
        if (static_cast<size_t>(i) < blockAutomationCapacity_) {
            auto& frame = blockAutomation_[static_cast<size_t>(i)];
            const uint16_t mask = frame.mask;
            for (uint32_t p = 0; p < 12; ++p) {
                if (mask & (1u << p)) applyAutomationPoint(p, frame.values[p]);
            }
            frame.mask = 0;
        }
        const bool bypassed = paramBypass_.load(std::memory_order_relaxed);
        const float targetDryWet = bypassed ? 0.0f : paramDryWetMix_.load(std::memory_order_relaxed);
        const float targetRmsMix = paramRmsMixRate_.load(std::memory_order_relaxed);
        const float targetAbMorph = paramAbMorph_.load(std::memory_order_relaxed);
        const float targetInjectDepth = paramInjectDepth_.load(std::memory_order_relaxed);
        const float targetPitchSt = paramPitchShiftSt_.load(std::memory_order_relaxed);
        const float targetThroatSt = paramThroatFormantSt_.load(std::memory_order_relaxed);
        const float targetPresenceDb = paramPresenceAirDb_.load(std::memory_order_relaxed);
        const float targetProtect = paramConsonantProtect_.load(std::memory_order_relaxed);
        const float targetSlotAIdx = paramSlotAIndexRate_.load(std::memory_order_relaxed);
        const float targetSlotBIdx = paramSlotBIndexRate_.load(std::memory_order_relaxed);
        const float targetOutputGainDb = bypassed ? 0.0f : paramOutputGainDb_.load(std::memory_order_relaxed);
        if (targetThroatSt != coefficientThroatSt) {
            coefficientThroatSt = targetThroatSt;
            const float ratio = std::pow(2.0f, targetThroatSt / 12.0f);
            for (int b = 0; b < 4; ++b)
                svfShiftCoeffs[b] = makeSvfBandpassCoeffs(kBaseFormantHz[b] * ratio, kFormantQ[b], sampleRate_);
        }
        const float dryL = inLeft[i];
        const float dryR = hasStereo ? inRight[i] : dryL;
        const float midIn = 0.5f * (dryL + dryR);

        // Always smooth continuous parameters so transitions are click-free
        smoothAbMorph_ += kSmoothCoeff * (targetAbMorph - smoothAbMorph_);
        smoothInjectDepth_ += kSmoothCoeff * (targetInjectDepth - smoothInjectDepth_);
        smoothPitchSt_ += kSmoothCoeff * (targetPitchSt - smoothPitchSt_);
        smoothThroatSt_ += kSmoothCoeff * (targetThroatSt - smoothThroatSt_);
        smoothPresenceDb_ += kSmoothCoeff * (targetPresenceDb - smoothPresenceDb_);
        smoothProtect_ += kSmoothCoeff * (targetProtect - smoothProtect_);
        smoothRmsMix_ += kSmoothCoeff * (targetRmsMix - smoothRmsMix_);
        smoothDryWet_ += kSmoothCoeff * (targetDryWet - smoothDryWet_);
        smoothSlotAIndex_ += kSmoothCoeff * (targetSlotAIdx - smoothSlotAIndex_);
        smoothSlotBIndex_ += kSmoothCoeff * (targetSlotBIdx - smoothSlotBIndex_);
        smoothOutputGainDb_ += kSmoothCoeff * (targetOutputGainDb - smoothOutputGainDb_);

        // Capture Mode: Record into the fixed project-addressed ring and pass through 100% dry
        if (curTimelineState == TimelineState::kCapture) {
            if (captureBlockAccepted && i >= captureFirstSample && hasValidRelBlock && (blockRelStart + i) >= 0) {
                const size_t relIdx = static_cast<size_t>(blockRelStart + i);
                if (relIdx < maxCacheSamples_) {
                    const size_t cIdx = relIdx / chunkLenSamples_;
                    const size_t offInChunk = relIdx % chunkLenSamples_;
                    if (cIdx < kMaxChunks) {
                        const size_t physical = static_cast<size_t>((hostPos + i) % static_cast<int64_t>(maxCacheSamples_));
                        if (appendCapture_.load(std::memory_order_relaxed)) {
                            ++appendCompared;
                            if (!hasCapturedSample(relIdx, captureGeneration, capStart) ||
                                dryTimelineCache_[physical].load(std::memory_order_relaxed) != midIn) ++appendChanged;
                        }
                        dryTimelineCache_[physical].store(midIn, std::memory_order_relaxed);
                        auto& validity = captureValidity_[physical / 64];
                        if (validity.generation.load(std::memory_order_relaxed) != captureGeneration) {
                            validity.mask.store(0, std::memory_order_relaxed);
                            validity.generation.store(captureGeneration, std::memory_order_release);
                        }
                        validity.mask.fetch_or(uint64_t{1} << (physical % 64), std::memory_order_release);
                        const int64_t projectFrame = automationFrameAt(hostPos + i);
                        const size_t autoIdx = static_cast<size_t>(projectFrame % kAutomationCacheFrames);
                        if (automationTimeline_) {
                            auto& frame = automationTimeline_[autoIdx];
                            // Automation is 10ms: preserve the whole old frame that straddles the arm floor.
                            const bool preserveBoundaryFrame = capturePassStartFloor_ > 0 &&
                                automationFrameAt(capturePassStartFloor_ - 1) == projectFrame &&
                                frame.generation.load(std::memory_order_relaxed) == captureGeneration &&
                                frame.projectFrame.load(std::memory_order_relaxed) == projectFrame;
                            if (!preserveBoundaryFrame && (i == 0 || automationFrameAt(hostPos + i - 1) != projectFrame ||
                                frame.generation.load(std::memory_order_relaxed) != captureGeneration ||
                                frame.projectFrame.load(std::memory_order_relaxed) != projectFrame)) {
                                if (appendCapture_.load(std::memory_order_relaxed) &&
                                    (frame.generation.load(std::memory_order_relaxed) != captureGeneration ||
                                     frame.projectFrame.load(std::memory_order_relaxed) != projectFrame ||
                                     frame.slotAIndexRate.load(std::memory_order_relaxed) != targetSlotAIdx ||
                                     frame.slotBIndexRate.load(std::memory_order_relaxed) != targetSlotBIdx))
                                    appendAutomationChanged_.store(true, std::memory_order_release);
                                frame.abMorph.store(targetAbMorph, std::memory_order_relaxed);
                                frame.injectDepth.store(targetInjectDepth, std::memory_order_relaxed);
                                frame.slotAIndexRate.store(targetSlotAIdx, std::memory_order_relaxed);
                                frame.slotBIndexRate.store(targetSlotBIdx, std::memory_order_relaxed);
                                frame.projectFrame.store(projectFrame, std::memory_order_relaxed);
                                frame.generation.store(captureGeneration, std::memory_order_release);
                            }
                        }
                        auto& cs = chunkStates_[cIdx];
                        const uint32_t prevCap = cs.capturedCount.load(std::memory_order_relaxed);
                        if (offInChunk + 1 > prevCap) {
                            cs.capturedCount.store(static_cast<uint32_t>(offInChunk + 1), std::memory_order_release);
                        }
                        const float absMid = std::abs(midIn);
                        const float prevPeak = cs.maxAbsPeak.load(std::memory_order_relaxed);
                        if (absMid > prevPeak) {
                            cs.maxAbsPeak.store(absMid, std::memory_order_relaxed);
                        }
                    }
                }
            }
            outLeft[i] = dryL;
            if (hasStereo) outRight[i] = dryR;
            blockOutPeak = std::max(blockOutPeak, std::max(std::abs(dryL), std::abs(dryR)));
            continue;
        }

        // Generating or Idle Mode: Direct dry pass-through
        if (curTimelineState == TimelineState::kGenerating || curTimelineState == TimelineState::kIdle) {
            outLeft[i] = dryL;
            if (hasStereo) outRight[i] = dryR;
            blockOutPeak = std::max(blockOutPeak, std::max(std::abs(dryL), std::abs(dryR)));
            continue;
        }

        // Active Mode: Relative Timeline Range Verification [0, totalCapSamples) with 5ms Cosine Crossfade
        // and 3ms Transport Play/Pause Anti-Click Gate
        const float playGateStep = 1.0f / std::max(32.0f, static_cast<float>(sampleRate_ * 0.003));
        if (isEffectivelyPlaying) {
            transportPlayGate_ = std::min(1.0f, transportPlayGate_ + playGateStep);
            dspStateFlushedOnPause_ = false;
        } else {
            transportPlayGate_ = std::max(0.0f, transportPlayGate_ - playGateStep);
        }

        const int64_t relPos = hasValidRelBlock ? (blockRelStart + i) : -1;
        float activeWeight = 0.0f;
        if (totalCapSamples > 0) {
            if (relPos >= 0 && relPos < totalCapSamples) {
                if (relPos < fadeLen) {
                    const float t = static_cast<float>(relPos) / static_cast<float>(fadeLen);
                    activeWeight = 0.5f * (1.0f - std::cos(kPi * t));
                } else if (totalCapSamples - relPos < fadeLen) {
                    const float t = static_cast<float>(totalCapSamples - relPos) / static_cast<float>(fadeLen);
                    activeWeight = 0.5f * (1.0f - std::cos(kPi * t));
                } else {
                    activeWeight = 1.0f;
                }
            }
        } else {
            activeWeight = 1.0f; // Fallback for unbounded playback
        }

        const size_t readyChunk = relPos >= 0 ? static_cast<size_t>(relPos) / chunkLenSamples_ : kMaxChunks;
        const bool readyAtPosition = relPos >= 0 && relPos < totalCapSamples && readyChunk < kMaxChunks &&
            chunkStates_[readyChunk].wetReady.load(std::memory_order_acquire) &&
            chunkStates_[readyChunk].wetGeneration.load(std::memory_order_acquire) == captureRevision;
        if (readyAtPosition && isEffectivelyPlaying) transportPlayGate_ = 1.0f;
        const float smoothPlayGate = 0.5f * (1.0f - std::cos(kPi * transportPlayGate_));
        if (readyAtPosition) activeWeight = 1.0f;
        else activeWeight *= smoothPlayGate;

        if (activeWeight <= 0.0f) {
            if (!isEffectivelyPlaying && !dspStateFlushedOnPause_) {
                const float defaultPeriod = 200.0f * srScale;
                pitchLpBuf_.fill(0.0f);
                pitchWritePos_ = 0;
                pitchTotalSamples_ = 0;
                pdaLpfState1_ = 0.0f;
                pdaLpfState2_ = 0.0f;
                pdaMedianTable_.fill(defaultPeriod);
                pdaMedianIdx_ = 0;
                trackedPeriodSamples_ = defaultPeriod;
                pitchLpfState_ = defaultPeriod;
                pdaTrackCount_ = 0;
                pdaVoiced_ = false;
                pdaChunkCounter_ = 0;
                goertzelRe_ = 0.0f;
                goertzelIm_ = 0.0f;
                prevGoertzelRe_ = 0.0f;
                epochTable_.fill(0.0);
                epochWriteIdx_ = 0;
                epochCount_ = 0;
                currentEpochLoc_ = 0.0;
                prevEpochLoc_ = -static_cast<double>(defaultPeriod);
                epochSpacing_ = defaultPeriod;
                pitchShiftCrossfade_ = 0.0f;
                smoothPeriodRatio_ = std::pow(2.0f, -std::clamp(smoothPitchSt_, -12.0f, 12.0f) / 12.0f);
                motif1Delay_ = 0.0f;
                motif2Delay_ = 0.0f;
                motif1Age_ = 0.0f;
                motif2Age_ = 0.0f;
                splicePhase_ = 1.0f;
                spliceStep_ = 1.0f / (48.0f * srScale);
                epochTimer_ = 0.0f;
                prevVoicedState_ = false;
                dcBlockX1_ = 0.0f;
                dcBlockY1_ = 0.0f;
                pitchInEnv_ = 0.001f;
                pitchOutEnv_ = 0.001f;
                pitchSilentSamples_ = 1000000;
                pitchTransientSamples_ = 0;
                for (auto& s : svfBase_) s = SvfBandState{};
                for (auto& s : svfShifted_) s = SvfBandState{};
                formantTiltLp_ = 0.0f;
                presenceShelfLp_ = 0.0f;
                presenceShelfLp2_ = 0.0f;
                drySibilanceLp_.fill(0.0f);
                wetSibilanceLp_.fill(0.0f);
                dryHfEnv_ = 0.0f;
                dryLowEnv_ = 0.0f;
                dryEnvFollower_ = 0.0f;
                wetEnvFollower_ = 0.0f;
                smoothEnvGain_ = 1.0f;
                neuralCacheCrossfade_ = 0.0f;
                dspStateFlushedOnPause_ = true;
            }
            outLeft[i] = dryL;
            if (hasStereo) outRight[i] = dryR;
            blockOutPeak = std::max(blockOutPeak, std::max(std::abs(dryL), std::abs(dryR)));
            continue;
        }

        bool sampleHasNeuralWet = false;
        float sampleWetA = midIn;
        float sampleWetB = midIn;

        if (relPos >= 0 && static_cast<size_t>(relPos) < maxCacheSamples_) {
            const size_t relIdx = static_cast<size_t>(relPos);
            const size_t cIdx = relIdx / chunkLenSamples_;

            if (cIdx < kMaxChunks) {
                auto& cs = chunkStates_[cIdx];
                const bool isReady = cs.wetReady.load(std::memory_order_acquire) &&
                    cs.wetGeneration.load(std::memory_order_acquire) == captureRevision;
                if (isReady) {
                    sampleHasNeuralWet = true;
                    sampleWetA = wetSlotACache_[relIdx].load(std::memory_order_relaxed);
                    sampleWetB = wetSlotBCache_[relIdx].load(std::memory_order_relaxed);
                    blockUsedNeuralCache = true;
                }
            }
        }

        // ====================================================================
        // 100% Real-Time Zero-Latency Three-Way Balance & Effector DSP Pipeline
        // ====================================================================

        // 1. Dual-Model RVC Timbre Blend (MODEL A + MODEL B, with automatic 1-Model / 2-Model adaptation)
        //    Note: The DRY component in Three-Way Balance is bi-directionally bound to P.06 Dry/Wet Mix
        //    and routed via the pure uncolored stereo dry path at Step 7, preventing comb-filtering
        //    or unwanted pitch/formant coloration of the dry voice.
        float wA = 0.0f;
        float wB = 0.0f;
        if (hasSlotA && hasSlotB) {
            wA = 1.0f - smoothAbMorph_;
            wB = smoothAbMorph_;
        } else if (hasSlotA) {
            wA = 1.0f;
            wB = 0.0f;
        } else if (hasSlotB) {
            wA = 0.0f;
            wB = 1.0f;
        } else {
            // If neither model slot is loaded (or fallback test mode), blend A/B cache streams directly
            wA = 1.0f - smoothAbMorph_;
            wB = smoothAbMorph_;
        }

        float rvcMorphed = wA * sampleWetA + wB * sampleWetB;

        // 2. Soundtoys Little AlterBoy 3-Stage Pitch Shifter (P.01: -12.0 .. +12.0 ST)
        //    - Stage 1 (`wmPitchDetect`): 2-pole 600Hz decimation LPF + normalized autocorrelation/CMNDF
        //      with track continuity boost + 5-Point Median Filter (`pdaMedianTable_`).
        //    - Stage 2 (`wmGrainShift`): Coupled Goertzel Fundamental Resonator (`goertzelRe_`, `goertzelIm_`)
        //      tuned to `omega0 = 2*pi / T0`, isolating the pure fundamental sinusoid so positive zero-crossings
        //      mark jitter-free fundamental epochs in `epochTable_`.
        //    - Stage 3 (`wmGrainShiftVoice`): Dual-motif (`motif1`, `motif2`) epoch-snapped splicer with
        //      `formantRatio = 1.0f` (1.0x intra-grain playback rate preserving original vocal-tract formants),
        //      short raised-cosine crossfade (`windowTbl`, `w1 + w2 === 1.0`), and `wmDCBlock` (`dcCoef = 0.999f`).
        const float inForPitch = rvcMorphed;
        pdaLpfState1_ += pdaLpfCoef * (inForPitch - pdaLpfState1_);
        pdaLpfState2_ += pdaLpfCoef * (pdaLpfState1_ - pdaLpfState2_);
        pitchLpBuf_[pitchWritePos_] = pdaLpfState2_;

        if (pdaChunkCounter_ <= 0) {
            pdaChunkCounter_ = std::max(32, static_cast<int>(96.0f * srScale));
            runWmPitchDetectChunk();
        }
        --pdaChunkCounter_;

        pitchLpfState_ = 0.9985f * pitchLpfState_ + 0.0015f * trackedPeriodSamples_;
        const float T0 = std::clamp(pitchLpfState_, 20.0f * srScale, 1200.0f * srScale);

        // Stage 2: `wmGrainShift` Goertzel Fundamental Resonator & `epochTable_` update
        const float omega0 = 2.0f * kPi / T0;
        const float rPole = std::clamp(1.0f - (1.2f / T0), 0.960f, 0.999f);
        const float cosW = std::cos(omega0);
        const float sinW = std::sin(omega0);
        const float nextRe = rPole * (cosW * goertzelRe_ - sinW * goertzelIm_) + (1.0f - rPole) * pdaLpfState2_;
        const float nextIm = rPole * (sinW * goertzelRe_ + cosW * goertzelIm_);
        prevGoertzelRe_ = goertzelRe_;
        goertzelRe_ = nextRe;
        goertzelIm_ = nextIm;

        const double curSampleD = static_cast<double>(pitchTotalSamples_);
        if (prevGoertzelRe_ <= 0.0f && goertzelRe_ > 0.0f) {
            const float frac = std::clamp(
                -prevGoertzelRe_ / std::max(1e-12f, goertzelRe_ - prevGoertzelRe_), 0.0f, 1.0f);
            const double exactEpoch = (curSampleD - 1.0) + static_cast<double>(frac);
            const double spacing = exactEpoch - currentEpochLoc_;
            if (spacing >= static_cast<double>(0.55f * T0)) {
                prevEpochLoc_ = currentEpochLoc_;
                currentEpochLoc_ = exactEpoch;
                if (spacing >= static_cast<double>(0.65f * T0) &&
                    spacing <= static_cast<double>(1.45f * T0)) {
                    epochSpacing_ = 0.35f * epochSpacing_ + 0.65f * static_cast<float>(spacing);
                }
                epochTable_[epochWriteIdx_ & kEpochTableMask] = exactEpoch;
                ++epochWriteIdx_;
                if (epochCount_ < kEpochTableSize) {
                    ++epochCount_;
                }
            }
        }

        const float targetPitchCrossfade = (std::abs(smoothPitchSt_) > 0.02f) ? 1.0f : 0.0f;
        pitchShiftCrossfade_ += 0.0060f * (targetPitchCrossfade - pitchShiftCrossfade_);

        const float targetPeriodRatio = std::pow(2.0f, -std::clamp(smoothPitchSt_, -12.0f, 12.0f) / 12.0f);
        smoothPeriodRatio_ = 0.999f * smoothPeriodRatio_ + 0.001f * targetPeriodRatio;

        if (pitchShiftCrossfade_ > 1e-4f || targetPitchCrossfade > 0.0f) {
            const float effPeriod = std::clamp(
                (pdaVoiced_ && epochCount_ >= 2) ? epochSpacing_ : T0,
                20.0f * srScale,
                1200.0f * srScale);
            const float Tout = std::clamp(effPeriod * smoothPeriodRatio_, 16.0f * srScale, 2400.0f * srScale);

            epochTimer_ -= 1.0f;
            if (epochTimer_ <= 0.0f) {
                const float fracStep = std::clamp(-epochTimer_, 0.0f, 0.9999f);
                // The complete generated cache supplies future samples. Center the grain around
                // this timeline position instead of introducing a pitch-dependent causal delay.
                const float minDelay = -0.5f * effPeriod;
                const float maxDelay = minDelay + effPeriod;

                float rawTargetDelay = motif1Delay_ + (Tout - effPeriod);
                while (rawTargetDelay < minDelay) rawTargetDelay += effPeriod;
                while (rawTargetDelay > maxDelay) rawTargetDelay -= effPeriod;

                float snappedDelay = rawTargetDelay;
                if (pdaVoiced_ && epochCount_ >= 2) {
                    const float midOffset = 0.5f * std::min(Tout, effPeriod);
                    const double targetPos = (curSampleD - static_cast<double>(fracStep) + static_cast<double>(midOffset)) -
                                             static_cast<double>(rawTargetDelay);
                    const size_t numSearch = std::min<size_t>(epochCount_, 16);
                    double bestEpoch = epochTable_[(epochWriteIdx_ - 1) & kEpochTableMask];
                    double minAbsDiff = std::abs(bestEpoch - targetPos);
                    for (size_t k = 1; k < numSearch; ++k) {
                        const double candE = epochTable_[(epochWriteIdx_ - 1 - k) & kEpochTableMask];
                        const double d = std::abs(candE - targetPos);
                        if (d < minAbsDiff) {
                            minAbsDiff = d;
                            bestEpoch = candE;
                        }
                    }

                    float epochErr = static_cast<float>(
                        ((curSampleD - static_cast<double>(fracStep) + static_cast<double>(midOffset)) - bestEpoch) -
                        static_cast<double>(rawTargetDelay));
                    while (epochErr > 0.5f * effPeriod) epochErr -= effPeriod;
                    while (epochErr < -0.5f * effPeriod) epochErr += effPeriod;

                    // On first voiced syllable onset, snap faster; in steady state, clamp nudge to +/-1.5*srScale
                    const float maxNudge = prevVoicedState_ ? (1.5f * srScale) : (8.0f * srScale);
                    const float nudge = std::clamp(0.25f * epochErr, -maxNudge, maxNudge);
                    snappedDelay = rawTargetDelay + nudge;
                    while (snappedDelay < minDelay) snappedDelay += effPeriod;
                    while (snappedDelay > maxDelay) snappedDelay -= effPeriod;
                }

                prevVoicedState_ = pdaVoiced_;
                motif2Delay_ = motif1Delay_;
                motif2Age_ = motif1Age_;
                motif1Delay_ = snappedDelay;
                motif1Age_ = fracStep;

                const float spliceLen = std::clamp(0.35f * std::min(Tout, effPeriod), 16.0f * srScale, 160.0f * srScale);
                spliceStep_ = 1.0f / spliceLen;
                splicePhase_ = fracStep * spliceStep_;
                epochTimer_ += Tout;
            }

            // Single-cycle glottal envelope protection for pitch-down (`Tout > effPeriod`)
            auto evalMotifEnv = [&](float age) noexcept -> float {
                if (Tout <= effPeriod) {
                    return 1.0f;
                }
                const float fullLen = 1.35f * effPeriod;
                const float startTaper = 0.65f * effPeriod;
                if (age <= startTaper) {
                    return 1.0f;
                }
                const float taperLen = std::max(16.0f * srScale, fullLen - startTaper);
                const float u = (age - startTaper) / taperLen;
                if (u >= 1.0f) {
                    return 0.0f;
                }
                return 0.5f * (1.0f + std::cos(kPi * u));
            };

            auto readPitchCache = [&](double position) noexcept -> float {
                const int64_t at = static_cast<int64_t>(std::floor(position));
                const float t = static_cast<float>(position - static_cast<double>(at));
                auto sample = [&](int64_t index) noexcept -> float {
                    if (!sampleHasNeuralWet || index < 0 || index >= totalCapSamples ||
                        static_cast<size_t>(index) >= maxCacheSamples_) return 0.0f;
                    const size_t idx = static_cast<size_t>(index);
                    const auto& chunk = chunkStates_[idx / chunkLenSamples_];
                    if (!chunk.wetReady.load(std::memory_order_acquire) ||
                        chunk.wetGeneration.load(std::memory_order_acquire) != captureRevision) return 0.0f;
                    return wA * wetSlotACache_[idx].load(std::memory_order_relaxed) +
                           wB * wetSlotBCache_[idx].load(std::memory_order_relaxed);
                };
                const float a = sample(at - 1), b = sample(at), c = sample(at + 1), d = sample(at + 2);
                return b + 0.5f * t * (c - a + t * (2.0f*a - 5.0f*b + 4.0f*c - d + t*(3.0f*(b-c) + d-a)));
            };
            const float s1 = readPitchCache(static_cast<double>(relPos) - motif1Delay_) * evalMotifEnv(motif1Age_);
            float voiceOut = s1;
            if (splicePhase_ < 1.0f) {
                const float s2 = readPitchCache(static_cast<double>(relPos) - motif2Delay_) * evalMotifEnv(motif2Age_);
                const float w1 = 0.5f * (1.0f - std::cos(kPi * splicePhase_));
                const float w2 = 1.0f - w1;
                voiceOut = w1 * s1 + w2 * s2;
                splicePhase_ = std::min(1.0f, splicePhase_ + spliceStep_);
            }

            motif1Age_ += 1.0f;
            motif2Age_ += 1.0f;

            // `wmDCBlock` (`dcCoef = 0.999f`)
            const float dcOut = voiceOut - dcBlockX1_ + 0.999f * dcBlockY1_;
            dcBlockX1_ = voiceOut;
            dcBlockY1_ = dcOut;

            // Smooth AC RMS envelope matching so output energy stays consistent across all semitones
            pitchInEnv_ = 0.995f * pitchInEnv_ + 0.005f * (inForPitch * inForPitch + 1e-6f);
            pitchOutEnv_ = 0.995f * pitchOutEnv_ + 0.005f * (dcOut * dcOut + 1e-6f);
            const float pitchEnvGain = std::clamp(std::sqrt(pitchInEnv_ / std::max(1e-6f, pitchOutEnv_)), 0.65f, 1.65f);
            const float shiftedOut = dcOut * pitchEnvGain;

            // Preserve the start of a phrase and exact silence: centered grains must never turn
            // lookahead into pre-echo or carry a previous syllable beyond its cached endpoint.
            const int transientLength = std::max(1, static_cast<int>(sampleRate_ * 0.005));
            if (std::abs(inForPitch) < 1e-7f) {
                pitchSilentSamples_ = std::min(pitchSilentSamples_ + 1, 1000000);
            } else {
                if (pitchSilentSamples_ >= static_cast<int>(sampleRate_ * 0.002))
                    pitchTransientSamples_ = transientLength;
                pitchSilentSamples_ = 0;
            }
            const float transientBlend = 1.0f - static_cast<float>(pitchTransientSamples_) / transientLength;
            if (pitchTransientSamples_ > 0) --pitchTransientSamples_;
            const float blend = pitchShiftCrossfade_ * std::clamp(transientBlend, 0.0f, 1.0f);
            rvcMorphed = (std::abs(inForPitch) < 1e-7f) ? 0.0f
                : (1.0f - blend) * inForPitch + blend * shiftedOut;
        } else {
            motif1Delay_ = 0.0f;
            motif2Delay_ = 0.0f;
            motif1Age_ = 0.0f;
            motif2Age_ = 0.0f;
            splicePhase_ = 1.0f;
            epochTimer_ = 0.0f;
            prevVoicedState_ = false;
            dcBlockX1_ = inForPitch;
            dcBlockY1_ = 0.0f;
            pitchInEnv_ = 0.995f * pitchInEnv_ + 0.005f * (inForPitch * inForPitch + 1e-6f);
            pitchOutEnv_ = pitchInEnv_;
        }

        pitchWritePos_ = (pitchWritePos_ + 1) & kPitchBufMask;
        ++pitchTotalSamples_;

        // 3. Real-Time 4-Band SVF Vocal Tract Formant Shifter (P.02: -6.0 .. +6.0 ST, FFocus = 3750Hz)
        const float inForFormant = rvcMorphed;
        float formantDelta = 0.0f;
        for (int b = 0; b < 4; ++b) {
            const float bpBase = processSvfUnityBandpass(
                inForFormant, svfBase_[b].ic1eq, svfBase_[b].ic2eq, svfBaseCoeffs[b]);
            const float bpShift = processSvfUnityBandpass(
                inForFormant, svfShifted_[b].ic1eq, svfShifted_[b].ic2eq, svfShiftCoeffs[b]);
            formantDelta += (bpShift - 0.65f * bpBase);
        }
        formantTiltLp_ += tiltCoeff * (inForFormant - formantTiltLp_);
        if (std::abs(smoothThroatSt_) > 0.02f) {
            const float warpStrength = std::clamp(std::abs(smoothThroatSt_) / 4.5f, 0.0f, 1.0f);
            const float hpTilt = inForFormant - formantTiltLp_;
            const float tiltGain = smoothThroatSt_ * 0.065f;
            rvcMorphed = inForFormant + warpStrength * 0.44f * formantDelta + tiltGain * hpTilt;
        }

        // 4. Real-Time 2-Pole Presence / Air Harmonic Exciter (P.03: -6.0 .. +6.0 dB)
        presenceShelfLp_ += airCoeff * (rvcMorphed - presenceShelfLp_);
        presenceShelfLp2_ += airCoeff * (presenceShelfLp_ - presenceShelfLp2_);
        const float hpAir = rvcMorphed - presenceShelfLp2_;
        if (std::abs(smoothPresenceDb_) > 0.02f) {
            const float airLinearGain = std::pow(10.0f, std::clamp(smoothPresenceDb_, -6.0f, 6.0f) / 20.0f);
            float exciter = hpAir * (airLinearGain - 1.0f);
            if (smoothPresenceDb_ > 0.0f) {
                const float drive = std::clamp(exciter * 1.4f, -1.5f, 1.5f);
                exciter = 0.75f * exciter + 0.25f * (drive - (drive * drive * drive) / 6.75f);
            }
            rvcMorphed += exciter;
        }

        // 5. Real-Time 60ms Macro-RMS Envelope Matcher (P.04: 0% = Raw RVC Dynamics, 100% = Dry Vocal RMS Lock)
        //    Uses a soft noise-floor bias (+2e-3f) and continuous audio-rate smoothing (`smoothEnvGain_`)
        //    so gain never jumps across silence thresholds or brief consonant timing offsets.
        const float envelopeReference = sampleHasNeuralWet && relPos >= 0
            ? readCapturedSample(static_cast<size_t>(relPos), captureGeneration, capStart) : midIn;
        dryEnvFollower_ += kRmsWindowCoeff * (envelopeReference * envelopeReference - dryEnvFollower_);
        wetEnvFollower_ += kRmsWindowCoeff * (rvcMorphed * rvcMorphed - wetEnvFollower_);

        const float dryRms = std::sqrt(std::max(0.0f, dryEnvFollower_));
        const float wetRms = std::sqrt(std::max(0.0f, wetEnvFollower_));
        const float envRatio = std::clamp((dryRms + 2e-3f) / (wetRms + 2e-3f), 0.40f, 2.50f);
        const float targetEnvGain = sampleHasNeuralWet
                                        ? std::pow(envRatio, std::clamp(smoothRmsMix_, 0.0f, 1.0f))
                                        : 1.0f;
        smoothEnvGain_ += 0.0025f * (targetEnvGain - smoothEnvGain_);
        rvcMorphed *= std::clamp(smoothEnvGain_, 0.35f, 2.50f);

        // Smooth crossfade between dry pass-through (when uncached) and RVC neural effector output
        const float targetCrossfade = sampleHasNeuralWet ? 1.0f : 0.0f;
        if (sampleHasNeuralWet) {
            neuralCacheCrossfade_ = 1.0f;
        } else {
            neuralCacheCrossfade_ += 0.0040f * (targetCrossfade - neuralCacheCrossfade_);
        }

        const float wetMid = (1.0f - neuralCacheCrossfade_) * midIn +
                             neuralCacheCrossfade_ * rvcMorphed;

        // Pure wet stereo output without any raw dry side-channel bleed
        // Transport fades the cached signal itself. A dry-to-wet fade can cancel a held note
        // or introduce the original consonant when replay starts at the Append cursor.
        const float wetTransportGain = sampleHasNeuralWet ? smoothPlayGate : 1.0f;
        const float wetL = std::clamp(wetMid * wetTransportGain, -4.0f, 4.0f);
        const float wetR = std::clamp(wetMid * wetTransportGain, -4.0f, 4.0f);

        // 6. Real-Time Dry/Wet Mix (P.05: 0.00 .. 1.00), Output Gain (P.06: -12 .. +12 dB), and Active Crossfade
        const float outGainLin = std::pow(10.0f, std::clamp(smoothOutputGainDb_, -12.0f, 12.0f) / 20.0f);
        const float mixedL = ((1.0f - smoothDryWet_) * dryL + smoothDryWet_ * wetL) * outGainLin;
        const float mixedR = ((1.0f - smoothDryWet_) * dryR + smoothDryWet_ * wetR) * outGainLin;
        const float finalL = std::clamp((1.0f - activeWeight) * dryL + activeWeight * mixedL, -4.0f, 4.0f);
        const float finalR = std::clamp((1.0f - activeWeight) * dryR + activeWeight * mixedR, -4.0f, 4.0f);

        outLeft[i] = finalL;
        if (hasStereo) {
            outRight[i] = finalR;
        }

        blockOutPeak = std::max(blockOutPeak, std::max(std::abs(finalL), std::abs(finalR)));
    }

    playingFromNeuralCache_.store(blockUsedNeuralCache, std::memory_order_relaxed);

    // Publish 60 FPS Visual Telemetry Snapshot
    const auto tEnd = std::chrono::high_resolution_clock::now();
    const float blockElapsedMs = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
    const float blockBudgetMs = static_cast<float>(numSamples) * 1000.0f / static_cast<float>(sampleRate_);
    const float cpuPercent = std::clamp((blockElapsedMs / std::max(0.1f, blockBudgetMs)) * 100.0f, 0.1f, 99.0f);

    const int curTelIdx = activeTelemetryIdx_.load(std::memory_order_relaxed) & 1;
    const int nextTelIdx = (curTelIdx + 1) & 1;
    VisualTelemetrySnapshot& snap = telemetryBuffers_[nextTelIdx];
    const VisualTelemetrySnapshot& prevSnap = telemetryBuffers_[curTelIdx];

    const int curNeuralIdx = activeNeuralBufIdx_.load(std::memory_order_acquire) & 1;
    const NeuralEnvelopeSet& neuralEnv = neuralBuffers_[curNeuralIdx];

    const float inDb = 20.0f * std::log10(blockInPeak + kEps);
    const float outDb = 20.0f * std::log10(blockOutPeak + kEps);
    const float dynModDb = std::clamp((inDb + 28.0f) * 0.25f, -8.0f, 4.0f);

    if (neuralEnv.hasValidEnvelopes) {
        for (size_t b = 0; b < kScopeBins; ++b) {
            snap.sourceEnvelopeDb[b] = 0.82f * prevSnap.sourceEnvelopeDb[b] + 0.18f * (neuralEnv.dryScopeDb[b] + dynModDb * 0.5f);
            snap.slotAEnvelopeDb[b] = 0.82f * prevSnap.slotAEnvelopeDb[b] + 0.18f * neuralEnv.slotAScopeDb[b];
            snap.slotBEnvelopeDb[b] = 0.82f * prevSnap.slotBEnvelopeDb[b] + 0.18f * neuralEnv.slotBScopeDb[b];
            snap.injectedOutputDb[b] = 0.78f * prevSnap.injectedOutputDb[b] + 0.22f * (neuralEnv.rvcScopeOutDb[b] + dynModDb * 0.5f);
        }
    } else {
        for (size_t b = 0; b < kScopeBins; ++b) {
            const float fNorm = static_cast<float>(b) / static_cast<float>(kScopeBins - 1);
            const float baseDb = std::clamp(inDb - 14.0f - 12.0f * fNorm, -54.0f, 0.0f);
            snap.sourceEnvelopeDb[b] = 0.85f * prevSnap.sourceEnvelopeDb[b] + 0.15f * baseDb;
            snap.slotAEnvelopeDb[b] = prevSnap.slotAEnvelopeDb[b];
            snap.slotBEnvelopeDb[b] = prevSnap.slotBEnvelopeDb[b];
            snap.injectedOutputDb[b] = 0.85f * prevSnap.injectedOutputDb[b] + 0.15f * baseDb;
        }
    }

    snap.inputPeakDb = 0.75f * prevSnap.inputPeakDb + 0.25f * inDb;
    snap.outputPeakDb = 0.75f * prevSnap.outputPeakDb + 0.25f * outDb;
    snap.detectedPitchHz = cachedNeuralPitchHz_.load(std::memory_order_relaxed);
    snap.consonantShieldActivity = paramConsonantProtect_.load(std::memory_order_relaxed) * 2.0f;
    snap.inferenceLatencyMs = modelEngine_.getLastInferenceTimeMs();
    snap.cpuLoadPercent = 0.88f * prevSnap.cpuLoadPercent + 0.12f * cpuPercent;
    snap.latencySamples = getLatencySamples();
    snap.sampleRate = sampleRate_;

    const bool daemonBusy = modelEngine_.isDaemonBusy();
    const bool isSynth = currentlySynthesizing_.load(std::memory_order_relaxed) || daemonBusy;
    snap.isSynthesizing = isSynth;
    snap.isPlayingFromNeuralCache = blockUsedNeuralCache;
    snap.isDaemonReady = modelEngine_.isNativeDaemonReady();
    snap.isCacheLocked = true;
    snap.isPendingRegenerate = pendingRegenerateNotice_.load(std::memory_order_relaxed);
    snap.cachedChunksCount = currentCachedChunks_.load(std::memory_order_relaxed);
    snap.totalActiveChunks = currentTotalActiveChunks_.load(std::memory_order_relaxed);
    snap.cacheCoverageRatio = currentCacheCoverage_.load(std::memory_order_relaxed);
    snap.synthesisProgress = isSynth
                                 ? modelEngine_.getLoadingProgress() * 0.35f +
                                       currentSynthProgress_.load(std::memory_order_relaxed) * 0.65f
                                 : snap.cacheCoverageRatio;

    if (captureBlockAccepted) {
        appendComparedSamples_.fetch_add(appendCompared, std::memory_order_release);
        appendChangedSamples_.fetch_add(appendChanged, std::memory_order_release);
        capturePassStarted_ = true;
        capturePassExpectedSamplePos_ = hostPos + numSamples;
        captureBoundsGeneration_.store(captureGeneration, std::memory_order_release);
        capturedAudioGeneration_.store(captureGeneration, std::memory_order_release);
    }
    snap.statusBannerText[0] = '\0';
    activeTelemetryIdx_.store(nextTelIdx, std::memory_order_release);
}

void RvcFeatureInjector::inferenceWorkerLoop() {
    uint64_t lastParamHash = 0;
    auto lastParamChangeTime = std::chrono::steady_clock::now();

    std::vector<float> localDrySegment;
    std::vector<float> localDry16k;

    while (workerRunning_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();

        // 2. Synchronize any newly loaded or cleared RVC Slot A / Slot B models with the CUDA daemon
        modelEngine_.syncDaemonSlotsIfNeeded();
        if (!workerRunning_.load(std::memory_order_acquire)) break;

        const RvcModelSlotInfo slotAInfo = modelEngine_.getSlotInfo(0);
        const RvcModelSlotInfo slotBInfo = modelEngine_.getSlotInfo(1);
        slotALoaded_.store(slotAInfo.isLoaded, std::memory_order_relaxed);
        slotBLoaded_.store(slotBInfo.isLoaded, std::memory_order_relaxed);

        // 3. Strictly enforce Capture -> Generate -> Active workflow:
        //    Do NOT run background synthesis while in kIdle or kCapture!
        const uint64_t workerControl = timelineControl_.load(std::memory_order_seq_cst);
        const uint64_t captureRevision = workerControl >> 2;
        const TimelineState curState = static_cast<TimelineState>(workerControl & 3);
        auto commandStillCurrent = [&] { return timelineControl_.load(std::memory_order_acquire) == workerControl; };
        if (curState == TimelineState::kIdle || curState == TimelineState::kCapture) {
            currentlySynthesizing_.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }

        if (audioBlockInFlight_.load(std::memory_order_seq_cst)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        int64_t capStart, capEnd;
        getCaptureRange(capStart, capEnd);
        if (!commandStillCurrent()) continue;
        const uint32_t captureGeneration = static_cast<uint32_t>(workerControl >> 32);
        const size_t totalCapSamples = (capStart >= 0 && capEnd > capStart && maxCacheSamples_ > 0)
                                           ? static_cast<size_t>(std::min<int64_t>(
                                                 capEnd - capStart,
                                                 static_cast<int64_t>(maxCacheSamples_)))
                                           : 0;
        const size_t numCapturedChunks = (totalCapSamples > 0 && chunkLenSamples_ > 0)
                                             ? std::min<size_t>(
                                                   kMaxChunks,
                                                   (totalCapSamples + chunkLenSamples_ - 1) / chunkLenSamples_)
                                             : 0;

        if (numCapturedChunks == 0) {
            currentlySynthesizing_.store(false, std::memory_order_release);
            currentTotalActiveChunks_.store(0, std::memory_order_release);
            currentCachedChunks_.store(0, std::memory_order_release);
            currentCacheCoverage_.store(0.0f, std::memory_order_release);
            if (curState == TimelineState::kGenerating) {
                uint64_t expected = workerControl;
                timelineControl_.compare_exchange_strong(expected, (workerControl & ~uint64_t{3}) |
                    static_cast<uint64_t>(TimelineState::kIdle), std::memory_order_acq_rel);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }

        // 4. Compute synthesis parameter hash (Slot A/B index rates, model/octave revision, manual counter)
        const float slotAIndex = paramSlotAIndexRate_.load(std::memory_order_relaxed);
        const float slotBIndex = paramSlotBIndexRate_.load(std::memory_order_relaxed);
        const float protect = paramConsonantProtect_.load(std::memory_order_relaxed);
        const uint64_t modelRev = modelEngine_.getModelRevision();
        const uint64_t manualCnt = manualResynthCounter_.load(std::memory_order_acquire);

        const uint64_t curParamHash = computeBaseParameterHash(slotAIndex, slotBIndex, protect, modelRev, manualCnt);
        if (curParamHash != lastParamHash) {
            lastParamHash = curParamHash;
            lastParamChangeTime = now;
        }

        // 5. Scan ALL captured chunks [0 .. numCapturedChunks - 1] so 100% of captured audio is converted!
        //    Explicit Generate-Only Rule: While in kActive, already-rendered chunks are strictly locked
        //    and NEVER re-synthesized in the background unless the user explicitly clicks [GENERATE].
        const uint32_t totalActive = static_cast<uint32_t>(numCapturedChunks);
        uint32_t cachedUpToDate = 0;
        int nextChunkToSynth = -1;
        bool anyStaleInActive = false;

        for (size_t c = 0; c < numCapturedChunks; ++c) {
            const bool isReady = chunkStates_[c].wetReady.load(std::memory_order_acquire) &&
                chunkStates_[c].wetGeneration.load(std::memory_order_acquire) == captureRevision;
            const uint64_t cGen = chunkStates_[c].synthesizedGen.load(std::memory_order_acquire);
            if (curState == TimelineState::kActive) {
                if (isReady) {
                    ++cachedUpToDate;
                    if (cGen != curParamHash) {
                        anyStaleInActive = true;
                    }
                } else if (nextChunkToSynth < 0) {
                    nextChunkToSynth = static_cast<int>(c);
                }
            } else {
                const bool upToDate = isReady && (cGen == curParamHash);
                if (upToDate) {
                    ++cachedUpToDate;
                } else if (nextChunkToSynth < 0) {
                    nextChunkToSynth = static_cast<int>(c);
                }
            }
        }
        pendingRegenerateNotice_.store(anyStaleInActive, std::memory_order_relaxed);

        currentTotalActiveChunks_.store(totalActive, std::memory_order_release);
        currentCachedChunks_.store(cachedUpToDate, std::memory_order_release);
        currentCacheCoverage_.store(
            (totalActive > 0) ? (static_cast<float>(cachedUpToDate) / static_cast<float>(totalActive)) : 0.0f,
            std::memory_order_release);

        if (nextChunkToSynth < 0) {
            currentlySynthesizing_.store(false, std::memory_order_release);
            currentSynthProgress_.store(1.0f, std::memory_order_release);
            if (curState == TimelineState::kGenerating && cachedUpToDate >= totalActive) {
                uint64_t expected = workerControl;
                timelineControl_.compare_exchange_strong(expected, (workerControl & ~uint64_t{3}) |
                    static_cast<uint64_t>(TimelineState::kActive), std::memory_order_acq_rel);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            continue;
        }

        // 6. Coalesce consecutive uncached active chunks [cStart .. cEnd] (up to 5 chunks = 30.0s continuous pass,
        //    per D:\RVC20260718Nvidia\infer\vc\pipeline.py long-window context) so phrases are never cut mid-word.
        currentlySynthesizing_.store(true, std::memory_order_release);
        const size_t c = static_cast<size_t>(nextChunkToSynth);
        const size_t chunkStart = c * chunkLenSamples_;
        const size_t firstChunkLen = std::min<size_t>(
            chunkLenSamples_,
            (totalCapSamples > chunkStart) ? (totalCapSamples - chunkStart) : 0);

        // Logical chunk boundaries can move when a pass is prepended. Recompute off the audio thread.
        auto capturedPeak = [&](size_t chunkIndex) {
            const size_t begin = chunkIndex * chunkLenSamples_;
            const size_t end = std::min(totalCapSamples, begin + chunkLenSamples_);
            float peak = 0.0f;
            for (size_t i = begin; i < end; ++i)
                peak = std::max(peak, std::abs(readCapturedSample(i, captureGeneration, capStart)));
            return peak;
        };
        const float firstChunkPeak = capturedPeak(c);
        const bool anyModelLoaded = slotAInfo.isLoaded || slotBInfo.isLoaded;

        // Fast-path for pure digital silence chunks or when no model is loaded into either slot:
        // Populate wet cache directly so wetReady is ALWAYS true across the entire captured range.
        if (firstChunkLen == 0 || firstChunkPeak < 1e-5f || !anyModelLoaded) {
            if (!commandStillCurrent()) continue;
            for (size_t i = 0; i < firstChunkLen; ++i) {
                const float dryVal = (firstChunkPeak < 1e-5f)
                                         ? 0.0f
                                         : readCapturedSample(chunkStart + i, captureGeneration, capStart);
                wetSlotACache_[chunkStart + i].store(dryVal, std::memory_order_relaxed);
                wetSlotBCache_[chunkStart + i].store(dryVal, std::memory_order_relaxed);
            }
            if (!commandStillCurrent()) continue;
            chunkStates_[c].synthesizedGen.store(curParamHash, std::memory_order_release);
            chunkStates_[c].wetGeneration.store(captureRevision, std::memory_order_release);
            chunkStates_[c].wetReady.store(true, std::memory_order_release);
            const uint32_t newCached = std::min<uint32_t>(totalActive, cachedUpToDate + 1);
            currentCachedChunks_.store(newCached, std::memory_order_release);
            currentCacheCoverage_.store(static_cast<float>(newCached) / static_cast<float>(totalActive), std::memory_order_release);
            continue;
        }

        constexpr size_t kMaxCoalescedChunks = 2; // 2 * 6.0s = 12.0s continuous pass
        size_t cEnd = c;
        while ((cEnd + 1) < numCapturedChunks && (cEnd + 1 - c) < kMaxCoalescedChunks) {
            const size_t cand = cEnd + 1;
            const bool candReady = chunkStates_[cand].wetReady.load(std::memory_order_acquire) &&
                chunkStates_[cand].wetGeneration.load(std::memory_order_acquire) == captureRevision;
            const uint64_t candGen = chunkStates_[cand].synthesizedGen.load(std::memory_order_acquire);
            const bool candNeedsSynth = (curState == TimelineState::kActive)
                                            ? (!candReady)
                                            : (!candReady || candGen != curParamHash);
            const float candPeak = capturedPeak(cand);
            if (!candNeedsSynth || candPeak < 1e-5f) {
                break;
            }
            cEnd = cand;
        }

        const size_t numCoalesced = (cEnd - c) + 1;
        const size_t spanEndSample = std::min<size_t>(totalCapSamples, (cEnd + 1) * chunkLenSamples_);
        const size_t validChunkLen = (spanEndSample > chunkStart) ? (spanEndSample - chunkStart) : 0;

        // Include 50 frames (0.50s) left context overlap and 30 frames (0.30s) right context overlap,
        // strictly quantized to 10ms HuBERT/RMVPE frame boundaries so multi-span transitions are 100% phase-locked.
        const size_t leftOverlap = (c > 0)
                                       ? std::min<size_t>(chunkStart, static_cast<size_t>(std::llround(sampleRate_ * 0.50)))
                                       : 0;
        const size_t availRight = (totalCapSamples > chunkStart + validChunkLen)
                                      ? (totalCapSamples - (chunkStart + validChunkLen))
                                      : 0;
        const size_t rightOverlap = std::min<size_t>(availRight, static_cast<size_t>(std::llround(sampleRate_ * 0.30)));

        const size_t extractStart = chunkStart - leftOverlap;
        const size_t rawExtractLen = leftOverlap + validChunkLen + rightOverlap;
        const size_t minModelLen = static_cast<size_t>(sampleRate_ * 0.30); // Ensure >= 0.30s for HuBERT + RMVPE
        const size_t paddedExtractLen = std::max<size_t>(rawExtractLen, minModelLen);

        localDrySegment.assign(paddedExtractLen, 0.0f);
        for (size_t i = 0; i < rawExtractLen && (extractStart + i) < maxCacheSamples_; ++i) {
            localDrySegment[i] = readCapturedSample(extractStart + i, captureGeneration, capStart);
        }

        downsampleTo16kHz(localDrySegment.data(), paddedExtractLen, sampleRate_, localDry16k);

        RvcChunkSynthRequest req{};
        req.abMorph = 0.50f;
        req.injectDepth = 1.00f;
        req.pitchShiftSt = 0.0f;
        req.slotAPitchSt = static_cast<float>(slotAInfo.targetOctaveSemitones);
        req.slotBPitchSt = static_cast<float>(slotBInfo.targetOctaveSemitones);
        req.f0FilterRadius = 3.0f;
        req.indexRate = 1.00f;
        req.protect = protect;
        req.slotAIndexRate = slotAIndex;
        req.slotBIndexRate = slotBIndex;
        req.rmsMixRate = 0.0f; // 100% decoupled from P.04 real-time RMS knob
        req.targetSampleRate = static_cast<uint32_t>(std::llround(sampleRate_));

        // Preserve a recorded automation trajectory across span/context boundaries. A constant
        // capture continues to use the current knob value when the user clicks Generate again.
        bool hasRecordedIndexVariation = false;
        if (automationTimeline_) {
            float minA = 1.0f, maxA = 0.0f, minB = 1.0f, maxB = 0.0f;
            const int64_t firstFrame = automationFrameAt(capStart);
            const int64_t lastFrame = automationFrameAt(capEnd - 1);
            for (int64_t f = firstFrame; f <= lastFrame; ++f) {
                const auto& frame = automationTimeline_[static_cast<size_t>(f % kAutomationCacheFrames)];
                if (frame.generation.load(std::memory_order_acquire) != captureGeneration ||
                    frame.projectFrame.load(std::memory_order_relaxed) != f) continue;
                const float a = frame.slotAIndexRate.load(std::memory_order_relaxed);
                const float b = frame.slotBIndexRate.load(std::memory_order_relaxed);
                minA = std::min(minA, a); maxA = std::max(maxA, a);
                minB = std::min(minB, b); maxB = std::max(maxB, b);
            }
            hasRecordedIndexVariation = (maxA - minA > 1e-5f) || (maxB - minB > 1e-5f);
        }
        if (hasRecordedIndexVariation) {
            const size_t fCount = std::max<size_t>(1, (localDry16k.size() + 159) / 160);
            req.automationCurves4Col.resize(fCount * 4);
            for (size_t f = 0; f < fCount; ++f) {
                const int64_t projectFrame = automationFrameAt(capStart + static_cast<int64_t>(extractStart) +
                    static_cast<int64_t>(std::llround(f * sampleRate_ / kAutoFrameHz)));
                const auto& frame = automationTimeline_[static_cast<size_t>(projectFrame % kAutomationCacheFrames)];
                const bool valid = frame.generation.load(std::memory_order_acquire) == captureGeneration &&
                    frame.projectFrame.load(std::memory_order_relaxed) == projectFrame;
                const float a = valid ? frame.slotAIndexRate.load(std::memory_order_relaxed) : slotAIndex;
                const float b = valid ? frame.slotBIndexRate.load(std::memory_order_relaxed) : slotBIndex;
                req.automationCurves4Col[f * 4 + 0] = 0.50f;
                req.automationCurves4Col[f * 4 + 1] = 1.00f;
                req.automationCurves4Col[f * 4 + 2] = a;
                req.automationCurves4Col[f * 4 + 3] = b;
            }
        }

        RvcChunkSynthResult res{};
        const float baseBatchProg = (totalActive > 0)
                                        ? (static_cast<float>(cachedUpToDate) / static_cast<float>(totalActive))
                                        : 0.0f;
        const float spanBatchProg = (totalActive > 0)
                                        ? (static_cast<float>(numCoalesced) / static_cast<float>(totalActive))
                                        : 1.0f;

        if (!workerRunning_.load(std::memory_order_acquire)) break;
        const bool ok = modelEngine_.synthesizeRvcChunk(
            localDry16k.data(),
            localDry16k.size(),
            req,
            res,
            [&](float chunkP, const std::string& /*msg*/) {
                currentSynthProgress_.store(
                    std::clamp(baseBatchProg + spanBatchProg * chunkP, 0.0f, 1.0f),
                    std::memory_order_relaxed);
            });

        if (!commandStillCurrent()) continue;

        if (ok && res.slotAAudioSamples.size() > leftOverlap && res.slotBAudioSamples.size() > leftOverlap) {
            const size_t outAvail = std::min(res.slotAAudioSamples.size(), res.slotBAudioSamples.size()) - leftOverlap;
            const size_t copyLen = std::min<size_t>(validChunkLen, outAvail);
            const float* synthAPtr = res.slotAAudioSamples.data() + leftOverlap;
            const float* synthBPtr = res.slotBAudioSamples.data() + leftOverlap;

            // First-Principles Seamless Overlap-Add Segment Stitching:
            // 1. If c > 0 and the previous chunk (c - 1) is already populated in cache, crossfade over
            //    [chunkStart - boundaryFade, chunkStart) using chunk c's synthesized pre-roll (`leftOverlap`).
            const size_t boundaryFade = std::min<size_t>(copyLen / 4, static_cast<size_t>(sampleRate_ * 0.060));
            const bool prevChunkReady = (c > 0) && chunkStates_[c - 1].wetReady.load(std::memory_order_acquire) &&
                chunkStates_[c - 1].wetGeneration.load(std::memory_order_acquire) == captureRevision;
            float polarityA = 1.0f, polarityB = 1.0f;
            if (prevChunkReady && leftOverlap >= boundaryFade && chunkStart >= boundaryFade && boundaryFade > 0) {
                const float* preRollAPtr = res.slotAAudioSamples.data() + (leftOverlap - boundaryFade);
                const float* preRollBPtr = res.slotBAudioSamples.data() + (leftOverlap - boundaryFade);
                const size_t fadeStartIdx = chunkStart - boundaryFade;
                // Independent VITS passes need not have the same polarity. Align it without
                // shifting project samples, then normalize only the overlap's correlation loss.
                auto alignPolarity = [&](const std::atomic<float>* cache, const float* incoming,
                                         float& polarity) {
                    double previousEnergy = 0, incomingEnergy = 0, dot = 0;
                    for (size_t k = 0; k < boundaryFade; ++k) {
                        if (!hasCapturedSample(fadeStartIdx + k, captureGeneration, capStart)) continue;
                        const double previous = cache[fadeStartIdx + k].load(std::memory_order_relaxed);
                        const double next = incoming[k];
                        previousEnergy += previous * previous;
                        incomingEnergy += next * next;
                        dot += previous * next;
                    }
                    polarity = dot < 0 ? -1.0f : 1.0f;
                    const double denominator = std::sqrt(previousEnergy * incomingEnergy);
                    return denominator > 1e-12
                        ? std::clamp(static_cast<float>(std::abs(dot) / denominator), 0.0f, 1.0f) : 1.0f;
                };
                const float correlationA = alignPolarity(wetSlotACache_.get(), preRollAPtr, polarityA);
                const float correlationB = alignPolarity(wetSlotBCache_.get(), preRollBPtr, polarityB);
                auto overlapGain = [](float weight, float correlation) {
                    return 1.0f / std::sqrt(1.0f - 2.0f * (1.0f-correlation) * weight * (1.0f-weight));
                };
                for (size_t k = 0; k < boundaryFade; ++k) {
                    const float w = 0.5f * (1.0f - std::cos(kPi * static_cast<float>(k + 1) / static_cast<float>(boundaryFade)));
                    const float prevA = wetSlotACache_[fadeStartIdx + k].load(std::memory_order_relaxed);
                    const float prevB = wetSlotBCache_[fadeStartIdx + k].load(std::memory_order_relaxed);
                    const bool captured = hasCapturedSample(fadeStartIdx + k, captureGeneration, capStart);
                    wetSlotACache_[fadeStartIdx + k].store(captured
                        ? ((1.0f-w)*prevA + w*polarityA*preRollAPtr[k])*overlapGain(w, correlationA) : 0.0f, std::memory_order_relaxed);
                    wetSlotBCache_[fadeStartIdx + k].store(captured
                        ? ((1.0f-w)*prevB + w*polarityB*preRollBPtr[k])*overlapGain(w, correlationB) : 0.0f, std::memory_order_relaxed);
                }
            }

            // 2. Write [chunkStart, chunkStart + copyLen) at 100% full amplitude (only crossfading if
            //    `hadPrevWet` is true during an in-place re-synthesis over an already-populated chunk c).
            const bool hadPrevWet = chunkStates_[c].wetReady.load(std::memory_order_acquire) &&
                chunkStates_[c].wetGeneration.load(std::memory_order_acquire) == captureRevision;
            for (size_t i = 0; i < copyLen; ++i) {
                float sa = polarityA * synthAPtr[i];
                float sb = polarityB * synthBPtr[i];
                if (hadPrevWet && i < boundaryFade && boundaryFade > 0) {
                    const float w = 0.5f * (1.0f - std::cos(kPi * static_cast<float>(i) / static_cast<float>(boundaryFade)));
                    sa = (1.0f - w) * wetSlotACache_[chunkStart + i].load(std::memory_order_relaxed) + w * sa;
                    sb = (1.0f - w) * wetSlotBCache_[chunkStart + i].load(std::memory_order_relaxed) + w * sb;
                }
                // Neural context can emit audio for silence; uncaptured holes must remain empty.
                const bool captured = hasCapturedSample(chunkStart + i, captureGeneration, capStart);
                wetSlotACache_[chunkStart + i].store(captured ? sa : 0.0f, std::memory_order_relaxed);
                wetSlotBCache_[chunkStart + i].store(captured ? sb : 0.0f, std::memory_order_relaxed);
            }
            // Smoothly hold/decay any rounding tail samples so 100% of validChunkLen is populated without steps
            const float lastA = (copyLen > 0) ? polarityA * synthAPtr[copyLen - 1] : 0.0f;
            const float lastB = (copyLen > 0) ? polarityB * synthBPtr[copyLen - 1] : 0.0f;
            const size_t remTail = (validChunkLen > copyLen) ? (validChunkLen - copyLen) : 0;
            for (size_t i = copyLen; i < validChunkLen; ++i) {
                const float decay = (remTail > 1)
                                        ? (1.0f - static_cast<float>(i - copyLen) / static_cast<float>(remTail))
                                        : 0.0f;
                const bool captured = hasCapturedSample(chunkStart + i, captureGeneration, capStart);
                wetSlotACache_[chunkStart + i].store(captured ? lastA * decay : 0.0f, std::memory_order_relaxed);
                wetSlotBCache_[chunkStart + i].store(captured ? lastB * decay : 0.0f, std::memory_order_relaxed);
            }

            if (!commandStillCurrent()) continue;
            for (size_t k = c; k <= cEnd; ++k) {
                chunkStates_[k].synthesizedGen.store(curParamHash, std::memory_order_release);
                chunkStates_[k].wetGeneration.store(captureRevision, std::memory_order_release);
                chunkStates_[k].wetReady.store(true, std::memory_order_release);
            }
            cachedNeuralPitchHz_.store(res.detectedPitchHz, std::memory_order_relaxed);

            // Publish authentic RVC 64-bin spectral envelopes
            const int curN = activeNeuralBufIdx_.load(std::memory_order_relaxed) & 1;
            const int nextN = (curN + 1) & 1;
            neuralBuffers_[nextN].dryScopeDb = res.dryEnvDb;
            neuralBuffers_[nextN].slotAScopeDb = res.slotAEnvDb;
            neuralBuffers_[nextN].slotBScopeDb = res.slotBEnvDb;
            neuralBuffers_[nextN].rvcScopeOutDb = res.injectedEnvDb;
            neuralBuffers_[nextN].hasValidEnvelopes = true;
            activeNeuralBufIdx_.store(nextN, std::memory_order_release);

            const uint32_t newCached = std::min<uint32_t>(totalActive, cachedUpToDate + static_cast<uint32_t>(numCoalesced));
            currentCachedChunks_.store(newCached, std::memory_order_release);
            currentCacheCoverage_.store(
                (totalActive > 0) ? (static_cast<float>(newCached) / static_cast<float>(totalActive)) : 1.0f,
                std::memory_order_release);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace rvc_vst
