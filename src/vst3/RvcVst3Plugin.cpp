#include "../dsp/RvcFeatureInjector.h"
#include "../inference/RvcModelEngine.h"
#include "../ui/PluginEditorView.h"
#include "../PluginVersion.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rvc_vst {

// ============================================================================
// Steinberg VST3 3.7.x Binary ABI Definitions (Windows x64 COM Compatible)
// ============================================================================

using int8 = int8_t;
using uint8 = uint8_t;
using int16 = int16_t;
using uint16 = uint16_t;
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
using tresult = int32;
using TBool = uint8;
using char8 = char;
using char16 = char16_t;
using TChar = char16;
using FIDString = const char8*;
using ParamID = uint32;
using ParamValue = double;
using Sample32 = float;
using Sample64 = double;
using SpeakerArrangement = uint64;
using MediaType = int32;
using BusDirection = int32;
using BusType = int32;
using IoMode = int32;
using SymbolicSampleSizes = int32;
using TUID = uint8[16];

enum : tresult {
    kNoInterface = -1,
    kResultOk = 0,
    kResultTrue = kResultOk,
    kResultFalse = 1,
    kInvalidArgument = 2,
    kNotImplemented = 3,
    kInternalError = 4
};

constexpr SpeakerArrangement kSpeakerStereo = 0x3ULL; // Left | Right
constexpr SpeakerArrangement kSpeakerMono   = 0x4ULL; // Center

struct Vst3UidPair {
    TUID com;
    TUID be;
};

inline bool iidEqual(const void* incoming, const Vst3UidPair& target) noexcept {
    if (!incoming) return false;
    return std::memcmp(incoming, target.com, 16) == 0 ||
           std::memcmp(incoming, target.be, 16) == 0;
}

// Dual-layout VST3 UID macro:
// - `com`: Windows COM GUID mixed-endian layout (Data1 uint32 LE, Data2 uint16 LE, Data3 uint16 LE, Data4[8] BE)
// - `be`:  Raw big-endian 16-byte layout
#define DECLARE_VST3_UID(name, l1, l2, l3, l4) \
    static const Vst3UidPair name = { \
        { \
            static_cast<uint8>((l1) & 0xFF),        static_cast<uint8>(((l1) >> 8) & 0xFF), \
            static_cast<uint8>(((l1) >> 16) & 0xFF), static_cast<uint8>(((l1) >> 24) & 0xFF), \
            static_cast<uint8>(((l2) >> 16) & 0xFF), static_cast<uint8>(((l2) >> 24) & 0xFF), \
            static_cast<uint8>((l2) & 0xFF),        static_cast<uint8>(((l2) >> 8) & 0xFF), \
            static_cast<uint8>(((l3) >> 24) & 0xFF), static_cast<uint8>(((l3) >> 16) & 0xFF), \
            static_cast<uint8>(((l3) >> 8) & 0xFF),  static_cast<uint8>((l3) & 0xFF), \
            static_cast<uint8>(((l4) >> 24) & 0xFF), static_cast<uint8>(((l4) >> 16) & 0xFF), \
            static_cast<uint8>(((l4) >> 8) & 0xFF),  static_cast<uint8>((l4) & 0xFF) \
        }, \
        { \
            static_cast<uint8>(((l1) >> 24) & 0xFF), static_cast<uint8>(((l1) >> 16) & 0xFF), \
            static_cast<uint8>(((l1) >> 8) & 0xFF),  static_cast<uint8>((l1) & 0xFF), \
            static_cast<uint8>(((l2) >> 24) & 0xFF), static_cast<uint8>(((l2) >> 16) & 0xFF), \
            static_cast<uint8>(((l2) >> 8) & 0xFF),  static_cast<uint8>((l2) & 0xFF), \
            static_cast<uint8>(((l3) >> 24) & 0xFF), static_cast<uint8>(((l3) >> 16) & 0xFF), \
            static_cast<uint8>(((l3) >> 8) & 0xFF),  static_cast<uint8>((l3) & 0xFF), \
            static_cast<uint8>(((l4) >> 24) & 0xFF), static_cast<uint8>(((l4) >> 16) & 0xFF), \
            static_cast<uint8>(((l4) >> 8) & 0xFF),  static_cast<uint8>((l4) & 0xFF) \
        } \
    }

