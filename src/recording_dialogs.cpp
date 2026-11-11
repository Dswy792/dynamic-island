// Dynamic Island - v5.4 custom recording dialogs.
//
// Two modeless popup windows (WS_POPUP + WS_EX_LAYERED | WS_EX_TOOLWINDOW |
// WS_EX_TOPMOST) without a title bar and without a taskbar entry. The whole
// dialog - shadow, rounded card, labels, buttons and the duration wheel - is
// rendered with GDI+ into a persistent 32bpp ARGB surface and presented with
// UpdateLayeredWindow, i.e. the exact same technique and visual language as the
// island and the settings window. GDI+ is initialised once by main(); this file
// never calls GdiplusStartup/GdiplusShutdown.
//
// The confirm dialog asks for confirmation; the duration dialog offers a
// 1 hour / 2 hours / unlimited wheel that can be operated with the mouse wheel,
// the up/down keys, on-screen chevrons or by clicking an item. Enter accepts,
// Esc cancels.

#include "recording_dialogs.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <shlobj.h>

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
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

namespace
{
// ---------------------------------------------------------------------------
// Geometry (device independent pixels, 96 DPI)
// ---------------------------------------------------------------------------
constexpr wchar_t kRecordingDialogClass[] = L"DynamicIslandRecordingDialogClass";

constexpr float kShadowDip = 20.0f;
constexpr float kCardRadiusDip = 16.0f;
constexpr float kCardWidthDip = 372.0f;
constexpr float kNoticeCardHeightDip = 214.0f;
constexpr float kConfirmCardHeightDip = 196.0f;
constexpr float kDurationCardHeightDip = 336.0f;
constexpr float kContentMarginDip = 22.0f;
constexpr float kButtonHeightDip = 32.0f;
constexpr float kButtonWidthDip = 96.0f;
constexpr float kButtonGapDip = 10.0f;
constexpr float kButtonBottomMarginDip = 18.0f;

constexpr float kWheelStepDip = 52.0f;
constexpr float kWheelCenterHeightDip = 46.0f;
constexpr float kWheelArrowSizeDip = 30.0f;
constexpr float kWheelViewportHeightDip = 172.0f;

constexpr UINT_PTR kWheelTimerId = 1;
constexpr UINT kWheelTimerMs = 16;
constexpr float kWheelMoveMsPerItem = 140.0f;

constexpr int kDurationCount = 3;
const int kDurationLimits[kDurationCount] = {kRecordingOneHourSeconds,
                                              kRecordingTwoHoursSeconds,
                                              kRecordingUnlimitedSeconds};
const wchar_t* const kDurationLabels[kDurationCount] = {L"1 小时", L"2 小时",
                                                        L"不限制"};

enum ControlId
{
    kCtrlNone = 0,
    kCtrlCancel,
    kCtrlStart,
    kCtrlWheelUp,
    kCtrlWheelDown,
};

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

float ClampFloat(float value, float minimum, float maximum)
{
    if (!(value > minimum)) // also catches NaN
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

void AddRoundedRectPath(GraphicsPath& path, const RectF& rect, float radius)
{
    const float maximumRadius = std::min(rect.Width, rect.Height) * 0.5f;
    const float clamped = std::clamp(radius, 0.0f, maximumRadius);
    if (clamped <= 0.5f)
    {
        path.AddRectangle(rect);
        return;
    }
    const float diameter = clamped * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f,
                90.0f);
    path.AddArc(rect.X + rect.Width - diameter,
                rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f,
                90.0f);
    path.CloseFigure();
}

COLORREF SecondaryTextColor(const ThemePalette& palette)
{
    return BlendColor(palette.islandText, palette.islandBackground,
                      palette.dark ? 110 : 120);
}

COLORREF TertiaryTextColor(const ThemePalette& palette)
{
    return BlendColor(palette.islandText, palette.islandBackground,
                      palette.dark ? 150 : 160);
}

// Colors for the "取消" style button on the island material.
COLORREF NeutralButtonFill(const ThemePalette& palette, bool hovered,
                           bool pressed)
{
    const COLORREF background = palette.islandBackground;
    const COLORREF tint =
        palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
    int weight = palette.dark ? 22 : 12;
    if (hovered)
        weight += palette.dark ? 16 : 12;
    if (pressed)
        weight += 8;
    return BlendColor(background, tint, weight);
}

COLORREF NeutralButtonBorder(const ThemePalette& palette)
{
    return BlendColor(palette.islandText, palette.islandBackground,
                      palette.dark ? 200 : 210);
}

UINT QueryWindowDpi(HWND hwnd)
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

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length =
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(MAX_PATH * 2));
    if (length == 0)
        return std::wstring();
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash != nullptr)
        *slash = L'\0';
    return path;
}

std::wstring WindowsErrorMessage(int code)
{
    wchar_t buffer[256] = {};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(code), 0, buffer,
        static_cast<DWORD>(256), nullptr);
    if (length == 0)
        return L"错误码 " + std::to_wstring(code);
    std::wstring message(buffer, length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ' || message.back() == L'\t'))
        message.pop_back();
    return message;
}

// ---------------------------------------------------------------------------
// The dialog window
// ---------------------------------------------------------------------------
class RecordingDialog
{
public:
    enum class Mode
    {
        Notice,   // AI endpoint / API key not configured: nothing can start
        Confirm,  // "取消" / "开始"
        Duration, // duration wheel + "取消" / "开始记录"
    };

