#pragma once

#include "LockFreeRingBuffer.h"
#include "../inference/RvcModelEngine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rvc_vst {

static constexpr size_t kScopeBins = 64;
static constexpr size_t kWaveformBins = 256;

/**
 * Pure RVC v1/v2 Dual-Basis Neural Parameters + Real-Time Zero-Latency Effector Matrix.
 *
 * Architectural Separation for 100% Real-Time Effector Response:
 * - Neural Basis Parameters (`slotAIndexRate`, `slotBIndexRate`, `consonantProtect`): Drive the background CUDA
 *   HuBERT + RMVPE + FAISS + Dual-Slot VITS/NSF-HiFiGAN basis cache (`wetSlotACache_`, `wetSlotBCache_`).
 * - Real-Time Effector Parameters (`abMorph`, `injectDepth`, pitch, formant, air, RMS and dry/wet):
 *   Computed 100% in real-time C++ inside `processBlock()` (`< 0.05ms`) on top of the cached
 *   dual RVC basis streams without ever invalidating the cache or triggering GPU re-calculation.
 */
struct RvcVoiceParameters {
    float abMorph = 0.35f;                  // 0.0 (100% Slot A) .. 1.0 (100% Slot B) [Real-Time]
    float injectDepth = 1.00f;              // 0.0 (Dry Vocal) .. 1.0 (100% RVC Timbre) [Bi-directionally linked with dryWetMix]
    float pitchShiftSemitones = 0.0f;       // P.01: -12.0 .. +12.0 ST Real-Time Dual-Grain Pitch Shifter
    float throatFormantSemitones = 0.0f;    // P.02: -6.0 .. +6.0 ST Real-Time 3-Band Vocal Tract Formant Shifter
    float presenceAirDb = 0.0f;             // P.03: -6.0 .. +6.0 dB Real-Time Harmonic Air & Presence Exciter
    float consonantProtect = 0.33f;         // P.04: RVC unvoiced consonant protection (0 strongest .. 0.50 off), requires Generate
    float rmsMixRate = 0.25f;               // P.05: 0.00 .. 1.00 Real-Time Sliding RMS Envelope Matcher
    float dryWetMix = 1.00f;                // P.06: 0.00 .. 1.00 Real-Time Parallel Dry/Wet Output Mix
    float slotAIndexRate = 0.75f;           // Slot A FAISS .index retrieval ratio (0.0 .. 1.0)
    float slotBIndexRate = 0.75f;           // Slot B FAISS .index retrieval ratio (0.0 .. 1.0)
    float outputGainDb = 0.0f;              // P.07: -12.0 .. +12.0 dB Real-Time Output Gain Trim
    bool bypass = false;                    // Master zero-latency bypass
};

struct VisualTelemetrySnapshot {
    std::array<float, kScopeBins> sourceEnvelopeDb{};
    std::array<float, kScopeBins> slotAEnvelopeDb{};
    std::array<float, kScopeBins> slotBEnvelopeDb{};
    std::array<float, kScopeBins> injectedOutputDb{};
    float inputPeakDb = -60.0f;
    float outputPeakDb = -60.0f;
    float detectedPitchHz = 0.0f;
    float consonantShieldActivity = 0.0f; // 0.0 .. 1.0
    float inferenceLatencyMs = 0.0f;
    float cpuLoadPercent = 0.5f;
    uint32_t latencySamples = 0;
    double sampleRate = 48000.0;

    // Native RVC Neural Cache & Progress Telemetry
    float synthesisProgress = 1.0f;      // 0.0 .. 1.0 (Drives UI Pre-Processing Progress Bar)
    float cacheCoverageRatio = 0.0f;     // 0.0 .. 1.0 (Ratio of captured vocal timeline ready)
    uint32_t cachedChunksCount = 0;
    uint32_t totalActiveChunks = 0;
    bool isSynthesizing = false;
    bool isPlayingFromNeuralCache = false;
    bool isDaemonReady = false;
    bool isCacheLocked = true;
    bool isPendingRegenerate = false;
    char statusBannerText[128]{};

    // Capture -> Generate -> Active 3-State Workflow Telemetry
    uint32_t timelineStateVal = 0; // 0=Idle, 1=Capture, 2=Generating, 3=Active
    int64_t captureStartSample = -1;
    int64_t captureEndSample = -1;
    bool isCapturingNow = false;
    bool isAppendCapture = false;
    bool captureCapacityExceeded = false;
    uint64_t appendComparedSamples = 0;
    uint64_t appendChangedSamples = 0;
    bool appendReusedCache = false;

