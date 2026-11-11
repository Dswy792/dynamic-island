// ===========================================================================
//  src/recorder.cpp  --  v5.4 meeting notes recorder engine
//
//  Pipeline:
//    WASAPI loopback capture of the default render device (system audio)
//      -> PCM16 48 kHz stereo (downmix + linear resample)
//      -> Media Foundation SinkWriter (AAC-LC, 128 kbps, .m4a container)
//      -> %TEMP%\DynamicIslandRec\<folder>\会议纪要.m4a  (TEMPORARY)
//    Afterwards: transcribe chunk by chunk + AI summary from src/summary.cpp.
//    The transcript is flushed after every chunk. The temporary audio is then
//    deleted again; only speech chunks that failed to transcribe are kept under
//    %TEMP%\DynamicIslandRec\pending\<session> for a later manual re-try. The
//    user save folder only receives text: 转写.md, 纪要.md, 会话信息.txt,
//    转写失败说明.txt (retained chunks) and 纪要说明.txt (summary failure).
//    State: Recording -> Stopping -> Summarizing -> Idle.
//
//  No third-party libraries. Required static libs are attached with
//  #pragma comment(lib, ...) so build.ps1 does not need to change.
//
//  Dev/test hook: the environment variable DSH_RECORD_LIMIT_SECONDS overrides
//  the requested recording limit (e.g. 12). 0 = unlimited. It is intended for
//  the automated end-to-end test and is otherwise harmless.
// ===========================================================================

#include "recorder.h"

#include "app_state.h"
#include "summary.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr wchar_t kTempAudioFileName[] = L"会议纪要.m4a"; // temporary, always deleted
constexpr wchar_t kSummaryFileName[] = L"纪要.md";
constexpr wchar_t kTranscriptFileName[] = L"转写.md";
constexpr wchar_t kSessionInfoFileName[] = L"会话信息.txt";
constexpr wchar_t kSummaryNoteFileName[] = L"纪要说明.txt";
constexpr wchar_t kTempRootName[] = L"DynamicIslandRec";
constexpr wchar_t kFailureNoteFileName[] = L"转写失败说明.txt";
constexpr wchar_t kKeepTempAudioEnv[] = L"DSH_RECORD_KEEP_TEMP_AUDIO";

constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kBytesPerFrame = kOutputChannels * 2; // PCM16
constexpr int kWriteFrames = 480;                   // 10 ms per MF sample

constexpr int kMaxLimitSeconds = 24 * 3600;
constexpr DWORD kCaptureBufferHns = 10000000; // 1 s WASAPI buffer

// The two uncompressed subtypes produced by GetMixFormat().
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010,
                          {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// ---------------------------------------------------------------------------
// Shared state (thread-safe; polled by the UI thread)
// ---------------------------------------------------------------------------
std::mutex g_mutex;
RecorderState g_state = RecorderState::Idle;
int g_elapsed = 0;
int g_limit = 0;
std::wstring g_folder;
std::wstring g_audio;
std::wstring g_markdown;
std::wstring g_transcript;
int g_pendingChunks = 0;
std::wstring g_pendingDirectory;
std::wstring g_message;
bool g_pauseRequested = false;
bool g_stopRequested = false;

struct StartResult
{
    bool ok = false;
    std::wstring error;
};

struct SessionConfig
{
    int requestedLimit = 0;
    int effectiveLimit = 0;
    std::wstring parentDirectory;   // user save directory
    std::wstring folderName;
    std::wstring folderPath;        // <parentDirectory>\<folderName>: text only
    std::wstring tempDirectory;     // %TEMP%\DynamicIslandRec\<folderName>
    std::wstring audioPath;         // temporary .m4a (deleted after transcription)
    std::wstring transcriptPath;    // <folderPath>\转写.md
    std::wstring markdownPath;      // <folderPath>\纪要.md (empty unless auto summary)
    std::wstring infoPath;          // <folderPath>\会话信息.txt
    std::wstring notePath;          // <folderPath>\纪要说明.txt (failure only)
    std::wstring failureNotePath;   // <folderPath>\转写失败说明.txt (retained chunks)
    std::wstring pendingDirectory;  // %TEMP%\DynamicIslandRec\pending\<folderName>
    bool autoSummary = false;
    bool keepTempAudio = false;     // dev/test hook DSH_RECORD_KEEP_TEMP_AUDIO
    AiApiSettings ai;
    std::wstring startTimeText;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0)
        return std::string();
    std::string bytes(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        bytes.data(), length, nullptr, nullptr);
    return bytes;
}

void SetStateAndMessage(RecorderState state, const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = state;
    g_message = message;
}

bool IsStopRequested()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stopRequested;
}

bool IsPauseRequested()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pauseRequested;
}

std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    if (length == 0 || length >= MAX_PATH * 2)
        return std::wstring();
    std::wstring full(path, length);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return std::wstring();
    return full.substr(0, slash);
}

bool EnsureDirectory(const std::wstring& directory, std::wstring* error)
{
    if (directory.empty())
    {
        if (error != nullptr)
            *error = L"录音保存目录为空";
        return false;
    }
    const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS ||
        result == ERROR_ALREADY_EXISTS)
        return true;
    if (error != nullptr)
        *error = L"无法创建录音文件夹（错误码 " + std::to_wstring(result) +
                 L"）：" + directory;
    return false;
}

bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text,
                       std::wstring* error)
{
    const std::string bytes = WideToUtf8(text);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (error != nullptr)
            *error = L"无法写入文件（错误码 " +
                     std::to_wstring(GetLastError()) + L"）：" + path;
        return false;
    }
    bool ok = true;
    size_t written = 0;
    while (written < bytes.size())
    {
        DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - written, 1u << 20));
        DWORD actual = 0;
        if (!WriteFile(file, bytes.data() + written, chunk, &actual, nullptr) ||
            actual == 0)
        {
            ok = false;
            if (error != nullptr)
                *error = L"写入文件失败（错误码 " +
                         std::to_wstring(GetLastError()) + L"）：" + path;
            break;
        }
        written += actual;
    }
    CloseHandle(file);
    return ok;
}

