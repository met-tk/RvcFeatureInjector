#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rvc_vst {

static constexpr size_t kRvcFeatureDim = 128;
static constexpr size_t kMaxCentroids = 64;

struct RvcModelSlotInfo {
    bool isLoaded = false;
    bool hasOnnxSession = false;
    bool hasFeatureIndex = false;
    uint32_t sampleRate = 40000;
    uint32_t rvcVersion = 2;
    std::string modelName = "UNLOADED";
    std::string filePath;
    std::string indexPath;
    std::string archTag = "NONE";
    uint32_t centroidCount = 0;
    uint32_t featureDim = 768;
    int32_t targetOctaveSemitones = 0; // -12 (Female->Male), 0 (Same Register), +12 (Male->Female)
    std::array<float, kRvcFeatureDim> baseFormantDb{};
    std::array<float, kRvcFeatureDim> speakerEmbedding{};
};

struct RvcChunkSynthRequest {
    float abMorph = 0.35f;
    float injectDepth = 1.00f;
    float pitchShiftSt = 0.0f;
    float slotAPitchSt = 0.0f;
    float slotBPitchSt = 0.0f;
    float f0FilterRadius = 3.0f;
    float indexRate = 0.75f;
    float protect = 0.33f;
    float slotAIndexRate = 0.75f;
    float slotBIndexRate = 0.75f;
    float rmsMixRate = 0.25f;
    uint32_t targetSampleRate = 48000;
    // Optional 10ms frame-level automation curves: 4 floats per frame
    // [abMorph, injectDepth, slotAIndexRate, slotBIndexRate]
    std::vector<float> automationCurves4Col;
};

struct RvcChunkSynthResult {
    std::vector<float> slotAAudioSamples;
    std::vector<float> slotBAudioSamples;
    float detectedPitchHz = 0.0f;
    float inferenceMs = 0.0f;
    std::array<float, 64> dryEnvDb{};
    std::array<float, 64> slotAEnvDb{};
    std::array<float, 64> slotBEnvDb{};
    std::array<float, 64> injectedEnvDb{};
};

/**
 * RvcModelEngine
 *
 * Manages the Native RVC v1/v2 Neural Synthesis Daemon (HuBERT Base + RMVPE +
 * FAISS .index + SynthesizerTrnMs768NSFsid VITS/NSF-HiFiGAN on CUDA) over
 * cancellable binary pipes. Both slots start unloaded.
 */
class RvcModelEngine {
public:
    RvcModelEngine();
    ~RvcModelEngine();

    RvcModelEngine(const RvcModelEngine&) = delete;
    RvcModelEngine& operator=(const RvcModelEngine&) = delete;

    /**
     * Locates the worker/runtime and optional Resources/state-root.txt for managed
     * installation caches and diagnostics (absolute path or leading
     * %LOCALAPPDATA% resolved for the host's user); does not load models.
     */
    bool initialize(const std::wstring& vstModuleDir);

    /**
     * Queues a .pth/.index selection into Slot A (0) or Slot B (1).
     * True means queued; getSlotInfo only reports loaded after daemon acknowledgement.
     */
    bool loadModelIntoSlot(int slotIndex, const std::wstring& filePath);
    // Explicit checkpoint/index pair; an empty indexPath disables retrieval.
    bool loadModelIntoSlot(int slotIndex, const std::wstring& filePath, const std::wstring& indexPath);

    /**
     * Clears the specified slot back to neutral unloaded state.
     */
    void clearSlot(int slotIndex);

    /**
     * Sets or retrieves per-slot target octave/key shift in semitones (-12, 0, +12 ST)
     * for cross-gender (Male <-> Female) RVC vocal conversion.
     */
    void setSlotOctaveShift(int slotIndex, int32_t semitones);
    int32_t getSlotOctaveShift(int slotIndex) const;

    /**
     * Returns metadata for UI display.
     */
    RvcModelSlotInfo getSlotInfo(int slotIndex) const;
    // Includes a queued selection for persistence, without claiming it has loaded.
    RvcModelSlotInfo getSlotSelection(int slotIndex) const;

    // Call before joining the inference worker; cancellation stays latched until resume.
    void cancelPendingWork() noexcept;
    // Only resume after the previous inference worker has joined.
    void resumePendingWork() noexcept;

    bool isOrtInitialized() const noexcept {
        return ortInitialized_.load(std::memory_order_acquire) ||
               daemonReady_.load(std::memory_order_acquire);
    }

    bool isNativeDaemonReady() const noexcept {
        return daemonReady_.load(std::memory_order_acquire);
    }

    bool isDaemonBusy() const noexcept {
        return daemonBusy_.load(std::memory_order_relaxed);
    }

    float getLoadingProgress() const noexcept {
        return loadingProgress_.load(std::memory_order_relaxed);
    }

    std::string getLoadingStatusText() const;
    std::wstring getRuntimeRootPath() const;

    uint64_t getModelRevision() const noexcept {
        return modelRevision_.load(std::memory_order_acquire);
    }

    float getLastInferenceTimeMs() const noexcept {
        return lastInferenceMs_.load(std::memory_order_relaxed);
    }

    /**
     * Synchronizes any pending Slot A / Slot B model loads with the CUDA daemon
     * on the background worker thread while streaming progress to the UI.
     */
    bool syncDaemonSlotsIfNeeded();

    /**
     * Ensures the background Python CUDA daemon is spawned and synced with the
     * latest Slot A / Slot B selections, then runs end-to-end RVC v2 neural
     * synthesis (HuBERT + RMVPE + FAISS + VITS/NSF-HiFiGAN) on a 16kHz vocal chunk.
     */
    bool synthesizeRvcChunk(
        const float* input16k,
        size_t numSamples16k,
        const RvcChunkSynthRequest& req,
        RvcChunkSynthResult& outResult,
        const std::function<void(float, const std::string&)>& onProgress = nullptr);

private:
    bool queueModelLoad(int slotIndex, const std::wstring& filePath, const std::wstring& indexPath, bool explicitIndex);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> ortInitialized_{false};
    std::atomic<bool> daemonReady_{false};
    std::atomic<bool> daemonBusy_{false};
    std::atomic<float> loadingProgress_{1.0f};
    std::atomic<uint64_t> modelRevision_{1};
    std::atomic<float> lastInferenceMs_{0.0f};
};

} // namespace rvc_vst
