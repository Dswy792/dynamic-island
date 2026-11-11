#pragma once

#include <windows.h>

#include <string>

// Meeting / video notes recorder (text only).
//
// Captures the current audio output (WASAPI loopback: what the video meeting or
// player is playing) on a worker thread and encodes it to a temporary AAC .m4a
// file under %TEMP%\DynamicIslandRec so the AI pipeline can transcribe it. The
// user-visible result is text only: the transcript (转写.md), the AI meeting
// summary (纪要.md) and 会话信息.txt in <parentDirectory>\<folderName>. The
// temporary audio is always deleted after transcription, on success and on
// failure; no audio is ever kept in the user's save directory.
//
// AIs are mandatory: without a configured AI endpoint/API key there is nothing
// to save, so RecorderStart() refuses to start.
//
// After a recording is stopped the worker finalises the temporary audio,
// transcribes it chunk by chunk (the transcript is flushed to disk after every
// chunk), asks the configured chat model for a Markdown meeting summary and
// then removes the audio. If a chunk with speech cannot be transcribed after
// retries, that chunk is kept under %TEMP%\DynamicIslandRec\pending for a later
// re-try and listed in 转写失败说明.txt (the save folder stays text only).
// All state below is thread-safe and may be polled by the UI.
enum class RecorderState
{
    Idle,        // nothing running
    Recording,   // capturing audio
    Paused,      // paused by the user
    Stopping,    // finalising and transcribing
    Summarizing  // transcription done, AI summary in progress
};

constexpr int kRecordingUnlimited = 0;

// Start recording into <parentDirectory>\<folderName> (text outputs only).
// limitSeconds: 3600 / 7200 / 0 (unlimited).
// Returns false when AI is not configured or the audio device could not be
// opened; check RecorderGetLastMessage() for the reason.
bool RecorderStart(int limitSeconds, const std::wstring& parentDirectory,
                   const std::wstring& folderName);
void RecorderPause();
void RecorderResume();

// Stop and finalise. The worker then transcribes the audio, optionally generates
// the AI summary (state becomes Summarizing -> Idle) and deletes the temporary
// audio. Text results are exposed through the getters below.
void RecorderStop();

RecorderState RecorderGetState();
int RecorderGetElapsedSeconds();   // recorded (active) seconds, paused time excluded
int RecorderGetLimitSeconds();     // 0 = unlimited

// Results of the last finished session.
std::wstring RecorderGetLastFolder();
// Deprecated compatibility getter: audio is never persisted any more, so this
// always returns an empty string. (The dev/test hook DSH_RECORD_KEEP_TEMP_AUDIO=1
// keeps the temporary m4a under %TEMP%\DynamicIslandRec for inspection, but the
// path is not exposed through the public API.)
std::wstring RecorderGetLastAudioPath();
std::wstring RecorderGetLastMarkdownPath();   // 纪要.md, empty when not requested
std::wstring RecorderGetLastTranscriptPath(); // 转写.md (full or partial transcript)
std::wstring RecorderGetLastMessage();        // short human readable status / error

// Chunks whose speech could not be transcribed and were therefore kept for a
// possible later re-try under %TEMP%\DynamicIslandRec\pending. 0 after a fully
// successful run (the audio is then deleted).
int RecorderGetPendingChunkCount();
std::wstring RecorderGetPendingChunkDirectory(); // empty when nothing is pending

// "会议纪要_20261111_165012" based on the current local time.
std::wstring RecorderMakeFolderName();