std::wstring NowText()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring FormatDuration(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02d:%02d:%02d", hours, minutes, secs);
    return buffer;
}

std::wstring FormatFileSize(unsigned long long bytes)
{
    wchar_t buffer[64] = {};
    if (bytes >= 1024ull * 1024ull)
        swprintf_s(buffer, L"%.1f MB",
                   static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024ull)
        swprintf_s(buffer, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        swprintf_s(buffer, L"%llu B", bytes);
    return buffer;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// SHCreateDirectoryExW requires a fully qualified path, and the UI may pass a
// relative one (e.g. in tests); make it absolute before use.
std::wstring MakeAbsolutePath(const std::wstring& path)
{
    if (path.empty())
        return path;
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0)
        return path;
    std::wstring absolute(static_cast<size_t>(required), L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, absolute.data(), nullptr);
    if (written == 0 || written >= required)
        return path;
    absolute.resize(written);
    while (absolute.size() > 3 &&
           (absolute.back() == L'\\' || absolute.back() == L'/'))
        absolute.pop_back();
    return absolute;
}

bool FileExistsW(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void DeleteFileIfExists(const std::wstring& path)
{
    if (path.empty())
        return;
    DeleteFileW(path.c_str());
}

std::wstring GetTempDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH)
        return std::wstring();
    std::wstring path(buffer, length);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

// Deletes one of our own %TEMP% session folders (audio + leftovers).
bool RemoveDirectoryTree(const std::wstring& path)
{
    if (path.empty())
        return true;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return true;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return DeleteFileW(path.c_str()) != FALSE;
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW operation = {};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = static_cast<FILEOP_FLAGS>(FOF_NO_UI);
    return SHFileOperationW(&operation) == 0;
}

// Dev/test hook: keep the temporary audio for inspection instead of deleting it.
bool KeepTempAudioFromEnv()
{
    wchar_t buffer[32] = {};
    const DWORD length = GetEnvironmentVariableW(kKeepTempAudioEnv, buffer,
                                                 static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return false;
    const wchar_t first = buffer[0];
    return first == L'1' || first == L't' || first == L'T' || first == L'y' ||
           first == L'Y';
}

// DSH_RECORD_LIMIT_SECONDS is a dev/test override (see file header).
int ApplyLimitOverride(int configured)
{
    wchar_t buffer[64] = {};
    const DWORD length = GetEnvironmentVariableW(L"DSH_RECORD_LIMIT_SECONDS", buffer,
                                                 static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return configured;
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    if (end == buffer)
        return configured;
    while (end != nullptr && (*end == L' ' || *end == L'\t'))
        ++end;
    if (end != nullptr && *end != L'\0')
        return configured;
    if (value < 0)
        return configured;
    const long clamped = value > kMaxLimitSeconds ? kMaxLimitSeconds : value;
    return static_cast<int>(clamped);
}

// ---------------------------------------------------------------------------
// Audio format handling (WASAPI mix format -> float stereo @ source rate)
// ---------------------------------------------------------------------------
struct AudioFormat
{
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    int blockAlign = 0;
    bool isFloat = false;
    DWORD channelMask = 0;
};

struct SpeakerMix
{
    DWORD mask;
    float left;
    float right;
};

const SpeakerMix kSpeakerMixTable[] = {
    {0x0001, 1.0f, 0.0f},    // front left
    {0x0002, 0.0f, 1.0f},    // front right
    {0x0004, 0.707f, 0.707f},// front center
    {0x0008, 0.35f, 0.35f},  // low frequency
    {0x0010, 0.707f, 0.0f},  // back left
    {0x0020, 0.0f, 0.707f},  // back right
    {0x0040, 0.707f, 0.0f},  // front left of center
    {0x0080, 0.0f, 0.707f},  // front right of center
    {0x0100, 0.5f, 0.5f},    // back center
    {0x0200, 0.707f, 0.0f},  // side left
    {0x0400, 0.0f, 0.707f},  // side right
    {0x0800, 0.5f, 0.5f},    // top center
    {0x1000, 0.5f, 0.0f},    // top front left
    {0x2000, 0.5f, 0.5f},    // top front center
    {0x4000, 0.0f, 0.5f},    // top front right
    {0x8000, 0.5f, 0.0f},    // top back left
    {0x10000, 0.5f, 0.5f},   // top back center
    {0x20000, 0.0f, 0.5f},   // top back right
};

float ReadOneSample(const BYTE* data, int bits, bool isFloat)
{
    if (isFloat)
    {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    switch (bits)
    {
    case 8:
        return (static_cast<int>(data[0]) - 128) / 128.0f;
    case 16:
    {
        int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }
    case 24:
    {
        int32_t value = static_cast<int32_t>(
            static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16));
        if ((value & 0x00800000) != 0)
            value |= static_cast<int32_t>(0xFF000000u);
        return static_cast<float>(value) / 8388608.0f;
    }
    case 32:
    {
        int32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(value / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

void DownmixToStereo(const float* input, int channels, DWORD channelMask, float* left,
                     float* right)
{
    if (channels <= 0)
    {
        *left = 0.0f;
        *right = 0.0f;
        return;
    }
    if (channels == 1)
    {
        *left = input[0];
        *right = input[0];
        return;
    }
    if (channels == 2)
    {
        *left = input[0];
        *right = input[1];
        return;
    }
    if (channelMask != 0)
    {
        float mixLeft = 0.0f;
        float mixRight = 0.0f;
        int mapped = 0;
        const int count = channels < 32 ? channels : 32;
        for (int channel = 0; channel < count; ++channel)
        {
            const DWORD bit = 1u << channel;
            if ((channelMask & bit) == 0)
                continue;
            for (const SpeakerMix& mix : kSpeakerMixTable)
            {
                if (mix.mask == bit)
                {
                    mixLeft += input[channel] * mix.left;
                    mixRight += input[channel] * mix.right;
                    ++mapped;
                    break;
                }
            }
        }
        if (mapped > 0)
        {
            *left = mixLeft;
            *right = mixRight;
            return;
        }
    }
    // Unknown layout: keep the first two channels (usually the stereo pair).
    *left = input[0];
    *right = input[1];
}

void ConvertPacketToStereoFloat(const BYTE* data, UINT32 frames, const AudioFormat& format,
                                std::vector<float>* out)
{
    out->clear();
    if (frames == 0 || data == nullptr)
        return;
    out->resize(static_cast<size_t>(frames) * 2);
    const int bytesPerSample = format.bits / 8;
    const int channelCount = format.channels < 8 ? format.channels : 8;
    if (bytesPerSample <= 0 || format.blockAlign <= 0)
        return;
    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        const BYTE* start = data + static_cast<size_t>(frame) *
                                       static_cast<size_t>(format.blockAlign);
        float values[8] = {};
        for (int channel = 0; channel < channelCount; ++channel)
        {
            values[channel] = ReadOneSample(
                start + static_cast<size_t>(channel) * static_cast<size_t>(bytesPerSample),
                format.bits, format.isFloat);
        }
        float left = 0.0f;
        float right = 0.0f;
        DownmixToStereo(values, channelCount, format.channelMask, &left, &right);
        (*out)[static_cast<size_t>(frame) * 2] = left;
        (*out)[static_cast<size_t>(frame) * 2 + 1] = right;
    }
}

// Linear stereo resampler from the device rate to 48 kHz.
class StereoResampler
{
public:
    StereoResampler(int sourceRate, int targetRate)
        : sourceRate_(sourceRate > 0 ? sourceRate : targetRate),
          targetRate_(targetRate > 0 ? targetRate : 48000),
          ratio_(static_cast<double>(sourceRate_) / static_cast<double>(targetRate_))
    {
    }

    void Push(const float* interleavedStereo, size_t frames)
    {
        if (interleavedStereo == nullptr || frames == 0)
            return;
        buffer_.insert(buffer_.end(), interleavedStereo, interleavedStereo + frames * 2);
    }

    void Drain(std::vector<float>* out)
    {
        out->clear();
        const size_t frames = buffer_.size() / 2;
        while (true)
        {
            const size_t index = static_cast<size_t>(position_);
            if (index + 1 >= frames)
                break;
            const double fraction = position_ - static_cast<double>(index);
            const float left0 = buffer_[index * 2];
            const float left1 = buffer_[(index + 1) * 2];
            const float right0 = buffer_[index * 2 + 1];
            const float right1 = buffer_[(index + 1) * 2 + 1];
            out->push_back(static_cast<float>(left0 + (left1 - left0) * fraction));
            out->push_back(static_cast<float>(right0 + (right1 - right0) * fraction));
            position_ += ratio_;
        }
        const size_t consumed = static_cast<size_t>(position_);
        if (consumed > 0 && consumed <= frames)
        {
            buffer_.erase(buffer_.begin(), buffer_.begin() + consumed * 2);
            position_ -= static_cast<double>(consumed);
        }
    }

private:
    int sourceRate_ = 48000;
    int targetRate_ = 48000;
    double ratio_ = 1.0;
    double position_ = 0.0;
    std::vector<float> buffer_;
};

void AppendFloatStereoAsPcm16(const std::vector<float>& input, std::vector<BYTE>* pending)
{
    if (input.empty())
        return;
    const size_t base = pending->size();
    pending->resize(base + input.size() * 2);
    BYTE* destination = pending->data() + base;
    for (size_t i = 0; i < input.size(); ++i)
    {
        float value = input[i];
        if (value > 1.0f)
            value = 1.0f;
        else if (value < -1.0f)
            value = -1.0f;
        const int scaled = static_cast<int>(std::lrintf(value * 32767.0f));
        const int16_t sample = static_cast<int16_t>(scaled);
        std::memcpy(destination + i * 2, &sample, sizeof(sample));
    }
}

// ---------------------------------------------------------------------------
// WASAPI loopback capture
// ---------------------------------------------------------------------------
class LoopbackCapture
{
public:
    ~LoopbackCapture() { Close(); }

    bool Open(std::wstring* error)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr))
        {
            if (error != nullptr)
                *error = L"无法创建音频设备枚举器（0x" + Hex(hr) + L"）";
            return false;
        }
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr))
        {
            if (error != nullptr)
                *error = L"没有可用的默认播放设备（0x" + Hex(hr) + L"）";
            return false;
        }
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client_));
        if (FAILED(hr))
        {
            if (error != nullptr)
                *error = L"无法打开系统声音客户端（0x" + Hex(hr) + L"）";
            return false;
        }
        hr = client_->GetMixFormat(&mixFormat_);
        if (FAILED(hr) || mixFormat_ == nullptr)
        {
            if (error != nullptr)
                *error = L"无法获取系统声音格式（0x" + Hex(hr) + L"）";
            return false;
        }
        if (!ParseFormat(mixFormat_, &format_))
        {
            if (error != nullptr)
                *error = L"不支持的音频设备格式（仅支持 PCM / 浮点）";
            return false;
        }
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 kCaptureBufferHns, 0, mixFormat_, nullptr);
        if (FAILED(hr))
        {
            if (error != nullptr)
                *error = L"无法初始化系统声音捕获（0x" + Hex(hr) + L"）";
            return false;
        }
        hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(&capture_));
        if (FAILED(hr))
        {
            if (error != nullptr)
                *error = L"无法获取系统声音捕获接口（0x" + Hex(hr) + L"）";
            return false;
        }
        return true;
    }

    bool Start(std::wstring* error)
    {
        if (client_ == nullptr)
            return false;
        const HRESULT hr = client_->Start();
        if (FAILED(hr))
        {
            if (error != nullptr)
                *error = L"无法开始系统声音捕获（0x" + Hex(hr) + L"）";
            return false;
        }
        return true;
    }

    void Close()
    {
        if (client_ != nullptr)
            client_->Stop();
        if (capture_ != nullptr)
        {
            capture_->Release();
            capture_ = nullptr;
        }
        if (client_ != nullptr)
        {
            client_->Release();
            client_ = nullptr;
        }
        if (mixFormat_ != nullptr)
        {
            CoTaskMemFree(mixFormat_);
            mixFormat_ = nullptr;
        }
        if (device_ != nullptr)
        {
            device_->Release();
            device_ = nullptr;
        }
        if (enumerator_ != nullptr)
        {
            enumerator_->Release();
            enumerator_ = nullptr;
        }
    }

    const AudioFormat& Format() const { return format_; }

    // Drains every queued loopback packet into stereo float frames at the
    // device sample rate. Returns false on a fatal capture error.
    bool Pull(std::vector<float>* interleavedStereo, std::wstring* error)
    {
        interleavedStereo->clear();
        if (capture_ == nullptr)
            return false;
        while (true)
        {
            UINT32 packetFrames = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packetFrames);
            if (FAILED(hr))
            {
                if (error != nullptr)
                    *error = L"读取系统声音失败（0x" + Hex(hr) + L"）";
                return false;
            }
            if (packetFrames == 0)
                break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, &devicePosition,
                                     &qpcPosition);
            if (FAILED(hr))
            {
                if (error != nullptr)
                    *error = L"读取系统声音数据失败（0x" + Hex(hr) + L"）";
                return false;
            }
            if (frames > 0)
            {
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    const size_t base = interleavedStereo->size();
                    interleavedStereo->resize(base + static_cast<size_t>(frames) * 2,
                                              0.0f);
                }
                else
                {
                    std::vector<float> converted;
                    ConvertPacketToStereoFloat(data, frames, format_, &converted);
                    interleavedStereo->insert(interleavedStereo->end(), converted.begin(),
                                              converted.end());
                }
            }
            capture_->ReleaseBuffer(frames);
        }
        return true;
    }