    bool Create(HWND owner, Mode mode, RecordingDialogCallback callback,
                void* context);
    void Close(bool invokeCallback, int limitSeconds);
    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);

    bool EnsureClass();
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void UpdateMetrics();
    void Relayout(UINT dpi, const RECT& suggested);
    void PositionWindow();
    void RequestPaint();
    void PaintNow();
    void FinishAction(int limitSeconds);
    void FinishCancel();
    void UpdateHoverFromPoint(POINT point);
    void OnMouseMove(POINT point);
    void OnMouseLeave();
    void OnLButtonDown(POINT point);
    void OnLButtonUp(POINT point);
    void OnMouseWheel(int delta);
    void OnTimer();
    void OnKeyDown(WPARAM key);
    void SelectWheelIndex(int index);

    // GDI+ surface helpers.
    bool EnsureSurface(int width, int height);
    void DestroySurface();
    bool PresentFrame();
    void ClearFonts();
    Font* GetFont(int pixelSize, bool bold);

    // Drawing.
    void DrawScene(Graphics& graphics);
    void DrawConfirmContent(Graphics& graphics);
    void DrawNoticeContent(Graphics& graphics);
    void DrawDurationContent(Graphics& graphics);
    void DrawWheel(Graphics& graphics);
    void DrawButtons(Graphics& graphics);
    void DrawChevron(Graphics& graphics, const RectF& rect, bool up,
                     bool hovered);
    void DrawLabel(Graphics& graphics, const std::wstring& text,
                   const RectF& rect, float sizeDip, bool bold, COLORREF color,
                   StringAlignment align = StringAlignmentNear,
                   StringAlignment lineAlign = StringAlignmentCenter,
                   bool ellipsis = false, bool wrap = false);

    // Layout helpers (device independent pixels -> physical pixels).
    float Px(float dip) const { return dip * m_scale; }
    float WindowWidthDip() const;
    float WindowHeightDip() const;
    float CardWidthDipPx() const { return Px(kCardWidthDip); }
    float CardHeightDipPx() const;
    RectF CardRectF() const;
    RectF TitleRectF() const;
    RectF SubtitleRectF() const;
    RectF BodyRectF() const;
    RectF PathRectF() const;
    RectF HintRectF() const;
    RectF WheelRectF() const;
    RectF WheelArrowRectF(bool up) const;
    RectF ButtonRectF(bool start) const;
    PointF WheelCenter() const;

    int HitTestControl(POINT point) const;
    int HitTestWheelItem(POINT point) const; // wrapped item index or -1

    // Continuous wheel position used for the animated item offsets.
    float WheelOffset(int index) const;
    float WrapWheelDelta(float delta) const;

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    Mode m_mode = Mode::Confirm;
    RecordingDialogCallback m_callback = nullptr;
    void* m_context = nullptr;

    UINT m_dpi = 96;
    float m_scale = 1.0f;
    int m_widthPx = 0;
    int m_heightPx = 0;

    int m_hoveredControl = kCtrlNone;
    int m_pressedControl = kCtrlNone;
    bool m_trackingMouse = false;

    int m_selected = 0;
    float m_wheelPosition = 0.0f;
    float m_wheelTarget = 0.0f;
    int m_hoveredWheelItem = -1;
    bool m_wheelTimerRunning = false;

    // Persistent layered-window surface.
    Bitmap* m_bitmap = nullptr;
    Graphics* m_graphics = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_previousDib = nullptr;
    void* m_dibPixels = nullptr;
    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;

    std::vector<Font*> m_fonts;
};

// ---------------------------------------------------------------------------
// Metrics / layout
// ---------------------------------------------------------------------------
float RecordingDialog::WindowWidthDip() const
{
    return kCardWidthDip + kShadowDip * 2.0f;
}

float RecordingDialog::WindowHeightDip() const
{
    float cardHeight = kConfirmCardHeightDip;
    if (m_mode == Mode::Duration)
        cardHeight = kDurationCardHeightDip;
    else if (m_mode == Mode::Notice)
        cardHeight = kNoticeCardHeightDip;
    return cardHeight + kShadowDip * 2.0f;
}

float RecordingDialog::CardHeightDipPx() const
{
    return Px(WindowHeightDip() - kShadowDip * 2.0f);
}

RectF RecordingDialog::CardRectF() const
{
    return RectF(Px(kShadowDip), Px(kShadowDip), Px(kCardWidthDip),
                 CardHeightDipPx());
}

RectF RecordingDialog::TitleRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(16.0f),
                 card.Width - Px(kContentMarginDip * 2.0f), Px(26.0f));
}

RectF RecordingDialog::SubtitleRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(43.0f),
                 card.Width - Px(kContentMarginDip * 2.0f), Px(18.0f));
}

RectF RecordingDialog::BodyRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(72.0f),
                 card.Width - Px(kContentMarginDip * 2.0f), Px(46.0f));
}

RectF RecordingDialog::PathRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(118.0f),
                 card.Width - Px(kContentMarginDip * 2.0f), Px(18.0f));
}

RectF RecordingDialog::HintRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip),
                 card.Y + Px(244.0f),
                 card.Width - Px(kContentMarginDip * 2.0f), Px(16.0f));
}

RectF RecordingDialog::WheelRectF() const
{
    const RectF card = CardRectF();
    const float arrowsWidth = Px(kWheelArrowSizeDip) + Px(10.0f);
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(68.0f),
                 card.Width - Px(kContentMarginDip * 2.0f) - arrowsWidth,
                 Px(kWheelViewportHeightDip));
}

RectF RecordingDialog::WheelArrowRectF(bool up) const
{
    const RectF card = CardRectF();
    const float size = Px(kWheelArrowSizeDip);
    const float x = card.X + card.Width - Px(kContentMarginDip) - size;
    const RectF wheel = WheelRectF();
    const float centerY = wheel.Y + wheel.Height * 0.5f;
    const float y = up ? centerY - Px(20.0f) - size : centerY + Px(20.0f);
    return RectF(x, y, size, size);
}

