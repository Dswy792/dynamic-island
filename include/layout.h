#pragma once

#include <windows.h>

// Geometry of the island in screen pixels. The window is larger than the
// visible capsule because the shadow is drawn inside the layered window.
struct IslandGeometry
{
    RECT window;  // whole layered window (capsule + shadow padding)
    RECT capsule; // visible capsule
    int dpi;
};

// Store the island HWND and initialise DPI information.
void InitIslandLayout(HWND hwnd);

// Current DPI of the island window (96 = 100%).
UINT GetIslandDpi();

// Convert device-independent pixels to physical pixels using the island DPI.
int ScaleDip(int dip);
int ScaleDipForDpi(int dip, UINT dpi);

// Work area of the primary monitor, in physical pixels.
RECT GetIslandWorkArea();

// Where the capsule rests when it is fully visible and not hovered.
RECT GetIslandRestingCapsuleRect();

// --- v5.1 message sub-islands ------------------------------------------------
// Up to two circular sub-islands (WeChat = index 0, QQ = index 1) pop out on the
// right side of the main capsule. The whole cluster stays centred on the main
// capsule's resting centre.
constexpr int kSubIslandSourceCount = 2;
constexpr int kSubIslandGapDip = 8;       // gap between capsule / circles
constexpr int kSubIslandTooltipWidthDip = 300;
constexpr int kSubIslandTooltipHeightDip = 34;

// --- v5.4 hover feature row and recording overlays ---------------------------
// A small pill with up to four feature icons slides out below the main capsule
// while it is hovered (currently only the "会议 / 视频纪要" icon is enabled).
// The transparent layered window permanently reserves the area of the row, the
// countdown and the result message so that hovering never resizes the window
// (transparent pixels are click-through, so the reserved area stays harmless).
constexpr int kFeatureSlotCount = 4;
constexpr int kFeatureSlotSizeDip = 30;   // square hit area of one icon
constexpr int kFeatureRowHeightDip = 36;  // pill height
constexpr int kFeatureRowPaddingDip = 3;  // inner horizontal padding
constexpr int kFeatureRowGapDip = 7;      // gap between capsule and pill
constexpr int kFeatureRowMinWidthDip = 74; // pill stays a pill with one icon
constexpr int kFeatureRowShiftDip = 6;    // pop-out slide distance

// Countdown / result message pill shown below the capsule (same slot as the
// feature row; only one of them is visible at a time).
constexpr int kOverlayPillHeightDip = 32;
constexpr int kCountdownPillWidthDip = 178;
constexpr int kToastPillWidthDip = 560; // fits the partial-failure message

// Recording activity circle (right of the message sub-islands) and its tooltip.
constexpr int kActivityTooltipWidthDip = 268;
constexpr int kActivityTooltipHeightDip = 32;

// Runtime state that influences the geometry of the v5.4 overlays. The renderer
// fills this in (it owns the text / hover state) and main.cpp reuses it for hit
// testing so both always agree.
struct IslandOverlayState
{
    int featureSlotCount = 0;          // enabled feature icons (0..4), 0 = hidden
    bool countdownVisible = false;     // countdown pill is on screen
    bool toastVisible = false;         // result / error message pill is on screen
    bool activityCircleVisible = false; // recording circle target is visible
    bool activityTooltipVisible = false; // recording tooltip is on screen
    float activityProgress = 0.0f;     // 0..1 eased pop animation of the circle
};

struct IslandClusterGeometry
{
    RECT window;                              // whole layered window (shadow + tooltip)
    RECT capsule;                             // main clock capsule
    RECT subIslands[kSubIslandSourceCount];   // circle bounding boxes (empty = hidden)
    RECT tooltip;                             // message tooltip capsule (empty = hidden)
    RECT featureRow;                          // feature pill (empty = hidden)
    RECT featureSlots[kFeatureSlotCount];     // per-icon hit areas (empty = hidden)
    RECT countdown;                           // countdown pill (empty = hidden)
    RECT toast;                               // result / error pill (empty = hidden)
    RECT activityCircle;                      // recording circle (empty = hidden)
    RECT activityTooltip;                     // recording tooltip (empty = hidden)
    int dpi;
};

// subProgress: eased 0..1 per source index (0 = WeChat, 1 = QQ)
// hoveredSub:  index of the hovered sub-island, or -1
// tooltipVisible: draw the message tooltip below the hovered sub-island
IslandClusterGeometry ComputeIslandClusterGeometry(
    float hoverProgress, float hideProgress,
    const float subProgress[kSubIslandSourceCount], int hoveredSub,
    bool tooltipVisible, const IslandOverlayState& overlays);

// Trigger area for auto-hide / reveal, including the space the sub-islands and
// the tooltip can occupy.
RECT GetIslandClusterTriggerRect();

// Area used to detect the mouse for auto-hide / reveal (resting capsule plus
// a small padding so the user does not need pixel precision).
RECT GetIslandTriggerRect();

// Move the layered window so that it matches the given geometry.
void ApplyIslandWindowGeometry(HWND hwnd, const IslandGeometry& geometry);

// Forget the custom (dragged) position; the next layout uses the preset.
void ClearCustomPosition();

// True when the point is inside the rectangle.
bool PointInRect(const RECT& rect, POINT pt);

// Inflate a rectangle (returns a copy; RECT may be "empty").
RECT InflateRectCopy(const RECT& rect, int dx, int dy);