// Standard Steinberg VST3 Interface IIDs
DECLARE_VST3_UID(kFUnknownIID,                    0x00000000, 0x00000000, 0xC0000000, 0x00000046);
DECLARE_VST3_UID(kIPluginBaseIID,                 0x22888DDB, 0x156E45AE, 0x8358B348, 0x08190625);
DECLARE_VST3_UID(kIPluginFactoryIID,              0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
DECLARE_VST3_UID(kIPluginFactory2IID,             0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
DECLARE_VST3_UID(kIPluginFactory3IID,             0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
DECLARE_VST3_UID(kIComponentIID,                  0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
DECLARE_VST3_UID(kIAudioProcessorIID,             0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
DECLARE_VST3_UID(kIProcessContextRequirementsIID, 0x2A654303, 0xEF764E3D, 0x95B5FE83, 0x730EF6D0);
DECLARE_VST3_UID(kIEditControllerIID,             0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
DECLARE_VST3_UID(kIPlugViewIID,                   0x5BC32DFE, 0xD7D94566, 0xB37202E1, 0x933D6B9E);
DECLARE_VST3_UID(kIPlugFrameIID,                  0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3);
// Steinberg official ChannelContext::IInfoListener IID (0x33B10DB4, 0x03564ED7, 0x823AACE6, 0x567D024B)
DECLARE_VST3_UID(kIChannelContextInfoListenerIID, 0x33B10DB4, 0x03564ED7, 0x823AACE6, 0x567D024B);

// Unique Plugin Class UIDs for RVC Feature Injector Processor & Controller
DECLARE_VST3_UID(kRvcFeatureInjectorCID,          0x8A71D4E2, 0x59C14B8F, 0x93A27C1E, 0x6F04D8B5);
DECLARE_VST3_UID(kRvcFeatureControllerCID,        0x8A71D4E2, 0x59C14B8F, 0x93A27C1E, 0x6F04D8B6);

inline void copyAscii(char8* dst, const char* src, size_t maxLen) noexcept {
    std::memset(dst, 0, maxLen);
    if (!src) return;
    std::strncpy(dst, src, maxLen - 1);
}

inline std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

inline void copyWideToChar16(TChar* dst, const wchar_t* src, size_t maxChars) noexcept {
    std::memset(dst, 0, maxChars * sizeof(TChar));
    if (!src) return;
    for (size_t i = 0; i + 1 < maxChars && src[i] != L'\0'; ++i) {
        dst[i] = static_cast<TChar>(src[i]);
    }
}

#pragma pack(push, 8)

struct PFactoryInfo {
    enum { kURLSize = 256, kEmailSize = 128, kNameSize = 64 };
    char8 vendor[kNameSize];
    char8 url[kURLSize];
    char8 email[kEmailSize];
    int32 flags;
};

struct PClassInfo {
    enum { kCategorySize = 32, kNameSize = 64 };
    TUID cid;
    int32 cardinality;
    char8 category[kCategorySize];
    char8 name[kNameSize];
};

struct PClassInfo2 {
    enum { kVendorSize = 64, kVersionSize = 64, kSubCategoriesSize = 128 };
    TUID cid;
    int32 cardinality;
    char8 category[PClassInfo::kCategorySize];
    char8 name[PClassInfo::kNameSize];
    uint32 classFlags;
    char8 subCategories[kSubCategoriesSize];
    char8 vendor[kVendorSize];
    char8 version[kVersionSize];
    char8 sdkVersion[kVersionSize];
};

struct PClassInfoW {
    TUID cid;
    int32 cardinality;
    char8 category[PClassInfo::kCategorySize];
    char16 name[PClassInfo::kNameSize];
    uint32 classFlags;
    char8 subCategories[PClassInfo2::kSubCategoriesSize];
    char16 vendor[PClassInfo2::kVendorSize];
    char16 version[PClassInfo2::kVersionSize];
    char16 sdkVersion[PClassInfo2::kVersionSize];
};

struct BusInfo {
    MediaType mediaType;
    BusDirection direction;
    int32 channelCount;
    TChar name[128];
    BusType busType;
    uint32 flags;
};

struct RoutingInfo {
    MediaType mediaType;
    int32 busIndex;
    int32 channel;
};

struct ProcessSetup {
    int32 processMode;
    SymbolicSampleSizes symbolicSampleSize;
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

struct ViewRect {
    int32 left = 0;
    int32 top = 0;
    int32 right = 0;
    int32 bottom = 0;
    int32 getWidth() const noexcept { return right - left; }
    int32 getHeight() const noexcept { return bottom - top; }
};

#pragma pack(pop)

// Base VST3 Interfaces
class FUnknown {
public:
    virtual tresult __stdcall queryInterface(const TUID _iid, void** obj) = 0;
    virtual uint32 __stdcall addRef() = 0;
    virtual uint32 __stdcall release() = 0;
};

class IBStream : public FUnknown {
public:
    virtual tresult __stdcall read(void* buffer, int32 numBytes, int32* numBytesRead) = 0;
    virtual tresult __stdcall write(void* buffer, int32 numBytes, int32* numBytesWritten) = 0;
    virtual tresult __stdcall seek(int64 pos, int32 mode, int64* result) = 0;
    virtual tresult __stdcall tell(int64* pos) = 0;
};

class IPluginBase : public FUnknown {
public:
    virtual tresult __stdcall initialize(FUnknown* context) = 0;
    virtual tresult __stdcall terminate() = 0;
};

class IComponent : public IPluginBase {
public:
    virtual tresult __stdcall getControllerClassId(TUID classId) = 0;
    virtual tresult __stdcall setIoMode(IoMode mode) = 0;
    virtual int32 __stdcall getBusCount(MediaType type, BusDirection dir) = 0;
    virtual tresult __stdcall getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) = 0;
    virtual tresult __stdcall getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
    virtual tresult __stdcall activateBus(MediaType type, BusDirection dir, int32 index, TBool state) = 0;
    virtual tresult __stdcall setActive(TBool state) = 0;
    virtual tresult __stdcall setState(IBStream* state) = 0;
    virtual tresult __stdcall getState(IBStream* state) = 0;
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

class IAudioProcessor : public FUnknown {
public:
    virtual tresult __stdcall setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns,
        SpeakerArrangement* outputs, int32 numOuts) = 0;
    virtual tresult __stdcall getBusArrangement(
        BusDirection dir, int32 index, SpeakerArrangement& arr) = 0;
    virtual tresult __stdcall canProcessSampleSize(int32 symbolicSampleSize) = 0;
    virtual uint32 __stdcall getLatencySamples() = 0;
    virtual tresult __stdcall setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult __stdcall setProcessing(TBool state) = 0;
    virtual tresult __stdcall process(ProcessData& data) = 0;
    virtual uint32 __stdcall getTailSamples() = 0;
};

class IProcessContextRequirements : public FUnknown {
public:
    enum Flags : uint32 {
        kNeedSystemTime = 1 << 0,
        kNeedContinousTimeSamples = 1 << 1,
        kNeedTransportState = 1 << 10
    };
    virtual uint32 __stdcall getProcessContextRequirements() = 0;
};

class IPlugFrame : public FUnknown {
public:
    virtual tresult __stdcall resizeView(class IPlugView* view, ViewRect* newSize) = 0;
};

class IPlugView : public FUnknown {
public:
    virtual tresult __stdcall isPlatformTypeSupported(FIDString type) = 0;
    virtual tresult __stdcall attached(void* parent, FIDString type) = 0;
    virtual tresult __stdcall removed() = 0;
    virtual tresult __stdcall onWheel(float distance) = 0;
    virtual tresult __stdcall onKeyDown(char16 key, int16 keyCode, int16 modifiers) = 0;
    virtual tresult __stdcall onKeyUp(char16 key, int16 keyCode, int16 modifiers) = 0;
    virtual tresult __stdcall getSize(ViewRect* size) = 0;
    virtual tresult __stdcall onSize(ViewRect* newSize) = 0;
    virtual tresult __stdcall onFocus(TBool state) = 0;
    virtual tresult __stdcall setFrame(IPlugFrame* frame) = 0;
    virtual tresult __stdcall canResize() = 0;
    virtual tresult __stdcall checkSizeConstraint(ViewRect* rect) = 0;
};

class IComponentHandler : public FUnknown {
public:
    virtual tresult __stdcall beginEdit(ParamID id) = 0;
    virtual tresult __stdcall performEdit(ParamID id, ParamValue valueNormalized) = 0;
    virtual tresult __stdcall endEdit(ParamID id) = 0;
    virtual tresult __stdcall restartComponent(int32 flags) = 0;
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
    virtual tresult __stdcall setComponentHandler(IComponentHandler* handler) = 0;
    virtual IPlugView* __stdcall createView(FIDString name) = 0;
};

// Steinberg VST3 Channel Context Info Interfaces (Pushed by Cubase 13)
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

class IPluginFactory : public FUnknown {
public:
    virtual tresult __stdcall getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32 __stdcall countClasses() = 0;
    virtual tresult __stdcall getClassInfo(int32 index, PClassInfo* info) = 0;
    virtual tresult __stdcall createInstance(FIDString cid, FIDString _iid, void** obj) = 0;
};

class IPluginFactory2 : public IPluginFactory {
public:
    virtual tresult __stdcall getClassInfo2(int32 index, PClassInfo2* info) = 0;
};

class IPluginFactory3 : public IPluginFactory2 {
public:
    virtual tresult __stdcall getClassInfoUnicode(int32 index, PClassInfoW* info) = 0;
    virtual tresult __stdcall setHostContext(FUnknown* context) = 0;
};

// ============================================================================
// Helper to locate current DLL module directory
// ============================================================================
std::wstring getCurrentModuleDirectory() {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&getCurrentModuleDirectory),
        &hMod);
    wchar_t pathBuf[MAX_PATH] = {0};
    if (hMod && GetModuleFileNameW(hMod, pathBuf, MAX_PATH) > 0) {
        return std::filesystem::path(pathBuf).parent_path().wstring();
    }
    return L"";
}

// ============================================================================
// VST3 IPlugView Implementation wrapping PluginEditorView
// ============================================================================
class RvcVst3PlugView final : public IPlugView {
public:
    RvcVst3PlugView(
        RvcModelEngine& modelEngine,
        RvcFeatureInjector& dspInjector,
        PluginEditorView::ParamChangeCallback onParamChanged,
        PluginEditorView::ParamGestureCallback onParamGesture)
        : editor_(modelEngine, dspInjector, std::move(onParamChanged), std::move(onParamGesture)) {}

    tresult __stdcall queryInterface(const TUID _iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (iidEqual(_iid, kFUnknownIID) || iidEqual(_iid, kIPlugViewIID)) {
            *obj = static_cast<IPlugView*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 __stdcall addRef() override {
        return ++refCount_;
    }

    uint32 __stdcall release() override {
        const uint32 r = --refCount_;
        if (r == 0) {
            delete this;
        }
        return r;
    }

    tresult __stdcall isPlatformTypeSupported(FIDString type) override {
        if (type && std::strcmp(type, "HWND") == 0) {
            return kResultTrue;
        }
        return kResultFalse;
    }

    tresult __stdcall attached(void* parent, FIDString type) override {
        if (!parent || isPlatformTypeSupported(type) != kResultTrue) {
            return kInvalidArgument;
        }
        return editor_.attachToParentWindow(static_cast<HWND>(parent)) ? kResultOk : kInternalError;
    }

    tresult __stdcall removed() override {
        editor_.detachWindow();
        return kResultOk;
    }

    tresult __stdcall onWheel(float distance) override {
        return editor_.onHostMouseWheel(distance) ? kResultTrue : kResultFalse;
    }
    tresult __stdcall onKeyDown(char16 key, int16 keyCode, int16) override {
        return (key == 27 || keyCode == 6) && editor_.onHostEscape() ? kResultTrue : kResultFalse;
    }
    tresult __stdcall onKeyUp(char16, int16, int16) override { return kResultFalse; }

    tresult __stdcall getSize(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        int w = PluginEditorView::kDefaultWidth;
        int h = PluginEditorView::kDefaultHeight;
        editor_.getWindowSize(w, h);
        size->left = 0;
        size->top = 0;
        size->right = w;
        size->bottom = h;
        return kResultOk;
    }

    tresult __stdcall onSize(ViewRect* newSize) override {
        if (!newSize) return kInvalidArgument;
        editor_.setWindowSize(newSize->getWidth(), newSize->getHeight());
        return kResultOk;
    }

    tresult __stdcall onFocus(TBool /*state*/) override { return kResultOk; }

    tresult __stdcall setFrame(IPlugFrame* frame) override {
        plugFrame_ = frame;
        return kResultOk;
    }

    tresult __stdcall canResize() override { return kResultFalse; }

    tresult __stdcall checkSizeConstraint(ViewRect* rect) override {
        if (!rect) return kInvalidArgument;
        rect->right = rect->left + PluginEditorView::kDefaultWidth;
        rect->bottom = rect->top + PluginEditorView::kDefaultHeight;
        return kResultOk;
    }

private:
    std::atomic<uint32> refCount_{1};
    IPlugFrame* plugFrame_ = nullptr;
    PluginEditorView editor_;
};

// ============================================================================
// Shared Processor + Controller State (Bridges Processor & Controller Instances)
// ============================================================================
struct SharedRvcPluginState {
    RvcModelEngine modelEngine;
    RvcFeatureInjector dspInjector;
    std::array<std::atomic<double>, kNumParameters> normParams{};
    std::mutex initMutex;
    bool modelsInitialized = false;

    SharedRvcPluginState()
        : dspInjector(modelEngine) {}

    void ensureModelsInitialized(const std::wstring& modDir) {
        std::lock_guard<std::mutex> lock(initMutex);
        if (modelsInitialized) return;
        modelEngine.initialize(modDir);
        modelsInitialized = true;
    }
};

// Safe attribute list reader wrapped with Windows Structured Exception Handling (SEH)
// Kept in a standalone static function with no C++ non-trivial destructors to comply with C2712.
static bool safeReadChannelContext(IAttributeList* list, char16* outName, size_t outNameMaxChars, int64* outIndex) noexcept {
    if (!list) return false;
    __try {
        if (outName && outNameMaxChars > 1) {
            const uint32 bufBytes = static_cast<uint32>((outNameMaxChars - 1) * sizeof(char16));
            if (list->getString("channel name", outName, bufBytes) != kResultOk) {
                list->getString("channelName", outName, bufBytes);
            }
        }
        if (outIndex) {
            if (list->getInt("channel index", *outIndex) != kResultOk) {
                list->getInt("channelIndex", *outIndex);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ============================================================================
// VST3 Processor + EditController Implementation
// ============================================================================
class RvcFeatureInjectorVst3 final : public IComponent,
                                     public IAudioProcessor,
                                     public IProcessContextRequirements,
                                     public IEditController,
                                     public IInfoListener {
public:
    explicit RvcFeatureInjectorVst3(std::shared_ptr<SharedRvcPluginState> sharedState = nullptr)
        : state_(sharedState ? std::move(sharedState) : std::make_shared<SharedRvcPluginState>()) {
        // Initialize normalized parameters to factory defaults
        const RvcVoiceParameters defaults = state_->dspInjector.getParameters();
        setNormalizedValueInternal(kParamAbMorph, PluginEditorView::plainToNormalized(kParamAbMorph, defaults.abMorph));
        setNormalizedValueInternal(kParamInjectDepth, PluginEditorView::plainToNormalized(kParamInjectDepth, defaults.injectDepth));
        setNormalizedValueInternal(kParamPitchShift, PluginEditorView::plainToNormalized(kParamPitchShift, defaults.pitchShiftSemitones));
        setNormalizedValueInternal(kParamThroatFormant, PluginEditorView::plainToNormalized(kParamThroatFormant, defaults.throatFormantSemitones));
        setNormalizedValueInternal(kParamPresenceAir, PluginEditorView::plainToNormalized(kParamPresenceAir, defaults.presenceAirDb));
        setNormalizedValueInternal(kParamConsonantProtect, PluginEditorView::plainToNormalized(kParamConsonantProtect, defaults.consonantProtect));
        setNormalizedValueInternal(kParamRmsMixRate, PluginEditorView::plainToNormalized(kParamRmsMixRate, defaults.rmsMixRate));
        setNormalizedValueInternal(kParamDryWetMix, PluginEditorView::plainToNormalized(kParamDryWetMix, defaults.dryWetMix));
        setNormalizedValueInternal(kParamSlotAIndexRate, PluginEditorView::plainToNormalized(kParamSlotAIndexRate, defaults.slotAIndexRate));
        setNormalizedValueInternal(kParamSlotBIndexRate, PluginEditorView::plainToNormalized(kParamSlotBIndexRate, defaults.slotBIndexRate));
        setNormalizedValueInternal(kParamOutputGain, PluginEditorView::plainToNormalized(kParamOutputGain, defaults.outputGainDb));
        setNormalizedValueInternal(kParamBypass, defaults.bypass ? 1.0 : 0.0);
    }

    ~RvcFeatureInjectorVst3() = default;

    // FUnknown
    tresult __stdcall queryInterface(const TUID _iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (iidEqual(_iid, kFUnknownIID) ||
            iidEqual(_iid, kIPluginBaseIID) ||
            iidEqual(_iid, kIComponentIID)) {
            *obj = static_cast<IComponent*>(this);
            addRef();
            return kResultOk;
        }
        if (iidEqual(_iid, kIAudioProcessorIID)) {
            *obj = static_cast<IAudioProcessor*>(this);
            addRef();
            return kResultOk;
        }
        if (iidEqual(_iid, kIProcessContextRequirementsIID)) {
            *obj = static_cast<IProcessContextRequirements*>(this);
            addRef();
            return kResultOk;
        }
        if (iidEqual(_iid, kIEditControllerIID)) {
            *obj = static_cast<IEditController*>(this);
            addRef();
            return kResultOk;
        }
        if (iidEqual(_iid, kIChannelContextInfoListenerIID)) {
            *obj = static_cast<IInfoListener*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    // Steinberg VST3 ChannelContext::IInfoListener
    tresult __stdcall setChannelContextInfos(IAttributeList* list) noexcept override {
        if (!list || list == reinterpret_cast<void*>(this)) return kInvalidArgument;
        char16 chName[128] = {0};
        int64 chIndex = -1;
        if (safeReadChannelContext(list, chName, 128, &chIndex)) {
            chName[127] = 0;
            std::wstring wname(reinterpret_cast<const wchar_t*>(chName));
            state_->dspInjector.setHostTrackContext(wname, static_cast<int32_t>(chIndex));
        }
        return kResultOk;
    }

    uint32 __stdcall addRef() override {
        return ++refCount_;
    }

    uint32 __stdcall release() override {
        const uint32 r = --refCount_;
        if (r == 0) {
            delete this;
        }
        return r;
    }

    // IPluginBase
    tresult __stdcall initialize(FUnknown* /*context*/) override {
        if (initialized_) return kResultOk;
        // IMPORTANT: Do NOT call ensureModelsInitialized() here.
        // Cubase's internal scanner calls initialize() during plugin scanning
        // with a strict timeout (~5s). Spawning the CUDA daemon or loading
        // singer checkpoints here would cause the scan to timeout and blacklist
        // the plugin. Heavy initialization is deferred to setActive(true).
        moduleDir_ = getCurrentModuleDirectory();
        initialized_ = true;
        return kResultOk;
    }

    tresult __stdcall terminate() override {
        initialized_ = false;
        return kResultOk;
    }

    // IComponent
    tresult __stdcall getControllerClassId(TUID classId) override {
        if (!classId) return kInvalidArgument;
        std::memcpy(classId, kRvcFeatureControllerCID.be, sizeof(TUID));
        return kResultOk;
    }

    tresult __stdcall setIoMode(IoMode /*mode*/) override {
        return kResultOk;
    }

    int32 __stdcall getBusCount(MediaType type, BusDirection dir) override {
        if (type == 0 /* kAudio */ && (dir == 0 /* kInput */ || dir == 1 /* kOutput */)) {
            return 1;
        }
        return 0;
    }

    tresult __stdcall getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != 0 || index != 0 || (dir != 0 && dir != 1)) return kInvalidArgument;
        std::memset(&bus, 0, sizeof(bus));
        bus.mediaType = 0; // kAudio
        bus.direction = dir;
        bus.channelCount = (dir == 0) ? ((inArrangement_ == kSpeakerMono) ? 1 : 2)
                                      : ((outArrangement_ == kSpeakerMono) ? 1 : 2);
        copyWideToChar16(bus.name, (dir == 0) ? L"Vocal Audio In" : L"RVC Audio Out", 128);
        bus.busType = 0;   // kMain
        bus.flags = 1;     // kDefaultActive
        return kResultOk;
    }

    tresult __stdcall getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) override {
        outInfo = inInfo;
        return kResultOk;
    }

    tresult __stdcall activateBus(MediaType type, BusDirection /*dir*/, int32 index, TBool state) override {
        if (type != 0 || index != 0) return kInvalidArgument;
        busActive_ = (state != 0);
        return kResultOk;
    }

    tresult __stdcall setActive(TBool state) override {
        if (state) {
            // Deferred heavy initialization: spawn CUDA daemon and load models
            // only when the host actually activates the plugin for audio processing,
            // never during the scan/enumerate phase.
            const std::wstring modDir = moduleDir_.empty() ? getCurrentModuleDirectory() : moduleDir_;
            state_->ensureModelsInitialized(modDir);
            state_->dspInjector.reset();
        }
        return kResultOk;
    }

    #pragma pack(push, 1)
    struct PersistedStateChunkV1 {
        char magic[4]; // "RVCS"
        uint32 version;
        double normParams[kNumParameters];
        char slotAPath[512];
        char slotBPath[512];
        char trackWavPath[512];
    };

    struct PersistedStateChunkV2 {
        char magic[4]; // "RVCS"
        uint32 version; // 2
        double normParams[kNumParameters];
        char slotAPath[512];
        char slotBPath[512];
        int64 captureStartSample;
        int64 captureEndSample;
        uint32 timelineState;
        uint32 reserved[8];
    };
    struct PersistedStateChunkV3 {
        PersistedStateChunkV2 base;
        char slotAIndexPath[512];
        char slotBIndexPath[512];
    };
    #pragma pack(pop)

    tresult __stdcall setState(IBStream* state) override {
        if (!state) return kInvalidArgument;
        char header[8]{};
        int32 bytesRead = 0;
        if (state->read(header, 8, &bytesRead) != kResultOk || bytesRead != 8 || std::memcmp(header, "RVCS", 4) != 0) {
            return kInvalidArgument;
        }
        uint32 ver = 0;
        std::memcpy(&ver, header + 4, 4);

        if (ver == 2 || ver == 3) {
            PersistedStateChunkV2 chunk{};
            std::memcpy(chunk.magic, header, 4);
            chunk.version = ver;
            const int32 remSize = static_cast<int32>(sizeof(PersistedStateChunkV2) - 8);
            if (state->read(reinterpret_cast<char*>(&chunk) + 8, remSize, &bytesRead) == kResultOk && bytesRead == remSize) {
                char indexPaths[2][512]{};
                if (ver == 3 && (state->read(indexPaths, sizeof(indexPaths), &bytesRead) != kResultOk || bytesRead != sizeof(indexPaths))) {
                    return kInvalidArgument;
                }
                indexPaths[0][511] = indexPaths[1][511] = '\0';
                const auto toWide = [](const char* text) {
                    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
                    if (count <= 1) return std::wstring{};
                    std::wstring result(static_cast<size_t>(count), L'\0');
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), count);
                    result.resize(static_cast<size_t>(count - 1));
                    return result;
                };
                chunk.slotAPath[sizeof(chunk.slotAPath) - 1] = '\0';
                chunk.slotBPath[sizeof(chunk.slotBPath) - 1] = '\0';
                for (uint32 i = 0; i < kNumParameters; ++i) {
                    const double val = std::isfinite(chunk.normParams[i]) ? chunk.normParams[i] : 0.0;
                    setParamNormalized(i, val);
                }
                if (chunk.slotAPath[0] != '\0') {
                    const auto wpath = toWide(chunk.slotAPath);
                    if (!wpath.empty()) {
                        if (ver == 3) state_->modelEngine.loadModelIntoSlot(0, wpath, toWide(indexPaths[0]));
                        else state_->modelEngine.loadModelIntoSlot(0, wpath);
                    }
                } else {
                    state_->modelEngine.clearSlot(0);
                }
                if (chunk.slotBPath[0] != '\0') {
                    const auto wpath = toWide(chunk.slotBPath);
                    if (!wpath.empty()) {
                        if (ver == 3) state_->modelEngine.loadModelIntoSlot(1, wpath, toWide(indexPaths[1]));
                        else state_->modelEngine.loadModelIntoSlot(1, wpath);
                    }
                } else {
                    state_->modelEngine.clearSlot(1);
                }
                const int32_t octA = std::clamp(static_cast<int32_t>(chunk.reserved[0]), -24, 24);
                const int32_t octB = std::clamp(static_cast<int32_t>(chunk.reserved[1]), -24, 24);
                state_->modelEngine.setSlotOctaveShift(0, octA);
                state_->modelEngine.setSlotOctaveShift(1, octB);
                // Audio is not persisted in the host state: never activate an empty timeline.
                state_->dspInjector.clearCapture();
            } else return kInvalidArgument;
        } else if (ver == 1) {
            PersistedStateChunkV1 chunk{};
            std::memcpy(chunk.magic, header, 4);
            chunk.version = ver;
            const int32 remSize = static_cast<int32>(sizeof(PersistedStateChunkV1) - 8);
            if (state->read(reinterpret_cast<char*>(&chunk) + 8, remSize, &bytesRead) == kResultOk && bytesRead == remSize) {
                chunk.slotAPath[sizeof(chunk.slotAPath) - 1] = '\0';
                chunk.slotBPath[sizeof(chunk.slotBPath) - 1] = '\0';
                for (uint32 i = 0; i < kNumParameters; ++i) {
                    const double val = std::isfinite(chunk.normParams[i]) ? chunk.normParams[i] : 0.0;
                    setParamNormalized(i, val);
                }
                if (chunk.slotAPath[0] != '\0') {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, chunk.slotAPath, -1, nullptr, 0);
                    if (wlen > 1) {
                        std::wstring wpath(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, chunk.slotAPath, -1, wpath.data(), wlen);
                        wpath.resize(wlen - 1);
                        state_->modelEngine.loadModelIntoSlot(0, wpath);
                    }
                }
                if (chunk.slotBPath[0] != '\0') {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, chunk.slotBPath, -1, nullptr, 0);
                    if (wlen > 1) {
                        std::wstring wpath(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, chunk.slotBPath, -1, wpath.data(), wlen);
                        wpath.resize(wlen - 1);
                        state_->modelEngine.loadModelIntoSlot(1, wpath);
                    }
                }
                state_->dspInjector.clearCapture();
            } else return kInvalidArgument;
        } else return kInvalidArgument;
        return kResultOk;
    }

    tresult __stdcall getState(IBStream* state) override {
        if (!state) return kInvalidArgument;
        PersistedStateChunkV3 persisted{};
        auto& chunk = persisted.base;
        std::memcpy(chunk.magic, "RVCS", 4);
        chunk.version = 3;
        for (uint32 i = 0; i < kNumParameters; ++i) {
            chunk.normParams[i] = state_->normParams[i].load(std::memory_order_relaxed);
        }
        const auto infoA = state_->modelEngine.getSlotSelection(0);
        const auto infoB = state_->modelEngine.getSlotSelection(1);
        copyAscii(chunk.slotAPath, infoA.filePath.c_str(), sizeof(chunk.slotAPath));
        copyAscii(chunk.slotBPath, infoB.filePath.c_str(), sizeof(chunk.slotBPath));
        copyAscii(persisted.slotAIndexPath, infoA.indexPath.c_str(), sizeof(persisted.slotAIndexPath));
        copyAscii(persisted.slotBIndexPath, infoB.indexPath.c_str(), sizeof(persisted.slotBIndexPath));

        int64_t startSample = -1, endSample = -1;
        state_->dspInjector.getCaptureRange(startSample, endSample);
        chunk.captureStartSample = startSample;
        chunk.captureEndSample = endSample;
        chunk.timelineState = static_cast<uint32>(state_->dspInjector.getTimelineState());
        chunk.reserved[0] = static_cast<uint32>(infoA.targetOctaveSemitones);
        chunk.reserved[1] = static_cast<uint32>(infoB.targetOctaveSemitones);

        int32 bytesWritten = 0;
        return state->write(&persisted, sizeof(persisted), &bytesWritten) == kResultOk && bytesWritten == sizeof(persisted)
            ? kResultOk : kInternalError;
    }

    // IAudioProcessor
    tresult __stdcall setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns,
        SpeakerArrangement* outputs, int32 numOuts) override {
        if (numIns < 1 || numOuts < 1 || !inputs || !outputs) return kResultFalse;
        inArrangement_ = inputs[0] ? inputs[0] : kSpeakerStereo;
        outArrangement_ = outputs[0] ? outputs[0] : kSpeakerStereo;
        return kResultOk;
    }

    tresult __stdcall getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        if (index != 0 || (dir != 0 && dir != 1)) return kInvalidArgument;
        arr = (dir == 0) ? inArrangement_ : outArrangement_;
        return kResultOk;
    }

    tresult __stdcall canProcessSampleSize(int32 symbolicSampleSize) override {
        // Supports both 32-bit float (kSample32 = 0) and 64-bit double (kSample64 = 1) Cubase audio engines
        if (symbolicSampleSize == 0 || symbolicSampleSize == 1) {
            return kResultTrue;
        }
        return kResultFalse;
    }

    uint32 __stdcall getLatencySamples() override {
        return state_->dspInjector.getLatencySamples();
    }

    tresult __stdcall setupProcessing(ProcessSetup& setup) override {
        state_->dspInjector.prepare(setup.sampleRate, setup.maxSamplesPerBlock);
        const size_t maxBlock = static_cast<size_t>(std::max<int32>(setup.maxSamplesPerBlock, 4096));
        f64TempInL_.assign(maxBlock, 0.0f);
        f64TempInR_.assign(maxBlock, 0.0f);
        f64TempOutL_.assign(maxBlock, 0.0f);
        f64TempOutR_.assign(maxBlock, 0.0f);
        return kResultOk;
    }

    tresult __stdcall setProcessing(TBool /*state*/) override {
        return kResultOk;
    }

    tresult __stdcall process(ProcessData& data) override {
        const bool hasAudio = data.numSamples > 0 && data.numInputs > 0 && data.numOutputs > 0 &&
            data.inputs && data.outputs && data.inputs[0].numChannels > 0 && data.outputs[0].numChannels > 0 &&
            ((data.symbolicSampleSize == 0 && data.inputs[0].channelBuffers32 && data.outputs[0].channelBuffers32 &&
              data.inputs[0].channelBuffers32[0] && data.outputs[0].channelBuffers32[0]) ||
             (data.symbolicSampleSize == 1 && data.inputs[0].channelBuffers64 && data.outputs[0].channelBuffers64 &&
              data.inputs[0].channelBuffers64[0] && data.outputs[0].channelBuffers64[0]));
        // 1. Apply incoming VST3 parameter automation points from Cubase (all points in IParamValueQueue)
        if (data.inputParameterChanges) {
            const int32 numChanges = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < numChanges; ++i) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
                if (queue) {
                    const ParamID pid = queue->getParameterId();
                    const int32 ptCount = queue->getPointCount();
                    if (ptCount > 0 && pid < kNumParameters) {
                        for (int32 ptIdx = 0; ptIdx < ptCount; ++ptIdx) {
                            int32 offset = 0;
                            ParamValue val = 0.0;
                            if (queue->getPoint(ptIdx, offset, val) == kResultOk && std::isfinite(val)) {
                                const double clampedNorm = std::clamp(val, 0.0, 1.0);
                                const float plain = PluginEditorView::normalizedToPlain(pid, clampedNorm);
                                if (hasAudio) state_->dspInjector.pushAutomationPoint(pid, offset, plain);
                                if (ptIdx == ptCount - 1) {
                                    if (!hasAudio) setNormalizedValueInternal(pid, clampedNorm);
                                    else {
                                        state_->normParams[pid].store(clampedNorm, std::memory_order_relaxed);
                                        if (pid == kParamInjectDepth || pid == kParamDryWetMix) {
                                            state_->normParams[kParamInjectDepth].store(clampedNorm, std::memory_order_relaxed);
                                            state_->normParams[kParamDryWetMix].store(clampedNorm, std::memory_order_relaxed);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0 || !data.inputs || !data.outputs) {
            return kResultOk;
        }

        const AudioBusBuffers& inBus = data.inputs[0];
        AudioBusBuffers& outBus = data.outputs[0];
        if (inBus.numChannels <= 0 || outBus.numChannels <= 0) {
            return kResultOk;
        }

        // Extract Cubase timeline sample position and transport state if provided
        int64 timelinePos = -1;
        bool isPlaying = true;
        if (data.processContext) {
            isPlaying = (data.processContext->state & Vst3ProcessContext::kPlaying) != 0;
            if (data.processContext->projectTimeSamples >= 0) {
                timelinePos = data.processContext->projectTimeSamples;
            }
        }

        // 2. Process 32-bit float or 64-bit double audio stream
        if (data.symbolicSampleSize == 0 && inBus.channelBuffers32 && outBus.channelBuffers32) {
            const float* inL = inBus.channelBuffers32[0];
            const float* inR = (inBus.numChannels >= 2) ? inBus.channelBuffers32[1] : inL;
            float* outL = outBus.channelBuffers32[0];
            float* outR = (outBus.numChannels >= 2) ? outBus.channelBuffers32[1] : nullptr;

            state_->dspInjector.processBlock(inL, inR, outL, outR, data.numSamples, outBus.numChannels, timelinePos, isPlaying);
            outBus.silenceFlags = 0;
        } else if (data.symbolicSampleSize == 1 && inBus.channelBuffers64 && outBus.channelBuffers64) {
            const int32 n = std::min<int32>(data.numSamples, static_cast<int32>(f64TempInL_.size()));
            const double* inL64 = inBus.channelBuffers64[0];
            const double* inR64 = (inBus.numChannels >= 2) ? inBus.channelBuffers64[1] : inL64;
            for (int32 s = 0; s < n; ++s) {
                f64TempInL_[s] = static_cast<float>(inL64[s]);
                f64TempInR_[s] = static_cast<float>(inR64[s]);
            }
            state_->dspInjector.processBlock(
                f64TempInL_.data(), f64TempInR_.data(),
                f64TempOutL_.data(), f64TempOutR_.data(),
                n, outBus.numChannels, timelinePos, isPlaying);

            double* outL64 = outBus.channelBuffers64[0];
            double* outR64 = (outBus.numChannels >= 2) ? outBus.channelBuffers64[1] : nullptr;
            for (int32 s = 0; s < n; ++s) {
                outL64[s] = static_cast<double>(f64TempOutL_[s]);
                if (outR64) outR64[s] = static_cast<double>(f64TempOutR_[s]);
            }
            outBus.silenceFlags = 0;
        }

        return kResultOk;
    }

    uint32 __stdcall getTailSamples() override {
        return state_->dspInjector.getLatencySamples();
    }

    // IProcessContextRequirements (VST3 3.7+)
    uint32 __stdcall getProcessContextRequirements() override {
        // projectTimeSamples is always valid; kPlaying requires an explicit transport request.
        return kNeedSystemTime | kNeedContinousTimeSamples | kNeedTransportState;
    }

    // IEditController
    tresult __stdcall setComponentState(IBStream* state) override {
        return setState(state);
    }

    int32 __stdcall getParameterCount() override {
        return kNumParameters;
    }

    tresult __stdcall getParameterInfo(int32 paramIndex, ParameterInfo& info) override {
        if (paramIndex < 0 || paramIndex >= kNumParameters) return kInvalidArgument;
        std::memset(&info, 0, sizeof(info));
        info.id = static_cast<ParamID>(paramIndex);
        info.unitId = 0;
        info.flags = 1; // kCanAutomate

        switch (paramIndex) {
            case kParamAbMorph:
                copyWideToChar16(info.title, L"A/B Timbre Morph", 128);
                copyWideToChar16(info.shortTitle, L"Morph", 128);
                copyWideToChar16(info.units, L"%", 128);
                info.defaultNormalizedValue = 0.35;
                break;
            case kParamInjectDepth:
                copyWideToChar16(info.title, L"Feature Inject Depth", 128);
                copyWideToChar16(info.shortTitle, L"Inject", 128);
                copyWideToChar16(info.units, L"%", 128);
                info.defaultNormalizedValue = 1.00;
                break;
            case kParamPitchShift:
                copyWideToChar16(info.title, L"Pitch Shift", 128);
                copyWideToChar16(info.shortTitle, L"Pitch", 128);
                copyWideToChar16(info.units, L"ST", 128);
                info.defaultNormalizedValue = 0.50;
                break;
            case kParamThroatFormant:
                copyWideToChar16(info.title, L"Throat Formant Shift", 128);
                copyWideToChar16(info.shortTitle, L"Throat", 128);
                copyWideToChar16(info.units, L"ST", 128);
                info.defaultNormalizedValue = 0.50;
                break;
            case kParamPresenceAir:
                copyWideToChar16(info.title, L"Presence Air Boost", 128);
                copyWideToChar16(info.shortTitle, L"Air", 128);
                copyWideToChar16(info.units, L"dB", 128);
                info.defaultNormalizedValue = 0.50;
                break;
            case kParamConsonantProtect:
                copyWideToChar16(info.title, L"Consonant Protect", 128);
                copyWideToChar16(info.shortTitle, L"Protect", 128);
                copyWideToChar16(info.units, L"", 128);
                info.defaultNormalizedValue = 0.66;
                break;
            case kParamRmsMixRate:
                copyWideToChar16(info.title, L"RMS Envelope Mix", 128);
                copyWideToChar16(info.shortTitle, L"RMS Env", 128);
                copyWideToChar16(info.units, L"%", 128);
                info.defaultNormalizedValue = 0.25;
                break;
            case kParamDryWetMix:
                copyWideToChar16(info.title, L"Dry/Wet Mix", 128);
                copyWideToChar16(info.shortTitle, L"Mix", 128);
                copyWideToChar16(info.units, L"%", 128);
                info.defaultNormalizedValue = 1.00;
                break;
            case kParamSlotAIndexRate:
                copyWideToChar16(info.title, L"Slot A Index Rate", 128);
                copyWideToChar16(info.shortTitle, L"SlotA Idx", 128);
                copyWideToChar16(info.units, L"%", 128);
                info.defaultNormalizedValue = 0.75;
                break;
            case kParamSlotBIndexRate:
                copyWideToChar16(info.title, L"Slot B Index Rate", 128);
                copyWideToChar16(info.shortTitle, L"SlotB Idx", 128);
                copyWideToChar16(info.units, L"%", 128);
                info.defaultNormalizedValue = 0.75;
                break;
            case kParamOutputGain:
                copyWideToChar16(info.title, L"Output Gain", 128);
                copyWideToChar16(info.shortTitle, L"Gain", 128);
                copyWideToChar16(info.units, L"dB", 128);
                info.stepCount = 0;
                info.defaultNormalizedValue = 0.50;
                break;
            case kParamBypass:
                copyWideToChar16(info.title, L"Master Bypass", 128);
                copyWideToChar16(info.shortTitle, L"Bypass", 128);
                info.stepCount = 1;
                info.defaultNormalizedValue = 0.0;
                info.flags = 1 | (1 << 16); // kCanAutomate | kIsBypass
                break;
        }
        return kResultOk;
    }

    tresult __stdcall getParamStringByValue(ParamID id, ParamValue valueNormalized, TChar* string) override {
        if (!string || id >= kNumParameters) return kInvalidArgument;
        const float plain = PluginEditorView::normalizedToPlain(id, valueNormalized);
        wchar_t buf[64]{};
        switch (id) {
            case kParamAbMorph:
            case kParamInjectDepth:
            case kParamRmsMixRate:
            case kParamDryWetMix:
            case kParamSlotAIndexRate:
            case kParamSlotBIndexRate:
                swprintf_s(buf, L"%.0f", plain * 100.0f);
                break;
            case kParamPitchShift:
                swprintf_s(buf, L"%+.1f", plain);
                break;
            case kParamThroatFormant:
                swprintf_s(buf, L"%+.1f", plain);
                break;
            case kParamPresenceAir:
                swprintf_s(buf, L"%+.1f", plain);
                break;
            case kParamConsonantProtect:
                swprintf_s(buf, L"%.2f", plain);
                break;
            case kParamOutputGain:
                swprintf_s(buf, L"%+.1f", plain);
                break;
            case kParamBypass:
                swprintf_s(buf, L"%s", (plain >= 0.5f) ? L"ON" : L"OFF");
                break;
        }
        copyWideToChar16(string, buf, 128);
        return kResultOk;
    }

    tresult __stdcall getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) override {
        if (!string || id >= kNumParameters) return kInvalidArgument;
        wchar_t wbuf[64]{};
        for (size_t i = 0; i < 63 && string[i] != 0; ++i) {
            wbuf[i] = static_cast<wchar_t>(string[i]);
        }
        const float plain = static_cast<float>(_wtof(wbuf));
        valueNormalized = PluginEditorView::plainToNormalized(id, plain);
        return kResultOk;
    }

    ParamValue __stdcall normalizedParamToPlain(ParamID id, ParamValue valueNormalized) override {
        return static_cast<ParamValue>(PluginEditorView::normalizedToPlain(id, valueNormalized));
    }

    ParamValue __stdcall plainParamToNormalized(ParamID id, ParamValue plainValue) override {
        return PluginEditorView::plainToNormalized(id, static_cast<float>(plainValue));
    }

    ParamValue __stdcall getParamNormalized(ParamID id) override {
        if (id >= kNumParameters) return 0.0;
        return state_->normParams[id].load(std::memory_order_relaxed);
    }

    tresult __stdcall setParamNormalized(ParamID id, ParamValue value) override {
        if (id >= kNumParameters) return kInvalidArgument;
        setNormalizedValueInternal(id, value);
        return kResultOk;
    }

    tresult __stdcall setComponentHandler(IComponentHandler* handler) override {
        componentHandler_ = handler;
        return kResultOk;
    }

    IPlugView* __stdcall createView(FIDString name) override {
        if (!name || std::strcmp(name, "editor") != 0) {
            return nullptr;
        }
        return new RvcVst3PlugView(
            state_->modelEngine,
            state_->dspInjector,
            [this](uint32_t paramId, double normVal) {
                state_->normParams[paramId].store(normVal, std::memory_order_relaxed);
                if (componentHandler_) {
                    componentHandler_->performEdit(paramId, normVal);
                }
            },
            [this](uint32_t paramId, bool isBegin) {
                if (componentHandler_) {
                    if (isBegin) {
                        componentHandler_->beginEdit(paramId);
                    } else {
                        componentHandler_->endEdit(paramId);
                    }
                }
            });
    }

    RvcModelEngine& getModelEngine() noexcept { return state_->modelEngine; }
    RvcFeatureInjector& getDspInjector() noexcept { return state_->dspInjector; }

    void forceModelInit() {
        const std::wstring modDir = moduleDir_.empty() ? getCurrentModuleDirectory() : moduleDir_;
        state_->ensureModelsInitialized(modDir);
    }

private:
    void setNormalizedValueInternal(ParamID id, ParamValue normVal) noexcept {
        if (!std::isfinite(normVal)) {
            normVal = 0.0;
        }
        normVal = std::clamp(normVal, 0.0, 1.0);
        state_->normParams[id].store(normVal, std::memory_order_relaxed);

        RvcVoiceParameters p = state_->dspInjector.getParameters();
        const float plain = PluginEditorView::normalizedToPlain(id, normVal);
        switch (id) {
            case kParamAbMorph:          p.abMorph = plain; break;
            case kParamInjectDepth:
                p.injectDepth = plain;
                p.dryWetMix = plain;
                state_->normParams[kParamDryWetMix].store(normVal, std::memory_order_relaxed);
                break;
            case kParamPitchShift:       p.pitchShiftSemitones = plain; break;
            case kParamThroatFormant:    p.throatFormantSemitones = plain; break;
            case kParamPresenceAir:      p.presenceAirDb = plain; break;
            case kParamConsonantProtect: p.consonantProtect = plain; break;
            case kParamRmsMixRate:       p.rmsMixRate = plain; break;
            case kParamDryWetMix:
                p.dryWetMix = plain;
                p.injectDepth = plain;
                state_->normParams[kParamInjectDepth].store(normVal, std::memory_order_relaxed);
                break;
            case kParamSlotAIndexRate:   p.slotAIndexRate = plain; break;
            case kParamSlotBIndexRate:   p.slotBIndexRate = plain; break;
            case kParamOutputGain:       p.outputGainDb = plain; break;
            case kParamBypass:           p.bypass = (plain >= 0.5f); break;
        }
        state_->dspInjector.updateParameters(p);
    }

    std::atomic<uint32> refCount_{1};
    bool initialized_ = false;
    std::wstring moduleDir_;
    bool busActive_ = true;
    SpeakerArrangement inArrangement_ = kSpeakerStereo;
    SpeakerArrangement outArrangement_ = kSpeakerStereo;
    IComponentHandler* componentHandler_ = nullptr;
    std::shared_ptr<SharedRvcPluginState> state_;

    std::vector<float> f64TempInL_;
    std::vector<float> f64TempInR_;
    std::vector<float> f64TempOutL_;
    std::vector<float> f64TempOutR_;
};

// ============================================================================
// VST3 Plugin Factory (IPluginFactory3)
// ============================================================================
class RvcVst3Factory final : public IPluginFactory3 {
public:
    tresult __stdcall queryInterface(const TUID _iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (iidEqual(_iid, kFUnknownIID) ||
            iidEqual(_iid, kIPluginFactoryIID) ||
            iidEqual(_iid, kIPluginFactory2IID) ||
            iidEqual(_iid, kIPluginFactory3IID)) {
            *obj = static_cast<IPluginFactory3*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 __stdcall addRef() override { return 1; }
    uint32 __stdcall release() override { return 1; }

    tresult __stdcall getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        copyAscii(info->vendor, "Taketo", PFactoryInfo::kNameSize);
        info->flags = 0x10; // kUnicode
        return kResultOk;
    }

    int32 __stdcall countClasses() override {
        return 2;
    }

    tresult __stdcall getClassInfo(int32 index, PClassInfo* info) override {
        if ((index != 0 && index != 1) || !info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, (index == 0) ? kRvcFeatureInjectorCID.be : kRvcFeatureControllerCID.be, sizeof(TUID));
        info->cardinality = 0x7FFFFFFF; // kManyInstances
        copyAscii(info->category, (index == 0) ? "Audio Module Class" : "Component Controller Class", PClassInfo::kCategorySize);
        copyAscii(info->name, "RVC Feature Injector", PClassInfo::kNameSize);
        return kResultOk;
    }

    tresult __stdcall getClassInfo2(int32 index, PClassInfo2* info) override {
        if ((index != 0 && index != 1) || !info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, (index == 0) ? kRvcFeatureInjectorCID.be : kRvcFeatureControllerCID.be, sizeof(TUID));
        info->cardinality = 0x7FFFFFFF;
        copyAscii(info->category, (index == 0) ? "Audio Module Class" : "Component Controller Class", PClassInfo::kCategorySize);
        copyAscii(info->name, "RVC Feature Injector", PClassInfo::kNameSize);
        info->classFlags = 0;
        copyAscii(info->subCategories, "Fx", PClassInfo2::kSubCategoriesSize);
        copyAscii(info->vendor, "Taketo", PClassInfo2::kVendorSize);
        copyAscii(info->version, plugin_info::kVersion, PClassInfo2::kVersionSize);
        copyAscii(info->sdkVersion, "VST 3.7.12", PClassInfo2::kVersionSize);
        return kResultOk;
    }

    tresult __stdcall getClassInfoUnicode(int32 index, PClassInfoW* info) override {
        if ((index != 0 && index != 1) || !info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, (index == 0) ? kRvcFeatureInjectorCID.be : kRvcFeatureControllerCID.be, sizeof(TUID));
        info->cardinality = 0x7FFFFFFF;
        copyAscii(info->category, (index == 0) ? "Audio Module Class" : "Component Controller Class", PClassInfo::kCategorySize);
        copyWideToChar16(info->name, L"RVC Feature Injector", PClassInfo::kNameSize);
        info->classFlags = 0;
        copyAscii(info->subCategories, "Fx", PClassInfo2::kSubCategoriesSize);
        copyWideToChar16(info->vendor, L"Taketo", PClassInfo2::kVendorSize);
        copyWideToChar16(info->version, plugin_info::kVersionWide, PClassInfo2::kVersionSize);
        copyWideToChar16(info->sdkVersion, L"VST 3.7.12", PClassInfo2::kVersionSize);
        return kResultOk;
    }

    tresult __stdcall setHostContext(FUnknown* /*context*/) override {
        return kResultOk;
    }

    tresult __stdcall createInstance(FIDString cid, FIDString _iid, void** obj) override {
        if (!cid || !_iid || !obj) return kInvalidArgument;
        const bool isProcessor = iidEqual(cid, kRvcFeatureInjectorCID);
        const bool isController = iidEqual(cid, kRvcFeatureControllerCID);
        if (isProcessor || isController) {
            std::shared_ptr<SharedRvcPluginState> state;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (isProcessor) {
                    state = std::make_shared<SharedRvcPluginState>();
                    lastProcessorState_ = state;
                } else {
                    state = lastProcessorState_.lock();
                    if (!state) {
                        state = std::make_shared<SharedRvcPluginState>();
                        lastProcessorState_ = state;
                    }
                }
            }
            auto* instance = new RvcFeatureInjectorVst3(std::move(state));
            tresult res = instance->queryInterface(reinterpret_cast<const uint8*>(_iid), obj);
            instance->release();
            return res;
        }
        *obj = nullptr;
        return kNoInterface;
    }

private:
    std::mutex stateMutex_;
    std::weak_ptr<SharedRvcPluginState> lastProcessorState_;
};

} // namespace rvc_vst

namespace {
// Test/preview exports require explicit caller selections; normal instances never use these variables.
bool queueExplicitTestModels(rvc_vst::RvcModelEngine& engine) {
    const wchar_t* modelVariables[] = {L"RVC_TEST_MODEL_A", L"RVC_TEST_MODEL_B"};
    const wchar_t* indexVariables[] = {L"RVC_TEST_INDEX_A", L"RVC_TEST_INDEX_B"};
    for (int slot = 0; slot < 2; ++slot) {
        wchar_t checkpoint[32768]{};
        wchar_t index[32768]{};
        const DWORD modelLength = GetEnvironmentVariableW(modelVariables[slot], checkpoint, 32768);
        const DWORD indexLength = GetEnvironmentVariableW(indexVariables[slot], index, 32768);
        if (modelLength == 0 || modelLength >= 32768 || indexLength >= 32768 ||
            !engine.loadModelIntoSlot(slot, checkpoint, index)) {
            std::wcerr << L"Test exports require valid RVC_TEST_MODEL_A/B and optional RVC_TEST_INDEX_A/B paths." << std::endl;
            return false;
        }
    }
    return true;
}
} // namespace

// ============================================================================
// Exported C Entrypoints for Cubase & VST3 Hosts
// ============================================================================
extern "C" {

void* __stdcall GetPluginFactory() {
    static rvc_vst::RvcVst3Factory sFactory;
    return static_cast<rvc_vst::IPluginFactory3*>(&sFactory);
}

bool __stdcall InitDll() { return true; }
bool __stdcall ExitDll() { return true; }
bool __stdcall BundleEntry(void*) { return true; }
bool __stdcall BundleExit() { return true; }

// Asset-backed continuity check at the user's 44.1 kHz / 128-sample host setting.
bool __stdcall RvcVst3_VerifyAppendAudio(const wchar_t* outputDirectory, float* metrics) {
    if (!outputDirectory || !metrics) return false;
    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr); plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine())) return false;
    if (!plugin->getModelEngine().syncDaemonSlotsIfNeeded() ||
        !plugin->getModelEngine().getSlotInfo(0).isLoaded ||
        !plugin->getModelEngine().getSlotInfo(1).isLoaded) {
        std::cerr << "Append model sync: " << plugin->getModelEngine().getLoadingStatusText() << std::endl;
        return false;
    }
    constexpr int sr = 44100, block = 128;
    rvc_vst::ProcessSetup setup{};
    setup.maxSamplesPerBlock = block; setup.sampleRate = sr;
    plugin->setupProcessing(setup); plugin->setActive(1);
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 1.0);
    plugin->setParamNormalized(rvc_vst::kParamRmsMixRate, 0.0);
    const int count = 14*sr + 197;
    const int64_t origin = 23LL*sr + 137;
    std::vector<float> dry(count), wet(count), left(block), right(block);
    double phase = 0;
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(i)/sr;
        const double f0 = 220 + 2.5*std::sin(2*3.141592653589793*5*t);
        phase += f0/sr;
        float value = 0;
        for (int h = 1; h <= 40; ++h) {
            const double hz = h*f0;
            const double envelope = std::exp(-0.5*std::pow((hz-680)/290, 2)) +
                0.6*std::exp(-0.5*std::pow((hz-1950)/460, 2)) + 0.35/h;
            value += static_cast<float>(0.03*envelope*std::sin(2*3.141592653589793*h*phase));
        }
        dry[i] = value;
    }
    auto capture = [&](int first, int end) {
        for (int n = first; n < end; n += block) {
            const float* input = dry.data()+n;
            plugin->getDspInjector().processBlock(input, input, left.data(), right.data(),
                std::min(block, end-n), 2, origin+n, true);
        }
    };
    auto& dsp = plugin->getDspInjector();
    dsp.setTimelineState(rvc_vst::TimelineState::kCapture); capture(2*sr, 7*sr+31);
    dsp.setTimelineState(rvc_vst::TimelineState::kIdle); dsp.beginAppendCapture(); capture(0, 2*sr+59);
    dsp.setTimelineState(rvc_vst::TimelineState::kIdle); dsp.beginAppendCapture(); capture(7*sr-197, count);
    dsp.setTimelineState(rvc_vst::TimelineState::kIdle);
    int64_t first = -1, end = -1; dsp.getCaptureRange(first, end);
    if (first != origin || end != origin+count) return false;
    const auto start = std::chrono::steady_clock::now();
    dsp.triggerGenerate();
    if (!dsp.waitForSynthesisIdle(180000)) {
        std::cerr << "Append synthesis: " << plugin->getModelEngine().getLoadingStatusText() << std::endl;
        return false;
    }
    metrics[0] = std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count();
    dsp.reset();
    for (int n = 0; n < count; n += block) {
        const float* input = dry.data()+n;
        dsp.processBlock(input, input, wet.data()+n, right.data(), std::min(block, count-n), 2, origin+n, true);
    }
    const std::vector<float> baselineWet = wet;
    const int punch = 5*sr+37, punchEnd = punch+sr+59;
    double minimumRepeatRatio = 1.0;
    // Replay the user's arm -> Space -> Space/return-to-start workflow through IAudioProcessor.
    auto hostBlock = [&](const float* input, int n, int64_t position, bool playing) {
        float* inputs[] = {const_cast<float*>(input), const_cast<float*>(input)};
        float* outputs[] = {left.data(), right.data()};
        rvc_vst::AudioBusBuffers inBus{}, outBus{};
        inBus.numChannels = outBus.numChannels = 2;
        inBus.channelBuffers32 = inputs; outBus.channelBuffers32 = outputs;
        rvc_vst::Vst3ProcessContext context{};
        context.state = playing ? rvc_vst::Vst3ProcessContext::kPlaying : 0;
        context.sampleRate = sr; context.projectTimeSamples = position;
        rvc_vst::ProcessData data{};
        data.numSamples = n; data.numInputs = data.numOutputs = 1;
        data.inputs = &inBus; data.outputs = &outBus; data.processContext = &context;
        return plugin->process(data) == rvc_vst::kResultOk;
    };
    std::vector<float> silence(block, 0.0f);
    for (int mode = 0; mode < 3; ++mode) {
        dsp.beginAppendCapture();
        if (!hostBlock(silence.data(), block, origin+punch, false) ||
            dsp.getTimelineState() != rvc_vst::TimelineState::kCapture) return false;
        for (int n = punch; n < punchEnd; n += block)
            if (!hostBlock(dry.data()+n, std::min(block, punchEnd-n), origin+n, true)) return false;
        // Correct stopped flag, a delayed playing flag at return, and a frozen last position.
        const int stopAt = mode == 2 ? punch+((punchEnd-punch-1)/block)*block : punch;
        if (!hostBlock(silence.data(), block, origin+stopAt, mode != 0) ||
            dsp.getTimelineState() != rvc_vst::TimelineState::kIdle) return false;
        if (!hostBlock(silence.data(), block, origin+punch, false)) return false;
        dsp.triggerGenerate();
        if (!dsp.waitForSynthesisIdle(180000)) return false;
        dsp.reset();
        for (int n = 0; n < count; n += block)
            dsp.processBlock(dry.data()+n, dry.data()+n, wet.data()+n, right.data(),
                std::min(block, count-n), 2, origin+n, true);
        for (int center : {punch, punchEnd}) {
            for (int n = center-sr/10; n < center+sr/5; n += sr/1000) {
                double current = 0, reference = 0;
                for (int k = 0; k < sr/50; ++k) {
                    current += wet[n+k]*wet[n+k];
                    reference += baselineWet[n+k]*baselineWet[n+k];
                }
                if (reference > 1e-8)
                    minimumRepeatRatio = std::min(minimumRepeatRatio, std::sqrt(current/reference));
            }
        }
    }
    if (minimumRepeatRatio < 0.98) {
        std::cerr << "Stop/return changed sustained-note energy: " << minimumRepeatRatio << std::endl;
        return false;
    }
    std::cout << "Append stop/return minimum 20ms repeat RMS ratio: " << minimumRepeatRatio << std::endl;
    metrics[1] = dsp.getVisualTelemetry().cacheCoverageRatio;
    auto rms = [&](int startSample, int length) {
        double energy = 0;
        for (int i = startSample; i < startSample+length; ++i) energy += wet[i]*wet[i];
        return std::sqrt(energy/length);
    };
    metrics[2] = 1.0f;
    for (double seconds : {2.0, 7.0, 12.0}) {
        const int center = static_cast<int>(seconds*sr);
        const double baseline = rms(center-sr/3, sr/10);
        if (baseline < 1e-4) return false;
        for (int at = center-sr/10; at < center+sr/10; at += sr/1000)
            metrics[2] = std::min(metrics[2], static_cast<float>(rms(at, sr/200)/baseline));
    }
    int zeroRun = 0, maxZeroRun = 0;
    for (int i = sr/4; i < count-sr/4; ++i) {
        if (!std::isfinite(wet[i])) return false;
        zeroRun = std::abs(wet[i]) < 1e-7f ? zeroRun+1 : 0;
        maxZeroRun = std::max(maxZeroRun, zeroRun);
    }
    metrics[3] = static_cast<float>(1000.0*maxZeroRun/sr);
    double dryEnergy = 0, wetEnergy = 0, cross = 0;
    const int latency = dsp.getLatencySamples();
    for (int i = latency+sr/4; i < count-sr/4; ++i) {
        const double d = dry[i-latency], w = wet[i];
        dryEnergy += d*d; wetEnergy += w*w; cross += d*w;
    }
    metrics[4] = dryEnergy > 0 && wetEnergy > 0
        ? static_cast<float>(cross/std::sqrt(dryEnergy*wetEnergy)) : 1.0f;
    const std::filesystem::path directory(outputDirectory);
    std::error_code error; std::filesystem::create_directories(directory, error);
    if (error) return false;
    auto save = [&](const wchar_t* name, const std::vector<float>& audio) {
        std::ofstream file(directory/name, std::ios::binary);
        const auto u32 = [&](uint32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };
        const auto u16 = [&](uint16_t value) { file.write(reinterpret_cast<const char*>(&value), 2); };
        const uint32_t bytes = static_cast<uint32_t>(audio.size()*sizeof(float));
        file.write("RIFF", 4); u32(36+bytes); file.write("WAVEfmt ", 8); u32(16);
        u16(3); u16(1); u32(sr); u32(sr*4); u16(4); u16(32);
        file.write("data", 4); u32(bytes); file.write(reinterpret_cast<const char*>(audio.data()), bytes);
        return static_cast<bool>(file);
    };
    return save(L"append-dry-44100.wav", dry) && save(L"append-wet-44100.wav", wet) &&
        metrics[1] >= 0.999f && metrics[2] > 0.05f && metrics[3] < 1.0f && std::abs(metrics[4]) < 0.97f;
}

// Repeat unchanged audio and replay at the edit cursor, including the very first sample.
bool __stdcall RvcVst3_VerifyAppendReplay(const float* source, uint32_t samples, uint32_t punch,
    const wchar_t* outputDirectory, float* metrics) {
    constexpr int sr = 44100, block = 128;
    if (!source || !metrics || !outputDirectory || samples < sr || punch < 256 || punch+sr/2 >= samples) return false;
    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr); plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine()) || !plugin->getModelEngine().syncDaemonSlotsIfNeeded()) return false;
    rvc_vst::ProcessSetup setup{}; setup.sampleRate = sr; setup.maxSamplesPerBlock = block;
    plugin->setupProcessing(setup); plugin->setActive(1);
    auto& dsp = plugin->getDspInjector();
    auto parameters = dsp.getParameters(); parameters.dryWetMix = 1; parameters.rmsMixRate = 0;
    dsp.updateParameters(parameters); dsp.reset();
    constexpr int64_t origin = 23LL*sr+137;
    std::vector<float> left(block), right(block), live(block), baseline(samples), after(samples);
    auto record = [&](uint32_t first, uint32_t end) {
        for (uint32_t n = first; n < end; n += block)
            dsp.processBlock(source+n, source+n, left.data(), right.data(), std::min<uint32_t>(block,end-n),
                2, origin+n, true);
    };
    auto render = [&](float* output, uint32_t first, uint32_t count, float inputValue) {
        std::fill(live.begin(), live.end(), inputValue);
        dsp.reset();
        for (uint32_t n = 0; n < count; n += block)
            dsp.processBlock(live.data(), live.data(), output+n, right.data(), std::min<uint32_t>(block,count-n),
                2, origin+first+n, true);
    };
    dsp.setTimelineState(rvc_vst::TimelineState::kCapture); record(0,samples);
    dsp.triggerGenerate(); if (!dsp.waitForSynthesisIdle(180000)) return false;
    render(baseline.data(),0,samples,0);
    dsp.beginAppendCapture(); record(punch,std::min<uint32_t>(samples,punch+sr));
    // Generate directly while Capture is armed, matching the reported workflow.
    dsp.triggerGenerate(); if (!dsp.waitForSynthesisIdle(180000)) return false;
    const auto telemetry = dsp.getVisualTelemetry();
    metrics[0] = telemetry.appendReusedCache ? 1.0f : 0.0f;
    metrics[1] = static_cast<float>(telemetry.appendChangedSamples);
    render(after.data(),0,samples,0);
    metrics[2] = 0;
    for (uint32_t i = 0; i < samples; ++i) metrics[2] = std::max(metrics[2],std::abs(after[i]-baseline[i]));
    const uint32_t count = std::min<uint32_t>(sr/2,samples-punch);
    std::vector<float> reference(count), replay(count);
    metrics[3] = metrics[4] = 0;
    for (float rms : {0.0f,0.25f,1.0f}) {
        parameters.rmsMixRate = rms; dsp.updateParameters(parameters);
        render(reference.data(),punch,count,0);
        for (float input : {-10.0f,10.0f}) {
            render(replay.data(),punch,count,input);
            for (uint32_t i = 0; i < count; ++i)
                metrics[3] = std::max(metrics[3],std::abs(replay[i]-reference[i]));
        }
        if (rms == 0) for (uint32_t i = 0; i < count; ++i)
            metrics[4] = std::max(metrics[4],std::abs(reference[i]-baseline[punch+i]));
    }
    std::filesystem::path folder(outputDirectory);
    std::error_code error; std::filesystem::create_directories(folder,error);
    if (error) return false;
    auto save = [&](const wchar_t* name, const float* data, size_t size) {
        std::ofstream file(folder/name,std::ios::binary);
        file.write(reinterpret_cast<const char*>(data),size*sizeof(float)); return static_cast<bool>(file);
    };
    return save(L"baseline-wet.f32",baseline.data(),baseline.size()) &&
        save(L"after-append-wet.f32",after.data(),after.size()) &&
        metrics[0] == 1 && metrics[1] == 0 && metrics[2] == 0 && metrics[3] == 0 && metrics[4] < 2e-7f;
}

