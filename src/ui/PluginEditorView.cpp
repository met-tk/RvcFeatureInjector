#include "PluginEditorView.h"
#include "../PluginVersion.h"

#include <commdlg.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace rvc_vst {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Pure Off-White & Matte Carbon Industrial Laboratory Color Palette (Teenage Engineering / Braun)
const D2D1_COLOR_F kColChassisBg      = D2D1::ColorF(0xF4F2EC, 1.0f); // Warm Alabaster matte chassis
const D2D1_COLOR_F kColPanelSurface   = D2D1::ColorF(0xFAF9F5, 1.0f); // Elevated clean paper-white module card
const D2D1_COLOR_F kColRecessedWell   = D2D1::ColorF(0xEAE7DF, 1.0f); // Recessed precision scope & track well
const D2D1_COLOR_F kColSubtleWell     = D2D1::ColorF(0xEFECE4, 1.0f); // Secondary technical cell background
const D2D1_COLOR_F kColFramePrimary   = D2D1::ColorF(0x181816, 1.0f); // Deep Matte Carbon primary structural line
const D2D1_COLOR_F kColGridHairline   = D2D1::ColorF(0xCFCBC0, 1.0f); // 1px architectural subdivision hairline
const D2D1_COLOR_F kColGridSubtle     = D2D1::ColorF(0xDFDCD2, 1.0f); // Fine scope rule line
const D2D1_COLOR_F kColInkPrimary     = D2D1::ColorF(0x141413, 1.0f); // Primary carbon black typography
const D2D1_COLOR_F kColInkSecondary   = D2D1::ColorF(0x625F57, 1.0f); // Secondary graphite technical label
const D2D1_COLOR_F kColInkMuted       = D2D1::ColorF(0x969288, 1.0f); // Muted scale coordinate text
const D2D1_COLOR_F kColAccentSlotA    = D2D1::ColorF(0xD84A1B, 1.0f); // Braun International Signal Orange (Slot A)
const D2D1_COLOR_F kColAccentSlotB    = D2D1::ColorF(0x1B5688, 1.0f); // Precision Cobalt Blue (Slot B)
const D2D1_COLOR_F kColStatusNominal  = D2D1::ColorF(0x1D7848, 1.0f); // Laboratory Emerald nominal indicator

inline bool ptInRect(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

inline D2D1_RECT_F snapRect(float l, float t, float r, float b) noexcept {
    return D2D1::RectF(
        std::floor(l) + 0.5f,
        std::floor(t) + 0.5f,
        std::floor(r) - 0.5f,
        std::floor(b) - 0.5f);
}

inline D2D1_POINT_2F lerpPt(const D2D1_POINT_2F& a, const D2D1_POINT_2F& b, float t) noexcept {
    return D2D1::Point2F(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring res(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), res.data(), len);
    return res;
}

void drawCrosshairTick(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float x, float y, float arm = 3.0f) {
    const float sx = std::floor(x) + 0.5f;
    const float sy = std::floor(y) + 0.5f;
    rt->DrawLine(D2D1::Point2F(sx - arm, sy), D2D1::Point2F(sx + arm, sy), brush, 1.0f);
    rt->DrawLine(D2D1::Point2F(sx, sy - arm), D2D1::Point2F(sx, sy + arm), brush, 1.0f);
}

const wchar_t* freqToNoteName(float hz, char* outBuf, size_t bufSize) {
    static thread_local wchar_t wbuf[32];
    if (hz < 50.0f || hz > 2000.0f) {
        swprintf_s(wbuf, L"--- Hz");
        return wbuf;
    }
    static const char* kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    const int midiNote = static_cast<int>(std::round(midi));
    const int noteIdx = ((midiNote % 12) + 12) % 12;
    const int octave = (midiNote / 12) - 1;
    snprintf(outBuf, bufSize, "%s%d (%.0fHz)", kNotes[noteIdx], octave, hz);
    MultiByteToWideChar(CP_UTF8, 0, outBuf, -1, wbuf, 32);
    return wbuf;
}

} // namespace

PluginEditorView::PluginEditorView(
    RvcModelEngine& modelEngine,
    RvcFeatureInjector& dspInjector,
    ParamChangeCallback onParamChanged,
    ParamGestureCallback onParamGesture)
    : modelEngine_(modelEngine),
      dspInjector_(dspInjector),
      onParamChanged_(std::move(onParamChanged)),
      onParamGesture_(std::move(onParamGesture)) {
    ensureD2DResources();
}

PluginEditorView::~PluginEditorView() {
    detachWindow();
    discardDeviceResources();
    if (fmtBrandTitle_) fmtBrandTitle_->Release();
    if (fmtSubHeader_) fmtSubHeader_->Release();
    if (fmtSectionCode_) fmtSectionCode_->Release();
    if (fmtModelName_) fmtModelName_->Release();
    if (fmtMonoValue_) fmtMonoValue_->Release();
    if (fmtMonoSmall_) fmtMonoSmall_->Release();
    if (fmtButtonLabel_) fmtButtonLabel_->Release();
    if (fmtAboutTitle_) fmtAboutTitle_->Release();
    if (fmtAboutBody_) fmtAboutBody_->Release();
    if (dwriteFactory_) dwriteFactory_->Release();
    if (d2dFactory_) d2dFactory_->Release();
}

double PluginEditorView::plainToNormalized(uint32_t paramId, float plainVal) noexcept {
    switch (paramId) {
        case kParamAbMorph:
        case kParamInjectDepth:
        case kParamRmsMixRate:
        case kParamDryWetMix:
        case kParamSlotAIndexRate:
        case kParamSlotBIndexRate:
            return std::clamp(static_cast<double>(plainVal), 0.0, 1.0);
        case kParamPitchShift:
        case kParamOutputGain:
            return std::clamp((static_cast<double>(plainVal) + 12.0) / 24.0, 0.0, 1.0);
        case kParamThroatFormant:
            return std::clamp((static_cast<double>(plainVal) + 6.0) / 12.0, 0.0, 1.0);
        case kParamPresenceAir:
            return std::clamp((static_cast<double>(plainVal) + 6.0) / 12.0, 0.0, 1.0);
        case kParamConsonantProtect:
            return std::clamp(static_cast<double>(plainVal) / 0.50, 0.0, 1.0);
        case kParamBypass:
            return (plainVal >= 0.5f) ? 1.0 : 0.0;
        default:
            return 0.0;
    }
}

float PluginEditorView::normalizedToPlain(uint32_t paramId, double normVal) noexcept {
    const float n = static_cast<float>(std::clamp(normVal, 0.0, 1.0));
    switch (paramId) {
        case kParamAbMorph:
        case kParamInjectDepth:
        case kParamRmsMixRate:
        case kParamDryWetMix:
        case kParamSlotAIndexRate:
        case kParamSlotBIndexRate:
            return n;
        case kParamPitchShift:
        case kParamOutputGain:
            return -12.0f + n * 24.0f;
        case kParamThroatFormant:
            return -6.0f + n * 12.0f;
        case kParamPresenceAir:
            return -6.0f + n * 12.0f;
        case kParamConsonantProtect:
            return n * 0.50f;
        case kParamBypass:
            return (n >= 0.5f) ? 1.0f : 0.0f;
        default:
            return n;
    }
}

bool PluginEditorView::ensureD2DResources() {
    if (!d2dFactory_) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
        if (FAILED(hr)) return false;
    }
    if (!dwriteFactory_) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&dwriteFactory_));
        if (FAILED(hr)) return false;
    }

    auto createFmt = [&](IDWriteTextFormat** outFmt, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
        if (*outFmt) return;
        dwriteFactory_->CreateTextFormat(
            family,
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"en-us",
            outFmt);
    };

    createFmt(&fmtBrandTitle_, L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_BOLD);
    createFmt(&fmtSubHeader_, L"Consolas", 9.5f, DWRITE_FONT_WEIGHT_NORMAL);
    createFmt(&fmtSectionCode_, L"Consolas", 10.5f, DWRITE_FONT_WEIGHT_BOLD);
    createFmt(&fmtModelName_, L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    createFmt(&fmtMonoValue_, L"Consolas", 12.0f, DWRITE_FONT_WEIGHT_BOLD);
    createFmt(&fmtMonoSmall_, L"Consolas", 9.5f, DWRITE_FONT_WEIGHT_NORMAL);
    createFmt(&fmtButtonLabel_, L"Consolas", 10.0f, DWRITE_FONT_WEIGHT_BOLD);
    createFmt(&fmtAboutTitle_, L"Segoe UI", 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    createFmt(&fmtAboutBody_, L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    return true;
}

void PluginEditorView::discardDeviceResources() {
    if (hwndRt_) {
        hwndRt_->Release();
        hwndRt_ = nullptr;
    }
}

bool PluginEditorView::attachToParentWindow(HWND parentHwnd) {
    detachWindow();
    if (!ensureD2DResources()) return false;

    static const wchar_t* kWndClassName = L"RvcFeatureInjectorVst3ViewClass";
    static bool classRegistered = false;
    HINSTANCE hInst = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&PluginEditorView::wndProcThunk),
        &hInst);
    if (!hInst) {
        hInst = GetModuleHandleW(nullptr);
    }

    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &PluginEditorView::wndProcThunk;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWndClassName;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    hwnd_ = CreateWindowExW(
        0,
        kWndClassName,
        plugin_info::kProductNameWide,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0,
        0,
        width_,
        height_,
        parentHwnd,
        nullptr,
        hInst,
        this);

    if (!hwnd_) return false;

    DragAcceptFiles(hwnd_, TRUE);
    timerId_ = SetTimer(hwnd_, 1001, 16, nullptr);
    return true;
}

void PluginEditorView::detachWindow() {
    setAboutPanelVisible(false);
    logoHovered_ = false;
    aboutCloseHovered_ = false;
    if (hwnd_) {
        if (timerId_) {
            KillTimer(hwnd_, timerId_);
            timerId_ = 0;
        }
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    discardDeviceResources();
}

void PluginEditorView::setWindowSize(int width, int height) {
    width_ = std::max(640, width);
    height_ = std::max(420, height);
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, width_, height_, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    if (hwndRt_) {
        D2D1_SIZE_U pixSize = D2D1::SizeU(static_cast<UINT32>(width_), static_cast<UINT32>(height_));
        hwndRt_->Resize(pixSize);
    }
}

LRESULT CALLBACK PluginEditorView::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PluginEditorView* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PluginEditorView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PluginEditorView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handleWindowMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PluginEditorView::handleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            if (!hwndRt_ && d2dFactory_) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
                d2dFactory_->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(),
                    D2D1::HwndRenderTargetProperties(hwnd, size),
                    &hwndRt_);
            }
            if (hwndRt_) {
                hwndRt_->BeginDraw();
                renderContent(hwndRt_, static_cast<float>(width_), static_cast<float>(height_));
                HRESULT hr = hwndRt_->EndDraw();
                if (hr == D2DERR_RECREATE_TARGET) {
                    discardDeviceResources();
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            handleMouseDown(
                static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)),
                false);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            handleMouseDown(
                static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)),
                true);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            handleMouseMove(
                static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            logoHovered_ = false;
            aboutCloseHovered_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                const bool hand = aboutPanelVisible_
                    ? ptInRect(rectAboutClose_, static_cast<float>(pt.x), static_cast<float>(pt.y))
                    : ptInRect(rectLogo_, static_cast<float>(pt.x), static_cast<float>(pt.y));
                if (hand) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && onHostEscape()) return 0;
            break;
        case WM_GETDLGCODE:
            if (aboutPanelVisible_) return DLGC_WANTALLKEYS;
            break;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            handleMouseUp();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            handleMouseWheel(
                static_cast<float>(pt.x),
                static_cast<float>(pt.y),
                GET_WHEEL_DELTA_WPARAM(wParam));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            DragFinish(hDrop);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool PluginEditorView::renderSnapshotToPng(const std::wstring& outputPath) {
    if (!ensureD2DResources()) return false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(hr);

    IWICImagingFactory* wicFactory = nullptr;
    hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr) || !wicFactory) {
        if (needUninit) CoUninitialize();
        return false;
    }

    IWICBitmap* wicBitmap = nullptr;
    hr = wicFactory->CreateBitmap(
        static_cast<UINT>(width_),
        static_cast<UINT>(height_),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad,
        &wicBitmap);

    ID2D1RenderTarget* wicRt = nullptr;
    if (SUCCEEDED(hr) && wicBitmap) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);
        hr = d2dFactory_->CreateWicBitmapRenderTarget(wicBitmap, rtProps, &wicRt);
    }

    bool saved = false;
    if (SUCCEEDED(hr) && wicRt) {
        wicRt->BeginDraw();
        renderContent(wicRt, static_cast<float>(width_), static_cast<float>(height_));
        hr = wicRt->EndDraw();

        if (SUCCEEDED(hr)) {
            IWICStream* stream = nullptr;
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;

            if (SUCCEEDED(wicFactory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
                SUCCEEDED(frame->Initialize(nullptr)) &&
                SUCCEEDED(frame->SetSize(static_cast<UINT>(width_), static_cast<UINT>(height_)))) {

                WICPixelFormatGUID formatGuid = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&formatGuid);
                if (SUCCEEDED(frame->WriteSource(wicBitmap, nullptr)) &&
                    SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(encoder->Commit())) {
                    saved = true;
                }
            }
            if (frame) frame->Release();
            if (encoder) encoder->Release();
            if (stream) stream->Release();
        }
        wicRt->Release();
    }

    if (wicBitmap) wicBitmap->Release();
    wicFactory->Release();
    if (needUninit) CoUninitialize();
    return saved;
}