private:
    static std::wstring Hex(HRESULT hr)
    {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"%08X", static_cast<unsigned int>(hr));
        return buffer;
    }

    static bool ParseFormat(const WAVEFORMATEX* format, AudioFormat* out)
    {
        if (format == nullptr || out == nullptr)
            return false;
        out->sampleRate = static_cast<int>(format->nSamplesPerSec);
        out->channels = static_cast<int>(format->nChannels);
        out->bits = static_cast<int>(format->wBitsPerSample);
        out->blockAlign = static_cast<int>(format->nBlockAlign);
        out->channelMask = 0;
        out->isFloat = false;
        if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            out->isFloat = true;
        }
        else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            out->channelMask = extended->dwChannelMask;
            if (IsEqualGUID(extended->SubFormat, kSubtypeFloat))
                out->isFloat = true;
            else if (!IsEqualGUID(extended->SubFormat, kSubtypePcm))
                return false;
        }
        else if (format->wFormatTag != WAVE_FORMAT_PCM)
        {
            return false;
        }
        if (out->sampleRate <= 0 || out->channels <= 0 || out->blockAlign <= 0)
            return false;
        if (!out->isFloat && out->bits != 8 && out->bits != 16 && out->bits != 24 &&
            out->bits != 32)
            return false;
        if (out->isFloat && out->bits != 32)
            return false;
        return true;
    }

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    AudioFormat format_;
};

