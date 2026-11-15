// ===========================================================================
//  src/screensaver.cpp  --  v5.5 built-in rest reminder / screensaver
//
//  Two lightweight layered windows:
//    * rest notice: a compact card below the island with a 10 s countdown and
//      "立即休息" / "暂不休息" buttons (never steals focus),
//    * fullscreen overlay: a topmost per-pixel-alpha window covering the whole
//      virtual desktop with a famous quote on a procedural background
//      (landscape / solid / gradient), a progress bar + remaining time at the
//      bottom of the primary monitor, a close button and an "extend" button.
//
//  The overlay never suspends or pauses other processes - it only covers the
//  screen. The background is rendered once into a cached bitmap; the once per
//  second progress update restores only the progress area from that cache and
//  presents just that dirty rectangle (UpdateLayeredWindowIndirect), so the
//  landscape is never re-rendered every second.
//
//  The module is driven by the island's 200 ms content timer
//  (ScreensaverOnTick) and reads its options from g_settings.
// ===========================================================================

#include "screensaver.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "actions.h"
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
// Constants
// ---------------------------------------------------------------------------
constexpr wchar_t kNoticeClass[] = L"DynamicIslandScreensaverNotice";
constexpr wchar_t kOverlayClass[] = L"DynamicIslandScreensaverOverlay";

constexpr UINT_PTR kFadeTimerId = 1;
constexpr UINT_PTR kClockTimerId = 2;
constexpr UINT kFadeIntervalMs = 16;
constexpr UINT kClockIntervalMs = 250;
constexpr UINT kFadeDurationMs = 340;

constexpr ULONGLONG kGameBlockPostponeSeconds = 5 * 60; // silently retry later

// Notice card geometry (device independent pixels @ 96 DPI).
constexpr float kNoticeCardWidthDip = 372.0f;
constexpr float kNoticeCardHeightDip = 124.0f;
constexpr float kNoticeShadowDip = 16.0f;
constexpr float kNoticeRadiusDip = 16.0f;
constexpr float kNoticeButtonWidthDip = 100.0f;
constexpr float kNoticeButtonHeightDip = 30.0f;
constexpr float kNoticeButtonGapDip = 10.0f;
constexpr float kNoticeContentMarginDip = 20.0f;

// Fullscreen overlay chrome.
constexpr float kBarWidthDip = 420.0f;
constexpr float kBarHeightDip = 6.0f;
constexpr float kExtendWidthDip = 156.0f;
constexpr float kExtendHeightDip = 30.0f;
constexpr float kSkipWidthDip = 54.0f;
constexpr float kSkipHeightDip = 24.0f;
constexpr float kSkipGapDip = 8.0f;

// Date / time block margins per background.
constexpr float kLandscapeTimeMarginDip = 56.0f;
constexpr float kSolidTimeTopDip = 46.0f;
constexpr float kGradientTimeMarginDip = 56.0f;

constexpr REAL kPi = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Quote bank (24 quotes: Chinese classics and translated / original foreign)
// ---------------------------------------------------------------------------
struct QuoteInfo
{
    const wchar_t* text;
    const wchar_t* author;
};

const QuoteInfo kQuotes[] = {
    {L"学而不思则罔，思而不学则殆。", L"孔子"},
    {L"上善若水，水善利万物而不争。", L"老子"},
    {L"吾生也有涯，而知也无涯。", L"庄子"},
    {L"天将降大任于斯人也，必先苦其心志，劳其筋骨。", L"孟子"},
    {L"不积跬步，无以至千里；不积小流，无以成江海。", L"荀子"},
    {L"知是行之始，行是知之成。", L"王阳明"},
    {L"非淡泊无以明志，非宁静无以致远。", L"诸葛亮"},
    {L"不识庐山真面目，只缘身在此山中。", L"苏轼"},
    {L"其实地上本没有路，走的人多了，也便成了路。", L"鲁迅"},
    {L"落霞与孤鹜齐飞，秋水共长天一色。", L"王勃"},
    {L"想象力比知识更重要。", L"爱因斯坦"},
    {L"如果说我看得比别人更远些，那是因为我站在巨人的肩膀上。", L"牛顿"},
    {L"生活中没有什么可怕的东西，只有需要理解的东西。", L"居里夫人"},
    {L"世界上只有一种真正的英雄主义，那就是在认清生活真相之后依然热爱生活。",
     L"罗曼·罗兰"},
    {L"生如夏花之绚烂，死如秋叶之静美。", L"泰戈尔"},
    {L"一个人只要知道自己为什么而活，就可以忍受任何一种生活。", L"尼采"},
    {L"我步入丛林，因为我希望生活得有意义，活得深刻。", L"梭罗"},
    {L"简单是终极的复杂。", L"达·芬奇"},
    {L"世界上最宽阔的是海洋，比海洋更宽阔的是天空，比天空更宽阔的是人的胸怀。",
     L"雨果"},
    {L"认识你自己。", L"苏格拉底"},
    {L"人生而自由，却无往不在枷锁之中。", L"卢梭"},
    {L"To be, or not to be, that is the question.", L"William Shakespeare"},
    {L"Reading maketh a full man; conference a ready man; and writing an exact man.",
     L"Francis Bacon"},
    {L"The unexamined life is not worth living.", L"Socrates"},
};
constexpr int kQuoteCount =
    static_cast<int>(sizeof(kQuotes) / sizeof(kQuotes[0]));

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
HWND g_owner = nullptr;
bool g_initialised = false;
bool g_testMode = false;
int g_testDurationSeconds = 0;
int g_backgroundOverride = -1;
bool g_scheduled = false;
ULONGLONG g_nextEntryTick = 0;
unsigned int g_rng = 0x9E3779B9u;

ULONGLONG NowMs()
{
    return GetTickCount64();
}

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

float ClampFloat(float value, float minimum, float maximum)
{
    if (!(value > minimum))
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
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

RECT GetVirtualScreenRect()
{
    RECT rect = {};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        rect.left = 0;
        rect.top = 0;
        rect.right = GetSystemMetrics(SM_CXSCREEN);
        rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    return rect;
}

RECT GetPrimaryMonitorRect()
{
    POINT origin = {0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info))
        return info.rcMonitor;
    RECT fallback = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN)};
    return fallback;
}

RECT GetPrimaryWorkArea()
{
    POINT origin = {0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info))
        return info.rcWork;
    return GetVirtualScreenRect();
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

// Soft radial glow built from many concentric ellipses (smooth in GDI+ where
// PathGradientBrush shows visible banding on large areas).
void DrawSoftGlow(Graphics& graphics, const PointF& center, REAL radiusX,
                  REAL radiusY, COLORREF color, int centerAlpha, int steps = 48)
{
    if (centerAlpha <= 0 || radiusX <= 1.0f || radiusY <= 1.0f)
        return;
    const int alpha = std::max(1, RoundToInt(static_cast<float>(centerAlpha) *
                                             1.6f / static_cast<float>(steps)));
    for (int i = steps; i >= 1; --i)
    {
        const REAL t = static_cast<REAL>(i) / static_cast<REAL>(steps);
        const REAL rx = radiusX * t;
        const REAL ry = radiusY * t;
        SolidBrush brush(GdiColor(color, static_cast<BYTE>(alpha)));
        graphics.FillEllipse(&brush,
                             RectF(center.X - rx, center.Y - ry, rx * 2.0f,
                                   ry * 2.0f));
    }
}

bool HasCjk(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        if (static_cast<unsigned int>(ch) >= 0x2E80u)
            return true;
    }
    return false;
}

std::wstring FormatClock(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    const int minutes = seconds / 60;
    const int remaining = seconds % 60;
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02d:%02d", minutes, remaining);
    return buffer;
}

std::wstring ToWString(int value)
{
    return std::to_wstring(value);
}

// Cheap "has the displayed minute changed" key (year/month/day/hour/minute).
ULONGLONG CurrentMinuteKey()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    const ULONGLONG day =
        (static_cast<ULONGLONG>(now.wYear) * 13ULL + now.wMonth) * 32ULL +
        now.wDay;
    return (day * 24ULL + now.wHour) * 60ULL + now.wMinute;
}

// ---------------------------------------------------------------------------
// Quote font families (created once, released in ShutdownScreensaver)
// ---------------------------------------------------------------------------
FontFamily* g_serifCjk = nullptr;
FontFamily* g_serifLatin = nullptr;

FontFamily* PickFamily(const wchar_t* const* candidates, int count)
{
    for (int i = 0; i < count; ++i)
    {
        FontFamily* family = new FontFamily(candidates[i]);
        if (family->GetLastStatus() == Ok && family->IsAvailable())
            return family;
        delete family;
    }
    return new FontFamily(L"Arial");
}

void EnsureFamilies()
{
    if (g_serifCjk == nullptr)
    {
        const wchar_t* const candidates[] = {L"SimSun", L"NSimSun",
                                             L"Microsoft YaHei UI",
                                             L"Microsoft YaHei", L"KaiTi"};
        g_serifCjk = PickFamily(candidates, 5);
    }
    if (g_serifLatin == nullptr)
    {
        const wchar_t* const candidates[] = {L"Georgia", L"Times New Roman",
                                             L"Cambria", L"Segoe UI", L"Arial"};
        g_serifLatin = PickFamily(candidates, 5);
    }
}

void ReleaseFamilies()
{
    if (g_serifCjk != nullptr)
    {
        delete g_serifCjk;
        g_serifCjk = nullptr;
    }
    if (g_serifLatin != nullptr)
    {
        delete g_serifLatin;
        g_serifLatin = nullptr;
    }
}

struct CachedFont
{
    Font* font = nullptr;
    FontFamily* family = nullptr;
    int pixelSize = 0;
    INT style = 0;
};

Font* FindFont(std::vector<CachedFont>& fonts, FontFamily* family, int pixelSize,
               bool bold, bool italic)
{
    pixelSize = std::max(5, pixelSize);
    const INT wantedStyle =
        (bold ? FontStyleBold : 0) | (italic ? FontStyleItalic : 0);
    for (const CachedFont& entry : fonts)
    {
        if (entry.family == family && entry.pixelSize == pixelSize &&
            entry.style == wantedStyle)
            return entry.font;
    }

    Font* font = new Font(family, static_cast<REAL>(pixelSize), wantedStyle,
                          UnitPixel);
    if (font->GetLastStatus() != Ok)
    {
        delete font;
        font = new Font(L"Microsoft YaHei", static_cast<REAL>(pixelSize),
                        wantedStyle, UnitPixel);
    }
    CachedFont entry;
    entry.font = font;
    entry.family = family;
    entry.pixelSize = pixelSize;
    entry.style = wantedStyle;
    fonts.push_back(entry);
    return font;
}

