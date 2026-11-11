#pragma once

#include "../dsp/RvcFeatureInjector.h"
#include "../inference/RvcModelEngine.h"

#include <windows.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>

namespace rvc_vst {

enum ParameterId : uint32_t {
    kParamAbMorph = 0,
    kParamInjectDepth = 1,
    kParamPitchShift = 2,
    kParamThroatFormant = 3,
    kParamPresenceAir = 4,
    kParamConsonantProtect = 5,
    kParamRmsMixRate = 6,
    kParamDryWetMix = 7,
    kParamSlotAIndexRate = 8,
    kParamSlotBIndexRate = 9,
    kParamOutputGain = 10,
    kParamBypass = 11,
    kNumParameters = 12
};

/**
 * PluginEditorView
 *
 * Hardware-accelerated Direct2D + DirectWrite VST3 Editor View.
 * Implements a Pure Off-White & Matte Carbon minimalist industrial laboratory
 * interface inspired by Teenage Engineering / Braun precision instruments.
 */
class PluginEditorView {
public:
    static constexpr int kDefaultWidth = 1040;
    static constexpr int kDefaultHeight = 580;

    using ParamChangeCallback = std::function<void(uint32_t paramId, double normalizedValue)>;
    using ParamGestureCallback = std::function<void(uint32_t paramId, bool isBegin)>;

    PluginEditorView(
        RvcModelEngine& modelEngine,
        RvcFeatureInjector& dspInjector,
        ParamChangeCallback onParamChanged,
        ParamGestureCallback onParamGesture = nullptr);
    ~PluginEditorView();

    PluginEditorView(const PluginEditorView&) = delete;
    PluginEditorView& operator=(const PluginEditorView&) = delete;

    bool attachToParentWindow(HWND parentHwnd);
    void detachWindow();
    void setWindowSize(int width, int height);
    void getWindowSize(int& outWidth, int& outHeight) const noexcept {
        outWidth = width_;
        outHeight = height_;
    }
    bool onHostMouseWheel(float wheelDistance);
    bool onHostEscape();
    bool isAboutPanelVisible() const noexcept { return aboutPanelVisible_; }

    /**
     * Renders the complete Direct2D UI into a standalone PNG file via WIC.
     * Used for automated visual verification and design inspection.
     */
    bool renderSnapshotToPng(const std::wstring& outputPath);

    static double plainToNormalized(uint32_t paramId, float plainVal) noexcept;
    static float normalizedToPlain(uint32_t paramId, double normVal) noexcept;

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool ensureD2DResources();
    void discardDeviceResources();
    void renderContent(ID2D1RenderTarget* rt, float width, float height);

    void drawHeaderBar(ID2D1RenderTarget* rt, const D2D1_RECT_F& rect, const VisualTelemetrySnapshot& tel, const RvcVoiceParameters& params);
    void drawAboutPanel(ID2D1RenderTarget* rt, float width, float height,
                        const RvcModelSlotInfo& slotA, const RvcModelSlotInfo& slotB);
    void setAboutPanelVisible(bool visible);
    void drawSlotPanel(ID2D1RenderTarget* rt, const D2D1_RECT_F& rect, int slotIndex, const RvcModelSlotInfo& info, float slotIndexRateVal);
    void drawCenterScopeAndMorph(ID2D1RenderTarget* rt, const D2D1_RECT_F& rect, const VisualTelemetrySnapshot& tel, const RvcVoiceParameters& params);
    void drawBottomParameterDeck(ID2D1RenderTarget* rt, const D2D1_RECT_F& rect, const RvcVoiceParameters& params);
    void drawPrecisionRotaryDial(
        ID2D1RenderTarget* rt,
        const D2D1_RECT_F& cellRect,
        int dialIndex,
        const wchar_t* codeTag,
        const wchar_t* title,
        const wchar_t* formattedValue,
        float normalizedVal,
        bool bipolar,
        bool isHovered,
        bool isActive);

    void handleMouseDown(float x, float y, bool doubleClick);
    void handleMouseMove(float x, float y);
    void handleMouseUp();
    bool handleMouseWheel(float x, float y, int wheelDelta);
    void openModelFileDialog(int slotIndex);
    void updateThreeWayBalanceFromMouse(float x, float y);
    void applyNormalizedParameter(uint32_t paramId, double normVal);
    void notifyGesture(uint32_t paramId, bool isBegin);

    RvcModelEngine& modelEngine_;
    RvcFeatureInjector& dspInjector_;
    ParamChangeCallback onParamChanged_;
    ParamGestureCallback onParamGesture_;

    HWND hwnd_ = nullptr;
    UINT_PTR timerId_ = 0;
    int width_ = kDefaultWidth;
    int height_ = kDefaultHeight;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1HwndRenderTarget* hwndRt_ = nullptr;

    IDWriteTextFormat* fmtBrandTitle_ = nullptr;
    IDWriteTextFormat* fmtSubHeader_ = nullptr;
    IDWriteTextFormat* fmtSectionCode_ = nullptr;
    IDWriteTextFormat* fmtModelName_ = nullptr;
    IDWriteTextFormat* fmtMonoValue_ = nullptr;
    IDWriteTextFormat* fmtMonoSmall_ = nullptr;
    IDWriteTextFormat* fmtButtonLabel_ = nullptr;
    IDWriteTextFormat* fmtAboutTitle_ = nullptr;
    IDWriteTextFormat* fmtAboutBody_ = nullptr;

    // Interactive hit-test regions updated during layout
    D2D1_RECT_F rectCaptureBtn_{};
    D2D1_RECT_F rectLogo_{};
    D2D1_RECT_F rectAboutPanel_{};
    D2D1_RECT_F rectAboutClose_{};
    D2D1_RECT_F rectAppendCaptureBtn_{};
    D2D1_RECT_F rectGenerateBtn_{};
    D2D1_RECT_F rectActiveBtn_{};
    D2D1_RECT_F rectClearCaptureBtn_{};
    D2D1_RECT_F rectSlotLoadBtn_[2]{};
    D2D1_RECT_F rectSlotClearBtn_[2]{};
    D2D1_RECT_F rectSlotOctaveBtn_[2][3]{};
    D2D1_RECT_F rectSlotSemiDownBtn_[2]{};
    D2D1_RECT_F rectSlotSemiUpBtn_[2]{};
    D2D1_RECT_F rectSlotIndexBar_[2]{};
    D2D1_RECT_F rectTrianglePad_{};
    D2D1_POINT_2F triVertexDry_{};
    D2D1_POINT_2F triVertexA_{};
    D2D1_POINT_2F triVertexB_{};
    D2D1_RECT_F rectBottomDials_[6]{};

    int activeDragParamId_ = -1;
    bool draggingTrianglePad_ = false;
    int hoveredParamId_ = -1;
    float dragStartMouseX_ = 0.0f;
    float dragStartMouseY_ = 0.0f;
    double dragStartNormVal_ = 0.0;
    bool aboutPanelVisible_ = false;
    bool logoHovered_ = false;
    bool aboutCloseHovered_ = false;
    std::wstring aboutRuntimeRoot_;
    std::wstring aboutAppendComparison_;
    std::wstring aboutAppendCacheStatus_;
    HWND aboutPreviousFocus_ = nullptr;
};

} // namespace rvc_vst