RectF RecordingDialog::ButtonRectF(bool start) const
{
    const RectF card = CardRectF();
    const float height = Px(kButtonHeightDip);
    const float width = Px(kButtonWidthDip);
    const float y = card.Y + card.Height - Px(kButtonBottomMarginDip) - height;
    const float startX = card.X + card.Width - Px(kContentMarginDip) - width;
    const float x = start ? startX : startX - Px(kButtonGapDip) - width;
    return RectF(x, y, width, height);
}

PointF RecordingDialog::WheelCenter() const
{
    const RectF wheel = WheelRectF();
    return PointF(wheel.X + wheel.Width * 0.5f, wheel.Y + wheel.Height * 0.5f);
}

int RecordingDialog::HitTestControl(POINT point) const
{
    const auto inside = [&point](const RectF& rect) {
        return static_cast<float>(point.x) >= rect.X &&
               static_cast<float>(point.x) < rect.X + rect.Width &&
               static_cast<float>(point.y) >= rect.Y &&
               static_cast<float>(point.y) < rect.Y + rect.Height;
    };

    if (inside(ButtonRectF(false)))
        return kCtrlCancel;
    if (inside(ButtonRectF(true)))
        return kCtrlStart;
    if (m_mode == Mode::Duration)
    {
        if (inside(WheelArrowRectF(true)))
            return kCtrlWheelUp;
        if (inside(WheelArrowRectF(false)))
            return kCtrlWheelDown;
    }
    return kCtrlNone;
}

int RecordingDialog::HitTestWheelItem(POINT point) const
{
    if (m_mode != Mode::Duration)
        return -1;
    const RectF wheel = WheelRectF();
    if (static_cast<float>(point.x) < wheel.X ||
        static_cast<float>(point.x) >= wheel.X + wheel.Width ||
        static_cast<float>(point.y) < wheel.Y ||
        static_cast<float>(point.y) >= wheel.Y + wheel.Height)
        return -1;

    const float step = Px(kWheelStepDip);
    const float delta = (static_cast<float>(point.y) - WheelCenter().Y) / step;
    const int rounded = ClampInt(RoundToInt(delta), -1, 1);
    if (rounded == 0)
        return m_selected;
    const int count = kDurationCount;
    return ((m_selected + rounded) % count + count) % count;
}

float RecordingDialog::WrapWheelDelta(float delta) const
{
    const float count = static_cast<float>(kDurationCount);
    while (delta > count / 2.0f)
        delta -= count;
    while (delta < -count / 2.0f)
        delta += count;
    return delta;
}

float RecordingDialog::WheelOffset(int index) const
{
    return WrapWheelDelta(static_cast<float>(index) - m_wheelPosition);
}

// ---------------------------------------------------------------------------
// Window class / creation
// ---------------------------------------------------------------------------
bool RecordingDialog::EnsureClass()
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &RecordingDialog::WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kRecordingDialogClass;

    registered = RegisterClassExW(&windowClass) != 0;
    if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        registered = true;
    return registered;
}

bool RecordingDialog::Create(HWND owner, Mode mode, RecordingDialogCallback callback,
                             void* context)
{
    if (m_hwnd != nullptr)
        return true;
    if (!EnsureClass())
        return false;

    ClearFonts();
    m_owner = owner;
    m_mode = mode;
    m_callback = callback;
    m_context = context;

    m_hoveredControl = kCtrlNone;
    m_pressedControl = kCtrlNone;
    m_trackingMouse = false;
    m_selected = 0;
    m_wheelPosition = 0.0f;
    m_wheelTarget = 0.0f;
    m_hoveredWheelItem = -1;

    UpdateMetrics();

    const RECT capsule = GetIslandRestingCapsuleRect();
    const int width = m_widthPx;
    const int height = m_heightPx;
    int x = capsule.left + (capsule.right - capsule.left) / 2 - width / 2;
    // Card visual top sits 10 DIP below the capsule; the window adds the shadow
    // padding on top of that.
    int y = capsule.bottom + ScaleDipForDpi(10, m_dpi) - RoundToInt(Px(kShadowDip));

    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetIslandWorkArea();
    if (GetMonitorInfoW(monitor, &info))
        workArea = info.rcWork;
    if (x + width > workArea.right)
        x = workArea.right - width;
    if (x < workArea.left)
        x = workArea.left;
    if (y + height > workArea.bottom)
        y = capsule.top - ScaleDipForDpi(10, m_dpi) - height +
            RoundToInt(Px(kShadowDip));
    if (y < workArea.top)
        y = workArea.top;

    const wchar_t* title = mode == Mode::Duration
                               ? L"选择转写时长"
                               : (mode == Mode::Notice ? L"需要配置 AI 接口"
                                                       : L"会议 / 视频纪要");

    const HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kRecordingDialogClass,
        title, WS_POPUP, x, y, width, height, owner, nullptr,
        GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr)
    {
        m_hwnd = nullptr;
        DestroySurface();
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    PaintNow();
    return true;
}

void RecordingDialog::Close(bool invokeCallback, int limitSeconds)
{
    if (m_hwnd == nullptr)
        return;

    RecordingDialogCallback callback = invokeCallback ? m_callback : nullptr;
    void* context = m_context;
    HWND hwnd = m_hwnd;
    DestroyWindow(hwnd);
    // WM_DESTROY has already cleared m_hwnd / callback pointers.
    if (callback != nullptr)
        callback(limitSeconds, context);
}