// ---------------------------------------------------------------------------
// AAC SinkWriter (PCM16 48 kHz stereo -> .m4a / AAC-LC 128 kbps)
// ---------------------------------------------------------------------------
class AacSinkWriter
{
public:
    ~AacSinkWriter() { Abort(); }

    bool Open(const std::wstring& path, std::wstring* error)
    {
        IMFAttributes* attributes = nullptr;
        HRESULT hr = MFCreateAttributes(&attributes, 3);
        if (FAILED(hr))
        {
            SetError(error, L"无法创建编码器属性", hr);
            return false;
        }
        attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes, &writer_);
        attributes->Release();
        if (FAILED(hr) || writer_ == nullptr)
        {
            SetError(error, L"无法创建 M4A 编码器（Media Foundation）", hr);
            return false;
        }

        IMFMediaType* outputType = nullptr;
        hr = MFCreateMediaType(&outputType);
        if (SUCCEEDED(hr))
        {
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutputSampleRate);
            outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kOutputChannels);
            outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbps
            outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            hr = writer_->AddStream(outputType, &streamIndex_);
        }
        if (outputType != nullptr)
            outputType->Release();
        if (FAILED(hr))
        {
            SetError(error, L"无法配置 AAC 输出流", hr);
            Abort();
            return false;
        }

        IMFMediaType* inputType = nullptr;
        hr = MFCreateMediaType(&inputType);
        if (SUCCEEDED(hr))
        {
            inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutputSampleRate);
            inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kOutputChannels);
            inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kBytesPerFrame);
            inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                 kOutputSampleRate * kBytesPerFrame);
            hr = writer_->SetInputMediaType(streamIndex_, inputType, nullptr);
        }
        if (inputType != nullptr)
            inputType->Release();
        if (FAILED(hr))
        {
            SetError(error, L"无法配置 PCM 输入格式", hr);
            Abort();
            return false;
        }

        hr = writer_->BeginWriting();
        if (FAILED(hr))
        {
            SetError(error, L"无法开始写入 M4A 文件", hr);
            Abort();
            return false;
        }
        framesWritten_ = 0;
        return true;
    }

    // Writes PCM16 48 kHz stereo data. Partial frames are allowed.
    bool WritePcm(const BYTE* data, size_t bytes, std::wstring* error)
    {
        if (writer_ == nullptr)
            return false;
        const size_t totalFrames = bytes / kBytesPerFrame;
        size_t frameIndex = 0;
        while (frameIndex < totalFrames)
        {
            const size_t count = std::min<size_t>(kWriteFrames, totalFrames - frameIndex);
            if (!WriteSample(data + frameIndex * kBytesPerFrame, count, error))
                return false;
            frameIndex += count;
        }
        return true;
    }

    bool Finish(std::wstring* error, unsigned long long* fileBytes)
    {
        if (writer_ == nullptr)
            return false;
        const HRESULT hr = writer_->Finalize();
        if (FAILED(hr))
        {
            SetError(error, L"保存 M4A 文件失败", hr);
            Abort();
            return false;
        }
        writer_->Release();
        writer_ = nullptr;
        if (fileBytes != nullptr)
            *fileBytes = QueryFileSize();
        return true;
    }

    void Abort()
    {
        if (writer_ != nullptr)
        {
            writer_->Release();
            writer_ = nullptr;
        }
    }

    long long FramesWritten() const { return framesWritten_; }

