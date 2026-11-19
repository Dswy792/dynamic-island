#pragma once

#include <windows.h>

// v5.7 built-in timer window (stopwatch + countdown).
//
// A modeless, island-material layered popup that lives under the island
// capsule. Close / Esc only hide it: both timers keep running and the hover
// feature icon reopens the panel. The countdown posts kMsgTimerFinished to the
// owner when it reaches zero so the island can show a toast.
constexpr UINT kMsgTimerFinished = WM_APP + 210;

// Store the owner window and register the timer window class. Also honours the
// internal --test-timer* visual test hooks.
void InitTimerWindow(HWND ownerHwnd);

// Destroy the window and stop every timer (application exit).
void ShutdownTimerWindow();

// Create the panel on first use, then show / focus it.
void ShowTimerWindow(HWND ownerHwnd);

// Hide the panel without stopping the timers (close button / Esc).
void HideTimerWindow();

// True while the panel is visible.
bool IsTimerWindowOpen();
