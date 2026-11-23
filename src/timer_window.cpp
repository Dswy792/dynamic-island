// ===========================================================================
//  src/timer_window.cpp  --  v5.8 built-in timer feature
//
//  * Timer panel: a modeless island-material popup under the island capsule.
//      - Stopwatch tab: MM:SS.cc (H:MM:SS.cc over one hour), start/pause/reset.
//      - Countdown tab: HH:MM:SS display with three independent vertical wheels
//        时 (0-99) / 分 (0-59) / 秒 (0-59), start/pause/reset and progress bar.
//        Wheels are editable only while the countdown is stopped.
//      - Starting a timer auto-collapses the panel.
//  * Mini timer sub-islands: when the panel is hidden and a timer is running,
//    compact material pills under the island show the live time (stopwatch
//    and/or countdown). Clicking a pill opens the panel on that tab.
//
//  Closing (x / Esc) only hides the panel; timers keep running. The countdown
//  posts kMsgTimerFinished to the owner when it reaches zero.
// ===========================================================================

#include "timer_window.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "animation.h"
#include "app_state.h"
#include "color_util.h"
#include "layout.h"
#include "rendering.h"
#include "screensaver.h"
#include "theme.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// Defined in main.cpp: true while the island toast band is on screen. Circular
// timer minis are pushed below that band while a toast is visible.
extern bool IsIslandToastVisible();