// Exercise the actual editor hit regions with a hidden project-local test window.
bool __stdcall RvcVst3_VerifyAppendCaptureUi(const wchar_t* outputDirectory) {
    if (!outputDirectory) return false;
    const std::filesystem::path output(outputDirectory);
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error) return false;
    rvc_vst::RvcModelEngine engine;
    rvc_vst::RvcFeatureInjector dsp(engine);
    rvc_vst::PluginEditorView view(engine, dsp, nullptr);
    HWND parent = CreateWindowExW(0, L"STATIC", L"Capture editor verification", WS_OVERLAPPED,
        0, 0, 1040, 580, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) return false;
    bool passed = false;
    do {
        if (!view.attachToParentWindow(parent)) break;
        const HWND child = FindWindowExW(parent, nullptr, L"RvcFeatureInjectorVst3ViewClass", nullptr);
        if (!child) break;
        auto snapshot = [&](const wchar_t* name) { return view.renderSnapshotToPng((output / name).wstring()); };
        auto click = [&](int x) {
            SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 37));
            SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(x, 37));
        };
        auto feed = [&](int64_t start, int count, float value) {
            std::vector<float> input(512, value), audio(512);
            for (int n = 0; n < count; n += 512)
                dsp.processBlock(input.data(), nullptr, audio.data(), nullptr,
                    std::min(512, count - n), 1, start + n, true);
        };
        if (!snapshot(L"capture-empty.png")) break;
        click(100);
        if (!view.isAboutPanelVisible() || !snapshot(L"about-version.png")) break;
        if (!view.onHostEscape() || view.isAboutPanelVisible() ||
            dsp.getTimelineState() != rvc_vst::TimelineState::kIdle) break;
        click(740); // Disabled append cannot start a destructive capture.
        if (dsp.getTimelineState() != rvc_vst::TimelineState::kIdle) break;
        click(639);
        if (dsp.getTimelineState() != rvc_vst::TimelineState::kCapture || dsp.getVisualTelemetry().isAppendCapture) break;
        feed(30 * 48000, 48000, 0.15f);
        click(639);
        if (dsp.getTimelineState() != rvc_vst::TimelineState::kIdle || !snapshot(L"capture-recorded.png")) break;
        click(740);
        if (dsp.getTimelineState() != rvc_vst::TimelineState::kCapture || !dsp.getVisualTelemetry().isAppendCapture) break;
        if (!snapshot(L"capture-append-armed.png")) break;
        feed(28 * 48000, 48000, 0.2f);
        std::vector<float> stopInput(512, 0.0f), stopOutput(512);
        dsp.processBlock(stopInput.data(), nullptr, stopOutput.data(), nullptr, 512, 1, 28*48000, false);
        if (dsp.getTimelineState() != rvc_vst::TimelineState::kIdle ||
            !snapshot(L"capture-append-stopped.png")) break;
        click(740);
        feed(32 * 48000, 48000, 0.25f);
        if (!snapshot(L"capture-appending.png")) break;
        click(740);
        int64_t start = -1, end = -1;
        dsp.getCaptureRange(start, end);
        if (dsp.getTimelineState() != rvc_vst::TimelineState::kIdle || start != 28 * 48000 || end != 33 * 48000) break;
        if (!snapshot(L"capture-merged.png")) break;
        click(840);
        if (!dsp.waitForSynthesisIdle(4000) || dsp.getTimelineState() != rvc_vst::TimelineState::kActive) break;
        if (!snapshot(L"capture-active.png")) break;
        click(639);
        feed(90 * 48000, 48000, 0.1f);
        click(639);
        dsp.getCaptureRange(start, end);
        if (start != 90 * 48000 || end != 91 * 48000) break;
        passed = true;
    } while (false);
    view.detachWindow();
    DestroyWindow(parent);
    return passed;
}