void DeleteFonts(std::vector<CachedFont>& fonts)
{
    for (const CachedFont& entry : fonts)
        delete entry.font;
    fonts.clear();
}

unsigned int NextRandom()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

int RandomInt(int maximumExclusive)
{
    if (maximumExclusive <= 1)
        return 0;
    return static_cast<int>(NextRandom() % static_cast<unsigned int>(maximumExclusive));
}

int PickQuoteIndex(int previous)
{
    // The quote bank always has more than one entry (guarded by the compiler
    // when the table above is edited).
    int index = RandomInt(kQuoteCount);
    if (index == previous)
        index = (index + 1 + RandomInt(kQuoteCount - 1)) % kQuoteCount;
    return index;
}

// ---------------------------------------------------------------------------
// Effective settings (with the internal test hooks on top)
// ---------------------------------------------------------------------------
int IntervalMinutes()
{
    return ClampInt(g_settings.screensaverIntervalMinutes,
                    kScreensaverIntervalMinMinutes,
                    kScreensaverIntervalMaxMinutes);
}

int DurationSeconds()
{
    if (g_testDurationSeconds > 0)
        return ClampInt(g_testDurationSeconds, 3, 3600);
    return ClampInt(g_settings.screensaverDurationSeconds,
                    kScreensaverDurationMinSeconds,
                    kScreensaverDurationMaxSeconds);
}

int PostponeMinutes()
{
    return ClampInt(g_settings.screensaverPostponeMinutes,
                    kScreensaverPostponeMinMinutes,
                    kScreensaverPostponeMaxMinutes);
}

int ExtendMinutes()
{
    return ClampInt(g_settings.screensaverExtendMinutes,
                    kScreensaverExtendMinMinutes,
                    kScreensaverExtendMaxMinutes);
}

int BackgroundMode()
{
    if (g_backgroundOverride >= 0)
        return ClampInt(g_backgroundOverride, 0, SCREENSAVER_BG_COUNT - 1);
    return ClampInt(g_settings.screensaverBackground, 0,
                    SCREENSAVER_BG_COUNT - 1);
}

bool IsGameBlocking()
{
    return g_settings.screensaverBlockInGame && IsGameFullscreenActive();
}

// ---------------------------------------------------------------------------
// Forward declarations shared by the two windows
// ---------------------------------------------------------------------------
void EnterOverlayNow();
void ExitOverlayNow();
void PostponeNoticeFromUi();

// ---------------------------------------------------------------------------
// Rest notice (10 s countdown card below the island)
// ---------------------------------------------------------------------------
class RestNotice
{
public:
    bool Create(HWND owner, int seconds);
    void Destroy();
    bool IsVisible() const { return m_hwnd != nullptr; }
    void UpdateCountdown(int seconds);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureClass();
    void ComputeMetrics();
    void PositionWindow();
    bool EnsureSurface();
    void ReleaseSurface();
    void Render();
    void Present();
    void DrawText(Graphics& graphics, const std::wstring& text,
                  const RectF& rect, int pixelSize, bool bold, Color color,
                  StringAlignment align, StringAlignment lineAlign,
                  bool ellipsis);
    void DrawButton(Graphics& graphics, const RectF& rect,
                    const std::wstring& label, bool accent, bool hovered,
                    bool pressed);
    RECT CardRectPx() const;
    RECT AcceptRectPx() const;
    RECT PostponeRectPx() const;
    int HitTest(POINT point) const;
    void UpdateHover(POINT point);

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    int m_seconds = kScreensaverNoticeSeconds;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    int m_widthPx = 0;
    int m_heightPx = 0;
    int m_hovered = 0; // 0 none, 1 accept, 2 postpone
    int m_pressed = 0;
    bool m_trackingMouse = false;

    Bitmap* m_bitmap = nullptr;
    Graphics* m_graphics = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_previousDib = nullptr;
    void* m_dibPixels = nullptr;
    std::vector<CachedFont> m_fonts;
};

bool RestNotice::EnsureClass()
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &RestNotice::WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kNoticeClass;
    registered = RegisterClassExW(&windowClass) != 0;
    if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        registered = true;
    return registered;
}

void RestNotice::ComputeMetrics()
{
    m_dpi = m_owner != nullptr ? QueryDpi(m_owner) : 96;
    if (m_dpi == 0)
        m_dpi = 96;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
    m_widthPx = std::max(64, RoundToInt(m_scale * (kNoticeCardWidthDip +
                                                   kNoticeShadowDip * 2.0f)));
    m_heightPx = std::max(64, RoundToInt(m_scale * (kNoticeCardHeightDip +
                                                    kNoticeShadowDip * 2.0f)));
}

void RestNotice::PositionWindow()
{
    if (m_hwnd == nullptr)
        return;

    const RECT capsule = GetIslandRestingCapsuleRect();
    const int gap = RoundToInt(10.0f * m_scale);
    const int shadow = RoundToInt(kNoticeShadowDip * m_scale);
    int x = capsule.left + (capsule.right - capsule.left) / 2 - m_widthPx / 2;
    int y = capsule.bottom + gap - shadow;

    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetPrimaryWorkArea();
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

bool RestNotice::EnsureSurface()
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

void RestNotice::ReleaseSurface()
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

void RestNotice::DrawText(Graphics& graphics, const std::wstring& text,
                          const RectF& rect, int pixelSize, bool bold,
                          Color color, StringAlignment align,
                          StringAlignment lineAlign, bool ellipsis)
{
    if (text.empty())
        return;
    FontFamily* family = g_serifCjk;
    if (family == nullptr)
        family = g_serifLatin;
    Font* font = FindFont(m_fonts, family, pixelSize, bold, false);
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(lineAlign);
    format.SetTrimming(ellipsis ? StringTrimmingEllipsisCharacter
                                : StringTrimmingNone);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(color);
    graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font, rect,
                        &format, &brush);
}

void RestNotice::DrawButton(Graphics& graphics, const RectF& rect,
                            const std::wstring& label, bool accent,
                            bool hovered, bool pressed)
{
    const ThemePalette& palette = GetThemePalette();
    COLORREF fill = palette.accent;
    COLORREF textColor = palette.accentText;
    if (!accent)
    {
        const COLORREF tint = palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        int weight = palette.dark ? 24 : 14;
        if (hovered)
            weight += palette.dark ? 16 : 12;
        if (pressed)
            weight += 8;
        fill = BlendColor(palette.islandBackground, tint, weight);
        textColor = palette.islandText;
    }
    else
    {
        if (pressed)
            fill = BlendColor(palette.accent, RGB(0, 0, 0), 32);
        else if (hovered)
            fill = BlendColor(palette.accent, RGB(255, 255, 255), 22);
    }

    SolidBrush brush(GdiColor(fill));
    GraphicsPath path;
    AddRoundedRectPath(path, rect, rect.Height * 0.5f);
    graphics.FillPath(&brush, &path);
    if (!accent)
    {
        Pen border(GdiColor(BlendColor(palette.islandText, palette.islandBackground,
                                       200)),
                   std::max(1.0f, m_scale));
        graphics.DrawPath(&border, &path);
    }

    Font* font = FindFont(m_fonts, g_serifCjk != nullptr ? g_serifCjk
                                                         : g_serifLatin,
                          std::max(6, RoundToInt(12.0f * m_scale)), true, false);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush textBrush(GdiColor(textColor));
    graphics.DrawString(label.c_str(), static_cast<INT>(label.length()), font,
                        rect, &format, &textBrush);
}

RECT RestNotice::CardRectPx() const
{
    RECT rect = {RoundToInt(kNoticeShadowDip * m_scale),
                 RoundToInt(kNoticeShadowDip * m_scale),
                 RoundToInt((kNoticeShadowDip + kNoticeCardWidthDip) * m_scale),
                 RoundToInt((kNoticeShadowDip + kNoticeCardHeightDip) * m_scale)};
    return rect;
}

RECT RestNotice::AcceptRectPx() const
{
    const RECT card = CardRectPx();
    const int width = RoundToInt(kNoticeButtonWidthDip * m_scale);
    const int height = RoundToInt(kNoticeButtonHeightDip * m_scale);
    const int margin = RoundToInt(16.0f * m_scale);
    const int bottom = RoundToInt(14.0f * m_scale);
    RECT rect = {};
    rect.right = card.right - margin;
    rect.left = rect.right - width;
    rect.bottom = card.bottom - bottom;
    rect.top = rect.bottom - height;
    return rect;
}

RECT RestNotice::PostponeRectPx() const
{
    const RECT accept = AcceptRectPx();
    const int width = RoundToInt(kNoticeButtonWidthDip * m_scale);
    const int gap = RoundToInt(kNoticeButtonGapDip * m_scale);
    RECT rect = accept;
    rect.right = accept.left - gap;
    rect.left = rect.right - width;
    return rect;
}

