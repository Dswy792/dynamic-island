// Dynamic Island - v5.8.1 custom recording dialogs.
//
// Modeless popup windows (WS_POPUP + WS_EX_LAYERED | WS_EX_TOOLWINDOW |
// WS_EX_TOPMOST) without a title bar and without a taskbar entry. The whole
// dialog - shadow, rounded card, labels, buttons and the duration wheels - is
// rendered with GDI+ into a persistent 32bpp ARGB surface and presented with
// UpdateLayeredWindow, i.e. the exact same technique and visual language as the
// island and the settings window. GDI+ is initialised once by main(); this file
// never calls GdiplusStartup/GdiplusShutdown.
//
// v5.8.1 redesign:
//   * compact cards (336 DIP wide): confirm 150, notice 176, duration 328 DIP
//     with much tighter paddings so a few lines of text no longer float in an
//     oversized card;
//   * the duration step uses THREE independent vertical wheels
//     (hours 0-23 / minutes 0-59 / seconds 0-59, default 1:00:00) instead of
//     the old 1h/2h/unlimited preset list. The centre row is highlighted and
//     the neighbouring rows fade out; the mouse wheel, click/drag, Up/Down and
//     Tab/Left/Right all operate on the active column only;
//   * "开始记录" is disabled while the total is 0 seconds and returns the
//     selected H*3600+M*60+S seconds through the existing callback;
//   * wheel changes repaint only the affected column plus the time display and
//     the buttons (dirty-rect UpdateLayeredWindow surface) instead of the whole
//     window, so scrolling stays instant at 150% DPI.

#include "recording_dialogs.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
constexpr float kCardWidthDip = 336.0f;
constexpr float kConfirmCardHeightDip = 150.0f;
constexpr float kNoticeCardHeightDip = 176.0f;
constexpr float kDurationCardHeightDip = 328.0f;
constexpr float kContentMarginDip = 18.0f;

constexpr float kTitleTopDip = 12.0f;
constexpr float kTitleHeightDip = 22.0f;
constexpr float kSubtitleTopDip = 34.0f;
constexpr float kSubtitleHeightDip = 15.0f;

constexpr float kButtonHeightDip = 30.0f;
constexpr float kButtonWidthDip = 88.0f;
constexpr float kButtonGapDip = 8.0f;
constexpr float kButtonBottomMarginDip = 14.0f;

// Confirm page.
constexpr float kConfirmBodyTopDip = 54.0f;
constexpr float kConfirmBodyHeightDip = 34.0f;
constexpr float kConfirmPathTopDip = 90.0f;
constexpr float kConfirmPathHeightDip = 14.0f;

// Notice page.
constexpr float kNoticeBodyTopDip = 56.0f;
constexpr float kNoticeBodyHeightDip = 70.0f;

// Duration page: big time display + three H/M/S wheels.
constexpr float kTimeDisplayTopDip = 52.0f;
constexpr float kTimeDisplayHeightDip = 40.0f;
constexpr float kWheelTopDip = 104.0f;
constexpr float kWheelViewportHeightDip = 132.0f; // exactly three 44 DIP rows
constexpr float kWheelColumnGapDip = 10.0f;
constexpr float kWheelRowHeightDip = 44.0f;
constexpr float kWheelCenterBandHeightDip = 40.0f;
constexpr float kWheelLabelsTopDip = 240.0f;
constexpr float kWheelLabelsHeightDip = 16.0f;
constexpr float kDurationHintTopDip = 260.0f;
constexpr float kDurationHintHeightDip = 14.0f;

constexpr int kWheelColumnCount = 3;
constexpr int kWheelMaxValues[kWheelColumnCount] = {23, 59, 59};
constexpr int kWheelDefaultValues[kWheelColumnCount] = {1, 0, 0};
const wchar_t* const kWheelUnitLabels[kWheelColumnCount] = {L"时", L"分", L"秒"};

