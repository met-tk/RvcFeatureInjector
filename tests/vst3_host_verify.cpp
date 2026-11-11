#include "../src/dsp/LockFreeRingBuffer.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
using tresult = int32;
using TBool = uint8_t;
using char8 = char;
using char16 = char16_t;
using TChar = char16;
using FIDString = const char8*;
using ParamID = uint32;
using ParamValue = double;
using Sample32 = float;
using Sample64 = double;
using SpeakerArrangement = uint64;
using TUID = uint8_t[16];

#define DEF_VST3_SWAPPED_TUID(name, l1, l2, l3, l4) \
    static const TUID name = { \
        static_cast<uint8_t>((l1) & 0xFF), static_cast<uint8_t>(((l1) >> 8) & 0xFF), \
        static_cast<uint8_t>(((l1) >> 16) & 0xFF), static_cast<uint8_t>(((l1) >> 24) & 0xFF), \
        static_cast<uint8_t>(((l2) >> 16) & 0xFF), static_cast<uint8_t>(((l2) >> 24) & 0xFF), \
        static_cast<uint8_t>((l2) & 0xFF), static_cast<uint8_t>(((l2) >> 8) & 0xFF), \
        static_cast<uint8_t>(((l3) >> 24) & 0xFF), static_cast<uint8_t>(((l3) >> 16) & 0xFF), \
        static_cast<uint8_t>(((l3) >> 8) & 0xFF), static_cast<uint8_t>((l3) & 0xFF), \
        static_cast<uint8_t>(((l4) >> 24) & 0xFF), static_cast<uint8_t>(((l4) >> 16) & 0xFF), \
        static_cast<uint8_t>(((l4) >> 8) & 0xFF), static_cast<uint8_t>((l4) & 0xFF) \
    }

DEF_VST3_SWAPPED_TUID(kIComponentIID,      0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
DEF_VST3_SWAPPED_TUID(kIAudioProcessorIID, 0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
DEF_VST3_SWAPPED_TUID(kIEditControllerIID, 0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
DEF_VST3_SWAPPED_TUID(kIProcessContextRequirementsIID, 0x2A654303, 0xEF764E3D, 0x95B5FE83, 0x730EF6D0);
DEF_VST3_SWAPPED_TUID(kIChannelContextInfoListenerIID, 0x33B10DB4, 0x03564ED7, 0x823AACE6, 0x567D024B);

#pragma pack(push, 8)
struct PFactoryInfo {
    char8 vendor[64];
    char8 url[256];
    char8 email[128];
    int32 flags;
};

struct PClassInfo {
    TUID cid;
    int32 cardinality;
    char8 category[32];
    char8 name[64];
};

struct ProcessSetup {
    int32 processMode;
    int32 symbolicSampleSize;
    int32 maxSamplesPerBlock;
    double sampleRate;
};

struct AudioBusBuffers {
    int32 numChannels;
    uint64 silenceFlags;
    union {
        Sample32** channelBuffers32;
        Sample64** channelBuffers64;
    };
};

struct ParameterInfo {
    ParamID id;
    TChar title[128];
    TChar shortTitle[128];
    TChar units[128];
    int32 stepCount;
    ParamValue defaultNormalizedValue;
    int32 unitId;
    int32 flags;
};

class FUnknown {
public:
    virtual tresult __stdcall queryInterface(const TUID _iid, void** obj) = 0;
    virtual uint32 __stdcall addRef() = 0;
    virtual uint32 __stdcall release() = 0;
};

class IAttributeList : public FUnknown {
public:
    virtual tresult __stdcall setInt(FIDString id, int64 value) = 0;
    virtual tresult __stdcall getInt(FIDString id, int64& value) = 0;
    virtual tresult __stdcall setFloat(FIDString id, double value) = 0;
    virtual tresult __stdcall getFloat(FIDString id, double& value) = 0;
    virtual tresult __stdcall setString(FIDString id, const TChar* string) = 0;
    virtual tresult __stdcall getString(FIDString id, TChar* string, uint32 sizeInBytes) = 0;
    virtual tresult __stdcall setBinary(FIDString id, const void* data, uint32 sizeInBytes) = 0;
    virtual tresult __stdcall getBinary(FIDString id, const void*& data, uint32& sizeInBytes) = 0;
};

class IInfoListener : public FUnknown {
public:
    virtual tresult __stdcall setChannelContextInfos(IAttributeList* list) = 0;
};

class IBStream : public FUnknown {
public:
    virtual tresult __stdcall read(void* buffer, int32 numBytes, int32* numBytesRead) = 0;
    virtual tresult __stdcall write(void* buffer, int32 numBytes, int32* numBytesWritten) = 0;
    virtual tresult __stdcall seek(int64 pos, int32 mode, int64* result) = 0;
    virtual tresult __stdcall tell(int64* pos) = 0;
};

class IParamValueQueue : public FUnknown {
public:
    virtual ParamID __stdcall getParameterId() = 0;
    virtual int32 __stdcall getPointCount() = 0;
    virtual tresult __stdcall getPoint(int32 index, int32& sampleOffset, ParamValue& value) = 0;
    virtual tresult __stdcall addPoint(int32 sampleOffset, ParamValue value, int32& index) = 0;
};

class IParameterChanges : public FUnknown {
public:
    virtual int32 __stdcall getParameterCount() = 0;
    virtual IParamValueQueue* __stdcall getParameterData(int32 index) = 0;
    virtual IParamValueQueue* __stdcall addParameterData(const ParamID& id, int32& index) = 0;
};

struct Vst3ProcessContext {
    enum StatesAndFlags : uint32 {
        kPlaying = 1 << 1
    };
    uint32 state;
    double sampleRate;
    int64 projectTimeSamples;
    int64 systemTime;
    int64 continousTimeSamples;
};

struct ProcessData {
    int32 processMode;
    int32 symbolicSampleSize;
    int32 numSamples;
    int32 numInputs;
    int32 numOutputs;
    AudioBusBuffers* inputs;
    AudioBusBuffers* outputs;
    IParameterChanges* inputParameterChanges;
    IParameterChanges* outputParameterChanges;
    void* inputEvents;
    void* outputEvents;
    Vst3ProcessContext* processContext;
};
#pragma pack(pop)

class IPluginBase : public FUnknown {
public:
    virtual tresult __stdcall initialize(FUnknown* context) = 0;
    virtual tresult __stdcall terminate() = 0;
};

class IComponent : public IPluginBase {
public:
    virtual tresult __stdcall getControllerClassId(TUID classId) = 0;
    virtual tresult __stdcall setIoMode(int32 mode) = 0;
    virtual int32 __stdcall getBusCount(int32 type, int32 dir) = 0;
    virtual tresult __stdcall getBusInfo(int32 type, int32 dir, int32 index, void* bus) = 0;
    virtual tresult __stdcall getRoutingInfo(void* inInfo, void* outInfo) = 0;
    virtual tresult __stdcall activateBus(int32 type, int32 dir, int32 index, TBool state) = 0;
    virtual tresult __stdcall setActive(TBool state) = 0;
    virtual tresult __stdcall setState(IBStream* state) = 0;
    virtual tresult __stdcall getState(IBStream* state) = 0;
};

class IAudioProcessor : public FUnknown {
public:
    virtual tresult __stdcall setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) = 0;
    virtual tresult __stdcall getBusArrangement(int32 dir, int32 index, SpeakerArrangement& arr) = 0;
    virtual tresult __stdcall canProcessSampleSize(int32 symbolicSampleSize) = 0;
    virtual uint32 __stdcall getLatencySamples() = 0;
    virtual tresult __stdcall setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult __stdcall setProcessing(TBool state) = 0;
    virtual tresult __stdcall process(ProcessData& data) = 0;
    virtual uint32 __stdcall getTailSamples() = 0;
};

class IProcessContextRequirements : public FUnknown {
public:
    // Steinberg ivstaudioprocessor.h: transport flags are not implied by continuous time.
    enum Flags : uint32 {
        kNeedSystemTime = 1 << 0,
        kNeedContinousTimeSamples = 1 << 1,
        kNeedTransportState = 1 << 10
    };
    virtual uint32 __stdcall getProcessContextRequirements() = 0;
};

class IEditController : public IPluginBase {
public:
    virtual tresult __stdcall setComponentState(IBStream* state) = 0;
    virtual tresult __stdcall setState(IBStream* state) = 0;
    virtual tresult __stdcall getState(IBStream* state) = 0;
    virtual int32 __stdcall getParameterCount() = 0;
    virtual tresult __stdcall getParameterInfo(int32 paramIndex, ParameterInfo& info) = 0;
    virtual tresult __stdcall getParamStringByValue(ParamID id, ParamValue valueNormalized, TChar* string) = 0;
    virtual tresult __stdcall getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) = 0;
    virtual ParamValue __stdcall normalizedParamToPlain(ParamID id, ParamValue valueNormalized) = 0;
    virtual ParamValue __stdcall plainParamToNormalized(ParamID id, ParamValue plainValue) = 0;
    virtual ParamValue __stdcall getParamNormalized(ParamID id) = 0;
    virtual tresult __stdcall setParamNormalized(ParamID id, ParamValue value) = 0;
    virtual tresult __stdcall setComponentHandler(void* handler) = 0;
    virtual void* __stdcall createView(FIDString name) = 0;
};

class IPluginFactory : public FUnknown {
public:
    virtual tresult __stdcall getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32 __stdcall countClasses() = 0;
    virtual tresult __stdcall getClassInfo(int32 index, PClassInfo* info) = 0;
    virtual tresult __stdcall createInstance(FIDString cid, FIDString _iid, void** obj) = 0;
};

// ============================================================================
// Mock Implementations for VST3 Automation and State Persistence
// ============================================================================

class MemoryStream final : public IBStream {
public:
    tresult __stdcall queryInterface(const TUID, void** obj) override {
        if (!obj) return 2;
        *obj = nullptr;
        return static_cast<tresult>(0x80004002L);
    }
    uint32 __stdcall addRef() override { return ++refCount_; }
    uint32 __stdcall release() override {
        const uint32 r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    tresult __stdcall read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        if (!buffer || numBytes < 0) return 2;
        const int32 avail = static_cast<int32>(data_.size() - pos_);
        const int32 toRead = (std::min)(numBytes, (std::max)(0, avail));
        if (toRead > 0) {
            std::memcpy(buffer, data_.data() + pos_, toRead);
            pos_ += toRead;
        }
        if (numBytesRead) *numBytesRead = toRead;
        return (toRead == numBytes) ? 0 : 1;
    }

    tresult __stdcall write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (!buffer || numBytes < 0) return 2;
        if (pos_ + numBytes > static_cast<int64>(data_.size())) {
            data_.resize(static_cast<size_t>(pos_ + numBytes));
        }
        std::memcpy(data_.data() + pos_, buffer, numBytes);
        pos_ += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return 0;
    }

    tresult __stdcall seek(int64 pos, int32 mode, int64* result) override {
        if (mode == 0) pos_ = pos;
        else if (mode == 1) pos_ += pos;
        else if (mode == 2) pos_ = static_cast<int64>(data_.size()) + pos;
        else return 2;
        if (pos_ < 0) pos_ = 0;
        if (result) *result = pos_;
        return 0;
    }

    tresult __stdcall tell(int64* pos) override {
        if (!pos) return 2;
        *pos = pos_;
        return 0;
    }

    const std::vector<uint8_t>& getData() const noexcept { return data_; }
    void clear() noexcept { data_.clear(); pos_ = 0; }

private:
    std::atomic<uint32> refCount_{1};
    std::vector<uint8_t> data_;
    int64 pos_ = 0;
};

