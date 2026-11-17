#pragma once

#include <windows.h>

// Built-in rest reminder / screensaver (v5.5).
//
// Periodically shows a 10-second notice under the island, then a full-screen
// topmost overlay with a quote and a background (landscape / solid / gradient).
// The overlay never suspends other processes; it only covers the screen. The
// user can enter immediately, postpone, exit early (x / Esc) or extend.
//
// The module is driven by the island's content timer (ScreensaverOnTick) and
// reads its options from g_settings (app_state.h).
void InitScreensaver(HWND ownerHwnd);
void ShutdownScreensaver();
void ScreensaverOnTick();
void ScreensaverApplySettings();
bool IsScreensaverActive();
bool IsScreensaverNoticeVisible();

// Enter the full-screen screensaver immediately (manual trigger from the island
// feature row: no notice / countdown, uses the configured duration). Does
// nothing when the overlay is already active or when a fullscreen game blocks it
// (screensaverBlockInGame).
void StartScreensaverNow();