void RestNotice::Render()
{
    if (m_graphics == nullptr)
        return;

    const ThemePalette& palette = GetThemePalette();
    m_graphics->Clear(Color(0, 0, 0, 0));

    const RECT card = CardRectPx();
    const RectF cardRect(static_cast<REAL>(card.left), static_cast<REAL>(card.top),
                         static_cast<REAL>(card.right - card.left),
                         static_cast<REAL>(card.bottom - card.top));
    RenderIslandMaterialRect(*m_graphics, cardRect, kNoticeRadiusDip, m_dpi,
                             g_settings.opacity);

    const COLORREF secondary =
        BlendColor(palette.islandText, palette.islandBackground, 110);
    const REAL margin = static_cast<REAL>(RoundToInt(kNoticeContentMarginDip *
                                                     m_scale));

    DrawText(*m_graphics, L"该休息一下了",
             RectF(cardRect.X + margin, cardRect.Y + RoundToInt(14.0f * m_scale),
                   cardRect.Width - margin * 2.0f,
                   static_cast<REAL>(RoundToInt(22.0f * m_scale))),
             std::max(6, RoundToInt(14.5f * m_scale)), true,
             GdiColor(palette.islandText), StringAlignmentNear,
             StringAlignmentCenter, true);

    const std::wstring countdown =
        L"还有 " + ToWString(std::max(0, m_seconds)) + L" 秒进入屏保";
    DrawText(*m_graphics, countdown,
             RectF(cardRect.X + margin, cardRect.Y + RoundToInt(40.0f * m_scale),
                   cardRect.Width - margin * 2.0f,
                   static_cast<REAL>(RoundToInt(18.0f * m_scale))),
             std::max(6, RoundToInt(10.5f * m_scale)), false, GdiColor(secondary),
             StringAlignmentNear, StringAlignmentCenter, true);

    const RECT accept = AcceptRectPx();
    const RECT postpone = PostponeRectPx();
    DrawButton(*m_graphics,
               RectF(static_cast<REAL>(postpone.left),
                     static_cast<REAL>(postpone.top),
                     static_cast<REAL>(postpone.right - postpone.left),
                     static_cast<REAL>(postpone.bottom - postpone.top)),
               L"暂不休息", false, m_hovered == 2, m_pressed == 2);
    DrawButton(*m_graphics,
               RectF(static_cast<REAL>(accept.left),
                     static_cast<REAL>(accept.top),
                     static_cast<REAL>(accept.right - accept.left),
                     static_cast<REAL>(accept.bottom - accept.top)),
               L"立即休息", true, m_hovered == 1, m_pressed == 1);
}

