#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace rvc_vst {

/**
 * LockFreeRingBuffer
 *
 * Single-Producer Single-Consumer (SPSC) wait-free ring buffer designed for
 * real-time DAW audio threads (Cubase processBlock).
 * Guarantees zero dynamic allocation, zero mutex locking, and deterministic
 * O(1) memory-order-acquire/release transfers between the audio thread and
 * the background ONNX neural inference worker thread.
 */
template <typename T, size_t CapacityPowerOfTwo = 65536>
class LockFreeRingBuffer {
    static_assert((CapacityPowerOfTwo & (CapacityPowerOfTwo - 1)) == 0,
                  "CapacityPowerOfTwo must be a power of two.");

public:
    LockFreeRingBuffer() noexcept {
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    void clear() noexcept {
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    size_t availableToRead() const noexcept {
        const size_t w = writePos_.load(std::memory_order_acquire);
        const size_t r = readPos_.load(std::memory_order_relaxed);
        return w - r;
    }

    size_t availableToWrite() const noexcept {
        const size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t r = readPos_.load(std::memory_order_acquire);
        return CapacityPowerOfTwo - (w - r);
    }

    size_t push(const T* data, size_t count) noexcept {
        const size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t r = readPos_.load(std::memory_order_acquire);
        const size_t freeSpace = CapacityPowerOfTwo - (w - r);
        const size_t toWrite = (count < freeSpace) ? count : freeSpace;
        if (toWrite == 0) {
            return 0;
        }

        const size_t startIdx = w & kMask;
        const size_t firstChunk = std::min(toWrite, CapacityPowerOfTwo - startIdx);
        std::memcpy(&buffer_[startIdx], data, firstChunk * sizeof(T));
        if (toWrite > firstChunk) {
            std::memcpy(&buffer_[0], data + firstChunk, (toWrite - firstChunk) * sizeof(T));
        }

        writePos_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    size_t pop(T* dest, size_t count) noexcept {
        const size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t w = writePos_.load(std::memory_order_acquire);
        const size_t avail = w - r;
        const size_t toRead = (count < avail) ? count : avail;
        if (toRead == 0) {
            return 0;
        }

        const size_t startIdx = r & kMask;
        const size_t firstChunk = std::min(toRead, CapacityPowerOfTwo - startIdx);
        std::memcpy(dest, &buffer_[startIdx], firstChunk * sizeof(T));
        if (toRead > firstChunk) {
            std::memcpy(dest + firstChunk, &buffer_[0], (toRead - firstChunk) * sizeof(T));
        }

        readPos_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    /**
     * Peeks the latest N samples without advancing the read pointer.
     * Used by the background ONNX Runtime inference worker to inspect a sliding
     * context window of incoming vocal audio.
     */
    size_t peekLatest(T* dest, size_t count) const noexcept {
        const size_t w = writePos_.load(std::memory_order_acquire);
        const size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t avail = w - r;
        if (avail < count) {
            return 0;
        }

        const size_t startPos = w - count;
        const size_t startIdx = startPos & kMask;
        const size_t firstChunk = std::min(count, CapacityPowerOfTwo - startIdx);
        std::memcpy(dest, &buffer_[startIdx], firstChunk * sizeof(T));
        if (count > firstChunk) {
            std::memcpy(dest + firstChunk, &buffer_[0], (count - firstChunk) * sizeof(T));
        }
        return count;
    }

    void discardOldest(size_t count) noexcept {
        const size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t w = writePos_.load(std::memory_order_acquire);
        const size_t avail = w - r;
        const size_t toAdvance = (count < avail) ? count : avail;
        readPos_.store(r + toAdvance, std::memory_order_release);
    }

private:
    static constexpr size_t kMask = CapacityPowerOfTwo - 1;

    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
    alignas(64) T buffer_[CapacityPowerOfTwo];
};

/**
 * SampleAccurateDelayLine
 *
 * Pre-allocated circular delay line used to align the dry Cubase vocal signal
 * with the STFT/Neural processing latency (PDC alignment), preventing comb
 * filtering when mixing Dry/Wet signals.
 */
class SampleAccurateDelayLine {
public:
    void prepare(size_t maxDelaySamples) {
        size_t pow2 = 1024;
        while (pow2 <= maxDelaySamples + 1024) {
            pow2 <<= 1;
        }
        mask_ = pow2 - 1;
        buffer_.assign(pow2, 0.0f);
        writePos_ = 0;
    }

    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writePos_ = 0;
    }

    void setDelaySamples(size_t delaySamples) noexcept {
        delaySamples_ = (delaySamples < mask_) ? delaySamples : mask_;
    }

    inline float processSample(float input) noexcept {
        if (buffer_.empty()) {
            return input;
        }
        buffer_[writePos_ & mask_] = input;
        const float delayed = buffer_[(writePos_ - delaySamples_) & mask_];
        ++writePos_;
        return delayed;
    }

private:
    std::vector<float> buffer_;
    size_t mask_ = 0;
    size_t writePos_ = 0;
    size_t delaySamples_ = 0;
};

} // namespace rvc_vst