void PluginEditorView::renderContent(ID2D1RenderTarget* rt, float width, float height) {
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt->Clear(kColChassisBg);

    const VisualTelemetrySnapshot tel = dspInjector_.getVisualTelemetry();
    const RvcVoiceParameters params = dspInjector_.getParameters();
    const RvcModelSlotInfo slotAInfo = modelEngine_.getSlotInfo(0);
    const RvcModelSlotInfo slotBInfo = modelEngine_.getSlotInfo(1);

    ID2D1SolidColorBrush* brFrame = nullptr;
    ID2D1SolidColorBrush* brHairline = nullptr;
    rt->CreateSolidColorBrush(kColFramePrimary, &brFrame);
    rt->CreateSolidColorBrush(kColGridHairline, &brHairline);

    const float pad = 14.0f;
    const D2D1_RECT_F outerFrame = snapRect(pad, pad, width - pad, height - pad);

    // Master outer technical border
    if (brFrame) {
        rt->DrawRectangle(outerFrame, brFrame, 1.5f);
    }

    // 1. Header Bar (Top 46px)
    const float headerBottom = outerFrame.top + 46.0f;
    const D2D1_RECT_F headerRect = D2D1::RectF(outerFrame.left, outerFrame.top, outerFrame.right, headerBottom);
    drawHeaderBar(rt, headerRect, tel, params);

    if (brFrame) {
        rt->DrawLine(
            D2D1::Point2F(outerFrame.left, std::floor(headerBottom) + 0.5f),
            D2D1::Point2F(outerFrame.right, std::floor(headerBottom) + 0.5f),
            brFrame,
            1.5f);
    }

    // 2. Upper 3-Column Workspace (Slot A | Timeline Waveform & Three-Way Balance | Slot B)
    const float upperTop = headerBottom;
    const float upperBottom = outerFrame.bottom - 214.0f;
    const float slotWidth = 234.0f;
    const float leftColRight = outerFrame.left + slotWidth;
    const float rightColLeft = outerFrame.right - slotWidth;

    const D2D1_RECT_F slotARect = D2D1::RectF(outerFrame.left, upperTop, leftColRight, upperBottom);
    const D2D1_RECT_F centerRect = D2D1::RectF(leftColRight, upperTop, rightColLeft, upperBottom);
    const D2D1_RECT_F slotBRect = D2D1::RectF(rightColLeft, upperTop, outerFrame.right, upperBottom);

    drawSlotPanel(rt, slotARect, 0, slotAInfo, params.slotAIndexRate);
    drawCenterScopeAndMorph(rt, centerRect, tel, params);
    drawSlotPanel(rt, slotBRect, 1, slotBInfo, params.slotBIndexRate);

    if (brHairline && brFrame) {
        const float lx = std::floor(leftColRight) + 0.5f;
        const float rx = std::floor(rightColLeft) + 0.5f;
        const float uy = std::floor(upperBottom) + 0.5f;
        rt->DrawLine(D2D1::Point2F(lx, upperTop), D2D1::Point2F(lx, upperBottom), brHairline, 1.0f);
        rt->DrawLine(D2D1::Point2F(rx, upperTop), D2D1::Point2F(rx, upperBottom), brHairline, 1.0f);
        rt->DrawLine(D2D1::Point2F(outerFrame.left, uy), D2D1::Point2F(outerFrame.right, uy), brFrame, 1.5f);

        drawCrosshairTick(rt, brFrame, lx, upperTop + 28.0f, 3.5f);
        drawCrosshairTick(rt, brFrame, rx, upperTop + 28.0f, 3.5f);
        drawCrosshairTick(rt, brFrame, lx, uy, 3.5f);
        drawCrosshairTick(rt, brFrame, rx, uy, 3.5f);
    }

    // 3. Bottom 6-Encoder Real-Time Effector Matrix
    const D2D1_RECT_F bottomRect = D2D1::RectF(outerFrame.left, upperBottom, outerFrame.right, outerFrame.bottom);
    drawBottomParameterDeck(rt, bottomRect, params);

    if (brHairline) brHairline->Release();
    if (brFrame) brFrame->Release();
    if (aboutPanelVisible_) drawAboutPanel(rt, width, height, slotAInfo, slotBInfo);
    else if (logoHovered_) {
        ID2D1SolidColorBrush* ink = nullptr;
        ID2D1SolidColorBrush* paper = nullptr;
        rt->CreateSolidColorBrush(kColInkPrimary, &ink);
        rt->CreateSolidColorBrush(kColPanelSurface, &paper);
        const auto hint = D2D1::RectF(rectLogo_.left + 12.0f, rectLogo_.bottom + 5.0f,
                                     rectLogo_.left + 146.0f, rectLogo_.bottom + 29.0f);
        if (ink && paper) {
            rt->FillRectangle(hint, paper);
            rt->DrawRectangle(hint, ink, 1.0f);
            rt->DrawText(L"ABOUT / VERSION", 15, fmtSectionCode_,
                D2D1::RectF(hint.left + 10.0f, hint.top + 6.0f, hint.right - 5.0f, hint.bottom), ink);
        }
        if (paper) paper->Release();
        if (ink) ink->Release();
    }
}

void PluginEditorView::setAboutPanelVisible(bool visible) {
    if (visible == aboutPanelVisible_) return;
    handleMouseUp();
    aboutPanelVisible_ = visible;
    logoHovered_ = false;
    aboutCloseHovered_ = false;
    hoveredParamId_ = -1;
    if (visible) {
        aboutRuntimeRoot_ = modelEngine_.getRuntimeRootPath();
        const auto telemetry = dspInjector_.getVisualTelemetry();
        wchar_t appendSummary[192]{};
        swprintf_s(appendSummary, L"Last append / Compared %llu samples   /   Changed %llu",
            static_cast<unsigned long long>(telemetry.appendComparedSamples),
            static_cast<unsigned long long>(telemetry.appendChangedSamples));
        aboutAppendComparison_ = telemetry.appendComparedSamples ? appendSummary : L"Last append / No comparison recorded";
        aboutAppendCacheStatus_ = telemetry.appendReusedCache
            ? L"Generate / Reused previous cache"
            : L"Generate / Previous cache not reused";
        if (hwnd_) {
            aboutPreviousFocus_ = GetFocus();
            SetFocus(hwnd_);
        }
    } else {
        if (hwnd_ && GetFocus() == hwnd_ && aboutPreviousFocus_ && IsWindow(aboutPreviousFocus_))
            SetFocus(aboutPreviousFocus_);
        aboutPreviousFocus_ = nullptr;
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool PluginEditorView::onHostEscape() {
    if (!aboutPanelVisible_) return false;
    setAboutPanelVisible(false);
    return true;
}

void PluginEditorView::drawAboutPanel(ID2D1RenderTarget* rt, float width, float height,
                                    const RvcModelSlotInfo& slotA, const RvcModelSlotInfo& slotB) {
    const float left = 24.0f;
    const float top = std::min(74.0f, std::max(24.0f, height - 448.0f));
    rectAboutPanel_ = snapRect(left, top, std::min(width - 24.0f, left + 640.0f),
                              std::min(height - 24.0f, top + 424.0f));
    const auto& box = rectAboutPanel_;
    rectAboutClose_ = snapRect(box.right - 88.0f, box.top + 16.0f, box.right - 20.0f, box.top + 44.0f);
    ID2D1SolidColorBrush* ink = nullptr;
    ID2D1SolidColorBrush* secondary = nullptr;
    ID2D1SolidColorBrush* paper = nullptr;
    ID2D1SolidColorBrush* rule = nullptr;
    rt->CreateSolidColorBrush(kColInkPrimary, &ink);
    rt->CreateSolidColorBrush(kColInkSecondary, &secondary);
    rt->CreateSolidColorBrush(kColPanelSurface, &paper);
    rt->CreateSolidColorBrush(kColGridHairline, &rule);
    if (ink && secondary && paper && rule && fmtAboutTitle_ && fmtAboutBody_) {
        rt->FillRectangle(box, paper);
        rt->DrawRectangle(box, ink, 1.25f);
        const float textLeft = box.left + 22.0f;
        const float textRight = box.right - 22.0f;
        auto text = [&](const std::wstring& value, float y, float h, IDWriteTextFormat* format,
                        ID2D1SolidColorBrush* brush, float right) {
            if (!format || value.empty()) return;
            IDWriteTextLayout* layout = nullptr;
            const HRESULT result = dwriteFactory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                format, std::max(1.0f, right - textLeft), h, &layout);
            if (SUCCEEDED(result) && layout) {
                IDWriteInlineObject* ellipsis = nullptr;
                if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, &ellipsis))) {
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    layout->SetTrimming(&trimming, ellipsis);
                    ellipsis->Release();
                }
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
                rt->DrawTextLayout(D2D1::Point2F(textLeft, y), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                layout->Release();
            }
        };
        text(plugin_info::kProductNameWide, box.top + 16.0f, 30.0f, fmtAboutTitle_, ink, rectAboutClose_.left - 12.0f);
        rt->FillRectangle(rectAboutClose_, aboutCloseHovered_ ? ink : paper);
        rt->DrawRectangle(rectAboutClose_, ink, 1.0f);
        fmtAboutBody_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(L"Close", 5, fmtAboutBody_, D2D1::RectF(rectAboutClose_.left, rectAboutClose_.top + 5.0f,
            rectAboutClose_.right, rectAboutClose_.bottom), aboutCloseHovered_ ? paper : ink);
        fmtAboutBody_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const std::wstring version = std::wstring(L"Version ") + plugin_info::kVersionWide + L"   /   Build " + plugin_info::kBuildWide;
        text(version, box.top + 52.0f, 22.0f, fmtAboutBody_, ink, textRight);
        const std::wstring publisher = std::wstring(plugin_info::kVendorWide) + L"   /   " + plugin_info::kFormatWide;
        text(publisher, box.top + 78.0f, 22.0f, fmtAboutBody_, secondary, textRight);
        rt->DrawLine(D2D1::Point2F(textLeft, box.top + 112.0f), D2D1::Point2F(textRight, box.top + 112.0f), rule);
        const wchar_t* runtimeStatus = modelEngine_.isNativeDaemonReady() ? L"Runtime / Inference ready"
            : (modelEngine_.isOrtInitialized() ? L"Runtime / Available" : L"Runtime / Not available");
        text(runtimeStatus, box.top + 128.0f, 22.0f, fmtAboutBody_, ink, textRight);
        text(aboutRuntimeRoot_.empty() ? L"No runtime path configured" : aboutRuntimeRoot_,
             box.top + 154.0f, 40.0f, fmtAboutBody_, secondary, textRight);
        const bool compact = box.bottom - box.top < 410.0f;
        auto slot = [&](const RvcModelSlotInfo& info, const wchar_t* label, float y) {
            const std::wstring name = std::wstring(label) + L" / " + (info.isLoaded ? utf8ToWide(info.modelName) : L"No model loaded");
            text(name, y, 21.0f, fmtAboutBody_, ink, textRight);
            if (compact) return;
            if (info.isLoaded) {
                wchar_t details[160]{};
                swprintf_s(details, L"RVC v%u   /   %u Hz   /   %s", info.rvcVersion, info.sampleRate,
                    info.hasFeatureIndex ? L"Retrieval index loaded" : L"No retrieval index");
                text(details, y + 22.0f, 20.0f, fmtAboutBody_, secondary, textRight);
            } else {
                text(L"Import your own .pth model in this slot", y + 22.0f, 20.0f, fmtAboutBody_, secondary, textRight);
            }
        };
        slot(slotA, L"Slot A", box.top + 204.0f);
        slot(slotB, L"Slot B", box.top + (compact ? 236.0f : 258.0f));
        const float diagnosticsTop = box.top + (compact ? 280.0f : 328.0f);
        rt->DrawLine(D2D1::Point2F(textLeft, diagnosticsTop - 12.0f),
                     D2D1::Point2F(textRight, diagnosticsTop - 12.0f), rule);
        text(aboutAppendComparison_, diagnosticsTop, 22.0f, fmtAboutBody_, ink, textRight);
        text(aboutAppendCacheStatus_, diagnosticsTop + 26.0f, 22.0f, fmtAboutBody_, secondary, textRight);
        text(L"Esc or click outside to close", box.bottom - 32.0f, 20.0f, fmtAboutBody_, secondary, textRight);
    }
    if (rule) rule->Release();
    if (paper) paper->Release();
    if (secondary) secondary->Release();
    if (ink) ink->Release();
}