/**
 * Helper entrypoint for automated UI inspection & screenshot rendering.
 * Instantiates the plugin, runs a realistic harmonic vocal test signal through
 * the RVC ONNX + STFT feature injection pipeline so the live spectral scope
 * shows active envelopes, and renders the Direct2D UI to a PNG file.
 */
bool __stdcall RvcVst3_RenderUiPreviewPng(const wchar_t* pngPath) {
    if (!pngPath) return false;

    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr);
    // For test/preview entrypoints, explicitly trigger model initialization
    // and load two models so the preview showcases the dual-model Three-Way Balance & waveform UI.
    plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine())) return false;
    plugin->getModelEngine().setSlotOctaveShift(0, 12);

    rvc_vst::ProcessSetup setup{};
    setup.processMode = 0;
    setup.symbolicSampleSize = 0;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    plugin->setupProcessing(setup);
    plugin->setActive(1);
    plugin->setParamNormalized(rvc_vst::kParamAbMorph, 0.494);
    plugin->setParamNormalized(rvc_vst::kParamInjectDepth, 0.89);
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 0.89);
    plugin->setParamNormalized(rvc_vst::kParamOutputGain, 0.50);

    // 1. Capture 24 blocks of realistic harmonic sung phrase with dynamic envelope variation
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kCapture);
    std::vector<float> inL(512), inR(512), outL(512), outR(512);
    double phase = 0.0;
    for (int block = 0; block < 24; ++block) {
        const double phraseEnv = 0.35 + 0.65 * std::sin(3.14159265 * (static_cast<double>(block) + 0.5) / 24.0);
        for (int i = 0; i < 512; ++i) {
            const double t = (block * 512 + i) / 48000.0;
            const double f0 = 220.0 + 4.5 * std::sin(2.0 * 3.14159265 * 5.5 * t);
            phase += f0 / 48000.0;
            float sample = 0.0f;
            for (int h = 1; h <= 65; ++h) {
                const double hf = h * f0;
                const double env = std::exp(-0.5 * std::pow((hf - 680.0) / 290.0, 2.0)) * 0.95 +
                                   std::exp(-0.5 * std::pow((hf - 1950.0) / 460.0, 2.0)) * 0.65 +
                                   std::exp(-0.5 * std::pow((hf - 3250.0) / 620.0, 2.0)) * 0.42 +
                                   std::exp(-0.5 * std::pow((hf - 7800.0) / 2400.0, 2.0)) * 0.14 +
                                   0.45 / std::pow(static_cast<double>(h), 0.78);
                sample += static_cast<float>(0.038 * phraseEnv * env * std::sin(2.0 * 3.14159265 * h * phase + h * 0.37));
            }
            inL[i] = sample;
            inR[i] = sample;
        }
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2,
            static_cast<int64_t>(block) * 512, true);
        Sleep(1);
    }

    // 2. Trigger Generate and wait for the CUDA RVC v2 neural synthesis worker to complete
    plugin->getDspInjector().triggerGenerate();
    plugin->getDspInjector().waitForSynthesisIdle(45000);

    // 3. Replay 10 blocks in Active state so playhead and telemetry are live mid-phrase
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kActive);
    for (int block = 0; block < 10; ++block) {
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2,
            static_cast<int64_t>(block) * 512, true);
    }

    rvc_vst::PluginEditorView view(
        plugin->getModelEngine(),
        plugin->getDspInjector(),
        nullptr);
    return view.renderSnapshotToPng(pngPath);
}

