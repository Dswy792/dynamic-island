#pragma once

#include <windows.h>

#include <string>

// Custom, fully owner-drawn recording dialogs (v5.4).
//
// Both windows are popup layered windows without a title bar and never appear
// in the taskbar. The rounded card, its soft shadow, the buttons and (for the
// duration dialog) the "1 小时 / 2 小时 / 不限制" wheel are rendered with GDI+
// and presented with UpdateLayeredWindow - exactly like the island and the
// settings window - so the recording UI uses the same visual language.
//
// Callbacks run on the UI thread after the dialog has already been destroyed, so
// the callback may immediately open the next dialog.
//
//   confirm dialog callback : limitSeconds == -1  (the user pressed 开始)
//   duration dialog callback: limitSeconds == 3600 / 7200 / 0 (unlimited)
//
// Cancelling (取消 / Esc / closing) does not invoke the callback.
typedef void (*RecordingDialogCallback)(int limitSeconds, void* context);

// Show the confirmation step ("取消" / "开始").
void ShowRecordingConfirmDialog(HWND owner, RecordingDialogCallback callback,
                                void* context);

// Show the duration wheel step ("1 小时" / "2 小时" / "不限制" + 取消 / 开始记录).
void ShowRecordingDurationDialog(HWND owner, RecordingDialogCallback callback,
                                 void* context);

// Show the "AI endpoint / API key not configured" notice. The callback receives
// -2 when the user presses 打开设置 (the caller opens the settings window).
void ShowRecordingConfigNoticeDialog(HWND owner, RecordingDialogCallback callback,
                                     void* context);

// True while one of the recording dialogs is open.
bool IsRecordingDialogOpen();

// Close the open dialog without invoking its callback (application shutdown,
// settings window opened, ...).
void CloseRecordingDialogs();

// <g_settings.recordSaveDir> when configured, otherwise <exe folder>\recordings.
std::wstring ResolveRecordingSaveDirectory();

// Create the directory recursively (SHCreateDirectoryExW). Returns false and
// fills errorText with a human readable reason when that failed.
bool EnsureRecordingDirectory(const std::wstring& directory,
                              std::wstring& errorText);
