#include <windows.h>
#include <gdiplus.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

#include "actions.h"
#include "animation.h"
#include "app_state.h"
#include "chat_watcher.h"
#include "island.h"
#include "layout.h"
#include "notifications.h"
#include "recorder.h"
#include "recording_dialogs.h"
#include "rendering.h"
#include "resource.h"
#include "settings.h"
#include "settings_dialog.h"
#include "theme.h"
#include "time_format.h"
#include "tray.h"
#include "version.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace
{
bool s_hovering = false;
bool s_dragging = false;
bool s_dragMoved = false;
POINT s_dragStartCursor = {};
RECT s_dragStartCapsule = {};
bool s_hiddenByFullscreen = false;
ULONGLONG s_lastInsideTick = 0;
int s_contentTickCount = 0;
UINT s_taskbarCreatedMessage = 0;

// Message sub-islands: at most one circle per source, each keeps its own dwell
// deadline. While the mouse hovers a circle its deadline is extended so the
// tooltip stays readable.
ULONGLONG s_subDeadline[kNotificationSourceCount] = {0, 0};
bool s_subDismissTimerRunning = false;
int s_pressedSub = -1;
int s_hoveredSub = -1;
bool s_lastNotifyEnable = false;

// --- v5.4 hover feature row -------------------------------------------------
// Only the "会议 / 视频纪要" slot is enabled today; the layout reserves room for
// up to four so adding more features later needs no geometry change.
constexpr int kEnabledFeatureSlots = 1;
constexpr UINT kTimerRecordingPulse = 7; // local to the island window
constexpr UINT kTimerIslandToast = 8;
constexpr int kRecordingPulseMs = 50;
constexpr ULONGLONG kActivityLongPressMs = 700;
constexpr ULONGLONG kToastVisibleMs = 4500;

bool s_forceHoverRowForTest = false; // internal --test-hover-row hook
bool s_hoverLeavePending = false;
int s_hoveredFeatureSlot = -1;
int s_pressedFeatureSlot = -1;

// --- v5.4 recording flow ----------------------------------------------------
bool s_countdownActive = false;
int s_countdownRemaining = 0;
int s_pendingLimitSeconds = kRecordingOneHourSeconds;
bool s_toastVisible = false;
bool s_toastTimerRunning = false;
ULONGLONG s_toastDeadlineTick = 0;

// --- v5.4 recording activity circle ----------------------------------------
bool s_activityVisible = false;
bool s_testRecordStart = false; // --test-record-start bypasses the AI config gate
bool s_activityHovered = false;
bool s_pressedActivity = false;
bool s_activityStopSent = false;
ULONGLONG s_activityPressTick = 0;
bool s_recordingPulseTimerRunning = false;
RecorderState s_lastRecorderState = RecorderState::Idle;
int s_lastElapsedSeconds = -1;
std::wstring s_activityTooltipText;

// Built-in visual demo for the recorder (--test-recording-circle and friends):
// the real recorder engine is asynchronous, so the UI states can be exercised
// without opening an audio device. Normal runs never touch s_demoRecording.
bool s_demoRecording = false;
bool s_demoAutoFinish = false;
RecorderState s_demoState = RecorderState::Idle;
ULONGLONG s_demoSegmentStartTick = 0;
ULONGLONG s_demoElapsedMs = 0;
ULONGLONG s_demoStateTick = 0;
std::wstring s_demoMessage;

// Forward declarations of the recording UI implementation (defined below the
// window procedure helpers so they can call RevealIsland / ShowIslandSettings).
RecorderState CurrentRecorderState();
int CurrentElapsedSeconds();
int CurrentLimitSeconds();
bool IsRecordingUiActive();
void RefreshRecordingVisual(bool force);
void TickRecordingCountdown();
void TickIslandToast();
void RefreshRecordingPulse();
void StopRecordingFromUi();
void ToggleRecordingPause();
void StartRecordingCountdown(int limitSeconds);
void OnFeatureSlotClicked(int slot);

void ApplyDpiAwareness()
{
    typedef BOOL(WINAPI * SetProcessDpiAwarenessContextFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
        return;

    auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext != nullptr)
    {
        // (HANDLE)-4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (setContext(reinterpret_cast<HANDLE>(-4)))
            return;
    }

    typedef BOOL(WINAPI * SetProcessDPIAwareFn)();
    auto setLegacy = reinterpret_cast<SetProcessDPIAwareFn>(
        GetProcAddress(user32, "SetProcessDPIAware"));
    if (setLegacy != nullptr)
        setLegacy();
}

void SaveSettingsQuietly()
{
    SaveSettings();
}

bool s_autoHideTimerRunning = false;

void UpdateAutoHideTimerState()
{
    // The timer is always required: apart from auto-hide it tracks fullscreen
    // apps (stay above video fullscreen, hide in exclusive D3D games).
    if (!s_autoHideTimerRunning)
    {
        s_lastInsideTick = GetTickCount64();
        SetTimer(g_hwnd, kTimerAutoHide, kAutoHideCheckIntervalMs, nullptr);
        s_autoHideTimerRunning = true;
    }
}

void RevealIsland()
{
    if (IsIslandHiddenTarget())
    {
        s_hiddenByFullscreen = false;
        SetIslandHiddenTarget(false);
    }
}

IslandClusterGeometry CurrentClusterGeometry()
{
    float progress[kNotificationSourceCount] = {GetSubIslandProgress(0),
                                                GetSubIslandProgress(1)};
    IslandOverlayState overlays;
    GetCurrentIslandOverlays(overlays);
    return ComputeIslandClusterGeometry(GetIslandHoverProgress(),
                                        GetIslandHideProgress(), progress,
                                        s_hoveredSub, s_hoveredSub >= 0,
                                        overlays);
}

// --- v5.4 hover feature row / recording circle hit testing -----------------
int HitTestFeatureSlot(POINT screenPoint)
{
    if (GetFeatureRowProgress() < 0.35f)
        return -1;
    const IslandClusterGeometry geometry = CurrentClusterGeometry();
    for (int i = 0; i < kFeatureSlotCount; ++i)
    {
        if (geometry.featureSlots[i].right <= geometry.featureSlots[i].left)
            continue;
        if (PointInRect(geometry.featureSlots[i], screenPoint))
            return i;
    }
    return -1;
}

bool HitTestActivityCircle(POINT screenPoint)
{
    if (!s_activityVisible || GetActivityCircleProgress() < 0.35f)
        return false;
    const IslandClusterGeometry geometry = CurrentClusterGeometry();
    return PointInRect(geometry.activityCircle, screenPoint);
}

// Area that keeps the hover state alive while the mouse crosses the fully
// transparent gap between the capsule and the feature row.
bool IsPointInHoverKeepZone(POINT screenPoint)
{
    const IslandClusterGeometry geometry = CurrentClusterGeometry();
    const int capsulePad = ScaleDip(10);
    if (PointInRect(InflateRectCopy(geometry.capsule, capsulePad, capsulePad),
                    screenPoint))
        return true;
    if (GetFeatureRowProgress() > 0.15f &&
        PointInRect(InflateRectCopy(geometry.featureRow, ScaleDip(6), ScaleDip(6)),
                    screenPoint))
        return true;
    return false;
}

int HitTestSubIsland(POINT screenPoint)
{
    if (!IsAnySubIslandVisible())
        return -1;
    const IslandClusterGeometry geometry = CurrentClusterGeometry();
    float progress[kNotificationSourceCount] = {GetSubIslandProgress(0),
                                                GetSubIslandProgress(1)};
    for (int i = 0; i < kNotificationSourceCount; ++i)
    {
        if (progress[i] < 0.35f)
            continue;
        if (PointInRect(geometry.subIslands[i], screenPoint))
            return i;
    }
    return -1;
}

void SetHoveredSub(int index)
{
    if (s_hoveredSub == index)
        return;
    s_hoveredSub = index;
    SetHoveredSubIsland(index);
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void EndDrag(bool commit);
void ShowIslandSettings();

void OnLButtonDownOnIsland(POINT cursor)
{
    // v5.4: recording activity circle (click = pause/resume, long press = stop).
    if (HitTestActivityCircle(cursor))
    {
        s_pressedActivity = true;
        s_activityStopSent = false;
        s_activityPressTick = GetTickCount64();
        SetCapture(g_hwnd);
        RefreshRecordingVisual(true);
        return;
    }

    // v5.4: hover feature row slot.
    const int featureSlot = HitTestFeatureSlot(cursor);
    if (featureSlot >= 0)
    {
        s_pressedFeatureSlot = featureSlot;
        SetCapture(g_hwnd);
        return;
    }

    const int sub = HitTestSubIsland(cursor);
    if (sub >= 0)
    {
        s_pressedSub = sub;
        SetCapture(g_hwnd);
        return;
    }

    const IslandClusterGeometry geometry = CurrentClusterGeometry();
    if (PointInRect(geometry.capsule, cursor))
    {
        s_dragging = true;
        s_dragMoved = false;
        s_dragStartCursor = cursor;
        s_dragStartCapsule = GetIslandRestingCapsuleRect();
        SetCapture(g_hwnd);
    }
}

void OnLButtonUpOnIsland(POINT cursor)
{
    // v5.4: recording activity circle. A short click toggles pause/resume; the
    // long-press-to-stop action was already dispatched by the pulse timer.
    if (s_pressedActivity)
    {
        const bool stopHandled = s_activityStopSent;
        s_pressedActivity = false;
        s_activityStopSent = false;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        if (!stopHandled)
            ToggleRecordingPause();
        RefreshRecordingVisual(true);
        return;
    }

    // v5.4: feature row slot click opens the recording dialogs.
    if (s_pressedFeatureSlot >= 0)
    {
        const int slot = s_pressedFeatureSlot;
        s_pressedFeatureSlot = -1;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        if (HitTestFeatureSlot(cursor) == slot)
            OnFeatureSlotClicked(slot);
        return;
    }

    if (s_pressedSub >= 0)
    {
        const int sub = s_pressedSub;
        s_pressedSub = -1;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        if (HitTestSubIsland(cursor) == sub)
        {
            ActivateSourceApp(sub == 1 ? NotificationSource::QQ
                                       : NotificationSource::WeChat);
            DismissNotificationSubIsland(sub == 1 ? NotificationSource::QQ
                                                  : NotificationSource::WeChat);
        }
        return;
    }

    const bool wasMoved = s_dragMoved;
    EndDrag(true);
    if (!wasMoved)
        ShowIslandSettings();
}

void UpdateDrag(POINT cursor)
{
    if (!s_dragging)
        return;

    const int deltaX = cursor.x - s_dragStartCursor.x;
    const int deltaY = cursor.y - s_dragStartCursor.y;
    if (!s_dragMoved && std::abs(deltaX) < 4 && std::abs(deltaY) < 4)
        return;
    s_dragMoved = true;

    const int width = s_dragStartCapsule.right - s_dragStartCapsule.left;
    const int height = s_dragStartCapsule.bottom - s_dragStartCapsule.top;

    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT workArea = GetIslandWorkArea();
    if (GetMonitorInfoW(monitor, &info))
        workArea = info.rcWork;

    const int minLeft = static_cast<int>(workArea.left);
    const int minTop = static_cast<int>(workArea.top);
    const int maxLeft = std::max(minLeft, static_cast<int>(workArea.right) - width);
    const int maxTop = std::max(minTop, static_cast<int>(workArea.bottom) - height);
    int left = s_dragStartCapsule.left + deltaX;
    int top = s_dragStartCapsule.top + deltaY;
    left = std::clamp(left, minLeft, maxLeft);
    top = std::clamp(top, minTop, maxTop);

    g_settings.position = ISLAND_POS_CUSTOM;
    g_settings.customX = left;
    g_settings.customY = top;

    RenderIslandFrame(true);
}

void EndDrag(bool commit)
{
    if (!s_dragging)
        return;

    s_dragging = false;
    if (GetCapture() == g_hwnd)
        ReleaseCapture();

    if (commit && s_dragMoved)
    {
        SaveSettingsQuietly();
        UpdateTrayIcon();
        RefreshSettingsDialog();
    }
    else if (!commit)
    {
        g_settings.customX = s_dragStartCapsule.left;
        g_settings.customY = s_dragStartCapsule.top;
        if (s_dragStartCapsule.left == -100000)
        {
            g_settings.customX = -100000;
            g_settings.customY = -100000;
        }
        RenderIslandFrame(true);
    }
}

void RaiseIslandAboveFullscreen()
{
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void UpdateHoverLeavePending()
{
    if (!s_hoverLeavePending)
        return;

    POINT cursor = {};
    GetCursorPos(&cursor);
    if (IsPointInHoverKeepZone(cursor))
        return; // the user is still moving between the capsule and the row

    s_hoverLeavePending = false;
    s_hovering = false;
    SetIslandHoverTarget(false);
    SetFeatureRowVisibleTarget(false);
    if (s_hoveredFeatureSlot >= 0)
    {
        s_hoveredFeatureSlot = -1;
        SetFeatureRowInfo(kEnabledFeatureSlots, -1);
    }
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void UpdateAutoHide()
{
    const ULONGLONG now = GetTickCount64();
    POINT cursor = {};
    GetCursorPos(&cursor);

    UpdateHoverLeavePending();

    const bool gameFullscreen = IsGameFullscreenActive();
    const bool otherFullscreen = !gameFullscreen && IsFullscreenAppActive();
    static bool s_lastOtherFullscreen = false;

    // Games in fullscreen (exclusive D3D or a known game engine window): hide
    // the island instead of drawing on top of the game.
    if (g_settings.hideOnFullscreen && gameFullscreen)
    {
        if (!IsIslandHiddenTarget())
        {
            s_hiddenByFullscreen = true;
            s_hovering = false;
            SetIslandHoverTarget(false);
            SetIslandHiddenTarget(true);
        }
    }
    else if (s_hiddenByFullscreen)
    {
        s_hiddenByFullscreen = false;
        SetIslandHiddenTarget(false);
    }

    // Any other fullscreen app (video players, browser fullscreen video,
    // presentations): keep the island visible and above it.
    if (otherFullscreen)
    {
        if (!s_lastOtherFullscreen)
            RevealIsland();
        RaiseIslandAboveFullscreen();
    }
    s_lastOtherFullscreen = otherFullscreen;

    // Keep the island on screen while message circles or the recording UI are
    // visible (a running recording must always be reachable).
    if (IsAnySubIslandVisible() || IsRecordingUiActive())
    {
        s_lastInsideTick = now;
        return;
    }

    if (!g_settings.autoHide)
        return;

    const RECT trigger = GetIslandClusterTriggerRect();
    const bool inside = PointInRect(trigger, cursor);
    if (inside)
        s_lastInsideTick = now;

    if (IsIslandHiddenTarget())
    {
        if (inside && !s_hiddenByFullscreen)
        {
            SetIslandHiddenTarget(false);
            s_hovering = true;
            SetIslandHoverTarget(true);
            s_lastInsideTick = now;
        }
        return;
    }

    if (s_dragging || IsSettingsDialogOpen())
    {
        s_lastInsideTick = now;
        return;
    }

    if (!inside)
    {
        if (s_hovering)
        {
            s_hovering = false;
            SetIslandHoverTarget(false);
        }

        const ULONGLONG delay =
            static_cast<ULONGLONG>(std::max(0, g_settings.hideDelaySeconds)) * 1000ULL;
        if (now - s_lastInsideTick >= delay)
            SetIslandHiddenTarget(true);
    }
}

// The sub-island is persistent by default (deadline 0): it stays on screen
// until the user clicks it or hides notifications. Only an explicit 1..10 s
// "停留时长" arms the auto-collapse timer.
void RefreshSubDismissTimer()
{
    bool anyTimed = false;
    for (int i = 0; i < kNotificationSourceCount; ++i)
    {
        if (IsSubIslandTarget(i) && s_subDeadline[i] > 0)
        {
            anyTimed = true;
            break;
        }
    }

    if (anyTimed && !s_subDismissTimerRunning)
    {
        SetTimer(g_hwnd, kTimerNotificationDismiss, 250, nullptr);
        s_subDismissTimerRunning = true;
    }
    else if (!anyTimed && s_subDismissTimerRunning)
    {
        KillTimer(g_hwnd, kTimerNotificationDismiss);
        s_subDismissTimerRunning = false;
    }
}

void CheckSubIslandDeadlines()
{
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kNotificationSourceCount; ++i)
    {
        if (!IsSubIslandTarget(i) || s_subDeadline[i] == 0)
            continue; // persistent badge
        if (s_hoveredSub == i)
        {
            // Keep the hovered circle (and its tooltip) alive a bit longer.
            s_subDeadline[i] = now + 2000;
            continue;
        }
        if (now >= s_subDeadline[i])
        {
            s_subDeadline[i] = 0;
            SetSubIslandTarget(i, false);
            if (s_hoveredSub == i)
                SetHoveredSub(-1);
            InvalidateIslandContent();
            RenderIslandFrame(true);
        }
    }
    RefreshSubDismissTimer();
}

void ShowIslandSettings()
{
    RevealIsland();
    CloseRecordingDialogs();
    ShowSettingsDialog(g_hwnd);
}

// ===========================================================================
// v5.4 meeting / video notes: hover feature row, recording dialogs, countdown,
// activity circle and result messages.
// ===========================================================================
std::wstring FormatElapsedClock(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int remaining = seconds % 60;
    wchar_t buffer[32] = {};
    if (hours > 0)
        swprintf_s(buffer, L"%d:%02d:%02d", hours, minutes, remaining);
    else
        swprintf_s(buffer, L"%02d:%02d", minutes, remaining);
    return buffer;
}

std::wstring FileNameFromPath(const std::wstring& path)
{
    const wchar_t* slash = wcsrchr(path.c_str(), L'\\');
    return slash != nullptr ? std::wstring(slash + 1) : path;
}

// --- recorder state (with the built-in visual test mode on top) ------------
void AccumulateDemoElapsed()
{
    if (!s_demoRecording || s_demoState != RecorderState::Recording)
        return;
    const ULONGLONG now = GetTickCount64();
    if (s_demoSegmentStartTick == 0)
    {
        s_demoSegmentStartTick = now;
        return;
    }
    s_demoElapsedMs += now - s_demoSegmentStartTick;
    s_demoSegmentStartTick = now;
}

void AdvanceDemoRecording()
{
    if (!s_demoRecording || !s_demoAutoFinish)
        return;
    const ULONGLONG now = GetTickCount64();
    if (s_demoState == RecorderState::Stopping)
    {
        if (now - s_demoStateTick >= 1800)
        {
            s_demoState = RecorderState::Summarizing;
            s_demoStateTick = now;
        }
    }
    else if (s_demoState == RecorderState::Summarizing)
    {
        if (now - s_demoStateTick >= 2000)
        {
            s_demoState = RecorderState::Idle;
            s_demoAutoFinish = false;
            s_demoMessage = L"会议纪要已生成（演示模式）";
        }
    }
}

RecorderState CurrentRecorderState()
{
    if (s_demoRecording)
        return s_demoState;
    return RecorderGetState();
}

int CurrentElapsedSeconds()
{
    if (s_demoRecording)
    {
        AccumulateDemoElapsed();
        return static_cast<int>(s_demoElapsedMs / 1000);
    }
    return RecorderGetElapsedSeconds();
}

int CurrentLimitSeconds()
{
    if (s_demoRecording)
        return kRecordingOneHourSeconds;
    return RecorderGetLimitSeconds();
}

bool IsRecordingUiActive()
{
    return s_countdownActive || s_activityVisible || s_toastVisible ||
           CurrentRecorderState() != RecorderState::Idle ||
           IsRecordingDialogOpen();
}

bool IsRecordingAiConfigured()
{
    // The feature only keeps the text result, so the AI endpoint and API key are
    // mandatory: without them the audio would be transcribed nowhere.
    return !g_settings.aiApiKey.empty() && !g_settings.aiApiBase.empty();
}

std::wstring BuildRecordingTooltip(RecorderState state, int elapsed)
{
    const std::wstring clock = FormatElapsedClock(elapsed);
    switch (state)
    {
    case RecorderState::Recording:
        return L"记录中 " + clock + L" · 点击暂停 / 长按结束";
    case RecorderState::Paused:
        return L"已暂停 " + clock + L" · 点击继续 / 长按结束";
    case RecorderState::Stopping:
        return L"正在转写…";
    case RecorderState::Summarizing:
        return L"正在生成纪要…";
    default:
        return std::wstring();
    }
}

float RecordingPressProgress()
{
    if (!s_pressedActivity)
        return 0.0f;
    const ULONGLONG held = GetTickCount64() - s_activityPressTick;
    return std::min(1.0f, static_cast<float>(held) /
                              static_cast<float>(kActivityLongPressMs));
}

void UpdateRecordingPulseTimer()
{
    const bool needed = s_activityVisible || s_pressedActivity ||
                        CurrentRecorderState() != RecorderState::Idle ||
                        IsActivityCircleVisibleTarget() ||
                        GetActivityCircleProgress() > 0.001f;
    if (needed && !s_recordingPulseTimerRunning)
    {
        SetTimer(g_hwnd, kTimerRecordingPulse, kRecordingPulseMs, nullptr);
        s_recordingPulseTimerRunning = true;
    }
    else if (!needed && s_recordingPulseTimerRunning)
    {
        KillTimer(g_hwnd, kTimerRecordingPulse);
        s_recordingPulseTimerRunning = false;
    }
}

// --- transient result / error message below the island --------------------
void ShowIslandToast(const std::wstring& text, bool error)
{
    if (g_hwnd == nullptr || text.empty())
        return;
    SetIslandToast(text, error);
    s_toastVisible = true;
    s_toastDeadlineTick = GetTickCount64() + kToastVisibleMs;
    if (!s_toastTimerRunning)
    {
        SetTimer(g_hwnd, kTimerIslandToast, 200, nullptr);
        s_toastTimerRunning = true;
    }
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void TickIslandToast()
{
    if (!s_toastVisible)
    {
        if (s_toastTimerRunning && g_hwnd != nullptr)
        {
            KillTimer(g_hwnd, kTimerIslandToast);
            s_toastTimerRunning = false;
        }
        return;
    }
    if (GetTickCount64() < s_toastDeadlineTick)
        return;

    s_toastVisible = false;
    SetIslandToast(L"", false);
    if (s_toastTimerRunning && g_hwnd != nullptr)
    {
        KillTimer(g_hwnd, kTimerIslandToast);
        s_toastTimerRunning = false;
    }
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void ShowRecordingResultToast()
{
    std::wstring message;
    bool isError = false;
    if (!s_demoMessage.empty() || s_demoRecording)
    {
        message = s_demoMessage.empty() ? L"转写已结束（演示模式）" : s_demoMessage;
        s_demoMessage.clear();
    }
    else
    {
        // Only the text result is kept (no audio file, no audio folder).
        // 纪要.md is the main result; when some speech chunks could not be
        // transcribed the recorder keeps their audio in the system temp folder
        // and reports the count through RecorderGetPendingChunkCount().
        const std::wstring folder = RecorderGetLastFolder();
        const std::wstring markdown = RecorderGetLastMarkdownPath();
        const std::wstring transcript = RecorderGetLastTranscriptPath();
        const std::wstring lastMessage = RecorderGetLastMessage();
        const int pending = RecorderGetPendingChunkCount();
        const std::wstring folderName =
            folder.empty() ? std::wstring() : FileNameFromPath(folder);

        if (!markdown.empty() || !transcript.empty())
        {
            message = markdown.empty()
                          ? L"转写已生成：" + folderName
                          : L"纪要已生成：" + folderName;
            if (pending > 0)
            {
                wchar_t suffix[96] = {};
                swprintf_s(suffix,
                           L"；另有 %d 个片段待重转（音频保留在系统临时目录）",
                           pending);
                message += suffix;
            }
        }
        else
        {
            message = lastMessage.empty() ? L"转写失败，未生成纪要" : lastMessage;
            isError = true;
        }
    }
    ShowIslandToast(message, isError);
}

// --- recording state machine ----------------------------------------------
void StopRecordingFromUi()
{
    if (s_demoRecording)
    {
        if (s_demoState == RecorderState::Recording)
            AccumulateDemoElapsed();
        else if (s_demoState != RecorderState::Paused)
            return;
        s_demoSegmentStartTick = 0;
        s_demoState = RecorderState::Stopping;
        s_demoAutoFinish = true;
        s_demoStateTick = GetTickCount64();
        RefreshRecordingVisual(true);
        return;
    }
    const RecorderState state = RecorderGetState();
    if (state != RecorderState::Recording && state != RecorderState::Paused)
        return;
    RecorderStop();
    RefreshRecordingVisual(true);
}

void ToggleRecordingPause()
{
    if (s_demoRecording)
    {
        if (s_demoState == RecorderState::Recording)
        {
            AccumulateDemoElapsed();
            s_demoState = RecorderState::Paused;
            s_demoSegmentStartTick = 0;
        }
        else if (s_demoState == RecorderState::Paused)
        {
            s_demoState = RecorderState::Recording;
            s_demoSegmentStartTick = GetTickCount64();
        }
        else
        {
            return;
        }
        RefreshRecordingVisual(true);
        return;
    }

    const RecorderState state = RecorderGetState();
    if (state == RecorderState::Recording)
        RecorderPause();
    else if (state == RecorderState::Paused)
        RecorderResume();
    else
        return;
    RefreshRecordingVisual(true);
}

void RefreshRecordingVisual(bool force)
{
    AdvanceDemoRecording();

    const RecorderState state = CurrentRecorderState();
    const bool active = state != RecorderState::Idle;
    const bool stateChanged = state != s_lastRecorderState;

    if (stateChanged)
    {
        if (!active && s_lastRecorderState != RecorderState::Idle)
        {
            // Session finished (stopped / auto-stopped / failed): hide the
            // circle and show the Markdown / result message for a moment.
            s_activityVisible = false;
            s_activityHovered = false;
            SetActivityCircleVisibleTarget(false);
            if (s_demoRecording)
                s_demoRecording = false;
            ShowRecordingResultToast();
        }
        else if (active)
        {
            s_activityVisible = true;
            SetActivityCircleVisibleTarget(true);
            RevealIsland();
            s_lastInsideTick = GetTickCount64();
        }
        s_lastRecorderState = state;
    }
    else if (active != s_activityVisible)
    {
        s_activityVisible = active;
        SetActivityCircleVisibleTarget(active);
    }

    const int elapsed = CurrentElapsedSeconds();
    if (elapsed != s_lastElapsedSeconds || force)
    {
        s_lastElapsedSeconds = elapsed;
        s_activityTooltipText = BuildRecordingTooltip(state, elapsed);
    }

    RecordingActivityVisual visual;
    visual.visible = s_activityVisible;
    visual.state = state;
    visual.elapsedSeconds = elapsed;
    visual.limitSeconds = CurrentLimitSeconds();
    visual.hovered = s_activityHovered;
    visual.pressProgress = RecordingPressProgress();
    visual.pulseTick = static_cast<int>(GetTickCount64() / 40);
    visual.tooltip = s_activityHovered ? s_activityTooltipText : std::wstring();
    SetRecordingActivityVisual(visual);

    UpdateRecordingPulseTimer();
    RenderIslandFrame(force);
}

void RefreshRecordingPulse()
{
    const ULONGLONG now = GetTickCount64();
    if (s_pressedActivity && !s_activityStopSent &&
        now - s_activityPressTick >= kActivityLongPressMs)
    {
        s_activityStopSent = true;
        StopRecordingFromUi();
    }
    RefreshRecordingVisual(false);
}

// --- 3 second countdown + starting the recorder ---------------------------
void OnRecordingDialogAction(int limitSeconds, void* context);

void UpdateCountdownText()
{
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%d 秒后开始记录", s_countdownRemaining);
    SetIslandCountdownText(buffer);
}

void StartRecordingCountdown(int limitSeconds)
{
    if (g_hwnd == nullptr)
        return;

    if (!s_testRecordStart && !IsRecordingAiConfigured())
    {
        ShowRecordingConfigNoticeDialog(g_hwnd, &OnRecordingDialogAction, nullptr);
        return;
    }

    s_pendingLimitSeconds = limitSeconds;
    s_countdownRemaining = std::max(1, kRecordingCountdownSeconds);
    s_countdownActive = true;
    UpdateCountdownText();
    SetTimer(g_hwnd, kTimerCountdown, 1000, nullptr);

    // The dialog just closed; make sure the island is shown unhovered.
    RevealIsland();
    s_hovering = false;
    s_hoverLeavePending = false;
    SetIslandHoverTarget(false);
    SetFeatureRowVisibleTarget(false);
    s_hoveredFeatureSlot = -1;
    SetFeatureRowInfo(kEnabledFeatureSlots, -1);
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void StartRecordingNow(int limitSeconds)
{
    std::wstring error;
    const std::wstring directory = ResolveRecordingSaveDirectory();
    if (!EnsureRecordingDirectory(directory, error))
    {
        ShowIslandToast(L"无法创建纪要目录：" + error, true);
        RefreshRecordingVisual(true);
        return;
    }

    if (!RecorderStart(limitSeconds, directory, RecorderMakeFolderName()))
    {
        std::wstring message = RecorderGetLastMessage();
        if (message.empty())
            message = L"无法打开音频设备";
        ShowIslandToast(L"记录失败：" + message, true);
        RefreshRecordingVisual(true);
        return;
    }

    RevealIsland();
    s_lastInsideTick = GetTickCount64();
    RefreshRecordingVisual(true);
}

void TickRecordingCountdown()
{
    if (!s_countdownActive)
        return;

    --s_countdownRemaining;
    if (s_countdownRemaining > 0)
    {
        UpdateCountdownText();
        InvalidateIslandContent();
        RenderIslandFrame(true);
        return;
    }

    if (g_hwnd != nullptr)
        KillTimer(g_hwnd, kTimerCountdown);
    s_countdownActive = false;
    SetIslandCountdownText(L"");
    InvalidateIslandContent();
    StartRecordingNow(s_pendingLimitSeconds);
}

// --- feature row click / dialog chain -------------------------------------
void OnRecordingDialogAction(int limitSeconds, void* context)
{
    (void)context;
    if (limitSeconds == -2)
    {
        // The "AI not configured" notice: open the settings window so the user
        // can fill in the AI endpoint / API key.
        ShowIslandSettings();
        return;
    }
    if (limitSeconds < 0)
    {
        // Confirmation step accepted: now let the user pick the duration.
        ShowRecordingDurationDialog(g_hwnd, &OnRecordingDialogAction, nullptr);
        return;
    }
    StartRecordingCountdown(limitSeconds);
}

void ShowRecordingFlow()
{
    s_hovering = false;
    s_hoverLeavePending = false;
    SetIslandHoverTarget(false);
    SetFeatureRowVisibleTarget(false);
    s_hoveredFeatureSlot = -1;
    SetFeatureRowInfo(kEnabledFeatureSlots, -1);
    InvalidateIslandContent();
    RenderIslandFrame(true);
    ShowRecordingConfirmDialog(g_hwnd, &OnRecordingDialogAction, nullptr);
}

void OnFeatureSlotClicked(int slot)
{
    if (slot != 0)
        return;
    if (s_countdownActive)
        return;
    if (CurrentRecorderState() != RecorderState::Idle)
    {
        ShowIslandToast(L"正在记录 · 点击圆环暂停，长按结束", false);
        return;
    }
    if (IsRecordingDialogOpen())
        return;
    if (!IsRecordingAiConfigured())
    {
        ShowRecordingConfigNoticeDialog(g_hwnd, &OnRecordingDialogAction, nullptr);
        return;
    }
    ShowRecordingFlow();
}

// --- internal visual test hooks -------------------------------------------
int TestLimitFromEnvironment()
{
    wchar_t buffer[32] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"DSH_RECORD_LIMIT_SECONDS", buffer, 32);
    if (length > 0 && length < 32)
    {
        wchar_t* end = nullptr;
        const long value = wcstol(buffer, &end, 10);
        if (end != buffer && value >= 0 && value <= 86400)
            return static_cast<int>(value);
    }
    return kRecordingOneHourSeconds;
}

void StartDemoRecording(RecorderState initialState)
{
    s_demoRecording = true;
    s_demoAutoFinish = false;
    s_demoState = initialState;
    s_demoElapsedMs = 0;
    s_demoSegmentStartTick =
        initialState == RecorderState::Recording ? GetTickCount64() : 0;
    s_demoStateTick = GetTickCount64();
    s_demoMessage.clear();
    s_lastRecorderState = RecorderState::Idle;
    s_activityVisible = true;
    SetActivityCircleVisibleTarget(true);
    RevealIsland();
    RefreshRecordingVisual(true);
}

void ForceHoverRowForTest()
{
    s_forceHoverRowForTest = true;
    s_hovering = true;
    SetIslandHoverTarget(true);
    SetFeatureRowVisibleTarget(true);
    s_hoveredFeatureSlot = 0;
    SetFeatureRowInfo(kEnabledFeatureSlots, 0);
    FinishIslandAnimation();
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void RunRecordingTestHooks(LPCWSTR commandLine)
{
    if (commandLine == nullptr)
        return;

    if (wcsstr(commandLine, L"--test-hover-row") != nullptr)
        ForceHoverRowForTest();

    if (wcsstr(commandLine, L"--test-recording-confirm") != nullptr)
        ShowRecordingConfirmDialog(g_hwnd, &OnRecordingDialogAction, nullptr);

    if (wcsstr(commandLine, L"--test-recording-wheel") != nullptr)
        ShowRecordingDurationDialog(g_hwnd, &OnRecordingDialogAction, nullptr);

    if (wcsstr(commandLine, L"--test-recording-notice") != nullptr)
        ShowRecordingConfigNoticeDialog(g_hwnd, &OnRecordingDialogAction, nullptr);

    if (wcsstr(commandLine, L"--test-recording-circle") != nullptr)
        StartDemoRecording(RecorderState::Recording);

    if (wcsstr(commandLine, L"--test-recording-paused") != nullptr)
        StartDemoRecording(RecorderState::Paused);

    if (wcsstr(commandLine, L"--test-recording-summarizing") != nullptr)
        StartDemoRecording(RecorderState::Summarizing);

    if (wcsstr(commandLine, L"--test-recording-tooltip") != nullptr)
    {
        StartDemoRecording(RecorderState::Recording);
        s_activityHovered = true;
        RefreshRecordingVisual(true);
    }

    if (wcsstr(commandLine, L"--test-toast") != nullptr)
        ShowIslandToast(L"会议纪要已生成：会议纪要_20261111_170001.md", false);

    if (wcsstr(commandLine, L"--test-toast-partial") != nullptr)
        ShowIslandToast(
            L"纪要已生成：会议纪要_20261111_180001；另有 2 个片段待重转"
            L"（音频保留在系统临时目录）",
            false);

    if (wcsstr(commandLine, L"--test-record-start") != nullptr)
    {
        s_testRecordStart = true;
        StartRecordingCountdown(TestLimitFromEnvironment());
    }
}


void HandleCommand(WORD command)
{
    switch (command)
    {
    case TRAY_CMD_SETTINGS:
        ShowIslandSettings();
        break;
    case TRAY_CMD_TIME_SETTINGS:
        OpenDateAndTimeSettings();
        break;
    case TRAY_CMD_AUTOSTART:
    {
        const bool enabled = !IsAutoStartEnabled();
        SetAutoStartEnabled(enabled);
        g_settings.autoStart = enabled;
        SaveSettings();
        UpdateTrayIcon();
        RefreshSettingsDialog();
        break;
    }
    case TRAY_CMD_RESET_POSITION:
        ResetIslandPosition();
        break;
    case TRAY_CMD_ABOUT:
        ShowAboutDialog(g_hwnd);
        break;
    case TRAY_CMD_EXIT:
        RequestApplicationExit();
        break;
    default:
        break;
    }
}

void RefreshFromSystemTheme()
{
    RefreshSystemThemeCache();
    InvalidateIslandContent();
    RenderIslandFrame(true);
    UpdateTrayIcon();
    RefreshSettingsDialog();
}

// ---------------------------------------------------------------------------
// Messaging app activation
// ---------------------------------------------------------------------------
bool ProcessExeMatches(DWORD processId, const wchar_t* const* names, int count)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 processId);
    if (process == nullptr)
        return false;

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok)
        return false;

    const wchar_t* base = wcsrchr(path, L'\\');
    base = (base != nullptr) ? base + 1 : path;
    for (int index = 0; index < count; ++index)
    {
        if (_wcsicmp(base, names[index]) == 0)
            return true;
    }
    return false;
}

struct WindowSearch
{
    NotificationSource source;
    HWND visibleResult;
    HWND hiddenResult;
};

BOOL CALLBACK FindMessagingWindowProc(HWND hwnd, LPARAM param)
{
    WindowSearch* search = reinterpret_cast<WindowSearch*>(param);
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;

    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
        return TRUE;
    // A minimised Qt/Chromium window reports a tiny taskbar-sized rectangle, so
    // only apply the size filter to windows that are not minimised.
    if (!IsIconic(hwnd) &&
        (rect.right - rect.left < 160 || rect.bottom - rect.top < 120))
        return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
        return TRUE;

    wchar_t className[256] = {};
    if (GetClassNameW(hwnd, className, 256) == 0)
        return TRUE;

    if (search->source == NotificationSource::WeChat)
    {
        const wchar_t* names[] = {L"Weixin.exe", L"WeChat.exe", L"WeixinApp.exe"};
        if (!ProcessExeMatches(processId, names, 3))
            return TRUE;
        // Only the real main window (Qt51514QWindowIcon), never the tray-icon
        // message helper window.
        if (wcsstr(className, L"QWindowIcon") == nullptr)
            return TRUE;
    }
    else if (search->source == NotificationSource::QQ)
    {
        const wchar_t* names[] = {L"QQ.exe"};
        if (!ProcessExeMatches(processId, names, 1))
            return TRUE;
        if (wcscmp(className, L"Chrome_WidgetWin_1") != 0)
            return TRUE;
    }
    else
    {
        return TRUE;
    }

    if (IsWindowVisible(hwnd))
    {
        search->visibleResult = hwnd;
        return FALSE;
    }

    if (search->hiddenResult == nullptr)
        search->hiddenResult = hwnd;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Notification handling
// ---------------------------------------------------------------------------
// QQ group / friend chats are wanted; official accounts, news feeds and
// "看点"-style pushes are not. The toast text is the only information available
// for QQ, so filter on well-known official-source keywords.
bool IsIgnoredQqNotification(const AppNotification& notification)
{
    std::wstring text = notification.title;
    text += L" ";
    text += notification.body;

    static const wchar_t* const ignoredKeywords[] = {
        L"订阅号", L"公众号", L"服务号", L"QQ看点", L"看点",
        L"QQ空间", L"空间动态", L"腾讯新闻", L"腾讯视频",
        L"QQ邮箱", L"邮箱提醒", L"游戏中心", L"直播提醒",
        L"会员活动", L"活动推送"
    };
    for (const wchar_t* keyword : ignoredKeywords)
    {
        if (text.find(keyword) != std::wstring::npos)
            return true;
    }
    return false;
}

void OnAppNotification(const AppNotification& notification)
{
    if (!g_settings.notifyEnable)
        return;
    if (notification.source == NotificationSource::WeChat && !g_settings.notifyWeChat)
        return;
    if (notification.source == NotificationSource::QQ && !g_settings.notifyQQ)
        return;
    if (notification.source == NotificationSource::Other)
        return;
    if (notification.source == NotificationSource::QQ &&
        IsIgnoredQqNotification(notification))
        return;

    ShowNotificationSubIsland(notification);
}

LRESULT CALLBACK IslandWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_TIMER:
        switch (wParam)
        {
        case kTimerContent:
            ++s_contentTickCount;
            if ((s_contentTickCount % 25) == 0)
                RefreshSystemThemeCache();
            RenderIslandFrame(false);
            return 0;
        case kTimerAutoHide:
            UpdateAutoHide();
            return 0;
        case kTimerAnimation:
            UpdateIslandAnimation();
            return 0;
        case kTimerTray:
            UpdateTrayIcon();
            return 0;
        case kTimerNotificationDismiss:
            CheckSubIslandDeadlines();
            return 0;
        case kTimerCountdown:
            TickRecordingCountdown();
            return 0;
        case kTimerRecordingPulse:
            RefreshRecordingPulse();
            return 0;
        case kTimerIslandToast:
            TickIslandToast();
            return 0;
        default:
            return 0;
        }

    case WM_MOUSEMOVE:
    {
        POINT cursor = {};
        GetCursorPos(&cursor);
        if (s_dragging)
        {
            UpdateDrag(cursor);
            return 0;
        }
        if (s_forceHoverRowForTest)
            return 0; // forced screenshot state: ignore the real mouse

        s_hoverLeavePending = false;

        const int sub = HitTestSubIsland(cursor);
        if (sub != s_hoveredSub)
            SetHoveredSub(sub);

        // v5.4 hover feature row: highlight the icon under the mouse.
        int featureSlot = -1;
        if (sub < 0)
            featureSlot = HitTestFeatureSlot(cursor);
        if (featureSlot != s_hoveredFeatureSlot)
        {
            s_hoveredFeatureSlot = featureSlot;
            SetFeatureRowInfo(kEnabledFeatureSlots, featureSlot);
            InvalidateIslandContent();
            RenderIslandFrame(true);
        }

        // v5.4 recording activity circle hover -> tooltip with elapsed time.
        const bool overActivity = HitTestActivityCircle(cursor);
        if (overActivity != s_activityHovered)
        {
            s_activityHovered = overActivity;
            RefreshRecordingVisual(true);
        }

        bool overMain = false;
        if (sub < 0 && !overActivity)
        {
            const IslandClusterGeometry geometry = CurrentClusterGeometry();
            const int padding = ScaleDip(3);
            overMain =
                PointInRect(InflateRectCopy(geometry.capsule, padding, padding),
                            cursor);
            if (!overMain && GetFeatureRowProgress() > 0.15f)
            {
                overMain = PointInRect(
                    InflateRectCopy(geometry.featureRow, padding, padding), cursor);
            }
        }

        if (overMain && !s_hovering)
        {
            s_hovering = true;
            SetIslandHoverTarget(true);
            SetFeatureRowVisibleTarget(true);
        }
        else if (!overMain && s_hovering)
        {
            s_hovering = false;
            SetIslandHoverTarget(false);
            SetFeatureRowVisibleTarget(false);
        }

        if (!s_hovering)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
    {
        if (s_forceHoverRowForTest)
            return 0;

        POINT cursor = {};
        GetCursorPos(&cursor);
        if (IsPointInHoverKeepZone(cursor))
        {
            // The fully transparent gap between the capsule and the feature row
            // reports a leave although the user is still interacting with the
            // cluster; keep the hover and re-check on the auto-hide tick.
            s_hoverLeavePending = true;
            return 0;
        }

        s_hoverLeavePending = false;
        s_hovering = false;
        SetIslandHoverTarget(false);
        SetFeatureRowVisibleTarget(false);
        SetHoveredSub(-1);
        if (s_hoveredFeatureSlot >= 0)
        {
            s_hoveredFeatureSlot = -1;
            SetFeatureRowInfo(kEnabledFeatureSlots, -1);
        }
        if (s_activityHovered)
        {
            s_activityHovered = false;
            RefreshRecordingVisual(true);
        }
        InvalidateIslandContent();
        RenderIslandFrame(true);
        s_lastInsideTick = GetTickCount64();
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        POINT cursor = {};
        GetCursorPos(&cursor);
        OnLButtonDownOnIsland(cursor);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT cursor = {};
        GetCursorPos(&cursor);
        OnLButtonUpOnIsland(cursor);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (s_dragging)
            EndDrag(false);
        s_pressedSub = -1;
        s_pressedFeatureSlot = -1;
        if (s_pressedActivity)
        {
            s_pressedActivity = false;
            s_activityStopSent = false;
            RefreshRecordingVisual(true);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, s_dragging ? IDC_SIZEALL : IDC_HAND));
            return TRUE;
        }
        break;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        InitIslandLayout(hwnd);
        FinishIslandAnimation();
        return 0;

    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA)
        {
            InitIslandLayout(hwnd);
            FinishIslandAnimation();
        }
        else if (lParam != 0 &&
                 wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0)
        {
            RefreshFromSystemTheme();
        }
        return 0;

    case WM_THEMECHANGED:
        RefreshFromSystemTheme();
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
            RefreshFromSystemTheme();
        return TRUE;

    case WM_ENDSESSION:
        if (wParam != FALSE)
            SaveSettings();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == 0)
            HandleCommand(LOWORD(wParam));
        return 0;

    case kMsgShowSettings:
        ShowIslandSettings();
        return 0;

    case kMsgApplySettings:
        ApplySettingsAndRefresh();
        return 0;

    case kMsgRefreshTheme:
        RefreshFromSystemTheme();
        return 0;

    case kMsgRequestExit:
        DestroyWindow(hwnd);
        return 0;

    case kMsgNotification:
        HandleNotificationMessage(wParam, lParam);
        return 0;

    case kTrayCallbackMessage:
        HandleTrayCallback(wParam, lParam);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerContent);
        KillTimer(hwnd, kTimerAutoHide);
        KillTimer(hwnd, kTimerTray);
        KillTimer(hwnd, kTimerNotificationDismiss);
        KillTimer(hwnd, kTimerCountdown);
        KillTimer(hwnd, kTimerRecordingPulse);
        KillTimer(hwnd, kTimerIslandToast);
        s_countdownActive = false;
        s_recordingPulseTimerRunning = false;
        s_toastTimerRunning = false;
        // Best effort: let the recorder finalise the audio file on exit.
        if (CurrentRecorderState() != RecorderState::Idle)
            RecorderStop();
        CloseRecordingDialogs();
        StopIslandAnimationTimer();
        ShutdownNotifications();
        ShutdownChatWatcher();
        CloseSettingsDialog(false);
        DestroyTrayIcon();
        ShutdownIslandRenderer();
        PostQuitMessage(0);
        return 0;

    default:
        if (s_taskbarCreatedMessage != 0 && message == s_taskbarCreatedMessage)
        {
            ReinstallTrayIcon();
            return 0;
        }
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

// ---------------------------------------------------------------------------
// Application level commands (declared in island.h)
// ---------------------------------------------------------------------------
void ApplySettingsAndRefresh()
{
    ValidateSettings(g_settings);

    if (!g_settings.hideOnFullscreen)
        s_hiddenByFullscreen = false;
    if (!g_settings.autoHide && !s_hiddenByFullscreen && IsIslandHiddenTarget())
    {
        SetIslandHiddenTarget(false);
        SetIslandHoverTarget(s_hovering);
    }

    if (!g_settings.notifyEnable)
    {
        for (int i = 0; i < kNotificationSourceCount; ++i)
        {
            s_subDeadline[i] = 0;
            SetSubIslandTarget(i, false);
        }
    }
    else
    {
        if (!g_settings.notifyWeChat)
        {
            s_subDeadline[0] = 0;
            SetSubIslandTarget(0, false);
        }
        if (!g_settings.notifyQQ)
        {
            s_subDeadline[1] = 0;
            SetSubIslandTarget(1, false);
        }
        if (!s_lastNotifyEnable)
            RequestNotificationAccessFromUi();

        const int configured = std::clamp(g_settings.notifyPeekSeconds,
                                          kNotifyPeekSecondsMin,
                                          kNotifyPeekSecondsMax);
        for (int i = 0; i < kNotificationSourceCount; ++i)
        {
            if (!IsSubIslandTarget(i))
                continue;
            s_subDeadline[i] =
                configured > 0
                    ? GetTickCount64() + static_cast<ULONGLONG>(configured) * 1000ULL
                    : 0ULL;
        }
    }
    s_lastNotifyEnable = g_settings.notifyEnable;
    RefreshSubDismissTimer();

    UpdateAutoHideTimerState();
    InitIslandLayout(g_hwnd);
    InvalidateIslandContent();
    FinishIslandAnimation();
    RefreshRecordingVisual(true);
    UpdateTrayIcon();
    RefreshSettingsDialog();
}

void ResetIslandPosition()
{
    ClearCustomPosition();
    SaveSettings();
    ApplySettingsAndRefresh();
}

void RequestApplicationExit()
{
    if (g_hwnd != nullptr)
        PostMessageW(g_hwnd, kMsgRequestExit, 0, 0);
}

void RefreshIslandFromSystemTheme()
{
    RefreshFromSystemTheme();
}

void ShowNotificationSubIsland(const AppNotification& notification)
{
    const int index = NotificationSourceIndex(notification.source);
    if (index < 0 || index >= kNotificationSourceCount)
        return;

    std::wstring title = notification.title;
    std::wstring body = notification.body;
    if (title.empty() && !body.empty())
    {
        title = body;
        body.clear();
    }
    SetSubIslandContent(index, title, body);
    SetSubIslandTarget(index, true);

    const int configured = std::clamp(g_settings.notifyPeekSeconds,
                                      kNotifyPeekSecondsMin,
                                      kNotifyPeekSecondsMax);
    s_subDeadline[index] =
        configured > 0 ? GetTickCount64() + static_cast<ULONGLONG>(configured) * 1000ULL
                       : 0ULL;
    RefreshSubDismissTimer();

    s_hiddenByFullscreen = false;
    SetIslandHiddenTarget(false);
    SetIslandHoverTarget(false);
    s_hovering = false;
    InvalidateIslandContent();
    RenderIslandFrame(true);
}

void DismissNotificationSubIsland(NotificationSource source)
{
    const int index = NotificationSourceIndex(source);
    if (index < 0 || index >= kNotificationSourceCount)
        return;
    s_subDeadline[index] = 0;
    SetSubIslandTarget(index, false);
    if (s_hoveredSub == index)
        SetHoveredSub(-1);
    InvalidateIslandContent();
    RenderIslandFrame(true);
    RefreshSubDismissTimer();
}

bool IsSubIslandActive()
{
    return IsAnySubIslandVisible();
}

void ActivateSourceApp(NotificationSource source)
{
    if (source == NotificationSource::Other)
        return;

    WindowSearch search = {};
    search.source = source;
    EnumWindows(FindMessagingWindowProc, reinterpret_cast<LPARAM>(&search));

    HWND target = search.visibleResult;
    if (target == nullptr)
        target = search.hiddenResult;
    if (target == nullptr)
        return;

    if (IsIconic(target))
        ShowWindow(target, SW_RESTORE);
    else if (!IsWindowVisible(target))
        ShowWindow(target, SW_SHOW);

    if (SetForegroundWindow(target) == FALSE)
    {
        typedef void(WINAPI * SwitchToThisWindowFn)(HWND, BOOL);
        auto switchTo = reinterpret_cast<SwitchToThisWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SwitchToThisWindow"));
        if (switchTo != nullptr)
            switchTo(target, TRUE);
    }
    BringWindowToTop(target);
}

void RequestNotificationAccessFromUi()
{
    RequestNotificationAccess();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    ApplyDpiAwareness();

    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kIslandMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kIslandWindowClass, nullptr);
        if (existing != nullptr)
            PostMessageW(existing, kMsgShowSettings, 0, 0);
        if (mutex != nullptr)
            CloseHandle(mutex);
        if (SUCCEEDED(comResult))
            CoUninitialize();
        return 0;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    const Gdiplus::Status gdiplusStatus =
        Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr);

    LoadSettings();

    if (g_settings.autoStart)
        SetAutoStartEnabled(true);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = IslandWndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kIslandWindowClass;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32,
        LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16,
        LR_DEFAULTCOLOR));

    if (!RegisterClassExW(&windowClass))
    {
        MessageBoxW(nullptr, L"Failed to register the island window class.",
                    APP_NAME, MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult))
            CoUninitialize();
        return 1;
    }

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kIslandWindowClass, APP_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        instance, nullptr);
    if (g_hwnd == nullptr)
    {
        MessageBoxW(nullptr, L"Failed to create the island window.", APP_NAME,
                    MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult))
            CoUninitialize();
        return 1;
    }

    InitIslandLayout(g_hwnd);
    InitIslandAnimation(g_hwnd);
    InitIslandRenderer(g_hwnd);
    SetFeatureRowInfo(kEnabledFeatureSlots, -1);

    s_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    s_lastInsideTick = GetTickCount64();
    s_lastNotifyEnable = g_settings.notifyEnable;

    SetTimer(g_hwnd, kTimerContent, kContentCheckIntervalMs, nullptr);
    UpdateAutoHideTimerState();
    SetTimer(g_hwnd, kTimerTray, kTooltipRefreshIntervalMs, nullptr);

    RenderIslandFrame(true);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    InitTrayIcon(g_hwnd);

    SetNotificationCallback(&OnAppNotification);
    InitNotifications(g_hwnd);
    InitChatWatcher(g_hwnd);

    if (wcsstr(GetCommandLineW(), L"--settings") != nullptr)
        PostMessageW(g_hwnd, kMsgShowSettings, 0, 0);

    // Internal visual-test hook: pop a sample sub-island without waiting for a
    // real WeChat/QQ message (used by the Lead's UI tests).
    if (wcsstr(GetCommandLineW(), L"--test-notify") != nullptr)
    {
        AppNotification sample;
        sample.id = 1;
        sample.source = NotificationSource::WeChat;
        sample.appId = L"Weixin.exe";
        sample.appName = L"微信";
        sample.title = L"张三";
        sample.body = L"晚上八点开会，记得带上电脑和资料";
        sample.arrivalTick = GetTickCount64();
        if (wcsstr(GetCommandLineW(), L"--test-notify-qq") != nullptr)
        {
            sample.source = NotificationSource::QQ;
            sample.appId = L"QQ";
            sample.appName = L"QQ";
            sample.title = L"产品群";
            sample.body = L"李四：新版设计稿已上传，麻烦看下";
        }
        ShowNotificationSubIsland(sample);
        if (wcsstr(GetCommandLineW(), L"--test-notify-both") != nullptr)
        {
            AppNotification second;
            second.id = 2;
            second.source = NotificationSource::QQ;
            second.appId = L"QQ";
            second.appName = L"QQ";
            second.title = L"产品群";
            second.body = L"李四：新版设计稿已上传，麻烦看下";
            second.arrivalTick = GetTickCount64();
            ShowNotificationSubIsland(second);
        }
    }

    // v5.4 internal visual-test hooks (see RunRecordingTestHooks):
    //   --test-hover-row              force the hover feature row visible
    //   --test-recording-confirm      show the confirm dialog
    //   --test-recording-wheel        show the duration wheel dialog
    //   --test-recording-circle       demo activity circle (recording)
    //   --test-recording-paused       demo activity circle (paused)
    //   --test-recording-summarizing  demo activity circle (summarizing)
    //   --test-recording-tooltip      demo circle + its hover tooltip
    //   --test-recording-notice       show the "AI not configured" notice
    //   --test-toast                  show a sample result message
    //   --test-record-start           skip dialogs: countdown + real start
    //                                 (limit from DSH_RECORD_LIMIT_SECONDS)
    RunRecordingTestHooks(GetCommandLineW());

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (gdiplusStatus == Gdiplus::Ok)
        Gdiplus::GdiplusShutdown(gdiplusToken);
    if (SUCCEEDED(comResult))
        CoUninitialize();
    if (mutex != nullptr)
        CloseHandle(mutex);

    return static_cast<int>(message.wParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    return wWinMain(instance, nullptr, GetCommandLineW(), SW_SHOWNORMAL);
}