/**
 * Test-only entrypoint: end-to-end verification of the native RVC v2 CUDA timeline cache.
 * Feeds 1.5 s of a sung vowel along the timeline, waits for daemon synthesis, replays the
 * same timeline range and reports cache coverage, wet output RMS and dry/wet correlation.
 */
bool __stdcall RvcVst3_VerifyNeuralCacheE2E(
    float* outCoverage, float* outWetRms, float* outDryCorrelation, float* outSynthSeconds) {
    if (!outCoverage || !outWetRms || !outDryCorrelation || !outSynthSeconds) return false;

    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr);
    // For test entrypoints, explicitly trigger model initialization and load both slots
    plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine())) return false;
    rvc_vst::ProcessSetup setup{};
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    plugin->setupProcessing(setup);
    plugin->setActive(1);
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 1.0);
    plugin->setParamNormalized(rvc_vst::kParamBypass, 0.0);
    plugin->setParamNormalized(rvc_vst::kParamOutputGain, 0.50);

    // 1. Arm timeline capture state
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kCapture);

    constexpr int kBlocks = 141; // ~1.5 s @ 48 kHz
    std::vector<float> dry(static_cast<size_t>(kBlocks) * 512);
    double phase = 0.0;
    for (size_t i = 0; i < dry.size(); ++i) {
        const double t = i / 48000.0;
        const double f0 = 220.0 + 4.5 * std::sin(2.0 * 3.14159265 * 5.5 * t);
        phase += f0 / 48000.0;
        float s = 0.0f;
        for (int h = 1; h <= 40; ++h) {
            const double hf = h * f0;
            const double env = std::exp(-0.5 * std::pow((hf - 680.0) / 290.0, 2.0)) +
                               0.6 * std::exp(-0.5 * std::pow((hf - 1950.0) / 460.0, 2.0)) +
                               0.35 / h;
            s += static_cast<float>(0.03 * env * std::sin(2.0 * 3.14159265 * h * phase));
        }
        dry[i] = s;
    }

    std::vector<float> outL(512), outR(512);
    for (int b = 0; b < kBlocks; ++b) {
        const float* in = dry.data() + static_cast<size_t>(b) * 512;
        plugin->getDspInjector().processBlock(in, in, outL.data(), outR.data(), 512, 2,
                                              static_cast<int64_t>(b) * 512, true);
    }

    // 2. Trigger background RVC neural synthesis
    plugin->getDspInjector().triggerGenerate();

    const auto t0 = std::chrono::steady_clock::now();
    const bool idle = plugin->getDspInjector().waitForSynthesisIdle(60000);
    if (!idle) std::cerr << "Neural cache: " << plugin->getModelEngine().getLoadingStatusText() << std::endl;
    *outSynthSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();

    // 3. Replay timeline in Active state with neural cache
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kActive);
    std::vector<float> wet;
    wet.reserve(dry.size());
    for (int b = 0; b < kBlocks; ++b) {
        const float* in = dry.data() + static_cast<size_t>(b) * 512;
        plugin->getDspInjector().processBlock(in, in, outL.data(), outR.data(), 512, 2,
                                              static_cast<int64_t>(b) * 512, true);
        wet.insert(wet.end(), outL.begin(), outL.end());
    }

    const auto tel = plugin->getDspInjector().getVisualTelemetry();
    *outCoverage = tel.cacheCoverageRatio;

    const size_t lat = plugin->getDspInjector().getLatencySamples();
    double ew = 0.0, ed = 0.0, cross = 0.0;
    size_t n = 0;
    for (size_t i = lat + 4800; i < wet.size(); ++i) {
        const double w = wet[i];
        const double d = dry[i - lat];
        ew += w * w;
        ed += d * d;
        cross += w * d;
        ++n;
    }
    *outWetRms = n ? static_cast<float>(std::sqrt(ew / n)) : 0.0f;
    *outDryCorrelation = (ew > 0.0 && ed > 0.0) ? static_cast<float>(cross / std::sqrt(ew * ed)) : 1.0f;
    plugin->terminate();
    return idle;
}

