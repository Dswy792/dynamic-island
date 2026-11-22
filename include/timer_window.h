#pragma once

#include <windows.h>

// v5.7/5.8 built-in timer feature (stopwatch + countdown with H/M/S wheels).
//
// A modeless, island-material layered popup under the island capsule. Close /
// Esc only hide it: both timers keep running as compact mini sub-islands under
// the island until reset. The countdown posts kMsgTimerFinished to the owner
// when it reaches zero so the island can show a toast.
constexpr UINT kMsgTimerFinished = WM_APP + 210;

// Store the owner window and register the timer windows. Also honours the
// internal --test-timer* visual test hooks.
void InitTimerWindow(HWND ownerHwnd);

// Destroy the panel and the mini sub-islands, stopping every timer.
void ShutdownTimerWindow();

// Create the panel on first use, then show / focus it (current tab).
void ShowTimerWindow(HWND ownerHwnd);

// Hide the panel without stopping the timers (close button / Esc).
void HideTimerWindow();

// True while the panel is visible.
bool IsTimerWindowOpen();
