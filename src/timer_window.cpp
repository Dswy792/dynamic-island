// ===========================================================================
//  src/timer_window.cpp  --  v5.7 built-in timer panel
//
//  A modeless, island-material popup under the island capsule with two
//  independent timers:
//    * stopwatch  (MM:SS.cc, start/pause/reset)
//    * countdown  (HH:MM:SS, presets 1/3/5/10/15/25/30 min, +/- 1 min,
//                  start/pause/reset, progress bar, "时间到" state)
//
//  Closing (x / Esc) only hides the window; both timers keep running and the
//  island feature icon shows the panel again. When the countdown reaches zero
//  the module posts kMsgTimerFinished to the owner (island toast). The panel is
//  a layered per-pixel-alpha window rendered with GDI+ exactly like the island
//  and the other popups, using RenderIslandMaterialRect for the material.
// ===========================================================================

#include "timer_window.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app_state.h"
#include "color_util.h"
#include "layout.h"
#include "rendering.h"
#include "theme.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace
{
// ---------------------------------------------------------------------------
// Geometry (device independent pixels at 96 DPI)
// ---------------------------------------------------------------------------
constexpr wchar_t kTimerWindowClass[] = L"DynamicIslandTimerWindow";

constexpr float kCardWidthDip = 384.0f;
constexpr float kCardHeightDip = 360.0f;
constexpr float kShadowDip = 16.0f;
constexpr float kCardRadiusDip = 16.0f;
constexpr float kContentMarginDip = 20.0f;

constexpr float kHeaderTitleTopDip = 14.0f;
constexpr float kCloseSizeDip = 28.0f;
constexpr float kCloseMarginDip = 14.0f;

constexpr float kTabsTopDip = 54.0f;
constexpr float kTabsWidthDip = 200.0f;
constexpr float kTabsHeightDip = 32.0f;
constexpr float kTabInsetDip = 2.0f;

constexpr float kTimeTopDip = 96.0f;
constexpr float kTimeHeightDip = 74.0f;
constexpr float kTimeFontDip = 50.0f;

constexpr float kStatusTopDip = 174.0f;
constexpr float kStatusHeightDip = 18.0f;

constexpr float kProgressTopDip = 198.0f;
constexpr float kProgressHeightDip = 6.0f;

constexpr float kPresetsTopDip = 214.0f;
constexpr float kChipHeightDip = 28.0f;
constexpr float kChipGapDip = 6.0f;

constexpr float kAdjustTopDip = 252.0f;
constexpr float kAdjustWidthDip = 92.0f;
constexpr float kAdjustHeightDip = 28.0f;
constexpr float kAdjustGapDip = 12.0f;

constexpr float kButtonsTopDip = 292.0f;
constexpr float kButtonHeightDip = 38.0f;
constexpr float kPrimaryWidthDip = 128.0f;
constexpr float kSecondaryWidthDip = 104.0f;
constexpr float kButtonGapDip = 12.0f;

constexpr UINT_PTR kStopwatchTimerId = 1;
constexpr UINT_PTR kCountdownTimerId = 2;
constexpr UINT kStopwatchIntervalMs = 33;
constexpr UINT kCountdownIntervalMs = 200;
constexpr ULONGLONG kFinishedFlashMs = 4000;

constexpr int kPresetMinutes[] = {1, 3, 5, 10, 15, 25, 30};
constexpr int kPresetCount =
    static_cast<int>(sizeof(kPresetMinutes) / sizeof(kPresetMinutes[0]));
constexpr int kDefaultMinutes = 5;
constexpr int kMinMinutes = 1;
constexpr int kMaxMinutes = 99;

int kPresetMs(int index)
{
    return kPresetMinutes[index] * 60 * 1000;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
int ClampInt(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

int RoundToInt(float value)
{
    return static_cast<int>(std::lround(value));
}

UINT QueryDpi(HWND hwnd)
{
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn function = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (function != nullptr && hwnd != nullptr)
    {
        const UINT dpi = function(hwnd);
        if (dpi != 0)
            return dpi;
    }
    UINT dpi = 96;
    HDC dc = GetDC(nullptr);
    if (dc != nullptr)
    {
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(nullptr, dc);
    }
    return dpi == 0 ? 96 : dpi;
}

RECT MakeRect(int left, int top, int width, int height)
{
    RECT rect = {left, top, left + width, top + height};
    return rect;
}

void AddRoundedRectPath(GraphicsPath& path, const RectF& rect, REAL radius)
{
    const REAL maximumRadius = std::min(rect.Width, rect.Height) * 0.5f;
    const REAL clamped = std::clamp(radius, 0.0f, maximumRadius);
    if (clamped <= 0.5f)
    {
        path.AddRectangle(rect);
        return;
    }
    const REAL diameter = clamped * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f,
                90.0f);
    path.AddArc(rect.X + rect.Width - diameter,
                rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f,
                90.0f);
    path.CloseFigure();
}

HWND g_owner = nullptr;

HWND OwnerWindow()
{
    return g_owner;
}

ULONGLONG NowMs()
{
    return GetTickCount64();
}

std::wstring FormatStopwatch(ULONGLONG milliseconds)
{
    const ULONGLONG totalSeconds = milliseconds / 1000ULL;
    const int centiseconds = static_cast<int>((milliseconds / 10ULL) % 100ULL);
    const int seconds = static_cast<int>(totalSeconds % 60ULL);
    const int minutes = static_cast<int>((totalSeconds / 60ULL) % 60ULL);
    const int hours = static_cast<int>(totalSeconds / 3600ULL);
    wchar_t buffer[32] = {};
    if (hours > 0)
        swprintf_s(buffer, L"%d:%02d:%02d.%02d", hours, minutes, seconds,
                   centiseconds);
    else
        swprintf_s(buffer, L"%02d:%02d.%02d", minutes, seconds, centiseconds);
    return buffer;
}

std::wstring FormatCountdown(ULONGLONG milliseconds)
{
    const ULONGLONG totalSeconds = (milliseconds + 999ULL) / 1000ULL;
    const int seconds = static_cast<int>(totalSeconds % 60ULL);
    const int minutes = static_cast<int>((totalSeconds / 60ULL) % 60ULL);
    const int hours = static_cast<int>(totalSeconds / 3600ULL);
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02d:%02d:%02d", hours, minutes, seconds);
    return buffer;
}

// ---------------------------------------------------------------------------
// Timer panel window
// ---------------------------------------------------------------------------
class TimerWindow
{
public:
    bool Create(HWND owner);
    void Destroy();
    void Show(HWND owner);
    void Hide();
    bool IsVisible() const { return m_visible; }
    HWND Handle() const { return m_hwnd; }

    void TestStopwatch();
    void TestCountdown();
    void TestCountdownIdle();
    void TestFinished();

private:
    enum class Tab
    {
        Stopwatch = 0,
        Countdown = 1,
    };

    enum ControlId
    {
        kNone = 0,
        kClose,
        kTabStopwatch,
        kTabCountdown,
        kPresetBase, // + index
        kMinusMinute,
        kPlusMinute,
        kPrimary,
        kSecondary,
    };

    struct StopwatchState
    {
        bool running = false;
        bool started = false;
        ULONGLONG accumulatedMs = 0;
        ULONGLONG startTick = 0;
    };

    struct CountdownState
    {
        bool running = false;
        bool started = false;
        bool finished = false;
        ULONGLONG durationMs = static_cast<ULONGLONG>(kDefaultMinutes) * 60000ULL;
        ULONGLONG remainingMs = static_cast<ULONGLONG>(kDefaultMinutes) * 60000ULL;
        ULONGLONG endTick = 0;
        ULONGLONG finishedTick = 0;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureClass();
    void ComputeMetrics();
    void PositionWindow();
    bool EnsureSurfaces();
    void ReleaseSurfaces();
    void RenderMaterial();
    void RenderFull();
    void PresentFull();
    void PresentRegion(const RECT& region);
    void UpdateTimeAndProgress();

    void DrawContent(Graphics& graphics);
    void DrawHeader(Graphics& graphics);
    void DrawTabs(Graphics& graphics);
    void DrawTime(Graphics& graphics);
    void DrawStatus(Graphics& graphics);
    void DrawProgress(Graphics& graphics);
    void DrawPresets(Graphics& graphics);
    void DrawAdjustRow(Graphics& graphics);
    void DrawButtons(Graphics& graphics);
    void DrawPill(Graphics& graphics, const RectF& rect, const std::wstring& text,
                  bool accent, bool hovered, bool pressed, bool enabled,
                  int fontDip);
    void DrawSymbolButton(Graphics& graphics, const RectF& rect,
                          const wchar_t* glyph, bool hovered, bool pressed);

    Font* GetFont(int pixelSize, bool bold);
    void ClearFonts();

    RECT CardRectPx() const;
    RectF CardRectF() const;
    RectF TimeRectF() const;
    RectF StatusRectF() const;
    RectF ProgressRectF() const;
    RectF PresetRectF(int index) const;
    RectF MinusRectF() const;
    RectF PlusRectF() const;
    RectF PrimaryRectF() const;
    RectF SecondaryRectF() const;
    RectF CloseRectF() const;
    RectF TabRectF(int tab) const;
    RECT TimeRegionPx() const;

    int HitTest(POINT point) const;
    void UpdateHover(POINT point);
    void OnMouseLeave();
    void OnLButtonDown(POINT point);
    void OnLButtonUp(POINT point);
    void ActivateControl(int id);

    // Timer state helpers.
    ULONGLONG StopwatchElapsedMs() const;
    void StartStopwatch();
    void PauseStopwatch();
    void ResetStopwatch();
    ULONGLONG CountdownRemainingMs() const;
    void StartCountdown();
    void PauseCountdown();
    void ResetCountdown();
    void CompleteCountdown();
    void SetCountdownDuration(ULONGLONG durationMs);
    void UpdateTimers();
    void OnStopwatchTick();
    void OnCountdownTick();
    bool IsCountdownEditable() const { return !m_countdown.running; }

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    int m_widthPx = 0;
    int m_heightPx = 0;

    Tab m_tab = Tab::Stopwatch;
    StopwatchState m_stopwatch;
    CountdownState m_countdown;

    int m_hovered = kNone;
    int m_pressed = kNone;
    bool m_trackingMouse = false;
    bool m_visible = false;
    bool m_stopwatchTimerRunning = false;
    bool m_countdownTimerRunning = false;

    Bitmap* m_material = nullptr;
    Bitmap* m_frame = nullptr;
    Graphics* m_graphics = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_previousDib = nullptr;
    void* m_dibPixels = nullptr;
    std::vector<Font*> m_fonts;

    FontFamily* m_fontFamily = nullptr;
};

bool TimerWindow::EnsureClass()
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &TimerWindow::WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kTimerWindowClass;
    registered = RegisterClassExW(&windowClass) != 0;
    if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        registered = true;
    return registered;
}

void TimerWindow::ComputeMetrics()
{
    m_dpi = m_owner != nullptr ? QueryDpi(m_owner) : 96;
    if (m_dpi == 0)
        m_dpi = 96;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    m_widthPx = std::max(80, RoundToInt((kCardWidthDip + kShadowDip * 2.0f) *
                                        m_scale));
    m_heightPx = std::max(80, RoundToInt((kCardHeightDip + kShadowDip * 2.0f) *
                                         m_scale));
}

RECT TimerWindow::CardRectPx() const
{
    return MakeRect(RoundToInt(kShadowDip * m_scale),
                    RoundToInt(kShadowDip * m_scale),
                    RoundToInt(kCardWidthDip * m_scale),
                    RoundToInt(kCardHeightDip * m_scale));
}

RectF TimerWindow::CardRectF() const
{
    const RECT card = CardRectPx();
    return RectF(static_cast<REAL>(card.left), static_cast<REAL>(card.top),
                 static_cast<REAL>(card.right - card.left),
                 static_cast<REAL>(card.bottom - card.top));
}

RectF TimerWindow::TimeRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X, card.Y + kTimeTopDip * m_scale, card.Width,
                 kTimeHeightDip * m_scale);
}