private:
    bool WriteSample(const BYTE* data, size_t frames, std::wstring* error)
    {
        const DWORD byteCount = static_cast<DWORD>(frames * kBytesPerFrame);
        IMFMediaBuffer* buffer = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(byteCount, &buffer);
        if (FAILED(hr))
        {
            SetError(error, L"无法分配音频缓冲区", hr);
            return false;
        }
        BYTE* destination = nullptr;
        hr = buffer->Lock(&destination, nullptr, nullptr);
        if (FAILED(hr) || destination == nullptr)
        {
            buffer->Release();
            SetError(error, L"无法锁定音频缓冲区", hr);
            return false;
        }
        std::memcpy(destination, data, byteCount);
        buffer->Unlock();
        buffer->SetCurrentLength(byteCount);

        IMFSample* sample = nullptr;
        hr = MFCreateSample(&sample);
        if (FAILED(hr))
        {
            buffer->Release();
            SetError(error, L"无法创建音频样本", hr);
            return false;
        }
        sample->AddBuffer(buffer);
        buffer->Release();

        const LONGLONG startTime = framesWritten_ * 10000000LL / kOutputSampleRate;
        framesWritten_ += static_cast<LONGLONG>(frames);
        LONGLONG endTime = framesWritten_ * 10000000LL / kOutputSampleRate;
        if (endTime <= startTime)
            endTime = startTime + 1;
        sample->SetSampleTime(startTime);
        sample->SetSampleDuration(endTime - startTime);

        hr = writer_->WriteSample(streamIndex_, sample);
        sample->Release();
        if (FAILED(hr))
        {
            SetError(error, L"写入音频数据失败", hr);
            return false;
        }
        return true;
    }

    static void SetError(std::wstring* error, const wchar_t* prefix, HRESULT hr)
    {
        if (error == nullptr)
            return;
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"%08X", static_cast<unsigned int>(hr));
        *error = std::wstring(prefix) + L"（0x" + buffer + L"）";
    }

    unsigned long long QueryFileSize() const
    {
        unsigned long long size = 0;
        HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER value = {};
            if (GetFileSizeEx(file, &value))
                size = static_cast<unsigned long long>(value.QuadPart);
            CloseHandle(file);
        }
        return size;
    }

public:
    void SetPath(const std::wstring& path) { path_ = path; }

private:
    IMFSinkWriter* writer_ = nullptr;
    DWORD streamIndex_ = 0;
    LONGLONG framesWritten_ = 0;
    std::wstring path_;
};

// ---------------------------------------------------------------------------
// Output file contents
// ---------------------------------------------------------------------------
std::wstring BuildSessionInfo(const SessionConfig& config, int durationSeconds,
                              const std::wstring& resultStatus, bool transcriptSaved,
                              bool markdownSaved, int pendingChunks)
{
    std::wstring limitText;
    if (config.effectiveLimit == kRecordingOneHourSeconds)
        limitText = L"1 小时";
    else if (config.effectiveLimit == kRecordingTwoHoursSeconds)
        limitText = L"2 小时";
    else if (config.effectiveLimit == kRecordingUnlimited)
        limitText = L"不限";
    else
        limitText = std::to_wstring(config.effectiveLimit) + L" 秒";

    std::wstring files;
    if (transcriptSaved)
        files = L"转写.md";
    if (markdownSaved)
    {
        if (!files.empty())
            files += L"、";
        files += L"纪要.md";
    }
    if (pendingChunks > 0)
    {
        if (!files.empty())
            files += L"、";
        files += L"转写失败说明.txt";
    }
    if (files.empty())
        files = L"暂无";

    std::wstring text;
    text += L"灵动岛 · 会议记录\r\n";
    text += L"==============================\r\n";
    text += L"文件夹：" + FileNameOf(config.folderPath) + L"\r\n";
    text += L"保存位置：" + config.folderPath + L"\r\n";
    text += L"开始时间：" + config.startTimeText + L"\r\n";
    text += L"结束时间：" + NowText() + L"\r\n";
    text += L"录音时长：" + FormatDuration(durationSeconds) + L"（" +
            std::to_wstring(durationSeconds) + L" 秒）\r\n";
    text += L"录制上限：" + limitText + L"\r\n";
    text += L"结果文件：" + files + L"\r\n";
    if (pendingChunks > 0)
        text += L"待重转录片段： " + std::to_wstring(pendingChunks) +
                L" 个（含语音但转写失败，音频保留在临时目录，未放进本文件夹）\r\n";
    text += L"隐私说明：本文件夹只保存文字结果，录音音频不会保存在这里。\r\n";
    text += L"处理结果：" + resultStatus + L"\r\n";
    return text;
}

std::wstring BuildSummaryNote(const std::wstring& reason, bool transcriptSaved)
{
    std::wstring text;
    text += L"本次录音没有生成 AI 会议纪要。\r\n\r\n";
    text += L"原因：" + reason + L"\r\n\r\n";
    if (transcriptSaved)
        text += L"已保留转写文本：转写.md（可能不完整）。\r\n\r\n";
    else
        text += L"本次没有可用的转写文本。\r\n\r\n";
    text += L"录音音频已在转写流程结束后自动删除，不会保存在本文件夹中。\r\n\r\n";
    text += L"处理办法：\r\n";
    text += L"1. 打开灵动岛设置 →「会议与 AI」，检查接口地址、API Key 与模型名；\r\n";
    text += L"2. 确认网络可以访问该 AI 接口（代理 / 防火墙）；\r\n";
    text += L"3. 修正配置后重新录音即可。\r\n";
    return text;
}

// ---------------------------------------------------------------------------
// Background worker
// ---------------------------------------------------------------------------
using StartPromise = std::shared_ptr<std::promise<StartResult>>;