enum ControlId
{
    kCtrlNone = 0,
    kCtrlCancel,
    kCtrlStart,
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

std::wstring FormatClock(int hours, int minutes, int seconds)
{
    wchar_t text[16] = {};
    swprintf_s(text, L"%02d:%02d:%02d", hours, minutes, seconds);
    return text;
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
        Duration, // H/M/S wheels + "取消" / "开始记录"
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
    void OnMouseWheel(int delta, POINT point);
    void OnKeyDown(WPARAM key);

    // Duration wheels.
    int WheelValue(int column) const;
    void SetWheelValue(int column, int value);
    void AdjustWheel(int column, int delta);
    void SetActiveColumn(int column);
    int TotalSeconds() const;
    bool StartEnabled() const;

    // Dirty-rect invalidation (device pixels, unioned until the next paint).
    void InvalidateRectDip(const RectF& rect);
    void InvalidateWheelColumn(int column);
    void InvalidateWheelChrome();
    void InvalidateFull();

    // GDI+ surface helpers.
    bool EnsureSurface(int width, int height);
    void DestroySurface();
    bool PresentFrame(const RECT& dirty);
    void ClearFonts();
    Font* GetFont(int pixelSize, bool bold);

    // Drawing.
    void DrawScene(Graphics& graphics);
    void DrawConfirmContent(Graphics& graphics);
    void DrawNoticeContent(Graphics& graphics);
    void DrawDurationContent(Graphics& graphics);
    void DrawTimeDisplay(Graphics& graphics);
    void DrawWheelColumn(Graphics& graphics, int column);
    void DrawWheelLabels(Graphics& graphics);
    void DrawButtons(Graphics& graphics);
    void DrawLabel(Graphics& graphics, const std::wstring& text,
                   const RectF& rect, float sizeDip, bool bold, COLORREF color,
                   StringAlignment align = StringAlignmentNear,
                   StringAlignment lineAlign = StringAlignmentCenter,
                   bool ellipsis = false, bool wrap = false);

    // Layout helpers (device independent pixels -> physical pixels).
    float Px(float dip) const { return dip * m_scale; }
    float WindowWidthDip() const;
    float WindowHeightDip() const;
    float CardHeightDipPx() const;
    RectF CardRectF() const;
    RectF TitleRectF() const;
    RectF SubtitleRectF() const;
    RectF ConfirmBodyRectF() const;
    RectF ConfirmPathRectF() const;
    RectF NoticeBodyRectF() const;
    RectF TimeDisplayRectF() const;
    RectF WheelColumnRectF(int column) const;
    RectF WheelLabelsRectF(int column) const;
    RectF DurationHintRectF() const;
    RectF ButtonRectF(bool start) const;

    int HitTestControl(POINT point) const;
    int HitTestWheelColumn(POINT point) const;

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

    int m_wheelValues[kWheelColumnCount] = {1, 0, 0};
    int m_activeColumn = 0;
    int m_dragColumn = -1;
    POINT m_dragStartPoint = {};
    int m_dragStartValue = 0;

    // Dirty rectangle of the persistent surface, in device pixels.
    RECT m_dirtyRect = {};
    bool m_hasDirty = false;

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
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(kTitleTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kTitleHeightDip));
}

RectF RecordingDialog::SubtitleRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip), card.Y + Px(kSubtitleTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kSubtitleHeightDip));
}

RectF RecordingDialog::ConfirmBodyRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip),
                 card.Y + Px(kConfirmBodyTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kConfirmBodyHeightDip));
}

RectF RecordingDialog::ConfirmPathRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip),
                 card.Y + Px(kConfirmPathTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kConfirmPathHeightDip));
}

RectF RecordingDialog::NoticeBodyRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip),
                 card.Y + Px(kNoticeBodyTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kNoticeBodyHeightDip));
}

RectF RecordingDialog::TimeDisplayRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip),
                 card.Y + Px(kTimeDisplayTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kTimeDisplayHeightDip));
}