void RecordingDialog::FinishAction(int limitSeconds)
{
    if (m_hwnd == nullptr)
        return;
    const RecordingDialogCallback callback = m_callback;
    void* context = m_context;
    HWND hwnd = m_hwnd;
    m_callback = nullptr;
    DestroyWindow(hwnd);
    if (callback != nullptr)
        callback(limitSeconds, context);
}

void RecordingDialog::FinishCancel()
{
    if (m_hwnd == nullptr)
        return;
    m_callback = nullptr;
    DestroyWindow(m_hwnd);
}

void RecordingDialog::UpdateMetrics()
{
    m_dpi = m_owner != nullptr ? QueryWindowDpi(m_owner) : 96;
    if (m_dpi == 0)
        m_dpi = 96;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    m_widthPx = std::max(64, RoundToInt(Px(WindowWidthDip())));
    m_heightPx = std::max(64, RoundToInt(Px(WindowHeightDip())));
}

void RecordingDialog::Relayout(UINT dpi, const RECT& suggested)
{
    if (m_hwnd == nullptr)
        return;
    m_dpi = dpi == 0 ? 96 : dpi;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    m_widthPx = std::max(64, RoundToInt(Px(WindowWidthDip())));
    m_heightPx = std::max(64, RoundToInt(Px(WindowHeightDip())));
    DestroySurface();
    SetWindowPos(m_hwnd, HWND_TOPMOST, suggested.left, suggested.top, m_widthPx,
                 m_heightPx, SWP_NOACTIVATE);
    PositionWindow();
    PaintNow();
}

void RecordingDialog::PositionWindow()
{
    if (m_hwnd == nullptr)
        return;
    RECT windowRect = {};
    GetWindowRect(m_hwnd, &windowRect);
    const RECT capsule = GetIslandRestingCapsuleRect();
    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetIslandWorkArea();
    if (GetMonitorInfoW(monitor, &info))
        workArea = info.rcWork;

    int x = capsule.left + (capsule.right - capsule.left) / 2 - m_widthPx / 2;
    int y = capsule.bottom + ScaleDipForDpi(10, m_dpi) - RoundToInt(Px(kShadowDip));
    if (x + m_widthPx > workArea.right)
        x = workArea.right - m_widthPx;
    if (x < workArea.left)
        x = workArea.left;
    if (y + m_heightPx > workArea.bottom)
        y = capsule.top - ScaleDipForDpi(10, m_dpi) - m_heightPx +
            RoundToInt(Px(kShadowDip));
    if (y < workArea.top)
        y = workArea.top;

    if (x != windowRect.left || y != windowRect.top)
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// Surface management (same technique as the island renderer)
// ---------------------------------------------------------------------------
bool RecordingDialog::EnsureSurface(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (m_bitmap != nullptr && m_surfaceWidth == width && m_surfaceHeight == height)
        return true;

    DestroySurface();

    m_bitmap = new Bitmap(width, height, PixelFormat32bppARGB);
    if (m_bitmap->GetLastStatus() != Ok)
    {
        delete m_bitmap;
        m_bitmap = nullptr;
        return false;
    }

    m_graphics = new Graphics(m_bitmap);
    m_graphics->SetSmoothingMode(SmoothingModeAntiAlias);
    m_graphics->SetPixelOffsetMode(PixelOffsetModeHighQuality);
    m_graphics->SetCompositingMode(CompositingModeSourceOver);
    m_graphics->SetCompositingQuality(CompositingQualityHighQuality);
    m_graphics->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    m_graphics->SetInterpolationMode(InterpolationModeHighQualityBicubic);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
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

    m_surfaceWidth = width;
    m_surfaceHeight = height;
    return true;
}

void RecordingDialog::DestroySurface()
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
    m_surfaceWidth = 0;
    m_surfaceHeight = 0;
}

bool RecordingDialog::PresentFrame()
{
    if (m_hwnd == nullptr || m_bitmap == nullptr)
        return false;

    RECT windowRect = {};
    GetWindowRect(m_hwnd, &windowRect);

    BitmapData data = {};
    Rect lockRect(0, 0, m_surfaceWidth, m_surfaceHeight);
    if (m_bitmap->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB,
                           &data) != Ok)
        return false;

    const BYTE* source = static_cast<const BYTE*>(data.Scan0);
    BYTE* destination = static_cast<BYTE*>(m_dibPixels);
    const size_t sourceStride = static_cast<size_t>(data.Stride);
    const size_t rowBytes = static_cast<size_t>(m_surfaceWidth) * 4;
    for (int y = 0; y < m_surfaceHeight; ++y)
    {
        const BYTE* src = source + static_cast<size_t>(y) * sourceStride;
        BYTE* dst = destination + static_cast<size_t>(y) * rowBytes;
        for (int x = 0; x < m_surfaceWidth; ++x)
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

    POINT destinationPoint = {windowRect.left, windowRect.top};
    SIZE size = {m_widthPx, m_heightPx};
    POINT sourcePoint = {0, 0};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    return UpdateLayeredWindow(m_hwnd, nullptr, &destinationPoint, &size,
                               m_memoryDc, &sourcePoint, 0, &blend,
                               ULW_ALPHA) != FALSE;
}

void RecordingDialog::RequestPaint()
{
    if (m_hwnd != nullptr)
        PaintNow();
}

void RecordingDialog::PaintNow()
{
    if (m_hwnd == nullptr || !EnsureSurface(m_widthPx, m_heightPx))
        return;

    m_graphics->Clear(Color(0, 0, 0, 0));
    DrawScene(*m_graphics);
    PresentFrame();
}

void RecordingDialog::ClearFonts()
{
    for (Font* font : m_fonts)
        delete font;
    m_fonts.clear();
}