namespace {
class LocalMemStream final : public rvc_vst::IBStream {
public:
    rvc_vst::tresult __stdcall queryInterface(const rvc_vst::TUID, void** obj) override {
        if (!obj) return rvc_vst::kInvalidArgument;
        *obj = nullptr;
        return rvc_vst::kNoInterface;
    }
    rvc_vst::uint32 __stdcall addRef() override { return ++ref_; }
    rvc_vst::uint32 __stdcall release() override {
        const rvc_vst::uint32 r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    rvc_vst::tresult __stdcall read(void* buffer, rvc_vst::int32 numBytes, rvc_vst::int32* numBytesRead) override {
        if (!buffer || numBytes < 0) return rvc_vst::kInvalidArgument;
        const rvc_vst::int32 avail = static_cast<rvc_vst::int32>(buf_.size() - pos_);
        const rvc_vst::int32 toRead = (std::min)(numBytes, (std::max)(0, avail));
        if (toRead > 0) {
            std::memcpy(buffer, buf_.data() + pos_, toRead);
            pos_ += toRead;
        }
        if (numBytesRead) *numBytesRead = toRead;
        return (toRead == numBytes) ? rvc_vst::kResultOk : rvc_vst::kResultFalse;
    }
    rvc_vst::tresult __stdcall write(void* buffer, rvc_vst::int32 numBytes, rvc_vst::int32* numBytesWritten) override {
        if (!buffer || numBytes < 0) return rvc_vst::kInvalidArgument;
        if (pos_ + numBytes > static_cast<rvc_vst::int64>(buf_.size())) {
            buf_.resize(static_cast<size_t>(pos_ + numBytes));
        }
        std::memcpy(buf_.data() + pos_, buffer, numBytes);
        pos_ += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return rvc_vst::kResultOk;
    }
    rvc_vst::tresult __stdcall seek(rvc_vst::int64 pos, rvc_vst::int32 mode, rvc_vst::int64* result) override {
        if (mode == 0) pos_ = pos;
        else if (mode == 1) pos_ += pos;
        else if (mode == 2) pos_ = static_cast<rvc_vst::int64>(buf_.size()) + pos;
        else return rvc_vst::kInvalidArgument;
        if (pos_ < 0) pos_ = 0;
        if (result) *result = pos_;
        return rvc_vst::kResultOk;
    }
    rvc_vst::tresult __stdcall tell(rvc_vst::int64* pos) override {
        if (!pos) return rvc_vst::kInvalidArgument;
        *pos = pos_;
        return rvc_vst::kResultOk;
    }
    void resetPos() noexcept { pos_ = 0; }
    void clear() noexcept { buf_.clear(); pos_ = 0; }
    const std::vector<uint8_t>& getData() const noexcept { return buf_; }
private:
    std::atomic<rvc_vst::uint32> ref_{1};
    std::vector<uint8_t> buf_;
    rvc_vst::int64 pos_ = 0;
};
} // namespace

/**
 * Regression Assertion 1:
 * Verify default initialization does NOT load any preset models, and clearSlot
 * completely resets slot state without rebounding across subsequent sync/process cycles.
 */
bool __stdcall RvcVst3_VerifyUnloadedInitAndClearSlot() {
    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    if (plugin->initialize(nullptr) != rvc_vst::kResultOk) return false;
    if (plugin->setActive(1) != rvc_vst::kResultOk) return false;

    // 1. Verify default initialization: neither slot should be loaded
    const auto initA = plugin->getModelEngine().getSlotInfo(0);
    const auto initB = plugin->getModelEngine().getSlotInfo(1);
    if (initA.isLoaded || !initA.filePath.empty() || initA.modelName != "UNLOADED") {
        return false;
    }
    if (initB.isLoaded || !initB.filePath.empty() || initB.modelName != "UNLOADED") {
        return false;
    }

    // Check VST3 state chunk: should have empty slot paths
    LocalMemStream stream;
    if (plugin->getState(&stream) != 0) return false;
    const auto& bytes = stream.getData();
    if (bytes.size() < sizeof(rvc_vst::RvcFeatureInjectorVst3::PersistedStateChunkV2)) return false;
    const auto* chunk = reinterpret_cast<const rvc_vst::RvcFeatureInjectorVst3::PersistedStateChunkV2*>(bytes.data());
    if (chunk->slotAPath[0] != '\0' || chunk->slotBPath[0] != '\0') {
        return false;
    }

    // 2. Import the test caller's explicit checkpoint/index selections
    plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine())) return false;
    if (!plugin->getModelEngine().syncDaemonSlotsIfNeeded()) return false;

    const auto loadedA = plugin->getModelEngine().getSlotInfo(0);
    const auto loadedB = plugin->getModelEngine().getSlotInfo(1);
    if (!loadedA.isLoaded || loadedA.filePath.empty()) return false;
    if (!loadedB.isLoaded || loadedB.filePath.empty()) return false;

    // 3. Clear Slot A and Slot B
    plugin->getModelEngine().clearSlot(0);
    plugin->getModelEngine().clearSlot(1);
    if (!plugin->getModelEngine().syncDaemonSlotsIfNeeded()) return false;

    const auto clearA = plugin->getModelEngine().getSlotInfo(0);
    const auto clearB = plugin->getModelEngine().getSlotInfo(1);
    if (clearA.isLoaded || !clearA.filePath.empty() || clearA.modelName != "UNLOADED") return false;
    if (clearB.isLoaded || !clearB.filePath.empty() || clearB.modelName != "UNLOADED") return false;

    // 4. Verify no bounce back ("不反弹") across multiple syncs and audio processing blocks
    plugin->getModelEngine().syncDaemonSlotsIfNeeded();

    std::vector<float> inL(512, 0.2f), inR(512, 0.2f), outL(512), outR(512);
    for (int b = 0; b < 12; ++b) {
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, b * 512, true);
    }
    plugin->getModelEngine().syncDaemonSlotsIfNeeded();

    const auto finalA = plugin->getModelEngine().getSlotInfo(0);
    const auto finalB = plugin->getModelEngine().getSlotInfo(1);
    if (finalA.isLoaded || !finalA.filePath.empty() || finalA.modelName != "UNLOADED" ||
        finalB.isLoaded || !finalB.filePath.empty() || finalB.modelName != "UNLOADED") {
        return false; // Rebounded!
    }

    // Check state chunk again after clear: paths must remain empty
    stream.clear();
    if (plugin->getState(&stream) != 0) return false;
    const auto* chunk2 = reinterpret_cast<const rvc_vst::RvcFeatureInjectorVst3::PersistedStateChunkV2*>(stream.getData().data());
    if (chunk2->slotAPath[0] != '\0' || chunk2->slotBPath[0] != '\0') {
        return false;
    }

    plugin->terminate();
    return true;
}