RectF TimerWindow::StatusRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X, card.Y + kStatusTopDip * m_scale, card.Width,
                 kStatusHeightDip * m_scale);
}

RectF TimerWindow::ProgressRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + kContentMarginDip * m_scale,
                 card.Y + kProgressTopDip * m_scale,
                 card.Width - kContentMarginDip * 2.0f * m_scale,
                 kProgressHeightDip * m_scale);
}

RectF TimerWindow::PresetRectF(int index) const
{
    const RectF card = CardRectF();
    const REAL chipWidth = (card.Width - kContentMarginDip * 2.0f * m_scale -
                            kChipGapDip * m_scale * (kPresetCount - 1)) /
                           static_cast<REAL>(kPresetCount);
    return RectF(card.X + kContentMarginDip * m_scale +
                     static_cast<REAL>(index) * (chipWidth + kChipGapDip * m_scale),
                 card.Y + kPresetsTopDip * m_scale, chipWidth,
                 kChipHeightDip * m_scale);
}

RectF TimerWindow::MinusRectF() const
{
    const RectF card = CardRectF();
    const REAL width = kAdjustWidthDip * m_scale;
    const REAL gap = kAdjustGapDip * m_scale;
    return RectF(card.X + card.Width * 0.5f - width - gap * 0.5f,
                 card.Y + kAdjustTopDip * m_scale, width,
                 kAdjustHeightDip * m_scale);
}

RectF TimerWindow::PlusRectF() const
{
    const RectF minus = MinusRectF();
    return RectF(minus.X + minus.Width + kAdjustGapDip * m_scale, minus.Y,
                 minus.Width, minus.Height);
}