Font* RecordingDialog::GetFont(int pixelSize, bool bold)
{
    pixelSize = std::max(4, pixelSize);
    for (Font* font : m_fonts)
    {
        const bool fontBold = (font->GetStyle() & FontStyleBold) != 0;
        if (RoundToInt(font->GetSize()) == pixelSize && fontBold == bold)
            return font;
    }

    static FontFamily* family = nullptr;
    if (family == nullptr)
    {
        const wchar_t* candidates[] = {L"Microsoft YaHei UI", L"Microsoft YaHei",
                                       L"Segoe UI", L"Arial"};
        for (const wchar_t* candidate : candidates)
        {
            FontFamily* candidateFamily = new FontFamily(candidate);
            if (candidateFamily->GetLastStatus() == Ok &&
                candidateFamily->IsAvailable())
            {
                family = candidateFamily;
                break;
            }
            delete candidateFamily;
        }
        if (family == nullptr)
            family = new FontFamily(L"Arial");
    }

    Font* font = new Font(family, static_cast<REAL>(pixelSize),
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

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void RecordingDialog::DrawLabel(Graphics& graphics, const std::wstring& text,
                                const RectF& rect, float sizeDip, bool bold,
                                COLORREF color, StringAlignment align,
                                StringAlignment lineAlign, bool ellipsis,
                                bool wrap)
{
    if (text.empty() || rect.Width <= 1.0f)
        return;

    Font* font = GetFont(std::max(4, RoundToInt(Px(sizeDip))), bold);
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(lineAlign);
    format.SetTrimming(ellipsis ? StringTrimmingEllipsisCharacter
                                : StringTrimmingNone);
    if (!wrap)
    {
        format.SetFormatFlags(StringFormatFlagsNoWrap |
                              StringFormatFlagsMeasureTrailingSpaces);
    }
    SolidBrush brush(GdiColor(color));
    graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font, rect,
                        &format, &brush);
}

void RecordingDialog::DrawScene(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF card = CardRectF();

    RenderIslandMaterialRect(graphics, card, kCardRadiusDip, m_dpi,
                             g_settings.opacity);

    std::wstring title = L"会议 / 视频纪要";
    std::wstring subtitle = L"只保存文字纪要（Markdown/TXT），不保存录音";
    if (m_mode == Mode::Duration)
    {
        title = L"选择转写时长";
        subtitle = L"到达时长上限后自动停止，只保存文字纪要（不保存录音）";
    }
    else if (m_mode == Mode::Notice)
    {
        title = L"需要配置 AI 接口";
        subtitle = L"本功能只保存文字纪要，不保存录音";
    }

    DrawLabel(graphics, title, TitleRectF(), 16.0f, true, palette.islandText);
    DrawLabel(graphics, subtitle, SubtitleRectF(), 10.5f, false,
              SecondaryTextColor(palette), StringAlignmentNear,
              StringAlignmentCenter, true);

    if (m_mode == Mode::Duration)
        DrawDurationContent(graphics);
    else if (m_mode == Mode::Notice)
        DrawNoticeContent(graphics);
    else
        DrawConfirmContent(graphics);

    DrawButtons(graphics);
}

void RecordingDialog::DrawConfirmContent(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const std::wstring body =
        L"个别片段转写失败时，其音频仅临时保留在系统临时目录供重试，不会写入"
        L"保存目录。关闭窗口或点击取消不会开始。";
    DrawLabel(graphics, body, BodyRectF(), 10.0f, false,
              SecondaryTextColor(palette), StringAlignmentNear,
              StringAlignmentNear, false, true);

    const std::wstring path = L"纪要保存到：" + ResolveRecordingSaveDirectory();
    DrawLabel(graphics, path, PathRectF(), 9.5f, false,
              TertiaryTextColor(palette), StringAlignmentNear,
              StringAlignmentCenter, true);
}

void RecordingDialog::DrawNoticeContent(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const std::wstring body =
        L"请先在 设置 → 会议与视频纪要 中配置 AI 接口地址和 API Key，"
        L"然后才能开始记录并转写。";
    DrawLabel(graphics, body, BodyRectF(), 10.0f, false,
              SecondaryTextColor(palette), StringAlignmentNear,
              StringAlignmentNear, false, true);
}

void RecordingDialog::DrawDurationContent(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    DrawWheel(graphics);

    DrawLabel(graphics, L"滚轮 · 方向键 · 点击切换时长", HintRectF(), 9.5f, false,
              TertiaryTextColor(palette), StringAlignmentCenter,
              StringAlignmentCenter, false);
}

void RecordingDialog::DrawWheel(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF wheel = WheelRectF();
    if (wheel.Width < 20.0f || wheel.Height < 20.0f)
        return;

    // Center selection highlight.
    {
        const float highlightWidth = wheel.Width - Px(16.0f);
        const float highlightHeight = Px(kWheelCenterHeightDip);
        const RectF highlight(wheel.X + (wheel.Width - highlightWidth) / 2.0f,
                              WheelCenter().Y - highlightHeight / 2.0f,
                              highlightWidth, highlightHeight);
        SolidBrush brush(palette.dark ? Color(46, GetRValue(palette.accent),
                                              GetGValue(palette.accent),
                                              GetBValue(palette.accent))
                                      : Color(38, GetRValue(palette.accent),
                                              GetGValue(palette.accent),
                                              GetBValue(palette.accent)));
        GraphicsPath* path = nullptr;
        path = new GraphicsPath();
        AddRoundedRectPath(*path, highlight, highlightHeight / 2.0f);
        graphics.FillPath(&brush, path);
        delete path;
    }

    // Clip the wheel viewport so the adjacent items fade out at its edges.
    GraphicsState state = graphics.Save();
    {
        GraphicsPath clipPath;
        AddRoundedRectPath(clipPath, wheel, Px(12.0f));
        Region clipRegion(&clipPath);
        graphics.SetClip(&clipRegion);
    }

    // Draw far items first so the selected one stays on top.
    int order[kDurationCount] = {0, 1, 2};
    for (int i = 0; i < kDurationCount; ++i)
    {
        for (int j = i + 1; j < kDurationCount; ++j)
        {
            if (std::fabs(WheelOffset(order[j])) >
                std::fabs(WheelOffset(order[i])))
                std::swap(order[i], order[j]);
        }
    }

    const PointF center = WheelCenter();
    const float step = Px(kWheelStepDip);
    for (int k = 0; k < kDurationCount; ++k)
    {
        const int index = order[k];
        const float offset = WheelOffset(index);
        const float distance = std::fabs(offset);
        if (distance > 1.8f)
            continue;

        const float itemScale = 1.0f / (1.0f + 0.36f * distance);
        const float alpha = 255.0f / (1.0f + 1.05f * distance);
        const float y = center.Y + offset * step;
        const RectF textRect(wheel.X + Px(14.0f), y - Px(24.0f),
                             wheel.Width - Px(28.0f), Px(48.0f));
        COLORREF color = palette.islandText;
        if (distance > 0.45f)
            color = BlendColor(palette.islandText, palette.islandBackground,
                               palette.dark ? 120 : 130);
        if (m_hoveredWheelItem == index && distance > 0.45f)
            color = palette.islandText;

        Font* font = GetFont(
            std::max(6, RoundToInt(Px(16.0f) * itemScale)),
            distance < 0.45f);
        StringFormat format(StringFormat::GenericTypographic());
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush brush(Color(static_cast<BYTE>(ClampFloat(alpha, 0.0f, 255.0f)),
                               GetRValue(color), GetGValue(color),
                               GetBValue(color)));
        graphics.DrawString(kDurationLabels[index], -1, font, textRect, &format,
                            &brush);
    }

    graphics.Restore(state);

    // Up / down chevrons on the right.
    DrawChevron(graphics, WheelArrowRectF(true), true,
                m_hoveredControl == kCtrlWheelUp);
    DrawChevron(graphics, WheelArrowRectF(false), false,
                m_hoveredControl == kCtrlWheelDown);
}

void RecordingDialog::DrawChevron(Graphics& graphics, const RectF& rect, bool up,
                                  bool hovered)
{
    const ThemePalette& palette = GetThemePalette();
    if (hovered)
    {
        SolidBrush brush(palette.dark ? Color(34, 255, 255, 255)
                                      : Color(28, 0, 0, 0));
        GraphicsPath path;
        AddRoundedRectPath(path, rect, rect.Height * 0.32f);
        graphics.FillPath(&brush, &path);
    }

    const COLORREF color = hovered ? palette.islandText
                                   : SecondaryTextColor(palette);
    const float centerX = rect.X + rect.Width * 0.5f;
    const float centerY = rect.Y + rect.Height * 0.5f;
    const float arm = Px(4.0f);
    Pen pen(GdiColor(color), std::max(1.2f, Px(1.4f)));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    if (up)
    {
        graphics.DrawLine(&pen, PointF(centerX - arm, centerY + arm * 0.7f),
                          PointF(centerX, centerY - arm * 0.7f));
        graphics.DrawLine(&pen, PointF(centerX, centerY - arm * 0.7f),
                          PointF(centerX + arm, centerY + arm * 0.7f));
    }
    else
    {
        graphics.DrawLine(&pen, PointF(centerX - arm, centerY - arm * 0.7f),
                          PointF(centerX, centerY + arm * 0.7f));
        graphics.DrawLine(&pen, PointF(centerX, centerY + arm * 0.7f),
                          PointF(centerX + arm, centerY - arm * 0.7f));
    }
}

void RecordingDialog::DrawButtons(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();

    // Cancel (neutral) button.
    {
        const RectF rect = ButtonRectF(false);
        const COLORREF fill = NeutralButtonFill(
            palette, m_hoveredControl == kCtrlCancel,
            m_pressedControl == kCtrlCancel);
        SolidBrush brush(GdiColor(fill));
        GraphicsPath path;
        AddRoundedRectPath(path, rect, Px(10.0f));
        graphics.FillPath(&brush, &path);
        Pen border(GdiColor(NeutralButtonBorder(palette)), std::max(1.0f, Px(1.0f)));
        graphics.DrawPath(&border, &path);
        DrawLabel(graphics, L"取消", rect, 11.5f, false, palette.islandText,
                  StringAlignmentCenter, StringAlignmentCenter, false);
    }

    // Accent (primary) button.
    {
        const RectF rect = ButtonRectF(true);
        COLORREF fill = palette.accent;
        if (m_pressedControl == kCtrlStart)
            fill = BlendColor(palette.accent, RGB(0, 0, 0), 30);
        else if (m_hoveredControl == kCtrlStart)
            fill = BlendColor(palette.accent, RGB(255, 255, 255), 22);
        SolidBrush brush(GdiColor(fill));
        GraphicsPath path;
        AddRoundedRectPath(path, rect, Px(10.0f));
        graphics.FillPath(&brush, &path);
        const wchar_t* label = m_mode == Mode::Duration
                                   ? L"开始记录"
                                   : (m_mode == Mode::Notice ? L"打开设置"
                                                             : L"开始");
        DrawLabel(graphics, label, rect, 11.5f, true, palette.accentText,
                  StringAlignmentCenter, StringAlignmentCenter, false);
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void RecordingDialog::UpdateHoverFromPoint(POINT point)
{
    int hoveredWheelItem = -1;
    if (m_mode == Mode::Duration &&
        static_cast<float>(point.x) >= WheelRectF().X &&
        static_cast<float>(point.x) < WheelRectF().X + WheelRectF().Width &&
        static_cast<float>(point.y) >= WheelRectF().Y &&
        static_cast<float>(point.y) < WheelRectF().Y + WheelRectF().Height)
    {
        hoveredWheelItem = HitTestWheelItem(point);
    }

    const int control = HitTestControl(point);
    if (control != m_hoveredControl || hoveredWheelItem != m_hoveredWheelItem)
    {
        m_hoveredControl = control;
        m_hoveredWheelItem = hoveredWheelItem;
        RequestPaint();
    }
}

void RecordingDialog::OnMouseMove(POINT point)
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
    UpdateHoverFromPoint(point);
}

void RecordingDialog::OnMouseLeave()
{
    m_trackingMouse = false;
    if (m_hoveredControl != kCtrlNone || m_hoveredWheelItem != -1)
    {
        m_hoveredControl = kCtrlNone;
        m_hoveredWheelItem = -1;
        RequestPaint();
    }
}

void RecordingDialog::OnLButtonDown(POINT point)
{
    const int control = HitTestControl(point);
    if (control == kCtrlNone)
    {
        if (m_mode == Mode::Duration &&
            HitTestWheelItem(point) >= 0)
        {
            m_pressedControl = kCtrlNone;
            SetCapture(m_hwnd);
            return;
        }
        return;
    }

    m_pressedControl = control;
    SetCapture(m_hwnd);
    RequestPaint();
}

void RecordingDialog::OnLButtonUp(POINT point)
{
    // Read the pressed control BEFORE releasing capture: ReleaseCapture sends
    // WM_CAPTURECHANGED synchronously, whose handler clears m_pressedControl,
    // which used to make every button click look like a click on empty space.
    const int pressed = m_pressedControl;
    m_pressedControl = kCtrlNone;

    if (GetCapture() == m_hwnd)
        ReleaseCapture();

    if (pressed == kCtrlNone)
    {
        // A click on a wheel item scrolls it into the center.
        const int item = HitTestWheelItem(point);
        if (m_mode == Mode::Duration && item >= 0 && item != m_selected)
            SelectWheelIndex(item);
        RequestPaint();
        return;
    }

    const int released = HitTestControl(point);
    RequestPaint();
    if (pressed != released)
        return;

    switch (pressed)
    {
    case kCtrlCancel:
        FinishCancel();
        break;
    case kCtrlStart:
        if (m_mode == Mode::Duration)
            FinishAction(kDurationLimits[m_selected]);
        else if (m_mode == Mode::Notice)
            FinishAction(-2); // caller opens the settings window
        else
            FinishAction(-1); // caller continues with the duration step
        break;
    case kCtrlWheelUp:
        SelectWheelIndex((m_selected - 1 + kDurationCount) % kDurationCount);
        break;
    case kCtrlWheelDown:
        SelectWheelIndex((m_selected + 1) % kDurationCount);
        break;
    default:
        break;
    }
}

void RecordingDialog::OnMouseWheel(int delta)
{
    if (m_mode != Mode::Duration || delta == 0)
        return;
    if (delta > 0)
        SelectWheelIndex((m_selected - 1 + kDurationCount) % kDurationCount);
    else
        SelectWheelIndex((m_selected + 1) % kDurationCount);
}

void RecordingDialog::OnKeyDown(WPARAM key)
{
    switch (key)
    {
    case VK_ESCAPE:
        FinishCancel();
        break;
    case VK_RETURN:
        if (m_mode == Mode::Duration)
            FinishAction(kDurationLimits[m_selected]);
        else if (m_mode == Mode::Notice)
            FinishAction(-2);
        else
            FinishAction(-1);
        break;
    case VK_UP:
        if (m_mode == Mode::Duration)
            SelectWheelIndex((m_selected - 1 + kDurationCount) % kDurationCount);
        break;
    case VK_DOWN:
        if (m_mode == Mode::Duration)
            SelectWheelIndex((m_selected + 1) % kDurationCount);
        break;
    case VK_TAB:
        if (m_mode == Mode::Duration)
        {
            const int current = m_hoveredControl == kCtrlCancel ? kCtrlStart
                                                                 : kCtrlCancel;
            m_hoveredControl = current;
            RequestPaint();
        }
        break;
    default:
        break;
    }
}

void RecordingDialog::SelectWheelIndex(int index)
{
    if (m_mode != Mode::Duration)
        return;
    index = ((index % kDurationCount) + kDurationCount) % kDurationCount;
    if (index == m_selected)
        return;
    m_selected = index;

    // Animate along the shortest (wrapped) path.
    float target = static_cast<float>(index);
    while (target - m_wheelPosition > kDurationCount / 2.0f)
        target -= kDurationCount;
    while (target - m_wheelPosition < -kDurationCount / 2.0f)
        target += kDurationCount;
    m_wheelTarget = target;

    if (!m_wheelTimerRunning && m_hwnd != nullptr)
    {
        SetTimer(m_hwnd, kWheelTimerId, kWheelTimerMs, nullptr);
        m_wheelTimerRunning = true;
    }
    RequestPaint();
}

void RecordingDialog::OnTimer()
{
    if (m_mode != Mode::Duration)
        return;

    const float delta = m_wheelTarget - m_wheelPosition;
    if (std::fabs(delta) <= 0.01f)
    {
        m_wheelPosition = m_wheelTarget;
        if (m_wheelTimerRunning && m_hwnd != nullptr)
        {
            KillTimer(m_hwnd, kWheelTimerId);
            m_wheelTimerRunning = false;
        }
        RequestPaint();
        return;
    }

    const float step = static_cast<float>(kWheelTimerMs) / kWheelMoveMsPerItem;
    if (std::fabs(delta) <= step)
        m_wheelPosition = m_wheelTarget;
    else
        m_wheelPosition += (delta > 0.0f) ? step : -step;
    RequestPaint();
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK RecordingDialog::WindowProc(HWND hwnd, UINT message,
                                             WPARAM wParam, LPARAM lParam)
{
    RecordingDialog* self = reinterpret_cast<RecordingDialog*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RecordingDialog*>(create->lpCreateParams);
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

LRESULT RecordingDialog::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    const auto ClientPoint = [lParam]() {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        return point;
    };

    switch (message)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_PAINT:
        // Painting goes through UpdateLayeredWindow; only validate.
        ValidateRect(m_hwnd, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_TIMER:
        if (wParam == kWheelTimerId)
            OnTimer();
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(ClientPoint());
        return 0;

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(ClientPoint());
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(ClientPoint());
        return 0;

    case WM_CAPTURECHANGED:
        if (m_pressedControl != kCtrlNone)
        {
            m_pressedControl = kCtrlNone;
            RequestPaint();
        }
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(m_hwnd, &point);
            const bool interactive =
                HitTestControl(point) != kCtrlNone ||
                (m_mode == Mode::Duration && HitTestWheelItem(point) >= 0);
            SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
    {
        const UINT dpi = LOWORD(wParam) != 0 ? static_cast<UINT>(LOWORD(wParam))
                                             : QueryWindowDpi(m_hwnd);
        RECT suggested = {};
        GetWindowRect(m_hwnd, &suggested);
        if (lParam != 0)
            suggested = *reinterpret_cast<const RECT*>(lParam);
        Relayout(dpi, suggested);
        return 0;
    }

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        RefreshSystemThemeCache();
        RequestPaint();
        return 0;

    case WM_CLOSE:
        FinishCancel();
        return 0;

    case WM_DESTROY:
        OnDestroy();
        return 0;

    default:
        break;
    }

    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

void RecordingDialog::OnCreate()
{
    UpdateMetrics();
    PaintNow();
}

void RecordingDialog::OnDestroy()
{
    m_callback = nullptr;
    m_context = nullptr;
    m_hoveredControl = kCtrlNone;
    m_pressedControl = kCtrlNone;
    m_trackingMouse = false;
    m_hoveredWheelItem = -1;

    if (m_hwnd != nullptr && GetCapture() == m_hwnd)
        ReleaseCapture();
    if (m_hwnd != nullptr && m_wheelTimerRunning)
    {
        KillTimer(m_hwnd, kWheelTimerId);
        m_wheelTimerRunning = false;
    }

    DestroySurface();
    ClearFonts();
    m_hwnd = nullptr; // owned window: never PostQuitMessage from here
}

// ---------------------------------------------------------------------------
// Singleton + public API
// ---------------------------------------------------------------------------
// Deliberately allocated once and never deleted: main() shuts GDI+ down before
// the process exits and a static destructor must not touch GDI+ afterwards.
RecordingDialog& DialogInstance()
{
    static RecordingDialog* instance = new RecordingDialog();
    return *instance;
}
} // namespace

std::wstring ResolveRecordingSaveDirectory()
{
    if (!g_settings.recordSaveDir.empty())
        return g_settings.recordSaveDir;
    const std::wstring exeDirectory = ExeDirectory();
    if (exeDirectory.empty())
        return L"recordings";
    return exeDirectory + L"\\recordings";
}

bool EnsureRecordingDirectory(const std::wstring& directory,
                              std::wstring& errorText)
{
    errorText.clear();
    if (directory.empty())
    {
        errorText = L"纪要目录为空";
        return false;
    }

    const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS ||
        result == ERROR_ALREADY_EXISTS)
        return true;

    errorText = WindowsErrorMessage(result);
    if (errorText.empty())
        errorText = L"无法创建纪要目录";
    return false;
}

void ShowRecordingConfirmDialog(HWND owner, RecordingDialogCallback callback,
                                void* context)
{
    RecordingDialog& dialog = DialogInstance();
    if (dialog.Handle() != nullptr)
    {
        SetForegroundWindow(dialog.Handle());
        return;
    }
    dialog.Create(owner, RecordingDialog::Mode::Confirm, callback, context);
}

void ShowRecordingDurationDialog(HWND owner, RecordingDialogCallback callback,
                                 void* context)
{
    RecordingDialog& dialog = DialogInstance();
    if (dialog.Handle() != nullptr)
    {
        SetForegroundWindow(dialog.Handle());
        return;
    }
    dialog.Create(owner, RecordingDialog::Mode::Duration, callback, context);
}

void ShowRecordingConfigNoticeDialog(HWND owner, RecordingDialogCallback callback,
                                     void* context)
{
    RecordingDialog& dialog = DialogInstance();
    if (dialog.Handle() != nullptr)
    {
        SetForegroundWindow(dialog.Handle());
        return;
    }
    dialog.Create(owner, RecordingDialog::Mode::Notice, callback, context);
}

bool IsRecordingDialogOpen()
{
    return DialogInstance().Handle() != nullptr;
}

void CloseRecordingDialogs()
{
    RecordingDialog& dialog = DialogInstance();
    if (dialog.Handle() != nullptr)
        dialog.Close(false, 0);
}