/**
 * Regression Assertion 2:
 * Verify Slot A / Slot B octave shifts (-12 ST, 0 ST, +12 ST) persist accurately
 * through getState / setState roundtrips across all permutations.
 */
bool __stdcall RvcVst3_VerifyOctaveShiftRoundtrip() {
    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr);
    plugin->setActive(1);

    const int32_t testCases[][2] = {
        {-12, 12},
        {12, -12},
        {0, 12},
        {-12, 0},
        {0, 0},
        {12, 12},
        {-12, -12}
    };

    for (const auto& tc : testCases) {
        const int32_t octA = tc[0];
        const int32_t octB = tc[1];

        plugin->getModelEngine().setSlotOctaveShift(0, octA);
        plugin->getModelEngine().setSlotOctaveShift(1, octB);

        if (plugin->getModelEngine().getSlotOctaveShift(0) != octA ||
            plugin->getModelEngine().getSlotOctaveShift(1) != octB) {
            return false;
        }

        LocalMemStream stream;
        if (plugin->getState(&stream) != 0) return false;

        // Mutate slots to 0 or inverted prior to restore
        plugin->getModelEngine().setSlotOctaveShift(0, 0);
        plugin->getModelEngine().setSlotOctaveShift(1, 0);

        // Restore via setState
        stream.resetPos();
        if (plugin->setState(&stream) != 0) return false;

        const int32_t restoredA = plugin->getModelEngine().getSlotOctaveShift(0);
        const int32_t restoredB = plugin->getModelEngine().getSlotOctaveShift(1);
        if (restoredA != octA || restoredB != octB) {
            return false;
        }
    }

    plugin->terminate();
    return true;
}

/**
 * Regression Assertion 3:
 * Verify Capture on a non-zero timeline origin with multiple continuous audio blocks
 * converts 100% of all captured intervals upon clicking Generate, with zero omitted tail chunks.
 */
bool __stdcall RvcVst3_VerifyCaptureNonZeroTimelineAndGenerateAll(
    float* outCoverage, uint32_t* outTotalChunks, uint32_t* outCachedChunks) {
    if (!outCoverage || !outTotalChunks || !outCachedChunks) return false;

    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr);
    plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine())) return false;

    rvc_vst::ProcessSetup setup{};
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    plugin->setupProcessing(setup);
    plugin->setActive(1);

    // 1. Arm Capture State
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kCapture);

    // Non-zero starting project sample: e.g. 192000 (4.0s into the project timeline)
    const int64_t nonZeroStart = 192000;
    // Capture 7.5 seconds of vocal audio @ 48kHz = 360,448 samples.
    // Chunk length is 6.0s (288,000 samples).
    // 360,448 / 288,000 = 1 full chunk (288,000 samples) + 1 tail chunk of 72,448 samples (~1.509 seconds).
    // Total chunks: exactly 2!
    const int numBlocks = 704;
    const int64_t totalCapturedSamples = static_cast<int64_t>(numBlocks) * 512;

    std::vector<float> inL(512), inR(512), outL(512), outR(512);
    double phase = 0.0;
    for (int b = 0; b < numBlocks; ++b) {
        const int64_t currentProjectTime = nonZeroStart + static_cast<int64_t>(b) * 512;
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0;
            const double f0 = 261.63 + 3.0 * std::sin(2.0 * 3.14159265 * 4.0 * t);
            phase += f0 / 48000.0;
            float s = 0.0f;
            for (int h = 1; h <= 25; ++h) {
                s += static_cast<float>((0.15 / h) * std::sin(2.0 * 3.14159265 * h * phase));
            }
            inL[i] = s;
            inR[i] = s;
        }
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, currentProjectTime, true);
    }

    // 2. Validate capture range bounds
    int64_t capStart = -1, capEnd = -1;
    plugin->getDspInjector().getCaptureRange(capStart, capEnd);
    if (capStart != nonZeroStart || capEnd != (nonZeroStart + totalCapturedSamples)) {
        return false;
    }

    // 3. Trigger Generate
    plugin->getDspInjector().triggerGenerate();
    if (plugin->getDspInjector().getTimelineState() != rvc_vst::TimelineState::kGenerating) {
        return false;
    }

    // 4. Wait for synthesis to complete
    const bool idle = plugin->getDspInjector().waitForSynthesisIdle(60000);

    const auto tel = plugin->getDspInjector().getVisualTelemetry();
    *outCoverage = tel.cacheCoverageRatio;
    *outTotalChunks = tel.totalActiveChunks;
    *outCachedChunks = tel.cachedChunksCount;

    if (!idle || tel.totalActiveChunks != 2 || tel.cachedChunksCount != 2 || tel.cacheCoverageRatio < 0.999f) {
        return false;
    }

    // 5. Test Active playback across the 6.0s chunk boundary (relPos = 288000, blocks 558..568)
    //    and across the tail chunk blocks [numBlocks - 8 .. numBlocks - 1]
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kActive);
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 1.0);
    plugin->setParamNormalized(rvc_vst::kParamInjectDepth, 1.0);
    plugin->setParamNormalized(rvc_vst::kParamBypass, 0.0);

    double preBoundaryRms = 0.0;
    float prevBoundarySample = 0.0f;
    bool hasPrevBoundarySample = false;
    for (int b = 556; b <= 568; ++b) {
        const int64_t currentProjectTime = nonZeroStart + static_cast<int64_t>(b) * 512;
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, currentProjectTime, true);
        double blkEnergy = 0.0;
        for (int i = 0; i < 512; ++i) {
            const float v = outL[i];
            if (std::isnan(v) || std::isinf(v)) {
                return false;
            }
            if (b >= 558) {
                if (hasPrevBoundarySample && std::abs(v - prevBoundarySample) > 0.30f) {
                    return false; // Discontinuous click at chunk boundary!
                }
                prevBoundarySample = v;
                hasPrevBoundarySample = true;
            }
            blkEnergy += v * v;
        }
        const double blkRms = std::sqrt(blkEnergy / 512.0);
        if (b == 559 || b == 560) {
            preBoundaryRms = std::max(preBoundaryRms, blkRms);
        } else if (b >= 562 && b <= 566 && preBoundaryRms > 0.005) {
            // Verify zero fade-from-silence dropout across the 6.0s chunk boundary (relPos = 288000)
            if (blkRms < preBoundaryRms * 0.45) {
                return false;
            }
        }
    }

    // Specifically test the tail chunk blocks [numBlocks - 8 .. numBlocks - 1]
    double tailEnergy = 0.0;
    for (int b = numBlocks - 8; b < numBlocks; ++b) {
        const int64_t currentProjectTime = nonZeroStart + static_cast<int64_t>(b) * 512;
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, currentProjectTime, true);
        for (int i = 0; i < 512; ++i) {
            tailEnergy += outL[i] * outL[i];
            if (std::isnan(outL[i]) || std::isinf(outL[i])) {
                return false;
            }
        }
    }
    const double tailRms = std::sqrt(tailEnergy / (8 * 512));
    if (tailRms < 0.005) {
        return false; // Tail chunk was omitted or silent!
    }

    // 6. Verify Transport Pause (`isTransportPlaying = false` and/or frozen `projectTimeSamples`)
    //    never loops the cached block slice (zero sustained wavetable buzz after pause)
    std::vector<float> silentIn(512, 0.0f);
    const int64_t pausedProjectTime = nonZeroStart + static_cast<int64_t>(numBlocks - 4) * 512;
    for (int pauseBlk = 0; pauseBlk < 4; ++pauseBlk) {
        plugin->getDspInjector().processBlock(
            silentIn.data(), silentIn.data(), outL.data(), outR.data(),
            512, 2, pausedProjectTime, false);
    }
    double pausedEnergy = 0.0;
    for (int i = 0; i < 512; ++i) {
        pausedEnergy += outL[i] * outL[i] + outR[i] * outR[i];
    }
    if (pausedEnergy > 1e-10) {
        return false; // Sustained sound leaked while DAW transport is paused!
    }

    plugin->terminate();
    return true;
}

/**
 * Regression Assertion 4:
 * Verify that all 6 bottom real-time DSP knobs (WSOLA Pitch Shift, 3-Band SVF Throat Formant,
 * Presence Air, Band-Split Protect Shield, 25ms True-RMS Envelope, Dry/Wet Mix) produce
 * significant, smooth, and 100% NaN/Inf-free acoustic responses.
 */