RectF TimerWindow::PrimaryRectF() const
{
    const RectF card = CardRectF();
    const REAL height = kButtonHeightDip * m_scale;
    const REAL width = kPrimaryWidthDip * m_scale;
    const REAL secondaryWidth = kSecondaryWidthDip * m_scale;
    const REAL gap = kButtonGapDip * m_scale;
    const REAL total = width + gap + secondaryWidth;
    return RectF(card.X + card.Width * 0.5f - total * 0.5f,
                 card.Y + kButtonsTopDip * m_scale, width, height);
}

RectF TimerWindow::SecondaryRectF() const
{
    const RectF primary = PrimaryRectF();
    return RectF(primary.X + primary.Width + kButtonGapDip * m_scale, primary.Y,
                 kSecondaryWidthDip * m_scale, primary.Height);
}

RectF TimerWindow::CloseRectF() const
{
    const RectF card = CardRectF();
    const REAL size = kCloseSizeDip * m_scale;
    return RectF(card.X + card.Width - kCloseMarginDip * m_scale - size,
                 card.Y + kCloseMarginDip * m_scale, size, size);
}

RectF TimerWindow::TabRectF(int tab) const
{
    const RectF card = CardRectF();
    const REAL width = kTabsWidthDip * m_scale;
    const REAL height = kTabsHeightDip * m_scale;
    const REAL inset = kTabInsetDip * m_scale;
    const REAL segment = (width - inset * 2.0f) * 0.5f;
    return RectF(card.X + card.Width * 0.5f - width * 0.5f + inset +
                     static_cast<REAL>(tab) * segment,
                 card.Y + kTabsTopDip * m_scale + inset, segment,
                 height - inset * 2.0f);
}

RECT TimerWindow::TimeRegionPx() const
{
    const RectF time = TimeRectF();
    RECT region = {RoundToInt(time.X), RoundToInt(time.Y - 4.0f * m_scale),
                   RoundToInt(time.X + time.Width),
                   RoundToInt(time.Y + time.Height + 4.0f * m_scale)};
    return region;
}

// ---------------------------------------------------------------------------
// Surface management
// ---------------------------------------------------------------------------
bool TimerWindow::EnsureSurfaces()
{
    if (m_frame != nullptr)
        return true;

    m_material = new Bitmap(m_widthPx, m_heightPx, PixelFormat32bppARGB);
    m_frame = new Bitmap(m_widthPx, m_heightPx, PixelFormat32bppARGB);
    if (m_material->GetLastStatus() != Ok || m_frame->GetLastStatus() != Ok)
    {
        ReleaseSurfaces();
        return false;
    }
    m_graphics = new Graphics(m_frame);
    m_graphics->SetSmoothingMode(SmoothingModeAntiAlias);
    m_graphics->SetPixelOffsetMode(PixelOffsetModeHighQuality);
    m_graphics->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    m_graphics->SetCompositingQuality(CompositingQualityHighQuality);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = m_widthPx;
    info.bmiHeader.biHeight = -m_heightPx;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    m_memoryDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (m_memoryDc == nullptr)
        return false;
    m_dib = CreateDIBSection(m_memoryDc, &info, DIB_RGB_COLORS, &m_dibPixels,
                             nullptr, 0);
    if (m_dib == nullptr || m_dibPixels == nullptr)
        return false;
    m_previousDib = SelectObject(m_memoryDc, m_dib);
    return true;
}

void TimerWindow::ReleaseSurfaces()
{
    if (m_graphics != nullptr)
    {
        delete m_graphics;
        m_graphics = nullptr;
    }
    if (m_frame != nullptr)
    {
        delete m_frame;
        m_frame = nullptr;
    }
    if (m_material != nullptr)
    {
        delete m_material;
        m_material = nullptr;
    }
    if (m_previousDib != nullptr)
    {
        SelectObject(m_memoryDc, m_previousDib);
        m_previousDib = nullptr;
    }
    if (m_dib != nullptr)
    {
        DeleteObject(m_dib);
        m_dib = nullptr;
        m_dibPixels = nullptr;
    }
    if (m_memoryDc != nullptr)
    {
        DeleteDC(m_memoryDc);
        m_memoryDc = nullptr;
    }
}

void TimerWindow::ClearFonts()
{
    for (Font* font : m_fonts)
        delete font;
    m_fonts.clear();
}