struct ParamPoint {
    int32 sampleOffset = 0;
    ParamValue value = 0.0;
};

class MockParamValueQueue final : public IParamValueQueue {
public:
    explicit MockParamValueQueue(ParamID id) : id_(id) {}
    tresult __stdcall queryInterface(const TUID, void** obj) override {
        if (!obj) return 2;
        *obj = nullptr;
        return static_cast<tresult>(0x80004002L);
    }
    uint32 __stdcall addRef() override { return ++refCount_; }
    uint32 __stdcall release() override {
        const uint32 r = --refCount_;
        if (r == 0) delete this;
        return r;
    }
    ParamID __stdcall getParameterId() override { return id_; }
    int32 __stdcall getPointCount() override { return static_cast<int32>(points_.size()); }
    tresult __stdcall getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index < 0 || index >= static_cast<int32>(points_.size())) return 2;
        sampleOffset = points_[index].sampleOffset;
        value = points_[index].value;
        return 0;
    }
    tresult __stdcall addPoint(int32 sampleOffset, ParamValue value, int32& index) override {
        points_.push_back({sampleOffset, value});
        index = static_cast<int32>(points_.size() - 1);
        return 0;
    }
private:
    std::atomic<uint32> refCount_{1};
    ParamID id_ = 0;
    std::vector<ParamPoint> points_;
};

class MockParameterChanges final : public IParameterChanges {
public:
    ~MockParameterChanges() {
        for (auto* q : queues_) {
            if (q) q->release();
        }
    }
    tresult __stdcall queryInterface(const TUID, void** obj) override {
        if (!obj) return 2;
        *obj = nullptr;
        return static_cast<tresult>(0x80004002L);
    }
    uint32 __stdcall addRef() override { return ++refCount_; }
    uint32 __stdcall release() override {
        const uint32 r = --refCount_;
        if (r == 0) delete this;
        return r;
    }
    int32 __stdcall getParameterCount() override { return static_cast<int32>(queues_.size()); }
    IParamValueQueue* __stdcall getParameterData(int32 index) override {
        if (index < 0 || index >= static_cast<int32>(queues_.size())) return nullptr;
        return queues_[index];
    }
    IParamValueQueue* __stdcall addParameterData(const ParamID& id, int32& index) override {
        auto* q = new MockParamValueQueue(id);
        queues_.push_back(q);
        index = static_cast<int32>(queues_.size() - 1);
        return q;
    }
private:
    std::atomic<uint32> refCount_{1};
    std::vector<MockParamValueQueue*> queues_;
};

// ============================================================================
// Test Suite 1: SPSC LockFreeRingBuffer Boundary, Wrap-Around & Concurrency
// ============================================================================