void WorkerMain(SessionConfig config, StartPromise promise)
{
    bool comReady = false;
    bool mfReady = false;
    bool startSignalled = false;
    LoopbackCapture capture;
    AacSinkWriter writer;
    writer.SetPath(config.audioPath);

    auto signalStart = [&](bool ok, const std::wstring& error)
    {
        if (startSignalled)
            return;
        startSignalled = true;
        StartResult result;
        result.ok = ok;
        result.error = error;
        promise->set_value(result);
    };

    auto failStart = [&](const std::wstring& error)
    {
        SetStateAndMessage(RecorderState::Idle, error);
        signalStart(false, error);
        capture.Close();
        writer.Abort();
        if (!config.keepTempAudio)
            RemoveDirectoryTree(config.tempDirectory);
        if (mfReady)
            MFShutdown();
        if (comReady)
            CoUninitialize();
    };

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult))
        comReady = true;
    else if (comResult != RPC_E_CHANGED_MODE)
    {
        failStart(L"音频线程初始化失败（COM 0x" + std::to_wstring(
                      static_cast<unsigned int>(comResult)) + L"）");
        return;
    }
    if (FAILED(MFStartup(MF_VERSION)))
    {
        failStart(L"Media Foundation 初始化失败，无法编码 M4A");
        return;
    }
    mfReady = true;

    std::wstring error;
    if (!EnsureDirectory(config.tempDirectory, &error))
    {
        failStart(error);
        return;
    }
    if (!capture.Open(&error))
    {
        failStart(error);
        return;
    }
    if (!writer.Open(config.audioPath, &error))
    {
        failStart(error);
        return;
    }
    if (!capture.Start(&error))
    {
        failStart(error);
        return;
    }

    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER startCounter = {};
    QueryPerformanceCounter(&startCounter);
    LARGE_INTEGER pauseStartCounter = {};
    bool pauseActive = false;
    long long pausedTicks = 0;

    auto activeMilliseconds = [&]() -> long long
    {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        long long ticks = now.QuadPart - startCounter.QuadPart - pausedTicks;
        if (pauseActive)
            ticks -= (now.QuadPart - pauseStartCounter.QuadPart);
        if (ticks < 0)
            ticks = 0;
        if (frequency.QuadPart <= 0)
            return 0;
        return ticks * 1000 / frequency.QuadPart;
    };

    SetStateAndMessage(RecorderState::Recording, L"正在录制系统声音…");
    signalStart(true, std::wstring());

    StereoResampler resampler(capture.Format().sampleRate, kOutputSampleRate);
    std::vector<float> captured;
    std::vector<float> resampled;
    std::vector<BYTE> pending;
    std::wstring captureError;
    bool writeError = false;
    bool autoStopped = false;
    int lastElapsed = -1;

    auto flushPending = [&](bool writeRemainder) -> bool
    {
        const size_t totalFrames = pending.size() / kBytesPerFrame;
        size_t frameIndex = 0;
        while (frameIndex < totalFrames)
        {
            const size_t remain = totalFrames - frameIndex;
            if (remain < static_cast<size_t>(kWriteFrames) && !writeRemainder)
                break;
            const size_t count = std::min<size_t>(kWriteFrames, remain);
            if (!writer.WritePcm(pending.data() + frameIndex * kBytesPerFrame,
                                 count * kBytesPerFrame, &error))
            {
                captureError = error;
                return false;
            }
            frameIndex += count;
        }
        if (frameIndex > 0)
            pending.erase(pending.begin(),
                          pending.begin() + static_cast<std::ptrdiff_t>(
                                                frameIndex * kBytesPerFrame));
        return true;
    };

    while (!IsStopRequested())
    {
        // ---- pause / resume bookkeeping ----
        const bool pauseNow = IsPauseRequested();
        LARGE_INTEGER nowCounter = {};
        QueryPerformanceCounter(&nowCounter);
        if (pauseNow && !pauseActive)
        {
            pauseActive = true;
            pauseStartCounter = nowCounter;
            SetStateAndMessage(RecorderState::Paused, L"录音已暂停");
        }
        else if (!pauseNow && pauseActive)
        {
            pausedTicks += nowCounter.QuadPart - pauseStartCounter.QuadPart;
            pauseActive = false;
            SetStateAndMessage(RecorderState::Recording, L"正在录制系统声音…");
        }

        // ---- drain the loopback buffer (discarding while paused) ----
        if (!capture.Pull(&captured, &error))
        {
            captureError = error;
            break;
        }
        if (!pauseNow && !captured.empty())
        {
            resampler.Push(captured.data(), captured.size() / 2);
            resampler.Drain(&resampled);
            AppendFloatStereoAsPcm16(resampled, &pending);
        }

        // ---- keep the timeline continuous (loopback delivers nothing when
        //      the system is silent, so pad with silence based on the clock) ----
        if (!pauseNow)
        {
            const long long expectedFrames = activeMilliseconds() * kOutputSampleRate / 1000;
            const long long queuedFrames =
                writer.FramesWritten() + static_cast<long long>(pending.size() / kBytesPerFrame);
            if (queuedFrames < expectedFrames)
            {
                const long long missing = std::min<long long>(
                    expectedFrames - queuedFrames, kOutputSampleRate);
                pending.insert(pending.end(),
                               static_cast<size_t>(missing) * kBytesPerFrame, 0);
            }
        }

        if (!flushPending(false))
        {
            writeError = true;
            break;
        }

        const int elapsedSeconds = static_cast<int>(activeMilliseconds() / 1000);
        if (elapsedSeconds != lastElapsed)
        {
            lastElapsed = elapsedSeconds;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_elapsed = elapsedSeconds;
        }

        const int limit = config.effectiveLimit;
        if (limit > 0 && elapsedSeconds >= limit)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stopRequested = true;
            }
            autoStopped = true;
            break;
        }

        Sleep(10);
    }

    // ---- finalise the temporary audio file ----
    SetStateAndMessage(RecorderState::Stopping, L"正在保存录音…");

    const long long finalActiveMs = activeMilliseconds();
    const long long targetFrames = finalActiveMs * kOutputSampleRate / 1000;
    while (true)
    {
        const long long queuedFrames =
            writer.FramesWritten() + static_cast<long long>(pending.size() / kBytesPerFrame);
        if (queuedFrames >= targetFrames)
            break;
        const long long missing = targetFrames - queuedFrames;
        pending.insert(pending.end(),
                       static_cast<size_t>(std::min<long long>(missing, kOutputSampleRate)) *
                           kBytesPerFrame,
                       0);
    }
    if (!writeError)
    {
        if (!flushPending(true))
            writeError = true;
    }

    unsigned long long audioBytes = 0;
    std::wstring finishError;
    if (!writeError)
        writeError = !writer.Finish(&finishError, &audioBytes);
    if (writeError)
        writer.Abort();
    capture.Close();

    const int durationSeconds = static_cast<int>(finalActiveMs / 1000);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_elapsed = durationSeconds;
    }

    if (audioBytes == 0 && !writeError)
    {
        HANDLE file = CreateFileW(config.audioPath.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size = {};
            if (GetFileSizeEx(file, &size))
                audioBytes = static_cast<unsigned long long>(size.QuadPart);
            CloseHandle(file);
        }
    }

    const bool audioOk = !writeError && audioBytes > 0;
    std::wstring audioProblem = finishError.empty() ? captureError : finishError;

    // ---- create the text-only session folder ----
    std::wstring folderError;
    bool folderReady = EnsureDirectory(config.folderPath, &folderError);
    if (!folderReady)
        folderError = L"无法创建保存文件夹：" + folderError;

    bool transcriptSaved = false;
    bool markdownSaved = false;
    int pendingChunks = 0;
    std::wstring pendingDirectory;
    std::wstring resultStatus;
    std::wstring uiMessage;

    if (!folderReady)
    {
        resultStatus = folderError;
        uiMessage = folderError;
    }
    else
    {
        WriteTextFileUtf8(config.infoPath,
                          BuildSessionInfo(config, durationSeconds, L"正在转写…", false,
                                           false, 0),
                          nullptr);
        if (!audioOk)
        {
            resultStatus = L"录音数据无效，无法转写";
            if (!audioProblem.empty())
                resultStatus += L"：" + audioProblem;
            std::wstring noteError;
            WriteTextFileUtf8(config.notePath,
                              BuildSummaryNote(audioProblem.empty() ? resultStatus
                                                                   : audioProblem,
                                               false),
                              &noteError);
            uiMessage = resultStatus;
        }
        else
        {
            SetStateAndMessage(RecorderState::Summarizing,
                               config.autoSummary ? L"正在转写并生成 AI 会议纪要…"
                                                  : L"正在转写录音…");
            std::wstring transcript;
            std::wstring summaryError;
            SummaryResult summaryResult;
            const bool ok = SummaryGenerate(
                config.audioPath, config.autoSummary ? config.markdownPath : std::wstring(),
                config.ai, &transcript, &summaryError, config.transcriptPath,
                config.failureNotePath, &summaryResult);
            transcriptSaved = summaryResult.transcribedChunks > 0 &&
                              FileExistsW(config.transcriptPath);
            markdownSaved = FileExistsW(config.markdownPath);
            pendingChunks = summaryResult.pendingChunks;
            pendingDirectory = summaryResult.pendingDirectory;

            if (pendingChunks > 0)
            {
                resultStatus = transcriptSaved
                                   ? (markdownSaved ? L"已生成 转写.md 与 纪要.md"
                                                    : L"已生成 转写.md")
                                   : L"未能转写任何文字";
                resultStatus +=
                    L"；部分片段转写失败，含语音的音频已保留在临时目录，详见 转写失败说明.txt";
                uiMessage = markdownSaved
                                ? L"纪要已生成（部分片段转写失败，音频已保留在临时目录）"
                                : (transcriptSaved
                                       ? L"转写已保存（部分片段转写失败，音频已保留在临时目录）"
                                       : L"转写失败（含语音片段已保留在临时目录）");
                if (config.autoSummary && transcriptSaved && !markdownSaved)
                {
                    std::wstring noteError;
                    WriteTextFileUtf8(
                        config.notePath,
                        BuildSummaryNote(summaryError.empty() ? L"AI 纪要生成失败"
                                                              : summaryError,
                                         true),
                        &noteError);
                }
            }
            else if (transcriptSaved && config.autoSummary && !markdownSaved)
            {
                resultStatus = L"已生成 转写.md；AI 纪要生成失败：" + summaryError;
                std::wstring noteError;
                WriteTextFileUtf8(
                    config.notePath,
                    BuildSummaryNote(summaryError.empty() ? L"AI 纪要生成失败"
                                                          : summaryError,
                                     true),
                    &noteError);
                uiMessage = L"转写已保存（AI 纪要生成失败）";
            }
            else if (ok && transcriptSaved)
            {
                resultStatus = markdownSaved ? L"已生成 转写.md 与 纪要.md"
                                             : L"已生成 转写.md";
                uiMessage = markdownSaved ? L"会议纪要与转写已保存"
                                          : L"转写已保存（未生成 AI 纪要）";
            }
            else
            {
                resultStatus = L"转写或 AI 纪要生成失败：" + summaryError;
                if (transcriptSaved)
                    resultStatus += L"（已保留已转写的文字）";
                std::wstring noteError;
                WriteTextFileUtf8(config.notePath,
                                  BuildSummaryNote(summaryError, transcriptSaved),
                                  &noteError);
                uiMessage = transcriptSaved ? L"AI 纪要生成失败，已保留部分转写"
                                            : L"转写失败：" + summaryError;
            }
        }
        WriteTextFileUtf8(config.infoPath,
                          BuildSessionInfo(config, durationSeconds, resultStatus,
                                           transcriptSaved, markdownSaved,
                                           pendingChunks),
                          nullptr);
    }

    // ---- temporary audio: always deleted after transcription. Chunks whose
    //      speech could not be transcribed are kept under
    //      %TEMP%\DynamicIslandRec\pending\<session> for a later re-try. ----
    if (!config.keepTempAudio)
    {
        DeleteFileIfExists(config.audioPath);
        RemoveDirectoryTree(config.tempDirectory);
    }
    if (pendingChunks == 0)
    {
        const std::wstring stalePending =
            pendingDirectory.empty() ? config.pendingDirectory : pendingDirectory;
        RemoveDirectoryTree(stalePending);
    }

    // ---- publish the final state ----
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_folder = folderReady ? config.folderPath : std::wstring();
        g_audio.clear(); // audio is never exposed / kept
        g_transcript = transcriptSaved ? config.transcriptPath : std::wstring();
        g_markdown = markdownSaved ? config.markdownPath : std::wstring();
        g_pendingChunks = pendingChunks;
        g_pendingDirectory = pendingChunks > 0 ? pendingDirectory : std::wstring();
        g_message = uiMessage;
        g_state = RecorderState::Idle;
        g_pauseRequested = false;
    }

    if (mfReady)
        MFShutdown();
    if (comReady)
        CoUninitialize();
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================
bool RecorderStart(int limitSeconds, const std::wstring& parentDirectory,
                   const std::wstring& folderName)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state != RecorderState::Idle)
        {
            g_message = L"已有录音任务正在进行，请先停止";
            return false;
        }
        g_stopRequested = false;
        g_pauseRequested = false;
        g_elapsed = 0;
        g_audio.clear();
        g_markdown.clear();
        g_transcript.clear();
        g_pendingChunks = 0;
        g_pendingDirectory.clear();
        g_message.clear();
    }

    // Text-only product: without a configured AI endpoint there is nothing to
    // save (audio is never kept), so refuse to start.
    const bool aiBaseConfigured =
        g_settings.aiApiBase.find_first_not_of(L" \t\r\n") != std::wstring::npos;
    if (g_settings.aiApiKey.empty() || !aiBaseConfigured)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = RecorderState::Idle;
        g_audio.clear();
        g_markdown.clear();
        g_transcript.clear();
        g_pendingChunks = 0;
        g_pendingDirectory.clear();
        g_folder.clear();
        g_message = L"未配置 AI 接口与 API Key，无法保存文字纪要。请先在设置 →「会议与 AI」中配置后重试。";
        return false;
    }

    SessionConfig config;
    config.requestedLimit = limitSeconds;
    config.effectiveLimit = ApplyLimitOverride(limitSeconds);
    config.parentDirectory = parentDirectory;
    if (config.parentDirectory.empty())
    {
        if (!g_settings.recordSaveDir.empty())
            config.parentDirectory = g_settings.recordSaveDir;
        else
            config.parentDirectory = GetExeDirectory() + L"\\recordings";
    }
    config.parentDirectory = MakeAbsolutePath(config.parentDirectory);
    config.folderName = folderName.empty() ? RecorderMakeFolderName() : folderName;
    config.folderPath = config.parentDirectory + L"\\" + config.folderName;
    config.tempDirectory = GetTempDirectory() + L"\\" + std::wstring(kTempRootName) +
                           L"\\" + config.folderName;
    config.audioPath = config.tempDirectory + L"\\" + kTempAudioFileName;
    config.transcriptPath = config.folderPath + L"\\" + kTranscriptFileName;
    config.autoSummary = g_settings.recordAutoSummary;
    config.markdownPath =
        config.autoSummary ? config.folderPath + L"\\" + kSummaryFileName : std::wstring();
    config.infoPath = config.folderPath + L"\\" + kSessionInfoFileName;
    config.notePath = config.folderPath + L"\\" + kSummaryNoteFileName;
    config.failureNotePath = config.folderPath + L"\\" + kFailureNoteFileName;
    config.pendingDirectory = GetTempDirectory() + L"\\" + std::wstring(kTempRootName) +
                             L"\\pending\\" + config.folderName;
    config.startTimeText = NowText();
    config.keepTempAudio = KeepTempAudioFromEnv();
    config.ai.baseUrl = g_settings.aiApiBase;
    config.ai.apiKey = g_settings.aiApiKey;
    config.ai.sttModel = g_settings.aiSttModel;
    config.ai.summaryModel = g_settings.aiSummaryModel;

    // Remove leftovers of a previous crashed session with the same folder name.
    if (!config.keepTempAudio)
    {
        RemoveDirectoryTree(config.tempDirectory);
        RemoveDirectoryTree(config.pendingDirectory);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_limit = config.effectiveLimit;
        g_folder = config.folderPath;
    }

    auto promise = std::make_shared<std::promise<StartResult>>();
    std::future<StartResult> future = promise->get_future();
    std::thread worker(WorkerMain, config, promise);
    worker.detach();

    if (future.wait_for(std::chrono::seconds(8)) != std::future_status::ready)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stopRequested = true;
        g_state = RecorderState::Idle;
        g_message = L"打开系统声音超时，无法开始录音";
        return false;
    }

    const StartResult result = future.get();
    if (!result.ok)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = RecorderState::Idle;
        if (!result.error.empty())
            g_message = result.error;
        return false;
    }
    return true;
}