Font* TimerWindow::GetFont(int pixelSize, bool bold)
{
    pixelSize = std::max(6, pixelSize);
    for (Font* font : m_fonts)
    {
        const bool fontBold = (font->GetStyle() & FontStyleBold) != 0;
        if (RoundToInt(font->GetSize()) == pixelSize && fontBold == bold)
            return font;
    }

    if (m_fontFamily == nullptr)
    {
        const wchar_t* const candidates[] = {L"Microsoft YaHei UI",
                                             L"Microsoft YaHei", L"Segoe UI",
                                             L"Arial"};
        for (const wchar_t* candidate : candidates)
        {
            FontFamily* family = new FontFamily(candidate);
            if (family->GetLastStatus() == Ok && family->IsAvailable())
            {
                m_fontFamily = family;
                break;
            }
            delete family;
        }
        if (m_fontFamily == nullptr)
            m_fontFamily = new FontFamily(L"Arial");
    }

    Font* font = new Font(m_fontFamily, static_cast<REAL>(pixelSize),
                          bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    if (font->GetLastStatus() != Ok)
    {
        delete font;
        font = new Font(L"Arial", static_cast<REAL>(pixelSize),
                        bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    }
    m_fonts.push_back(font);
    return font;
}

void TimerWindow::RenderMaterial()
{
    if (m_material == nullptr)
        return;
    Graphics materialGraphics(m_material);
    materialGraphics.SetSmoothingMode(SmoothingModeAntiAlias);
    materialGraphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    materialGraphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    materialGraphics.Clear(Color(0, 0, 0, 0));
    RenderIslandMaterialRect(materialGraphics, CardRectF(), kCardRadiusDip, m_dpi,
                             g_settings.opacity);
}

void TimerWindow::RenderFull()
{
    if (m_graphics == nullptr || m_material == nullptr)
        return;
    m_graphics->Clear(Color(0, 0, 0, 0));
    m_graphics->DrawImage(m_material, 0, 0, m_widthPx, m_heightPx);
    DrawContent(*m_graphics);
    PresentFull();
}

void TimerWindow::PresentFull()
{
    if (m_hwnd == nullptr || m_frame == nullptr || m_memoryDc == nullptr)
        return;

    BitmapData data = {};
    const Rect lockRect(0, 0, m_widthPx, m_heightPx);
    if (m_frame->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB,
                          &data) != Ok)
        return;

    const BYTE* source = static_cast<const BYTE*>(data.Scan0);
    BYTE* destination = static_cast<BYTE*>(m_dibPixels);
    const size_t sourceStride = static_cast<size_t>(data.Stride);
    const size_t rowBytes = static_cast<size_t>(m_widthPx) * 4;
    for (int y = 0; y < m_heightPx; ++y)
    {
        const BYTE* src = source + static_cast<size_t>(y) * sourceStride;
        BYTE* dst = destination + static_cast<size_t>(y) * rowBytes;
        for (int x = 0; x < m_widthPx; ++x)
        {
            const BYTE alpha = src[3];
            if (alpha == 255)
            {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            else if (alpha == 0)
            {
                dst[0] = 0;
                dst[1] = 0;
                dst[2] = 0;
            }
            else
            {
                dst[0] = static_cast<BYTE>(src[0] * alpha / 255);
                dst[1] = static_cast<BYTE>(src[1] * alpha / 255);
                dst[2] = static_cast<BYTE>(src[2] * alpha / 255);
            }
            dst[3] = alpha;
            src += 4;
            dst += 4;
        }
    }
    m_frame->UnlockBits(&data);

    RECT windowRect = {};
    GetWindowRect(m_hwnd, &windowRect);
    POINT destinationPoint = {windowRect.left, windowRect.top};
    SIZE size = {m_widthPx, m_heightPx};
    POINT sourcePoint = {0, 0};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(m_hwnd, nullptr, &destinationPoint, &size, m_memoryDc,
                        &sourcePoint, 0, &blend, ULW_ALPHA);
}

void TimerWindow::PresentRegion(const RECT& region)
{
    if (m_hwnd == nullptr || m_frame == nullptr || m_memoryDc == nullptr)
        return;
    RECT dirty = region;
    if (dirty.left < 0)
        dirty.left = 0;
    if (dirty.top < 0)
        dirty.top = 0;
    if (dirty.right > m_widthPx)
        dirty.right = m_widthPx;
    if (dirty.bottom > m_heightPx)
        dirty.bottom = m_heightPx;
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top)
        return;

    BitmapData data = {};
    const Rect lockRect(dirty.left, dirty.top, dirty.right - dirty.left,
                        dirty.bottom - dirty.top);
    if (m_frame->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB,
                          &data) != Ok)
        return;

    const BYTE* source = static_cast<const BYTE*>(data.Scan0);
    const size_t sourceStride = static_cast<size_t>(data.Stride);
    const size_t rowBytes = static_cast<size_t>(m_widthPx) * 4;
    const int regionWidth = dirty.right - dirty.left;
    for (int y = 0; y < dirty.bottom - dirty.top; ++y)
    {
        const BYTE* src = source + static_cast<size_t>(y) * sourceStride;
        BYTE* dst = static_cast<BYTE*>(m_dibPixels) +
                    static_cast<size_t>(dirty.top + y) * rowBytes +
                    static_cast<size_t>(dirty.left) * 4;
        for (int x = 0; x < regionWidth; ++x)
        {
            const BYTE alpha = src[3];
            if (alpha == 255)
            {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            else if (alpha == 0)
            {
                dst[0] = 0;
                dst[1] = 0;
                dst[2] = 0;
            }
            else
            {
                dst[0] = static_cast<BYTE>(src[0] * alpha / 255);
                dst[1] = static_cast<BYTE>(src[1] * alpha / 255);
                dst[2] = static_cast<BYTE>(src[2] * alpha / 255);
            }
            dst[3] = alpha;
            src += 4;
            dst += 4;
        }
    }
    m_frame->UnlockBits(&data);

    RECT windowRect = {};
    GetWindowRect(m_hwnd, &windowRect);
    POINT destinationPoint = {windowRect.left, windowRect.top};
    SIZE size = {m_widthPx, m_heightPx};
    POINT sourcePoint = {0, 0};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    RECT dirtyRect = dirty;
    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof(info);
    info.hdcDst = nullptr;
    info.pptDst = &destinationPoint;
    info.psize = &size;
    info.hdcSrc = m_memoryDc;
    info.pptSrc = &sourcePoint;
    info.crKey = 0;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &dirtyRect;
    UpdateLayeredWindowIndirect(m_hwnd, &info);
}

void TimerWindow::UpdateTimeAndProgress()
{
    if (m_graphics == nullptr || m_material == nullptr || m_hwnd == nullptr)
        return;

    RECT region = TimeRegionPx();
    if (m_tab == Tab::Countdown)
    {
        const RectF progress = ProgressRectF();
        const RECT progressRect = {RoundToInt(progress.X - 4.0f * m_scale),
                                   RoundToInt(progress.Y - 4.0f * m_scale),
                                   RoundToInt(progress.X + progress.Width +
                                              4.0f * m_scale),
                                   RoundToInt(progress.Y + progress.Height +
                                              4.0f * m_scale)};
        region.left = std::min(region.left, progressRect.left);
        region.top = std::min(region.top, progressRect.top);
        region.right = std::max(region.right, progressRect.right);
        region.bottom = std::max(region.bottom, progressRect.bottom);
    }

    const GraphicsState state = m_graphics->Save();
    m_graphics->SetClip(Gdiplus::Rect(region.left, region.top,
                                      region.right - region.left,
                                      region.bottom - region.top));
    m_graphics->DrawImage(m_material, 0, 0, m_widthPx, m_heightPx);
    m_graphics->Restore(state);

    DrawTime(*m_graphics);
    // The dirty rectangle spans time -> progress, so it also covers the status
    // line in between: repaint it as well or it would be erased.
    DrawStatus(*m_graphics);
    if (m_tab == Tab::Countdown)
        DrawProgress(*m_graphics);
    PresentRegion(region);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void TimerWindow::DrawPill(Graphics& graphics, const RectF& rect,
                           const std::wstring& text, bool accent, bool hovered,
                           bool pressed, bool enabled, int fontDip)
{
    const ThemePalette& palette = GetThemePalette();
    COLORREF fill = RGB(0, 0, 0);
    COLORREF textColor = palette.islandText;
    if (accent)
    {
        fill = palette.accent;
        textColor = palette.accentText;
        if (!enabled)
            fill = BlendColor(palette.accent, palette.islandBackground, 130);
        else if (pressed)
            fill = BlendColor(palette.accent, RGB(0, 0, 0), 32);
        else if (hovered)
            fill = BlendColor(palette.accent, RGB(255, 255, 255), 22);
    }
    else
    {
        const COLORREF tint = palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        int weight = palette.dark ? 24 : 12;
        if (!enabled)
            weight = palette.dark ? 12 : 6;
        else if (pressed)
            weight += 14;
        else if (hovered)
            weight += palette.dark ? 16 : 10;
        fill = BlendColor(palette.islandBackground, tint, weight);
        textColor = enabled ? palette.islandText
                            : BlendColor(palette.islandText,
                                         palette.islandBackground, 150);
    }

    SolidBrush brush(GdiColor(fill, enabled ? 255 : 190));
    GraphicsPath path;
    AddRoundedRectPath(path, rect, rect.Height * 0.5f);
    graphics.FillPath(&brush, &path);
    if (!accent)
    {
        Pen border(GdiColor(BlendColor(palette.islandText, palette.islandBackground,
                                       200),
                            enabled ? 255 : 120),
                   std::max(1.0f, m_scale));
        graphics.DrawPath(&border, &path);
    }

    Font* font = GetFont(std::max(6, RoundToInt(static_cast<float>(fontDip) *
                                                m_scale)),
                         true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush textBrush(GdiColor(textColor, enabled ? 255 : 190));
    graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font, rect,
                        &format, &textBrush);
}

void TimerWindow::DrawSymbolButton(Graphics& graphics, const RectF& rect,
                                   const wchar_t* glyph, bool hovered,
                                   bool pressed)
{
    const ThemePalette& palette = GetThemePalette();
    const bool active = hovered || pressed;
    if (active)
    {
        const COLORREF fill = pressed ? RGB(72, 72, 80) : RGB(56, 56, 64);
        SolidBrush brush(GdiColor(fill));
        graphics.FillEllipse(&brush, rect);
    }
    Font* font = GetFont(std::max(6, RoundToInt(15.0f * m_scale)), true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush text(GdiColor(active ? palette.islandText
                                    : BlendColor(palette.islandText,
                                                 palette.islandBackground, 80)));
    graphics.DrawString(glyph, -1, font, rect, &format, &text);
}

void TimerWindow::DrawHeader(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF card = CardRectF();
    Font* titleFont = GetFont(std::max(6, RoundToInt(15.0f * m_scale)), true);
    StringFormat titleFormat;
    titleFormat.SetAlignment(StringAlignmentNear);
    titleFormat.SetLineAlignment(StringAlignmentCenter);
    titleFormat.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush titleBrush(GdiColor(palette.islandText));
    const RectF titleRect(card.X + kContentMarginDip * m_scale,
                          card.Y + kHeaderTitleTopDip * m_scale,
                          card.Width - kContentMarginDip * 2.0f * m_scale,
                          24.0f * m_scale);
    graphics.DrawString(L"计时器", -1, titleFont, titleRect, &titleFormat,
                        &titleBrush);

    DrawSymbolButton(graphics, CloseRectF(), L"×", m_hovered == kClose,
                     m_pressed == kClose);
}

void TimerWindow::DrawTabs(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF card = CardRectF();
    const RectF container(card.X + card.Width * 0.5f -
                              kTabsWidthDip * m_scale * 0.5f,
                          card.Y + kTabsTopDip * m_scale,
                          kTabsWidthDip * m_scale, kTabsHeightDip * m_scale);
    SolidBrush recessed(GdiColor(
        palette.dark ? BlendColor(palette.islandBackground, RGB(255, 255, 255), 22)
                     : BlendColor(palette.islandBackground, RGB(0, 0, 0), 10)));
    GraphicsPath containerPath;
    AddRoundedRectPath(containerPath, container, container.Height * 0.5f);
    graphics.FillPath(&recessed, &containerPath);

    for (int tab = 0; tab < 2; ++tab)
    {
        const RectF rect = TabRectF(tab);
        const bool selected = static_cast<int>(m_tab) == tab;
        if (selected)
        {
            SolidBrush selectedBrush(GdiColor(palette.accent));
            GraphicsPath selectedPath;
            AddRoundedRectPath(selectedPath, rect, rect.Height * 0.5f);
            graphics.FillPath(&selectedBrush, &selectedPath);
        }
        Font* font = GetFont(std::max(6, RoundToInt(12.5f * m_scale)), selected);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush text(GdiColor(
            selected ? palette.accentText
                     : BlendColor(palette.islandText, palette.islandBackground,
                                  90)));
        const wchar_t* label = tab == 0 ? L"秒表" : L"倒计时";
        graphics.DrawString(label, -1, font, rect, &format, &text);
    }
}

void TimerWindow::DrawTime(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const bool finished = m_tab == Tab::Countdown && m_countdown.finished;
    COLORREF color = palette.islandText;
    if (finished)
    {
        const ULONGLONG elapsed = NowMs() - m_countdown.finishedTick;
        const REAL pulse =
            0.5f + 0.5f * std::sin(static_cast<REAL>(elapsed) * 0.012f);
        color = BlendColor(palette.accent, palette.islandText,
                           static_cast<int>(60.0f + 80.0f * pulse));
    }

    std::wstring text;
    if (m_tab == Tab::Stopwatch)
        text = FormatStopwatch(StopwatchElapsedMs());
    else
        text = FormatCountdown(CountdownRemainingMs());

    Font* font = GetFont(std::max(8, RoundToInt(kTimeFontDip * m_scale)), true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush shadow(GdiColor(RGB(0, 0, 0), 90));
    const RectF rect = TimeRectF();
    const REAL offset = std::max(1.0f, 1.5f * m_scale);
    graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font,
                        RectF(rect.X + offset, rect.Y + offset, rect.Width,
                              rect.Height),
                        &format, &shadow);
    SolidBrush brush(GdiColor(color));
    graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font, rect,
                        &format, &brush);
}

void TimerWindow::DrawStatus(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const COLORREF secondary =
        BlendColor(palette.islandText, palette.islandBackground, 110);
    std::wstring status;
    COLORREF color = secondary;
    if (m_tab == Tab::Stopwatch)
    {
        if (m_stopwatch.running)
            status = L"秒表计时中";
        else if (m_stopwatch.started)
            status = L"已暂停 · 点击继续";
        else
            status = L"点击开始计时";
    }
    else
    {
        if (m_countdown.finished)
        {
            status = L"时间到";
            color = palette.accent;
        }
        else if (m_countdown.running)
            status = L"倒计时进行中";
        else if (m_countdown.started)
            status = L"已暂停 · 点击继续或重置";
        else
            status = L"选择时长后点击开始";
    }

    Font* font = GetFont(std::max(6, RoundToInt(11.0f * m_scale)), false);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(GdiColor(color));
    graphics.DrawString(status.c_str(), static_cast<INT>(status.length()), font,
                        StatusRectF(), &format, &brush);
}

void TimerWindow::DrawProgress(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF rect = ProgressRectF();
    const REAL radius = rect.Height * 0.5f;
    SolidBrush track(GdiColor(palette.islandText, 40));
    GraphicsPath trackPath;
    AddRoundedRectPath(trackPath, rect, radius);
    graphics.FillPath(&track, &trackPath);

    const ULONGLONG duration = m_countdown.durationMs > 0
                                   ? m_countdown.durationMs
                                   : 1ULL;
    const ULONGLONG remaining = CountdownRemainingMs();
    const float fraction = static_cast<float>(
        std::min<ULONGLONG>(remaining, duration) * 1000ULL / duration) /
                           1000.0f;
    if (fraction > 0.0f)
    {
        RectF fillRect = rect;
        fillRect.Width = rect.Width * fraction;
        if (fillRect.Width >= 1.0f)
        {
            SolidBrush fill(GdiColor(palette.accent, 220));
            GraphicsPath fillPath;
            AddRoundedRectPath(fillPath, fillRect, radius);
            graphics.FillPath(&fill, &fillPath);
        }
    }
}

void TimerWindow::DrawPresets(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const bool enabled = IsCountdownEditable();
    const int activeIndex = [&]() {
        const int minutes = static_cast<int>(m_countdown.durationMs / 60000ULL);
        for (int i = 0; i < kPresetCount; ++i)
        {
            if (kPresetMinutes[i] == minutes)
                return i;
        }
        return -1;
    }();
    for (int i = 0; i < kPresetCount; ++i)
    {
        const RectF rect = PresetRectF(i);
        const bool hovered = m_hovered == kPresetBase + i;
        const bool pressed = m_pressed == kPresetBase + i;
        const bool selected = i == activeIndex;
        COLORREF fill;
        if (selected)
        {
            fill = BlendColor(palette.islandBackground, palette.accent,
                              enabled ? 150 : 80);
        }
        else
        {
            const COLORREF tint = palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
            int weight = palette.dark ? 22 : 10;
            if (!enabled)
                weight = palette.dark ? 12 : 6;
            else if (pressed)
                weight += 14;
            else if (hovered)
                weight += palette.dark ? 16 : 10;
            fill = BlendColor(palette.islandBackground, tint, weight);
        }
        SolidBrush brush(GdiColor(fill, enabled ? 255 : 190));
        GraphicsPath path;
        AddRoundedRectPath(path, rect, rect.Height * 0.5f);
        graphics.FillPath(&brush, &path);
        Pen border(GdiColor(selected ? palette.accent
                                     : BlendColor(palette.islandText,
                                                  palette.islandBackground, 200),
                            enabled ? (selected ? 220 : 160) : 100),
                   std::max(1.0f, m_scale));
        graphics.DrawPath(&border, &path);

        Font* font = GetFont(std::max(6, RoundToInt(10.5f * m_scale)), selected);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush text(GdiColor(
            selected ? palette.islandText
                     : (enabled
                            ? BlendColor(palette.islandText,
                                         palette.islandBackground, 45)
                            : BlendColor(palette.islandText,
                                         palette.islandBackground, 140))));
        wchar_t label[16] = {};
        swprintf_s(label, L"%d 分", kPresetMinutes[i]);
        graphics.DrawString(label, -1, font, rect, &format, &text);
    }
}

void TimerWindow::DrawAdjustRow(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const bool enabled = IsCountdownEditable();
    const RectF minus = MinusRectF();
    const RectF plus = PlusRectF();
    DrawPill(graphics, minus, L"- 1 分钟", false, m_hovered == kMinusMinute,
             m_pressed == kMinusMinute, enabled, 11);
    DrawPill(graphics, plus, L"+ 1 分钟", false, m_hovered == kPlusMinute,
             m_pressed == kPlusMinute, enabled, 11);
    (void)palette;
}

void TimerWindow::DrawButtons(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF primary = PrimaryRectF();
    const RectF secondary = SecondaryRectF();
    std::wstring primaryLabel;
    if (m_tab == Tab::Stopwatch)
    {
        if (m_stopwatch.running)
            primaryLabel = L"暂停";
        else if (m_stopwatch.started)
            primaryLabel = L"继续";
        else
            primaryLabel = L"开始";
    }
    else
    {
        if (m_countdown.finished)
            primaryLabel = L"再来一次";
        else if (m_countdown.running)
            primaryLabel = L"暂停";
        else if (m_countdown.started)
            primaryLabel = L"继续";
        else
            primaryLabel = L"开始";
    }
    const std::wstring secondaryLabel =
        m_tab == Tab::Stopwatch ? L"清零" : L"重置";
    DrawPill(graphics, primary, primaryLabel, true, m_hovered == kPrimary,
             m_pressed == kPrimary, true, 13);
    DrawPill(graphics, secondary, secondaryLabel, false,
             m_hovered == kSecondary, m_pressed == kSecondary, true, 13);
    (void)palette;
}

void TimerWindow::DrawContent(Graphics& graphics)
{
    DrawHeader(graphics);
    DrawTabs(graphics);
    DrawTime(graphics);
    DrawStatus(graphics);
    if (m_tab == Tab::Countdown)
    {
        DrawProgress(graphics);
        DrawPresets(graphics);
        DrawAdjustRow(graphics);
    }
    DrawButtons(graphics);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
int TimerWindow::HitTest(POINT point) const
{
    const auto inside = [&point](const RectF& rect) {
        return static_cast<float>(point.x) >= rect.X &&
               static_cast<float>(point.x) < rect.X + rect.Width &&
               static_cast<float>(point.y) >= rect.Y &&
               static_cast<float>(point.y) < rect.Y + rect.Height;
    };

    if (inside(CloseRectF()))
        return kClose;
    for (int tab = 0; tab < 2; ++tab)
    {
        if (inside(TabRectF(tab)))
            return tab == 0 ? kTabStopwatch : kTabCountdown;
    }
    if (m_tab == Tab::Countdown)
    {
        for (int i = 0; i < kPresetCount; ++i)
        {
            if (inside(PresetRectF(i)))
                return kPresetBase + i;
        }
        if (inside(MinusRectF()))
            return kMinusMinute;
        if (inside(PlusRectF()))
            return kPlusMinute;
    }
    if (inside(PrimaryRectF()))
        return kPrimary;
    if (inside(SecondaryRectF()))
        return kSecondary;
    return kNone;
}

void TimerWindow::UpdateHover(POINT point)
{
    const int hit = HitTest(point);
    if (hit == m_hovered)
        return;
    m_hovered = hit;
    RenderFull();
}

void TimerWindow::OnMouseLeave()
{
    m_trackingMouse = false;
    if (m_hovered == kNone)
        return;
    m_hovered = kNone;
    RenderFull();
}

void TimerWindow::OnLButtonDown(POINT point)
{
    const int hit = HitTest(point);
    if (hit == kNone)
    {
        m_pressed = kNone;
        return;
    }
    m_pressed = hit;
    SetCapture(m_hwnd);
    RenderFull();
}

void TimerWindow::OnLButtonUp(POINT point)
{
    const int pressed = m_pressed;
    m_pressed = kNone;
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    if (pressed != kNone && HitTest(point) == pressed)
        ActivateControl(pressed);
    else
        RenderFull();
}

void TimerWindow::ActivateControl(int id)
{
    switch (id)
    {
    case kClose:
        Hide();
        return;
    case kTabStopwatch:
    case kTabCountdown:
        m_tab = id == kTabStopwatch ? Tab::Stopwatch : Tab::Countdown;
        RenderFull();
        UpdateTimers();
        return;
    case kPrimary:
        if (m_tab == Tab::Stopwatch)
        {
            if (m_stopwatch.running)
                PauseStopwatch();
            else
                StartStopwatch();
        }
        else
        {
            if (m_countdown.finished)
            {
                m_countdown.finished = false;
                m_countdown.remainingMs = m_countdown.durationMs;
                StartCountdown();
            }
            else if (m_countdown.running)
            {
                PauseCountdown();
            }
            else
            {
                StartCountdown();
            }
        }
        return;
    case kSecondary:
        if (m_tab == Tab::Stopwatch)
            ResetStopwatch();
        else
            ResetCountdown();
        return;
    case kMinusMinute:
        if (IsCountdownEditable())
        {
            const int minutes = static_cast<int>(m_countdown.durationMs / 60000ULL);
            SetCountdownDuration(static_cast<ULONGLONG>(
                ClampInt(minutes - 1, kMinMinutes, kMaxMinutes)) *
                                 60000ULL);
        }
        return;
    case kPlusMinute:
        if (IsCountdownEditable())
        {
            const int minutes = static_cast<int>(m_countdown.durationMs / 60000ULL);
            SetCountdownDuration(static_cast<ULONGLONG>(
                ClampInt(minutes + 1, kMinMinutes, kMaxMinutes)) *
                                 60000ULL);
        }
        return;
    default:
        break;
    }

    if (id >= kPresetBase && id < kPresetBase + kPresetCount &&
        IsCountdownEditable())
    {
        const int index = id - kPresetBase;
        SetCountdownDuration(static_cast<ULONGLONG>(kPresetMs(index)));
    }
}

// ---------------------------------------------------------------------------
// Timer state
// ---------------------------------------------------------------------------
ULONGLONG TimerWindow::StopwatchElapsedMs() const
{
    if (!m_stopwatch.running)
        return m_stopwatch.accumulatedMs;
    return m_stopwatch.accumulatedMs + (NowMs() - m_stopwatch.startTick);
}

void TimerWindow::StartStopwatch()
{
    if (m_stopwatch.running)
        return;
    m_stopwatch.running = true;
    m_stopwatch.started = true;
    m_stopwatch.startTick = NowMs();
    RenderFull();
    UpdateTimers();
}

void TimerWindow::PauseStopwatch()
{
    if (!m_stopwatch.running)
        return;
    m_stopwatch.accumulatedMs = StopwatchElapsedMs();
    m_stopwatch.running = false;
    RenderFull();
    UpdateTimers();
}

void TimerWindow::ResetStopwatch()
{
    m_stopwatch.running = false;
    m_stopwatch.started = false;
    m_stopwatch.accumulatedMs = 0;
    RenderFull();
    UpdateTimers();
}

ULONGLONG TimerWindow::CountdownRemainingMs() const
{
    if (!m_countdown.running)
        return m_countdown.remainingMs;
    const ULONGLONG now = NowMs();
    if (now >= m_countdown.endTick)
        return 0;
    return m_countdown.endTick - now;
}

void TimerWindow::StartCountdown()
{
    if (m_countdown.running || m_countdown.durationMs == 0)
        return;
    if (m_countdown.remainingMs == 0)
        m_countdown.remainingMs = m_countdown.durationMs;
    m_countdown.finished = false;
    m_countdown.started = true;
    m_countdown.running = true;
    m_countdown.endTick = NowMs() + m_countdown.remainingMs;
    RenderFull();
    UpdateTimers();
}

void TimerWindow::PauseCountdown()
{
    if (!m_countdown.running)
        return;
    m_countdown.remainingMs = CountdownRemainingMs();
    m_countdown.running = false;
    RenderFull();
    UpdateTimers();
}

void TimerWindow::ResetCountdown()
{
    m_countdown.running = false;
    m_countdown.started = false;
    m_countdown.finished = false;
    m_countdown.remainingMs = m_countdown.durationMs;
    RenderFull();
    UpdateTimers();
}

void TimerWindow::SetCountdownDuration(ULONGLONG durationMs)
{
    m_countdown.durationMs = durationMs;
    m_countdown.remainingMs = durationMs;
    m_countdown.running = false;
    m_countdown.started = false;
    m_countdown.finished = false;
    RenderFull();
    UpdateTimers();
}

void TimerWindow::CompleteCountdown()
{
    if (m_countdown.finished)
        return;
    m_countdown.running = false;
    m_countdown.finished = true;
    m_countdown.remainingMs = 0;
    m_countdown.finishedTick = NowMs();
    HWND owner = OwnerWindow();
    if (owner != nullptr)
        PostMessageW(owner, kMsgTimerFinished, 0, 0);
    RenderFull();
    UpdateTimers();
}

void TimerWindow::UpdateTimers()
{
    const bool needStopwatch = m_visible && m_stopwatch.running;
    const bool needCountdown =
        m_countdown.running ||
        (m_visible && m_countdown.finished &&
         NowMs() - m_countdown.finishedTick < kFinishedFlashMs);

    if (needStopwatch && !m_stopwatchTimerRunning)
    {
        SetTimer(m_hwnd, kStopwatchTimerId, kStopwatchIntervalMs, nullptr);
        m_stopwatchTimerRunning = true;
    }
    else if (!needStopwatch && m_stopwatchTimerRunning)
    {
        KillTimer(m_hwnd, kStopwatchTimerId);
        m_stopwatchTimerRunning = false;
    }

    if (needCountdown && !m_countdownTimerRunning)
    {
        SetTimer(m_hwnd, kCountdownTimerId, kCountdownIntervalMs, nullptr);
        m_countdownTimerRunning = true;
    }
    else if (!needCountdown && m_countdownTimerRunning)
    {
        KillTimer(m_hwnd, kCountdownTimerId);
        m_countdownTimerRunning = false;
    }
}

void TimerWindow::OnStopwatchTick()
{
    if (!m_stopwatch.running || !m_visible)
        return;
    UpdateTimeAndProgress();
}

void TimerWindow::OnCountdownTick()
{
    if (m_countdown.running)
    {
        if (NowMs() >= m_countdown.endTick)
        {
            CompleteCountdown();
            return;
        }
        if (m_visible)
            UpdateTimeAndProgress();
        return;
    }
    if (m_visible && m_countdown.finished &&
        NowMs() - m_countdown.finishedTick < kFinishedFlashMs)
    {
        UpdateTimeAndProgress();
    }
    else
    {
        UpdateTimers();
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK TimerWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam)
{
    TimerWindow* self = reinterpret_cast<TimerWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TimerWindow*>(create->lpCreateParams);
        if (self != nullptr)
        {
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    if (message == WM_NCDESTROY)
    {
        const LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return result;
    }
    if (self == nullptr || self->m_hwnd == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT TimerWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
        ValidateRect(m_hwnd, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_TIMER:
        if (wParam == kStopwatchTimerId)
            OnStopwatchTick();
        else if (wParam == kCountdownTimerId)
            OnCountdownTick();
        return 0;

    case WM_MOUSEMOVE:
    {
        if (!m_trackingMouse)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = m_hwnd;
            TrackMouseEvent(&track);
            m_trackingMouse = true;
        }
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        UpdateHover(point);
        return 0;
    }

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        OnLButtonDown(point);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        OnLButtonUp(point);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (m_pressed != kNone)
        {
            m_pressed = kNone;
            RenderFull();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            Hide();
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(m_hwnd, &point);
            SetCursor(LoadCursorW(nullptr, HitTest(point) != kNone ? IDC_HAND
                                                                   : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        ComputeMetrics();
        ReleaseSurfaces();
        EnsureSurfaces();
        RenderMaterial();
        PositionWindow();
        RenderFull();
        return 0;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        RefreshSystemThemeCache();
        RenderMaterial();
        RenderFull();
        return 0;

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        KillTimer(m_hwnd, kStopwatchTimerId);
        KillTimer(m_hwnd, kCountdownTimerId);
        m_stopwatchTimerRunning = false;
        m_countdownTimerRunning = false;
        ReleaseSurfaces();
        ClearFonts();
        if (m_fontFamily != nullptr)
        {
            delete m_fontFamily;
            m_fontFamily = nullptr;
        }
        m_hwnd = nullptr;
        m_visible = false;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Create / show / hide
// ---------------------------------------------------------------------------
void TimerWindow::PositionWindow()
{
    if (m_hwnd == nullptr)
        return;
    const RECT capsule = GetIslandRestingCapsuleRect();
    const int gap = RoundToInt(10.0f * m_scale);
    const int shadow = RoundToInt(kShadowDip * m_scale);
    int x = capsule.left + (capsule.right - capsule.left) / 2 - m_widthPx / 2;
    int y = capsule.bottom + gap - shadow;

    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetIslandWorkArea();
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info))
        workArea = info.rcWork;

    if (x + m_widthPx > workArea.right)
        x = workArea.right - m_widthPx;
    if (x < workArea.left)
        x = workArea.left;
    if (y + m_heightPx > workArea.bottom)
        y = capsule.top - m_heightPx - gap + shadow;
    if (y < workArea.top)
        y = workArea.top;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

bool TimerWindow::Create(HWND owner)
{
    if (m_hwnd != nullptr)
        return true;
    if (!EnsureClass())
        return false;
    m_owner = owner;
    ComputeMetrics();

    const RECT capsule = GetIslandRestingCapsuleRect();
    const int gap = RoundToInt(10.0f * m_scale);
    const int shadow = RoundToInt(kShadowDip * m_scale);
    int x = capsule.left + (capsule.right - capsule.left) / 2 - m_widthPx / 2;
    int y = capsule.bottom + gap - shadow;
    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetIslandWorkArea();
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info))
        workArea = info.rcWork;
    if (x + m_widthPx > workArea.right)
        x = workArea.right - m_widthPx;
    if (x < workArea.left)
        x = workArea.left;
    if (y + m_heightPx > workArea.bottom)
        y = capsule.top - m_heightPx - gap + shadow;
    if (y < workArea.top)
        y = workArea.top;

    const HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTimerWindowClass,
        L"计时器", WS_POPUP, x, y, m_widthPx, m_heightPx, owner, nullptr,
        GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr)
    {
        m_hwnd = nullptr;
        return false;
    }
    if (!EnsureSurfaces())
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        return false;
    }
    RenderMaterial();
    RenderFull();
    return true;
}

void TimerWindow::Show(HWND owner)
{
    if (!Create(owner))
        return;
    m_visible = true;
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    UpdateTimers();
    RenderFull();
}

void TimerWindow::Hide()
{
    if (m_hwnd == nullptr)
        return;
    m_visible = false;
    ShowWindow(m_hwnd, SW_HIDE);
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    UpdateTimers();
}

void TimerWindow::Destroy()
{
    if (m_hwnd != nullptr)
    {
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwnd);
    }
}

void TimerWindow::TestStopwatch()
{
    Show(OwnerWindow());
    m_tab = Tab::Stopwatch;
    m_stopwatch.started = true;
    m_stopwatch.running = true;
    m_stopwatch.accumulatedMs = 42370;
    m_stopwatch.startTick = NowMs();
    UpdateTimers();
    RenderFull();
}

void TimerWindow::TestCountdown()
{
    Show(OwnerWindow());
    m_tab = Tab::Countdown;
    m_countdown.durationMs = 300000;
    m_countdown.remainingMs = 300000;
    m_countdown.started = true;
    m_countdown.finished = false;
    m_countdown.running = true;
    m_countdown.endTick = NowMs() + 300000;
    UpdateTimers();
    RenderFull();
}

void TimerWindow::TestCountdownIdle()
{
    Show(OwnerWindow());
    m_tab = Tab::Countdown;
    m_countdown.durationMs = 300000;
    m_countdown.remainingMs = 300000;
    m_countdown.running = false;
    m_countdown.started = false;
    m_countdown.finished = false;
    UpdateTimers();
    RenderFull();
}

void TimerWindow::TestFinished()
{
    Show(OwnerWindow());
    m_tab = Tab::Countdown;
    m_countdown.durationMs = 300000;
    m_countdown.remainingMs = 0;
    m_countdown.running = false;
    m_countdown.started = true;
    m_countdown.finished = true;
    m_countdown.finishedTick = NowMs();
    UpdateTimers();
    RenderFull();
    HWND owner = OwnerWindow();
    if (owner != nullptr)
        PostMessageW(owner, kMsgTimerFinished, 0, 0);
}

TimerWindow& Instance()
{
    static TimerWindow* instance = new TimerWindow();
    return *instance;
}
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InitTimerWindow(HWND ownerHwnd)
{
    g_owner = ownerHwnd;
    LPCWSTR commandLine = GetCommandLineW();
    if (commandLine == nullptr)
        return;

    if (wcsstr(commandLine, L"--test-timer-finished") != nullptr)
    {
        Instance().TestFinished();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-stopwatch") != nullptr)
    {
        Instance().TestStopwatch();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-countdown-idle") != nullptr)
    {
        Instance().TestCountdownIdle();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-countdown") != nullptr)
    {
        Instance().TestCountdown();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer") != nullptr)
    {
        ShowTimerWindow(ownerHwnd);
    }
}

void ShutdownTimerWindow()
{
    Instance().Destroy();
    g_owner = nullptr;
}

void ShowTimerWindow(HWND ownerHwnd)
{
    if (ownerHwnd != nullptr)
        g_owner = ownerHwnd;
    Instance().Show(OwnerWindow());
}

void HideTimerWindow()
{
    Instance().Hide();
}

bool IsTimerWindowOpen()
{
    return Instance().IsVisible();
}