void PluginEditorView::drawHeaderBar(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    const VisualTelemetrySnapshot& tel,
    const RvcVoiceParameters& params) {

    ID2D1SolidColorBrush* brInk = nullptr;
    ID2D1SolidColorBrush* brSec = nullptr;
    ID2D1SolidColorBrush* brMuted = nullptr;
    ID2D1SolidColorBrush* brCard = nullptr;
    ID2D1SolidColorBrush* brWell = nullptr;
    ID2D1SolidColorBrush* brSubWell = nullptr;
    ID2D1SolidColorBrush* brHair = nullptr;
    ID2D1SolidColorBrush* brStatus = nullptr;
    ID2D1SolidColorBrush* brOrange = nullptr;

    rt->CreateSolidColorBrush(kColInkPrimary, &brInk);
    rt->CreateSolidColorBrush(kColInkSecondary, &brSec);
    rt->CreateSolidColorBrush(kColInkMuted, &brMuted);
    rt->CreateSolidColorBrush(kColPanelSurface, &brCard);
    rt->CreateSolidColorBrush(kColRecessedWell, &brWell);
    rt->CreateSolidColorBrush(kColSubtleWell, &brSubWell);
    rt->CreateSolidColorBrush(kColGridHairline, &brHair);
    rt->CreateSolidColorBrush(kColStatusNominal, &brStatus);
    rt->CreateSolidColorBrush(kColAccentSlotA, &brOrange);

    rt->FillRectangle(rect, brCard);
    rectLogo_ = D2D1::RectF(rect.left + 1.0f, rect.top + 1.0f, rect.left + 248.0f, rect.bottom - 1.0f);
    if (logoHovered_ || aboutPanelVisible_) rt->FillRectangle(rectLogo_, brSubWell);

    // Small geometric square badge
    D2D1_RECT_F badgeRect = D2D1::RectF(rect.left + 12.0f, rect.top + 11.0f, rect.left + 20.0f, rect.top + 19.0f);
    rt->FillRectangle(badgeRect, brOrange);
    D2D1_RECT_F badgeRect2 = D2D1::RectF(rect.left + 12.0f, rect.top + 23.0f, rect.left + 20.0f, rect.top + 35.0f);
    rt->FillRectangle(badgeRect2, brInk);

    // Leave room for separate new/append capture controls at the default size.
    const wchar_t* titleText = L"RVC // FEATURE INJECTOR";
    D2D1_RECT_F titleRect = D2D1::RectF(rect.left + 28.0f, rect.top + 7.0f, rect.left + 244.0f, rect.top + 25.0f);
    rt->DrawText(titleText, static_cast<UINT32>(wcslen(titleText)), fmtBrandTitle_, titleRect, brInk);

    const wchar_t* subText = L"DUAL-MODEL NEURAL VOCAL MORPHING UNIT";
    D2D1_RECT_F subRect = D2D1::RectF(rect.left + 28.0f, rect.top + 26.0f, rect.left + 244.0f, rect.bottom - 4.0f);
    rt->DrawText(subText, static_cast<UINT32>(wcslen(subText)), fmtSubHeader_, subRect, brSec);

    // Right-side Telemetry Cells & Action Switches (Non-overlapping precision layout)
    const float cellTop = rect.top + 9.0f;
    const float cellBot = rect.bottom - 9.0f;

    // 1. Unified CUDA engine and DSP load readout.
    const bool rvcOk = modelEngine_.isOrtInitialized();
    const bool synthBusy = tel.isSynthesizing;
    D2D1_RECT_F ortBox = snapRect(rect.right - 756.0f, cellTop, rect.right - 632.0f, cellBot);
    rt->FillRectangle(ortBox, brSubWell);
    rt->DrawRectangle(ortBox, brHair, 1.0f);
    D2D1_RECT_F ledDot = D2D1::RectF(ortBox.left + 7.0f, ortBox.top + 10.0f, ortBox.left + 13.0f, ortBox.top + 16.0f);
    rt->FillRectangle(ledDot, synthBusy ? brOrange : (rvcOk ? brStatus : brOrange));

    wchar_t ortBuf[64];
    if (synthBusy) {
        swprintf_s(ortBuf, L"CUDA:SYNTH|%.1f%%", tel.cpuLoadPercent);
    } else if (rvcOk) {
        swprintf_s(ortBuf, L"CUDA:READY|%.1f%%", tel.cpuLoadPercent);
    } else {
        swprintf_s(ortBuf, L"RVC:STDBY |%.1f%%", tel.cpuLoadPercent);
    }
    D2D1_RECT_F ortTxtRect = D2D1::RectF(ortBox.left + 16.0f, ortBox.top + 6.0f, ortBox.right - 3.0f, ortBox.bottom);
    rt->DrawText(ortBuf, static_cast<UINT32>(wcslen(ortBuf)), fmtSectionCode_, ortTxtRect, brInk);

    // 2. Real-time input/output peak level meter.
    D2D1_RECT_F meterBox = snapRect(rect.right - 626.0f, cellTop, rect.right - 438.0f, cellBot);
    rt->FillRectangle(meterBox, brSubWell);
    rt->DrawRectangle(meterBox, brHair, 1.0f);

    const float inNorm = std::clamp((tel.inputPeakDb + 48.0f) / 48.0f, 0.0f, 1.0f);
    const float outNorm = std::clamp((tel.outputPeakDb + 48.0f) / 48.0f, 0.0f, 1.0f);
    const bool outClip = (tel.outputPeakDb > -0.5f);

    rt->DrawText(
        L"IN",
        2,
        fmtMonoSmall_,
        D2D1::RectF(meterBox.left + 6.0f, meterBox.top + 2.0f, meterBox.left + 26.0f, meterBox.top + 15.0f),
        brSec);
    rt->DrawText(
        L"OUT",
        3,
        fmtMonoSmall_,
        D2D1::RectF(meterBox.left + 6.0f, meterBox.top + 13.5f, meterBox.left + 28.0f, meterBox.bottom),
        brInk);

    const float barLeft = meterBox.left + 30.0f;
    const float barRight = meterBox.right - 54.0f;
    const float barW = std::max(10.0f, barRight - barLeft);

    D2D1_RECT_F inTrack = snapRect(barLeft, meterBox.top + 5.0f, barRight, meterBox.top + 11.0f);
    D2D1_RECT_F outTrack = snapRect(barLeft, meterBox.top + 16.0f, barRight, meterBox.top + 22.0f);
    rt->FillRectangle(inTrack, brWell);
    rt->FillRectangle(outTrack, brWell);
    rt->DrawRectangle(inTrack, brHair, 1.0f);
    rt->DrawRectangle(outTrack, brHair, 1.0f);

    if (inNorm > 0.01f) {
        rt->FillRectangle(
            D2D1::RectF(inTrack.left + 1.0f, inTrack.top + 1.0f, inTrack.left + 1.0f + (barW - 2.0f) * inNorm, inTrack.bottom - 1.0f),
            brSec);
    }
    if (outNorm > 0.01f) {
        rt->FillRectangle(
            D2D1::RectF(outTrack.left + 1.0f, outTrack.top + 1.0f, outTrack.left + 1.0f + (barW - 2.0f) * outNorm, outTrack.bottom - 1.0f),
            outClip ? brOrange : brStatus);
    }

    wchar_t inDbStr[24], outDbStr[24];
    swprintf_s(inDbStr, (tel.inputPeakDb <= -58.0f) ? L"-INF dB" : L"%+4.1fdB", tel.inputPeakDb);
    swprintf_s(outDbStr, (tel.outputPeakDb <= -58.0f) ? L"-INF dB" : L"%+4.1fdB", tel.outputPeakDb);
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            inDbStr,
            static_cast<UINT32>(wcslen(inDbStr)),
            fmtMonoSmall_,
            D2D1::RectF(barRight + 2.0f, meterBox.top + 2.0f, meterBox.right - 4.0f, meterBox.top + 15.0f),
            brSec);
        rt->DrawText(
            outDbStr,
            static_cast<UINT32>(wcslen(outDbStr)),
            fmtMonoSmall_,
            D2D1::RectF(barRight + 2.0f, meterBox.top + 13.5f, meterBox.right - 4.0f, meterBox.bottom),
            outClip ? brOrange : brInk);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // 3. New and append capture are separate actions; both stop on a second click.
    const auto curState = static_cast<TimelineState>(tel.timelineStateVal);
    const bool isCapturing = (curState == TimelineState::kCapture);
    const bool isGenerating = (curState == TimelineState::kGenerating);
    const bool isActive = (curState == TimelineState::kActive) && !params.bypass;
    const bool hasCaptured = (tel.captureStartSample >= 0 && tel.captureEndSample > tel.captureStartSample);
    const bool isAppending = isCapturing && tel.isAppendCapture;
    const bool isNewCapture = isCapturing && !isAppending;
    const bool canNewCapture = !isGenerating && !isAppending;
    const bool canAppendCapture = hasCaptured && !isGenerating && (!isCapturing || isAppending);

    rectCaptureBtn_ = snapRect(rect.right - 430.0f, cellTop, rect.right - 344.0f, cellBot);
    if (isNewCapture) {
        rt->FillRectangle(rectCaptureBtn_, brOrange);
        rt->DrawRectangle(rectCaptureBtn_, brInk, 1.0f);
    } else {
        rt->FillRectangle(rectCaptureBtn_, canNewCapture ? brWell : brCard);
        rt->DrawRectangle(rectCaptureBtn_, brHair, 1.0f);
    }
    const wchar_t* capStr = isNewCapture ? L"STOP CAP" : (hasCaptured ? L"NEW CAPTURE" : L"CAPTURE");
    D2D1_RECT_F capTxt = D2D1::RectF(rectCaptureBtn_.left, rectCaptureBtn_.top + 6.0f, rectCaptureBtn_.right, rectCaptureBtn_.bottom);
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(capStr, static_cast<UINT32>(wcslen(capStr)), fmtButtonLabel_, capTxt, isNewCapture ? brCard : (canNewCapture ? brInk : brMuted));
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    rectAppendCaptureBtn_ = snapRect(rect.right - 338.0f, cellTop, rect.right - 234.0f, cellBot);
    rt->FillRectangle(rectAppendCaptureBtn_, isAppending ? brOrange : (canAppendCapture ? brWell : brCard));
    rt->DrawRectangle(rectAppendCaptureBtn_, isAppending ? brInk : brHair, 1.0f);
    const wchar_t* appendStr = isAppending ? L"STOP APPEND" : L"APPEND CAPTURE";
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(appendStr, static_cast<UINT32>(wcslen(appendStr)), fmtButtonLabel_,
            D2D1::RectF(rectAppendCaptureBtn_.left, cellTop + 6.0f, rectAppendCaptureBtn_.right, cellBot),
            isAppending ? brCard : (canAppendCapture ? brInk : brMuted));
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // 3b. Generate Button [right - 228 .. right - 142]
    rectGenerateBtn_ = snapRect(rect.right - 228.0f, cellTop, rect.right - 142.0f, cellBot);
    if (isGenerating) {
        rt->FillRectangle(rectGenerateBtn_, brOrange);
        rt->DrawRectangle(rectGenerateBtn_, brInk, 1.0f);
    } else if (tel.isPendingRegenerate) {
        rt->FillRectangle(rectGenerateBtn_, brSubWell);
        rt->DrawRectangle(rectGenerateBtn_, brOrange, 1.5f);
    } else {
        rt->FillRectangle(rectGenerateBtn_, hasCaptured ? brWell : brCard);
        rt->DrawRectangle(rectGenerateBtn_, hasCaptured ? brInk : brHair, 1.0f);
    }
    wchar_t genBuf[32];
    if (isGenerating) {
        swprintf_s(genBuf, L"GEN: %.0f%%", tel.synthesisProgress * 100.0f);
    } else {
        wcscpy_s(genBuf, L"GENERATE");
    }
    D2D1_RECT_F genTxt = D2D1::RectF(rectGenerateBtn_.left, rectGenerateBtn_.top + 6.0f, rectGenerateBtn_.right, rectGenerateBtn_.bottom);
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(genBuf, static_cast<UINT32>(wcslen(genBuf)), fmtButtonLabel_, genTxt, isGenerating ? brCard : (hasCaptured ? brInk : brMuted));
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // 3c. Unified Active / Bypass Button [right - 136 .. right - 62]
    rectActiveBtn_ = snapRect(rect.right - 136.0f, cellTop, rect.right - 62.0f, cellBot);
    if (isActive) {
        rt->FillRectangle(rectActiveBtn_, brStatus);
        rt->DrawRectangle(rectActiveBtn_, brInk, 1.0f);
    } else if (params.bypass) {
        rt->FillRectangle(rectActiveBtn_, brSubWell);
        rt->DrawRectangle(rectActiveBtn_, brOrange, 1.2f);
    } else {
        rt->FillRectangle(rectActiveBtn_, brCard);
        rt->DrawRectangle(rectActiveBtn_, brHair, 1.0f);
    }
    const wchar_t* actStr = isActive ? L"ACTIVE" : (params.bypass ? L"BYPASSED" : L"ACTIVE");
    D2D1_RECT_F actTxt = D2D1::RectF(rectActiveBtn_.left, rectActiveBtn_.top + 6.0f, rectActiveBtn_.right, rectActiveBtn_.bottom);
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(actStr, static_cast<UINT32>(wcslen(actStr)), fmtButtonLabel_, actTxt, isActive ? brCard : (params.bypass && !isCapturing && !isGenerating ? brOrange : brMuted));
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // 3d. Clear Capture Button [right - 56 .. right - 8]
    rectClearCaptureBtn_ = snapRect(rect.right - 56.0f, cellTop, rect.right - 8.0f, cellBot);
    rt->FillRectangle(rectClearCaptureBtn_, brWell);
    rt->DrawRectangle(rectClearCaptureBtn_, brHair, 1.0f);
    const wchar_t* clrStr = L"CLEAR";
    D2D1_RECT_F clrTxt = D2D1::RectF(rectClearCaptureBtn_.left, rectClearCaptureBtn_.top + 6.0f, rectClearCaptureBtn_.right, rectClearCaptureBtn_.bottom);
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(clrStr, static_cast<UINT32>(wcslen(clrStr)), fmtButtonLabel_, clrTxt, brSec);
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    if (brOrange) brOrange->Release();
    if (brStatus) brStatus->Release();
    if (brHair) brHair->Release();
    if (brSubWell) brSubWell->Release();
    if (brWell) brWell->Release();
    if (brCard) brCard->Release();
    if (brMuted) brMuted->Release();
    if (brSec) brSec->Release();
    if (brInk) brInk->Release();
}