    // Captured Time-Domain Waveform & Model Slot Presence Telemetry
    std::array<float, kWaveformBins> capturedWaveformPeak{};
    std::array<float, kWaveformBins> capturedWaveformRms{};
    float capturedDurationSec = 0.0f;
    float playheadRelSec = 0.0f;
    bool slotALoaded = false;
    bool slotBLoaded = false;
};

/**
 * Three-state timeline locking workflow enum:
 * kIdle: Direct dry pass-through, zero processing.
 * kCapture: Records incoming dry audio & projectTimeSamples from Cubase transport into dryTimelineCache_.
 * kGenerating: CUDA background worker synthesizes RVC neural features for captured chunks.
 * kActive: Sample-accurate neural cache replay inside [captureStartSample, captureEndSample] with 5ms crossfade.
 */
enum class TimelineState : uint32_t {
    kIdle = 0,
    kCapture = 1,
    kGenerating = 2,
    kActive = 3
};

/**
 * RvcFeatureInjector
 *
 * Dual-Basis Phase-Locked RVC v1/v2 Timeline Cache & Real-Time Vocal Effector Engine:
 * 1. Captures dry vocal audio along the Cubase project timeline (`projectTimeSamples`), or
 *    automatically discovers and loads the Cubase track's `.wav` file from the project `Audio/`
 *    folder starting at sample 0.
 * 2. Background CUDA worker synthesizes two phase-aligned RVC neural basis streams (`wetSlotACache_`
 *    for Slot A and `wetSlotBCache_` for Slot B) using shared VITS prior noise.
 * 3. `processBlock()` performs 100% real-time, zero-latency C++ processing for A/B Timbre Morph,
 *    Feature Injection Depth, Pitch Shift, Throat Formant, Presence Air,
 *    RMS Envelope Matching, and Dry/Wet Mix — responding instantaneously to knob turns and DAW
 *    automation without invalidating the neural cache. Neural consonant protection takes
 *    effect on the next explicit Generate, together with model and retrieval changes.
 */
class RvcFeatureInjector {
public:
    static constexpr size_t kMaxCacheSeconds = 240; // 4 minutes full-track timeline cache
    static constexpr size_t kChunkSeconds = 6;      // 6.0s long coherent phrase segments
    static constexpr size_t kMaxChunks = kMaxCacheSeconds / kChunkSeconds; // 40 long segments
    static constexpr size_t kAutoFrameHz = 100;     // 10ms frame resolution (matches 100fps HuBERT)
    static constexpr size_t kMaxAutoFrames = kMaxCacheSeconds * kAutoFrameHz;

    explicit RvcFeatureInjector(RvcModelEngine& modelEngine);
    ~RvcFeatureInjector();

