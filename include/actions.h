#pragma once

#include <windows.h>

// Open the Windows "Date & time" control panel page.
void OpenDateAndTimeSettings();

// Show the about dialog (version, links).
void ShowAboutDialog(HWND owner);

// True when the foreground window covers an entire monitor (borderless or
// DWM-composited fullscreen video, browser fullscreen, presentations...).
bool IsFullscreenAppActive();

// True when Windows reports an exclusive Direct3D full-screen application
// (SHQueryUserNotificationState == QUNS_RUNNING_D3D_FULL_SCREEN), i.e. a game
// running in exclusive fullscreen.
bool IsD3DFullscreenAppActive();

// True when the foreground app looks like a game in fullscreen: either the
// exclusive Direct3D state above, or a fullscreen window using a well-known
// game engine window class (Unity / Unreal / SDL / Source ...). The island
// hides for games but stays above video players and other fullscreen apps.
bool IsGameFullscreenActive();