void PluginEditorView::drawSlotPanel(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    int slotIndex,
    const RvcModelSlotInfo& info,
    float slotIndexRateVal) {

    ID2D1SolidColorBrush* brInk = nullptr;
    ID2D1SolidColorBrush* brSec = nullptr;
    ID2D1SolidColorBrush* brMuted = nullptr;
    ID2D1SolidColorBrush* brCard = nullptr;
    ID2D1SolidColorBrush* brWell = nullptr;
    ID2D1SolidColorBrush* brSubWell = nullptr;
    ID2D1SolidColorBrush* brHair = nullptr;
    ID2D1SolidColorBrush* brSubGrid = nullptr;
    ID2D1SolidColorBrush* brAccent = nullptr;

    rt->CreateSolidColorBrush(kColInkPrimary, &brInk);
    rt->CreateSolidColorBrush(kColInkSecondary, &brSec);
    rt->CreateSolidColorBrush(kColInkMuted, &brMuted);
    rt->CreateSolidColorBrush(kColPanelSurface, &brCard);
    rt->CreateSolidColorBrush(kColRecessedWell, &brWell);
    rt->CreateSolidColorBrush(kColSubtleWell, &brSubWell);
    rt->CreateSolidColorBrush(kColGridHairline, &brHair);
    rt->CreateSolidColorBrush(kColGridSubtle, &brSubGrid);
    rt->CreateSolidColorBrush((slotIndex == 0) ? kColAccentSlotA : kColAccentSlotB, &brAccent);

    const float pad = 10.0f;

    // Section Header Bar
    D2D1_RECT_F secBar = D2D1::RectF(rect.left, rect.top, rect.right, rect.top + 28.0f);
    rt->FillRectangle(secBar, brWell);
    rt->DrawLine(
        D2D1::Point2F(rect.left, std::floor(secBar.bottom) + 0.5f),
        D2D1::Point2F(rect.right, std::floor(secBar.bottom) + 0.5f),
        brHair,
        1.0f);

    // Accent geometric marker
    D2D1_RECT_F marker = D2D1::RectF(rect.left + pad, rect.top + 9.0f, rect.left + pad + 8.0f, rect.top + 17.0f);
    rt->FillRectangle(marker, brAccent);

    const wchar_t* secTitle = (slotIndex == 0) ? L"01 // RVC MODEL SLOT A" : L"03 // RVC MODEL SLOT B";
    D2D1_RECT_F secTitleRect = D2D1::RectF(rect.left + pad + 13.0f, rect.top + 6.0f, rect.right - pad, secBar.bottom);
    rt->DrawText(secTitle, static_cast<UINT32>(wcslen(secTitle)), fmtSectionCode_, secTitleRect, brInk);

    // Model Identity Card (94px height with bottom 24px Formant Signature Micro-Well)
    const float cardTop = secBar.bottom + 8.0f;
    D2D1_RECT_F modelCard = snapRect(rect.left + pad, cardTop, rect.right - pad, cardTop + 94.0f);
    rt->FillRectangle(modelCard, brCard);

    const float sigWellTop = modelCard.bottom - 24.0f;
    rt->FillRectangle(
        D2D1::RectF(modelCard.left + 4.0f, sigWellTop, modelCard.right - 1.0f, modelCard.bottom - 1.0f),
        brSubWell);
    rt->DrawLine(
        D2D1::Point2F(modelCard.left + 4.0f, std::floor(sigWellTop) + 0.5f),
        D2D1::Point2F(modelCard.right - 1.0f, std::floor(sigWellTop) + 0.5f),
        brSubGrid,
        1.0f);

    rt->FillRectangle(D2D1::RectF(modelCard.left, modelCard.top, modelCard.left + 4.0f, modelCard.bottom), brAccent);
    rt->DrawRectangle(modelCard, brHair, 1.0f);

    const wchar_t* slotStatus = info.isLoaded ? L"STATUS: LOADED // READY" : L"STATUS: USER IMPORT ONLY";
    rt->DrawText(
        slotStatus,
        static_cast<UINT32>(wcslen(slotStatus)),
        fmtMonoSmall_,
        D2D1::RectF(modelCard.left + 10.0f, modelCard.top + 6.0f, modelCard.right - 6.0f, modelCard.top + 20.0f),
        info.isLoaded ? brAccent : brSec);

    std::wstring wModelName = utf8ToWide(info.modelName);
    if (wModelName.length() > 22) {
        wModelName = wModelName.substr(0, 20) + L"..";
    }
    rt->DrawText(
        wModelName.c_str(),
        static_cast<UINT32>(wModelName.length()),
        fmtModelName_,
        D2D1::RectF(modelCard.left + 10.0f, modelCard.top + 22.0f, modelCard.right - 6.0f, modelCard.top + 44.0f),
        brInk);

    std::wstring wArch = utf8ToWide(info.archTag);
    wchar_t metaLine[96];
    swprintf_s(metaLine, L"%s | KEY: %+d ST", wArch.c_str(), info.targetOctaveSemitones);
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        rt->DrawText(
            metaLine,
            static_cast<UINT32>(wcslen(metaLine)),
            fmtMonoSmall_,
            D2D1::RectF(modelCard.left + 10.0f, modelCard.top + 47.0f, modelCard.right - 6.0f, sigWellTop - 2.0f),
            brSec);
        fmtMonoSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    // FAISS .index Feature Retrieval Pairing Status Micro-Well inside Model Card
    D2D1_RECT_F idxDot = D2D1::RectF(modelCard.left + 10.0f, sigWellTop + 8.0f, modelCard.left + 16.0f, sigWellTop + 14.0f);
    wchar_t idxStatus[96];
    ID2D1SolidColorBrush* brIdxText = brMuted;
    if (!info.isLoaded) {
        wcscpy_s(idxStatus, L"INDEX: UNLOADED [.PTH + .INDEX]");
        rt->FillRectangle(idxDot, brMuted);
        brIdxText = brMuted;
    } else if (info.hasFeatureIndex) {
        swprintf_s(idxStatus, L"INDEX: PAIRED [K=%u VECS]", info.centroidCount);
        rt->FillRectangle(idxDot, brAccent);
        brIdxText = brInk;
    } else {
        wcscpy_s(idxStatus, L"INDEX: NONE [.PTH ONLY // RATE OFF]");
        rt->FillRectangle(idxDot, brSec);
        brIdxText = brSec;
    }
    rt->DrawText(
        idxStatus,
        static_cast<UINT32>(wcslen(idxStatus)),
        fmtMonoSmall_,
        D2D1::RectF(modelCard.left + 21.0f, sigWellTop + 4.0f, modelCard.right - 6.0f, modelCard.bottom - 2.0f),
        brIdxText);

    // Action Buttons Row: [ LOAD MODEL / INDEX ]  [ CLEAR ]
    const float btnRowTop = modelCard.bottom + 8.0f;
    const float splitX = rect.left + pad + (rect.right - rect.left - 2.0f * pad) * 0.70f;
    rectSlotLoadBtn_[slotIndex] = snapRect(rect.left + pad, btnRowTop, splitX - 3.0f, btnRowTop + 26.0f);
    rectSlotClearBtn_[slotIndex] = snapRect(splitX + 3.0f, btnRowTop, rect.right - pad, btnRowTop + 26.0f);

    rt->FillRectangle(rectSlotLoadBtn_[slotIndex], brInk);
    const wchar_t* loadTxt = L"LOAD MODEL / INDEX";
    if (fmtButtonLabel_) {
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(
            loadTxt,
            static_cast<UINT32>(wcslen(loadTxt)),
            fmtButtonLabel_,
            D2D1::RectF(rectSlotLoadBtn_[slotIndex].left, rectSlotLoadBtn_[slotIndex].top + 5.0f, rectSlotLoadBtn_[slotIndex].right, rectSlotLoadBtn_[slotIndex].bottom),
            brCard);
    }

    rt->FillRectangle(rectSlotClearBtn_[slotIndex], brCard);
    rt->DrawRectangle(rectSlotClearBtn_[slotIndex], brHair, 1.0f);
    const wchar_t* clrTxt = L"CLEAR";
    if (fmtButtonLabel_) {
        rt->DrawText(
            clrTxt,
            static_cast<UINT32>(wcslen(clrTxt)),
            fmtButtonLabel_,
            D2D1::RectF(rectSlotClearBtn_[slotIndex].left, rectSlotClearBtn_[slotIndex].top + 5.0f, rectSlotClearBtn_[slotIndex].right, rectSlotClearBtn_[slotIndex].bottom),
            brSec);
        fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Cross-Octave & Semitone Register Shift Selector: [-12 F->M] [-1] [0 SAME] [+1] [+12 M->F]
    const float octLabelTop = rectSlotLoadBtn_[slotIndex].bottom + 8.0f;
    const wchar_t* octHeader = L"F0 KEY SHIFT";
    rt->DrawText(
        octHeader,
        static_cast<UINT32>(wcslen(octHeader)),
        fmtMonoSmall_,
        D2D1::RectF(rect.left + pad, octLabelTop, rect.right - pad - 60.0f, octLabelTop + 14.0f),
        brSec);

    wchar_t curKeyBadge[32];
    swprintf_s(curKeyBadge, L"[ %+d ST ]", info.targetOctaveSemitones);
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            curKeyBadge,
            static_cast<UINT32>(wcslen(curKeyBadge)),
            fmtMonoSmall_,
            D2D1::RectF(rect.right - pad - 68.0f, octLabelTop, rect.right - pad, octLabelTop + 14.0f),
            (info.targetOctaveSemitones != 0) ? brAccent : brInk);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    const float octBtnTop = octLabelTop + 15.0f;
    const float totalOctW = (rect.right - rect.left) - 2.0f * pad;
    const float gap = 3.0f;
    const float stepW = 27.0f;
    const float presetW = (totalOctW - 2.0f * stepW - 4.0f * gap) / 3.0f;

    const float x0 = rect.left + pad;
    const float x1 = x0 + presetW + gap;
    const float x2 = x1 + stepW + gap;
    const float x3 = x2 + presetW + gap;
    const float x4 = x3 + stepW + gap;

    rectSlotOctaveBtn_[slotIndex][0] = snapRect(x0, octBtnTop, x0 + presetW, octBtnTop + 22.0f);
    rectSlotSemiDownBtn_[slotIndex]  = snapRect(x1, octBtnTop, x1 + stepW,   octBtnTop + 22.0f);
    rectSlotOctaveBtn_[slotIndex][1] = snapRect(x2, octBtnTop, x2 + presetW, octBtnTop + 22.0f);
    rectSlotSemiUpBtn_[slotIndex]    = snapRect(x3, octBtnTop, x3 + stepW,   octBtnTop + 22.0f);
    rectSlotOctaveBtn_[slotIndex][2] = snapRect(x4, octBtnTop, rect.right - pad, octBtnTop + 22.0f);

    static const int32_t kOctValues[3] = {-12, 0, +12};
    static const wchar_t* kOctLabels[3] = {L"-12 F->M", L"0 SAME", L"+12 M->F"};

    for (int k = 0; k < 3; ++k) {
        const bool selected = (info.targetOctaveSemitones == kOctValues[k]);
        rt->FillRectangle(rectSlotOctaveBtn_[slotIndex][k], selected ? brAccent : brCard);
        rt->DrawRectangle(rectSlotOctaveBtn_[slotIndex][k], selected ? brInk : brHair, 1.0f);

        if (fmtButtonLabel_) {
            fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            rt->DrawText(
                kOctLabels[k],
                static_cast<UINT32>(wcslen(kOctLabels[k])),
                fmtButtonLabel_,
                D2D1::RectF(
                    rectSlotOctaveBtn_[slotIndex][k].left,
                    rectSlotOctaveBtn_[slotIndex][k].top + 4.0f,
                    rectSlotOctaveBtn_[slotIndex][k].right,
                    rectSlotOctaveBtn_[slotIndex][k].bottom),
                selected ? brCard : brInk);
            fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    // Draw [-1] and [+1] fine semitone stepper buttons
    const D2D1_RECT_F stepRects[2] = {rectSlotSemiDownBtn_[slotIndex], rectSlotSemiUpBtn_[slotIndex]};
    const wchar_t* stepLabels[2] = {L"-1", L"+1"};
    for (int st = 0; st < 2; ++st) {
        rt->FillRectangle(stepRects[st], brWell);
        rt->DrawRectangle(stepRects[st], brHair, 1.0f);
        if (fmtButtonLabel_) {
            fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            rt->DrawText(
                stepLabels[st],
                static_cast<UINT32>(wcslen(stepLabels[st])),
                fmtButtonLabel_,
                D2D1::RectF(stepRects[st].left, stepRects[st].top + 4.0f, stepRects[st].right, stepRects[st].bottom),
                brInk);
            fmtButtonLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    // Per-Slot FAISS Index Retrieval Ratio Control
    const float trimTop = octBtnTop + 29.0f;
    const bool indexActive = (!info.isLoaded || info.hasFeatureIndex);
    wchar_t trimLabel[80];
    if (info.isLoaded && !info.hasFeatureIndex) {
        swprintf_s(
            trimLabel,
            L"SLOT %c INDEX RATE: %d%% [NO .INDEX]",
            (slotIndex == 0) ? L'A' : L'B',
            static_cast<int>(std::round(slotIndexRateVal * 100.0f)));
    } else {
        swprintf_s(
            trimLabel,
            L"SLOT %c INDEX RATE: %d%%",
            (slotIndex == 0) ? L'A' : L'B',
            static_cast<int>(std::round(slotIndexRateVal * 100.0f)));
    }
    rt->DrawText(
        trimLabel,
        static_cast<UINT32>(wcslen(trimLabel)),
        fmtMonoSmall_,
        D2D1::RectF(rect.left + pad, trimTop, rect.right - pad, trimTop + 14.0f),
        indexActive ? brSec : brMuted);

    rectSlotIndexBar_[slotIndex] = snapRect(rect.left + pad, trimTop + 16.0f, rect.right - pad, trimTop + 32.0f);
    rt->FillRectangle(rectSlotIndexBar_[slotIndex], brWell);
    rt->DrawRectangle(rectSlotIndexBar_[slotIndex], brHair, 1.0f);

    const float normIdxRate = static_cast<float>(plainToNormalized(
        (slotIndex == 0) ? kParamSlotAIndexRate : kParamSlotBIndexRate, slotIndexRateVal));
    const float fillW = (rectSlotIndexBar_[slotIndex].right - rectSlotIndexBar_[slotIndex].left) * normIdxRate;
    D2D1_RECT_F fillRect = D2D1::RectF(
        rectSlotIndexBar_[slotIndex].left + 1.0f,
        rectSlotIndexBar_[slotIndex].top + 1.0f,
        rectSlotIndexBar_[slotIndex].left + fillW,
        rectSlotIndexBar_[slotIndex].bottom - 1.0f);
    rt->FillRectangle(fillRect, indexActive ? brAccent : brMuted);

    const float thumbX = std::clamp(
        rectSlotIndexBar_[slotIndex].left + fillW,
        rectSlotIndexBar_[slotIndex].left + 2.0f,
        rectSlotIndexBar_[slotIndex].right - 2.0f);
    rt->DrawLine(
        D2D1::Point2F(thumbX, rectSlotIndexBar_[slotIndex].top - 2.0f),
        D2D1::Point2F(thumbX, rectSlotIndexBar_[slotIndex].bottom + 2.0f),
        brInk,
        2.0f);

    rt->DrawText(
        L"MIN 0%",
        6,
        fmtMonoSmall_,
        D2D1::RectF(rect.left + pad, trimTop + 34.0f, rect.left + pad + 70.0f, trimTop + 48.0f),
        brMuted);
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            L"MAX 100%",
            8,
            fmtMonoSmall_,
            D2D1::RectF(rect.right - pad - 70.0f, trimTop + 34.0f, rect.right - pad, trimTop + 48.0f),
            brMuted);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    if (brAccent) brAccent->Release();
    if (brSubGrid) brSubGrid->Release();
    if (brHair) brHair->Release();
    if (brSubWell) brSubWell->Release();
    if (brWell) brWell->Release();
    if (brCard) brCard->Release();
    if (brMuted) brMuted->Release();
    if (brSec) brSec->Release();
    if (brInk) brInk->Release();
}

void PluginEditorView::drawCenterScopeAndMorph(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    const VisualTelemetrySnapshot& tel,
    const RvcVoiceParameters& params) {

    ID2D1SolidColorBrush* brInk = nullptr;
    ID2D1SolidColorBrush* brSec = nullptr;
    ID2D1SolidColorBrush* brMuted = nullptr;
    ID2D1SolidColorBrush* brWell = nullptr;
    ID2D1SolidColorBrush* brSubWell = nullptr;
    ID2D1SolidColorBrush* brCard = nullptr;
    ID2D1SolidColorBrush* brHair = nullptr;
    ID2D1SolidColorBrush* brSubGrid = nullptr;
    ID2D1SolidColorBrush* brSlotA = nullptr;
    ID2D1SolidColorBrush* brSlotB = nullptr;
    ID2D1SolidColorBrush* brStatus = nullptr;

    rt->CreateSolidColorBrush(kColInkPrimary, &brInk);
    rt->CreateSolidColorBrush(kColInkSecondary, &brSec);
    rt->CreateSolidColorBrush(kColInkMuted, &brMuted);
    rt->CreateSolidColorBrush(kColRecessedWell, &brWell);
    rt->CreateSolidColorBrush(kColSubtleWell, &brSubWell);
    rt->CreateSolidColorBrush(kColPanelSurface, &brCard);
    rt->CreateSolidColorBrush(kColGridHairline, &brHair);
    rt->CreateSolidColorBrush(kColGridSubtle, &brSubGrid);
    rt->CreateSolidColorBrush(kColAccentSlotA, &brSlotA);
    rt->CreateSolidColorBrush(kColAccentSlotB, &brSlotB);
    rt->CreateSolidColorBrush(kColStatusNominal, &brStatus);

    // Section Header Bar
    D2D1_RECT_F secBar = D2D1::RectF(rect.left, rect.top, rect.right, rect.top + 28.0f);
    rt->FillRectangle(secBar, brWell);
    rt->DrawLine(
        D2D1::Point2F(rect.left, std::floor(secBar.bottom) + 0.5f),
        D2D1::Point2F(rect.right, std::floor(secBar.bottom) + 0.5f),
        brHair,
        1.0f);

    const wchar_t* secTitle = L"02 // TIMELINE WAVEFORM & THREE-WAY BALANCE";
    rt->DrawText(
        secTitle,
        static_cast<UINT32>(wcslen(secTitle)),
        fmtSectionCode_,
        D2D1::RectF(rect.left + 10.0f, rect.top + 6.0f, rect.right - 108.0f, secBar.bottom),
        brInk);

    // Pitch Telemetry on right of Section Header
    char noteAscii[32]{};
    const wchar_t* pitchStr = freqToNoteName(tel.detectedPitchHz, noteAscii, sizeof(noteAscii));
    wchar_t headerRightBuf[64];
    swprintf_s(headerRightBuf, L"F0: %s", pitchStr);
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            headerRightBuf,
            static_cast<UINT32>(wcslen(headerRightBuf)),
            fmtMonoSmall_,
            D2D1::RectF(rect.right - 115.0f, rect.top + 7.0f, rect.right - 8.0f, secBar.bottom),
            brSec);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    const float pad = 10.0f;
    const float contentTop = secBar.bottom + 8.0f;
    const float contentBottom = rect.bottom - 8.0f;

    // Split Center Workspace into Left (Timeline Waveform + Cache Strip) and Right (Three-Way Balance Card)
    const float rightCardWidth = 224.0f;
    const float rightCardLeft = rect.right - pad - rightCardWidth;
    const float leftAreaRight = rightCardLeft - 8.0f;

    // ========================================================================
    // LEFT MODULE: Captured Time-Domain Waveform View + Neural Cache Strip
    // ========================================================================
    const float waveBottom = contentBottom - 54.0f;
    D2D1_RECT_F waveRect = snapRect(rect.left + pad, contentTop, leftAreaRight, waveBottom);
    rt->FillRectangle(waveRect, brCard);

    // Top Strip of Waveform Card (18px)
    const float waveHeaderBot = waveRect.top + 18.0f;
    rt->FillRectangle(D2D1::RectF(waveRect.left + 1.0f, waveRect.top + 1.0f, waveRect.right - 1.0f, waveHeaderBot), brWell);
    rt->DrawLine(D2D1::Point2F(waveRect.left, waveHeaderBot + 0.5f), D2D1::Point2F(waveRect.right, waveHeaderBot + 0.5f), brHair, 1.0f);

    rt->DrawText(
        L"CAPTURED TIMELINE WAVEFORM",
        26,
        fmtMonoSmall_,
        D2D1::RectF(waveRect.left + 6.0f, waveRect.top + 2.0f, waveRect.right - 74.0f, waveHeaderBot),
        brInk);

    wchar_t durBuf[48];
    if (tel.capturedDurationSec > 0.001f) {
        swprintf_s(durBuf, L"%.2fs", tel.capturedDurationSec);
    } else {
        wcscpy_s(durBuf, L"0.00s");
    }
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            durBuf,
            static_cast<UINT32>(wcslen(durBuf)),
            fmtMonoSmall_,
            D2D1::RectF(waveRect.right - 72.0f, waveRect.top + 2.0f, waveRect.right - 6.0f, waveHeaderBot),
            brSec);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Bottom Time Ruler Strip (15px)
    const float rulerTop = waveRect.bottom - 15.0f;
    rt->FillRectangle(D2D1::RectF(waveRect.left + 1.0f, rulerTop, waveRect.right - 1.0f, waveRect.bottom - 1.0f), brWell);
    rt->DrawLine(D2D1::Point2F(waveRect.left, std::floor(rulerTop) + 0.5f), D2D1::Point2F(waveRect.right, std::floor(rulerTop) + 0.5f), brHair, 1.0f);

    const float plotTop = waveHeaderBot + 2.0f;
    const float plotBot = rulerTop - 2.0f;
    const float plotMidY = std::floor((plotTop + plotBot) * 0.5f) + 0.5f;
    const float plotHalfH = (plotBot - plotTop) * 0.46f;

    // Horizontal amplitude guide lines (+0.5, 0.0, -0.5)
    const float qTopY = std::floor(plotMidY - plotHalfH * 0.55f) + 0.5f;
    const float qBotY = std::floor(plotMidY + plotHalfH * 0.55f) + 0.5f;
    rt->DrawLine(D2D1::Point2F(waveRect.left + 1.0f, qTopY), D2D1::Point2F(waveRect.right - 1.0f, qTopY), brSubGrid, 1.0f);
    rt->DrawLine(D2D1::Point2F(waveRect.left + 1.0f, qBotY), D2D1::Point2F(waveRect.right - 1.0f, qBotY), brSubGrid, 1.0f);
    rt->DrawLine(D2D1::Point2F(waveRect.left + 1.0f, plotMidY), D2D1::Point2F(waveRect.right - 1.0f, plotMidY), brHair, 1.0f);

    // 5 Time Ruler Ticks (in seconds)
    const float dispDur = (tel.capturedDurationSec > 0.01f) ? tel.capturedDurationSec : 10.0f;
    for (int t = 0; t < 5; ++t) {
        const float frac = static_cast<float>(t) / 4.0f;
        const float gx = std::floor(waveRect.left + 8.0f + (waveRect.right - waveRect.left - 16.0f) * frac) + 0.5f;
        if (t > 0 && t < 4) {
            rt->DrawLine(D2D1::Point2F(gx, waveHeaderBot + 1.0f), D2D1::Point2F(gx, rulerTop), brSubGrid, 1.0f);
        }
        wchar_t tLabel[24];
        swprintf_s(tLabel, (dispDur < 2.0f) ? L"%.2fs" : L"%.1fs", dispDur * frac);
        D2D1_RECT_F tr = D2D1::RectF(gx - 18.0f, rulerTop + 1.0f, gx + 24.0f, waveRect.bottom);
        if (t == 0) {
            tr = D2D1::RectF(waveRect.left + 4.0f, rulerTop + 1.0f, waveRect.left + 44.0f, waveRect.bottom);
        } else if (t == 4) {
            tr = D2D1::RectF(waveRect.right - 44.0f, rulerTop + 1.0f, waveRect.right - 3.0f, waveRect.bottom);
        }
        if (fmtMonoSmall_) {
            fmtMonoSmall_->SetTextAlignment((t == 0) ? DWRITE_TEXT_ALIGNMENT_LEADING : ((t == 4) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER));
            rt->DrawText(tLabel, static_cast<UINT32>(wcslen(tLabel)), fmtMonoSmall_, tr, brSec);
            fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    // Plot Time-Domain Captured Waveform Bars
    const auto curState = static_cast<TimelineState>(tel.timelineStateVal);
    if (tel.capturedDurationSec > 0.001f) {
        const float plotW = (waveRect.right - waveRect.left) - 8.0f;
        const size_t numCols = 112;
        ID2D1SolidColorBrush* brRmsBody = (curState == TimelineState::kActive) ? brStatus : brSlotA;

        for (size_t c = 0; c < numCols; ++c) {
            const size_t bin0 = (c * kWaveformBins) / numCols;
            const size_t bin1 = std::max(bin0 + 1, ((c + 1) * kWaveformBins) / numCols);
            float pk = 0.0f;
            float rm = 0.0f;
            for (size_t b = bin0; b < bin1 && b < kWaveformBins; ++b) {
                pk = std::max(pk, tel.capturedWaveformPeak[b]);
                rm = std::max(rm, tel.capturedWaveformRms[b]);
            }
            // Visual dynamic scaling so quiet phrases are still clearly legible
            const float normPk = std::clamp( std::pow(std::clamp(pk * 2.6f, 0.0f, 1.0f), 0.72f), 0.02f, 1.0f );
            const float normRm = std::clamp( std::pow(std::clamp(rm * 2.6f, 0.0f, 1.0f), 0.72f), 0.01f, normPk );

            const float x = std::floor(waveRect.left + 4.0f + (static_cast<float>(c) + 0.5f) * (plotW / static_cast<float>(numCols))) + 0.5f;
            const float pkH = normPk * plotHalfH;
            const float rmH = normRm * plotHalfH;

            rt->DrawLine(D2D1::Point2F(x, plotMidY - pkH), D2D1::Point2F(x, plotMidY + pkH), brMuted, 1.2f);
            rt->DrawLine(D2D1::Point2F(x, plotMidY - rmH), D2D1::Point2F(x, plotMidY + rmH), brRmsBody, 1.5f);
        }

        // Playhead Position Marker inside Captured Region
        if (tel.playheadRelSec >= 0.0f && tel.playheadRelSec <= tel.capturedDurationSec) {
            const float phNorm = std::clamp(tel.playheadRelSec / tel.capturedDurationSec, 0.0f, 1.0f);
            const float phX = std::floor(waveRect.left + 4.0f + phNorm * plotW) + 0.5f;
            rt->DrawLine(D2D1::Point2F(phX, waveHeaderBot + 1.0f), D2D1::Point2F(phX, rulerTop), brInk, 1.6f);
            D2D1_RECT_F phCap = D2D1::RectF(phX - 3.0f, waveHeaderBot + 1.0f, phX + 3.0f, waveHeaderBot + 6.0f);
            rt->FillRectangle(phCap, brSlotA);
        }
    } else {
        if (fmtMonoSmall_) {
            fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            const wchar_t* emptyMsg = L"CLICK [CAPTURE] & PLAY DAW TO RECORD VOCAL";
            rt->DrawText(
                emptyMsg,
                static_cast<UINT32>(wcslen(emptyMsg)),
                fmtMonoSmall_,
                D2D1::RectF(waveRect.left + 8.0f, plotMidY - 8.0f, waveRect.right - 8.0f, plotMidY + 10.0f),
                brMuted);
            fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    rt->DrawRectangle(waveRect, brInk, 1.0f);

    // Neural Cache Status & Progress Strip below Waveform (48px tall, clean buttonless status & segment readout)
    const float cacheTop = waveRect.bottom + 6.0f;
    D2D1_RECT_F cacheCard = snapRect(rect.left + pad, cacheTop, leftAreaRight, contentBottom);
    rt->FillRectangle(cacheCard, brCard);
    rt->DrawRectangle(cacheCard, brHair, 1.0f);

    const bool attentionColor = (tel.isSynthesizing || tel.isPendingRegenerate || tel.captureCapacityExceeded);
    D2D1_RECT_F cacheLed = D2D1::RectF(cacheCard.left + 6.0f, cacheCard.top + 6.0f, cacheCard.left + 11.0f, cacheCard.top + 11.0f);
    rt->FillRectangle(cacheLed, attentionColor ? brSlotA : brStatus);

    std::wstring wBanner = tel.captureCapacityExceeded
        ? L"240S LIMIT: PREVIOUS AUDIO KEPT" : utf8ToWide(tel.statusBannerText);
    if (wBanner.length() > 33) {
        wBanner = wBanner.substr(0, 31) + L"..";
    }
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        rt->DrawText(
            wBanner.c_str(),
            static_cast<UINT32>(wBanner.length()),
            fmtMonoSmall_,
            D2D1::RectF(cacheCard.left + 15.0f, cacheCard.top + 4.0f, cacheCard.right - 78.0f, cacheCard.top + 18.0f),
            (tel.isPendingRegenerate || tel.captureCapacityExceeded) ? brSlotA : brInk);

        wchar_t segsBuf[32];
        swprintf_s(segsBuf, L"SEGS: %u/%u", tel.cachedChunksCount, tel.totalActiveChunks);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            segsBuf,
            static_cast<UINT32>(wcslen(segsBuf)),
            fmtMonoSmall_,
            D2D1::RectF(cacheCard.right - 76.0f, cacheCard.top + 4.0f, cacheCard.right - 6.0f, cacheCard.top + 18.0f),
            brSec);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmtMonoSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    // Progress Bar Track on lower row of Cache Strip
    D2D1_RECT_F progTrack = snapRect(cacheCard.left + 6.0f, cacheCard.top + 24.0f, cacheCard.right - 42.0f, cacheCard.bottom - 5.0f);
    rt->FillRectangle(progTrack, brWell);
    rt->DrawRectangle(progTrack, brInk, 1.0f);

    const float progVal = std::clamp(tel.synthesisProgress, 0.0f, 1.0f);
    const float progTrackW = (progTrack.right - progTrack.left) - 2.0f;
    const float progFillW = progTrackW * progVal;
    if (progFillW > 1.0f) {
        D2D1_RECT_F progFill = D2D1::RectF(progTrack.left + 1.0f, progTrack.top + 1.5f, progTrack.left + 1.0f + progFillW, progTrack.bottom - 1.5f);
        rt->FillRectangle(progFill, attentionColor ? brSlotA : brStatus);
    }

    wchar_t progPctBuf[16];
    swprintf_s(progPctBuf, L"%3d%%", static_cast<int>(std::round(progVal * 100.0f)));
    rt->DrawText(
        progPctBuf,
        static_cast<UINT32>(wcslen(progPctBuf)),
        fmtSectionCode_,
        D2D1::RectF(progTrack.right + 4.0f, cacheCard.top + 22.0f, cacheCard.right - 2.0f, cacheCard.bottom),
        attentionColor ? brSlotA : brInk);

    // ========================================================================
    // RIGHT MODULE: THREE-WAY BALANCE Triangle Card (Image 1 Specification)
    // ========================================================================
    rectTrianglePad_ = snapRect(rightCardLeft, contentTop, rect.right - pad, contentBottom);
    rt->FillRectangle(rectTrianglePad_, brCard);
    rt->DrawRectangle(rectTrianglePad_, brHair, 1.0f);

    // Top Header Band of Three-Way Balance Card
    const float triHeaderBot = rectTrianglePad_.top + 22.0f;
    rt->FillRectangle(D2D1::RectF(rectTrianglePad_.left + 1.0f, rectTrianglePad_.top + 1.0f, rectTrianglePad_.right - 1.0f, triHeaderBot), brWell);
    rt->DrawLine(D2D1::Point2F(rectTrianglePad_.left, triHeaderBot + 0.5f), D2D1::Point2F(rectTrianglePad_.right, triHeaderBot + 0.5f), brHair, 1.0f);

    rt->DrawText(
        L"THREE-WAY BALANCE",
        17,
        fmtSectionCode_,
        D2D1::RectF(rectTrianglePad_.left + 8.0f, rectTrianglePad_.top + 4.0f, rectTrianglePad_.right - 6.0f, triHeaderBot),
        brInk);

    // Subtitle: "Original voice + baked A / B"
    rt->DrawText(
        L"Original voice + baked A / B",
        28,
        fmtMonoSmall_,
        D2D1::RectF(rectTrianglePad_.left + 8.0f, triHeaderBot + 4.0f, rectTrianglePad_.right - 6.0f, triHeaderBot + 18.0f),
        brSec);

    // Triangle Geometry
    const float triCx = std::floor((rectTrianglePad_.left + rectTrianglePad_.right) * 0.5f) + 0.5f;
    triVertexDry_ = D2D1::Point2F(triCx, triHeaderBot + 35.0f);
    triVertexA_   = D2D1::Point2F(triCx - 74.0f, triHeaderBot + 144.0f);
    triVertexB_   = D2D1::Point2F(triCx + 74.0f, triHeaderBot + 144.0f);

    // "DRY" Vertex Title
    if (fmtSectionCode_) {
        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(
            L"DRY",
            3,
            fmtSectionCode_,
            D2D1::RectF(triCx - 30.0f, triVertexDry_.y - 16.0f, triCx + 30.0f, triVertexDry_.y - 1.0f),
            brInk);
        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Internal Barycentric Subdivision Grid Lines (4-part subdivision along all 3 axes)
    for (int k = 1; k <= 3; ++k) {
        const float t = static_cast<float>(k) / 4.0f;
        // Horizontal lines parallel to A-B
        rt->DrawLine(lerpPt(triVertexDry_, triVertexA_, t), lerpPt(triVertexDry_, triVertexB_, t), brSubGrid, 1.0f);
        // Lines parallel to DRY-B
        rt->DrawLine(lerpPt(triVertexA_, triVertexDry_, t), lerpPt(triVertexA_, triVertexB_, 1.0f - t), brSubGrid, 1.0f);
        // Lines parallel to DRY-A
        rt->DrawLine(lerpPt(triVertexB_, triVertexDry_, t), lerpPt(triVertexB_, triVertexA_, 1.0f - t), brSubGrid, 1.0f);
    }
    // Central vertical median line from DRY to midpoint of A-B
    rt->DrawLine(triVertexDry_, D2D1::Point2F(triCx, triVertexA_.y), brHair, 1.0f);

    // Compute Effective Barycentric Weights (wDry, wA, wB) bi-directionally linked with P.06 DRY/WET MIX
    const float rawWet = std::clamp(params.dryWetMix, 0.0f, 1.0f);
    const float rawMorph = std::clamp(params.abMorph, 0.0f, 1.0f);
    float wDry = 1.0f - rawWet;
    float wA = rawWet * (1.0f - rawMorph);
    float wB = rawWet * rawMorph;

    if (tel.slotALoaded && !tel.slotBLoaded) {
        wDry = 1.0f - rawWet;
        wA = rawWet;
        wB = 0.0f;
    } else if (!tel.slotALoaded && tel.slotBLoaded) {
        wDry = 1.0f - rawWet;
        wA = 0.0f;
        wB = rawWet;
    }

    const D2D1_POINT_2F puckPt = D2D1::Point2F(
        wDry * triVertexDry_.x + wA * triVertexA_.x + wB * triVertexB_.x,
        wDry * triVertexDry_.y + wA * triVertexA_.y + wB * triVertexB_.y);

    // Thin vector tether lines from the 3 vertices to the puck center
    rt->DrawLine(triVertexDry_, puckPt, brHair, 1.0f);
    rt->DrawLine(triVertexA_, puckPt, brHair, 1.0f);
    rt->DrawLine(triVertexB_, puckPt, brHair, 1.0f);

    // Outer Triangle Colored Edges (Orange Left = Model A, Blue Right = Model B, Carbon Bottom)
    rt->DrawLine(triVertexDry_, triVertexA_, (!tel.slotBLoaded || tel.slotALoaded) ? brSlotA : brMuted, 2.0f);
    rt->DrawLine(triVertexDry_, triVertexB_, (!tel.slotALoaded || tel.slotBLoaded) ? brSlotB : brMuted, 2.0f);
    rt->DrawLine(triVertexA_, triVertexB_, brSec, 1.5f);

    // Dual-Circle Precision Puck (Outer Dark Ring + Center Dot)
    D2D1_ELLIPSE puckOuter = D2D1::Ellipse(puckPt, 7.0f, 7.0f);
    rt->FillEllipse(puckOuter, brCard);
    rt->DrawEllipse(puckOuter, brInk, 1.8f);
    D2D1_ELLIPSE puckInner = D2D1::Ellipse(puckPt, 2.2f, 2.2f);
    rt->FillEllipse(puckInner, brInk);

    // Bottom Vertex Labels: "MODEL A" (left orange) and "MODEL B" (right blue)
    const float modelLabelTop = triVertexA_.y + 5.0f;
    rt->DrawText(
        L"MODEL A",
        7,
        fmtSectionCode_,
        D2D1::RectF(rectTrianglePad_.left + 14.0f, modelLabelTop, triCx, modelLabelTop + 16.0f),
        brSlotA);

    if (fmtSectionCode_) {
        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            L"MODEL B",
            7,
            fmtSectionCode_,
            D2D1::RectF(triCx, modelLabelTop, rectTrianglePad_.right - 14.0f, modelLabelTop + 16.0f),
            brSlotB);
        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Horizontal Divider Rule below MODEL A / MODEL B
    const float divLineY = std::floor(modelLabelTop + 19.0f) + 0.5f;
    rt->DrawLine(
        D2D1::Point2F(rectTrianglePad_.left + 10.0f, divLineY),
        D2D1::Point2F(rectTrianglePad_.right - 10.0f, divLineY),
        brSec,
        1.0f);

    // Three-Way Percentage Readout Row: "DRY xx%" | "A xx%" | "B xx%"
    int pctDry = static_cast<int>(std::round(wDry * 100.0f));
    int pctA = static_cast<int>(std::round(wA * 100.0f));
    int pctB = 100 - pctDry - pctA;
    if (pctB < 0) pctB = 0;

    const float pctRowTop = divLineY + 4.0f;
    wchar_t dryPctStr[24], aPctStr[24], bPctStr[24];
    swprintf_s(dryPctStr, L"DRY %d%%", pctDry);
    swprintf_s(aPctStr, L"A %d%%", pctA);
    swprintf_s(bPctStr, L"B %d%%", pctB);

    rt->DrawText(
        dryPctStr,
        static_cast<UINT32>(wcslen(dryPctStr)),
        fmtSectionCode_,
        D2D1::RectF(rectTrianglePad_.left + 14.0f, pctRowTop, rectTrianglePad_.left + 80.0f, pctRowTop + 16.0f),
        brInk);

    if (fmtSectionCode_) {
        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(
            aPctStr,
            static_cast<UINT32>(wcslen(aPctStr)),
            fmtSectionCode_,
            D2D1::RectF(triCx - 34.0f, pctRowTop, triCx + 34.0f, pctRowTop + 16.0f),
            brSlotA);

        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(
            bPctStr,
            static_cast<UINT32>(wcslen(bPctStr)),
            fmtSectionCode_,
            D2D1::RectF(rectTrianglePad_.right - 76.0f, pctRowTop, rectTrianglePad_.right - 14.0f, pctRowTop + 16.0f),
            brSlotB);
        fmtSectionCode_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Bottom Caption: "Input balance / before realtime effects"
    if (fmtMonoSmall_) {
        fmtMonoSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const wchar_t* capBottom = L"Input balance / before realtime effects";
        rt->DrawText(
            capBottom,
            static_cast<UINT32>(wcslen(capBottom)),
            fmtMonoSmall_,
            D2D1::RectF(rectTrianglePad_.left + 2.0f, pctRowTop + 21.0f, rectTrianglePad_.right - 2.0f, rectTrianglePad_.bottom - 2.0f),
            brSec);
        fmtMonoSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmtMonoSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    if (brStatus) brStatus->Release();
    if (brSlotB) brSlotB->Release();
    if (brSlotA) brSlotA->Release();
    if (brSubGrid) brSubGrid->Release();
    if (brHair) brHair->Release();
    if (brCard) brCard->Release();
    if (brSubWell) brSubWell->Release();
    if (brWell) brWell->Release();
    if (brMuted) brMuted->Release();
    if (brSec) brSec->Release();
    if (brInk) brInk->Release();
}

void PluginEditorView::drawBottomParameterDeck(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    const RvcVoiceParameters& params) {

    ID2D1SolidColorBrush* brInk = nullptr;
    ID2D1SolidColorBrush* brWell = nullptr;
    ID2D1SolidColorBrush* brHair = nullptr;

    rt->CreateSolidColorBrush(kColInkPrimary, &brInk);
    rt->CreateSolidColorBrush(kColRecessedWell, &brWell);
    rt->CreateSolidColorBrush(kColGridHairline, &brHair);

    // Section Header Strip
    D2D1_RECT_F secBar = D2D1::RectF(rect.left, rect.top, rect.right, rect.top + 26.0f);
    rt->FillRectangle(secBar, brWell);
    rt->DrawLine(
        D2D1::Point2F(rect.left, std::floor(secBar.bottom) + 0.5f),
        D2D1::Point2F(rect.right, std::floor(secBar.bottom) + 0.5f),
        brHair,
        1.0f);

    const wchar_t* deckTitle = L"04 // REAL-TIME VOCAL EFFECTOR MATRIX  [ DOUBLE-CLICK KNOB TO RESET ]";
    rt->DrawText(
        deckTitle,
        static_cast<UINT32>(wcslen(deckTitle)),
        fmtSectionCode_,
        D2D1::RectF(rect.left + 12.0f, rect.top + 5.0f, rect.right - 12.0f, secBar.bottom),
        brInk);

    struct DialSpec {
        uint32_t paramId;
        const wchar_t* codeTag;
        const wchar_t* title;
        float plainVal;
        bool bipolar;
        wchar_t valueText[32];
    };

    DialSpec specs[6] = {
        {kParamPitchShift, L"P.01", L"PITCH SHIFT", params.pitchShiftSemitones, true, L""},
        {kParamThroatFormant, L"P.02", L"THROAT FORMANT", params.throatFormantSemitones, true, L""},
        {kParamPresenceAir, L"P.03", L"PRESENCE AIR", params.presenceAirDb, true, L""},
        {kParamRmsMixRate, L"P.04", L"RMS ENVELOPE", params.rmsMixRate, false, L""},
        {kParamDryWetMix, L"P.05", L"DRY / WET MIX", params.dryWetMix, false, L""},
        {kParamOutputGain, L"P.06", L"OUTPUT GAIN", params.outputGainDb, true, L""},
    };

    swprintf_s(specs[0].valueText, L"%+.1f ST", params.pitchShiftSemitones);
    swprintf_s(specs[1].valueText, L"%+.1f ST", params.throatFormantSemitones);
    swprintf_s(specs[2].valueText, L"%+.1f dB", params.presenceAirDb);
    swprintf_s(specs[3].valueText, L"%d %%", static_cast<int>(std::round(params.rmsMixRate * 100.0f)));
    swprintf_s(specs[4].valueText, L"%d %%", static_cast<int>(std::round(params.dryWetMix * 100.0f)));
    swprintf_s(specs[5].valueText, L"%+.1f dB", params.outputGainDb);

    const float totalW = rect.right - rect.left;
    const float cellW = totalW / 6.0f;
    const float cellTop = secBar.bottom;

    for (int i = 0; i < 6; ++i) {
        const float cl = rect.left + static_cast<float>(i) * cellW;
        const float cr = (i == 5) ? rect.right : (cl + cellW);
        D2D1_RECT_F cellRect = D2D1::RectF(cl, cellTop, cr, rect.bottom);
        rectBottomDials_[i] = cellRect;

        if (i > 0) {
            const float divX = std::floor(cl) + 0.5f;
            rt->DrawLine(D2D1::Point2F(divX, cellTop), D2D1::Point2F(divX, rect.bottom), brHair, 1.0f);
            drawCrosshairTick(rt, brInk, divX, cellTop, 3.0f);
        }

        const float normVal = static_cast<float>(plainToNormalized(specs[i].paramId, specs[i].plainVal));
        const bool isHovered = (hoveredParamId_ == static_cast<int>(specs[i].paramId));
        const bool isActive = (activeDragParamId_ == static_cast<int>(specs[i].paramId));

        drawPrecisionRotaryDial(
            rt,
            cellRect,
            i,
            specs[i].codeTag,
            specs[i].title,
            specs[i].valueText,
            normVal,
            specs[i].bipolar,
            isHovered,
            isActive);
    }

    if (brHair) brHair->Release();
    if (brWell) brWell->Release();
    if (brInk) brInk->Release();
}

void PluginEditorView::drawPrecisionRotaryDial(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& cellRect,
    int /*dialIndex*/,
    const wchar_t* codeTag,
    const wchar_t* title,
    const wchar_t* formattedValue,
    float normalizedVal,
    bool bipolar,
    bool isHovered,
    bool isActive) {

    ID2D1SolidColorBrush* brInk = nullptr;
    ID2D1SolidColorBrush* brSec = nullptr;
    ID2D1SolidColorBrush* brMuted = nullptr;
    ID2D1SolidColorBrush* brCard = nullptr;
    ID2D1SolidColorBrush* brWell = nullptr;
    ID2D1SolidColorBrush* brHair = nullptr;
    ID2D1SolidColorBrush* brAccent = nullptr;

    rt->CreateSolidColorBrush(kColInkPrimary, &brInk);
    rt->CreateSolidColorBrush(kColInkSecondary, &brSec);
    rt->CreateSolidColorBrush(kColInkMuted, &brMuted);
    rt->CreateSolidColorBrush(kColPanelSurface, &brCard);
    rt->CreateSolidColorBrush(kColSubtleWell, &brWell);
    rt->CreateSolidColorBrush(kColGridHairline, &brHair);
    rt->CreateSolidColorBrush(kColInkPrimary, &brAccent);

    if (isHovered || isActive) {
        rt->FillRectangle(cellRect, brWell);
    }

    D2D1_RECT_F codeRect = D2D1::RectF(cellRect.left + 10.0f, cellRect.top + 4.0f, cellRect.right - 10.0f, cellRect.top + 16.0f);
    rt->DrawText(codeTag, static_cast<UINT32>(wcslen(codeTag)), fmtMonoSmall_, codeRect, brSec);

    D2D1_RECT_F titleRect = D2D1::RectF(cellRect.left + 10.0f, cellRect.top + 17.0f, cellRect.right - 10.0f, cellRect.top + 33.0f);
    rt->DrawText(title, static_cast<UINT32>(wcslen(title)), fmtSectionCode_, titleRect, brInk);

    const float cx = (cellRect.left + cellRect.right) * 0.5f;
    const float cy = cellRect.top + 90.0f;
    const float outerRadius = 42.0f;
    const float rotorRadius = 31.0f;

    const float startDeg = -135.0f;
    const float endDeg = 135.0f;
    const float valDeg = startDeg + normalizedVal * (endDeg - startDeg);

    constexpr int kNumTicks = 25;
    for (int t = 0; t < kNumTicks; ++t) {
        const float tNorm = static_cast<float>(t) / static_cast<float>(kNumTicks - 1);
        const float deg = startDeg + tNorm * (endDeg - startDeg);
        const float rad = (deg - 90.0f) * (kPi / 180.0f);
        const bool isMajor = (t == 0 || t == kNumTicks - 1 || t == kNumTicks / 2);

        const float rInner = outerRadius - (isMajor ? 7.5f : 4.0f);
        const float rOuter = outerRadius;

        const bool isLit = bipolar
                               ? ((normalizedVal >= 0.5f && tNorm >= 0.5f && tNorm <= normalizedVal + 0.01f) ||
                                  (normalizedVal < 0.5f && tNorm <= 0.5f && tNorm >= normalizedVal - 0.01f))
                               : (tNorm <= normalizedVal + 0.01f);

        ID2D1SolidColorBrush* tickBrush = isLit ? brAccent : (isMajor ? brInk : brHair);
        const float strokeW = isLit ? 1.8f : (isMajor ? 1.5f : 1.0f);

        rt->DrawLine(
            D2D1::Point2F(cx + std::cos(rad) * rInner, cy + std::sin(rad) * rInner),
            D2D1::Point2F(cx + std::cos(rad) * rOuter, cy + std::sin(rad) * rOuter),
            tickBrush,
            strokeW);
    }

    if (bipolar) {
        D2D1_ELLIPSE centerDot = D2D1::Ellipse(D2D1::Point2F(cx, cy - outerRadius - 4.5f), 1.3f, 1.3f);
        rt->FillEllipse(centerDot, brSec);
    }

    D2D1_ELLIPSE outerCap = D2D1::Ellipse(D2D1::Point2F(cx, cy), rotorRadius, rotorRadius);
    rt->FillEllipse(outerCap, brCard);
    rt->DrawEllipse(outerCap, brInk, isActive ? 2.0f : 1.5f);

    D2D1_ELLIPSE innerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), rotorRadius - 5.0f, rotorRadius - 5.0f);
    rt->DrawEllipse(innerRing, brHair, 1.0f);

    const float valRad = (valDeg - 90.0f) * (kPi / 180.0f);
    rt->DrawLine(
        D2D1::Point2F(cx + std::cos(valRad) * 6.0f, cy + std::sin(valRad) * 6.0f),
        D2D1::Point2F(cx + std::cos(valRad) * (rotorRadius - 2.0f), cy + std::sin(valRad) * (rotorRadius - 2.0f)),
        brAccent,
        2.2f);

    D2D1_ELLIPSE axleDot = D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.5f, 2.5f);
    rt->FillEllipse(axleDot, brInk);

    if (fmtMonoValue_) {
        fmtMonoValue_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        D2D1_RECT_F valTextRect = D2D1::RectF(cellRect.left, cellRect.bottom - 32.0f, cellRect.right, cellRect.bottom - 12.0f);
        rt->DrawText(
            formattedValue,
            static_cast<UINT32>(wcslen(formattedValue)),
            fmtMonoValue_,
            valTextRect,
            isActive ? brAccent : brInk);
        fmtMonoValue_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    const float midX = (cellRect.left + cellRect.right) * 0.5f;
    rt->DrawLine(
        D2D1::Point2F(midX - 20.0f, cellRect.bottom - 9.0f),
        D2D1::Point2F(midX + 20.0f, cellRect.bottom - 9.0f),
        brHair,
        1.0f);

    if (brAccent) brAccent->Release();
    if (brHair) brHair->Release();
    if (brWell) brWell->Release();
    if (brCard) brCard->Release();
    if (brMuted) brMuted->Release();
    if (brSec) brSec->Release();
    if (brInk) brInk->Release();
}

void PluginEditorView::notifyGesture(uint32_t paramId, bool isBegin) {
    if (onParamGesture_) {
        onParamGesture_(paramId, isBegin);
    }
}

void PluginEditorView::applyNormalizedParameter(uint32_t paramId, double normVal) {
    normVal = std::clamp(normVal, 0.0, 1.0);
    RvcVoiceParameters p = dspInjector_.getParameters();
    const float plain = normalizedToPlain(paramId, normVal);

    switch (paramId) {
        case kParamAbMorph:          p.abMorph = plain; break;
        case kParamInjectDepth:      p.injectDepth = plain; p.dryWetMix = plain; break;
        case kParamPitchShift:       p.pitchShiftSemitones = plain; break;
        case kParamThroatFormant:    p.throatFormantSemitones = plain; break;
        case kParamPresenceAir:      p.presenceAirDb = plain; break;
        case kParamConsonantProtect: p.consonantProtect = plain; break;
        case kParamRmsMixRate:       p.rmsMixRate = plain; break;
        case kParamDryWetMix:        p.dryWetMix = plain; p.injectDepth = plain; break;
        case kParamSlotAIndexRate:   p.slotAIndexRate = plain; break;
        case kParamSlotBIndexRate:   p.slotBIndexRate = plain; break;
        case kParamOutputGain:       p.outputGainDb = plain; break;
        case kParamBypass:           p.bypass = (plain >= 0.5f); break;
        default: break;
    }

    dspInjector_.updateParameters(p);
    if (onParamChanged_) {
        onParamChanged_(paramId, normVal);
        if (paramId == kParamInjectDepth) {
            onParamChanged_(kParamDryWetMix, normVal);
        } else if (paramId == kParamDryWetMix) {
            onParamChanged_(kParamInjectDepth, normVal);
        }
    }
}

void PluginEditorView::updateThreeWayBalanceFromMouse(float x, float y) {
    const float triH = std::max(1.0f, triVertexA_.y - triVertexDry_.y);
    const double rawWet = std::clamp(static_cast<double>((y - triVertexDry_.y) / triH), 0.0, 1.0);

    const float maxHalfSpan = std::max(1.0f, (triVertexB_.x - triVertexA_.x) * 0.5f);
    const float curHalfSpan = std::max(4.0f, static_cast<float>(rawWet) * maxHalfSpan);
    double rawMorph = std::clamp(0.5 + static_cast<double>(x - triVertexDry_.x) / (2.0 * curHalfSpan), 0.0, 1.0);

    const auto slotA = modelEngine_.getSlotInfo(0);
    const auto slotB = modelEngine_.getSlotInfo(1);
    if (slotA.isLoaded && !slotB.isLoaded) {
        rawMorph = 0.0;
    } else if (!slotA.isLoaded && slotB.isLoaded) {
        rawMorph = 1.0;
    }

    applyNormalizedParameter(kParamDryWetMix, rawWet);
    applyNormalizedParameter(kParamAbMorph, rawMorph);
}

void PluginEditorView::handleMouseDown(float x, float y, bool doubleClick) {
    if (aboutPanelVisible_) {
        if (ptInRect(rectAboutClose_, x, y) || !ptInRect(rectAboutPanel_, x, y)) setAboutPanelVisible(false);
        return;
    }
    if (ptInRect(rectLogo_, x, y)) {
        setAboutPanelVisible(true);
        return;
    }
    const RvcVoiceParameters params = dspInjector_.getParameters();

    // Keep destructive new capture distinct from preserving the existing timeline.
    if (ptInRect(rectCaptureBtn_, x, y)) {
        const auto cur = dspInjector_.getTimelineState();
        const auto tel = dspInjector_.getVisualTelemetry();
        if (cur == TimelineState::kGenerating || (cur == TimelineState::kCapture && tel.isAppendCapture)) return;
        if (cur == TimelineState::kCapture) {
            dspInjector_.setTimelineState(TimelineState::kIdle);
        } else {
            dspInjector_.setTimelineState(TimelineState::kCapture);
        }
        return;
    }
    if (ptInRect(rectAppendCaptureBtn_, x, y)) {
        const auto tel = dspInjector_.getVisualTelemetry();
        const auto cur = static_cast<TimelineState>(tel.timelineStateVal);
        if (cur == TimelineState::kCapture && tel.isAppendCapture) {
            dspInjector_.setTimelineState(TimelineState::kIdle);
        } else if (cur != TimelineState::kGenerating && cur != TimelineState::kCapture &&
                   tel.captureStartSample >= 0 && tel.captureEndSample > tel.captureStartSample) {
            dspInjector_.beginAppendCapture();
        }
        return;
    }
    if (ptInRect(rectGenerateBtn_, x, y)) {
        dspInjector_.triggerGenerate();
        return;
    }
    if (ptInRect(rectActiveBtn_, x, y)) {
        const auto cur = dspInjector_.getTimelineState();
        if (cur == TimelineState::kCapture || cur == TimelineState::kGenerating) return;
        if (cur == TimelineState::kActive && !params.bypass) {
            dspInjector_.setTimelineState(TimelineState::kIdle);
        } else {
            if (params.bypass) {
                notifyGesture(kParamBypass, true);
                applyNormalizedParameter(kParamBypass, 0.0);
                notifyGesture(kParamBypass, false);
            }
            dspInjector_.setTimelineState(TimelineState::kActive);
        }
        return;
    }
    if (ptInRect(rectClearCaptureBtn_, x, y)) {
        dspInjector_.clearCapture();
        return;
    }

    // Slot A & Slot B buttons, Octave Shift buttons, Semitone Steppers, and FAISS index rate sliders
    static const int32_t kOctValues[3] = {-12, 0, +12};
    for (int s = 0; s < 2; ++s) {
        if (ptInRect(rectSlotLoadBtn_[s], x, y)) {
            openModelFileDialog(s);
            return;
        }
        if (ptInRect(rectSlotClearBtn_[s], x, y)) {
            modelEngine_.clearSlot(s);
            return;
        }
        for (int k = 0; k < 3; ++k) {
            if (ptInRect(rectSlotOctaveBtn_[s][k], x, y)) {
                modelEngine_.setSlotOctaveShift(s, kOctValues[k]);
                return;
            }
        }
        if (ptInRect(rectSlotSemiDownBtn_[s], x, y)) {
            const int32_t curSt = modelEngine_.getSlotInfo(s).targetOctaveSemitones;
            modelEngine_.setSlotOctaveShift(s, std::clamp(curSt - 1, -24, 24));
            return;
        }
        if (ptInRect(rectSlotSemiUpBtn_[s], x, y)) {
            const int32_t curSt = modelEngine_.getSlotInfo(s).targetOctaveSemitones;
            modelEngine_.setSlotOctaveShift(s, std::clamp(curSt + 1, -24, 24));
            return;
        }
        if (ptInRect(rectSlotIndexBar_[s], x, y)) {
            const uint32_t pid = (s == 0) ? kParamSlotAIndexRate : kParamSlotBIndexRate;
            if (doubleClick) {
                notifyGesture(pid, true);
                applyNormalizedParameter(pid, plainToNormalized(pid, 0.75f));
                notifyGesture(pid, false);
                activeDragParamId_ = -1;
                return;
            }
            activeDragParamId_ = static_cast<int>(pid);
            notifyGesture(pid, true);
            const double norm = (x - rectSlotIndexBar_[s].left) /
                                std::max(1.0f, rectSlotIndexBar_[s].right - rectSlotIndexBar_[s].left);
            applyNormalizedParameter(pid, norm);
            return;
        }
    }

    // Three-Way Balance Triangle Mixer Pad (bi-directionally linked with P.06 DRY / WET MIX)
    if (ptInRect(rectTrianglePad_, x, y)) {
        if (doubleClick) {
            notifyGesture(kParamDryWetMix, true);
            notifyGesture(kParamInjectDepth, true);
            notifyGesture(kParamAbMorph, true);
            applyNormalizedParameter(kParamDryWetMix, 0.89);
            applyNormalizedParameter(kParamAbMorph, 0.50);
            notifyGesture(kParamAbMorph, false);
            notifyGesture(kParamInjectDepth, false);
            notifyGesture(kParamDryWetMix, false);
            draggingTrianglePad_ = false;
            return;
        }
        draggingTrianglePad_ = true;
        notifyGesture(kParamDryWetMix, true);
        notifyGesture(kParamInjectDepth, true);
        notifyGesture(kParamAbMorph, true);
        updateThreeWayBalanceFromMouse(x, y);
        return;
    }

    // Bottom 6 Rotary Dials (P.01 .. P.06)
    static const uint32_t kDialParamIds[6] = {
        kParamPitchShift, kParamThroatFormant, kParamPresenceAir,
        kParamRmsMixRate, kParamDryWetMix, kParamOutputGain
    };
    static const float kDialDefaultPlains[6] = {0.0f, 0.0f, 0.0f, 0.25f, 1.00f, 0.0f};

    for (int i = 0; i < 6; ++i) {
        if (ptInRect(rectBottomDials_[i], x, y)) {
            const uint32_t pid = kDialParamIds[i];
            if (doubleClick) {
                notifyGesture(pid, true);
                applyNormalizedParameter(pid, plainToNormalized(pid, kDialDefaultPlains[i]));
                notifyGesture(pid, false);
                activeDragParamId_ = -1;
                return;
            }
            activeDragParamId_ = static_cast<int>(pid);
            notifyGesture(pid, true);
            dragStartMouseX_ = x;
            dragStartMouseY_ = y;

            float curPlain = 0.0f;
            switch (pid) {
                case kParamPitchShift:       curPlain = params.pitchShiftSemitones; break;
                case kParamThroatFormant:    curPlain = params.throatFormantSemitones; break;
                case kParamPresenceAir:      curPlain = params.presenceAirDb; break;
                case kParamRmsMixRate:       curPlain = params.rmsMixRate; break;
                case kParamDryWetMix:        curPlain = params.dryWetMix; break;
                case kParamOutputGain:       curPlain = params.outputGainDb; break;
                default: break;
            }
            dragStartNormVal_ = plainToNormalized(pid, curPlain);
            return;
        }
    }
}

void PluginEditorView::handleMouseMove(float x, float y) {
    logoHovered_ = !aboutPanelVisible_ && activeDragParamId_ < 0 && !draggingTrianglePad_ && ptInRect(rectLogo_, x, y);
    aboutCloseHovered_ = aboutPanelVisible_ && ptInRect(rectAboutClose_, x, y);
    if (aboutPanelVisible_ || logoHovered_) {
        hoveredParamId_ = -1;
        return;
    }
    if (draggingTrianglePad_) {
        updateThreeWayBalanceFromMouse(x, y);
        return;
    }

    if (activeDragParamId_ >= 0) {
        const uint32_t pid = static_cast<uint32_t>(activeDragParamId_);
        if (pid == kParamSlotAIndexRate || pid == kParamSlotBIndexRate) {
            const int s = (pid == kParamSlotAIndexRate) ? 0 : 1;
            const double norm = (x - rectSlotIndexBar_[s].left) /
                                std::max(1.0f, rectSlotIndexBar_[s].right - rectSlotIndexBar_[s].left);
            applyNormalizedParameter(pid, norm);
            return;
        }

        // Vertical drag for Rotary Encoders
        // P.01 Pitch Shift defaults to 1-semitone integer steps; hold Shift/Ctrl for 0.1 ST fine control
        const double deltaY = static_cast<double>(dragStartMouseY_ - y);
        const bool fineControl = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ||
                                 (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const double dragScale = fineControl ? 600.0 : 150.0;
        double newNorm = std::clamp(dragStartNormVal_ + deltaY / dragScale, 0.0, 1.0);
        if (pid == kParamPitchShift) {
            const float rawPlain = normalizedToPlain(pid, newNorm);
            const float snappedPlain = fineControl
                                           ? (std::round(rawPlain * 10.0f) * 0.1f)
                                           : std::round(rawPlain);
            newNorm = plainToNormalized(pid, snappedPlain);
        }
        applyNormalizedParameter(pid, newNorm);
        return;
    }

    hoveredParamId_ = -1;
    static const uint32_t kDialParamIds[6] = {
        kParamPitchShift, kParamThroatFormant, kParamPresenceAir,
        kParamRmsMixRate, kParamDryWetMix, kParamOutputGain
    };
    for (int i = 0; i < 6; ++i) {
        if (ptInRect(rectBottomDials_[i], x, y)) {
            hoveredParamId_ = static_cast<int>(kDialParamIds[i]);
            return;
        }
    }
    for (int s = 0; s < 2; ++s) {
        if (ptInRect(rectSlotIndexBar_[s], x, y)) {
            hoveredParamId_ = static_cast<int>((s == 0) ? kParamSlotAIndexRate : kParamSlotBIndexRate);
            return;
        }
    }
}

void PluginEditorView::handleMouseUp() {
    if (draggingTrianglePad_) {
        notifyGesture(kParamAbMorph, false);
        notifyGesture(kParamInjectDepth, false);
        notifyGesture(kParamDryWetMix, false);
        draggingTrianglePad_ = false;
    }
    if (activeDragParamId_ >= 0) {
        notifyGesture(static_cast<uint32_t>(activeDragParamId_), false);
        activeDragParamId_ = -1;
    }
}

bool PluginEditorView::onHostMouseWheel(float wheelDistance) {
    if (std::abs(wheelDistance) < 1e-4f) return false;
    float mx = 0.0f;
    float my = 0.0f;
    if (hwnd_) {
        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt)) {
            mx = static_cast<float>(pt.x);
            my = static_cast<float>(pt.y);
        }
    }
    int delta = static_cast<int>(std::round(wheelDistance * static_cast<float>(WHEEL_DELTA)));
    if (delta == 0) {
        delta = (wheelDistance > 0.0f) ? WHEEL_DELTA : -WHEEL_DELTA;
    }
    const bool handled = handleMouseWheel(mx, my, delta);
    if (handled && hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return handled;
}

bool PluginEditorView::handleMouseWheel(float x, float y, int wheelDelta) {
    if (aboutPanelVisible_) return true;
    int steps = wheelDelta / WHEEL_DELTA;
    if (steps == 0) {
        steps = (wheelDelta > 0) ? 1 : ((wheelDelta < 0) ? -1 : 0);
    }
    if (steps == 0) return false;

    static const uint32_t kDialParamIds[6] = {
        kParamPitchShift, kParamThroatFormant, kParamPresenceAir,
        kParamRmsMixRate, kParamDryWetMix, kParamOutputGain
    };

    int targetPid = -1;
    for (int i = 0; i < 6; ++i) {
        if (ptInRect(rectBottomDials_[i], x, y)) {
            targetPid = static_cast<int>(kDialParamIds[i]);
            break;
        }
    }
    if (targetPid < 0) {
        for (int s = 0; s < 2; ++s) {
            if (ptInRect(rectSlotIndexBar_[s], x, y)) {
                targetPid = static_cast<int>((s == 0) ? kParamSlotAIndexRate : kParamSlotBIndexRate);
                break;
            }
        }
    }
    if (targetPid < 0 && hoveredParamId_ >= 0) {
        targetPid = hoveredParamId_;
    }
    if (targetPid < 0) return false;

    const uint32_t pid = static_cast<uint32_t>(targetPid);
    const RvcVoiceParameters params = dspInjector_.getParameters();
    float nextPlain = 0.0f;

    switch (pid) {
        case kParamPitchShift:
            nextPlain = std::clamp(
                std::round(params.pitchShiftSemitones) + static_cast<float>(steps),
                -12.0f,
                12.0f);
            break;
        case kParamThroatFormant:
            nextPlain = std::clamp(
                std::round(params.throatFormantSemitones) + static_cast<float>(steps),
                -6.0f,
                6.0f);
            break;
        case kParamPresenceAir:
            nextPlain = std::clamp(
                std::round(params.presenceAirDb) + static_cast<float>(steps),
                -6.0f,
                6.0f);
            break;
        case kParamOutputGain:
            nextPlain = std::clamp(
                std::round(params.outputGainDb) + static_cast<float>(steps),
                -12.0f,
                12.0f);
            break;
        case kParamRmsMixRate: {
            const float nextPct = std::clamp(
                std::round(params.rmsMixRate * 100.0f) + static_cast<float>(steps),
                0.0f,
                100.0f);
            nextPlain = nextPct * 0.01f;
            break;
        }
        case kParamDryWetMix: {
            const float nextPct = std::clamp(
                std::round(params.dryWetMix * 100.0f) + static_cast<float>(steps),
                0.0f,
                100.0f);
            nextPlain = nextPct * 0.01f;
            break;
        }
        case kParamSlotAIndexRate: {
            const float nextPct = std::clamp(
                std::round(params.slotAIndexRate * 100.0f) + static_cast<float>(steps),
                0.0f,
                100.0f);
            nextPlain = nextPct * 0.01f;
            break;
        }
        case kParamSlotBIndexRate: {
            const float nextPct = std::clamp(
                std::round(params.slotBIndexRate * 100.0f) + static_cast<float>(steps),
                0.0f,
                100.0f);
            nextPlain = nextPct * 0.01f;
            break;
        }
        default:
            return false;
    }

    const double nextNorm = plainToNormalized(pid, nextPlain);
    notifyGesture(pid, true);
    applyNormalizedParameter(pid, nextNorm);
    notifyGesture(pid, false);
    return true;
}

void PluginEditorView::openModelFileDialog(int slotIndex) {
    wchar_t fileBuf[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"RVC Models & Indices (*.pth;*.index)\0*.pth;*.index\0"
        L"RVC PyTorch Checkpoints (*.pth)\0*.pth\0"
        L"RVC FAISS Feature Index (*.index)\0*.index\0"
        L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = (slotIndex == 0)
                         ? L"Select RVC .pth / .index Model for Slot A"
                         : L"Select RVC .pth / .index Model for Slot B";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        modelEngine_.loadModelIntoSlot(slotIndex, fileBuf);
    }
}

} // namespace rvc_vst