void RestNotice::Present()
{
    if (m_hwnd == nullptr || m_bitmap == nullptr || m_memoryDc == nullptr)
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

void RestNotice::UpdateHover(POINT point)
{
    const int hit = HitTest(point);
    if (hit != m_hovered)
    {
        m_hovered = hit;
        Render();
        Present();
    }
}

int RestNotice::HitTest(POINT point) const
{
    const auto inside = [&point](const RECT& rect) {
        return point.x >= rect.left && point.x < rect.right &&
               point.y >= rect.top && point.y < rect.bottom;
    };
    if (inside(AcceptRectPx()))
        return 1;
    if (inside(PostponeRectPx()))
        return 2;
    return 0;
}

bool RestNotice::Create(HWND owner, int seconds)
{
    if (m_hwnd != nullptr)
    {
        UpdateCountdown(seconds);
        return true;
    }
    if (!EnsureClass())
        return false;

    m_owner = owner;
    m_seconds = std::max(0, seconds);
    m_hovered = 0;
    m_pressed = 0;
    m_trackingMouse = false;
    ComputeMetrics();

    const RECT capsule = GetIslandRestingCapsuleRect();
    const int gap = RoundToInt(10.0f * m_scale);
    const int shadow = RoundToInt(kNoticeShadowDip * m_scale);
    int x = capsule.left + (capsule.right - capsule.left) / 2 - m_widthPx / 2;
    int y = capsule.bottom + gap - shadow;

    HMONITOR monitor = MonitorFromRect(&capsule, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetPrimaryWorkArea();
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
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kNoticeClass, L"休息提醒", WS_POPUP, x, y, m_widthPx, m_heightPx, owner,
        nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr)
    {
        m_hwnd = nullptr;
        ReleaseSurface();
        return false;
    }

    EnsureSurface();
    m_graphics->Clear(Color(0, 0, 0, 0));
    Render();
    Present();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void RestNotice::UpdateCountdown(int seconds)
{
    seconds = std::max(0, seconds);
    if (seconds == m_seconds)
        return;
    m_seconds = seconds;
    if (m_hwnd == nullptr)
        return;
    Render();
    Present();
}

void RestNotice::Destroy()
{
    if (m_hwnd != nullptr)
    {
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwnd);
    }
    ReleaseSurface();
    DeleteFonts(m_fonts);
}

LRESULT CALLBACK RestNotice::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                        LPARAM lParam)
{
    RestNotice* self = reinterpret_cast<RestNotice*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RestNotice*>(create->lpCreateParams);
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

LRESULT RestNotice::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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
        m_trackingMouse = false;
        if (m_hovered != 0)
        {
            m_hovered = 0;
            Render();
            Present();
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        m_pressed = HitTest(point);
        if (m_pressed != 0)
        {
            SetCapture(m_hwnd);
            Render();
            Present();
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int pressed = m_pressed;
        m_pressed = 0;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        const int hit = HitTest(point);
        if (pressed != 0 && pressed == hit)
        {
            if (hit == 1)
            {
                EnterOverlayNow();
                return 0; // this window has been destroyed
            }
            if (hit == 2)
            {
                PostponeNoticeFromUi();
                return 0;
            }
        }
        Render();
        Present();
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (m_pressed != 0)
        {
            m_pressed = 0;
            Render();
            Present();
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(m_hwnd, &point);
            SetCursor(LoadCursorW(nullptr, HitTest(point) != 0 ? IDC_HAND
                                                               : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_CLOSE:
        Destroy();
        return 0;

    case WM_DESTROY:
        KillTimer(m_hwnd, kClockTimerId);
        ReleaseSurface();
        DeleteFonts(m_fonts);
        m_hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Fullscreen overlay
// ---------------------------------------------------------------------------
class RestOverlay
{
public:
    bool Create(HWND owner, int backgroundMode);
    void Destroy();
    bool IsActive() const { return m_hwnd != nullptr; }
    void SetBackgroundMode(int backgroundMode);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureClass();
    void ComputeMetrics();
    bool EnsureSurface();
    void ReleaseResources();
    void RenderAll();
    void RenderBackground();
    void ComposeFrame();
    void DrawLandscape(Graphics& graphics);
    void DrawSolid(Graphics& graphics);
    void DrawGradient(Graphics& graphics);
    void DrawVignette(Graphics& graphics);
    void DrawQuote(Graphics& graphics);
    void DrawTimeBlock(Graphics& graphics);
    void DrawSkipButton(Graphics& graphics);
    void DrawExtendButton(Graphics& graphics);
    void DrawProgress(Graphics& graphics);
    void RestoreRegion(Graphics& graphics, const RECT& region);
    void PresentFull(BYTE constantAlpha);
    void PresentRegion(const RECT& dirty);
    void UpdateSkipRegion();
    void UpdateExtendRegion();
    void UpdateProgressRegion();
    void UpdateTimeRegion();
    void UpdateHover(POINT point);
    void OnMouseLeave();
    void OnLButtonDown(POINT point);
    void OnLButtonUp(POINT point);
    void OnClockTick();
    void OnFadeTick();
    void ExtendDuration();
    Font* GetFont(int pixelSize, bool bold, bool italic, bool cjk);
    RectF PrimaryRectF() const;
    RECT PrimaryRect() const;
    RECT SkipRect() const;
    RECT ExtendRect() const;
    RECT BarRect() const;
    RECT RemainingTextRect() const;
    RECT ProgressRect() const;
    RECT TimeRect() const;
    int HitTest(POINT point) const; // 0 none, 1 skip, 2 extend

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    RECT m_virtualRect = {};
    RECT m_primaryRect = {};
    POINT m_origin = {0, 0};
    int m_widthPx = 0;
    int m_heightPx = 0;
    int m_primaryLocalLeft = 0;
    int m_primaryLocalTop = 0;
    int m_primaryLocalWidth = 0;
    int m_primaryLocalHeight = 0;

    int m_backgroundMode = SCREENSAVER_BG_LANDSCAPE;
    int m_quoteIndex = 0;
    ULONGLONG m_startTick = 0;
    ULONGLONG m_endTick = 0;
    int m_lastRemainingSeconds = -1;
    ULONGLONG m_lastMinuteKey = 0;
    bool m_fadeActive = false;
    ULONGLONG m_fadeStartTick = 0;
    BYTE m_currentAlpha = 255;

    int m_hovered = 0;
    int m_pressed = 0;
    bool m_trackingMouse = false;

    Bitmap* m_background = nullptr;
    Bitmap* m_frame = nullptr;
    Graphics* m_graphics = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_previousDib = nullptr;
    void* m_dibPixels = nullptr;
    std::vector<CachedFont> m_fonts;
};

bool RestOverlay::EnsureClass()
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = 0;
    windowClass.lpfnWndProc = &RestOverlay::WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kOverlayClass;
    registered = RegisterClassExW(&windowClass) != 0;
    if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        registered = true;
    return registered;
}

void RestOverlay::ComputeMetrics()
{
    m_virtualRect = GetVirtualScreenRect();
    m_primaryRect = GetPrimaryMonitorRect();
    m_origin.x = m_virtualRect.left;
    m_origin.y = m_virtualRect.top;
    m_widthPx = m_virtualRect.right - m_virtualRect.left;
    m_heightPx = m_virtualRect.bottom - m_virtualRect.top;
    m_primaryLocalLeft = m_primaryRect.left - m_origin.x;
    m_primaryLocalTop = m_primaryRect.top - m_origin.y;
    m_primaryLocalWidth = m_primaryRect.right - m_primaryRect.left;
    m_primaryLocalHeight = m_primaryRect.bottom - m_primaryRect.top;
    m_dpi = m_owner != nullptr ? QueryDpi(m_owner) : 96;
    if (m_dpi == 0)
        m_dpi = 96;
    m_scale = static_cast<float>(m_dpi) / 96.0f;
}

RectF RestOverlay::PrimaryRectF() const
{
    return RectF(static_cast<REAL>(m_primaryLocalLeft),
                 static_cast<REAL>(m_primaryLocalTop),
                 static_cast<REAL>(m_primaryLocalWidth),
                 static_cast<REAL>(m_primaryLocalHeight));
}

RECT RestOverlay::PrimaryRect() const
{
    RECT rect = {m_primaryLocalLeft, m_primaryLocalTop, m_primaryLocalLeft +
                 m_primaryLocalWidth, m_primaryLocalTop + m_primaryLocalHeight};
    return rect;
}

RECT RestOverlay::SkipRect() const
{
    // Subtle "跳过" right next to the remaining-time text.
    const RECT text = RemainingTextRect();
    const int width = RoundToInt(kSkipWidthDip * m_scale);
    const int height = RoundToInt(kSkipHeightDip * m_scale);
    const int centerY = (text.top + text.bottom) / 2;
    RECT rect = {};
    rect.left = text.right + RoundToInt(kSkipGapDip * m_scale);
    rect.right = rect.left + width;
    rect.top = centerY - height / 2;
    rect.bottom = rect.top + height;
    return rect;
}

RECT RestOverlay::ExtendRect() const
{
    // Always visible, sitting right next to the progress bar.
    const RECT primary = PrimaryRect();
    const RECT bar = BarRect();
    const int width = RoundToInt(kExtendWidthDip * m_scale);
    const int height = RoundToInt(kExtendHeightDip * m_scale);
    const int gap = RoundToInt(22.0f * m_scale);
    RECT rect = {};
    rect.left = bar.right + gap;
    rect.right = rect.left + width;
    const int centerY = (bar.top + bar.bottom) / 2;
    rect.top = centerY - height / 2;
    rect.bottom = rect.top + height;
    const int limit = primary.right - RoundToInt(24.0f * m_scale);
    if (rect.right > limit)
    {
        const int shift = rect.right - limit;
        rect.left -= shift;
        rect.right -= shift;
    }
    return rect;
}

RECT RestOverlay::BarRect() const
{
    const RECT primary = PrimaryRect();
    const int width = RoundToInt(kBarWidthDip * m_scale);
    const int height = std::max(3, RoundToInt(kBarHeightDip * m_scale));
    const int centerX = (primary.left + primary.right) / 2;
    const int bottom = RoundToInt(96.0f * m_scale);
    RECT rect = {};
    rect.left = centerX - width / 2;
    rect.right = rect.left + width;
    rect.bottom = primary.bottom - bottom;
    rect.top = rect.bottom - height;
    return rect;
}

RECT RestOverlay::RemainingTextRect() const
{
    // Right-aligned text box that ends just left of the "跳过" button.
    const RECT primary = PrimaryRect();
    const int width = RoundToInt(320.0f * m_scale);
    const int height = RoundToInt(24.0f * m_scale);
    const int centerX = (primary.left + primary.right) / 2;
    RECT rect = {};
    rect.right = centerX - RoundToInt(4.0f * m_scale);
    rect.left = rect.right - width;
    rect.bottom = primary.bottom - RoundToInt(112.0f * m_scale);
    rect.top = rect.bottom - height;
    return rect;
}

RECT RestOverlay::ProgressRect() const
{
    RECT rect = RemainingTextRect();
    const RECT bar = BarRect();
    rect.left = std::min(rect.left, bar.left);
    rect.top = std::min(rect.top, bar.top);
    rect.right = std::max(rect.right, bar.right);
    rect.bottom = std::max(rect.bottom, bar.bottom);
    const int pad = RoundToInt(14.0f * m_scale);
    InflateRect(&rect, pad, pad);
    return rect;
}

RECT RestOverlay::TimeRect() const
{
    const RECT primary = PrimaryRect();
    const int pad = RoundToInt(26.0f * m_scale);
    RECT rect = {};
    switch (m_backgroundMode)
    {
    case SCREENSAVER_BG_SOLID:
    {
        const int width = RoundToInt(460.0f * m_scale);
        const int height = RoundToInt(76.0f * m_scale);
        const int centerX = (primary.left + primary.right) / 2;
        const int top = primary.top + RoundToInt(kSolidTimeTopDip * m_scale);
        rect.left = centerX - width / 2;
        rect.right = centerX + width / 2;
        rect.top = top;
        rect.bottom = top + height;
        break;
    }
    case SCREENSAVER_BG_GRADIENT:
    {
        const int width = RoundToInt(460.0f * m_scale);
        const int height = RoundToInt(76.0f * m_scale);
        const int margin = RoundToInt(kGradientTimeMarginDip * m_scale);
        rect.left = primary.right - margin - width;
        rect.right = primary.right - margin;
        rect.top = primary.top + margin;
        rect.bottom = primary.top + margin + height;
        break;
    }
    case SCREENSAVER_BG_LANDSCAPE:
    default:
    {
        const int width = RoundToInt(620.0f * m_scale);
        const int height = RoundToInt(100.0f * m_scale);
        const int margin = RoundToInt(kLandscapeTimeMarginDip * m_scale);
        rect.left = primary.left + margin;
        rect.right = primary.left + margin + width;
        rect.top = primary.top + margin;
        rect.bottom = primary.top + margin + height;
        break;
    }
    }
    InflateRect(&rect, pad, pad);
    return rect;
}

bool RestOverlay::EnsureSurface()
{
    if (m_frame != nullptr)
        return true;

    m_frame = new Bitmap(m_widthPx, m_heightPx, PixelFormat32bppARGB);
    if (m_frame->GetLastStatus() != Ok)
    {
        delete m_frame;
        m_frame = nullptr;
        return false;
    }
    m_graphics = new Graphics(m_frame);
    m_graphics->SetSmoothingMode(SmoothingModeAntiAlias);
    m_graphics->SetPixelOffsetMode(PixelOffsetModeHighQuality);
    m_graphics->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    m_graphics->SetCompositingQuality(CompositingQualityHighQuality);
    m_graphics->SetInterpolationMode(InterpolationModeHighQualityBicubic);

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

void RestOverlay::ReleaseResources()
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
    if (m_background != nullptr)
    {
        delete m_background;
        m_background = nullptr;
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
    DeleteFonts(m_fonts);
}

Font* RestOverlay::GetFont(int pixelSize, bool bold, bool italic, bool cjk)
{
    FontFamily* family = cjk ? g_serifCjk : g_serifLatin;
    if (family == nullptr)
        family = g_serifCjk != nullptr ? g_serifCjk : g_serifLatin;
    return FindFont(m_fonts, family, pixelSize, bold, italic);
}

// --- background: procedural landscape -------------------------------------
void RestOverlay::DrawLandscape(Graphics& graphics)
{
    const RectF area(0.0f, 0.0f, static_cast<REAL>(m_widthPx),
                     static_cast<REAL>(m_heightPx));
    const REAL width = area.Width;
    const REAL height = area.Height;
    const REAL horizon = height * 0.665f;

    // Sky: deep blue at the top fading into a warm dusk band at the horizon.
    LinearGradientBrush sky(RectF(area.X, area.Y, width, horizon + 2.0f),
                            Color(255, 12, 20, 42), Color(255, 88, 92, 122),
                            LinearGradientModeVertical);
    graphics.FillRectangle(&sky, RectF(area.X, area.Y, width, horizon + 2.0f));

    LinearGradientBrush warm(
        RectF(area.X, horizon - height * 0.30f, width, height * 0.30f + 2.0f),
        Color(0, 244, 176, 120), Color(150, 248, 190, 138),
        LinearGradientModeVertical);
    graphics.FillRectangle(&warm,
                           RectF(area.X, horizon - height * 0.30f, width,
                                 height * 0.30f + 2.0f));

    // Sun glow + disc.
    const REAL sunX = area.X + width * 0.665f;
    const REAL sunY = area.Y + horizon - height * 0.072f;
    DrawSoftGlow(graphics, PointF(sunX, sunY), height * 0.34f, height * 0.26f,
                 RGB(255, 214, 156), 170, 56);
    {
        const REAL radius = height * 0.030f;
        SolidBrush disc(Color(230, 255, 238, 200));
        graphics.FillEllipse(&disc,
                             RectF(sunX - radius, sunY - radius, radius * 2.0f,
                                   radius * 2.0f));
    }

    // Soft clouds.
    const REAL cloudAlpha[] = {20.0f, 26.0f, 17.0f, 22.0f, 18.0f, 14.0f};
    for (int i = 0; i < 6; ++i)
    {
        const REAL cx = area.X + width * (0.08f + 0.15f * static_cast<REAL>(i)) +
                        std::sin(static_cast<REAL>(i) * 1.7f) * width * 0.05f;
        const REAL cy = area.Y + height * (0.10f + 0.065f * static_cast<REAL>(i % 4)) +
                        std::cos(static_cast<REAL>(i) * 2.1f) * height * 0.02f;
        const REAL cw = width * (0.16f + 0.05f * static_cast<REAL>(i % 3));
        const REAL ch = height * (0.030f + 0.008f * static_cast<REAL>(i % 3));
        const BYTE alpha = static_cast<BYTE>(cloudAlpha[i]);
        SolidBrush cloud(Color(alpha, 244, 238, 238));
        graphics.FillEllipse(&cloud, RectF(cx - cw * 0.5f, cy - ch * 0.5f, cw, ch));
        graphics.FillEllipse(&cloud,
                             RectF(cx - cw * 0.30f, cy - ch * 0.95f, cw * 0.60f,
                                   ch * 1.2f));
        graphics.FillEllipse(&cloud,
                             RectF(cx - cw * 0.10f, cy - ch * 1.25f, cw * 0.55f,
                                   ch * 1.1f));
    }

    // Mountain ridges (back to front).
    struct Ridge
    {
        REAL baseY;
        REAL amplitude;
        REAL phase;
        REAL bottom;
        COLORREF topColor;
        COLORREF bottomColor;
    };
    const Ridge ridges[4] = {
        {horizon + 2.0f, height * 0.100f, 0.8f, horizon + 4.0f,
         RGB(104, 112, 148), RGB(62, 70, 100)},
        {horizon + 6.0f, height * 0.155f, 2.4f, horizon + 8.0f,
         RGB(70, 78, 112), RGB(38, 44, 68)},
        {horizon + 10.0f, height * 0.215f, 4.1f, horizon + 12.0f,
         RGB(42, 48, 72), RGB(20, 24, 40)},
        {horizon + 14.0f, height * 0.280f, 5.7f, horizon + 16.0f,
         RGB(26, 30, 46), RGB(10, 12, 22)}};

    for (int layer = 0; layer < 4; ++layer)
    {
        const Ridge& ridge = ridges[layer];
        std::vector<PointF> points;
        const REAL step = std::max(12.0f, width / 220.0f);
        for (REAL x = area.X - step * 2.0f; x <= area.X + width + step * 2.0f;
             x += step)
        {
            const REAL t = (x - area.X) / width;
            const REAL wave = std::sin(t * 6.2f + ridge.phase) * 0.55f +
                              std::sin(t * 13.7f + ridge.phase * 1.7f) * 0.28f +
                              std::sin(t * 27.0f + ridge.phase * 2.3f) * 0.17f;
            const REAL y = ridge.baseY - ridge.amplitude * (0.55f + 0.45f * wave);
            points.push_back(PointF(x, y));
        }
        points.push_back(PointF(area.X + width + step * 2.0f, ridge.bottom));
        points.push_back(PointF(area.X - step * 2.0f, ridge.bottom));

        GraphicsPath path;
        path.AddPolygon(points.data(), static_cast<INT>(points.size()));
        RectF gradientRect(area.X, ridge.baseY - ridge.amplitude, width,
                           ridge.amplitude + (ridge.bottom - (ridge.baseY -
                                                              ridge.amplitude)));
        LinearGradientBrush brush(gradientRect, GdiColor(ridge.topColor),
                                  GdiColor(ridge.bottomColor),
                                  LinearGradientModeVertical);
        graphics.FillPath(&brush, &path);
    }

    // Water.
    RectF waterRect(area.X, horizon, width, height - horizon + 2.0f);
    {
        LinearGradientBrush water(waterRect, Color(255, 22, 32, 54),
                                  Color(255, 6, 9, 18),
                                  LinearGradientModeVertical);
        graphics.FillRectangle(&water, waterRect);
    }

    // Sun reflection: a small 2D gradient bitmap (gaussian horizontal falloff +
    // vertical fade) stretched over the water. This is perfectly smooth, unlike
    // strips or a plain linear gradient, and it only costs a few thousand pixels
    // per entry.
    {
        constexpr int kReflectionSize = 64;
        Bitmap reflection(kReflectionSize, kReflectionSize,
                          PixelFormat32bppARGB);
        if (reflection.GetLastStatus() == Ok)
        {
            BitmapData data = {};
            const Rect lockRect(0, 0, kReflectionSize, kReflectionSize);
            if (reflection.LockBits(&lockRect, ImageLockModeWrite,
                                    PixelFormat32bppARGB, &data) == Ok)
            {
                BYTE* rows = static_cast<BYTE*>(data.Scan0);
                for (int y = 0; y < kReflectionSize; ++y)
                {
                    BYTE* row = rows + static_cast<size_t>(y) *
                                            static_cast<size_t>(data.Stride);
                    const REAL vertical =
                        1.0f - static_cast<REAL>(y) /
                                   static_cast<REAL>(kReflectionSize) * 0.92f;
                    for (int x = 0; x < kReflectionSize; ++x)
                    {
                        const REAL normalized =
                            (static_cast<REAL>(x) + 0.5f) /
                                static_cast<REAL>(kReflectionSize) * 2.0f - 1.0f;
                        const REAL falloff =
                            std::exp(-normalized * normalized * 2.6f);
                        const int alpha =
                            RoundToInt(82.0f * falloff * vertical);
                        row[x * 4 + 0] = 255;
                        row[x * 4 + 1] = 212;
                        row[x * 4 + 2] = 164;
                        row[x * 4 + 3] =
                            static_cast<BYTE>(ClampInt(alpha, 0, 255));
                    }
                }
                reflection.UnlockBits(&data);

                const REAL reflectionHalf = width * 0.13f;
                graphics.DrawImage(&reflection,
                                   RectF(sunX - reflectionHalf, horizon,
                                         reflectionHalf * 2.0f,
                                         (height - horizon) * 0.94f));
            }
        }
    }

    // Shimmering water highlights.
    for (int i = 0; i < 46; ++i)
    {
        const REAL t = static_cast<REAL>(i) / 46.0f;
        const REAL y = horizon + std::pow(t, 1.55f) * (height - horizon) * 0.94f;
        const REAL jitter = std::sin(static_cast<REAL>(i) * 2.37f) * width * 0.018f;
        const REAL x = sunX + jitter +
                       (std::fmod(static_cast<REAL>(i) * 0.618f, 1.0f) - 0.5f) *
                           width * 0.16f;
        const REAL lineWidth = width * (0.020f + 0.012f * (1.0f - t));
        const BYTE alpha = static_cast<BYTE>(std::lround(92.0f * (1.0f - t * 0.85f)));
        SolidBrush shimmer(Color(alpha, 255, 222, 178));
        graphics.FillRectangle(&shimmer,
                               RectF(x - lineWidth * 0.5f, y, lineWidth,
                                     1.6f * m_scale + 1.0f));
    }

    // Very subtle dark band right at the horizon for depth.
    {
        LinearGradientBrush haze(
            RectF(area.X, horizon - height * 0.02f, width, height * 0.04f),
            Color(0, 8, 12, 24), Color(70, 8, 12, 24),
            LinearGradientModeVertical);
        graphics.FillRectangle(&haze,
                               RectF(area.X, horizon - height * 0.02f, width,
                                     height * 0.04f));
    }
}

void RestOverlay::DrawSolid(Graphics& graphics)
{
    const RectF area(0.0f, 0.0f, static_cast<REAL>(m_widthPx),
                     static_cast<REAL>(m_heightPx));
    LinearGradientBrush base(area, Color(255, 26, 32, 46),
                             Color(255, 11, 14, 22),
                             LinearGradientModeVertical);
    graphics.FillRectangle(&base, area);

    // Soft teal glow behind the quote area.
    const PointF center(area.X + area.Width * 0.5f, area.Y + area.Height * 0.40f);
    DrawSoftGlow(graphics, center, area.Width * 0.40f, area.Height * 0.34f,
                 RGB(78, 152, 156), 58, 56);
}

void RestOverlay::DrawGradient(Graphics& graphics)
{
    const RectF area(0.0f, 0.0f, static_cast<REAL>(m_widthPx),
                     static_cast<REAL>(m_heightPx));
    LinearGradientBrush base(area, Color(255, 8, 34, 42), Color(255, 78, 38, 58),
                             LinearGradientModeForwardDiagonal);
    graphics.FillRectangle(&base, area);

    LinearGradientBrush wash(area, Color(120, 24, 78, 92), Color(0, 24, 78, 92),
                             LinearGradientModeVertical);
    graphics.FillRectangle(&wash, area);

    const PointF center(area.X + area.Width * 0.74f, area.Y + area.Height * 0.80f);
    DrawSoftGlow(graphics, center, area.Width * 0.46f, area.Height * 0.40f,
                 RGB(196, 100, 70), 84, 56);
}

void RestOverlay::DrawVignette(Graphics& graphics)
{
    const RectF area(0.0f, 0.0f, static_cast<REAL>(m_widthPx),
                     static_cast<REAL>(m_heightPx));
    const REAL band = area.Height * 0.20f;

    LinearGradientBrush top(RectF(area.X, area.Y, area.Width, band),
                            Color(120, 0, 0, 0), Color(0, 0, 0, 0),
                            LinearGradientModeVertical);
    graphics.FillRectangle(&top, RectF(area.X, area.Y, area.Width, band));

    LinearGradientBrush bottom(
        RectF(area.X, area.Y + area.Height - band * 1.4f, area.Width,
              band * 1.4f),
        Color(0, 0, 0, 0), Color(170, 0, 0, 0), LinearGradientModeVertical);
    graphics.FillRectangle(&bottom,
                           RectF(area.X, area.Y + area.Height - band * 1.4f,
                                 area.Width, band * 1.4f));

    const REAL side = area.Width * 0.10f;
    LinearGradientBrush left(RectF(area.X, area.Y, side, area.Height),
                             Color(90, 0, 0, 0), Color(0, 0, 0, 0),
                             LinearGradientModeHorizontal);
    graphics.FillRectangle(&left, RectF(area.X, area.Y, side, area.Height));
    LinearGradientBrush right(
        RectF(area.X + area.Width - side, area.Y, side, area.Height),
        Color(0, 0, 0, 0), Color(90, 0, 0, 0), LinearGradientModeHorizontal);
    graphics.FillRectangle(&right,
                           RectF(area.X + area.Width - side, area.Y, side,
                                 area.Height));
}

void RestOverlay::RenderBackground()
{
    if (m_background == nullptr)
        m_background = new Bitmap(m_widthPx, m_heightPx, PixelFormat32bppARGB);
    if (m_background->GetLastStatus() != Ok)
    {
        delete m_background;
        m_background = nullptr;
        return;
    }

    Graphics graphics(m_background);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);

    switch (m_backgroundMode)
    {
    case SCREENSAVER_BG_SOLID:
        DrawSolid(graphics);
        break;
    case SCREENSAVER_BG_GRADIENT:
        DrawGradient(graphics);
        break;
    case SCREENSAVER_BG_LANDSCAPE:
    default:
        DrawLandscape(graphics);
        break;
    }
    DrawVignette(graphics);
}

// --- foreground: quote / chrome / progress --------------------------------
void RestOverlay::DrawQuote(Graphics& graphics)
{
    const QuoteInfo& quote = kQuotes[m_quoteIndex];
    const std::wstring text = quote.text;
    const std::wstring author = std::wstring(L"—— ") + quote.author;
    const bool cjk = HasCjk(text);
    const RectF primary = PrimaryRectF();
    const REAL centerX = primary.X + primary.Width * 0.5f;
    const REAL centerY = primary.Y + primary.Height * 0.41f;
    const REAL maxWidth =
        std::min(primary.Width * 0.62f, static_cast<REAL>(RoundToInt(1400.0f *
                                                                     m_scale)));

    const int sizes[] = {58, 52, 46, 40, 34, 28, 24};
    int chosenSize = sizes[6];
    REAL chosenHeight = 0.0f;
    Font* quoteFont = GetFont(std::max(8, RoundToInt(chosenSize * m_scale)),
                              false, !cjk, cjk);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentNear);
    format.SetTrimming(StringTrimmingNone);
    format.SetFormatFlags(0);
    const REAL maxHeight = primary.Height * 0.40f;
    for (int i = 0; i < 7; ++i)
    {
        Font* font = GetFont(std::max(8, RoundToInt(sizes[i] * m_scale)), false,
                             !cjk, cjk);
        RectF layout(0.0f, 0.0f, maxWidth, 100000.0f);
        RectF bounds;
        graphics.MeasureString(text.c_str(), static_cast<INT>(text.length()), font,
                               layout, &format, &bounds);
        if (bounds.Height <= maxHeight)
        {
            chosenSize = sizes[i];
            chosenHeight = bounds.Height;
            quoteFont = font;
            break;
        }
    }
    if (chosenHeight <= 0.0f)
    {
        RectF layout(0.0f, 0.0f, maxWidth, 100000.0f);
        RectF bounds;
        graphics.MeasureString(text.c_str(), static_cast<INT>(text.length()),
                               quoteFont, layout, &format, &bounds);
        chosenHeight = std::min(bounds.Height, maxHeight);
    }

    const REAL quoteTop = centerY - chosenHeight * 0.5f;
    const REAL shadowOffset = std::max(1.5f, 2.5f * m_scale);
    RectF quoteRect(centerX - maxWidth * 0.5f, quoteTop, maxWidth,
                    chosenHeight + 4.0f * m_scale);
    {
        SolidBrush shadow(Color(120, 0, 0, 0));
        RectF shadowRect(quoteRect.X + shadowOffset, quoteRect.Y + shadowOffset,
                         quoteRect.Width, quoteRect.Height);
        graphics.DrawString(text.c_str(), static_cast<INT>(text.length()),
                            quoteFont, shadowRect, &format, &shadow);
        SolidBrush brush(Color(250, 248, 246, 240));
        graphics.DrawString(text.c_str(), static_cast<INT>(text.length()),
                            quoteFont, quoteRect, &format, &brush);
    }

    Font* authorFont = GetFont(std::max(6, RoundToInt(20.0f * m_scale)), false,
                               !HasCjk(author), HasCjk(author));
    StringFormat authorFormat;
    authorFormat.SetAlignment(StringAlignmentFar);
    authorFormat.SetLineAlignment(StringAlignmentCenter);
    authorFormat.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF authorRect(centerX - maxWidth * 0.5f,
                     quoteTop + chosenHeight + 30.0f * m_scale, maxWidth,
                     32.0f * m_scale);
    {
        SolidBrush shadow(Color(110, 0, 0, 0));
        RectF shadowRect(authorRect.X + shadowOffset, authorRect.Y + shadowOffset,
                         authorRect.Width, authorRect.Height);
        graphics.DrawString(author.c_str(), static_cast<INT>(author.length()),
                            authorFont, shadowRect, &authorFormat, &shadow);
        SolidBrush brush(Color(215, 232, 232, 238));
        graphics.DrawString(author.c_str(), static_cast<INT>(author.length()),
                            authorFont, authorRect, &authorFormat, &brush);
    }
}

void RestOverlay::DrawSkipButton(Graphics& graphics)
{
    const RECT rect = SkipRect();
    const RectF pill(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top),
                     static_cast<REAL>(rect.right - rect.left),
                     static_cast<REAL>(rect.bottom - rect.top));
    const bool hovered = m_hovered == 1;
    const bool pressed = m_pressed == 1;

    SolidBrush fill(Color(hovered ? 130 : (pressed ? 165 : 70), 0, 0, 0));
    GraphicsPath path;
    AddRoundedRectPath(path, pill, pill.Height * 0.5f);
    graphics.FillPath(&fill, &path);
    Pen border(Color(hovered ? 150 : 85, 255, 255, 255),
               std::max(1.0f, m_scale));
    graphics.DrawPath(&border, &path);

    Font* font =
        GetFont(std::max(6, RoundToInt(11.5f * m_scale)), false, false, true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush text(Color(hovered ? 245 : 205, 250, 250, 252));
    graphics.DrawString(L"跳过", -1, font, pill, &format, &text);
}

void RestOverlay::DrawTimeBlock(Graphics& graphics)
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    const wchar_t* const weekdayNames[] = {L"日", L"一", L"二", L"三",
                                           L"四", L"五", L"六"};
    const int weekday = ClampInt(static_cast<int>(now.wDayOfWeek), 0, 6);

    wchar_t timeText[16] = {};
    swprintf_s(timeText, L"%02d:%02d", static_cast<int>(now.wHour),
               static_cast<int>(now.wMinute));
    wchar_t dateText[64] = {};
    swprintf_s(dateText, L"%d年%d月%d日 星期%s", static_cast<int>(now.wYear),
               static_cast<int>(now.wMonth), static_cast<int>(now.wDay),
               weekdayNames[weekday]);
    wchar_t shortDate[64] = {};
    swprintf_s(shortDate, L"%d月%d日 星期%s", static_cast<int>(now.wMonth),
               static_cast<int>(now.wDay), weekdayNames[weekday]);

    const RectF primary = PrimaryRectF();
    const auto drawText = [this, &graphics](
                              const std::wstring& text, const RectF& rect,
                              int pixelSize, bool bold, bool italic, bool cjk,
                              StringAlignment align, BYTE alpha) {
        Font* font = GetFont(std::max(6, pixelSize), bold, italic, cjk);
        StringFormat format;
        format.SetAlignment(align);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        const REAL shadowOffset = std::max(1.0f, 1.6f * m_scale);
        SolidBrush shadow(Color(110, 0, 0, 0));
        const RectF shadowRect(rect.X + shadowOffset, rect.Y + shadowOffset,
                               rect.Width, rect.Height);
        graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font,
                            shadowRect, &format, &shadow);
        SolidBrush brush(Color(alpha, 248, 247, 244));
        graphics.DrawString(text.c_str(), static_cast<INT>(text.length()), font,
                            rect, &format, &brush);
    };

    switch (m_backgroundMode)
    {
    case SCREENSAVER_BG_SOLID:
    {
        const REAL centerX = primary.X + primary.Width * 0.5f;
        const REAL top =
            primary.Y + static_cast<REAL>(RoundToInt(kSolidTimeTopDip * m_scale));
        drawText(timeText,
                 RectF(centerX - 230.0f * m_scale, top, 460.0f * m_scale,
                       40.0f * m_scale),
                 RoundToInt(30.0f * m_scale), false, false, false,
                 StringAlignmentCenter, 235);
        drawText(shortDate,
                 RectF(centerX - 230.0f * m_scale, top + 36.0f * m_scale,
                       460.0f * m_scale, 22.0f * m_scale),
                 RoundToInt(12.5f * m_scale), false, false, true,
                 StringAlignmentCenter, 185);
        break;
    }
    case SCREENSAVER_BG_GRADIENT:
    {
        const REAL right =
            primary.X + primary.Width -
            static_cast<REAL>(RoundToInt(kGradientTimeMarginDip * m_scale));
        const REAL top = primary.Y +
                         static_cast<REAL>(RoundToInt(kGradientTimeMarginDip *
                                                      m_scale));
        drawText(timeText,
                 RectF(right - 420.0f * m_scale, top, 420.0f * m_scale,
                       46.0f * m_scale),
                 RoundToInt(34.0f * m_scale), false, true, false,
                 StringAlignmentFar, 236);
        drawText(shortDate,
                 RectF(right - 420.0f * m_scale, top + 42.0f * m_scale,
                       420.0f * m_scale, 22.0f * m_scale),
                 RoundToInt(12.5f * m_scale), false, false, true,
                 StringAlignmentFar, 190);
        break;
    }
    case SCREENSAVER_BG_LANDSCAPE:
    default:
    {
        const REAL left =
            primary.X + static_cast<REAL>(RoundToInt(kLandscapeTimeMarginDip *
                                                     m_scale));
        const REAL top = primary.Y +
                         static_cast<REAL>(RoundToInt(kLandscapeTimeMarginDip *
                                                      m_scale));
        drawText(timeText,
                 RectF(left, top, 600.0f * m_scale, 64.0f * m_scale),
                 RoundToInt(46.0f * m_scale), false, false, false,
                 StringAlignmentNear, 240);
        drawText(dateText,
                 RectF(left, top + 60.0f * m_scale, 620.0f * m_scale,
                       26.0f * m_scale),
                 RoundToInt(16.0f * m_scale), false, false, true,
                 StringAlignmentNear, 228);
        break;
    }
    }
}

void RestOverlay::DrawExtendButton(Graphics& graphics)
{
    const RECT rect = ExtendRect();
    const RectF pill(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top),
                     static_cast<REAL>(rect.right - rect.left),
                     static_cast<REAL>(rect.bottom - rect.top));
    const bool hovered = m_hovered == 2;
    const bool pressed = m_pressed == 2;

    SolidBrush brush(Color(hovered ? 205 : (pressed ? 175 : 150), 0, 0, 0));
    GraphicsPath path;
    AddRoundedRectPath(path, pill, pill.Height * 0.5f);
    graphics.FillPath(&brush, &path);
    Pen border(Color(hovered ? 150 : 110, 255, 255, 255),
               std::max(1.0f, m_scale));
    graphics.DrawPath(&border, &path);

    const std::wstring label =
        std::wstring(L"延长 ") + ToWString(ExtendMinutes()) + L" 分钟";
    Font* font = GetFont(std::max(6, RoundToInt(13.5f * m_scale)), true, false,
                         true);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush text(Color(hovered ? 252 : 238, 250, 250, 252));
    graphics.DrawString(label.c_str(), static_cast<INT>(label.length()), font, pill,
                        &format, &text);
}

void RestOverlay::DrawProgress(Graphics& graphics)
{
    const ULONGLONG now = NowMs();
    const ULONGLONG total = m_endTick > m_startTick ? m_endTick - m_startTick : 1;
    const ULONGLONG remaining =
        m_endTick > now ? std::min(m_endTick - now, total) : 0;
    const int secondsLeft = static_cast<int>((remaining + 999) / 1000);
    const float fraction =
        ClampFloat(static_cast<float>(remaining) / static_cast<float>(total),
                   0.0f, 1.0f);

    const RECT bar = BarRect();
    const RectF track(static_cast<REAL>(bar.left), static_cast<REAL>(bar.top),
                      static_cast<REAL>(bar.right - bar.left),
                      static_cast<REAL>(bar.bottom - bar.top));
    const REAL radius = track.Height * 0.5f;

    SolidBrush trackBrush(Color(46, 255, 255, 255));
    GraphicsPath trackPath;
    AddRoundedRectPath(trackPath, track, radius);
    graphics.FillPath(&trackBrush, &trackPath);

    if (fraction > 0.0f)
    {
        RectF fillRect = track;
        fillRect.Width = track.Width * fraction;
        if (fillRect.Width >= 1.0f)
        {
            SolidBrush fillBrush(Color(225, 246, 238, 222));
            GraphicsPath fillPath;
            AddRoundedRectPath(fillPath, fillRect, radius);
            graphics.FillPath(&fillBrush, &fillPath);
        }
    }

    const RECT textRect = RemainingTextRect();
    const std::wstring label = L"剩余 " + FormatClock(secondsLeft);
    Font* font = GetFont(std::max(6, RoundToInt(12.5f * m_scale)), false, false,
                         true);
    StringFormat format;
    format.SetAlignment(StringAlignmentFar);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    const RectF text(static_cast<REAL>(textRect.left),
                     static_cast<REAL>(textRect.top),
                     static_cast<REAL>(textRect.right - textRect.left),
                     static_cast<REAL>(textRect.bottom - textRect.top));
    {
        SolidBrush shadow(Color(110, 0, 0, 0));
        RectF shadowRect(text.X + 1.5f * m_scale, text.Y + 1.5f * m_scale,
                         text.Width, text.Height);
        graphics.DrawString(label.c_str(), static_cast<INT>(label.length()), font,
                            shadowRect, &format, &shadow);
        SolidBrush brush(Color(225, 248, 248, 250));
        graphics.DrawString(label.c_str(), static_cast<INT>(label.length()), font,
                            text, &format, &brush);
    }
}

void RestOverlay::RestoreRegion(Graphics& graphics, const RECT& region)
{
    if (m_background == nullptr)
        return;
    const GraphicsState state = graphics.Save();
    const Gdiplus::Rect clip(region.left, region.top, region.right - region.left,
                             region.bottom - region.top);
    graphics.SetClip(clip);
    graphics.DrawImage(m_background, 0, 0, m_widthPx, m_heightPx);
    graphics.Restore(state);
}

void RestOverlay::ComposeFrame()
{
    if (m_graphics == nullptr || m_background == nullptr)
        return;

    m_graphics->Clear(Color(0, 0, 0, 0));
    m_graphics->DrawImage(m_background, 0, 0, m_widthPx, m_heightPx);
    DrawQuote(*m_graphics);
    DrawTimeBlock(*m_graphics);
    DrawExtendButton(*m_graphics);
    DrawProgress(*m_graphics);
    DrawSkipButton(*m_graphics);
    m_lastMinuteKey = CurrentMinuteKey();
}

void RestOverlay::PresentFull(BYTE constantAlpha)
{
    if (m_hwnd == nullptr || m_frame == nullptr || m_memoryDc == nullptr)
        return;

    const RECT full = {0, 0, m_widthPx, m_heightPx};
    BitmapData data = {};
    const Rect lockRect(full.left, full.top, full.right, full.bottom);
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

    POINT destinationPoint = {m_origin.x, m_origin.y};
    SIZE size = {m_widthPx, m_heightPx};
    POINT sourcePoint = {0, 0};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = constantAlpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

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
    info.prcDirty = nullptr;
    UpdateLayeredWindowIndirect(m_hwnd, &info);
}

void RestOverlay::PresentRegion(const RECT& dirty)
{
    if (m_hwnd == nullptr || m_frame == nullptr || m_memoryDc == nullptr)
        return;

    RECT region = dirty;
    if (region.left < 0)
        region.left = 0;
    if (region.top < 0)
        region.top = 0;
    if (region.right > m_widthPx)
        region.right = m_widthPx;
    if (region.bottom > m_heightPx)
        region.bottom = m_heightPx;
    if (region.right <= region.left || region.bottom <= region.top)
        return;

    BitmapData data = {};
    const Rect lockRect(region.left, region.top, region.right - region.left,
                        region.bottom - region.top);
    if (m_frame->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB,
                          &data) != Ok)
        return;

    const BYTE* source = static_cast<const BYTE*>(data.Scan0);
    const size_t sourceStride = static_cast<size_t>(data.Stride);
    const size_t rowBytes = static_cast<size_t>(m_widthPx) * 4;
    const int regionWidth = region.right - region.left;
    for (int y = 0; y < region.bottom - region.top; ++y)
    {
        const BYTE* src = source + static_cast<size_t>(y) * sourceStride;
        BYTE* dst = static_cast<BYTE*>(m_dibPixels) +
                    static_cast<size_t>(region.top + y) * rowBytes +
                    static_cast<size_t>(region.left) * 4;
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

    POINT destinationPoint = {m_origin.x, m_origin.y};
    SIZE size = {m_widthPx, m_heightPx};
    POINT sourcePoint = {0, 0};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = m_currentAlpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    RECT dirtyRect = region;
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

void RestOverlay::UpdateSkipRegion()
{
    RECT expanded = SkipRect();
    InflateRect(&expanded, RoundToInt(8.0f * m_scale),
                RoundToInt(8.0f * m_scale));
    RestoreRegion(*m_graphics, expanded);
    DrawSkipButton(*m_graphics);
    PresentRegion(expanded);
}

void RestOverlay::UpdateTimeRegion()
{
    RECT expanded = TimeRect();
    RestoreRegion(*m_graphics, expanded);
    DrawTimeBlock(*m_graphics);
    PresentRegion(expanded);
}

void RestOverlay::UpdateExtendRegion()
{
    RECT expanded = ExtendRect();
    InflateRect(&expanded, 12, 12);
    RestoreRegion(*m_graphics, expanded);
    DrawExtendButton(*m_graphics);
    PresentRegion(expanded);
}

void RestOverlay::UpdateProgressRegion()
{
    RECT region = ProgressRect();
    RestoreRegion(*m_graphics, region);
    DrawProgress(*m_graphics);
    // The inflated progress region overlaps the left edge of the "跳过" button.
    DrawSkipButton(*m_graphics);
    PresentRegion(region);
}

void RestOverlay::UpdateHover(POINT point)
{
    const int hit = HitTest(point);
    if (hit == m_hovered)
        return;
    const int previous = m_hovered;
    m_hovered = hit;
    if (previous == 1 || hit == 1)
        UpdateSkipRegion();
    if (previous == 2 || hit == 2)
        UpdateExtendRegion();
}

int RestOverlay::HitTest(POINT point) const
{
    const auto inside = [&point](const RECT& rect) {
        return point.x >= rect.left && point.x < rect.right &&
               point.y >= rect.top && point.y < rect.bottom;
    };
    if (inside(SkipRect()))
        return 1;
    if (inside(ExtendRect()))
        return 2;
    return 0;
}

void RestOverlay::OnMouseLeave()
{
    m_trackingMouse = false;
    if (m_hovered == 0)
        return;
    const int previous = m_hovered;
    m_hovered = 0;
    if (previous == 1)
        UpdateSkipRegion();
    if (previous == 2)
        UpdateExtendRegion();
}

void RestOverlay::OnLButtonDown(POINT point)
{
    const int hit = HitTest(point);
    if (hit == 0)
    {
        m_pressed = 0;
        return;
    }
    m_pressed = hit;
    SetCapture(m_hwnd);
    if (hit == 1)
        UpdateSkipRegion();
    else
        UpdateExtendRegion();
}

void RestOverlay::ExtendDuration()
{
    m_endTick += static_cast<ULONGLONG>(ExtendMinutes()) * 60ULL * 1000ULL;
    m_lastRemainingSeconds = -1;
    UpdateProgressRegion();
}

void RestOverlay::OnLButtonUp(POINT point)
{
    const int pressed = m_pressed;
    m_pressed = 0;
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    const int hit = HitTest(point);
    if (pressed != 0 && pressed == hit)
    {
        if (hit == 1)
        {
            ExitOverlayNow();
            return;
        }
        if (hit == 2)
        {
            ExtendDuration();
            return;
        }
    }
    if (pressed == 1)
        UpdateSkipRegion();
    if (pressed == 2)
        UpdateExtendRegion();
}

void RestOverlay::OnClockTick()
{
    if (m_hwnd == nullptr)
        return;
    const ULONGLONG now = NowMs();
    if (now >= m_endTick)
    {
        ExitOverlayNow();
        return;
    }
    if (m_fadeActive)
        return; // the progress text is already correct in the first frame
    const ULONGLONG minuteKey = CurrentMinuteKey();
    if (minuteKey != m_lastMinuteKey)
    {
        m_lastMinuteKey = minuteKey;
        UpdateTimeRegion();
    }
    const int remaining =
        static_cast<int>((m_endTick - now + 999ULL) / 1000ULL);
    if (remaining != m_lastRemainingSeconds)
    {
        m_lastRemainingSeconds = remaining;
        UpdateProgressRegion();
    }
}

void RestOverlay::OnFadeTick()
{
    if (m_hwnd == nullptr)
        return;
    const ULONGLONG elapsed = NowMs() - m_fadeStartTick;
    if (elapsed >= kFadeDurationMs)
    {
        KillTimer(m_hwnd, kFadeTimerId);
        m_fadeActive = false;
        m_currentAlpha = 255;
        PresentFull(255);
        return;
    }
    const BYTE alpha =
        static_cast<BYTE>(std::min<ULONGLONG>(255ULL, elapsed * 255ULL /
                                                         kFadeDurationMs));
    m_currentAlpha = alpha;
    PresentFull(alpha);
}

bool RestOverlay::Create(HWND owner, int backgroundMode)
{
    if (m_hwnd != nullptr)
        return true;
    if (!EnsureClass())
        return false;

    m_owner = owner;
    m_backgroundMode = ClampInt(backgroundMode, 0, SCREENSAVER_BG_COUNT - 1);
    m_quoteIndex = PickQuoteIndex(m_quoteIndex);
    m_hovered = 0;
    m_pressed = 0;
    m_trackingMouse = false;
    m_fadeActive = false;
    m_currentAlpha = 0;
    m_lastRemainingSeconds = -1;

    ComputeMetrics();
    if (m_widthPx <= 0 || m_heightPx <= 0)
        return false;

    const HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kOverlayClass,
        L"休息一下", WS_POPUP, m_origin.x, m_origin.y, m_widthPx, m_heightPx,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr)
    {
        m_hwnd = nullptr;
        ReleaseResources();
        return false;
    }

    if (!EnsureSurface())
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        ReleaseResources();
        return false;
    }

    RenderBackground();
    ComposeFrame();
    if (g_testMode)
    {
        // Deterministic screenshots: no fade-in for the internal test hooks.
        PresentFull(255);
    }
    else
    {
        PresentFull(0);
    }

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_origin.x, m_origin.y, m_widthPx,
                 m_heightPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const ULONGLONG now = NowMs();
    m_startTick = now;
    m_endTick = now + static_cast<ULONGLONG>(DurationSeconds()) * 1000ULL;
    m_currentAlpha = 255;
    if (!g_testMode)
    {
        m_fadeStartTick = now;
        m_fadeActive = true;
        SetTimer(m_hwnd, kFadeTimerId, kFadeIntervalMs, nullptr);
    }
    SetTimer(m_hwnd, kClockTimerId, kClockIntervalMs, nullptr);
    return true;
}