bool __stdcall RvcVst3_VerifySixRealtimeDspKnobs(char* outDiagnostics, size_t maxDiagLen) {
    auto plugin = std::make_unique<rvc_vst::RvcFeatureInjectorVst3>();
    plugin->initialize(nullptr);
    plugin->forceModelInit();
    if (!queueExplicitTestModels(plugin->getModelEngine())) return false;

    rvc_vst::ProcessSetup setup{};
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    plugin->setupProcessing(setup);
    plugin->setActive(1);

    // Warm up / capture 2 seconds so neural wet cache is active
    std::vector<float> inL(512), inR(512), outL(512), outR(512);
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kCapture);
    for (int b = 0; b < 188; ++b) { // ~2.0s
        for (int i = 0; i < 512; ++i) {
            const double t = (b * 512 + i) / 48000.0;
            const float s = static_cast<float>(0.28 * std::sin(2.0 * 3.14159265 * 330.0 * t) +
                                               0.12 * std::sin(2.0 * 3.14159265 * 660.0 * t) +
                                               0.15 * std::sin(2.0 * 3.14159265 * 7800.0 * t) +
                                               0.10 * std::sin(2.0 * 3.14159265 * 9600.0 * t));
            inL[i] = s;
            inR[i] = s;
        }
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, b * 512, true);
    }
    plugin->getDspInjector().triggerGenerate();
    if (!plugin->getDspInjector().waitForSynthesisIdle(120000) ||
        plugin->getDspInjector().getVisualTelemetry().cacheCoverageRatio < 0.999f) {
        if (outDiagnostics && maxDiagLen > 0) std::snprintf(outDiagnostics, maxDiagLen,
            "FAIL: Neural cache not ready: %s", plugin->getModelEngine().getLoadingStatusText().c_str());
        return false;
    }
    plugin->getDspInjector().setTimelineState(rvc_vst::TimelineState::kActive);

    // Baseline neutral parameter settings
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 1.0);
    plugin->setParamNormalized(rvc_vst::kParamBypass, 0.0);
    plugin->setParamNormalized(rvc_vst::kParamAbMorph, 0.50);
    plugin->setParamNormalized(rvc_vst::kParamInjectDepth, 1.00);
    plugin->setParamNormalized(rvc_vst::kParamPitchShift, 0.50);      // 0 ST
    plugin->setParamNormalized(rvc_vst::kParamThroatFormant, 0.50);   // 0 ST
    plugin->setParamNormalized(rvc_vst::kParamPresenceAir, 0.50);     // 0 dB
    plugin->setParamNormalized(rvc_vst::kParamConsonantProtect, 0.00);// 0.00 (Off)
    plugin->setParamNormalized(rvc_vst::kParamRmsMixRate, 0.25);      // 0.25
    plugin->setParamNormalized(rvc_vst::kParamOutputGain, 0.50);      // 0.0 dB

    auto reportFail = [&](const char* stepName, double v1 = 0, double v2 = 0) {
        if (outDiagnostics && maxDiagLen > 0) {
            std::snprintf(outDiagnostics, maxDiagLen, "FAIL: %s (v1=%.4f, v2=%.4f)", stepName, v1, v2);
        }
    };

    int maxStepBlock = -1;
    int maxStepIdx = -1;

    auto feedAudioAndMeasure = [&](int numBlocks, std::vector<float>& outBuffer, float& outMaxStep) -> double {
        outBuffer.assign(numBlocks * 512, 0.0f);
        float prevSample = 0.0f;
        bool hasPrev = false;
        outMaxStep = 0.0f;
        maxStepBlock = -1;
        maxStepIdx = -1;
        double energy = 0.0;
        for (int b = 0; b < numBlocks; ++b) {
            const int64_t tPos = 4096 + static_cast<int64_t>(b) * 512;
            for (int i = 0; i < 512; ++i) {
                const double t = (4096 + b * 512 + i) / 48000.0;
                const float s = static_cast<float>(0.35 * std::sin(2.0 * 3.14159265 * 330.0 * t) +
                                                   0.15 * std::sin(2.0 * 3.14159265 * 660.0 * t));
                inL[i] = s;
                inR[i] = s;
            }
            plugin->getDspInjector().processBlock(
                inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, tPos, true);
            for (int i = 0; i < 512; ++i) {
                const float val = outL[i];
                if (std::isnan(val) || std::isinf(val)) {
                    reportFail("NaN/Inf detected");
                    return -1.0;
                }
                outBuffer[b * 512 + i] = val;
                if (!hasPrev) {
                    prevSample = val;
                    hasPrev = true;
                } else {
                    const float step = std::abs(val - prevSample);
                    if (step > outMaxStep) {
                        outMaxStep = step;
                        maxStepBlock = b;
                        maxStepIdx = i;
                    }
                    prevSample = val;
                }
                if (b >= numBlocks / 2) {
                    energy += val * val;
                }
            }
        }
        return std::sqrt(energy / ((numBlocks - numBlocks / 2) * 512));
    };

    std::vector<float> bufBase, bufKnobA, bufKnobB;
    float maxStep = 0.0f;

    // --- 1. WSOLA Pitch Shift (-12 ST vs 0 ST vs +12 ST) ---
    plugin->setParamNormalized(rvc_vst::kParamPitchShift, 0.50); // 0 ST
    feedAudioAndMeasure(12, bufBase, maxStep);

    plugin->setParamNormalized(rvc_vst::kParamPitchShift, 1.00); // +12 ST
    feedAudioAndMeasure(12, bufKnobA, maxStep);
    if (maxStep > 0.30f) { reportFail("Pitch maxStep", maxStep, maxStepBlock); return false; }

    plugin->setParamNormalized(rvc_vst::kParamPitchShift, 0.00); // -12 ST
    feedAudioAndMeasure(12, bufKnobB, maxStep);
    if (maxStep > 0.30f) { reportFail("Pitch neg maxStep", maxStep, maxStepBlock); return false; }

    double pitchDiff = 0.0;
    for (size_t i = 6 * 512; i < bufBase.size(); ++i) {
        pitchDiff += std::abs(bufKnobA[i] - bufBase[i]);
    }
    pitchDiff /= (6 * 512);
    if (pitchDiff < 0.015) { reportFail("Pitch diff", pitchDiff); return false; }

    // Reset pitch
    plugin->setParamNormalized(rvc_vst::kParamPitchShift, 0.50);
    feedAudioAndMeasure(6, bufBase, maxStep);

    // --- 2. 3-Band SVF Throat Formant (-6 ST vs +6 ST) ---
    plugin->setParamNormalized(rvc_vst::kParamThroatFormant, 0.00); // -6 ST
    feedAudioAndMeasure(10, bufKnobA, maxStep);
    if (maxStep > 0.20f) { reportFail("Throat neg maxStep", maxStep); return false; }

    plugin->setParamNormalized(rvc_vst::kParamThroatFormant, 1.00); // +6 ST
    feedAudioAndMeasure(10, bufKnobB, maxStep);
    if (maxStep > 0.20f) { reportFail("Throat pos maxStep", maxStep); return false; }

    double throatDiff = 0.0;
    for (size_t i = 5 * 512; i < bufKnobA.size(); ++i) {
        throatDiff += std::abs(bufKnobB[i] - bufKnobA[i]);
    }
    throatDiff /= (5 * 512);
    if (throatDiff < 0.012) { reportFail("Throat diff", throatDiff); return false; }

    // Reset throat
    plugin->setParamNormalized(rvc_vst::kParamThroatFormant, 0.50);
    feedAudioAndMeasure(6, bufBase, maxStep);

    // --- 3. Presence Air Boost (-6 dB vs +6 dB) ---
    auto feedAirAudioAndMeasure = [&](int numBlocks, std::vector<float>& outBuffer, float& outMaxStep) -> double {
        outBuffer.assign(numBlocks * 512, 0.0f);
        float prevSample = 0.0f;
        bool hasPrev = false;
        outMaxStep = 0.0f;
        maxStepBlock = maxStepIdx = -1;
        double hfEnergy = 0.0;
        float lp = 0.0f;
        const float lpCoeff = 0.50f;
        for (int b = 0; b < numBlocks; ++b) {
            const int64_t tPos = 4096 + static_cast<int64_t>(b) * 512;
            for (int i = 0; i < 512; ++i) {
                const double t = (4096 + b * 512 + i) / 48000.0;
                const float s = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265 * 330.0 * t) +
                                                   0.15 * std::sin(2.0 * 3.14159265 * 7800.0 * t) +
                                                   0.10 * std::sin(2.0 * 3.14159265 * 9600.0 * t));
                inL[i] = s;
                inR[i] = s;
            }
            plugin->getDspInjector().processBlock(
                inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, tPos, true);
            for (int i = 0; i < 512; ++i) {
                const float val = outL[i];
                if (std::isnan(val) || std::isinf(val)) {
                    reportFail("NaN/Inf detected in Air test");
                    return -1.0;
                }
                outBuffer[b * 512 + i] = val;
                if (!hasPrev) {
                    prevSample = val;
                    hasPrev = true;
                } else {
                    const float step = std::abs(val - prevSample);
                    if (step > outMaxStep) {
                        outMaxStep = step;
                        maxStepBlock = b;
                        maxStepIdx = i;
                    }
                    prevSample = val;
                }
                lp += lpCoeff * (val - lp);
                const float hf = val - lp;
                if (b >= numBlocks / 2) {
                    hfEnergy += hf * hf;
                }
            }
        }
        return std::sqrt(hfEnergy / ((numBlocks - numBlocks / 2) * 512));
    };

    plugin->setParamNormalized(rvc_vst::kParamConsonantProtect, 0.00);
    plugin->setParamNormalized(rvc_vst::kParamRmsMixRate, 0.00);

    plugin->setParamNormalized(rvc_vst::kParamPresenceAir, 0.00); // -6 dB
    const double rmsAirLow = feedAirAudioAndMeasure(10, bufKnobA, maxStep);
    if (maxStep > 0.20f) { reportFail("Air low maxStep", maxStep, maxStepBlock * 512 + maxStepIdx); return false; }

    plugin->setParamNormalized(rvc_vst::kParamPresenceAir, 1.00); // +6 dB
    const double rmsAirHigh = feedAirAudioAndMeasure(10, bufKnobB, maxStep);
    if (maxStep > 0.20f) { reportFail("Air high maxStep", maxStep); return false; }

    double airDiff = 0.0;
    for (size_t i = 5 * 512; i < bufKnobA.size(); ++i) {
        airDiff += std::abs(bufKnobB[i] - bufKnobA[i]);
    }
    airDiff /= (5 * 512);

    if (rmsAirHigh <= rmsAirLow || airDiff < 0.005) {
        reportFail("Air diff", airDiff, rmsAirHigh - rmsAirLow);
        return false;
    }

    // Reset air and verify 0% stereo dry side-channel bleed at 100% Wet
    plugin->setParamNormalized(rvc_vst::kParamPresenceAir, 0.50);
    plugin->setParamNormalized(rvc_vst::kParamInjectDepth, 1.00);
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 1.00);
    for (int b = 0; b < 6; ++b) {
        const int64_t tPos = 4096 + static_cast<int64_t>(b) * 512;
        for (int i = 0; i < 512; ++i) {
            inL[i] = 0.45f;
            inR[i] = -0.45f; // Pure stereo side signal (midIn == 0, sideIn == 0.45)
        }
        plugin->getDspInjector().processBlock(
            inL.data(), inR.data(), outL.data(), outR.data(), 512, 2, tPos, true);
        if (b >= 3) {
            for (int i = 0; i < 512; ++i) {
                if (std::abs(outL[i] - outR[i]) > 1e-5f) {
                    reportFail("Stereo dry side bleed detected at 100% Mix", outL[i], outR[i]);
                    return false;
                }
            }
        }
    }

    // --- 4. 60ms Macro-RMS Envelope (0% vs 100%) ---
    plugin->setParamNormalized(rvc_vst::kParamRmsMixRate, 0.00); // 0%
    feedAudioAndMeasure(10, bufKnobA, maxStep);
    if (maxStep > 0.20f) { reportFail("RMS 0% maxStep", maxStep); return false; }

    plugin->setParamNormalized(rvc_vst::kParamRmsMixRate, 1.00); // 100%
    feedAudioAndMeasure(10, bufKnobB, maxStep);
    if (maxStep > 0.20f) { reportFail("RMS 100% maxStep", maxStep); return false; }

    double envDiff = 0.0;
    for (size_t i = 5 * 512; i < bufKnobA.size(); ++i) {
        envDiff += std::abs(bufKnobB[i] - bufKnobA[i]);
    }
    envDiff /= (5 * 512);
    if (envDiff < 0.004) { reportFail("Env diff", envDiff); return false; }

    // Reset RMS
    plugin->setParamNormalized(rvc_vst::kParamRmsMixRate, 0.25);

    // --- 5. Dry/Wet Mix (0% vs 100%) ---
    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 0.00); // 100% Dry
    feedAudioAndMeasure(8, bufKnobA, maxStep);
    if (maxStep > 0.20f) { reportFail("Mix 0% maxStep", maxStep); return false; }

    plugin->setParamNormalized(rvc_vst::kParamDryWetMix, 1.00); // 100% Wet
    feedAudioAndMeasure(8, bufKnobB, maxStep);
    if (maxStep > 0.20f) { reportFail("Mix 100% maxStep", maxStep); return false; }

    double mixDiff = 0.0;
    for (size_t i = 4 * 512; i < bufKnobA.size(); ++i) {
        mixDiff += std::abs(bufKnobB[i] - bufKnobA[i]);
    }
    mixDiff /= (4 * 512);
    if (mixDiff < 0.01) { reportFail("Mix diff", mixDiff); return false; }

    // --- 6. Output Gain (-12 dB vs +6 dB) ---
    plugin->setParamNormalized(rvc_vst::kParamOutputGain, 0.00); // -12 dB
    const double rmsGainLow = feedAudioAndMeasure(8, bufKnobA, maxStep);
    if (maxStep > 0.20f) { reportFail("OutputGain -12dB maxStep", maxStep); return false; }

    plugin->setParamNormalized(rvc_vst::kParamOutputGain, 0.75); // +6 dB
    const double rmsGainHigh = feedAudioAndMeasure(8, bufKnobB, maxStep);
    if (maxStep > 0.35f) { reportFail("OutputGain +6dB maxStep", maxStep); return false; }
    if (rmsGainHigh <= rmsGainLow * 3.5) {
        reportFail("OutputGain ratio", rmsGainLow, rmsGainHigh);
        return false;
    }
    plugin->setParamNormalized(rvc_vst::kParamOutputGain, 0.50); // Reset 0 dB

    if (outDiagnostics && maxDiagLen > 0) {
        std::snprintf(
            outDiagnostics, maxDiagLen,
            "PitchDiff=%.4f, ThroatDiff=%.4f, AirDiff=%.4f (Low=%.4f/High=%.4f), EnvDiff=%.4f, MixDiff=%.4f, GainRatio=%.2fx",
            pitchDiff, throatDiff, airDiff, rmsAirLow, rmsAirHigh, envDiff, mixDiff,
            (rmsGainLow > 1e-6) ? (rmsGainHigh / rmsGainLow) : 0.0);
    }

    plugin->terminate();
    return true;
}

} // extern "C"
