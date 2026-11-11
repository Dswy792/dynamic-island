#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <string>

#include "app_state.h"
#include "layout.h"
#include "recorder.h"
#include "theme.h"

// Per-pixel alpha layered renderer (GDI+ -> premultiplied DIB -> UpdateLayeredWindow).
void InitIslandRenderer(HWND hwnd);
void ShutdownIslandRenderer();

// Render the current frame. When force is false the frame is skipped if the
// visible content and geometry did not change.
void RenderIslandFrame(bool force);

// Mark the content as stale (time / theme / settings changed).
void InvalidateIslandContent();

// Latest message sender / body per sub-island source index (0 = WeChat, 1 = QQ).
// Used by the hover tooltip when "show content" is enabled.
void SetSubIslandContent(int sourceIndex, const std::wstring& title,
                         const std::wstring& body);

// Index of the sub-island currently under the mouse (-1 = none). Controls the
// hover ring and the message tooltip.
void SetHoveredSubIsland(int sourceIndex);

// --- v5.4 hover feature row -------------------------------------------------
// enabledSlots: number of interactive feature icons (0..kFeatureSlotCount),
//               0 hides the whole row.
// hoveredSlot:  index of the icon under the mouse, or -1.
// The row only becomes visible while the island is hovered; the renderer reads
// the eased progress from the animation module.
void SetFeatureRowInfo(int enabledSlots, int hoveredSlot);

// --- v5.4 countdown / result message pill ----------------------------------
// Shown in the same slot below the capsule as the feature row. An empty string
// hides the pill.
void SetIslandCountdownText(const std::wstring& text);
void SetIslandToast(const std::wstring& text, bool error);

// --- v5.4 recording activity circle ---------------------------------------
// Everything the activity circle needs. main.cpp refills it from the recorder
// state (or from the built-in visual test mode) on every pulse timer tick.
struct RecordingActivityVisual
{
    bool visible = false;         // circle should be on screen
    RecorderState state = RecorderState::Idle;
    int elapsedSeconds = 0;
    int limitSeconds = 0;
    bool hovered = false;         // mouse over the circle -> tooltip
    float pressProgress = 0.0f;   // 0..1 long-press-to-stop ring
    int pulseTick = 0;            // free-running tick driving the icon animation
    std::wstring tooltip;         // hover tooltip text (empty = no tooltip)
};

void SetRecordingActivityVisual(const RecordingActivityVisual& visual);

// Fill the overlay state used by ComputeIslandClusterGeometry. Exposed so that
// main.cpp hit-tests exactly the geometry the renderer draws.
void GetCurrentIslandOverlays(IslandOverlayState& overlays);

// Draw the island's material (soft shadow, gradient fill and hairline border)
// into an arbitrary GDI+ context. Shared with the custom recording dialogs so
// they use exactly the same visual language as the island and the settings
// preview.
void RenderIslandMaterialRect(Gdiplus::Graphics& graphics,
                              const Gdiplus::RectF& rect, float radiusDip,
                              UINT dpi, int opacityPercent);

// Draws exactly the same capsule (shadow, gradient, border, fitted clock text)
// as the live island, at its real DPI-scaled size, centred in bounds. Used by
// the settings window preview so it matches the real thing pixel for pixel.
void RenderIslandPreview(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds,
                         UINT dpi, const AppSettings& settings,
                         const ThemePalette& palette);