void RestOverlay::Destroy()
{
    if (m_hwnd != nullptr)
    {
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwnd);
    }
    ReleaseResources();
}

void RestOverlay::SetBackgroundMode(int backgroundMode)
{
    backgroundMode = ClampInt(backgroundMode, 0, SCREENSAVER_BG_COUNT - 1);
    if (backgroundMode == m_backgroundMode || m_hwnd == nullptr)
        return;
    m_backgroundMode = backgroundMode;
    if (m_background != nullptr)
    {
        delete m_background;
        m_background = nullptr;
    }
    RenderBackground();
    ComposeFrame();
    m_lastRemainingSeconds = -1;
    m_currentAlpha = 255;
    PresentFull(255);
}

LRESULT CALLBACK RestOverlay::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam)
{
    RestOverlay* self = reinterpret_cast<RestOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RestOverlay*>(create->lpCreateParams);
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

LRESULT RestOverlay::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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
        if (wParam == kFadeTimerId)
            OnFadeTick();
        else if (wParam == kClockTimerId)
            OnClockTick();
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
        if (m_pressed != 0)
        {
            const int pressed = m_pressed;
            m_pressed = 0;
            if (pressed == 1)
                UpdateSkipRegion();
            if (pressed == 2)
                UpdateExtendRegion();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ExitOverlayNow();
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(m_hwnd, &point);
            SetCursor(LoadCursorW(nullptr, HitTest(point) != 0 ? IDC_HAND
                                                               : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        ExitOverlayNow();
        return 0;

    case WM_CLOSE:
        ExitOverlayNow();
        return 0;

    case WM_DESTROY:
        KillTimer(m_hwnd, kFadeTimerId);
        KillTimer(m_hwnd, kClockTimerId);
        m_fadeActive = false;
        ReleaseResources();
        m_hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
RestNotice g_notice;
RestOverlay g_overlay;

int SecondsUntilEntry()
{
    if (!g_scheduled)
        return 0;
    const ULONGLONG now = NowMs();
    if (g_nextEntryTick <= now)
        return 0;
    const ULONGLONG delta = g_nextEntryTick - now;
    return static_cast<int>((delta + 999ULL) / 1000ULL);
}

void ScheduleNext(int seconds)
{
    g_nextEntryTick =
        NowMs() + static_cast<ULONGLONG>(std::max(1, seconds)) * 1000ULL;
    g_scheduled = true;
}

void EnterOverlayNow()
{
    if (g_overlay.IsActive())
        return;
    if (g_notice.IsVisible())
        g_notice.Destroy();
    g_scheduled = false;
    if (!g_overlay.Create(g_owner, BackgroundMode()))
        ScheduleNext(IntervalMinutes() * 60);
}

void ExitOverlayNow()
{
    if (!g_overlay.IsActive())
    {
        if (!g_scheduled)
            ScheduleNext(IntervalMinutes() * 60);
        return;
    }
    g_overlay.Destroy();
    ScheduleNext(IntervalMinutes() * 60);
}

void PostponeNoticeFromUi()
{
    if (g_notice.IsVisible())
        g_notice.Destroy();
    ScheduleNext(PostponeMinutes() * 60);
}

void BlockBecauseOfGame()
{
    if (g_notice.IsVisible())
        g_notice.Destroy();
    ScheduleNext(static_cast<int>(kGameBlockPostponeSeconds));
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------
int ReadEnvInt(const wchar_t* name, int fallback)
{
    wchar_t buffer[32] = {};
    const DWORD length = GetEnvironmentVariableW(name, buffer, 32);
    if (length == 0 || length >= 32)
        return fallback;
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    if (end == buffer)
        return fallback;
    return static_cast<int>(value);
}

int ParseBackgroundToken(const wchar_t* token)
{
    if (token == nullptr)
        return -1;
    if (_wcsicmp(token, L"landscape") == 0)
        return SCREENSAVER_BG_LANDSCAPE;
    if (_wcsicmp(token, L"solid") == 0)
        return SCREENSAVER_BG_SOLID;
    if (_wcsicmp(token, L"gradient") == 0)
        return SCREENSAVER_BG_GRADIENT;
    if (_wcsicmp(token, L"0") == 0)
        return SCREENSAVER_BG_LANDSCAPE;
    if (_wcsicmp(token, L"1") == 0)
        return SCREENSAVER_BG_SOLID;
    if (_wcsicmp(token, L"2") == 0)
        return SCREENSAVER_BG_GRADIENT;
    return -1;
}

int ReadEnvBackground()
{
    wchar_t buffer[32] = {};
    const DWORD length = GetEnvironmentVariableW(L"DSH_SCREENSAVER_BG", buffer, 32);
    if (length == 0 || length >= 32)
        return -1;
    return ParseBackgroundToken(buffer);
}

int ParseBackgroundArgument(LPCWSTR commandLine)
{
    const wchar_t* flag = wcsstr(commandLine, L"--test-screensaver-overlay");
    if (flag == nullptr)
        return -1;
    const wchar_t* cursor = flag + wcslen(L"--test-screensaver-overlay");
    while (*cursor == L' ' || *cursor == L'\t')
        ++cursor;
    wchar_t token[32] = {};
    int count = 0;
    while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t' &&
           count < 31)
    {
        token[count++] = *cursor++;
    }
    return ParseBackgroundToken(token);
}

void ReadTestHooks()
{
    g_testDurationSeconds = ReadEnvInt(L"DSH_SCREENSAVER_TEST_SECONDS", 0);
    if (g_testDurationSeconds < 0)
        g_testDurationSeconds = 0;

    g_backgroundOverride = ReadEnvBackground();

    LPCWSTR commandLine = GetCommandLineW();
    const bool overlayTest =
        wcsstr(commandLine, L"--test-screensaver-overlay") != nullptr;
    const bool noticeTest =
        wcsstr(commandLine, L"--test-screensaver-notice") != nullptr;

    if (overlayTest)
    {
        const int argumentBackground = ParseBackgroundArgument(commandLine);
        if (argumentBackground >= 0)
            g_backgroundOverride = argumentBackground;
        g_testMode = true;
        EnterOverlayNow();
        return;
    }

    if (noticeTest)
    {
        g_testMode = true;
        ScheduleNext(kScreensaverNoticeSeconds);
        if (!g_notice.Create(g_owner, SecondsUntilEntry()))
            ScheduleNext(IntervalMinutes() * 60);
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InitScreensaver(HWND ownerHwnd)
{
    g_owner = ownerHwnd;
    EnsureFamilies();
    g_rng = static_cast<unsigned int>(GetTickCount64() & 0xFFFFFFFFu) | 1u;
    g_initialised = true;

    ReadTestHooks();

    if (!g_testMode && !g_overlay.IsActive() && !g_notice.IsVisible() &&
        g_settings.screensaverEnable)
    {
        ScheduleNext(IntervalMinutes() * 60);
    }
}

void ShutdownScreensaver()
{
    g_overlay.Destroy();
    g_notice.Destroy();
    ReleaseFamilies();
    g_initialised = false;
    g_scheduled = false;
    g_nextEntryTick = 0;
}

void ScreensaverOnTick()
{
    if (!g_initialised)
        return;

    const ULONGLONG now = NowMs();
    const bool enabled = g_settings.screensaverEnable || g_testMode;

    // The overlay owns the screen; only the game check is needed from here.
    if (g_overlay.IsActive())
    {
        if (g_settings.screensaverBlockInGame && IsGameFullscreenActive())
            ExitOverlayNow();
        return;
    }

    if (!enabled)
    {
        if (g_notice.IsVisible())
            g_notice.Destroy();
        g_scheduled = false;
        return;
    }

    // Games have priority: postpone silently (5 min) instead of showing the
    // reminder in front of a fullscreen game.
    if (g_settings.screensaverBlockInGame && IsGameFullscreenActive())
    {
        if (g_notice.IsVisible())
        {
            BlockBecauseOfGame();
        }
        else if (!g_scheduled ||
                 now + static_cast<ULONGLONG>(kScreensaverNoticeSeconds) * 1000ULL >=
                     g_nextEntryTick)
        {
            ScheduleNext(static_cast<int>(kGameBlockPostponeSeconds));
        }
        return;
    }

    if (!g_scheduled)
        ScheduleNext(IntervalMinutes() * 60);

    if (g_notice.IsVisible())
    {
        if (g_nextEntryTick <= now)
        {
            EnterOverlayNow();
            return;
        }
        g_notice.UpdateCountdown(SecondsUntilEntry());
        return;
    }

    if (g_nextEntryTick <= now)
    {
        EnterOverlayNow();
        return;
    }
    if (g_nextEntryTick - now <=
        static_cast<ULONGLONG>(kScreensaverNoticeSeconds) * 1000ULL)
    {
        if (!g_notice.Create(g_owner, SecondsUntilEntry()))
            ScheduleNext(IntervalMinutes() * 60);
    }
}

void ScreensaverApplySettings()
{
    if (!g_initialised || g_testMode)
        return;

    if (!g_settings.screensaverEnable)
    {
        if (g_overlay.IsActive())
            g_overlay.Destroy();
        if (g_notice.IsVisible())
            g_notice.Destroy();
        g_scheduled = false;
        return;
    }

    if (g_overlay.IsActive())
    {
        g_overlay.SetBackgroundMode(BackgroundMode());
        return;
    }
    if (g_notice.IsVisible())
        return;
    ScheduleNext(IntervalMinutes() * 60);
}

bool IsScreensaverActive()
{
    return g_overlay.IsActive();
}

bool IsScreensaverNoticeVisible()
{
    return g_notice.IsVisible();
}