RectF RecordingDialog::WheelColumnRectF(int column) const
{
    const RectF card = CardRectF();
    const float contentWidth = card.Width - Px(kContentMarginDip * 2.0f);
    const float columnWidth =
        (contentWidth - Px(kWheelColumnGapDip) * (kWheelColumnCount - 1)) /
        static_cast<float>(kWheelColumnCount);
    const float x = card.X + Px(kContentMarginDip) +
                    static_cast<float>(column) *
                        (columnWidth + Px(kWheelColumnGapDip));
    return RectF(x, card.Y + Px(kWheelTopDip), columnWidth,
                 Px(kWheelViewportHeightDip));
}

RectF RecordingDialog::WheelLabelsRectF(int column) const
{
    const RectF wheel = WheelColumnRectF(column);
    const RectF card = CardRectF();
    return RectF(wheel.X, card.Y + Px(kWheelLabelsTopDip), wheel.Width,
                 Px(kWheelLabelsHeightDip));
}

RectF RecordingDialog::DurationHintRectF() const
{
    const RectF card = CardRectF();
    return RectF(card.X + Px(kContentMarginDip),
                 card.Y + Px(kDurationHintTopDip),
                 card.Width - Px(kContentMarginDip * 2.0f),
                 Px(kDurationHintHeightDip));
}

RectF RecordingDialog::ButtonRectF(bool start) const
{
    const RectF card = CardRectF();
    const float height = Px(kButtonHeightDip);
    const float width = Px(kButtonWidthDip);
    const float y =
        card.Y + card.Height - Px(kButtonBottomMarginDip) - height;
    const float startX = card.X + card.Width - Px(kContentMarginDip) - width;
    const float x = start ? startX : startX - Px(kButtonGapDip) - width;
    return RectF(x, y, width, height);
}

int RecordingDialog::WheelValue(int column) const
{
    if (column < 0 || column >= kWheelColumnCount)
        return 0;
    return ClampInt(m_wheelValues[column], 0, kWheelMaxValues[column]);
}

int RecordingDialog::TotalSeconds() const
{
    return WheelValue(0) * 3600 + WheelValue(1) * 60 + WheelValue(2);
}

bool RecordingDialog::StartEnabled() const
{
    if (m_mode != Mode::Duration)
        return true;
    return TotalSeconds() > 0;
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
    if (StartEnabled() && inside(ButtonRectF(true)))
        return kCtrlStart;
    return kCtrlNone;
}