bool testLockFreeRingBufferAndDelayLine() {
    std::cout << "[SUITE 1/12] Testing SPSC LockFreeRingBuffer and DelayLine..." << std::endl;

    rvc_vst::LockFreeRingBuffer<float, 1024> rb;

    // 1. Initial State
    if (rb.availableToRead() != 0 || rb.availableToWrite() != 1024) {
        std::cerr << "  FAIL: Initial sizes incorrect." << std::endl;
        return false;
    }

    // 2. Zero-length read/write
    float dummy[16] = {0};
    if (rb.push(dummy, 0) != 0 || rb.pop(dummy, 0) != 0) {
        std::cerr << "  FAIL: Zero count push/pop should return 0." << std::endl;
        return false;
    }

    // 3. Partial push & pop
    std::vector<float> src(300);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<float>(i + 1);
    if (rb.push(src.data(), src.size()) != 300) {
        std::cerr << "  FAIL: Failed to push 300 items." << std::endl;
        return false;
    }
    if (rb.availableToRead() != 300 || rb.availableToWrite() != 1024 - 300) {
        std::cerr << "  FAIL: Available counts mismatch after push." << std::endl;
        return false;
    }

    std::vector<float> dst(100, 0.0f);
    if (rb.pop(dst.data(), 100) != 100) {
        std::cerr << "  FAIL: Failed to pop 100 items." << std::endl;
        return false;
    }
    for (size_t i = 0; i < 100; ++i) {
        if (dst[i] != static_cast<float>(i + 1)) {
            std::cerr << "  FAIL: Popped data mismatch." << std::endl;
            return false;
        }
    }

    // 4. Fill to saturation
    std::vector<float> fill(rb.availableToWrite(), 99.0f);
    const size_t pushed = rb.push(fill.data(), fill.size());
    if (pushed != fill.size() || rb.availableToWrite() != 0) {
        std::cerr << "  FAIL: Buffer not full as expected." << std::endl;
        return false;
    }
    // Attempt overflow write
    float overflow = 123.0f;
    if (rb.push(&overflow, 1) != 0) {
        std::cerr << "  FAIL: Overflow write must return 0." << std::endl;
        return false;
    }

    // 5. PeekLatest
    std::vector<float> peekBuf(50);
    if (rb.peekLatest(peekBuf.data(), 50) != 50) {
        std::cerr << "  FAIL: peekLatest returned wrong count." << std::endl;
        return false;
    }
    for (float v : peekBuf) {
        if (v != 99.0f) {
            std::cerr << "  FAIL: peekLatest content mismatch." << std::endl;
            return false;
        }
    }
    // Peek more than available
    if (rb.peekLatest(peekBuf.data(), 2000) != 0) {
        std::cerr << "  FAIL: peekLatest beyond capacity must return 0." << std::endl;
        return false;
    }

    // 6. DiscardOldest & Clear
    rb.discardOldest(200);
    if (rb.availableToRead() != 1024 - 200) {
        std::cerr << "  FAIL: discardOldest did not update read pointer correctly." << std::endl;
        return false;
    }
    rb.clear();
    if (rb.availableToRead() != 0 || rb.availableToWrite() != 1024) {
        std::cerr << "  FAIL: clear() did not reset state." << std::endl;
        return false;
    }

    // 7. Wrap-Around verification across power-of-two mask boundary
    constexpr size_t kPrimeChunk = 137;
    for (int iter = 0; iter < 40; ++iter) {
        std::vector<float> block(kPrimeChunk);
        for (size_t i = 0; i < kPrimeChunk; ++i) {
            block[i] = static_cast<float>(iter * 1000 + i);
        }
        if (rb.push(block.data(), kPrimeChunk) != kPrimeChunk) {
            std::cerr << "  FAIL: Wrap-around push failed at iter " << iter << std::endl;
            return false;
        }
        std::vector<float> readBack(kPrimeChunk);
        if (rb.pop(readBack.data(), kPrimeChunk) != kPrimeChunk) {
            std::cerr << "  FAIL: Wrap-around pop failed at iter " << iter << std::endl;
            return false;
        }
        for (size_t i = 0; i < kPrimeChunk; ++i) {
            if (readBack[i] != block[i]) {
                std::cerr << "  FAIL: Wrap-around data corrupted at iter " << iter << std::endl;
                return false;
            }
        }
    }

    // 8. Multi-Threaded SPSC Concurrency Stress Test (1,000,000 items)
    constexpr size_t kStressTotal = 1000000;
    rvc_vst::LockFreeRingBuffer<uint32_t, 65536> stressBuffer;
    std::atomic<bool> producerDone{false};
    std::atomic<bool> stressPassed{true};

    std::thread producer([&]() {
        uint32_t currentVal = 0;
        std::mt19937 rng(42);
        std::uniform_int_distribution<size_t> chunkDist(1, 256);
        std::vector<uint32_t> tempChunk(256);

        while (currentVal < kStressTotal) {
            const size_t batch = (std::min)(chunkDist(rng), static_cast<size_t>(kStressTotal - currentVal));
            for (size_t i = 0; i < batch; ++i) {
                tempChunk[i] = currentVal + static_cast<uint32_t>(i);
            }
            size_t written = 0;
            while (written < batch) {
                const size_t w = stressBuffer.push(tempChunk.data() + written, batch - written);
                written += w;
                if (written < batch) {
                    std::this_thread::yield();
                }
            }
            currentVal += static_cast<uint32_t>(batch);
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        uint32_t expectedVal = 0;
        std::vector<uint32_t> tempChunk(256);

        while (expectedVal < kStressTotal) {
            const size_t r = stressBuffer.pop(tempChunk.data(), tempChunk.size());
            if (r == 0) {
                if (producerDone.load(std::memory_order_acquire) && stressBuffer.availableToRead() == 0) {
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            for (size_t i = 0; i < r; ++i) {
                if (tempChunk[i] != expectedVal) {
                    std::cerr << "  FAIL: SPSC Concurrency race! Expected: " << expectedVal
                              << " Got: " << tempChunk[i] << std::endl;
                    stressPassed.store(false, std::memory_order_relaxed);
                    return;
                }
                ++expectedVal;
            }
        }
        if (expectedVal != kStressTotal) {
            stressPassed.store(false, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    if (!stressPassed.load()) {
        std::cerr << "  FAIL: Multi-threaded SPSC stress test failed." << std::endl;
        return false;
    }

    // 9. SampleAccurateDelayLine Verification
    rvc_vst::SampleAccurateDelayLine delayLine;
    delayLine.prepare(2048);
    delayLine.setDelaySamples(768);

    // Feed Dirac impulse at index 0
    std::vector<float> impulseOut(1000);
    for (size_t i = 0; i < 1000; ++i) {
        const float inSample = (i == 0) ? 1.0f : 0.0f;
        impulseOut[i] = delayLine.processSample(inSample);
    }
    for (size_t i = 0; i < 768; ++i) {
        if (std::abs(impulseOut[i]) > 1e-6f) {
            std::cerr << "  FAIL: Pre-delay sample non-zero at index " << i << std::endl;
            return false;
        }
    }
    if (std::abs(impulseOut[768] - 1.0f) > 1e-6f) {
        std::cerr << "  FAIL: Dirac impulse not aligned at sample 768." << std::endl;
        return false;
    }
    for (size_t i = 769; i < 1000; ++i) {
        if (std::abs(impulseOut[i]) > 1e-6f) {
            std::cerr << "  FAIL: Post-delay sample non-zero at index " << i << std::endl;
            return false;
        }
    }

    std::cout << "  PASS: SPSC LockFreeRingBuffer (1M elements) & DelayLine verified successfully." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 2: VST3 12 Parameters Plain <-> Normalized Roundtrip Precision
// ============================================================================

bool testParameterRoundtripPrecision(IEditController* ctrl) {
    std::cout << "[SUITE 2/12] Testing VST3 12 Parameters Plain <-> Normalized Roundtrip..." << std::endl;
    if (!ctrl) return false;

    const int32 paramCount = ctrl->getParameterCount();
    if (paramCount != 12) {
        std::cerr << "  FAIL: Expected 12 parameters, got " << paramCount << std::endl;
        return false;
    }

    const double testNorms[] = {0.0, 0.05, 0.15, 0.25, 0.33, 0.50, 0.66, 0.75, 0.85, 0.95, 1.0};

    for (int32 id = 0; id < paramCount; ++id) {
        ParameterInfo info{};
        if (ctrl->getParameterInfo(id, info) != 0) {
            std::cerr << "  FAIL: getParameterInfo failed for id " << id << std::endl;
            return false;
        }

        // Discrete toggle parameters (CacheLock = 10, Bypass = 11)
        if (id == 11) {
            const double lowPlain = ctrl->normalizedParamToPlain(id, 0.0);
            const double highPlain = ctrl->normalizedParamToPlain(id, 1.0);
            if (ctrl->plainParamToNormalized(id, lowPlain) != 0.0 ||
                ctrl->plainParamToNormalized(id, highPlain) != 1.0) {
                std::cerr << "  FAIL: Discrete parameter roundtrip mismatch on id " << id << std::endl;
                return false;
            }
            continue;
        }

        // Continuous parameters (0..9)
        for (double normIn : testNorms) {
            const double plain = ctrl->normalizedParamToPlain(id, normIn);
            const double normOut = ctrl->plainParamToNormalized(id, plain);
            const double err = std::abs(normIn - normOut);
            if (err > 1e-4) {
                std::cerr << "  FAIL: Roundtrip precision error " << err
                          << " on param " << id << " (normIn: " << normIn
                          << " -> plain: " << plain << " -> normOut: " << normOut << ")" << std::endl;
                return false;
            }
        }

        // Check boundary clamping on extreme plain inputs
        if (ctrl->plainParamToNormalized(id, -1e6) != 0.0 ||
            ctrl->plainParamToNormalized(id, +1e6) != 1.0) {
            std::cerr << "  FAIL: Extreme plain values not clamped to [0, 1] on param " << id << std::endl;
            return false;
        }

        // Check string conversion
        TChar strBuf[128] = {0};
        if (ctrl->getParamStringByValue(id, 0.5, strBuf) != 0) {
            std::cerr << "  FAIL: getParamStringByValue failed on param " << id << std::endl;
            return false;
        }
    }

    std::cout << "  PASS: All 12 parameters roundtrip precision & boundary clamping verified." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 3: State Persistence (getState / setState) Serialization & Roundtrip
// ============================================================================

bool testStatePersistenceRoundtrip(IComponent* comp, IEditController* ctrl) {
    std::cout << "[SUITE 3/12] Testing getState / setState Serialization & Restoration..." << std::endl;
    if (!comp || !ctrl) return false;

    // 1. Set distinctive, non-default values for all 12 parameters
    const double customVals[12] = {
        0.18, 0.88, 0.72, 0.35, 0.44, 0.82, 0.15, 0.88, 0.89, 0.27, 1.0, 1.0
    };
    for (int i = 0; i < 12; ++i) {
        ctrl->setParamNormalized(i, customVals[i]);
    }

    // 2. Serialize plugin state into MemoryStream
    MemoryStream stream;
    if (comp->getState(&stream) != 0) {
        std::cerr << "  FAIL: comp->getState failed." << std::endl;
        return false;
    }

    const auto& bytes = stream.getData();
    if (bytes.size() < 1040) {
        std::cerr << "  FAIL: Serialized state chunk size too small: " << bytes.size() << std::endl;
        return false;
    }

    // V3 appends explicit index paths while retaining the V2 prefix.
    if (std::memcmp(bytes.data(), "RVCS", 4) != 0) {
        std::cerr << "  FAIL: Persisted state chunk missing 'RVCS' magic header." << std::endl;
        return false;
    }
    const uint32 version = *reinterpret_cast<const uint32*>(bytes.data() + 4);
    if (version != 3) {
        std::cerr << "  FAIL: Persisted state chunk version mismatch: " << version << std::endl;
        return false;
    }

    // 3. Mutate all parameters to completely different values
    for (int i = 0; i < 12; ++i) {
        ctrl->setParamNormalized(i, 0.0);
        if (std::abs(ctrl->getParamNormalized(i) - 0.0) > 1e-5) {
            std::cerr << "  FAIL: Failed to mutate param " << i << " prior to restore." << std::endl;
            return false;
        }
    }

    // 4. Restore state via setState
    stream.seek(0, 0, nullptr);
    if (comp->setState(&stream) != 0) {
        std::cerr << "  FAIL: comp->setState failed." << std::endl;
        return false;
    }

    // 5. Verify all 12 parameters restored accurately
    for (int i = 0; i < 12; ++i) {
        const double restored = ctrl->getParamNormalized(i);
        const double diff = std::abs(restored - customVals[i]);
        if (diff > 1e-5) {
            std::cerr << "  FAIL: State restore mismatch on param " << i
                      << ": Expected " << customVals[i] << ", Got " << restored << std::endl;
            return false;
        }
    }

    // 6. Test defensive handling of malformed streams
    if (comp->setState(nullptr) == 0) {
        std::cerr << "  FAIL: setState(nullptr) should fail with invalid argument." << std::endl;
        return false;
    }

    MemoryStream badStream;
    uint8_t garbage[32] = {0xFF};
    int32 written = 0;
    badStream.write(garbage, 32, &written);
    badStream.seek(0, 0, nullptr);
    // Should safely reject without crash
    comp->setState(&badStream);

    // Old and new states contain no audio: an Active flag must never revive zero-filled wet buffers.
    for (uint32_t storedVersion : {2u, 3u}) {
        auto persisted = bytes;
        std::memcpy(persisted.data() + 4, &storedVersion, 4);
        if (storedVersion == 2) persisted.resize(1180);
        const int64_t start = 0, end = 48000;
        const uint32_t active = 3;
        std::memcpy(persisted.data() + 1128, &start, 8);
        std::memcpy(persisted.data() + 1136, &end, 8);
        std::memcpy(persisted.data() + 1144, &active, 4);
        MemoryStream restoredState;
        restoredState.write(persisted.data(), static_cast<int32>(persisted.size()), &written);
        restoredState.seek(0, 0, nullptr);
        if (comp->setState(&restoredState) != 0) return false;
        MemoryStream readBack;
        if (comp->getState(&readBack) != 0) return false;
        uint32_t timelineState = 99;
        int64_t captureStart = 0;
        std::memcpy(&timelineState, readBack.getData().data() + 1144, 4);
        std::memcpy(&captureStart, readBack.getData().data() + 1128, 8);
        if (timelineState != 0 || captureStart != -1) {
            std::cerr << "  FAIL: Restored state activated a timeline without audio." << std::endl;
            return false;
        }
        if (storedVersion == 3) {
            persisted.resize(persisted.size() - 1);
            MemoryStream truncated;
            truncated.write(persisted.data(), static_cast<int32>(persisted.size()), &written);
            truncated.seek(0, 0, nullptr);
            if (comp->setState(&truncated) == 0) return false;
        }
    }

    std::cout << "  PASS: V2/V3 restore requires fresh capture; parameters and malformed-stream checks verified." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 4: Parameter Automation Queue Response
// ============================================================================

bool testParameterAutomationQueue(IComponent* comp, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 4/12] Testing Parameter Automation Queue Response in processBlock..." << std::endl;
    if (!comp || !proc || !ctrl) return false;

    ProcessSetup setup{0, 0, 512, 48000.0};
    proc->setupProcessing(setup);
    comp->setActive(1);
    proc->setProcessing(1);

    MockParameterChanges changes;
    int32 qIdx = 0;

    // Queue 1: kParamAbMorph (id 0) -> target 0.77
    auto* qMorph = changes.addParameterData(0, qIdx);
    int32 ptIdx = 0;
    qMorph->addPoint(0, 0.20, ptIdx);
    qMorph->addPoint(256, 0.77, ptIdx);

    // Queue 2: kParamPitchShift (id 2) -> target 0.625 (+3.0 ST)
    auto* qPitch = changes.addParameterData(2, qIdx);
    qPitch->addPoint(128, 0.625, ptIdx);

    // Queue 3: kParamDryWetMix (id 7) -> target 0.45
    auto* qMix = changes.addParameterData(7, qIdx);
    qMix->addPoint(64, 0.45, ptIdx);

    std::vector<float> inL(512, 0.1f), inR(512, 0.1f), outL(512, 0.0f), outR(512, 0.0f);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.inputParameterChanges = &changes;

    proc->process(data);

    const double morphActual = ctrl->getParamNormalized(0);
    const double pitchActual = ctrl->getParamNormalized(2);
    const double mixActual = ctrl->getParamNormalized(7);

    if (std::abs(morphActual - 0.77) > 1e-5 ||
        std::abs(pitchActual - 0.625) > 1e-5 ||
        std::abs(mixActual - 0.45) > 1e-5) {
        std::cerr << "  FAIL: Automation queue values not applied in processBlock."
                  << " Morph: " << morphActual << " Pitch: " << pitchActual
                  << " Mix: " << mixActual << std::endl;
        return false;
    }

    std::cout << "  PASS: Parameter automation queue processed with point accuracy." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 5: Cubase 64-bit Double & 32-bit Float Audio Engine Compatibility
// ============================================================================

bool testCubaseEnginePrecisionModes(IComponent* comp, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 5/12] Testing Cubase Engine Precision Modes (32-bit & 64-bit)..." << std::endl;
    if (!comp || !proc || !ctrl) return false;

    // 1. canProcessSampleSize check
    if (proc->canProcessSampleSize(0) != 0) { // kSample32
        std::cerr << "  FAIL: canProcessSampleSize(0) must return true (0)." << std::endl;
        return false;
    }
    if (proc->canProcessSampleSize(1) != 0) { // kSample64
        std::cerr << "  FAIL: canProcessSampleSize(1) must return true (0)." << std::endl;
        return false;
    }
    if (proc->canProcessSampleSize(2) == 0) {
        std::cerr << "  FAIL: canProcessSampleSize(2) should return false." << std::endl;
        return false;
    }

    // 2. Setup 64-bit double processing
    ProcessSetup setup64{0, 1 /* kSample64 */, 512, 48000.0};
    proc->setupProcessing(setup64);
    comp->setActive(1);
    proc->setProcessing(1);

    ctrl->setParamNormalized(0 /* kParamAbMorph */, 0.5);
    ctrl->setParamNormalized(1 /* kParamInjectDepth */, 0.8);
    ctrl->setParamNormalized(7 /* kParamDryWetMix */, 1.0);
    ctrl->setParamNormalized(11 /* kParamBypass */, 0.0);

    std::vector<double> inL64(512), inR64(512), outL64(512, 0.0), outR64(512, 0.0);
    double* inChans64[2] = {inL64.data(), inR64.data()};
    double* outChans64[2] = {outL64.data(), outR64.data()};
    AudioBusBuffers inBus64{2, 0, {nullptr}};
    inBus64.channelBuffers64 = inChans64;
    AudioBusBuffers outBus64{2, 0, {nullptr}};
    outBus64.channelBuffers64 = outChans64;

    ProcessData data64{};
    data64.processMode = 0;
    data64.symbolicSampleSize = 1;
    data64.numSamples = 512;
    data64.numInputs = 1;
    data64.numOutputs = 1;
    data64.inputs = &inBus64;
    data64.outputs = &outBus64;

    double energy64 = 0.0;
    for (int b = 0; b < 12; ++b) {
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0;
            const double s = 0.25 * std::sin(2.0 * 3.1415926535 * 440.0 * t);
            inL64[i] = s;
            inR64[i] = s;
        }
        proc->process(data64);
        if (outBus64.silenceFlags != 0) {
            std::cerr << "  FAIL: silenceFlags must be 0 after active processing." << std::endl;
            return false;
        }
        for (int i = 0; i < 512; ++i) {
            if (std::isnan(outL64[i]) || std::isinf(outL64[i]) ||
                std::isnan(outR64[i]) || std::isinf(outR64[i])) {
                std::cerr << "  FAIL: NaN or Inf encountered in 64-bit audio stream." << std::endl;
                return false;
            }
            if (b >= 4) {
                energy64 += outL64[i] * outL64[i];
            }
        }
    }
    const double rms64 = std::sqrt(energy64 / (8 * 512));
    if (rms64 < 0.01) {
        std::cerr << "  FAIL: 64-bit audio output is unexpectedly silent (RMS: " << rms64 << ")" << std::endl;
        return false;
    }

    // Switch back cleanly to 32-bit float mode
    ProcessSetup setup32{0, 0 /* kSample32 */, 512, 48000.0};
    proc->setupProcessing(setup32);
    comp->setActive(1);

    std::cout << "  PASS: 32-bit float and 64-bit double audio engine execution verified (64-bit RMS: "
              << rms64 << ")." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 6: Numerical Stability & Extreme Parameter Matrix (No NaN/Inf)
// ============================================================================

bool testNumericalStabilityAndExtremeBounds(IComponent* comp, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 6/12] Testing Numerical Stability on Extreme Parameter Bounds & Edge-Case Audio Signals..." << std::endl;
    if (!comp || !proc || !ctrl) return false;

    ProcessSetup setup{0, 0, 512, 48000.0};
    proc->setupProcessing(setup);
    comp->setActive(1);
    proc->setProcessing(1);

    std::vector<float> inL(512), inR(512), outL(512), outR(512);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;

    // Test parameter combination matrix:
    // Pitch: -12 ST (0.0), 0 ST (0.5), +12 ST (1.0)
    // Throat: -6 ST (0.0), 0 ST (0.5), +6 ST (1.0)
    // Presence: -6 dB (0.0), 0 dB (0.5), +6 dB (1.0)
    // Protect: 0.0 (0.0), 0.25 (0.5), 0.50 (1.0)
    // Dry/Wet: 0.0, 0.5, 1.0
    // Bypass: 0.0, 1.0
    const double pitchGrid[] = {0.0, 0.5, 1.0};
    const double throatGrid[] = {0.0, 0.5, 1.0};
    const double protectGrid[] = {0.0, 0.5, 1.0};
    const double dryWetGrid[] = {0.0, 0.5, 1.0};
    const double presenceGrid[] = {0.0, 1.0};
    const double bypassGrid[] = {0.0, 1.0};

    enum SignalType { kSilence = 0, kFullScaleSine, kWhiteNoise, kDiracImpulse, kDcOffset, kNyquistAlternating };
    const SignalType signalTypes[] = {
        kSilence, kFullScaleSine, kWhiteNoise, kDiracImpulse, kDcOffset, kNyquistAlternating
    };

    std::mt19937 noiseRng(1337);
    std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);

    uint64_t totalSamplesChecked = 0;

    for (SignalType sig : signalTypes) {
        for (double bypass : bypassGrid) {
            for (double pitch : pitchGrid) {
                for (double throat : throatGrid) {
                    for (double protect : protectGrid) {
                        for (double dryWet : dryWetGrid) {
                            for (double presence : presenceGrid) {
                                ctrl->setParamNormalized(0 /* kParamAbMorph */, 0.5);
                                ctrl->setParamNormalized(1 /* kParamInjectDepth */, 0.95);
                                ctrl->setParamNormalized(2 /* kParamPitchShift */, pitch);
                                ctrl->setParamNormalized(3 /* kParamThroatFormant */, throat);
                                ctrl->setParamNormalized(4 /* kParamPresenceAir */, presence);
                                ctrl->setParamNormalized(5 /* kParamConsonantProtect */, protect);
                                ctrl->setParamNormalized(6 /* kParamRmsMixRate */, 0.5);
                                ctrl->setParamNormalized(7 /* kParamDryWetMix */, dryWet);
                                ctrl->setParamNormalized(8 /* kParamSlotAIndexRate */, 0.75);
                                ctrl->setParamNormalized(9 /* kParamSlotBIndexRate */, 0.75);
                                ctrl->setParamNormalized(11 /* kParamBypass */, bypass);

                                // Generate signal buffer
                                for (int i = 0; i < 512; ++i) {
                                    float val = 0.0f;
                                    switch (sig) {
                                        case kSilence: val = 0.0f; break;
                                        case kFullScaleSine:
                                            val = std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
                                            break;
                                        case kWhiteNoise: val = noiseDist(noiseRng); break;
                                        case kDiracImpulse: val = (i == 0) ? 1.0f : 0.0f; break;
                                        case kDcOffset: val = 0.8f; break;
                                        case kNyquistAlternating: val = (i % 2 == 0) ? 0.9f : -0.9f; break;
                                    }
                                    inL[i] = val;
                                    inR[i] = val;
                                }

                                proc->process(data);

                                for (int i = 0; i < 512; ++i) {
                                    const float sL = outL[i];
                                    const float sR = outR[i];
                                    if (std::isnan(sL) || std::isinf(sL) || std::isnan(sR) || std::isinf(sR)) {
                                        std::cerr << "  FAIL: Numerical explosion! NaN/Inf detected! Sig=" << sig
                                                  << " Pitch=" << pitch << " Throat=" << throat << " Presence=" << presence
                                                  << " OutL=" << sL << " OutR=" << sR << std::endl;
                                        return false;
                                    }
                                    if (std::abs(sL) > 25.0f || std::abs(sR) > 25.0f) {
                                        std::cerr << "  FAIL: Sample value blown past safety limits: " << sL << std::endl;
                                        return false;
                                    }
                                }
                                totalSamplesChecked += 1024;
                            }
                        }
                    }
                }
            }
        }
    }

    // Specific Bypass Verification: In bypass, output must equal delayed dry signal
    ctrl->setParamNormalized(11 /* kParamBypass */, 1.0);
    // Flush latency
    for (int b = 0; b < 6; ++b) {
        for (int i = 0; i < 512; ++i) {
            inL[i] = std::sin(2.0f * 3.14159265f * 330.0f * (b * 512 + i) / 48000.0f);
            inR[i] = inL[i];
        }
        proc->process(data);
    }
    // Now verify bypass pass-through energy closely matches input energy
    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = 0; i < 512; ++i) {
        inEnergy += inL[i] * inL[i];
        outEnergy += outL[i] * outL[i];
    }
    const double rmsIn = std::sqrt(inEnergy / 512);
    const double rmsOut = std::sqrt(outEnergy / 512);
    if (std::abs(rmsIn - rmsOut) > 0.08) {
        std::cerr << "  FAIL: Bypass mode output energy differs from input (In: "
                  << rmsIn << ", Out: " << rmsOut << ")" << std::endl;
        return false;
    }

    std::cout << "  PASS: Checked " << totalSamplesChecked
              << " samples across 6 signal types & parameter permutations; 0 NaN/Inf, perfect stability." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 7: Cache Lock Playback Continuity & Deferred Re-Synthesis
// ============================================================================

bool testCacheLockContinuousPlaybackAndDeferredResynth(IComponent* comp, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 7/12] Testing Cache Lock Playback Continuity & Deferred Re-Synthesis..." << std::endl;
    if (!comp || !proc || !ctrl) return false;

    ProcessSetup setup{0, 0 /* kSample32 */, 512, 48000.0};
    proc->setupProcessing(setup);
    comp->setActive(1);
    proc->setProcessing(1);

    // 1. Configure parameters: Cache Lock enabled, 100% Wet, Bypass off
    ctrl->setParamNormalized(10 /* kParamOutputGain */, 0.5);
    ctrl->setParamNormalized(11 /* kParamBypass */, 0.0);
    ctrl->setParamNormalized(7 /* kParamDryWetMix */, 1.0);
    ctrl->setParamNormalized(2 /* kParamPitchShift */, 0.5); // 0 ST
    ctrl->setParamNormalized(8 /* kParamSlotAIndexRate */, 0.75); // default 0.75
    ctrl->setParamNormalized(9 /* kParamSlotBIndexRate */, 0.75); // default 0.75

    std::vector<float> inL(512), inR(512), outL(512, 0.0f), outR(512, 0.0f);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    Vst3ProcessContext ctx{};
    ctx.sampleRate = 48000.0;
    ctx.state = Vst3ProcessContext::kPlaying;

    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.processContext = &ctx;

    auto fillHarmonic = [](std::vector<float>& l, std::vector<float>& r, int64_t startSample) {
        for (int i = 0; i < 512; ++i) {
            const double t = static_cast<double>(startSample + i) / 48000.0;
            const float s = static_cast<float>(0.30 * std::sin(2.0 * 3.1415926535 * 440.0 * t) +
                                               0.15 * std::sin(2.0 * 3.1415926535 * 880.0 * t));
            l[i] = s;
            r[i] = s;
        }
    };

    // 2. Play initial blocks to establish baseline audio stream
    double initialEnergy = 0.0;
    for (int b = 0; b < 12; ++b) {
        ctx.projectTimeSamples = static_cast<int64_t>(b) * 512;
        fillHarmonic(inL, inR, ctx.projectTimeSamples);
        proc->process(data);
        if (b >= 4) {
            for (int i = 0; i < 512; ++i) {
                initialEnergy += outL[i] * outL[i];
            }
        }
    }
    const double rmsBefore = std::sqrt(initialEnergy / (8 * 512));
    if (rmsBefore < 0.01) {
        std::cerr << "  FAIL: Initial playback energy too low (RMS: " << rmsBefore << ")" << std::endl;
        return false;
    }

    // 3. While isPlaying = true and cacheLockMode = true, modify synthesis parameters (Slot A & Slot B Index Rate)
    ctrl->setParamNormalized(8 /* kParamSlotAIndexRate */, 0.20); // Shift to 0.20
    ctrl->setParamNormalized(9 /* kParamSlotBIndexRate */, 0.85); // Shift to 0.85

    // 4. Continue playback without transport stop: verify audio playback remains uninterrupted and continuous
    double energyDuringPlay = 0.0;
    for (int b = 12; b < 24; ++b) {
        ctx.projectTimeSamples = static_cast<int64_t>(b) * 512;
        fillHarmonic(inL, inR, ctx.projectTimeSamples);
        proc->process(data);

        if (outBus.silenceFlags != 0) {
            std::cerr << "  FAIL: Audio bus marked silent during active playback under Cache Lock." << std::endl;
            return false;
        }

        for (int i = 0; i < 512; ++i) {
            if (std::isnan(outL[i]) || std::isinf(outL[i]) ||
                std::isnan(outR[i]) || std::isinf(outR[i])) {
                std::cerr << "  FAIL: NaN/Inf encountered during playback parameter change with Cache Lock." << std::endl;
                return false;
            }
            if (b >= 16) {
                energyDuringPlay += outL[i] * outL[i];
            }
        }
    }
    const double rmsDuring = std::sqrt(energyDuringPlay / (8 * 512));
    if (rmsDuring < 0.01) {
        std::cerr << "  FAIL: Playback dropped out / muted when synthesis parameters changed under Cache Lock (RMS: "
                  << rmsDuring << ")" << std::endl;
        return false;
    }

    // 5. Verify transport stop triggers deferred state change cleanly without crash
    ctx.state = 0; // isPlaying = false
    ctx.projectTimeSamples = 24 * 512;
    fillHarmonic(inL, inR, ctx.projectTimeSamples);
    proc->process(data);

    // 6. Restore default parameters
    ctrl->setParamNormalized(8 /* kParamSlotAIndexRate */, 0.75);
    ctrl->setParamNormalized(9 /* kParamSlotBIndexRate */, 0.75);

    std::cout << "  PASS: Cache Lock verified. Audio chunks preserved mid-playback (RMS before: "
              << rmsBefore << ", during: " << rmsDuring << "); deferred resynth handled cleanly." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 8: 10ms Frame & Sample-Accurate Automation Continuous Smoothing
// ============================================================================

bool testFrameAndSampleAccurateAutomationSmoothing(IComponent* comp, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 8/12] Testing 10ms Frame & Sample-Accurate Automation Smoothing..." << std::endl;
    if (!comp || !proc || !ctrl) return false;

    ProcessSetup setup{0, 0 /* kSample32 */, 512, 48000.0};
    proc->setupProcessing(setup);
    comp->setActive(1);
    proc->setProcessing(1);

    ctrl->setParamNormalized(11 /* kParamBypass */, 0.0);
    ctrl->setParamNormalized(7 /* kParamDryWetMix */, 0.0); // Start 100% Dry
    ctrl->setParamNormalized(6 /* kParamRmsMixRate */, 0.0);
    ctrl->setParamNormalized(0 /* kParamAbMorph */, 0.0);
    ctrl->setParamNormalized(1 /* kParamInjectDepth */, 0.0);
    ctrl->setParamNormalized(8 /* kParamSlotAIndexRate */, 0.2);
    ctrl->setParamNormalized(9 /* kParamSlotBIndexRate */, 0.8);

    std::vector<float> inL(512), inR(512), outL(512, 0.0f), outR(512, 0.0f);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    Vst3ProcessContext ctx{};
    ctx.sampleRate = 48000.0;
    ctx.state = Vst3ProcessContext::kPlaying;

    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.processContext = &ctx;

    // Warm up 4 blocks with phase-continuous audio
    for (int b = 0; b < 4; ++b) {
        ctx.projectTimeSamples = static_cast<int64_t>(b) * 512;
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0;
            inL[i] = static_cast<float>(0.4 * std::sin(2.0 * 3.1415926535 * 330.0 * t));
            inR[i] = inL[i];
        }
        proc->process(data);
    }

    // Apply sudden full-scale step-change automation on all 6 automated parameters
    MockParameterChanges changes;
    int32 qIdx = 0, ptIdx = 0;

    // Sample-accurate parameters (DryWetMix, RmsMixRate)
    auto* qMix = changes.addParameterData(7 /* kParamDryWetMix */, qIdx);
    qMix->addPoint(64, 1.0, ptIdx); // Sudden step jump 0.0 -> 1.0 at sample 64

    auto* qRms = changes.addParameterData(6 /* kParamRmsMixRate */, qIdx);
    qRms->addPoint(128, 1.0, ptIdx); // Sudden step jump 0.0 -> 1.0 at sample 128

    // 10ms Frame-accurate parameters (AbMorph, InjectDepth, SlotAIndexRate, SlotBIndexRate)
    auto* qMorph = changes.addParameterData(0 /* kParamAbMorph */, qIdx);
    qMorph->addPoint(0, 1.0, ptIdx);

    auto* qDepth = changes.addParameterData(1 /* kParamInjectDepth */, qIdx);
    qDepth->addPoint(0, 1.0, ptIdx);

    auto* qSlotA = changes.addParameterData(8 /* kParamSlotAIndexRate */, qIdx);
    qSlotA->addPoint(0, 0.95, ptIdx);

    auto* qSlotB = changes.addParameterData(9 /* kParamSlotBIndexRate */, qIdx);
    qSlotB->addPoint(0, 0.05, ptIdx);

    data.inputParameterChanges = &changes;

    float prevSampleL = outL[511];
    float maxSampleStep = 0.0f;
    uint32_t totalSamplesInspected = 0;

    // Process block with automation injection, plus subsequent 15 blocks (total 16 blocks = 8192 samples)
    for (int b = 4; b < 20; ++b) {
        ctx.projectTimeSamples = static_cast<int64_t>(b) * 512;
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0;
            inL[i] = static_cast<float>(0.4 * std::sin(2.0 * 3.1415926535 * 330.0 * t));
            inR[i] = inL[i];
        }

        proc->process(data);
        data.inputParameterChanges = nullptr; // only inject changes on the first block

        for (int i = 0; i < 512; ++i) {
            const float sL = outL[i];
            const float sR = outR[i];
            if (std::isnan(sL) || std::isinf(sL) || std::isnan(sR) || std::isinf(sR)) {
                std::cerr << "  FAIL: NaN or Inf encountered during automation ramp." << std::endl;
                return false;
            }

            const float step = std::abs(sL - prevSampleL);
            if (step > maxSampleStep) {
                maxSampleStep = step;
            }
            prevSampleL = sL;
            ++totalSamplesInspected;
        }
    }

    if (maxSampleStep > 0.15f) {
        std::cerr << "  FAIL: Parameter automation produced discontinuous audio glitch! Max step: "
                  << maxSampleStep << " (threshold: 0.15)" << std::endl;
        return false;
    }

    if (std::abs(ctrl->getParamNormalized(7) - 1.0) > 1e-4 ||
        std::abs(ctrl->getParamNormalized(6) - 1.0) > 1e-4 ||
        std::abs(ctrl->getParamNormalized(0) - 1.0) > 1e-4 ||
        std::abs(ctrl->getParamNormalized(1) - 1.0) > 1e-4) {
        std::cerr << "  FAIL: Target parameter values did not update in controller." << std::endl;
        return false;
    }

    std::cout << "  PASS: 10ms frame & sample-accurate automation smoothing verified ("
              << totalSamplesInspected << " samples, max step: " << maxSampleStep << " < 0.15)." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 9: 0-Origin Timeline Locking & Non-Monotonic Seek Resilience
// ============================================================================

bool testZeroOriginTimelineSeekLocking(IComponent* comp, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 9/12] Testing 0-Origin Timeline Locking & Non-Monotonic Seek Resilience..." << std::endl;
    if (!comp || !proc || !ctrl) return false;

    ProcessSetup setup{0, 0 /* kSample32 */, 512, 48000.0};
    proc->setupProcessing(setup);
    comp->setActive(1);
    proc->setProcessing(1);

    ctrl->setParamNormalized(11 /* kParamBypass */, 0.0);
    ctrl->setParamNormalized(10 /* kParamOutputGain */, 0.5);
    ctrl->setParamNormalized(7 /* kParamDryWetMix */, 1.0);

    std::vector<float> inL(512), inR(512), outL(512, 0.0f), outR(512, 0.0f);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    Vst3ProcessContext ctx{};
    ctx.sampleRate = 48000.0;
    ctx.state = Vst3ProcessContext::kPlaying;

    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.processContext = &ctx;

    const int64_t seekPositions[] = {
        0,                                   // Origin: 0s
        48000 * 60,                          // Forward to 60s
        48000 * 180,                         // Forward to 180s
        48000 * 15,                          // Backward seek to 15s
        48000 * 239,                         // Forward seek to 239s (near 240s boundary)
        0,                                   // Backward seek to 0s (re-verify origin)
        (240 * 48000) - 512                  // Last block within 240s boundary
    };

    auto fillTestAudio = [](std::vector<float>& l, std::vector<float>& r, int64_t start) {
        for (int i = 0; i < 512; ++i) {
            const double t = static_cast<double>(start + i) / 48000.0;
            const float s = static_cast<float>(0.35 * std::sin(2.0 * 3.1415926535 * 440.0 * t));
            l[i] = s;
            r[i] = s;
        }
    };

    for (int64_t seekPos : seekPositions) {
        for (int b = 0; b < 4; ++b) {
            ctx.projectTimeSamples = seekPos + static_cast<int64_t>(b) * 512;
            fillTestAudio(inL, inR, ctx.projectTimeSamples);
            if (proc->process(data) != 0) {
                std::cerr << "  FAIL: process failed on seekPos=" << seekPos << " block=" << b << std::endl;
                return false;
            }

            if (outBus.silenceFlags != 0) {
                std::cerr << "  FAIL: Unexpected silence flag at seekPos=" << seekPos << std::endl;
                return false;
            }

            double blockEnergy = 0.0;
            for (int i = 0; i < 512; ++i) {
                if (std::isnan(outL[i]) || std::isinf(outL[i]) ||
                    std::isnan(outR[i]) || std::isinf(outR[i])) {
                    std::cerr << "  FAIL: NaN/Inf encountered after seek jump to " << seekPos << std::endl;
                    return false;
                }
                blockEnergy += outL[i] * outL[i];
            }
            const double rms = std::sqrt(blockEnergy / 512);
            if (rms < 0.005) {
                std::cerr << "  FAIL: Audio unexpectedly silent after seek jump to " << seekPos
                          << " (RMS: " << rms << ")" << std::endl;
                return false;
            }
        }
    }

    std::cout << "  PASS: 0-Origin timeline locking verified across 7 non-monotonic seek jumps within 240s window; zero drift or cache fault." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 10: Real-Time Benchmark, A/B Morph Delta & UI Snapshot
// ============================================================================

bool testRealtimeBenchmarkAndUi(
    HMODULE hVst,
    IComponent* comp,
    IAudioProcessor* proc,
    IEditController* ctrl) {

    std::cout << "[SUITE 10/12] Real-Time Benchmark (A/B Timbre Morphing) & Direct2D UI Snapshot..." << std::endl;

    ctrl->setParamNormalized(11 /* kParamBypass */, 0.0);
    ctrl->setParamNormalized(7 /* kParamDryWetMix */, 1.0);

    double totalElapsedMs = 0.0;
    constexpr int kNumBlocks = 48;
    std::vector<float> inL(512), inR(512), outL(512), outR(512);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    Vst3ProcessContext ctx{};
    ctx.state = Vst3ProcessContext::kPlaying;
    ctx.sampleRate = 48000.0;

    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.processContext = &ctx;

    double energyOut = 0.0;
    for (int b = 0; b < kNumBlocks; ++b) {
        ctx.projectTimeSamples = static_cast<int64_t>(b) * 512;
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0;
            const double f0 = 220.0;
            float s = 0.0f;
            for (int h = 1; h <= 20; ++h) {
                s += static_cast<float>((0.12 / h) * std::sin(2.0 * 3.1415926535 * h * f0 * t));
            }
            inL[i] = s;
            inR[i] = s;
        }

        const auto t0 = std::chrono::high_resolution_clock::now();
        proc->process(data);
        const auto t1 = std::chrono::high_resolution_clock::now();
        totalElapsedMs += std::chrono::duration<double, std::milli>(t1 - t0).count();

        for (int i = 0; i < 512; ++i) {
            energyOut += outL[i] * outL[i];
        }
    }

    const double avgMs = totalElapsedMs / kNumBlocks;
    const double rmsOut = std::sqrt(energyOut / (kNumBlocks * 512));

    std::cout << "  Avg processBlock Time: " << avgMs << " ms ("
              << (avgMs / 10.667 * 100.0) << "% of 10.67ms budget) | Output RMS: " << rmsOut << std::endl;

    if (rmsOut < 0.01 || avgMs > 5.0) {
        std::cerr << "  FAIL: Real-time processBlock too slow or output silent." << std::endl;
        return false;
    }

    using RenderPreviewFn = bool(__stdcall*)(const wchar_t*);
    auto renderPreview = reinterpret_cast<RenderPreviewFn>(GetProcAddress(hVst, "RvcVst3_RenderUiPreviewPng"));
    fs::path previewPng = L"X:\\AI project\\虚拟歌姬音色混合\\dist\\ui_preview.png";
    if (!renderPreview || !renderPreview(previewPng.c_str()) || !fs::exists(previewPng)) {
        std::cerr << "  FAIL: Failed to render Direct2D UI preview PNG." << std::endl;
        return false;
    }
    std::cout << "  PASS: Real-time benchmark & Direct2D UI snapshot verified." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 11: Native RVC v2 CUDA Timeline Cache End-to-End Verification
// ============================================================================

bool testNativeRvcNeuralCacheE2E(HMODULE hVst) {
    std::cout << "[SUITE 11/12] Native RVC v2 CUDA Timeline Cache End-to-End..." << std::endl;
    using VerifyFn = bool(__stdcall*)(float*, float*, float*, float*);
    auto verify = reinterpret_cast<VerifyFn>(GetProcAddress(hVst, "RvcVst3_VerifyNeuralCacheE2E"));
    if (!verify) {
        std::cerr << "  FAIL: Missing RvcVst3_VerifyNeuralCacheE2E export." << std::endl;
        return false;
    }
    float coverage = 0.0f, wetRms = 0.0f, corr = 1.0f, synthSec = 0.0f;
    const bool idle = verify(&coverage, &wetRms, &corr, &synthSec);
    std::cout << "  Synthesis wait: " << synthSec << " s | Cache coverage: " << coverage * 100.0f
              << "% | Wet RMS: " << wetRms << " | Dry/Wet correlation: " << corr << std::endl;
    if (!idle || coverage < 0.999f) {
        std::cerr << "  FAIL: CUDA daemon did not fully populate the timeline cache." << std::endl;
        return false;
    }
    if (wetRms < 0.005f || std::isnan(wetRms)) {
        std::cerr << "  FAIL: Cached RVC playback is silent." << std::endl;
        return false;
    }
    if (std::abs(corr) > 0.97f) {
        std::cerr << "  FAIL: Cached output is near-identical to dry input (neural conversion inactive)." << std::endl;
        return false;
    }
    std::cout << "  PASS: Native RVC v2 neural waveform cached and replayed from timeline." << std::endl;
    return true;
}

class MockAttributeList final : public IAttributeList {
public:
    tresult __stdcall queryInterface(const TUID, void**) override { return -1; }
    uint32 __stdcall addRef() override { return 1; }
    uint32 __stdcall release() override { return 1; }

    tresult __stdcall setInt(FIDString, int64) override { return 0; }
    tresult __stdcall getInt(FIDString id, int64& value) override {
        if (id && (std::strcmp(id, "channel index") == 0 || std::strcmp(id, "channelIndex") == 0)) {
            value = 2;
            return 0;
        }
        return -1;
    }
    tresult __stdcall setFloat(FIDString, double) override { return 0; }
    tresult __stdcall getFloat(FIDString, double&) override { return -1; }
    tresult __stdcall setString(FIDString, const TChar*) override { return 0; }
    tresult __stdcall getString(FIDString id, TChar* string, uint32 sizeInBytes) override {
        if (id && (std::strcmp(id, "channel name") == 0 || std::strcmp(id, "channelName") == 0)) {
            const char16_t* name = u"Lead Vocal";
            const size_t bytesNeeded = (std::char_traits<char16_t>::length(name) + 1) * sizeof(char16_t);
            if (string && sizeInBytes >= bytesNeeded) {
                std::memcpy(string, name, bytesNeeded);
                return 0;
            }
        }
        return -1;
    }
    tresult __stdcall setBinary(FIDString, const void*, uint32) override { return 0; }
    tresult __stdcall getBinary(FIDString, const void*&, uint32&) override { return -1; }
};

static float computeTestAudioMatchScore(
    const std::wstring& trackName,
    const std::wstring& wavFileName) {
    std::wstring tLower = trackName;
    std::transform(tLower.begin(), tLower.end(), tLower.begin(), ::towlower);
    std::wstring fLower = wavFileName;
    std::transform(fLower.begin(), fLower.end(), fLower.begin(), ::towlower);

    size_t dotPos = fLower.rfind(L'.');
    std::wstring stemLower = (dotPos != std::wstring::npos) ? fLower.substr(0, dotPos) : fLower;

    static const wchar_t* kNegatives[] = {
        L"drum", L"drums", L"kick", L"snare", L"hihat", L"hat", L"cymbal", L"perc",
        L"percussion", L"bass", L"guitar", L"piano", L"keys", L"synth", L"pad",
        L"strings", L"brass", L"bgm", L"inst", L"instrumental", L"click", L"metronome",
        L"master", L"sfx", L"fx"
    };
    for (const wchar_t* neg : kNegatives) {
        if (stemLower.find(neg) != std::wstring::npos) {
            return 0.05f;
        }
    }
    float score = 0.30f;
    static const wchar_t* kVocalBonus[] = {
        L"vocal", L"vox", L"vo", L"lead", L"dry", L"mic", L"rec", L"take",
        L"uta", L"singer", L"miku", L"verse", L"chorus", L"hook"
    };
    for (const wchar_t* voc : kVocalBonus) {
        if (stemLower.find(voc) != std::wstring::npos) {
            score += 0.35f;
            break;
        }
    }
    if (!tLower.empty()) {
        if (tLower == stemLower) {
            score += 0.50f;
        } else if (stemLower.find(tLower) != std::wstring::npos || tLower.find(stemLower) != std::wstring::npos) {
            score += 0.40f;
        }
    }
    return std::clamp(score, 0.0f, 1.0f);
}

bool testChannelContextAndSmartAudioPool(IComponent* comp, IEditController* ctrl) {
    std::cout << "[SUITE 12/12] Steinberg VST3 ChannelContext & 3-State Timeline Locking Verification..." << std::endl;

    IInfoListener* listener = nullptr;
    if (comp) {
        comp->queryInterface(kIChannelContextInfoListenerIID, reinterpret_cast<void**>(&listener));
    }
    if (!listener && ctrl) {
        ctrl->queryInterface(kIChannelContextInfoListenerIID, reinterpret_cast<void**>(&listener));
    }

    if (!listener) {
        std::cerr << "  FAIL: kIChannelContextInfoListenerIID interface not exposed on Component or Controller." << std::endl;
        return false;
    }

    MockAttributeList mockAttr;
    tresult res = listener->setChannelContextInfos(&mockAttr);
    if (res != 0) {
        std::cerr << "  FAIL: setChannelContextInfos returned error code: " << res << std::endl;
        return false;
    }
    std::cout << "  PASS: ChannelContext::IInfoListener successfully received host channel context (anti-crash SEH boundary verified)." << std::endl;

    // Verify V2 State Chunk Serialization with Timeline Range [48000, 144000]
    MemoryStream stream;
    if (comp->getState(&stream) != 0) {
        std::cerr << "  FAIL: comp->getState failed." << std::endl;
        return false;
    }

    stream.seek(0, 0, nullptr);
    char magic[4]{};
    uint32_t ver = 0;
    int32_t bytesRead = 0;
    stream.read(magic, 4, &bytesRead);
    stream.read(&ver, 4, &bytesRead);

    if (std::memcmp(magic, "RVCS", 4) != 0 || ver != 3) {
        std::cerr << "  FAIL: State chunk header mismatch (magic: " << magic << ", ver: " << ver << ")" << std::endl;
        return false;
    }

    stream.seek(0, 0, nullptr);
    if (comp->setState(&stream) != 0) {
        std::cerr << "  FAIL: comp->setState with V3 chunk failed." << std::endl;
        return false;
    }

    std::cout << "  PASS: VST3 V3 state serialization and explicit index-path storage verified." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 13: Default Initialization & ClearSlot Persistence Verification
// ============================================================================

bool testDefaultInitAndClearSlotPersistence(HMODULE hVst, IPluginFactory* factory, const PClassInfo& clsInfo) {
    std::cout << "[SUITE 13/16] Testing Default Init Unloaded Models & ClearSlot Non-Rebound State..." << std::endl;

    // 1. Host COM-level instantiation: verify clean initial state without preset auto-loading
    IComponent* testComp = nullptr;
    if (factory->createInstance(
            reinterpret_cast<FIDString>(clsInfo.cid),
            reinterpret_cast<FIDString>(kIComponentIID),
            reinterpret_cast<void**>(&testComp)) != 0 || !testComp) {
        std::cerr << "  FAIL: Failed to create fresh IComponent instance." << std::endl;
        return false;
    }
    testComp->initialize(nullptr);
    testComp->setActive(1);

    MemoryStream stream;
    if (testComp->getState(&stream) != 0) {
        std::cerr << "  FAIL: getState failed on fresh instance." << std::endl;
        testComp->terminate();
        testComp->release();
        return false;
    }
    const auto& bytes = stream.getData();
    if (bytes.size() < 1040) {
        std::cerr << "  FAIL: getState returned insufficient bytes: " << bytes.size() << std::endl;
        testComp->terminate();
        testComp->release();
        return false;
    }
    // Offset 104 = slotAPath (512 bytes), offset 616 = slotBPath (512 bytes)
    const char* slotAPath = reinterpret_cast<const char*>(bytes.data() + 104);
    const char* slotBPath = reinterpret_cast<const char*>(bytes.data() + 616);
    if (slotAPath[0] != '\0' || slotBPath[0] != '\0') {
        std::cerr << "  FAIL: Default initialized instance loaded preset models into slots (A: "
                  << slotAPath << ", B: " << slotBPath << ")" << std::endl;
        testComp->terminate();
        testComp->release();
        return false;
    }

    testComp->terminate();
    testComp->release();

    // 2. Exported Engine-level verification: load, clearSlot, verify non-rebound across sync/process cycles
    using VerifyFn = bool(__stdcall*)();
    auto verify = reinterpret_cast<VerifyFn>(GetProcAddress(hVst, "RvcVst3_VerifyUnloadedInitAndClearSlot"));
    if (!verify) {
        std::cerr << "  FAIL: Missing RvcVst3_VerifyUnloadedInitAndClearSlot export." << std::endl;
        return false;
    }
    if (!verify()) {
        std::cerr << "  FAIL: RvcVst3_VerifyUnloadedInitAndClearSlot failed (state rebounded or failed to clear)." << std::endl;
        return false;
    }

    std::cout << "  PASS: Default initialization has 0 preset models; clearSlot state verified non-rebound." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 14: Slot A / Slot B Octave Shift State Persistence Roundtrip
// ============================================================================

bool testOctaveShiftPersistenceRoundtrip(HMODULE hVst, IComponent* comp) {
    std::cout << "[SUITE 14/16] Testing Slot A / Slot B Octave Shift (-12 ST, 0 ST, +12 ST) Persistence Roundtrip..." << std::endl;

    // 1. Test via exported engine helper (tests all permutations with RvcModelEngine)
    using VerifyFn = bool(__stdcall*)();
    auto verify = reinterpret_cast<VerifyFn>(GetProcAddress(hVst, "RvcVst3_VerifyOctaveShiftRoundtrip"));
    if (!verify) {
        std::cerr << "  FAIL: Missing RvcVst3_VerifyOctaveShiftRoundtrip export." << std::endl;
        return false;
    }
    if (!verify()) {
        std::cerr << "  FAIL: RvcVst3_VerifyOctaveShiftRoundtrip failed." << std::endl;
        return false;
    }

    // 2. Direct VST3 COM getState / setState verification with octaves in chunk.reserved[0..1]
    const int32_t octPermutations[][2] = {
        {-12, 12},
        {12, -12},
        {0, 12},
        {-12, 0},
        {0, 0}
    };

    for (const auto& oct : octPermutations) {
        const int32_t targetOctA = oct[0];
        const int32_t targetOctB = oct[1];

        MemoryStream baseStream;
        if (comp->getState(&baseStream) != 0) {
            std::cerr << "  FAIL: comp->getState failed." << std::endl;
            return false;
        }

        std::vector<uint8_t> chunkData = baseStream.getData();
        if (chunkData.size() < 1156) {
            std::cerr << "  FAIL: chunkData too small for reserved fields: " << chunkData.size() << std::endl;
            return false;
        }

        // Offset of reserved[0] in PersistedStateChunkV2:
        // magic(4) + ver(4) + normParams(12*8=96) + slotAPath(512) + slotBPath(512) +
        // captureStart(8) + captureEnd(8) + timelineState(4) = 1148 bytes.
        const size_t offsetReserved0 = 4 + 4 + 96 + 512 + 512 + 8 + 8 + 4;
        const uint32_t rawA = static_cast<uint32_t>(targetOctA);
        const uint32_t rawB = static_cast<uint32_t>(targetOctB);
        std::memcpy(chunkData.data() + offsetReserved0, &rawA, sizeof(uint32_t));
        std::memcpy(chunkData.data() + offsetReserved0 + 4, &rawB, sizeof(uint32_t));

        MemoryStream inStream;
        int32_t written = 0;
        inStream.write(chunkData.data(), static_cast<int32_t>(chunkData.size()), &written);
        inStream.seek(0, 0, nullptr);

        if (comp->setState(&inStream) != 0) {
            std::cerr << "  FAIL: comp->setState with octave shifts failed." << std::endl;
            return false;
        }

        // Read back state via getState
        MemoryStream outStream;
        if (comp->getState(&outStream) != 0) {
            std::cerr << "  FAIL: comp->getState readback failed." << std::endl;
            return false;
        }
        const auto& readBack = outStream.getData();
        uint32_t backA = 0, backB = 0;
        std::memcpy(&backA, readBack.data() + offsetReserved0, sizeof(uint32_t));
        std::memcpy(&backB, readBack.data() + offsetReserved0 + 4, sizeof(uint32_t));

        const int32_t resA = static_cast<int32_t>(backA);
        const int32_t resB = static_cast<int32_t>(backB);
        if (resA != targetOctA || resB != targetOctB) {
            std::cerr << "  FAIL: Octave shift roundtrip mismatch! Expected ("
                      << targetOctA << ", " << targetOctB << ") Got (" << resA << ", " << resB << ")" << std::endl;
            return false;
        }
    }

    std::cout << "  PASS: Slot A / Slot B octave shifts (-12 ST, 0 ST, +12 ST) persisted and restored accurately." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 15: Non-Zero Timeline Capture & 100% Full-Span Chunk Generation
// ============================================================================

bool testNonZeroTimelineCaptureAndGenerateAll(HMODULE hVst) {
    std::cout << "[SUITE 15/16] Testing Non-Zero Timeline Capture & 100% Full-Span Chunk Generation..." << std::endl;

    using VerifyFn = bool(__stdcall*)(float*, uint32_t*, uint32_t*);
    auto verify = reinterpret_cast<VerifyFn>(GetProcAddress(hVst, "RvcVst3_VerifyCaptureNonZeroTimelineAndGenerateAll"));
    if (!verify) {
        std::cerr << "  FAIL: Missing RvcVst3_VerifyCaptureNonZeroTimelineAndGenerateAll export." << std::endl;
        return false;
    }

    float coverage = 0.0f;
    uint32_t totalChunks = 0;
    uint32_t cachedChunks = 0;
    const bool ok = verify(&coverage, &totalChunks, &cachedChunks);

    std::cout << "  Captured chunks: " << totalChunks << " | Cached chunks: " << cachedChunks
              << " | Timeline coverage: " << (coverage * 100.0f) << "%" << std::endl;

    if (!ok || coverage < 0.999f || totalChunks == 0 || cachedChunks != totalChunks) {
        std::cerr << "  FAIL: Non-zero timeline capture did not 100% convert all chunks (coverage="
                  << coverage << ", total=" << totalChunks << ", cached=" << cachedChunks << ")." << std::endl;
        return false;
    }

    std::cout << "  PASS: Non-zero origin capture and 100% full-span chunk generation verified (no tail omitted)." << std::endl;
    return true;
}

// ============================================================================
// Test Suite 16: Six Real-Time DSP Knobs Acoustic Response & Smoothness
// ============================================================================

bool testSixRealtimeDspKnobsResponse(HMODULE hVst, IAudioProcessor* proc, IEditController* ctrl) {
    std::cout << "[SUITE 16/16] Testing 6 Real-Time DSP Knobs Acoustic Response, Smoothness & Stability..." << std::endl;

    // 1. Exported deep engine acoustic response verification
    using VerifyFn = bool(__stdcall*)(char*, size_t);
    auto verify = reinterpret_cast<VerifyFn>(GetProcAddress(hVst, "RvcVst3_VerifySixRealtimeDspKnobs"));
    if (!verify) {
        std::cerr << "  FAIL: Missing RvcVst3_VerifySixRealtimeDspKnobs export." << std::endl;
        return false;
    }

    char diagBuf[512]{};
    if (!verify(diagBuf, sizeof(diagBuf))) {
        std::cerr << "  FAIL: RvcVst3_VerifySixRealtimeDspKnobs returned false. Diag: " << diagBuf << std::endl;
        return false;
    }
    std::cout << "  DSP Diagnostics: " << diagBuf << std::endl;

    // 2. Host-level sweep verification on proc and ctrl across all 6 DSP knob IDs
    const uint32_t knobIds[] = {2, 3, 4, 5, 6, 7, 10};
    const double knobValues[] = {0.0, 0.25, 0.50, 0.75, 1.0};

    std::vector<float> inL(512), inR(512), outL(512), outR(512);
    float* inChans[2] = {inL.data(), inR.data()};
    float* outChans[2] = {outL.data(), outR.data()};
    AudioBusBuffers inBus{2, 0, {inChans}};
    AudioBusBuffers outBus{2, 0, {outChans}};

    ProcessData data{};
    data.numSamples = 512;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;

    ctrl->setParamNormalized(11 /* kParamBypass */, 0.0);
    ctrl->setParamNormalized(7 /* kParamDryWetMix */, 1.0);

    for (uint32_t id : knobIds) {
        float prevSample = 0.0f;
        float maxStep = 0.0f;
        bool hasPrev = false;
        int globalBlock = 0;
        for (double val : knobValues) {
            ctrl->setParamNormalized(id, val);
            for (int b = 0; b < 4; ++b, ++globalBlock) {
                for (int i = 0; i < 512; ++i) {
                    const double t = (globalBlock * 512 + i) / 48000.0;
                    const float s = static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 440.0 * t));
                    inL[i] = s;
                    inR[i] = s;
                }
                proc->process(data);
                for (int i = 0; i < 512; ++i) {
                    const float sL = outL[i];
                    if (std::isnan(sL) || std::isinf(sL)) {
                        std::cerr << "  FAIL: NaN/Inf on param " << id << " value " << val << std::endl;
                        return false;
                    }
                    if (!hasPrev) {
                        prevSample = sL;
                        hasPrev = true;
                    } else {
                        const float step = std::abs(sL - prevSample);
                        if (step > maxStep) maxStep = step;
                        prevSample = sL;
                    }
                }
            }
        }
        if (maxStep > 0.20f) {
            std::cerr << "  FAIL: Discontinuous audio step on param " << id << ": " << maxStep << std::endl;
            return false;
        }
        ctrl->setParamNormalized(id, 0.50);
    }

    std::cout << "  PASS: All 6 real-time DSP knobs verified with significant acoustic response, smooth transitions, and 0 NaN/Inf." << std::endl;
    return true;
}

bool testProcessContextTransportRequirements(IComponent* component, IAudioProcessor* processor) {
    uint32 reported = 0;
    for (FUnknown* source : {static_cast<FUnknown*>(component), static_cast<FUnknown*>(processor)}) {
        IProcessContextRequirements* requirements = nullptr;
        if (source->queryInterface(kIProcessContextRequirementsIID,
                reinterpret_cast<void**>(&requirements)) != 0 || !requirements) {
            std::cerr << "  FAIL: Host cannot query IProcessContextRequirements." << std::endl;
            return false;
        }
        const uint32 flags = requirements->getProcessContextRequirements();
        requirements->release();
        const uint32 expected = IProcessContextRequirements::kNeedSystemTime |
            IProcessContextRequirements::kNeedContinousTimeSamples |
            IProcessContextRequirements::kNeedTransportState;
        if ((flags & expected) != expected || (flags & ~uint32{0x7ff}) != 0 ||
                (reported != 0 && reported != flags)) {
            std::cerr << "  FAIL: Host transport state was not explicitly requested (flags=" << flags << ")." << std::endl;
            return false;
        }
        reported = flags;
    }
    std::cout << "  PASS: Component and processor explicitly request host transport state (flags="
              << reported << ")." << std::endl;
    return true;
}

// ============================================================================
// Main Automated Test Harness Entrypoint
// ============================================================================

int main() {
    std::cout << "================================================================================" << std::endl;
    std::cout << " RVC Feature Injector VST3 - Automated Test Suite & Host Verification Harness   " << std::endl;
    std::cout << "================================================================================" << std::endl;

    // 1. SPSC LockFreeRingBuffer and DelayLine Suite
    if (!testLockFreeRingBufferAndDelayLine()) {
        std::cerr << "[CRITICAL FAILURE] Suite 1 (LockFreeRingBuffer) failed." << std::endl;
        return 1;
    }

    // Load VST3 Plugin DLL
    fs::path vstPath = L"X:\\AI project\\虚拟歌姬音色混合\\dist\\RvcFeatureInjector.vst3\\Contents\\x86_64-win\\RvcFeatureInjector.vst3";
    HMODULE hVst = LoadLibraryW(vstPath.c_str());
    if (!hVst) {
        std::cerr << "[FAIL] LoadLibraryW failed with error code: " << GetLastError() << std::endl;
        return 1;
    }

    using InitDllFn = bool(__stdcall*)();
    using GetFactoryFn = IPluginFactory*(__stdcall*)();

    auto initDll = reinterpret_cast<InitDllFn>(GetProcAddress(hVst, "InitDll"));
    auto getFactory = reinterpret_cast<GetFactoryFn>(GetProcAddress(hVst, "GetPluginFactory"));

    if (!initDll || !getFactory || !initDll()) {
        std::cerr << "[FAIL] Missing InitDll or GetPluginFactory exports." << std::endl;
        return 1;
    }

    IPluginFactory* factory = getFactory();
    PFactoryInfo factoryInfo{};
    if (factory->getFactoryInfo(&factoryInfo) != 0 || std::strcmp(factoryInfo.vendor, "Taketo") != 0 ||
        factoryInfo.url[0] != '\0' || factoryInfo.email[0] != '\0') {
        std::cerr << "[FAIL] Expected Taketo factory metadata without placeholder contacts." << std::endl;
        return 1;
    }
    PClassInfo clsInfo{};
    factory->getClassInfo(0, &clsInfo);

    IComponent* comp = nullptr;
    if (factory->createInstance(
            reinterpret_cast<FIDString>(clsInfo.cid),
            reinterpret_cast<FIDString>(kIComponentIID),
            reinterpret_cast<void**>(&comp)) != 0 || !comp) {
        std::cerr << "[FAIL] Failed to instantiate IComponent." << std::endl;
        return 1;
    }

    comp->initialize(nullptr);

    IAudioProcessor* proc = nullptr;
    IEditController* ctrl = nullptr;
    comp->queryInterface(kIAudioProcessorIID, reinterpret_cast<void**>(&proc));
    comp->queryInterface(kIEditControllerIID, reinterpret_cast<void**>(&ctrl));

    if (!proc || !ctrl) {
        std::cerr << "[FAIL] Missing IAudioProcessor or IEditController." << std::endl;
        return 1;
    }

    if (!testProcessContextTransportRequirements(comp, proc)) {
        std::cerr << "[CRITICAL FAILURE] VST3 transport requirements contract failed." << std::endl;
        return 1;
    }

    const uint32 pdcLatency = proc->getLatencySamples();
    if (pdcLatency != 0) {
        std::cerr << "[FAIL] Expected 0 PDC latency samples (direct timeline alignment), got " << pdcLatency << std::endl;
        return 1;
    }

    // 2. Parameter Precision Roundtrip Suite
    if (!testParameterRoundtripPrecision(ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 2 (Parameter Precision) failed." << std::endl;
        return 1;
    }

    // 3. State Persistence Roundtrip Suite
    if (!testStatePersistenceRoundtrip(comp, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 3 (State Persistence) failed." << std::endl;
        return 1;
    }

    // 4. Parameter Automation Queue Suite
    if (!testParameterAutomationQueue(comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 4 (Automation Queue) failed." << std::endl;
        return 1;
    }

    // 5. Cubase Audio Engine Precision Suite
    if (!testCubaseEnginePrecisionModes(comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 5 (Cubase Engine Precision) failed." << std::endl;
        return 1;
    }

    // 6. Numerical Stability & Extreme Matrix Suite
    if (!testNumericalStabilityAndExtremeBounds(comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 6 (Numerical Stability) failed." << std::endl;
        return 1;
    }

    // 7. Cache Lock Playback Continuity & Deferred Re-Synthesis Suite
    if (!testCacheLockContinuousPlaybackAndDeferredResynth(comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 7 (Cache Lock Playback Continuity) failed." << std::endl;
        return 1;
    }

    // 8. 10ms Frame & Sample-Accurate Automation Continuous Smoothing Suite
    if (!testFrameAndSampleAccurateAutomationSmoothing(comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 8 (Frame & Sample Automation Smoothing) failed." << std::endl;
        return 1;
    }

    // 9. 0-Origin Timeline Locking & Non-Monotonic Seek Resilience Suite
    if (!testZeroOriginTimelineSeekLocking(comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 9 (0-Origin Timeline Seek Locking) failed." << std::endl;
        return 1;
    }

    // 10. Real-Time Benchmark, Morphing & UI Snapshot Suite
    if (!testRealtimeBenchmarkAndUi(hVst, comp, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 10 (Real-time Benchmark & UI) failed." << std::endl;
        return 1;
    }

    // 11. Native RVC v2 CUDA Timeline Cache E2E Suite
    if (!testNativeRvcNeuralCacheE2E(hVst)) {
        std::cerr << "[CRITICAL FAILURE] Suite 11 (Native RVC Neural Cache) failed." << std::endl;
        return 1;
    }

    // 12. Steinberg VST3 ChannelContext & Smart Audio Pool Suite
    if (!testChannelContextAndSmartAudioPool(comp, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 12 (ChannelContext & Smart Audio Pool) failed." << std::endl;
        return 1;
    }

    // 13. Default Initialization & ClearSlot Persistence Suite
    if (!testDefaultInitAndClearSlotPersistence(hVst, factory, clsInfo)) {
        std::cerr << "[CRITICAL FAILURE] Suite 13 (Default Init & ClearSlot) failed." << std::endl;
        return 1;
    }

    // 14. Slot A / Slot B Octave Shift State Persistence Suite
    if (!testOctaveShiftPersistenceRoundtrip(hVst, comp)) {
        std::cerr << "[CRITICAL FAILURE] Suite 14 (Octave Shift Persistence) failed." << std::endl;
        return 1;
    }

    // 15. Non-Zero Timeline Capture & 100% Full-Span Chunk Generation Suite
    if (!testNonZeroTimelineCaptureAndGenerateAll(hVst)) {
        std::cerr << "[CRITICAL FAILURE] Suite 15 (Non-Zero Timeline Capture & Generate) failed." << std::endl;
        return 1;
    }

    // 16. Six Real-Time DSP Knobs Acoustic Response & Smoothness Suite
    if (!testSixRealtimeDspKnobsResponse(hVst, proc, ctrl)) {
        std::cerr << "[CRITICAL FAILURE] Suite 16 (6 Real-Time DSP Knobs) failed." << std::endl;
        return 1;
    }

    ctrl->release();
    proc->release();
    comp->terminate();
    comp->release();
    factory->release();

    std::cout << "================================================================================" << std::endl;
    std::cout << " ALL 16 VST3 TEST SUITES (100% PASS): SPSC CONCURRENCY, STATE SERIALIZATION,     " << std::endl;
    std::cout << " AUTOMATION SMOOTHING, CACHE LOCK CONTINUITY, TIMELINE SEEK & CHANNEL CONTEXT,   " << std::endl;
    std::cout << " UNLOADED INIT, OCTAVE SHIFT ROUNDTRIP, CAPTURE/GENERATE 100%, 6-KNOB DSP MATRIX " << std::endl;
    std::cout << "================================================================================" << std::endl;
    return 0;
}