namespace
{
// ---------------------------------------------------------------------------
// Geometry (device independent pixels at 96 DPI)
// ---------------------------------------------------------------------------
constexpr wchar_t kTimerWindowClass[] = L"DynamicIslandTimerWindow";
constexpr wchar_t kMiniWindowClass[] = L"DynamicIslandTimerMini";

constexpr float kCardWidthDip = 336.0f;
constexpr float kCardHeightDip = 352.0f;
constexpr float kShadowDip = 16.0f;
constexpr float kCardRadiusDip = 14.0f;
constexpr float kContentMarginDip = 16.0f;

constexpr float kHeaderTitleTopDip = 11.0f;
constexpr float kCloseSizeDip = 24.0f;
constexpr float kCloseMarginDip = 12.0f;

constexpr float kTabsTopDip = 46.0f;
constexpr float kTabsWidthDip = 184.0f;
constexpr float kTabsHeightDip = 30.0f;
constexpr float kTabInsetDip = 2.0f;

constexpr float kTimeTopDip = 84.0f;
constexpr float kTimeHeightDip = 64.0f;
constexpr float kTimeFontDip = 44.0f;

constexpr float kStatusTopDip = 150.0f;
constexpr float kStatusHeightDip = 16.0f;

constexpr float kProgressTopDip = 172.0f;
constexpr float kProgressHeightDip = 5.0f;

constexpr float kWheelsLabelTopDip = 184.0f;
constexpr float kWheelsLabelHeightDip = 14.0f;
constexpr float kWheelsTopDip = 200.0f;
constexpr float kWheelColumnWidthDip = 88.0f;
constexpr float kWheelColumnGapDip = 12.0f;
constexpr float kWheelRowHeightDip = 20.0f;
constexpr int kWheelVisibleRows = 5;
constexpr float kWheelHeightDip = kWheelRowHeightDip * kWheelVisibleRows;

constexpr float kButtonsTopDip = 306.0f;
constexpr float kButtonHeightDip = 34.0f;
constexpr float kPrimaryWidthDip = 116.0f;
constexpr float kSecondaryWidthDip = 92.0f;
constexpr float kButtonGapDip = 10.0f;

constexpr int kWheelRanges[3] = {100, 60, 60}; // 时 / 分 / 秒

// Circular mini sub-islands, to the right of the main capsule.
constexpr float kMiniDiameterDip = 48.0f;
constexpr float kMiniPadDip = 12.0f;
constexpr float kMiniGapDip = 8.0f;
constexpr float kMiniRightGapDip = 10.0f;

constexpr UINT_PTR kStopwatchTimerId = 1;
constexpr UINT_PTR kCountdownTimerId = 2;
constexpr UINT_PTR kMiniPollTimerId = 3;
constexpr UINT_PTR kWheelAnimTimerId = 4;
constexpr UINT kStopwatchIntervalMs = 33;
constexpr UINT kCountdownIntervalMs = 200;
constexpr UINT kMiniPollIntervalMs = 250;
constexpr UINT kWheelAnimIntervalMs = 16;
constexpr ULONGLONG kFinishedFlashMs = 4000;

constexpr int kDefaultHours = 0;
constexpr int kDefaultMinutes = 5;
constexpr int kDefaultSeconds = 0;

int RoundToInt(float value)
{
    return static_cast<int>(std::lround(value));
}

int ClampInt(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

int WrapValue(int value, int range)
{
    if (range <= 0)
        return 0;
    int result = value % range;
    if (result < 0)
        result += range;
    return result;
}

ULONGLONG NowMs()
{
    return GetTickCount64();
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

std::wstring FormatStopwatchPrecise(ULONGLONG milliseconds)
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

std::wstring FormatCountdownClock(ULONGLONG milliseconds)
{
    const ULONGLONG totalSeconds = (milliseconds + 999ULL) / 1000ULL;
    const int seconds = static_cast<int>(totalSeconds % 60ULL);
    const int minutes = static_cast<int>((totalSeconds / 60ULL) % 60ULL);
    const int hours = static_cast<int>(totalSeconds / 3600ULL);
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02d:%02d:%02d", hours, minutes, seconds);
    return buffer;
}

// Very compact form for the 48 DIP circular mini islands.
std::wstring FormatMiniTime(ULONGLONG milliseconds, bool roundUp)
{
    ULONGLONG totalSeconds = milliseconds / 1000ULL;
    if (roundUp && milliseconds % 1000ULL != 0)
        ++totalSeconds;
    const int seconds = static_cast<int>(totalSeconds % 60ULL);
    const int minutes = static_cast<int>((totalSeconds / 60ULL) % 60ULL);
    const int hours = static_cast<int>(totalSeconds / 3600ULL);
    wchar_t buffer[32] = {};
    if (hours > 0)
        swprintf_s(buffer, L"%d:%02d", hours, minutes);
    else
        swprintf_s(buffer, L"%d:%02d", minutes, seconds);
    return buffer;
}

HWND g_owner = nullptr;

HWND OwnerWindow()
{
    return g_owner;
}

// Forward declaration used by the mini windows.
void OpenTimerPanelFromMini(int tab);

// ---------------------------------------------------------------------------
// Mini timer sub-island
// ---------------------------------------------------------------------------
class TimerMini
{
public:
    bool Create(HWND owner, int tab);
    void Destroy();
    void SetVisible(bool visible);
    bool IsVisible() const { return m_visible; }
    void Place(int pillLeft, int pillTop);
    void Update(bool paused, bool finished, const std::wstring& text,
                float progress);
    int Tab() const { return m_tab; }
    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureClass();
    void ComputeMetrics();
    bool EnsureSurface();
    void ReleaseSurface();
    void Render();
    void Present();
    void DrawGlyph(Graphics& graphics, const RectF& box);
    Font* GetFont(int pixelSize, bool bold);
    void ClearFonts();
    RectF CircleRectF() const;

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    int m_tab = 0;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    int m_widthPx = 0;
    int m_heightPx = 0;
    POINT m_windowPos = {0, 0};
    bool m_visible = false;
    bool m_paused = false;
    bool m_finished = false;
    std::wstring m_text;
    float m_progress = 0.0f;
    bool m_hasContent = false;

    Bitmap* m_bitmap = nullptr;
    Graphics* m_graphics = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_previousDib = nullptr;
    void* m_dibPixels = nullptr;
    FontFamily* m_fontFamily = nullptr;
    std::vector<Font*> m_fonts;
};

bool TimerMini::EnsureClass()
{
    static bool registered = false;
    if (registered)
        return true;
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &TimerMini::WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kMiniWindowClass;
    registered = RegisterClassExW(&windowClass) != 0;
    if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        registered = true;
    return registered;
}

void TimerMini::ComputeMetrics()
{
    m_dpi = m_owner != nullptr ? QueryDpi(m_owner) : 96;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    m_widthPx = RoundToInt((kMiniDiameterDip + kMiniPadDip * 2.0f) * m_scale);
    m_heightPx = RoundToInt((kMiniDiameterDip + kMiniPadDip * 2.0f) * m_scale);
}

RectF TimerMini::CircleRectF() const
{
    return RectF(kMiniPadDip * m_scale, kMiniPadDip * m_scale,
                 kMiniDiameterDip * m_scale, kMiniDiameterDip * m_scale);
}

bool TimerMini::EnsureSurface()
{
    if (m_bitmap != nullptr)
        return true;
    m_bitmap = new Bitmap(m_widthPx, m_heightPx, PixelFormat32bppARGB);
    if (m_bitmap->GetLastStatus() != Ok)
    {
        delete m_bitmap;
        m_bitmap = nullptr;
        return false;
    }
    m_graphics = new Graphics(m_bitmap);
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

void TimerMini::ReleaseSurface()
{
    if (m_graphics != nullptr)
    {
        delete m_graphics;
        m_graphics = nullptr;
    }
    if (m_bitmap != nullptr)
    {
        delete m_bitmap;
        m_bitmap = nullptr;
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

void TimerMini::ClearFonts()
{
    for (Font* font : m_fonts)
        delete font;
    m_fonts.clear();
}

Font* TimerMini::GetFont(int pixelSize, bool bold)
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

void TimerMini::DrawGlyph(Graphics& graphics, const RectF& box)
{
    const ThemePalette& palette = GetThemePalette();
    const COLORREF color = m_finished ? palette.accent
                                      : (m_paused ? BlendColor(palette.islandText,
                                                               palette.islandBackground,
                                                               90)
                                                  : palette.islandText);
    const REAL centerX = box.X + box.Width * 0.5f;
    const REAL centerY = box.Y + box.Height * 0.5f;
    const REAL radius = box.Width * 0.30f;
    SolidBrush brush(GdiColor(color));
    Pen pen(GdiColor(color), std::max(1.2f, box.Width * 0.075f));

    if (m_paused)
    {
        const REAL barWidth = box.Width * 0.12f;
        const REAL barHeight = box.Height * 0.42f;
        const REAL gap = box.Width * 0.10f;
        GraphicsPath* left =
            new GraphicsPath();
        left->AddRectangle(
            RectF(centerX - gap / 2.0f - barWidth,
                  centerY - barHeight / 2.0f, barWidth, barHeight));
        graphics.FillPath(&brush, left);
        delete left;
        GraphicsPath* right = new GraphicsPath();
        right->AddRectangle(RectF(centerX + gap / 2.0f,
                                  centerY - barHeight / 2.0f, barWidth,
                                  barHeight));
        graphics.FillPath(&brush, right);
        delete right;
        return;
    }

    if (m_tab == 0)
    {
        // Stopwatch.
        graphics.DrawEllipse(&pen,
                             RectF(centerX - radius, centerY - radius,
                                   radius * 2.0f, radius * 2.0f));
        graphics.DrawLine(&pen, PointF(centerX, centerY - radius - box.Height * 0.12f),
                          PointF(centerX, centerY - radius + box.Height * 0.02f));
        graphics.DrawLine(&pen, PointF(centerX - box.Width * 0.08f,
                                       centerY - radius - box.Height * 0.12f),
                          PointF(centerX + box.Width * 0.08f,
                                 centerY - radius - box.Height * 0.12f));
        graphics.DrawLine(&pen, PointF(centerX, centerY),
                          PointF(centerX, centerY - radius * 0.55f));
        graphics.DrawLine(&pen, PointF(centerX, centerY),
                          PointF(centerX + radius * 0.45f,
                                 centerY + radius * 0.10f));
        return;
    }

    // Countdown clock.
    graphics.DrawEllipse(&pen,
                         RectF(centerX - radius, centerY - radius, radius * 2.0f,
                               radius * 2.0f));
    graphics.DrawLine(&pen, PointF(centerX, centerY),
                      PointF(centerX, centerY - radius * 0.55f));
    graphics.DrawLine(&pen, PointF(centerX, centerY),
                      PointF(centerX + radius * 0.42f, centerY));
    graphics.DrawLine(&pen, PointF(centerX - box.Width * 0.08f,
                                   centerY - radius - box.Height * 0.10f),
                      PointF(centerX + box.Width * 0.08f,
                             centerY - radius - box.Height * 0.10f));
}

void TimerMini::Render()
{
    if (m_bitmap == nullptr)
        return;
    const ThemePalette& palette = GetThemePalette();
    m_graphics->Clear(Color(0, 0, 0, 0));
    const RectF circle = CircleRectF();
    const REAL diameter = circle.Height;
    RenderIslandMaterialRect(*m_graphics, circle, diameter * 0.5f, m_dpi,
                             g_settings.opacity);

    // Countdown progress ring around the circle.
    if (m_tab == 1)
    {
        const REAL ringRadius = diameter * 0.5f - 1.6f * m_scale;
        const REAL ringWidth = std::max(1.2f, 2.0f * m_scale);
        const REAL centerX = circle.X + diameter * 0.5f;
        const REAL centerY = circle.Y + diameter * 0.5f;
        const RectF ringRect(centerX - ringRadius, centerY - ringRadius,
                             ringRadius * 2.0f, ringRadius * 2.0f);
        SolidBrush track(GdiColor(palette.islandText, 38));
        Pen trackPen(GdiColor(palette.islandText, 38), ringWidth);
        m_graphics->DrawEllipse(&trackPen, ringRect);
        (void)track;
        const float fraction = std::max(0.0f, std::min(1.0f, m_progress));
        if (!m_finished && fraction > 0.001f)
        {
            Pen ringPen(GdiColor(palette.accent, m_paused ? 120 : 230),
                        ringWidth);
            m_graphics->DrawArc(&ringPen, ringRect, -90.0f, 360.0f * fraction);
        }
    }

    if (m_finished)
    {
        Pen border(GdiColor(palette.accent, 220), std::max(1.4f, m_scale));
        m_graphics->DrawEllipse(&border, circle);
    }

    const REAL diameterScale = m_scale;
    const RectF glyphBox(circle.X + diameter * 0.5f - 7.0f * diameterScale,
                         circle.Y + 5.0f * diameterScale, 14.0f * diameterScale,
                         14.0f * diameterScale);
    DrawGlyph(*m_graphics, glyphBox);

    const RectF textRect(circle.X + 2.0f * diameterScale,
                         circle.Y + diameter - 21.0f * diameterScale,
                         diameter - 4.0f * diameterScale, 16.0f * diameterScale);
    Font* font = GetFont(std::max(6, RoundToInt(11.0f * diameterScale)), true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush textBrush(GdiColor(m_finished ? palette.accent
                                             : palette.islandText));
    m_graphics->DrawString(m_text.c_str(), static_cast<INT>(m_text.length()),
                           font, textRect, &format, &textBrush);
    Present();
}

void TimerMini::Present()
{
    if (m_hwnd == nullptr || m_bitmap == nullptr)
        return;
    BitmapData data = {};
    const Rect lockRect(0, 0, m_widthPx, m_heightPx);
    if (m_bitmap->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB,
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
    m_bitmap->UnlockBits(&data);

    POINT destinationPoint = {m_windowPos.x, m_windowPos.y};
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

bool TimerMini::Create(HWND owner, int tab)
{
    if (m_hwnd != nullptr)
        return true;
    if (!EnsureClass())
        return false;
    m_owner = owner;
    m_tab = tab;
    ComputeMetrics();
    if (!EnsureSurface())
        return false;

    const wchar_t* title = tab == 0 ? L"Timer Stopwatch" : L"Timer Countdown";
    const HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kMiniWindowClass, title, WS_POPUP, 0, 0, m_widthPx, m_heightPx, owner,
        nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr)
    {
        ReleaseSurface();
        return false;
    }
    return true;
}

void TimerMini::Destroy()
{
    if (m_hwnd != nullptr)
    {
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwnd);
    }
    ReleaseSurface();
    ClearFonts();
    if (m_fontFamily != nullptr)
    {
        delete m_fontFamily;
        m_fontFamily = nullptr;
    }
    m_visible = false;
    m_hasContent = false;
}

void TimerMini::Place(int pillLeft, int pillTop)
{
    if (m_hwnd == nullptr)
        return;
    const int pad = RoundToInt(kMiniPadDip * m_scale);
    const POINT position = {pillLeft - pad, pillTop - pad};
    if (position.x == m_windowPos.x && position.y == m_windowPos.y)
        return;
    m_windowPos = position;
    SetWindowPos(m_hwnd, HWND_TOPMOST, position.x, position.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    if (m_visible)
        Present();
}

void TimerMini::SetVisible(bool visible)
{
    if (m_hwnd == nullptr)
        return;
    if (m_visible == visible)
        return;
    m_visible = visible;
    if (visible)
    {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        Render();
    }
    else
    {
        ShowWindow(m_hwnd, SW_HIDE);
    }
}

void TimerMini::Update(bool paused, bool finished, const std::wstring& text,
                       float progress)
{
    const bool changed = !m_hasContent || paused != m_paused ||
                         finished != m_finished || text != m_text ||
                         std::fabs(progress - m_progress) > 0.005f;
    m_paused = paused;
    m_finished = finished;
    m_text = text;
    m_progress = progress;
    if (changed && m_visible)
    {
        m_hasContent = true;
        Render();
    }
    else if (changed)
    {
        m_hasContent = true;
    }
}

LRESULT CALLBACK TimerMini::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam)
{
    TimerMini* self = reinterpret_cast<TimerMini*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TimerMini*>(create->lpCreateParams);
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

LRESULT TimerMini::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
        ValidateRect(m_hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RectF pill = CircleRectF();
        if (static_cast<float>(point.x) >= pill.X && point.x < pill.X + pill.Width &&
            static_cast<float>(point.y) >= pill.Y &&
            point.y < pill.Y + pill.Height)
        {
            OpenTimerPanelFromMini(m_tab);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_DPICHANGED:
        ComputeMetrics();
        ReleaseSurface();
        EnsureSurface();
        Render();
        return 0;
    case WM_DESTROY:
        ReleaseSurface();
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
// Timer panel window
// ---------------------------------------------------------------------------
class TimerWindow
{
public:
    bool Create(HWND owner);
    void Destroy();
    void Show(HWND owner);
    void ShowOnTab(HWND owner, int tab);
    void Hide();
    bool IsVisible() const { return m_visible; }
    void OpenFromMini(int tab);

    void TestOpen(int tab);
    void TestStopwatchRunning();
    void TestCountdownRunning();
    void TestBothRunning();
    void TestFinished();
    void TestFinishedPanel();
    void TestReopenRunning();
    void TestWheels(int hours, int minutes, int seconds);

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
        kPrimary = 6,
        kSecondary = 7,
        // Wheels use their own high id range: the old contiguous ids made the
        // primary button look like wheel column 1 and swallowed its clicks.
        kWheelBase = 20, // + column (0..2)
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
        ULONGLONG durationMs =
            (static_cast<ULONGLONG>(kDefaultMinutes) * 60ULL) * 1000ULL;
        ULONGLONG remainingMs = durationMs;
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
    void UpdateTimeRegion();
    void UpdateWheelsAndTime(int columnMask);

    void DrawContent(Graphics& graphics);
    void DrawHeader(Graphics& graphics);
    void DrawTabs(Graphics& graphics);
    void DrawTime(Graphics& graphics);
    void DrawStatus(Graphics& graphics);
    void DrawProgress(Graphics& graphics);
    void DrawWheels(Graphics& graphics);
    void DrawWheelColumn(Graphics& graphics, int column);
    void DrawButtons(Graphics& graphics);
    void DrawPill(Graphics& graphics, const RectF& rect, const std::wstring& text,
                  bool accent, bool hovered, bool pressed, bool enabled,
                  int fontDip);
    void DrawSymbolButton(Graphics& graphics, const RectF& rect,
                          const wchar_t* glyph, bool hovered, bool pressed);

    Font* GetFont(int pixelSize, bool bold);
    void ClearFonts();

    RectF CardRectF() const;
    RectF TimeRectF() const;
    RectF StatusRectF() const;
    RectF ProgressRectF() const;
    RectF WheelLabelRectF(int column) const;
    RectF WheelColumnRectF(int column) const;
    RectF PrimaryRectF() const;
    RectF SecondaryRectF() const;
    RectF CloseRectF() const;
    RectF TabRectF(int tab) const;
    RECT TimeRegionPx() const;
    RECT WheelRegionPx(int column) const;

    int HitTest(POINT point) const;
    void UpdateHover(POINT point);
    void OnMouseLeave();
    void OnLButtonDown(POINT point);
    void OnLButtonUp(POINT point);
    void OnMouseMove(POINT point);
    void OnMouseWheel(POINT screenPoint, int delta);
    void ActivateControl(int id);

    // Wheel helpers.
    int WheelValue(int column) const;
    void SetWheelValue(int column, int value);
    void AnimateWheels();
    void StartWheelAnimation();
    void SyncCountdownFromWheels();
    bool WheelsEditable() const;
    int WheelValueUnderPoint(int column, POINT point) const;
    int WheelRange(int column) const { return kWheelRanges[column]; }

    // Timer state helpers.
    ULONGLONG StopwatchElapsedMs() const;
    void StartStopwatch();
    void PauseStopwatch();
    void ResetStopwatch();
    ULONGLONG CountdownRemainingMs() const;
    ULONGLONG CountdownDisplayMs() const;
    void StartCountdown();
    void PauseCountdown();
    void ResetCountdown();
    void CompleteCountdown();
    void UpdateTimers();
    void UpdateMinis();
    void OnStopwatchTick();
    void OnCountdownTick();
    void OnMiniPoll();

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    int m_widthPx = 0;
    int m_heightPx = 0;

    Tab m_tab = Tab::Stopwatch;
    StopwatchState m_stopwatch;
    CountdownState m_countdown;
    int m_wheels[3] = {kDefaultHours, kDefaultMinutes, kDefaultSeconds};
    float m_wheelPos[3] = {static_cast<float>(kDefaultHours),
                           static_cast<float>(kDefaultMinutes),
                           static_cast<float>(kDefaultSeconds)};
    bool m_wheelAnimRunning = false;

    int m_hovered = kNone;
    int m_pressed = kNone;
    int m_wheelDragColumn = -1;
    int m_wheelDragStartY = 0;
    int m_wheelDragStartValue = 0;
    bool m_trackingMouse = false;
    bool m_visible = false;
    bool m_stopwatchTimerRunning = false;
    bool m_countdownTimerRunning = false;
    bool m_miniPollTimerRunning = false;

    Bitmap* m_material = nullptr;
    Bitmap* m_frame = nullptr;
    Graphics* m_graphics = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_previousDib = nullptr;
    void* m_dibPixels = nullptr;
    FontFamily* m_fontFamily = nullptr;
    std::vector<Font*> m_fonts;
    TimerMini m_minis[2];
};

bool TimerWindow::EnsureClass()
{
    static bool registered = false;
    if (registered)
        return true;
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
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
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    m_widthPx =
        RoundToInt((kCardWidthDip + kShadowDip * 2.0f) * m_scale);
    m_heightPx =
        RoundToInt((kCardHeightDip + kShadowDip * 2.0f) * m_scale);
}

RectF TimerWindow::CardRectF() const
{
    return RectF(kShadowDip * m_scale, kShadowDip * m_scale,
                 kCardWidthDip * m_scale, kCardHeightDip * m_scale);
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

RectF TimerWindow::WheelLabelRectF(int column) const
{
    const RectF card = CardRectF();
    const REAL totalWidth = kWheelColumnWidthDip * 3.0f * m_scale +
                            kWheelColumnGapDip * 2.0f * m_scale;
    const REAL left = card.X + card.Width * 0.5f - totalWidth * 0.5f;
    return RectF(left + static_cast<REAL>(column) *
                            (kWheelColumnWidthDip + kWheelColumnGapDip) * m_scale,
                 card.Y + kWheelsLabelTopDip * m_scale,
                 kWheelColumnWidthDip * m_scale,
                 kWheelsLabelHeightDip * m_scale);
}

RectF TimerWindow::WheelColumnRectF(int column) const
{
    RectF label = WheelLabelRectF(column);
    return RectF(label.X, CardRectF().Y + kWheelsTopDip * m_scale, label.Width,
                 kWheelHeightDip * m_scale);
}

RectF TimerWindow::PrimaryRectF() const
{
    const RectF card = CardRectF();
    const REAL width = kPrimaryWidthDip * m_scale;
    const REAL secondary = kSecondaryWidthDip * m_scale;
    const REAL total = width + kButtonGapDip * m_scale + secondary;
    return RectF(card.X + card.Width * 0.5f - total * 0.5f,
                 card.Y + kButtonsTopDip * m_scale, width,
                 kButtonHeightDip * m_scale);
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

RECT TimerWindow::WheelRegionPx(int column) const
{
    const RectF wheel = WheelColumnRectF(column);
    const REAL pad = 6.0f * m_scale;
    RECT region = {RoundToInt(wheel.X - pad), RoundToInt(wheel.Y - pad),
                   RoundToInt(wheel.X + wheel.Width + pad),
                   RoundToInt(wheel.Y + wheel.Height + pad)};
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

void TimerWindow::UpdateTimeRegion()
{
    if (m_graphics == nullptr || m_hwnd == nullptr)
        return;
    RECT region = TimeRegionPx();
    const GraphicsState state = m_graphics->Save();
    m_graphics->SetClip(Gdiplus::Rect(region.left, region.top,
                                      region.right - region.left,
                                      region.bottom - region.top));
    m_graphics->DrawImage(m_material, 0, 0, m_widthPx, m_heightPx);
    m_graphics->Restore(state);
    DrawTime(*m_graphics);
    DrawStatus(*m_graphics);
    PresentRegion(region);
}

void TimerWindow::UpdateWheelsAndTime(int columnMask)
{
    if (m_graphics == nullptr || m_hwnd == nullptr)
        return;
    RECT region = TimeRegionPx();
    for (int column = 0; column < 3; ++column)
    {
        if ((columnMask & (1 << column)) == 0)
            continue;
        const RECT wheel = WheelRegionPx(column);
        region.left = std::min(region.left, wheel.left);
        region.top = std::min(region.top, wheel.top);
        region.right = std::max(region.right, wheel.right);
        region.bottom = std::max(region.bottom, wheel.bottom);
    }
    const GraphicsState state = m_graphics->Save();
    m_graphics->SetClip(Gdiplus::Rect(region.left, region.top,
                                      region.right - region.left,
                                      region.bottom - region.top));
    m_graphics->DrawImage(m_material, 0, 0, m_widthPx, m_heightPx);
    m_graphics->Restore(state);
    DrawTime(*m_graphics);
    DrawStatus(*m_graphics);
    DrawWheels(*m_graphics);
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
        Pen border(
            GdiColor(BlendColor(palette.islandText, palette.islandBackground, 200),
                     enabled ? 255 : 120),
            std::max(1.0f, m_scale));
        graphics.DrawPath(&border, &path);
    }
    Font* font = GetFont(
        std::max(6, RoundToInt(static_cast<float>(fontDip) * m_scale)), true);
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
    if (hovered || pressed)
    {
        SolidBrush brush(GdiColor(pressed ? RGB(72, 72, 80) : RGB(56, 56, 64)));
        graphics.FillEllipse(&brush, rect);
    }
    Font* font = GetFont(std::max(6, RoundToInt(13.5f * m_scale)), true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush text(GdiColor((hovered || pressed)
                                 ? palette.islandText
                                 : BlendColor(palette.islandText,
                                              palette.islandBackground, 80)));
    graphics.DrawString(glyph, -1, font, rect, &format, &text);
}

void TimerWindow::DrawHeader(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF card = CardRectF();
    Font* titleFont = GetFont(std::max(6, RoundToInt(14.0f * m_scale)), true);
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
    const RectF container(
        card.X + card.Width * 0.5f - kTabsWidthDip * m_scale * 0.5f,
        card.Y + kTabsTopDip * m_scale, kTabsWidthDip * m_scale,
        kTabsHeightDip * m_scale);
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
        Font* font = GetFont(std::max(6, RoundToInt(12.0f * m_scale)), selected);
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
        const REAL pulse = 0.5f + 0.5f * std::sin(static_cast<REAL>(elapsed) *
                                                   0.012f);
        color = BlendColor(palette.accent, palette.islandText,
                           static_cast<int>(60.0f + 80.0f * pulse));
    }
    std::wstring text;
    if (m_tab == Tab::Stopwatch)
        text = FormatStopwatchPrecise(StopwatchElapsedMs());
    else
        text = FormatCountdownClock(CountdownDisplayMs());

    Font* font = GetFont(std::max(8, RoundToInt(kTimeFontDip * m_scale)), true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    const RectF rect = TimeRectF();
    const REAL offset = std::max(1.0f, 1.5f * m_scale);
    SolidBrush shadow(GdiColor(RGB(0, 0, 0), 90));
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
        else if (CountdownDisplayMs() == 0)
        {
            status = L"请先设置时长";
            color = palette.accent;
        }
        else
            status = L"滚动滚轮设置时长";
    }
    Font* font = GetFont(std::max(6, RoundToInt(10.5f * m_scale)), false);
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
    const float fraction =
        std::max(0.0f, std::min(1.0f,
                                static_cast<float>(remaining) /
                                    static_cast<float>(duration)));
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

void TimerWindow::DrawWheelColumn(Graphics& graphics, int column)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF wheel = WheelColumnRectF(column);
    const bool editable = WheelsEditable();
    const int range = WheelRange(column);
    const float position = m_wheelPos[column];
    const float rowHeight = kWheelRowHeightDip * m_scale;
    const float centerY = wheel.Y + wheel.Height * 0.5f;

    GraphicsState state = graphics.Save();
    // Inset clip: only complete rows are visible, never clipped glyph tips.
    const RectF clipRect(wheel.X + 2.0f * m_scale, wheel.Y + 2.0f * m_scale,
                         wheel.Width - 4.0f * m_scale,
                         wheel.Height - 4.0f * m_scale);
    GraphicsPath clipPath;
    AddRoundedRectPath(clipPath, clipRect, 10.0f * m_scale);
    Region clipRegion(&clipPath);
    graphics.SetClip(&clipRegion);

    // Center selection pill (muted while the countdown is running).
    {
        const RectF pill(wheel.X + 4.0f * m_scale,
                         centerY - rowHeight * 0.5f,
                         wheel.Width - 8.0f * m_scale, rowHeight);
        const COLORREF pillColor =
            editable ? palette.accent
                     : BlendColor(palette.islandText, palette.islandBackground,
                                  135);
        SolidBrush pillBrush(GdiColor(pillColor, editable ? 46 : 26));
        GraphicsPath pillPath;
        AddRoundedRectPath(pillPath, pill, rowHeight * 0.5f);
        graphics.FillPath(&pillBrush, &pillPath);
    }

    const float rounded = std::floor(position + 0.5f);
    const int centerValue =
        WrapValue(static_cast<int>(rounded), range);
    const float fraction = position - rounded;
    for (int row = -2; row <= 2; ++row)
    {
        const int value = WrapValue(centerValue + row, range);
        const float offset = static_cast<float>(row) - fraction;
        const float y = centerY + offset * rowHeight;
        const float distance = std::fabs(offset);
        // Edge rows fade out so the wheel never shows half-clipped digits.
        const float fade = distance > 1.7f ? 0.42f : (distance > 0.9f ? 0.70f : 1.0f);
        const BYTE alpha = static_cast<BYTE>(
            ClampInt(RoundToInt(255.0f * fade / (1.0f + 0.55f * distance)), 28, 255));
        const int fontDip =
            distance < 0.5f ? 14 : (distance < 1.5f ? 11 : 9);
        Font* font =
            GetFont(std::max(6, RoundToInt(static_cast<float>(fontDip) * m_scale)),
                    distance < 0.5f);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        const COLORREF rowColor =
            distance < 0.5f
                ? (editable ? palette.accent
                            : BlendColor(palette.islandText,
                                         palette.islandBackground, 110))
                : palette.islandText;
        SolidBrush brush(GdiColor(rowColor,
                                  editable ? alpha
                                           : static_cast<BYTE>(alpha / 2)));
        (void)rowColor;
        wchar_t buffer[8] = {};
        swprintf_s(buffer, L"%02d", value);
        const RectF textRect(wheel.X, y - rowHeight * 0.5f, wheel.Width,
                             rowHeight);
        graphics.DrawString(buffer, -1, font, textRect, &format, &brush);
    }
    graphics.Restore(state);
}

void TimerWindow::DrawWheels(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    static const wchar_t* const labels[3] = {L"时", L"分", L"秒"};
    for (int column = 0; column < 3; ++column)
    {
        const RectF label = WheelLabelRectF(column);
        Font* font = GetFont(std::max(6, RoundToInt(10.0f * m_scale)), false);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush brush(GdiColor(BlendColor(palette.islandText,
                                             palette.islandBackground,
                                             WheelsEditable() ? 110 : 145)));
        graphics.DrawString(labels[column], -1, font, label, &format, &brush);
        DrawWheelColumn(graphics, column);
    }
}

void TimerWindow::DrawButtons(Graphics& graphics)
{
    const RectF primary = PrimaryRectF();
    const RectF secondary = SecondaryRectF();
    std::wstring primaryLabel;
    bool primaryEnabled = true;
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
        if (!m_countdown.finished && !m_countdown.running &&
            !m_countdown.started && CountdownDisplayMs() == 0)
        {
            primaryEnabled = false;
            primaryLabel = L"请先设时长";
        }
    }
    const std::wstring secondaryLabel =
        m_tab == Tab::Stopwatch ? L"清零" : L"重置";
    DrawPill(graphics, primary, primaryLabel, true, m_hovered == kPrimary,
             m_pressed == kPrimary, primaryEnabled, 12);
    DrawPill(graphics, secondary, secondaryLabel, false,
             m_hovered == kSecondary, m_pressed == kSecondary, true, 12);
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
        DrawWheels(graphics);
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
    if (m_tab == Tab::Countdown && WheelsEditable())
    {
        for (int column = 0; column < 3; ++column)
        {
            if (inside(WheelColumnRectF(column)))
                return kWheelBase + column;
        }
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
    if (hit >= kWheelBase && hit < kWheelBase + 3 && WheelsEditable())
    {
        const int column = hit - kWheelBase;
        m_wheelDragColumn = column;
        m_wheelDragStartY = point.y;
        m_wheelDragStartValue = WheelValue(column);
        SetCapture(m_hwnd);
        // Clicking a visible number selects it immediately.
        const int value = WheelValueUnderPoint(column, point);
        if (value >= 0)
            SetWheelValue(column, value);
        return;
    }
    if (hit == kNone)
    {
        m_pressed = kNone;
        return;
    }
    m_pressed = hit;
    SetCapture(m_hwnd);
    RenderFull();
}

void TimerWindow::OnMouseMove(POINT point)
{
    if (m_wheelDragColumn >= 0 && WheelsEditable())
    {
        const float rowHeight = kWheelRowHeightDip * m_scale;
        const int rows = RoundToInt(static_cast<float>(m_wheelDragStartY -
                                                       point.y) /
                                    std::max(1.0f, rowHeight));
        const int range = WheelRange(m_wheelDragColumn);
        const int value = WrapValue(m_wheelDragStartValue + rows, range);
        if (value != WheelValue(m_wheelDragColumn))
            SetWheelValue(m_wheelDragColumn, value);
        return;
    }
    UpdateHover(point);
}

void TimerWindow::OnMouseWheel(POINT screenPoint, int delta)
{
    if (m_tab != Tab::Countdown || !WheelsEditable() || delta == 0)
        return;
    POINT client = screenPoint;
    ScreenToClient(m_hwnd, &client);
    for (int column = 0; column < 3; ++column)
    {
        const RectF wheel = WheelColumnRectF(column);
        if (static_cast<float>(client.x) >= wheel.X &&
            static_cast<float>(client.x) < wheel.X + wheel.Width &&
            static_cast<float>(client.y) >= wheel.Y &&
            static_cast<float>(client.y) < wheel.Y + wheel.Height)
        {
            const int range = WheelRange(column);
            const int value = WrapValue(WheelValue(column) + (delta > 0 ? -1 : 1),
                                        range);
            SetWheelValue(column, value);
            return;
        }
    }
}

void TimerWindow::OnLButtonUp(POINT point)
{
    if (m_wheelDragColumn >= 0)
    {
        m_wheelDragColumn = -1;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        return;
    }
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
        m_tab = Tab::Stopwatch;
        RenderFull();
        UpdateTimers();
        return;
    case kTabCountdown:
        m_tab = Tab::Countdown;
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
                PauseCountdown();
            else
                StartCountdown();
        }
        return;
    case kSecondary:
        if (m_tab == Tab::Stopwatch)
            ResetStopwatch();
        else
            ResetCountdown();
        return;
    default:
        break;
    }

}

int TimerWindow::WheelValue(int column) const
{
    if (column < 0 || column > 2)
        return 0;
    return m_wheels[column];
}

void TimerWindow::SetWheelValue(int column, int value)
{
    if (column < 0 || column > 2)
        return;
    const int range = WheelRange(column);
    const int wrapped = WrapValue(value, range);
    if (wrapped == m_wheels[column] && std::fabs(m_wheelPos[column] -
                                                static_cast<float>(wrapped)) <
                                          0.01f)
        return;
    m_wheels[column] = wrapped;
    SyncCountdownFromWheels();
    StartWheelAnimation();
    UpdateWheelsAndTime(1 << column);
}

void TimerWindow::SyncCountdownFromWheels()
{
    const ULONGLONG duration =
        (static_cast<ULONGLONG>(m_wheels[0]) * 3600ULL +
         static_cast<ULONGLONG>(m_wheels[1]) * 60ULL +
         static_cast<ULONGLONG>(m_wheels[2])) *
        1000ULL;
    m_countdown.durationMs = duration;
    m_countdown.remainingMs = duration;
    m_countdown.running = false;
    m_countdown.started = false;
    m_countdown.finished = false;
}

int TimerWindow::WheelValueUnderPoint(int column, POINT point) const
{
    const RectF wheel = WheelColumnRectF(column);
    const REAL rowHeight = kWheelRowHeightDip * m_scale;
    const REAL centerY = wheel.Y + wheel.Height * 0.5f;
    const int rows = RoundToInt((static_cast<REAL>(point.y) - centerY) / rowHeight);
    if (rows < -2 || rows > 2)
        return -1;
    return WrapValue(m_wheels[column] + rows, WheelRange(column));
}

bool TimerWindow::WheelsEditable() const
{
    return m_tab == Tab::Countdown && !m_countdown.running &&
           !m_countdown.started && !m_countdown.finished;
}

void TimerWindow::StartWheelAnimation()
{
    if (m_wheelAnimRunning)
        return;
    SetTimer(m_hwnd, kWheelAnimTimerId, kWheelAnimIntervalMs, nullptr);
    m_wheelAnimRunning = true;
}

void TimerWindow::AnimateWheels()
{
    bool moving = false;
    bool changed = false;
    for (int column = 0; column < 3; ++column)
    {
        const float target = static_cast<float>(m_wheels[column]);
        const float range = static_cast<float>(WheelRange(column));
        float delta = target - m_wheelPos[column];
        if (delta > range * 0.5f)
            delta -= range;
        if (delta < -range * 0.5f)
            delta += range;
        if (std::fabs(delta) < 0.02f)
        {
            if (m_wheelPos[column] != target)
            {
                m_wheelPos[column] = target;
                changed = true;
            }
            continue;
        }
        m_wheelPos[column] += delta * 0.38f;
        if (m_wheelPos[column] < 0.0f)
            m_wheelPos[column] += range;
        if (m_wheelPos[column] >= range)
            m_wheelPos[column] -= range;
        moving = true;
        changed = true;
    }
    if (changed)
        UpdateWheelsAndTime(0x7);
    if (!moving && m_wheelAnimRunning)
    {
        KillTimer(m_hwnd, kWheelAnimTimerId);
        m_wheelAnimRunning = false;
        RenderFull();
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
    if (m_visible)
        Hide();
    else
    {
        RenderFull();
        UpdateTimers();
        UpdateMinis();
    }
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
    UpdateMinis();
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

ULONGLONG TimerWindow::CountdownDisplayMs() const
{
    if (m_countdown.running)
        return CountdownRemainingMs();
    if (m_countdown.started || m_countdown.finished)
        return m_countdown.remainingMs;
    return m_countdown.durationMs;
}

void TimerWindow::StartCountdown()
{
    if (m_countdown.running)
        return;
    if (m_countdown.durationMs == 0)
        return;
    if (m_countdown.remainingMs == 0)
        m_countdown.remainingMs = m_countdown.durationMs;
    m_countdown.finished = false;
    m_countdown.started = true;
    m_countdown.running = true;
    m_countdown.endTick = NowMs() + m_countdown.remainingMs;
    if (m_visible)
        Hide();
    else
    {
        RenderFull();
        UpdateTimers();
        UpdateMinis();
    }
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
    UpdateMinis();
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
    if (m_visible)
        RenderFull();
    UpdateTimers();
    UpdateMinis();
}

void TimerWindow::UpdateTimers()
{
    const bool needStopwatch = m_visible && m_stopwatch.running;
    const bool needCountdown =
        m_countdown.running ||
        (m_visible && m_countdown.finished &&
         NowMs() - m_countdown.finishedTick < kFinishedFlashMs);
    const bool needMiniPoll =
        !m_visible && (m_stopwatch.started || m_countdown.started ||
                       m_countdown.finished);

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
    if (needMiniPoll && !m_miniPollTimerRunning)
    {
        SetTimer(m_hwnd, kMiniPollTimerId, kMiniPollIntervalMs, nullptr);
        m_miniPollTimerRunning = true;
    }
    else if (!needMiniPoll && m_miniPollTimerRunning)
    {
        KillTimer(m_hwnd, kMiniPollTimerId);
        m_miniPollTimerRunning = false;
    }
}

void TimerWindow::UpdateMinis()
{
    const bool featureRow = GetFeatureRowProgress() > 0.15f;
    const bool islandHidden = IsIslandHiddenTarget() ||
                              GetIslandHideProgress() > 0.05f;
    const bool screensaverActive = IsScreensaverActive();
    const bool suppress = m_visible || featureRow || islandHidden ||
                          screensaverActive;

    const bool stopwatchActive = m_stopwatch.started;
    const bool countdownActive = m_countdown.started || m_countdown.finished;

    if (suppress || (!stopwatchActive && !countdownActive))
    {
        m_minis[0].SetVisible(false);
        m_minis[1].SetVisible(false);
        return;
    }

    const RECT capsule = GetIslandRestingCapsuleRect();
    const UINT dpi = m_owner != nullptr ? QueryDpi(m_owner) : m_dpi;
    const float scale = static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0f;
    const int diameter = RoundToInt(kMiniDiameterDip * scale);
    const int pad = RoundToInt(kMiniPadDip * scale);
    const int gap = RoundToInt(kMiniGapDip * scale);
    const int rightGap = RoundToInt(kMiniRightGapDip * scale);
    const int count = (stopwatchActive ? 1 : 0) + (countdownActive ? 1 : 0);
    const int totalHeight = count * diameter + (count - 1) * gap;
    const int capsuleCenterY = (capsule.top + capsule.bottom) / 2;

    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetIslandWorkArea();
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info))
        workArea = info.rcWork;

    // Circles live to the RIGHT of the capsule, vertically centred on it.
    // While an island toast uses the band below the capsule, drop them below it
    // so both the toast and the timer circles stay fully visible.
    int pillTop = capsuleCenterY - totalHeight / 2;
    if (IsIslandToastVisible())
        pillTop = capsule.bottom + RoundToInt(47.0f * scale);
    int pillLeft = capsule.right + rightGap;
    const int windowWidth = diameter + pad * 2;
    if (pillLeft - pad + windowWidth > workArea.right)
        pillLeft = workArea.right - windowWidth + pad;
    if (pillLeft - pad < workArea.left)
        pillLeft = workArea.left + pad;
    if (pillTop - pad < workArea.top)
        pillTop = workArea.top + pad;
    if (pillTop - pad + totalHeight + pad * 2 > workArea.bottom)
        pillTop = workArea.bottom - totalHeight - pad;

    int cursorTop = pillTop;
    if (stopwatchActive)
    {
        if (!m_minis[0].Handle())
            m_minis[0].Create(m_owner, 0);
        const ULONGLONG elapsed = StopwatchElapsedMs();
        m_minis[0].Update(!m_stopwatch.running && m_stopwatch.started, false,
                          FormatMiniTime(elapsed, false), 0.0f);
        m_minis[0].Place(pillLeft, cursorTop);
        m_minis[0].SetVisible(true);
        cursorTop += diameter + gap;
    }
    else
    {
        m_minis[0].SetVisible(false);
    }

    if (countdownActive)
    {
        if (!m_minis[1].Handle())
            m_minis[1].Create(m_owner, 1);
        const ULONGLONG remaining = CountdownRemainingMs();
        const float progress =
            m_countdown.durationMs > 0
                ? static_cast<float>(remaining) /
                      static_cast<float>(m_countdown.durationMs)
                : 0.0f;
        const std::wstring text =
            m_countdown.finished ? std::wstring(L"时间到")
                                 : FormatMiniTime(remaining, true);
        m_minis[1].Update(!m_countdown.running && m_countdown.started &&
                              !m_countdown.finished,
                          m_countdown.finished, text, progress);
        m_minis[1].Place(pillLeft, cursorTop);
        m_minis[1].SetVisible(true);
    }
    else
    {
        m_minis[1].SetVisible(false);
    }
}

void TimerWindow::OnStopwatchTick()
{
    if (!m_stopwatch.running || !m_visible)
        return;
    UpdateTimeRegion();
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
            UpdateTimeRegion();
        else
            UpdateMinis();
        return;
    }
    if (m_visible && m_countdown.finished &&
        NowMs() - m_countdown.finishedTick < kFinishedFlashMs)
    {
        UpdateTimeRegion();
    }
    else
    {
        UpdateTimers();
    }
}

void TimerWindow::OnMiniPoll()
{
    UpdateMinis();
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
        else if (wParam == kMiniPollTimerId)
            OnMiniPoll();
        else if (wParam == kWheelAnimTimerId)
            AnimateWheels();
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
        OnMouseMove(point);
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
    case WM_MOUSEWHEEL:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        OnMouseWheel(point, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (m_pressed != kNone || m_wheelDragColumn >= 0)
        {
            m_pressed = kNone;
            m_wheelDragColumn = -1;
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
            SetCursor(LoadCursorW(nullptr,
                                  HitTest(point) != kNone ? IDC_HAND : IDC_ARROW));
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
        KillTimer(m_hwnd, kMiniPollTimerId);
        KillTimer(m_hwnd, kWheelAnimTimerId);
        m_stopwatchTimerRunning = false;
        m_countdownTimerRunning = false;
        m_miniPollTimerRunning = false;
        m_wheelAnimRunning = false;
        m_minis[0].Destroy();
        m_minis[1].Destroy();
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
    UpdateMinis();
    RenderFull();
}

void TimerWindow::ShowOnTab(HWND owner, int tab)
{
    m_tab = tab == 0 ? Tab::Stopwatch : Tab::Countdown;
    Show(owner);
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
    UpdateMinis();
}

void TimerWindow::Destroy()
{
    if (m_hwnd != nullptr)
    {
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwnd);
    }
    else
    {
        m_minis[0].Destroy();
        m_minis[1].Destroy();
    }
}

void TimerWindow::OpenFromMini(int tab)
{
    ShowOnTab(m_owner, tab);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------
void TimerWindow::TestOpen(int tab)
{
    m_tab = tab == 0 ? Tab::Stopwatch : Tab::Countdown;
    Show(OwnerWindow());
}

void TimerWindow::TestStopwatchRunning()
{
    Show(OwnerWindow());
    m_tab = Tab::Stopwatch;
    m_stopwatch.started = true;
    m_stopwatch.running = true;
    m_stopwatch.accumulatedMs = 42370;
    m_stopwatch.startTick = NowMs();
    Hide();
    UpdateTimers();
    UpdateMinis();
}

void TimerWindow::TestCountdownRunning()
{
    Show(OwnerWindow());
    m_tab = Tab::Countdown;
    m_wheels[0] = 0;
    m_wheels[1] = 5;
    m_wheels[2] = 0;
    m_wheelPos[0] = 0.0f;
    m_wheelPos[1] = 5.0f;
    m_wheelPos[2] = 0.0f;
    m_countdown.durationMs = 300000;
    m_countdown.remainingMs = 300000;
    m_countdown.finished = false;
    m_countdown.started = true;
    m_countdown.running = true;
    m_countdown.endTick = NowMs() + 300000;
    Hide();
    UpdateTimers();
    UpdateMinis();
}

void TimerWindow::TestBothRunning()
{
    Show(OwnerWindow());
    m_stopwatch.started = true;
    m_stopwatch.running = true;
    m_stopwatch.accumulatedMs = 42370;
    m_stopwatch.startTick = NowMs();
    m_wheels[0] = 0;
    m_wheels[1] = 5;
    m_wheels[2] = 0;
    m_wheelPos[0] = 0.0f;
    m_wheelPos[1] = 5.0f;
    m_wheelPos[2] = 0.0f;
    m_countdown.durationMs = 300000;
    m_countdown.remainingMs = 300000;
    m_countdown.finished = false;
    m_countdown.started = true;
    m_countdown.running = true;
    m_countdown.endTick = NowMs() + 300000;
    Hide();
    UpdateTimers();
    UpdateMinis();
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
    Hide();
    HWND owner = OwnerWindow();
    if (owner != nullptr)
        PostMessageW(owner, kMsgTimerFinished, 0, 0);
    UpdateTimers();
    UpdateMinis();
}

void TimerWindow::TestFinishedPanel()
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
}

void TimerWindow::TestReopenRunning()
{
    Show(OwnerWindow());
    m_tab = Tab::Countdown;
    m_wheels[0] = 0;
    m_wheels[1] = 5;
    m_wheels[2] = 0;
    m_wheelPos[0] = 0.0f;
    m_wheelPos[1] = 5.0f;
    m_wheelPos[2] = 0.0f;
    m_countdown.durationMs = 300000;
    m_countdown.remainingMs = 300000;
    m_countdown.finished = false;
    m_countdown.started = true;
    m_countdown.running = true;
    m_countdown.endTick = NowMs() + 300000;
    ShowOnTab(OwnerWindow(), 1);
}

void TimerWindow::TestWheels(int hours, int minutes, int seconds)
{
    Show(OwnerWindow());
    m_tab = Tab::Countdown;
    m_wheels[0] = WrapValue(hours, 100);
    m_wheels[1] = WrapValue(minutes, 60);
    m_wheels[2] = WrapValue(seconds, 60);
    m_wheelPos[0] = static_cast<float>(m_wheels[0]);
    m_wheelPos[1] = static_cast<float>(m_wheels[1]);
    m_wheelPos[2] = static_cast<float>(m_wheels[2]);
    SyncCountdownFromWheels();
    RenderFull();
    UpdateTimers();
    UpdateMinis();
}

TimerWindow& Instance()
{
    static TimerWindow* instance = new TimerWindow();
    return *instance;
}

void OpenTimerPanelFromMini(int tab)
{
    Instance().OpenFromMini(tab);
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

    if (wcsstr(commandLine, L"--test-timer-running-both") != nullptr)
    {
        Instance().TestBothRunning();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-running-countdown") != nullptr)
    {
        Instance().TestCountdownRunning();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-stopwatch") != nullptr)
    {
        Instance().TestStopwatchRunning();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-finished-panel") != nullptr)
    {
        Instance().TestFinishedPanel();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-finished") != nullptr)
    {
        Instance().TestFinished();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-reopen") != nullptr)
    {
        Instance().TestReopenRunning();
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-wheel-hours") != nullptr)
    {
        Instance().TestWheels(2, 5, 0);
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-wheel-minutes") != nullptr)
    {
        Instance().TestWheels(0, 37, 0);
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-wheel-seconds") != nullptr)
    {
        Instance().TestWheels(0, 5, 42);
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-wheel-changed") != nullptr)
    {
        Instance().TestWheels(1, 23, 45);
        return;
    }
    if (wcsstr(commandLine, L"--test-timer-countdown") != nullptr)
    {
        Instance().TestOpen(1);
        return;
    }
    if (wcsstr(commandLine, L"--test-timer") != nullptr)
    {
        Instance().TestOpen(0);
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