int RecordingDialog::HitTestWheelColumn(POINT point) const
{
    if (m_mode != Mode::Duration)
        return -1;
    for (int column = 0; column < kWheelColumnCount; ++column)
    {
        const RectF rect = WheelColumnRectF(column);
        // Include the unit label below the wheel so clicking 时/分/秒 selects
        // that column too.
        const RectF labels = WheelLabelsRectF(column);
        const float left = rect.X;
        const float right = rect.X + rect.Width;
        const float top = rect.Y;
        const float bottom = labels.Y + labels.Height;
        if (static_cast<float>(point.x) >= left &&
            static_cast<float>(point.x) < right &&
            static_cast<float>(point.y) >= top &&
            static_cast<float>(point.y) < bottom)
        {
            return column;
        }
    }
    return -1;
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

bool RecordingDialog::Create(HWND owner, Mode mode,
                             RecordingDialogCallback callback, void* context)
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
    m_activeColumn = 0;
    m_dragColumn = -1;
    m_dragStartPoint = {};
    m_dragStartValue = 0;
    for (int column = 0; column < kWheelColumnCount; ++column)
        m_wheelValues[column] = kWheelDefaultValues[column];
    m_dirtyRect = {};
    m_hasDirty = false;

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
    InvalidateFull();
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
    InvalidateFull();
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
// Dirty-rect invalidation
// ---------------------------------------------------------------------------
void RecordingDialog::InvalidateRectDip(const RectF& rect)
{
    if (m_hwnd == nullptr)
        return;
    RECT r = {};
    r.left = ClampInt(RoundToInt(rect.X) - 2, 0, m_widthPx);
    r.top = ClampInt(RoundToInt(rect.Y) - 2, 0, m_heightPx);
    r.right = ClampInt(RoundToInt(rect.X + rect.Width) + 2, 0, m_widthPx);
    r.bottom = ClampInt(RoundToInt(rect.Y + rect.Height) + 2, 0, m_heightPx);
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    if (!m_hasDirty)
    {
        m_dirtyRect = r;
        m_hasDirty = true;
        return;
    }
    m_dirtyRect.left = std::min(m_dirtyRect.left, r.left);
    m_dirtyRect.top = std::min(m_dirtyRect.top, r.top);
    m_dirtyRect.right = std::max(m_dirtyRect.right, r.right);
    m_dirtyRect.bottom = std::max(m_dirtyRect.bottom, r.bottom);
}

void RecordingDialog::InvalidateWheelColumn(int column)
{
    if (column < 0 || column >= kWheelColumnCount)
        return;
    InvalidateRectDip(WheelColumnRectF(column));
    InvalidateRectDip(WheelLabelsRectF(column));
}

void RecordingDialog::InvalidateWheelChrome()
{
    InvalidateRectDip(TimeDisplayRectF());
    InvalidateRectDip(ButtonRectF(true));
}

void RecordingDialog::InvalidateFull()
{
    m_dirtyRect = RECT{0, 0, m_widthPx, m_heightPx};
    m_hasDirty = true;
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

bool RecordingDialog::PresentFrame(const RECT& dirty)
{
    if (m_hwnd == nullptr || m_bitmap == nullptr)
        return false;

    RECT windowRect = {};
    GetWindowRect(m_hwnd, &windowRect);

    const int left = ClampInt(dirty.left, 0, m_surfaceWidth);
    const int top = ClampInt(dirty.top, 0, m_surfaceHeight);
    const int right = ClampInt(dirty.right, 0, m_surfaceWidth);
    const int bottom = ClampInt(dirty.bottom, 0, m_surfaceHeight);
    if (right <= left || bottom <= top)
        return false;

    BitmapData data = {};
    Rect lockRect(0, 0, m_surfaceWidth, m_surfaceHeight);
    if (m_bitmap->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB,
                           &data) != Ok)
        return false;

    const BYTE* source = static_cast<const BYTE*>(data.Scan0);
    BYTE* destination = static_cast<BYTE*>(m_dibPixels);
    const size_t sourceStride = static_cast<size_t>(data.Stride);
    const size_t rowBytes = static_cast<size_t>(m_surfaceWidth) * 4;

    // Only the dirty rectangle is premultiplied into the DIB; the rest of the
    // surface is already up to date from the previous frame.
    for (int y = top; y < bottom; ++y)
    {
        const BYTE* src = source + static_cast<size_t>(y) * sourceStride +
                          static_cast<size_t>(left) * 4;
        BYTE* dst = destination + static_cast<size_t>(y) * rowBytes +
                    static_cast<size_t>(left) * 4;
        for (int x = left; x < right; ++x)
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

    RECT dirty = m_hasDirty ? m_dirtyRect
                            : RECT{0, 0, m_widthPx, m_heightPx};
    m_hasDirty = false;
    m_dirtyRect = RECT{};

    const int left = ClampInt(dirty.left, 0, m_widthPx);
    const int top = ClampInt(dirty.top, 0, m_heightPx);
    const int right = ClampInt(dirty.right, 0, m_widthPx);
    const int bottom = ClampInt(dirty.bottom, 0, m_heightPx);
    if (right <= left || bottom <= top)
        return;

    // Clear only the dirty area (SourceCopy writes transparent pixels).
    m_graphics->SetCompositingMode(CompositingModeSourceCopy);
    {
        SolidBrush clear(Color(0, 0, 0, 0));
        m_graphics->FillRectangle(
            &clear, RectF(static_cast<REAL>(left), static_cast<REAL>(top),
                          static_cast<REAL>(right - left),
                          static_cast<REAL>(bottom - top)));
    }
    m_graphics->SetCompositingMode(CompositingModeSourceOver);

    const GraphicsState state = m_graphics->Save();
    {
        Region clip(Rect(left, top, right - left, bottom - top));
        m_graphics->SetClip(&clip, CombineModeReplace);
        DrawScene(*m_graphics);
    }
    m_graphics->Restore(state);

    PresentFrame(RECT{left, top, right, bottom});
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
    DrawLabel(graphics, subtitle, SubtitleRectF(), 10.0f, false,
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
        L"个别片段转写失败时，音频仅临时保留在系统临时目录供重试，不会写入保存目录。"
        L"关闭窗口或点击取消不会开始。";
    DrawLabel(graphics, body, ConfirmBodyRectF(), 9.5f, false,
              SecondaryTextColor(palette), StringAlignmentNear,
              StringAlignmentNear, false, true);

    const std::wstring path = L"纪要保存到：" + ResolveRecordingSaveDirectory();
    DrawLabel(graphics, path, ConfirmPathRectF(), 9.0f, false,
              TertiaryTextColor(palette), StringAlignmentNear,
              StringAlignmentCenter, true);
}

void RecordingDialog::DrawNoticeContent(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const std::wstring body =
        L"请先在 设置 → 会议与视频纪要 中配置 AI 接口地址和 API Key，"
        L"然后才能开始记录并转写。";
    DrawLabel(graphics, body, NoticeBodyRectF(), 10.0f, false,
              SecondaryTextColor(palette), StringAlignmentNear,
              StringAlignmentCenter, false, true);
}

void RecordingDialog::DrawTimeDisplay(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    const std::wstring text =
        FormatClock(WheelValue(0), WheelValue(1), WheelValue(2));
    DrawLabel(graphics, text, TimeDisplayRectF(), 26.0f, true,
              palette.islandText, StringAlignmentCenter,
              StringAlignmentCenter, false);
}

void RecordingDialog::DrawDurationContent(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();

    DrawTimeDisplay(graphics);

    for (int column = 0; column < kWheelColumnCount; ++column)
        DrawWheelColumn(graphics, column);

    // ":" separators between the wheels.
    for (int gap = 0; gap + 1 < kWheelColumnCount; ++gap)
    {
        const RectF left = WheelColumnRectF(gap);
        const RectF right = WheelColumnRectF(gap + 1);
        const float centerX = (left.X + left.Width + right.X) * 0.5f;
        const float centerY = left.Y + left.Height * 0.5f;
        DrawLabel(graphics, L":",
                  RectF(centerX - Px(6.0f), centerY - Px(12.0f), Px(12.0f),
                        Px(24.0f)),
                  18.0f, true, SecondaryTextColor(palette),
                  StringAlignmentCenter, StringAlignmentCenter, false);
    }

    DrawWheelLabels(graphics);

    DrawLabel(graphics, L"滚轮 / ↑↓ 调整 · 点击列或 Tab 切换",
              DurationHintRectF(), 9.0f, false,
              TertiaryTextColor(palette), StringAlignmentCenter,
              StringAlignmentCenter, false);
}

void RecordingDialog::DrawWheelColumn(Graphics& graphics, int column)
{
    const ThemePalette& palette = GetThemePalette();
    const RectF rect = WheelColumnRectF(column);
    if (rect.Width < 12.0f || rect.Height < 12.0f)
        return;

    // Recessed column background.
    {
        SolidBrush brush(palette.dark ? Color(26, 255, 255, 255)
                                      : Color(16, 0, 0, 0));
        GraphicsPath path;
        AddRoundedRectPath(path, rect, Px(10.0f));
        graphics.FillPath(&brush, &path);
    }

    const bool active = (column == m_activeColumn);

    // Center row highlight band.
    {
        const float bandHeight = Px(kWheelCenterBandHeightDip);
        const RectF band(rect.X + Px(4.0f),
                         rect.Y + (rect.Height - bandHeight) * 0.5f,
                         rect.Width - Px(8.0f), bandHeight);
        const BYTE alpha = active ? (palette.dark ? 58 : 46)
                                  : (palette.dark ? 30 : 24);
        SolidBrush brush(Color(alpha, GetRValue(palette.accent),
                               GetGValue(palette.accent),
                               GetBValue(palette.accent)));
        GraphicsPath path;
        AddRoundedRectPath(path, band, band.Height * 0.5f);
        graphics.FillPath(&brush, &path);

        if (active)
        {
            Pen border(Color(150, GetRValue(palette.accent),
                             GetGValue(palette.accent),
                             GetBValue(palette.accent)),
                       std::max(1.0f, Px(1.0f)));
            GraphicsPath borderPath;
            AddRoundedRectPath(borderPath, rect, Px(10.0f));
            graphics.DrawPath(&border, &borderPath);
        }
    }

    // Clip so nothing can bleed outside the wheel viewport (no stray partial
    // digits at the top/bottom edges).
    const GraphicsState state = graphics.Save();
    {
        GraphicsPath clipPath;
        AddRoundedRectPath(clipPath, rect, Px(10.0f));
        Region clipRegion(&clipPath);
        graphics.SetClip(&clipRegion, CombineModeReplace);
    }

    const int value = WheelValue(column);
    const int maximum = kWheelMaxValues[column];
    const float centerY = rect.Y + rect.Height * 0.5f;
    const float step = Px(kWheelRowHeightDip);

    for (int offset = -1; offset <= 1; ++offset)
    {
        int display = value + offset;
        const int span = maximum + 1;
        display = ((display % span) + span) % span;

        const bool center = (offset == 0);
        const float y = centerY + static_cast<float>(offset) * step;
        wchar_t text[8] = {};
        swprintf_s(text, L"%02d", display);

        COLORREF color = center ? palette.islandText
                                : TertiaryTextColor(palette);
        const BYTE alpha = center ? 255 : (palette.dark ? 170 : 190);
        Font* font = GetFont(std::max(6, RoundToInt(Px(center ? 23.0f : 15.0f))),
                             center);
        StringFormat format(StringFormat::GenericTypographic());
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush brush(Color(alpha, GetRValue(color), GetGValue(color),
                               GetBValue(color)));
        const RectF textRect(rect.X, y - step * 0.5f, rect.Width, step);
        graphics.DrawString(text, -1, font, textRect, &format, &brush);
    }

    graphics.Restore(state);
}

void RecordingDialog::DrawWheelLabels(Graphics& graphics)
{
    const ThemePalette& palette = GetThemePalette();
    for (int column = 0; column < kWheelColumnCount; ++column)
    {
        const bool active = (column == m_activeColumn);
        const COLORREF color = active ? palette.islandText
                                      : SecondaryTextColor(palette);
        DrawLabel(graphics, kWheelUnitLabels[column],
                  WheelLabelsRectF(column), 10.0f, active, color,
                  StringAlignmentCenter, StringAlignmentCenter, false);
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
        Pen border(GdiColor(NeutralButtonBorder(palette)),
                   std::max(1.0f, Px(1.0f)));
        graphics.DrawPath(&border, &path);
        DrawLabel(graphics, L"取消", rect, 11.5f, false, palette.islandText,
                  StringAlignmentCenter, StringAlignmentCenter, false);
    }

    // Accent (primary) button; visibly disabled while the duration is 0.
    {
        const RectF rect = ButtonRectF(true);
        const bool enabled = StartEnabled();
        COLORREF fill = palette.accent;
        if (!enabled)
        {
            fill = BlendColor(palette.accent, palette.islandBackground, 155);
        }
        else if (m_pressedControl == kCtrlStart)
        {
            fill = BlendColor(palette.accent, RGB(0, 0, 0), 30);
        }
        else if (m_hoveredControl == kCtrlStart)
        {
            fill = BlendColor(palette.accent, RGB(255, 255, 255), 22);
        }

        SolidBrush brush(GdiColor(fill));
        GraphicsPath path;
        AddRoundedRectPath(path, rect, Px(10.0f));
        graphics.FillPath(&brush, &path);
        const wchar_t* label = m_mode == Mode::Duration
                                   ? L"开始记录"
                                   : (m_mode == Mode::Notice ? L"打开设置"
                                                             : L"开始");
        const COLORREF textColor =
            enabled ? palette.accentText : SecondaryTextColor(palette);
        DrawLabel(graphics, label, rect, 11.5f, true, textColor,
                  StringAlignmentCenter, StringAlignmentCenter, false);
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void RecordingDialog::UpdateHoverFromPoint(POINT point)
{
    const int control = HitTestControl(point);
    if (control != m_hoveredControl)
    {
        const int previous = m_hoveredControl;
        m_hoveredControl = control;
        if (previous != kCtrlNone)
            InvalidateRectDip(ButtonRectF(previous == kCtrlStart));
        if (control != kCtrlNone)
            InvalidateRectDip(ButtonRectF(control == kCtrlStart));
        PaintNow();
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

    if (m_dragColumn >= 0)
    {
        const float step = Px(kWheelRowHeightDip);
        if (step > 0.5f)
        {
            const int deltaRows =
                RoundToInt((static_cast<float>(m_dragStartPoint.y) -
                            static_cast<float>(point.y)) /
                           step);
            const int value = ClampInt(m_dragStartValue + deltaRows, 0,
                                       kWheelMaxValues[m_dragColumn]);
            SetWheelValue(m_dragColumn, value);
        }
        return;
    }

    UpdateHoverFromPoint(point);
}

void RecordingDialog::OnMouseLeave()
{
    m_trackingMouse = false;
    if (m_hoveredControl != kCtrlNone)
    {
        const int previous = m_hoveredControl;
        m_hoveredControl = kCtrlNone;
        InvalidateRectDip(ButtonRectF(previous == kCtrlStart));
        PaintNow();
    }
}

void RecordingDialog::OnLButtonDown(POINT point)
{
    const int control = HitTestControl(point);
    if (control != kCtrlNone)
    {
        m_pressedControl = control;
        SetCapture(m_hwnd);
        InvalidateRectDip(ButtonRectF(control == kCtrlStart));
        PaintNow();
        return;
    }

    const int column = HitTestWheelColumn(point);
    if (column >= 0)
    {
        SetActiveColumn(column);
        m_dragColumn = column;
        m_dragStartPoint = point;
        m_dragStartValue = WheelValue(column);
        SetCapture(m_hwnd);
        InvalidateWheelColumn(column);
        PaintNow();
    }
}

void RecordingDialog::OnLButtonUp(POINT point)
{
    if (m_dragColumn >= 0)
    {
        const int column = m_dragColumn;
        m_dragColumn = -1;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();

        // A plain click (no drag) on a neighbouring row scrolls that row into
        // the centre; clicking the centre row itself does nothing.
        const float step = Px(kWheelRowHeightDip);
        if (step > 0.5f)
        {
            const int draggedRows = RoundToInt(
                (static_cast<float>(m_dragStartPoint.y) -
                 static_cast<float>(point.y)) /
                step);
            if (draggedRows == 0)
            {
                const RectF rect = WheelColumnRectF(column);
                const float centerY = rect.Y + rect.Height * 0.5f;
                int rowOffset = RoundToInt(
                    (static_cast<float>(point.y) - centerY) / step);
                rowOffset = ClampInt(rowOffset, -1, 1);
                if (rowOffset != 0)
                {
                    const int span = kWheelMaxValues[column] + 1;
                    int value = (WheelValue(column) + rowOffset) % span;
                    if (value < 0)
                        value += span;
                    m_wheelValues[column] = value;
                }
            }
        }

        InvalidateWheelColumn(column);
        InvalidateWheelChrome();
        PaintNow();
        return;
    }

    // Read the pressed control BEFORE releasing capture: ReleaseCapture sends
    // WM_CAPTURECHANGED synchronously, whose handler clears m_pressedControl.
    const int pressed = m_pressedControl;
    m_pressedControl = kCtrlNone;

    if (GetCapture() == m_hwnd)
        ReleaseCapture();

    InvalidateRectDip(ButtonRectF(false));
    InvalidateRectDip(ButtonRectF(true));
    PaintNow();

    if (pressed == kCtrlNone)
        return;

    const int released = HitTestControl(point);
    if (pressed != released)
        return;

    switch (pressed)
    {
    case kCtrlCancel:
        FinishCancel();
        break;
    case kCtrlStart:
        if (m_mode == Mode::Duration)
        {
            if (StartEnabled())
                FinishAction(TotalSeconds());
        }
        else if (m_mode == Mode::Notice)
        {
            FinishAction(-2); // caller opens the settings window
        }
        else
        {
            FinishAction(-1); // caller continues with the duration step
        }
        break;
    default:
        break;
    }
}

void RecordingDialog::OnMouseWheel(int delta, POINT point)
{
    if (m_mode != Mode::Duration || delta == 0)
        return;

    int column = HitTestWheelColumn(point);
    if (column < 0)
        column = m_activeColumn;
    SetActiveColumn(column);
    AdjustWheel(column, delta > 0 ? 1 : -1);
}

void RecordingDialog::OnKeyDown(WPARAM key)
{
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    switch (key)
    {
    case VK_ESCAPE:
        FinishCancel();
        break;
    case VK_RETURN:
        if (m_mode == Mode::Duration)
        {
            if (StartEnabled())
                FinishAction(TotalSeconds());
        }
        else if (m_mode == Mode::Notice)
        {
            FinishAction(-2);
        }
        else
        {
            FinishAction(-1);
        }
        break;
    case VK_UP:
        if (m_mode == Mode::Duration)
            AdjustWheel(m_activeColumn, 1);
        break;
    case VK_DOWN:
        if (m_mode == Mode::Duration)
            AdjustWheel(m_activeColumn, -1);
        break;
    case VK_LEFT:
        if (m_mode == Mode::Duration)
            SetActiveColumn(ClampInt(m_activeColumn - 1, 0, kWheelColumnCount - 1));
        break;
    case VK_RIGHT:
        if (m_mode == Mode::Duration)
            SetActiveColumn(ClampInt(m_activeColumn + 1, 0, kWheelColumnCount - 1));
        break;
    case VK_TAB:
        if (m_mode == Mode::Duration)
        {
            const int next = shift ? (m_activeColumn + kWheelColumnCount - 1) %
                                         kWheelColumnCount
                                   : (m_activeColumn + 1) % kWheelColumnCount;
            SetActiveColumn(next);
        }
        break;
    default:
        break;
    }
}

void RecordingDialog::SetWheelValue(int column, int value)
{
    if (column < 0 || column >= kWheelColumnCount)
        return;
    const int clamped = ClampInt(value, 0, kWheelMaxValues[column]);
    if (clamped == m_wheelValues[column])
        return;

    m_wheelValues[column] = clamped;
    InvalidateWheelColumn(column);
    InvalidateWheelChrome();
    PaintNow();
}

void RecordingDialog::AdjustWheel(int column, int delta)
{
    if (column < 0 || column >= kWheelColumnCount)
        return;
    SetWheelValue(column, WheelValue(column) + delta);
}

void RecordingDialog::SetActiveColumn(int column)
{
    if (column < 0 || column >= kWheelColumnCount || column == m_activeColumn)
        return;
    const int previous = m_activeColumn;
    m_activeColumn = column;
    InvalidateWheelColumn(previous);
    InvalidateWheelColumn(column);
    PaintNow();
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
            InvalidateRectDip(ButtonRectF(false));
            InvalidateRectDip(ButtonRectF(true));
            PaintNow();
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(m_hwnd, &point);
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), point);
        return 0;
    }

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(m_hwnd, &point);
            const bool interactive = HitTestControl(point) != kCtrlNone ||
                                     HitTestWheelColumn(point) >= 0;
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
        InvalidateFull();
        PaintNow();
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
    InvalidateFull();
    PaintNow();
}

void RecordingDialog::OnDestroy()
{
    m_callback = nullptr;
    m_context = nullptr;
    m_hoveredControl = kCtrlNone;
    m_pressedControl = kCtrlNone;
    m_trackingMouse = false;
    m_dragColumn = -1;

    if (m_hwnd != nullptr && GetCapture() == m_hwnd)
        ReleaseCapture();

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