    RvcFeatureInjector(const RvcFeatureInjector&) = delete;
    RvcFeatureInjector& operator=(const RvcFeatureInjector&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    uint32_t getLatencySamples() const noexcept {
        return 0; // Pitch reads centered grains from the complete generated cache, including lookahead.
    }

    void updateParameters(const RvcVoiceParameters& params) noexcept;
    RvcVoiceParameters getParameters() const noexcept;

    /**
     * Records a sample-accurate parameter change from the DAW automation queue
     * at a specific sample offset within the current audio block.
     */
    void pushAutomationPoint(uint32_t paramId, int32_t sampleOffset, float plainValue) noexcept;

    /**
     * Sets host channel context provided by Steinberg VST3 ChannelContext::IInfoListener.
     */
    void setHostTrackContext(const std::wstring& trackName, int32_t channelIndex);

    /**
     * Returns the host track context name if provided by host.
     */
    std::wstring getHostTrackName() const;

    /**
     * Sets timeline workflow state (kIdle, kCapture, kGenerating, kActive).
     */
    void setTimelineState(TimelineState state) noexcept;

    // Arm a new pass without clearing previously captured audio or automation.
    void beginAppendCapture() noexcept;

    /**
     * Returns the current timeline workflow state.
     */
    TimelineState getTimelineState() const noexcept;

    /**
     * Retrieves the sample bounds of the currently captured phrase.
     */
    void getCaptureRange(int64_t& startSample, int64_t& endSample) const noexcept;

    /**
     * Sets the sample bounds of the captured phrase (e.g. from VST3 state deserialization).
     */
    void setCaptureRange(int64_t startSample, int64_t endSample) noexcept;

    /**
     * Clears any active capture range and resets timeline state to kIdle.
     */
    void clearCapture() noexcept;

    /**
     * Triggers neural synthesis on the captured phrase, switching to kGenerating.
     */
    void triggerGenerate() noexcept;

    /**
     * Triggers an immediate re-synthesis of all captured timeline vocal segments
     * using the current RVC models and FAISS index retrieval rates.
     */
    void triggerResynthesize() noexcept;

    /**
     * Clears all captured dry/wet timeline audio segments and automation curves.
     */
    void clearTimelineCache() noexcept;

    /**
     * Waits until the background CUDA worker finishes synthesizing all currently
     * captured vocal segments (used by offline verification tests).
     */
    bool waitForSynthesisIdle(uint32_t timeoutMs = 20000);

    /**
     * Real-time audio callback (called from Cubase processBlock).
     * Lock-free, allocation-free, deterministic execution time.
     */
    void processBlock(
        const float* inLeft,
        const float* inRight,
        float* outLeft,
        float* outRight,
        int numSamples,
        int numChannels,
        int64_t timelineSamplePos = -1,
        bool isTransportPlaying = true) noexcept;

    /**
     * Reads the latest lock-free visual telemetry snapshot for 60 FPS UI rendering.
     */
    VisualTelemetrySnapshot getVisualTelemetry() const noexcept;

private:
    friend struct RvcCaptureTestPeer;
    void inferenceWorkerLoop();
    void applyAutomationPoint(uint32_t paramId, float plainValue) noexcept;
    bool hasCapturedSample(size_t index, uint32_t generation, int64_t startSample) const noexcept;
    float readCapturedSample(size_t index, uint32_t generation, int64_t startSample) const noexcept;
    int64_t automationFrameAt(int64_t sample) const noexcept;
    void lockCaptureControl() noexcept;
    void clearTimelineCacheLocked() noexcept;
    void setTimelineCommand(TimelineState state, bool invalidate, bool resetCapture = false) noexcept;
    void applyCaptureGeneration(uint32_t generation) noexcept;
    void configureCaptureArm(bool resetCapture) noexcept;

    RvcModelEngine& modelEngine_;
    double sampleRate_ = 48000.0;

    // Atomic parameters shared between UI/Controller, Audio Thread, and Worker Thread
    std::atomic<float> paramAbMorph_{0.35f};
    std::atomic<float> paramInjectDepth_{1.00f};
    std::atomic<float> paramPitchShiftSt_{0.0f};
    std::atomic<float> paramThroatFormantSt_{0.0f};
    std::atomic<float> paramPresenceAirDb_{0.0f};
    std::atomic<float> paramConsonantProtect_{0.33f};
    std::atomic<float> paramRmsMixRate_{0.25f};
    std::atomic<float> paramDryWetMix_{1.00f};
    std::atomic<float> paramSlotAIndexRate_{0.75f};
    std::atomic<float> paramSlotBIndexRate_{0.75f};
    std::atomic<float> paramOutputGainDb_{0.0f};
    std::atomic<bool> paramBypass_{false};

    // Smoothed audio-rate parameters for click-free real-time knob turns and DAW automation
    float smoothAbMorph_ = 0.35f;
    float smoothInjectDepth_ = 1.00f;
    float smoothPitchSt_ = 0.0f;
    float smoothThroatSt_ = 0.0f;
    float smoothPresenceDb_ = 0.0f;
    float smoothProtect_ = 0.33f;
    float smoothRmsMix_ = 0.25f;
    float smoothDryWet_ = 1.00f;
    float smoothSlotAIndex_ = 0.75f;
    float smoothSlotBIndex_ = 0.75f;
    float smoothOutputGainDb_ = 0.0f;

    // Real-time DSP state variables (Audio Thread only — zero allocation)
    // Soundtoys Little AlterBoy 3-Stage Pitch Shifter (`wmPitchDetect` + `wmGrainShift` + `wmGrainShiftVoice`)
    static constexpr size_t kPitchBufSize = 8192;
    static constexpr size_t kPitchBufMask = kPitchBufSize - 1;
    static constexpr size_t kEpochTableSize = 64;
    static constexpr size_t kEpochTableMask = kEpochTableSize - 1;
    std::array<float, kPitchBufSize> pitchLpBuf_{};
    size_t pitchWritePos_ = 0;
    uint64_t pitchTotalSamples_ = 0;

    // Stage 1: `wmPitchDetect` (Decimation LPF + Autocorrelation/CMNDF + 5-Point Median Filter)
    float pdaLpfState1_ = 0.0f;
    float pdaLpfState2_ = 0.0f;
    std::array<float, 5> pdaMedianTable_{200.0f, 200.0f, 200.0f, 200.0f, 200.0f};
    int pdaMedianIdx_ = 0;
    float trackedPeriodSamples_ = 200.0f;
    float pitchLpfState_ = 200.0f;
    int pdaTrackCount_ = 0;
    bool pdaVoiced_ = false;
    int pdaChunkCounter_ = 0;

    // Stage 2: `wmGrainShift` (Coupled Goertzel Fundamental Resonator & `epochTable`)
    float goertzelRe_ = 0.0f;
    float goertzelIm_ = 0.0f;
    float prevGoertzelRe_ = 0.0f;
    std::array<double, kEpochTableSize> epochTable_{};
    size_t epochWriteIdx_ = 0;
    size_t epochCount_ = 0;
    double currentEpochLoc_ = 0.0;
    double prevEpochLoc_ = -200.0;
    float epochSpacing_ = 200.0f;

    // Stage 3: `wmGrainShiftVoice` (Dual-Motif Epoch-Snapped Splicer + `wmDCBlock`)
    float pitchShiftCrossfade_ = 0.0f;
    float smoothPeriodRatio_ = 1.0f;
    float motif1Delay_ = 320.0f;
    float motif2Delay_ = 320.0f;
    float motif1Age_ = 0.0f;
    float motif2Age_ = 0.0f;
    float splicePhase_ = 1.0f;
    float spliceStep_ = 1.0f / 48.0f;
    float epochTimer_ = 0.0f;
    bool prevVoicedState_ = false;
    float dcBlockX1_ = 0.0f;
    float dcBlockY1_ = 0.0f;
    float pitchInEnv_ = 0.001f;
    float pitchOutEnv_ = 0.001f;
    int pitchSilentSamples_ = 1000000;
    int pitchTransientSamples_ = 0;

    struct SvfBandState {
        float ic1eq = 0.0f;
        float ic2eq = 0.0f;
    };

    // 4-band P.02 Throat Formant warping filterbank (520Hz, 1420Hz, 2550Hz, 3750Hz)
    std::array<SvfBandState, 4> svfBase_{};
    std::array<SvfBandState, 4> svfShifted_{};
    float formantTiltLp_ = 0.0f;
    float presenceShelfLp_ = 0.0f;
    float presenceShelfLp2_ = 0.0f;
    std::array<float, 4> drySibilanceLp_{};
    std::array<float, 4> wetSibilanceLp_{};
    float dryHfEnv_ = 0.0f;
    float dryLowEnv_ = 0.0f;
    float dryEnvFollower_ = 0.0f;
    float wetEnvFollower_ = 0.0f;
    float smoothEnvGain_ = 1.0f;
    float neuralCacheCrossfade_ = 0.0f;
    int64_t lastTimelineSamplePos_ = -1;
    bool lastTransportWasPlaying_ = false;
    uint64_t lastAudioTimelineControl_ = 0;
    bool capturePassStarted_ = false;
    int64_t capturePassExpectedSamplePos_ = -1;
    int64_t capturePassStartFloor_ = -1; // Audio-thread copy, retained through a seek/reset.
    std::atomic<int64_t> latestStoppedProjectSample_{-1};
    std::atomic<int64_t> captureArmStartFloor_{-1};
    std::atomic<uint64_t> captureArmRevision_{0};
    int64_t expectedTimelineSamplePos_ = -1;
    int64_t lastActiveRelEnd_ = -1;
    float transportPlayGate_ = 0.0f;
    bool dspStateFlushedOnPause_ = true;

    std::atomic<bool> slotALoaded_{false};
    std::atomic<bool> slotBLoaded_{false};

    // Pre-allocated Full-Track Dual-Basis Timeline Audio Cache (Lock-free)
    size_t chunkLenSamples_ = 288000;     // 6.0s at 48kHz
    size_t maxCacheSamples_ = 0;          // 240s at host sample rate (set in prepare)
    std::unique_ptr<std::atomic<float>[]> dryTimelineCache_;
    std::unique_ptr<std::atomic<float>[]> wetSlotACache_;
    std::unique_ptr<std::atomic<float>[]> wetSlotBCache_;
    // A generation plus 64 validity bits isolates holes without clearing 240s on the audio thread.
    struct CaptureValidity {
        std::atomic<uint32_t> generation{0};
        std::atomic<uint64_t> mask{0};
    };
    std::unique_ptr<CaptureValidity[]> captureValidity_;
    // Only the audio thread applies dry generations and writes capture bounds.
    std::atomic<uint32_t> captureGeneration_{1};
    std::atomic<uint32_t> captureBoundsGeneration_{0};
    std::atomic<uint32_t> capturedAudioGeneration_{0};
    std::atomic<bool> audioBlockInFlight_{false};

    // Host queues are delivered on the audio thread before processBlock. One last value per
    // parameter/sample is sufficient, even when the host sends duplicate automation points.
    struct BlockAutomationFrame {
        std::array<float, 12> values{};
        uint16_t mask = 0;
    };
    std::unique_ptr<BlockAutomationFrame[]> blockAutomation_;
    size_t blockAutomationCapacity_ = 0;

    // 10ms Frame-Level Automation Timeline (100Hz, synchronized with HuBERT frame rate)
    struct AutomationTimelineFrame {
        std::atomic<uint32_t> generation{0};
        std::atomic<int64_t> projectFrame{-1};
        std::atomic<float> abMorph{0.35f};
        std::atomic<float> injectDepth{1.00f};
        std::atomic<float> slotAIndexRate{0.75f};
        std::atomic<float> slotBIndexRate{0.75f};
    };
    // One extra frame preserves both partial boundary frames in a 240s span.
    static constexpr size_t kAutomationCacheFrames = kMaxAutoFrames + 1;
    std::unique_ptr<AutomationTimelineFrame[]> automationTimeline_;

    struct TimelineChunkState {
        std::atomic<uint64_t> wetGeneration{0};
        std::atomic<uint32_t> capturedCount{0};
        std::atomic<float> maxAbsPeak{0.0f};
        std::atomic<uint64_t> synthesizedGen{0};
        std::atomic<bool> wetReady{false};
    };
    std::array<TimelineChunkState, kMaxChunks> chunkStates_{};
    std::atomic<int64_t> timelineOriginSample_{0};
    std::atomic<int64_t> internalTimelineCursor_{0};
    std::atomic<int64_t> latestPlayheadRelSample_{0};
    std::atomic<bool> hostIsCurrentlyPlaying_{false};
    std::atomic<uint64_t> manualResynthCounter_{1};
    std::atomic<bool> pendingRegenerateNotice_{false};

    // Three-State Timeline Locking Variables
    // High 32 bits: dry generation; low 32: revision and state, published as one command.
    std::atomic<uint64_t> timelineControl_{uint64_t{1} << 32};
    std::atomic_flag captureControlBusy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> appendCapture_{false};
    std::atomic<bool> appendCacheReusable_{false};
    std::atomic<bool> appendAutomationChanged_{false};
    std::atomic<uint64_t> appendComparedSamples_{0};
    std::atomic<uint64_t> appendChangedSamples_{0};
    std::atomic<bool> appendReusedCache_{false};
    int64_t appendBaseStart_ = -1;
    int64_t appendBaseEnd_ = -1;
    uint64_t appendBaseRevision_ = 0;
    uint64_t appendBaseParameterHash_ = 0;
    std::atomic<bool> captureCapacityExceeded_{false};
    std::atomic<int64_t> captureStartSample_{-1};
    std::atomic<int64_t> captureEndSample_{-1};
    std::atomic<bool> isCapturingNow_{false};

    // Host Track Context (for UI display)
    mutable std::mutex hostContextMutex_;
    std::wstring hostTrackName_;
    int32_t hostChannelIndex_ = -1;

    // Cached neural telemetry from the most recently synthesized/played RVC segment
    std::atomic<float> cachedNeuralPitchHz_{0.0f};
    std::atomic<float> currentSynthProgress_{1.0f};
    std::atomic<float> currentCacheCoverage_{0.0f};
    std::atomic<uint32_t> currentCachedChunks_{0};
    std::atomic<uint32_t> currentTotalActiveChunks_{0};
    std::atomic<bool> currentlySynthesizing_{false};
    std::atomic<bool> playingFromNeuralCache_{false};

    // Double-buffered lock-free 64-bin spectral envelopes from RVC neural synthesis
    struct NeuralEnvelopeSet {
        std::array<float, kScopeBins> dryScopeDb{};
        std::array<float, kScopeBins> slotAScopeDb{};
        std::array<float, kScopeBins> slotBScopeDb{};
        std::array<float, kScopeBins> rvcScopeOutDb{};
        bool hasValidEnvelopes = false;
    };
    std::array<NeuralEnvelopeSet, 2> neuralBuffers_{};
    std::atomic<int> activeNeuralBufIdx_{0};

    // Double-buffered Visual Telemetry Snapshot for 60 FPS UI
    std::array<VisualTelemetrySnapshot, 2> telemetryBuffers_{};
    std::atomic<int> activeTelemetryIdx_{0};

    // Background RVC Neural Synthesis Worker Thread
    std::atomic<bool> workerRunning_{false};
    std::thread workerThread_;
};

} // namespace rvc_vst
