#include "layout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "app_state.h"

namespace
{
// Capsule sizes in device-independent pixels (96 DPI).
constexpr int kBaseWidthDip[ISLAND_SIZE_COUNT] = {120, 150, 180};
constexpr int kBaseHeightDip[ISLAND_SIZE_COUNT] = {24, 28, 32};

// Transparent padding inside the layered window that holds the soft shadow.
constexpr int kShadowHorizontalDip = 16;
constexpr int kShadowTopDip = 10;
constexpr int kShadowBottomDip = 20;

constexpr int kTopMarginDip = 6;
constexpr int kSideMarginDip = 12;
constexpr int kTriggerPaddingDip = 10;
constexpr float kHoverScale = 1.08f;

HWND s_hwnd = nullptr;
UINT s_dpi = 96;

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

int ClampValue(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

float ClampProgress(float value)
{
    if (!(value > 0.0f)) // also catches NaN
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

RECT MakeRect(int left, int top, int width, int height)
{
    RECT rect = {left, top, left + width, top + height};
    return rect;
}

bool HasCustomPosition()
{
    return g_settings.position == ISLAND_POS_CUSTOM && g_settings.customX > -32000 &&
           g_settings.customY > -32000;
}

RECT GetMonitorWorkAreaForPoint(POINT point)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
        return info.rcWork;

    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}
} // namespace

void InitIslandLayout(HWND hwnd)
{
    s_hwnd = hwnd;
    s_dpi = QueryWindowDpi(hwnd);
    if (s_dpi == 0)
        s_dpi = 96;
}

UINT GetIslandDpi()
{
    return s_dpi;
}

int ScaleDipForDpi(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

int ScaleDip(int dip)
{
    return ScaleDipForDpi(dip, s_dpi);
}

RECT GetIslandWorkArea()
{
    RECT workArea = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) ||
        workArea.right <= workArea.left || workArea.bottom <= workArea.top)
    {
        workArea.left = 0;
        workArea.top = 0;
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    return workArea;
}

RECT GetIslandRestingCapsuleRect()
{
    const int widthDip = kBaseWidthDip[ClampValue(g_settings.windowSize, 0, ISLAND_SIZE_COUNT - 1)];
    const int heightDip =
        kBaseHeightDip[ClampValue(g_settings.windowSize, 0, ISLAND_SIZE_COUNT - 1)];
    const int width = ScaleDip(widthDip);
    const int height = ScaleDip(heightDip);

    int left = 0;
    int top = 0;

    if (HasCustomPosition())
    {
        POINT point = {g_settings.customX, g_settings.customY};
        const RECT workArea = GetMonitorWorkAreaForPoint(point);
        left = ClampValue(g_settings.customX, workArea.left, workArea.right - width);
        top = ClampValue(g_settings.customY, workArea.top, workArea.bottom - height);
    }
    else
    {
        const RECT workArea = GetIslandWorkArea();
        const int available = workArea.right - workArea.left;
        switch (g_settings.position)
        {
        case ISLAND_POS_LEFT:
            left = workArea.left + ScaleDip(kSideMarginDip);
            break;
        case ISLAND_POS_RIGHT:
            left = workArea.right - width - ScaleDip(kSideMarginDip);
            break;
        default:
            left = workArea.left + (available - width) / 2;
            break;
        }
        top = workArea.top + ScaleDip(kTopMarginDip);
    }

    return MakeRect(left, top, width, height);
}

namespace
{
RECT UnionRectCopy(const RECT& a, const RECT& b)
{
    if (a.right <= a.left || a.bottom <= a.top)
        return b;
    if (b.right <= b.left || b.bottom <= b.top)
        return a;
    RECT result = a;
    result.left = std::min(a.left, b.left);
    result.top = std::min(a.top, b.top);
    result.right = std::max(a.right, b.right);
    result.bottom = std::max(a.bottom, b.bottom);
    return result;
}

bool IsEmptyRect(const RECT& rect)
{
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

// Area the capsule shadow can paint into. The layered window always covers this
// rectangle so the soft shadow is never clipped.
RECT ShadowBounds(const RECT& capsule)
{
    RECT area = capsule;
    InflateRect(&area, ScaleDip(kShadowHorizontalDip), 0);
    area.top -= ScaleDip(kShadowTopDip);
    area.bottom += ScaleDip(kShadowBottomDip);
    return area;
}

// Circles (sub-islands / recording activity) reserve a little extra room for
// their shadow.
RECT CircleShadowBounds(const RECT& circle)
{
    RECT area = circle;
    InflateRect(&area, ScaleDip(10), ScaleDip(10));
    area.top += ScaleDip(2);
    area.bottom += ScaleDip(6);
    return area;
}

// Pills below the island (feature row, countdown, toast) reserve room for their
// own soft shadow.
RECT PillShadowBounds(const RECT& pill)
{
    RECT area = pill;
    InflateRect(&area, ScaleDip(12), ScaleDip(6));
    area.bottom += ScaleDip(10);
    return area;
}

void ShiftRect(RECT& rect, int dx, int dy)
{
    if (IsEmptyRect(rect))
        return;
    rect.left += dx;
    rect.right += dx;
    rect.top += dy;
    rect.bottom += dy;
}

void ClampRectHorizontally(RECT& rect, const RECT& workArea)
{
    if (IsEmptyRect(rect))
        return;
    const int width = rect.right - rect.left;
    if (width >= workArea.right - workArea.left)
    {
        rect.left = workArea.left;
        rect.right = workArea.left + width;
        return;
    }
    if (rect.right > workArea.right)
    {
        rect.left -= rect.right - workArea.right;
        rect.right = workArea.right;
    }
    if (rect.left < workArea.left)
    {
        rect.right += workArea.left - rect.left;
        rect.left = workArea.left;
    }
}
} // namespace

RECT InflateRectCopy(const RECT& rect, int dx, int dy)
{
    RECT result = rect;
    InflateRect(&result, dx, dy);
    return result;
}

IslandClusterGeometry ComputeIslandClusterGeometry(
    float hoverProgress, float hideProgress,
    const float subProgress[kSubIslandSourceCount], int hoveredSub,
    bool tooltipVisible, const IslandOverlayState& overlays)
{
    const RECT resting = GetIslandRestingCapsuleRect();
    const RECT workArea = GetIslandWorkArea();

    hoverProgress = ClampProgress(hoverProgress);
    hideProgress = ClampProgress(hideProgress);

    const float scale = 1.0f + (kHoverScale - 1.0f) * hoverProgress;
    const int restingWidth = resting.right - resting.left;
    const int restingHeight = resting.bottom - resting.top;
    const int hoverWidth = std::max(1, static_cast<int>(std::lround(restingWidth * kHoverScale)));
    const int hoverHeight = std::max(1, static_cast<int>(std::lround(restingHeight * kHoverScale)));
    const int capsuleWidth =
        std::max(1, static_cast<int>(std::lround(restingWidth * scale)));
    const int capsuleHeight =
        std::max(1, static_cast<int>(std::lround(restingHeight * scale)));
    const int centerX = (resting.left + resting.right) / 2;

    const int circleDiameter = capsuleHeight;
    const int gap = ScaleDip(kSubIslandGapDip);

    // Cluster width = main capsule + every visible sub-island + the recording
    // activity circle (scaled by its own progress). The whole cluster is centred
    // on the resting centre.
    float clusterWidth = static_cast<float>(capsuleWidth);
    float progress[kSubIslandSourceCount] = {};
    for (int i = 0; i < kSubIslandSourceCount; ++i)
    {
        progress[i] = ClampProgress(subProgress[i]);
        if (progress[i] > 0.001f)
            clusterWidth += gap * progress[i] +
                            static_cast<float>(circleDiameter) * progress[i];
    }

    const float activityProgress = ClampProgress(overlays.activityProgress);
    const bool activityVisible =
        overlays.activityCircleVisible || activityProgress > 0.001f;
    if (activityVisible)
        clusterWidth += gap * activityProgress +
                        static_cast<float>(circleDiameter) * activityProgress;

    const int hiddenDistance = (resting.top - workArea.top) + capsuleHeight +
                               ScaleDip(kShadowBottomDip) + ScaleDip(4);
    const int capsuleTop =
        resting.top - static_cast<int>(std::lround(hiddenDistance * hideProgress));
    const int capsuleCenterY = capsuleTop + capsuleHeight / 2;
    const int clusterLeft =
        centerX - static_cast<int>(std::lround(clusterWidth / 2.0f));

    IslandClusterGeometry geometry = {};
    geometry.dpi = static_cast<int>(s_dpi);
    geometry.capsule =
        MakeRect(clusterLeft, capsuleTop, capsuleWidth, capsuleHeight);
    // The capsule itself stays horizontally centred on the resting centre only
    // while the cluster has no circles; the hover reservation below must follow
    // the capsule, not the resting centre.
    const int capsuleCentreX = clusterLeft + capsuleWidth / 2;

    float cursorX = static_cast<float>(geometry.capsule.right);
    for (int i = 0; i < kSubIslandSourceCount; ++i)
    {
        if (progress[i] <= 0.001f)
            continue;
        const float diameter = static_cast<float>(circleDiameter) * progress[i];
        const float gapWidth = static_cast<float>(gap) * progress[i];
        const float circleCenterX = cursorX + gapWidth + diameter / 2.0f;
        const int radius = static_cast<int>(std::lround(diameter / 2.0f));
        geometry.subIslands[i] = MakeRect(
            static_cast<int>(std::lround(circleCenterX)) - radius,
            capsuleCenterY - radius, radius * 2, radius * 2);
        cursorX += gapWidth + diameter;
    }

    // v5.4: the recording activity circle sits to the right of the message
    // sub-islands and pops out with its own progress.
    if (activityVisible && activityProgress > 0.001f)
    {
        const float diameter = static_cast<float>(circleDiameter) * activityProgress;
        const float gapWidth = static_cast<float>(gap) * activityProgress;
        const float circleCenterX = cursorX + gapWidth + diameter / 2.0f;
        const int radius = static_cast<int>(std::lround(diameter / 2.0f));
        geometry.activityCircle = MakeRect(
            static_cast<int>(std::lround(circleCenterX)) - radius,
            capsuleCenterY - radius, radius * 2, radius * 2);
    }

    // -----------------------------------------------------------------------
    // v5.4 overlays below the capsule: feature row + countdown / toast pills.
    // They share one vertical slot. The slot is anchored to the *fully hovered*
    // capsule bottom so nothing jumps while the hover animation plays.
    // -----------------------------------------------------------------------
    const int overlayCentreX = (geometry.capsule.left + geometry.capsule.right) / 2;
    const int overlayTop =
        capsuleTop + hoverHeight + ScaleDip(kFeatureRowGapDip);
    const int rowHeight = ScaleDip(kFeatureRowHeightDip);
    const int slotSize = ScaleDip(kFeatureSlotSizeDip);
    const int slotPadding = ScaleDip(kFeatureRowPaddingDip);
    const int enabledSlots =
        ClampValue(overlays.featureSlotCount, 0, kFeatureSlotCount);
    const int rowWidth =
        enabledSlots > 0
            ? std::max(enabledSlots * slotSize + slotPadding * 2,
                       ScaleDip(kFeatureRowMinWidthDip))
            : 0;
    if (enabledSlots > 0)
    {
        geometry.featureRow =
            MakeRect(overlayCentreX - rowWidth / 2, overlayTop, rowWidth, rowHeight);
        for (int i = 0; i < enabledSlots; ++i)
        {
            const int slotCentreX = overlayCentreX + static_cast<int>(std::lround(
                (static_cast<float>(i) -
                 static_cast<float>(enabledSlots - 1) * 0.5f) *
                static_cast<float>(slotSize)));
            geometry.featureSlots[i] =
                MakeRect(slotCentreX - slotSize / 2,
                         overlayTop + (rowHeight - slotSize) / 2, slotSize,
                         slotSize);
        }
    }

    const int pillHeight = ScaleDip(kOverlayPillHeightDip);
    const int pillTop = overlayTop + (rowHeight - pillHeight) / 2;
    const int countdownWidth = ScaleDip(kCountdownPillWidthDip);
    const int toastWidth = ScaleDip(kToastPillWidthDip);
    if (overlays.countdownVisible)
        geometry.countdown =
            MakeRect(overlayCentreX - countdownWidth / 2, pillTop,
                     countdownWidth, pillHeight);
    if (overlays.toastVisible)
        geometry.toast = MakeRect(overlayCentreX - toastWidth / 2, pillTop,
                                  toastWidth, pillHeight);

    // Message tooltip below the hovered sub-island.
    if (tooltipVisible && hoveredSub >= 0 && hoveredSub < kSubIslandSourceCount &&
        !IsEmptyRect(geometry.subIslands[hoveredSub]))
    {
        const int tooltipWidth = ScaleDip(kSubIslandTooltipWidthDip);
        const int tooltipHeight = ScaleDip(kSubIslandTooltipHeightDip);
        const int centerXTooltip = (geometry.subIslands[hoveredSub].left +
                                    geometry.subIslands[hoveredSub].right) / 2;
        const int left = centerXTooltip - tooltipWidth / 2;
        const int top = geometry.capsule.bottom + ScaleDip(kSubIslandGapDip);
        geometry.tooltip = MakeRect(left, top, tooltipWidth, tooltipHeight);
    }

    // Recording tooltip below the activity circle.
    if (overlays.activityTooltipVisible && !IsEmptyRect(geometry.activityCircle))
    {
        const int tooltipWidth = ScaleDip(kActivityTooltipWidthDip);
        const int tooltipHeight = ScaleDip(kActivityTooltipHeightDip);
        const int centerXTooltip =
            (geometry.activityCircle.left + geometry.activityCircle.right) / 2;
        const int top = geometry.capsule.bottom + ScaleDip(kSubIslandGapDip);
        geometry.activityTooltip = MakeRect(centerXTooltip - tooltipWidth / 2, top,
                                           tooltipWidth, tooltipHeight);
    }

    // -----------------------------------------------------------------------
    // Keep the visible composition inside the monitor work area horizontally.
    // The reserved (transparent) window area is allowed to run off screen: it
    // never paints anything, and shrinking the composition around it would move
    // the island away from its configured position.
    // -----------------------------------------------------------------------
    RECT content = ShadowBounds(geometry.capsule);
    content = UnionRectCopy(content, ShadowBounds(MakeRect(
        capsuleCentreX - hoverWidth / 2, capsuleTop, hoverWidth, hoverHeight)));
    for (int i = 0; i < kSubIslandSourceCount; ++i)
    {
        if (IsEmptyRect(geometry.subIslands[i]))
            continue;
        content = UnionRectCopy(content, CircleShadowBounds(geometry.subIslands[i]));
    }
    if (!IsEmptyRect(geometry.activityCircle))
        content = UnionRectCopy(content, CircleShadowBounds(geometry.activityCircle));
    if (!IsEmptyRect(geometry.tooltip))
        content = UnionRectCopy(content, PillShadowBounds(geometry.tooltip));

    int shiftX = 0;
    if (content.right > workArea.right)
        shiftX = static_cast<int>(workArea.right) - content.right;
    if (content.left + shiftX < workArea.left)
        shiftX = static_cast<int>(workArea.left) - content.left;
    if (shiftX != 0)
    {
        ShiftRect(geometry.capsule, shiftX, 0);
        for (int i = 0; i < kSubIslandSourceCount; ++i)
            ShiftRect(geometry.subIslands[i], shiftX, 0);
        ShiftRect(geometry.tooltip, shiftX, 0);
        ShiftRect(geometry.activityCircle, shiftX, 0);
        ShiftRect(geometry.activityTooltip, shiftX, 0);
        ShiftRect(geometry.featureRow, shiftX, 0);
        for (int i = 0; i < kFeatureSlotCount; ++i)
            ShiftRect(geometry.featureSlots[i], shiftX, 0);
        ShiftRect(geometry.countdown, shiftX, 0);
        ShiftRect(geometry.toast, shiftX, 0);
    }
    ClampRectHorizontally(geometry.countdown, workArea);
    ClampRectHorizontally(geometry.toast, workArea);
    ClampRectHorizontally(geometry.activityTooltip, workArea);

    // -----------------------------------------------------------------------
    // The layered window permanently reserves:
    //   * the capsule shadow at rest and at full hover,
    //   * the feature-row / countdown / toast slot below the capsule,
    //   * the currently visible circles and tooltips.
    // Hovering therefore never resizes or moves the window; the unused area is
    // fully transparent and the OS lets mouse input pass through it.
    // -----------------------------------------------------------------------
    RECT window = ShadowBounds(geometry.capsule);
    window = UnionRectCopy(window, ShadowBounds(MakeRect(
        capsuleCentreX - hoverWidth / 2, capsuleTop, hoverWidth, hoverHeight)));
    for (int i = 0; i < kSubIslandSourceCount; ++i)
    {
        if (IsEmptyRect(geometry.subIslands[i]))
            continue;
        window = UnionRectCopy(window, CircleShadowBounds(geometry.subIslands[i]));
    }
    if (!IsEmptyRect(geometry.activityCircle))
        window = UnionRectCopy(window, CircleShadowBounds(geometry.activityCircle));
    if (!IsEmptyRect(geometry.tooltip))
        window = UnionRectCopy(window, PillShadowBounds(geometry.tooltip));
    if (!IsEmptyRect(geometry.activityTooltip))
        window = UnionRectCopy(window, PillShadowBounds(geometry.activityTooltip));

    // Reserved overlay slot (feature row band): always part of the window so a
    // hover, a countdown or a result message never resizes the surface.
    const int reservedCentreX =
        (geometry.capsule.left + geometry.capsule.right) / 2;
    const int bandWidth = std::max(
        static_cast<int>(std::lround(static_cast<float>(kFeatureSlotCount) *
                                         static_cast<float>(slotSize))) +
            slotPadding * 2,
        std::max(countdownWidth, toastWidth));
    const RECT band =
        MakeRect(reservedCentreX - bandWidth / 2, overlayTop, bandWidth, rowHeight);
    window = UnionRectCopy(window, PillShadowBounds(band));
    if (!IsEmptyRect(geometry.countdown))
        window = UnionRectCopy(window, PillShadowBounds(geometry.countdown));
    if (!IsEmptyRect(geometry.toast))
        window = UnionRectCopy(window, PillShadowBounds(geometry.toast));

    geometry.window = window;
    return geometry;
}

RECT GetIslandClusterTriggerRect()
{
    RECT trigger = GetIslandRestingCapsuleRect();
    const int padding = ScaleDip(kTriggerPaddingDip);
    InflateRect(&trigger, padding, padding);

    // Reserve room for two sub-islands / the recording circle on the right and
    // the tooltip plus the hover feature row below.
    const int restingHeightPx = trigger.bottom - trigger.top - 2 * padding;
    const int circleDiameter = restingHeightPx;
    const int gap = ScaleDip(kSubIslandGapDip);
    trigger.right +=
        (gap + circleDiameter) * kSubIslandSourceCount + ScaleDip(10);
    trigger.bottom += ScaleDip(kSubIslandTooltipHeightDip) + ScaleDip(20) +
                      ScaleDip(kFeatureRowHeightDip);

    const RECT workArea = GetIslandWorkArea();
    if (trigger.top > workArea.top)
        trigger.top = workArea.top;
    if (trigger.right > workArea.right)
        trigger.right = workArea.right;
    return trigger;
}

void ApplyIslandWindowGeometry(HWND hwnd, const IslandGeometry& geometry)
{
    if (hwnd == nullptr)
        return;

    SetWindowPos(hwnd, HWND_TOPMOST, geometry.window.left, geometry.window.top,
                 geometry.window.right - geometry.window.left,
                 geometry.window.bottom - geometry.window.top,
                 SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOSENDCHANGING);
}

void ClearCustomPosition()
{
    g_settings.position = ISLAND_POS_CENTER;
    g_settings.customX = -100000;
    g_settings.customY = -100000;
}

bool PointInRect(const RECT& rect, POINT pt)
{
    return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top &&
           pt.y < rect.bottom;
}
