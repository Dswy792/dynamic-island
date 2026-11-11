#pragma once

#include <windows.h>

#include <string>

// OpenAI-compatible AI endpoint configuration (chat + audio transcription).
struct AiApiSettings
{
    std::wstring baseUrl;      // e.g. https://api.openai.com/v1
    std::wstring apiKey;
    std::wstring sttModel;     // e.g. whisper-1
    std::wstring summaryModel; // e.g. gpt-4o-mini
};

// Detailed outcome of a transcription / summary run. The transcript is written
// chunk by chunk (flushed to disk after every successful chunk), so a later
// failure never loses already transcribed speech.
struct SummaryResult
{
    bool transcriptWritten = false;  // 转写.md exists (full or partial)
    bool markdownWritten = false;    // 纪要.md exists
    int transcribedChunks = 0;       // chunks successfully transcribed
    int pendingChunks = 0;           // speech chunks kept for a later re-try
    int droppedSilentChunks = 0;     // failed chunks that were pure silence/noise
    std::wstring pendingDirectory;   // %TEMP%\DynamicIslandRec\pending\<session>
    std::wstring failureSummary;     // short human readable list of kept chunks
};

// Transcribe the given (temporary) audio file (m4a/wav) chunk by chunk and,
// when markdownPath is not empty, summarize the transcript into a Markdown
// meeting note. Large recordings are decoded and split into 16 kHz mono WAV
// chunks in memory so every request stays below the common 25 MB STT limit.
//
// transcriptPath (转写.md) is written incrementally and flushed after each
// chunk. failureNotePath (转写失败说明.txt) is written when a speech chunk
// could not be transcribed after retries; that chunk's WAV is kept under
// %TEMP%\DynamicIslandRec\pending\<session>\ for a later manual re-try.
// markdownPath may be empty for transcript-only runs. No input audio is
// modified or deleted here: the caller owns the temporary audio file.
bool SummaryGenerate(const std::wstring& audioPath, const std::wstring& markdownPath,
                     const AiApiSettings& ai, std::wstring* transcriptOut,
                     std::wstring* errorOut,
                     const std::wstring& transcriptPath = std::wstring(),
                     const std::wstring& failureNotePath = std::wstring(),
                     SummaryResult* resultOut = nullptr);