void RecorderPause()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state == RecorderState::Recording)
    {
        g_pauseRequested = true;
        g_state = RecorderState::Paused;
        g_message = L"录音已暂停";
    }
}

void RecorderResume()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state == RecorderState::Paused)
    {
        g_pauseRequested = false;
        g_state = RecorderState::Recording;
        g_message = L"正在录制系统声音…";
    }
}

void RecorderStop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state == RecorderState::Recording || g_state == RecorderState::Paused)
    {
        g_pauseRequested = false;
        g_stopRequested = true;
        g_state = RecorderState::Stopping;
        g_message = L"正在保存录音…";
    }
}

RecorderState RecorderGetState()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

int RecorderGetElapsedSeconds()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_elapsed;
}

int RecorderGetLimitSeconds()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_limit;
}

std::wstring RecorderGetLastFolder()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_folder;
}

std::wstring RecorderGetLastAudioPath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_audio;
}

std::wstring RecorderGetLastMarkdownPath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_markdown;
}

std::wstring RecorderGetLastTranscriptPath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_transcript;
}

int RecorderGetPendingChunkCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pendingChunks;
}

std::wstring RecorderGetPendingChunkDirectory()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pendingDirectory;
}

std::wstring RecorderGetLastMessage()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_message;
}

std::wstring RecorderMakeFolderName()
{
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"会议纪要_%04d%02d%02d_%02d%02d%02d", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}
