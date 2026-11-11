// ===========================================================================
//  src/summary.cpp  --  v5.4 AI transcription + meeting summary pipeline
//
//  SummaryGenerate():
//    1. decode the recording (m4a / wav, via Media Foundation SourceReader)
//       to 16 kHz mono PCM16 and split it into ~8 minute WAV chunks
//       (<= 19 MB per request, the common speech-to-text upload limit);
//    2. POST each chunk to {base}/audio/transcriptions as multipart/form-data
//       (file=chunk_N.wav, model=<sttModel>, response_format=json) and
//       concatenate the returned "text" values;
//    3. POST the transcript to {base}/chat/completions with a Chinese
//       meeting-minutes system prompt and write choices[0].message.content
//       into <markdownPath> behind a small Markdown header.
//
//  OpenAI-compatible, WinHTTP, UTF-8. No third-party libraries; the static
//  libraries are attached with #pragma comment(lib, ...).
//
//  The base URL is normalised: a trailing slash is tolerated, a plain host
//  without a scheme is accepted (http for loopback hosts, https otherwise).
// ===========================================================================

#include "summary.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
constexpr int kTargetSampleRate = 16000;
constexpr int kChunkSeconds = 480; // 8 minutes
constexpr long long kChunkSamples =
    static_cast<long long>(kChunkSeconds) * kTargetSampleRate;
constexpr unsigned int kChunkPcmBytes =
    static_cast<unsigned int>(kChunkSamples * 2); // 15.36 MB < 19 MB
constexpr size_t kMaxErrorBodyChars = 400;
constexpr DWORD kFirstAudioStream = 0xFFFFFFFDu; // MF_SOURCE_READER_FIRST_AUDIO_STREAM
constexpr DWORD kAllStreams = 0xFFFFFFFEu;       // MF_SOURCE_READER_ALL_STREAMS

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
std::string TrimAscii(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ')
        ++begin;
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ')
        --end;
    return text.substr(begin, end - begin);
}

std::wstring TrimWide(const std::wstring& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && text[begin] <= L' ')
        ++begin;
    while (end > begin && text[end - 1] <= L' ')
        --end;
    return text.substr(begin, end - begin);
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring();
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

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

std::wstring Hex32(unsigned int value)
{
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"%08X", value);
    return buffer;
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

std::wstring FileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirectoryOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool WriteTextFileUtf8(const std::wstring& path, const std::string& bytes,
                       std::wstring* error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (error != nullptr)
            *error = L"无法写入文件（错误码 " + std::to_wstring(GetLastError()) +
                     L"）：" + path;
        return false;
    }
    bool ok = true;
    size_t written = 0;
    while (written < bytes.size())
    {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - written, 1u << 20));
        DWORD actual = 0;
        if (!WriteFile(file, bytes.data() + written, chunk, &actual, nullptr) ||
            actual == 0)
        {
            ok = false;
            if (error != nullptr)
                *error = L"写入文件失败（错误码 " + std::to_wstring(GetLastError()) +
                         L"）：" + path;
            break;
        }
        written += actual;
    }
    CloseHandle(file);
    return ok;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (only the OpenAI response shapes are needed)
// ---------------------------------------------------------------------------
size_t SkipJsonWhitespace(const std::string& text, size_t position)
{
    while (position < text.size())
    {
        const char c = text[position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        ++position;
    }
    return position;
}

// Returns the index of the opening quote of the *string value* for "key".
size_t FindJsonStringValue(const std::string& json, const std::string& key,
                           size_t fromPosition)
{
    const std::string needle = "\"" + key + "\"";
    size_t position = json.find(needle, fromPosition);
    while (position != std::string::npos)
    {
        size_t cursor = SkipJsonWhitespace(json, position + needle.size());
        if (cursor < json.size() && json[cursor] == ':')
        {
            cursor = SkipJsonWhitespace(json, cursor + 1);
            if (cursor < json.size() && json[cursor] == '"')
                return cursor;
        }
        position = json.find(needle, position + needle.size());
    }
    return std::string::npos;
}

void AppendUtf8CodePoint(unsigned int codePoint, std::string* out)
{
    if (codePoint <= 0x7F)
    {
        out->push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7FF)
    {
        out->push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0xFFFF)
    {
        out->push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        out->push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out->push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

int HexDigitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool ReadJsonString(const std::string& json, size_t quotePosition, std::string* out)
{
    if (quotePosition >= json.size() || json[quotePosition] != '"')
        return false;
    std::string result;
    size_t position = quotePosition + 1;
    while (position < json.size())
    {
        const char c = json[position];
        if (c == '"')
        {
            *out = std::move(result);
            return true;
        }
        if (c != '\\')
        {
            result.push_back(c);
            ++position;
            continue;
        }
        ++position;
        if (position >= json.size())
            return false;
        const char escape = json[position];
        switch (escape)
        {
        case '"':
            result.push_back('"');
            ++position;
            break;
        case '\\':
            result.push_back('\\');
            ++position;
            break;
        case '/':
            result.push_back('/');
            ++position;
            break;
        case 'b':
            result.push_back('\b');
            ++position;
            break;
        case 'f':
            result.push_back('\f');
            ++position;
            break;
        case 'n':
            result.push_back('\n');
            ++position;
            break;
        case 'r':
            result.push_back('\r');
            ++position;
            break;
        case 't':
            result.push_back('\t');
            ++position;
            break;
        case 'u':
        {
            if (position + 4 >= json.size())
                return false;
            int code = 0;
            for (int i = 0; i < 4; ++i)
            {
                const int digit = HexDigitValue(json[position + 1 + static_cast<size_t>(i)]);
                if (digit < 0)
                    return false;
                code = code * 16 + digit;
            }
            position += 5;
            unsigned int codePoint = static_cast<unsigned int>(code);
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF && position + 6 <= json.size() &&
                json[position] == '\\' && json[position + 1] == 'u')
            {
                int low = 0;
                bool valid = true;
                for (int i = 0; i < 4; ++i)
                {
                    const int digit =
                        HexDigitValue(json[position + 2 + static_cast<size_t>(i)]);
                    if (digit < 0)
                    {
                        valid = false;
                        break;
                    }
                    low = low * 16 + digit;
                }
                if (valid && low >= 0xDC00 && low <= 0xDFFF)
                {
                    codePoint = 0x10000 + (static_cast<unsigned int>(codePoint - 0xD800)
                                           << 10) +
                                static_cast<unsigned int>(low - 0xDC00);
                    position += 6;
                }
            }
            AppendUtf8CodePoint(codePoint, &result);
            break;
        }
        default:
            result.push_back(escape);
            ++position;
            break;
        }
    }
    return false;
}

bool GetJsonStringField(const std::string& json, const std::string& key,
                        size_t fromPosition, std::string* out, size_t* nextPosition)
{
    const size_t quote = FindJsonStringValue(json, key, fromPosition);
    if (quote == std::string::npos)
        return false;
    return ReadJsonString(json, quote, out) &&
           (nextPosition == nullptr || (*nextPosition = quote + out->size(), true));
}

// Extracts choices[0].message.content from a chat completion response.
bool ExtractChatContent(const std::string& body, std::string* content)
{
    const size_t choices = body.find("\"choices\"");
    if (choices == std::string::npos)
        return false;
    const size_t message = body.find("\"message\"", choices);
    if (message == std::string::npos)
        return false;
    return GetJsonStringField(body, "content", message, content, nullptr);
}

bool TryExtractApiError(const std::string& body, std::string* message)
{
    const size_t error = body.find("\"error\"");
    if (error == std::string::npos)
        return false;
    return GetJsonStringField(body, "message", error, message, nullptr);
}

std::string BuildBodySnippet(const std::string& body)
{
    std::string snippet = TrimAscii(body);
    if (snippet.size() > kMaxErrorBodyChars)
        snippet.resize(kMaxErrorBodyChars);
    for (char& c : snippet)
    {
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
    }
    return TrimAscii(snippet);
}

// ---------------------------------------------------------------------------
// URL normalisation
// ---------------------------------------------------------------------------
bool IsLoopbackHost(const std::wstring& host)
{
    return _wcsicmp(host.c_str(), L"localhost") == 0 ||
           _wcsicmp(host.c_str(), L"127.0.0.1") == 0 ||
           _wcsicmp(host.c_str(), L"::1") == 0 ||
           _wcsicmp(host.c_str(), L"[::1]") == 0;
}

bool CrackUrl(const std::wstring& url, URL_COMPONENTS* components, std::wstring* error)
{
    URL_COMPONENTS local = {};
    local.dwStructSize = sizeof(local);
    local.dwHostNameLength = static_cast<DWORD>(-1);
    local.dwUrlPathLength = static_cast<DWORD>(-1);
    local.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &local))
    {
        if (error != nullptr)
            *error = L"AI 接口地址无效：" + url;
        return false;
    }
    *components = local;
    return true;
}

std::wstring UrlPathOf(const URL_COMPONENTS& components)
{
    std::wstring path;
    if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0)
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (path.empty())
        path = L"/";
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0)
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    return path;
}

// Base URL -> "scheme://host[:port][/path]" without a trailing slash.
bool NormalizeBaseUrl(const std::wstring& raw, std::wstring* normalized,
                      std::wstring* error)
{
    std::wstring url = TrimWide(raw);
    if (url.empty())
    {
        if (error != nullptr)
            *error = L"未配置 AI 接口地址";
        return false;
    }
    if (url.find(L"://") == std::wstring::npos)
    {
        const std::wstring lower = url;
        const bool local = lower.rfind(L"localhost", 0) == 0 ||
                           lower.rfind(L"127.", 0) == 0 ||
                           lower.rfind(L"[::1]", 0) == 0;
        url = (local ? L"http://" : L"https://") + url;
    }
    while (!url.empty() && url.back() == L'/')
        url.pop_back();
    if (url.empty())
    {
        if (error != nullptr)
            *error = L"AI 接口地址无效：" + raw;
        return false;
    }
    URL_COMPONENTS components = {};
    if (!CrackUrl(url, &components, error))
        return false;
    if (components.lpszHostName == nullptr || components.dwHostNameLength == 0)
    {
        if (error != nullptr)
            *error = L"AI 接口地址缺少主机名：" + raw;
        return false;
    }
    *normalized = std::move(url);
    return true;
}

// ---------------------------------------------------------------------------
// WinHTTP POST
// ---------------------------------------------------------------------------
struct HttpResponse
{
    DWORD status = 0;
    std::string body;
};

bool HttpPost(const std::wstring& url, const std::wstring& apiKey,
              const std::string& contentType, const std::string& body,
              HttpResponse* response, std::wstring* error)
{
    URL_COMPONENTS components = {};
    if (!CrackUrl(url, &components, error))
        return false;
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path = UrlPathOf(components);
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const bool local = IsLoopbackHost(host);

    HINTERNET session = WinHttpOpen(
        L"DynamicIsland/5.4",
        local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        if (error != nullptr)
            *error = L"无法初始化网络会话（错误码 " +
                     std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 300000, 600000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (connection == nullptr)
    {
        if (error != nullptr)
            *error = L"无法连接 AI 接口 " + host + L"（错误码 " +
                     std::to_wstring(GetLastError()) + L"）";
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
    {
        if (error != nullptr)
            *error = L"无法创建网络请求（错误码 " +
                     std::to_wstring(GetLastError()) + L"）";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring headers = L"Content-Type: " + Utf8ToWide(contentType) + L"\r\n";
    headers += L"Accept: application/json\r\n";
    if (!apiKey.empty())
        headers += L"Authorization: Bearer " + apiKey + L"\r\n";

    const DWORD bodyLength = static_cast<DWORD>(body.size());
    BOOL ok = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        bodyLength, bodyLength, 0);
    if (!ok)
    {
        if (error != nullptr)
            *error = L"无法发送 AI 请求（错误码 " +
                     std::to_wstring(GetLastError()) + L"）";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok)
    {
        if (error != nullptr)
            *error = L"AI 接口无响应（错误码 " + std::to_wstring(GetLastError()) +
                     L"）";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);

    std::string responseBody;
    bool readOk = true;
    while (true)
    {
        char buffer[16384] = {};
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, static_cast<DWORD>(sizeof(buffer)), &read))
        {
            readOk = false;
            if (error != nullptr)
                *error = L"读取 AI 响应失败（错误码 " +
                         std::to_wstring(GetLastError()) + L"）";
            break;
        }
        if (read == 0)
            break;
        responseBody.append(buffer, read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!readOk)
        return false;
    if (response != nullptr)
    {
        response->status = status;
        response->body = std::move(responseBody);
    }
    return true;
}

// ---------------------------------------------------------------------------
// WAV / multipart bodies
// ---------------------------------------------------------------------------
void AppendLe16(std::string* out, unsigned int value)
{
    out->push_back(static_cast<char>(value & 0xFF));
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
}

void AppendLe32(std::string* out, unsigned int value)
{
    out->push_back(static_cast<char>(value & 0xFF));
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
    out->push_back(static_cast<char>((value >> 16) & 0xFF));
    out->push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::string BuildWavFile(const std::vector<int16_t>& samples)
{
    const unsigned int dataBytes = static_cast<unsigned int>(samples.size() * 2);
    std::string wav;
    wav.reserve(44 + dataBytes);
    wav.append("RIFF", 4);
    AppendLe32(&wav, 36 + dataBytes);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    AppendLe32(&wav, 16);
    AppendLe16(&wav, 1);                       // PCM
    AppendLe16(&wav, 1);                       // mono
    AppendLe32(&wav, kTargetSampleRate);       // sample rate
    AppendLe32(&wav, kTargetSampleRate * 2);   // byte rate
    AppendLe16(&wav, 2);                       // block align
    AppendLe16(&wav, 16);                      // bits per sample
    wav.append("data", 4);
    AppendLe32(&wav, dataBytes);
    const size_t offset = wav.size();
    wav.resize(offset + dataBytes);
    if (dataBytes > 0)
        std::memcpy(&wav[offset], samples.data(), dataBytes);
    return wav;
}

std::string MakeBoundary()
{
    static unsigned long long counter = 0;
    ++counter;
    char buffer[96] = {};
    snprintf(buffer, sizeof(buffer), "----DynamicIslandBoundary%08llx%04llx%04llx",
             static_cast<unsigned long long>(GetTickCount64()) & 0xFFFFFFFFull,
             counter & 0xFFFFull,
             static_cast<unsigned long long>(GetCurrentProcessId()) & 0xFFFFull);
    return buffer;
}

std::string BuildTranscriptionBody(const std::string& boundary,
                                   const std::string& wav, const std::string& model)
{
    std::string body;
    body.reserve(wav.size() + 1024);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    body += model;
    body += "\r\n--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"chunk.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body += wav;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

std::string JsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 32);
    for (const char rawChar : text)
    {
        const unsigned char c = static_cast<unsigned char>(rawChar);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buffer[8] = {};
                snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            }
            else
            {
                out.push_back(rawChar);
            }
            break;
        }
    }
    return out;
}

const char* kMeetingMinutesSystemPrompt =
    "你是一名专业的中文会议秘书。请根据用户提供的会议转写文本，整理一份结构清晰、"
    "忠实于原文的 Markdown 会议纪要。要求：\n"
    "1. 只依据转写内容，不要编造事实；无法确定的信息写“未提及”。\n"
    "2. 直接输出 Markdown 正文，不要用代码块包裹，不要再重复“# 会议纪要”大标题。\n"
    "3. 建议包含以下小节：## 会议主题、## 主要内容（按议题归纳）、"
    "## 结论与决议、## 待办事项（负责人 / 事项 / 时间，缺失写“未提及”）。\n"
    "4. 如果转写文本存在明显的语音识别错误或错别字，可以结合上下文合理修正。\n"
    "5. 行文简洁、分点清晰，方便人工快速阅读和复制。";

std::string BuildChatBody(const std::string& model, const std::string& transcriptUtf8)
{
    std::string body;
    body.reserve(transcriptUtf8.size() + 2048);
    body += "{\"model\":\"";
    body += JsonEscape(model);
    body += "\",\"temperature\":0.3,\"messages\":[{\"role\":\"system\",\"content\":\"";
    body += JsonEscape(kMeetingMinutesSystemPrompt);
    body += "\"},{\"role\":\"user\",\"content\":\"";
    body += JsonEscape(transcriptUtf8);
    body += "\"}]}";
    return body;
}

// ---------------------------------------------------------------------------
// Audio decoding (Media Foundation SourceReader -> 16 kHz mono PCM16)
// ---------------------------------------------------------------------------
class MediaFoundationSession
{
public:
    MediaFoundationSession() : initialized_(SUCCEEDED(MFStartup(MF_VERSION))) {}
    ~MediaFoundationSession()
    {
        if (initialized_)
            MFShutdown();
    }
    bool IsReady() const { return initialized_; }

private:
    bool initialized_ = false;
};

struct InputAudioType
{
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    bool isFloat = false;
    bool directPcm16 = false; // already 16 kHz mono PCM16
};

bool ParseReaderType(IMFMediaType* type, InputAudioType* out, std::wstring* error)
{
    if (type == nullptr || out == nullptr)
        return false;
    GUID major = {};
    GUID subtype = {};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
        !IsEqualGUID(major, MFMediaType_Audio))
    {
        if (error != nullptr)
            *error = L"音频文件不包含音频流";
        return false;
    }
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
    {
        if (error != nullptr)
            *error = L"无法读取音频格式";
        return false;
    }
    out->isFloat = IsEqualGUID(subtype, MFAudioFormat_Float);
    if (!out->isFloat && !IsEqualGUID(subtype, MFAudioFormat_PCM))
    {
        if (error != nullptr)
            *error = L"不支持的音频编码格式（需要先解码为 PCM / 浮点）";
        return false;
    }
    UINT32 value = 0;
    if (FAILED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &value)))
        return false;
    out->sampleRate = static_cast<int>(value);
    if (FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value)))
        return false;
    out->channels = static_cast<int>(value);
    value = 16;
    type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &value);
    out->bits = out->isFloat ? 32 : static_cast<int>(value);
    if (out->sampleRate <= 0 || out->channels <= 0 || out->bits <= 0)
    {
        if (error != nullptr)
            *error = L"音频格式信息不完整";
        return false;
    }
    out->directPcm16 = !out->isFloat && out->bits == 16 &&
                       out->sampleRate == kTargetSampleRate && out->channels == 1;
    return true;
}

bool OpenAudioReader(const std::wstring& audioPath, IMFSourceReader** reader,
                     InputAudioType* type, std::wstring* error)
{
    *reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(audioPath.c_str(), nullptr, reader);
    if (FAILED(hr))
    {
        if (error != nullptr)
            *error = L"无法打开录音文件（Media Foundation 0x" +
                     Hex32(static_cast<unsigned int>(hr)) + L"）：" + audioPath;
        return false;
    }
    IMFSourceReader* sourceReader = *reader;
    hr = sourceReader->SetStreamSelection(kAllStreams, FALSE);
    if (SUCCEEDED(hr))
        hr = sourceReader->SetStreamSelection(kFirstAudioStream, TRUE);
    if (FAILED(hr))
    {
        if (error != nullptr)
            *error = L"录音文件没有可用的音频流（0x" +
                     Hex32(static_cast<unsigned int>(hr)) + L"）";
        sourceReader->Release();
        *reader = nullptr;
        return false;
    }

    // Ask the source reader to produce exactly what the transcription API
    // wants; MF inserts the decoder + audio resampler when it can.
    IMFMediaType* target = nullptr;
    hr = MFCreateMediaType(&target);
    if (SUCCEEDED(hr))
    {
        target->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        target->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        target->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        target->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kTargetSampleRate);
        target->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        target->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        target->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                          kTargetSampleRate * 2);
        hr = sourceReader->SetCurrentMediaType(kFirstAudioStream,
                                               nullptr, target);
    }
    if (target != nullptr)
        target->Release();

    IMFMediaType* current = nullptr;
    if (SUCCEEDED(sourceReader->GetCurrentMediaType(kFirstAudioStream, &current)))
    {
        const bool parsed = ParseReaderType(current, type, error);
        current->Release();
        if (!parsed)
        {
            sourceReader->Release();
            *reader = nullptr;
            return false;
        }
        return true;
    }

    if (error != nullptr)
        *error = L"无法获取录音文件的音频格式";
    sourceReader->Release();
    *reader = nullptr;
    return false;
}

// Linear mono resampler to 16 kHz.
class MonoResampler
{
public:
    MonoResampler(int sourceRate, int targetRate)
        : sourceRate_(sourceRate > 0 ? sourceRate : targetRate),
          targetRate_(targetRate > 0 ? targetRate : kTargetSampleRate),
          ratio_(static_cast<double>(sourceRate_) / static_cast<double>(targetRate_))
    {
    }

    void Push(const float* monoSamples, size_t frames)
    {
        if (monoSamples == nullptr || frames == 0)
            return;
        buffer_.insert(buffer_.end(), monoSamples, monoSamples + frames);
    }

    void Drain(std::vector<float>* out)
    {
        out->clear();
        const size_t frames = buffer_.size();
        while (true)
        {
            const size_t index = static_cast<size_t>(position_);
            if (index + 1 >= frames)
                break;
            const double fraction = position_ - static_cast<double>(index);
            const float sample0 = buffer_[index];
            const float sample1 = buffer_[index + 1];
            out->push_back(static_cast<float>(sample0 + (sample1 - sample0) * fraction));
            position_ += ratio_;
        }
        const size_t consumed = static_cast<size_t>(position_);
        if (consumed > 0 && consumed <= frames)
        {
            buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
            position_ -= static_cast<double>(consumed);
        }
    }

private:
    int sourceRate_ = kTargetSampleRate;
    int targetRate_ = kTargetSampleRate;
    double ratio_ = 1.0;
    double position_ = 0.0;
    std::vector<float> buffer_;
};

void AppendFloatsAsPcm16(const std::vector<float>& floats, std::vector<int16_t>* pcm)
{
    const size_t base = pcm->size();
    pcm->resize(base + floats.size());
    for (size_t i = 0; i < floats.size(); ++i)
    {
        float value = floats[i];
        if (value > 1.0f)
            value = 1.0f;
        else if (value < -1.0f)
            value = -1.0f;
        (*pcm)[base + i] = static_cast<int16_t>(std::lrintf(value * 32767.0f));
    }
}

float ReadFloatSample(const BYTE* data, int bits, bool isFloat)
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

// Appends one decoded sample to the mono 16 kHz PCM accumulator.
bool AppendSample(IMFSample* sample, const InputAudioType& type,
                  MonoResampler* resampler, std::vector<int16_t>* pcm,
                  long long* producedFrames, std::wstring* error)
{
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || buffer == nullptr)
    {
        if (error != nullptr)
            *error = L"无法读取解码后的音频数据（0x" +
                     Hex32(static_cast<unsigned int>(hr)) + L"）";
        return false;
    }
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&data, &maxLength, &currentLength);
    if (FAILED(hr) || data == nullptr)
    {
        buffer->Release();
        if (error != nullptr)
            *error = L"无法锁定解码后的音频数据（0x" +
                     Hex32(static_cast<unsigned int>(hr)) + L"）";
        return false;
    }

    bool ok = true;
    if (type.directPcm16)
    {
        const size_t count = currentLength / sizeof(int16_t);
        const size_t base = pcm->size();
        pcm->resize(base + count);
        if (count > 0)
            std::memcpy(pcm->data() + base, data, count * sizeof(int16_t));
        *producedFrames += static_cast<long long>(count);
    }
    else
    {
        const int bytesPerSample = type.bits / 8;
        const size_t frameBytes =
            static_cast<size_t>(bytesPerSample) * static_cast<size_t>(type.channels);
        if (bytesPerSample <= 0 || frameBytes == 0)
        {
            ok = false;
            if (error != nullptr)
                *error = L"不支持的音频位深";
        }
        else
        {
            const size_t frameCount = currentLength / frameBytes;
            std::vector<float> mono(frameCount, 0.0f);
            for (size_t frame = 0; frame < frameCount; ++frame)
            {
                const BYTE* frameData = data + frame * frameBytes;
                float sum = 0.0f;
                for (int channel = 0; channel < type.channels; ++channel)
                {
                    sum += ReadFloatSample(
                        frameData + static_cast<size_t>(channel) *
                                        static_cast<size_t>(bytesPerSample),
                        type.bits, type.isFloat);
                }
                mono[frame] = sum / static_cast<float>(type.channels);
            }
            resampler->Push(mono.data(), mono.size());
            std::vector<float> resampled;
            resampler->Drain(&resampled);
            AppendFloatsAsPcm16(resampled, pcm);
            *producedFrames += static_cast<long long>(resampled.size());
        }
    }

    buffer->Unlock();
    buffer->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Results / errors
// ---------------------------------------------------------------------------
void SetErrorMessage(std::wstring* errorOut, const std::wstring& message)
{
    if (errorOut != nullptr)
        *errorOut = message;
}

std::wstring HttpFailureMessage(const wchar_t* what, const HttpResponse& response)
{
    std::wstring detail;
    std::string apiMessage;
    if (TryExtractApiError(response.body, &apiMessage) && !apiMessage.empty())
        detail = L"：" + Utf8ToWide(TrimAscii(apiMessage));
    else
    {
        const std::string snippet = BuildBodySnippet(response.body);
        if (!snippet.empty())
            detail = L"：" + Utf8ToWide(snippet);
    }
    return std::wstring(what) + L"（HTTP " + std::to_wstring(response.status) + L"）" +
           detail;
}

bool EnsureDirectory(const std::wstring& directory, std::wstring* error)
{
    if (directory.empty())
        return true;
    const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS ||
        result == ERROR_ALREADY_EXISTS)
        return true;
    if (error != nullptr)
        *error = L"无法创建纪要输出目录（错误码 " + std::to_wstring(result) +
                 L"）：" + directory;
    return false;
}

// ---------------------------------------------------------------------------
// Streaming transcript file (opened lazily on the first successful chunk and
// flushed after every chunk so no transcribed speech can be lost)
// ---------------------------------------------------------------------------
class TranscriptFile
{
public:
    ~TranscriptFile() { Close(); }

    bool Open(const std::wstring& path, std::wstring* error)
    {
        Close();
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
        {
            if (error != nullptr)
                *error = L"无法创建转写文件（错误码 " + std::to_wstring(GetLastError()) +
                         L"）：" + path;
            return false;
        }
        return true;
    }

    bool Write(const std::string& utf8)
    {
        if (file_ == INVALID_HANDLE_VALUE)
            return false;
        size_t written = 0;
        while (written < utf8.size())
        {
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(utf8.size() - written, 1u << 20));
            DWORD actual = 0;
            if (!WriteFile(file_, utf8.data() + written, chunk, &actual, nullptr) ||
                actual == 0)
                return false;
            written += actual;
        }
        FlushFileBuffers(file_);
        return true;
    }

    bool Write(const std::wstring& text) { return Write(WideToUtf8(text)); }

    void Close()
    {
        if (file_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

// ---------------------------------------------------------------------------
// Speech detection for a chunk that failed to transcribe
// ---------------------------------------------------------------------------
constexpr double kSpeechRmsDbfsThreshold = -50.0;
constexpr int kSpeechFrameSamples = 320; // 20 ms at 16 kHz
constexpr int kSpeechMinVoicedFrames = 5; // >= 100 ms of non-silent audio

struct SpeechStats
{
    bool hasSpeech = false;
    double maxFrameDbfs = -120.0;
    int voicedFrames = 0;
    int totalFrames = 0;
};

SpeechStats AnalyzeChunkSpeech(const std::vector<int16_t>& pcm)
{
    SpeechStats stats;
    if (pcm.empty())
        return stats;
    const size_t frameCount =
        (pcm.size() + static_cast<size_t>(kSpeechFrameSamples) - 1) /
        static_cast<size_t>(kSpeechFrameSamples);
    stats.totalFrames = static_cast<int>(frameCount);
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        const size_t begin = frame * static_cast<size_t>(kSpeechFrameSamples);
        const size_t end = std::min(begin + static_cast<size_t>(kSpeechFrameSamples),
                                    pcm.size());
        double energy = 0.0;
        for (size_t i = begin; i < end; ++i)
        {
            const double sample = static_cast<double>(pcm[i]) / 32768.0;
            energy += sample * sample;
        }
        const size_t count = end - begin;
        if (count == 0)
            continue;
        const double rms = std::sqrt(energy / static_cast<double>(count));
        const double dbfs = rms > 1e-7 ? 20.0 * std::log10(rms) : -120.0;
        if (dbfs > stats.maxFrameDbfs)
            stats.maxFrameDbfs = dbfs;
        if (dbfs > kSpeechRmsDbfsThreshold)
            ++stats.voicedFrames;
    }
    stats.hasSpeech = stats.voicedFrames >= kSpeechMinVoicedFrames;
    return stats;
}

std::wstring FormatDbfs(double dbfs)
{
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%.1f dBFS", dbfs);
    return buffer;
}

std::wstring TempRootDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH)
        return std::wstring();
    std::wstring root(buffer, length);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    return root;
}

// %TEMP%\DynamicIslandRec\pending\<sessionFolderName>
std::wstring PendingDirectory(const std::wstring& sessionName)
{
    const std::wstring root = TempRootDirectory();
    if (root.empty() || sessionName.empty())
        return std::wstring();
    return root + L"\\DynamicIslandRec\\pending\\" + sessionName;
}

std::wstring BuildPendingChunkFileName(int index)
{
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"chunk_%03d.wav", index);
    return buffer;
}

bool WriteBinaryFile(const std::wstring& path, const std::string& bytes,
                     std::wstring* error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (error != nullptr)
            *error = L"无法保留音频片段（错误码 " + std::to_wstring(GetLastError()) +
                     L"）：" + path;
        return false;
    }
    size_t written = 0;
    bool ok = true;
    while (written < bytes.size())
    {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - written, 1u << 20));
        DWORD actual = 0;
        if (!WriteFile(file, bytes.data() + written, chunk, &actual, nullptr) ||
            actual == 0)
        {
            ok = false;
            if (error != nullptr)
                *error = L"写入音频片段失败（错误码 " +
                         std::to_wstring(GetLastError()) + L"）：" + path;
            break;
        }
        written += actual;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

struct ChunkFailure
{
    int index = 0;
    std::wstring reason;
    int attempts = 0;
    std::wstring pendingPath;
    unsigned long long bytes = 0;
    int durationSeconds = 0;
};

std::wstring BuildFailureNote(const std::wstring& sessionName,
                              const std::vector<ChunkFailure>& failures)
{
    std::wstring text;
    text += L"# 转写失败说明\r\n\r\n";
    text += L"以下片段包含语音，但多次重试后仍未能转写成功。它们的音频已保留在临时目录，" +
            std::wstring(L"不在本会话文件夹中，等待后续重新转写：\r\n\r\n");
    for (const ChunkFailure& failure : failures)
    {
        text += L"- 片段 " + std::to_wstring(failure.index) + L"：" +
                failure.pendingPath + L"（" + std::to_wstring(failure.bytes) +
                L" 字节，约 " + std::to_wstring(failure.durationSeconds) +
                L" 秒，已重试 " + std::to_wstring(failure.attempts) +
                L" 次，原因：" + failure.reason + L"）\r\n";
    }
    text += L"\r\n说明：\r\n";
    text += L"1. 已经转写成功的文字都已写入 转写.md，不会丢失；\r\n";
    text += L"2. 保留的音频位于 " +
            PendingDirectory(sessionName) +
            L"，请勿把该目录当作正式保存目录；\r\n";
    text += L"3. 请检查网络、API Key 与模型配置后重新录音；当前版本不会自动重新转写这些片段。\r\n\r\n";
    text += L"生成时间：" + NowText() + L"\r\n";
    return text;
}

} // namespace

// ===========================================================================
// Public entry point
// ===========================================================================
bool SummaryGenerate(const std::wstring& audioPath, const std::wstring& markdownPath,
                     const AiApiSettings& ai, std::wstring* transcriptOut,
                     std::wstring* errorOut, const std::wstring& transcriptPath,
                     const std::wstring& failureNotePath, SummaryResult* resultOut)
{
    if (transcriptOut != nullptr)
        transcriptOut->clear();
    if (errorOut != nullptr)
        errorOut->clear();
    if (resultOut != nullptr)
        *resultOut = SummaryResult();

    if (audioPath.empty())
    {
        SetErrorMessage(errorOut, L"录音文件路径为空");
        return false;
    }
    if (markdownPath.empty() && transcriptPath.empty() && failureNotePath.empty())
    {
        SetErrorMessage(errorOut, L"没有需要保存的文本输出（转写 / 纪要路径均为空）");
        return false;
    }
    AiApiSettings settings = ai;
    if (settings.sttModel.empty())
        settings.sttModel = L"whisper-1";
    if (settings.summaryModel.empty())
        settings.summaryModel = L"gpt-4o-mini";

    std::wstring baseUrl;
    if (!NormalizeBaseUrl(settings.baseUrl, &baseUrl, errorOut))
        return false;
    const std::wstring transcriptionUrl = baseUrl + L"/audio/transcriptions";
    const std::wstring chatUrl = baseUrl + L"/chat/completions";
    const std::string sttModelUtf8 = WideToUtf8(settings.sttModel);

    MediaFoundationSession mfSession;
    if (!mfSession.IsReady())
    {
        SetErrorMessage(errorOut, L"Media Foundation 初始化失败，无法解析录音文件");
        return false;
    }

    IMFSourceReader* reader = nullptr;
    InputAudioType inputType;
    std::wstring error;
    if (!OpenAudioReader(audioPath, &reader, &inputType, &error))
    {
        SetErrorMessage(errorOut, error);
        return false;
    }

    long long totalFrames = 0;
    std::vector<int16_t> chunk;
    chunk.reserve(static_cast<size_t>(kChunkSamples) + 4096);
    MonoResampler resampler(inputType.sampleRate, kTargetSampleRate);

    const std::wstring sessionName =
        FileNameOf(DirectoryOf(!transcriptPath.empty() ? transcriptPath : failureNotePath));
    std::wstring pendingDirectory = PendingDirectory(sessionName);
    std::wstring transcript; // successfully transcribed text (in memory)
    int chunkIndex = 0;
    int transcribedChunks = 0;
    int droppedSilentChunks = 0;
    std::wstring lastChunkError;
    std::vector<ChunkFailure> pendingFailures;

    // ---- streaming transcript file (lazy header + flush after every chunk) ----
    TranscriptFile transcriptFile;
    bool transcriptOpened = false;
    std::wstring transcriptError;
    std::wstring bufferedStatusLines;
    auto ensureTranscriptOpen = [&]() -> bool
    {
        if (transcriptPath.empty() || transcriptOpened)
            return true;
        if (!EnsureDirectory(DirectoryOf(transcriptPath), errorOut))
            return false;
        if (!transcriptFile.Open(transcriptPath, &transcriptError))
        {
            SetErrorMessage(errorOut, transcriptError);
            return false;
        }
        std::wstring header;
        header += L"# 转写稿\r\n\r\n";
        header += L"- 生成时间：" + NowText() + L"\r\n";
        header += L"- 转写模型：" + settings.sttModel + L"\r\n\r\n---\r\n\r\n";
        if (!transcriptFile.Write(header))
        {
            SetErrorMessage(errorOut, L"写入转写文件失败：" + transcriptPath);
            return false;
        }
        if (!bufferedStatusLines.empty())
            transcriptFile.Write(bufferedStatusLines);
        bufferedStatusLines.clear();
        transcriptOpened = true;
        return true;
    };

    auto transcribeChunkOnce = [&](const std::string& wavBody, std::string* textOut,
                                   std::wstring* reasonOut) -> bool
    {
        const std::string boundary = MakeBoundary();
        const std::string body = BuildTranscriptionBody(boundary, wavBody, sttModelUtf8);
        const std::string contentType = "multipart/form-data; boundary=" + boundary;
        HttpResponse response;
        std::wstring httpError;
        if (!HttpPost(transcriptionUrl, settings.apiKey, contentType, body, &response,
                      &httpError))
        {
            *reasonOut = httpError;
            return false;
        }
        if (response.status < 200 || response.status >= 300)
        {
            *reasonOut = HttpFailureMessage(L"转写请求失败", response);
            return false;
        }
        std::string piece;
        if (GetJsonStringField(response.body, "text", 0, &piece, nullptr))
        {
            *textOut = TrimAscii(piece);
        }
        else
        {
            const std::string trimmed = TrimAscii(response.body);
            if (!trimmed.empty() && trimmed[0] != '{' && trimmed[0] != '[')
                *textOut = trimmed;
            else
            {
                *reasonOut = L"转写响应解析失败：缺少 text 字段（HTTP " +
                             std::to_wstring(response.status) + L"）";
                return false;
            }
        }
        if (textOut->empty())
        {
            *reasonOut = L"转写响应为空";
            return false;
        }
        return true;
    };

    bool fatalWriteError = false;
    auto processChunk = [&]() -> void
    {
        if (chunk.empty())
            return;
        ++chunkIndex;
        const std::string wavBody = BuildWavFile(chunk);
        std::string pieceUtf8;
        std::wstring reason;
        bool ok = false;
        int attempts = 0;
        for (; attempts < 3; ++attempts)
        {
            if (attempts > 0)
                Sleep(attempts == 1 ? 500 : 1500);
            std::string attemptText;
            std::wstring attemptReason;
            if (transcribeChunkOnce(wavBody, &attemptText, &attemptReason) &&
                !TrimAscii(attemptText).empty())
            {
                pieceUtf8 = attemptText;
                ok = true;
                break;
            }
            reason = attemptReason;
            if (reason.empty())
                reason = L"转写响应为空";
        }

        if (ok)
        {
            ++transcribedChunks;
            const std::wstring piece = Utf8ToWide(TrimAscii(pieceUtf8)) + L"\n\n";
            transcript += piece;
            if (!ensureTranscriptOpen() ||
                (transcriptOpened && !transcriptFile.Write(piece)))
            {
                fatalWriteError = true;
            }
            chunk.clear();
            return;
        }

        // Failed after retries: decide whether the chunk contains speech.
        lastChunkError = reason;
        const SpeechStats stats = AnalyzeChunkSpeech(chunk);
        {
            std::wstring statusLine;
            if (stats.hasSpeech)
            {
                if (pendingDirectory.empty())
                    pendingDirectory = PendingDirectory(sessionName);
                std::wstring writeError;
                if (EnsureDirectory(pendingDirectory, &writeError))
                {
                    std::wstring name = BuildPendingChunkFileName(chunkIndex);
                    std::wstring path = pendingDirectory + L"\\" + name;
                    int suffix = 2;
                    while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        wchar_t buffer[64] = {};
                        swprintf_s(buffer, L"chunk_%03d_%d.wav", chunkIndex, suffix);
                        path = pendingDirectory + L"\\" + buffer;
                        ++suffix;
                    }
                    if (WriteBinaryFile(path, wavBody, &writeError))
                    {
                        ChunkFailure failure;
                        failure.index = chunkIndex;
                        failure.reason = reason;
                        failure.attempts = attempts;
                        failure.pendingPath = path;
                        failure.bytes = static_cast<unsigned long long>(wavBody.size());
                        failure.durationSeconds = static_cast<int>(
                            chunk.size() / static_cast<size_t>(kTargetSampleRate));
                        pendingFailures.push_back(failure);
                        statusLine = L"\r\n[片段 " + std::to_wstring(chunkIndex) +
                                     L" 转写失败：语音音频已保留在临时目录（" + path +
                                     L"）]\r\n";
                    }
                    else
                    {
                        statusLine = L"\r\n[片段 " + std::to_wstring(chunkIndex) +
                                     L" 转写失败，且无法保留音频：" + writeError + L"]\r\n";
                    }
                }
                else
                {
                    statusLine = L"\r\n[片段 " + std::to_wstring(chunkIndex) +
                                 L" 转写失败，且无法创建临时保留目录：" + writeError +
                                 L"]\r\n";
                }
            }
            else
            {
                ++droppedSilentChunks;
                statusLine = L"\r\n[片段 " + std::to_wstring(chunkIndex) +
                             L" 未识别到语音（最高 " +
                             FormatDbfs(stats.maxFrameDbfs) +
                             L"），已忽略]\r\n";
            }
            if (transcriptOpened)
            {
                if (!transcriptFile.Write(statusLine))
                    fatalWriteError = true;
            }
            else
            {
                bufferedStatusLines += statusLine;
            }
        }
        chunk.clear();
    };

    bool decodeFailed = false;
    for (;;)
    {
        DWORD streamFlags = 0;
        DWORD actualStream = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        const HRESULT hr = reader->ReadSample(kFirstAudioStream, 0, &actualStream,
                                              &streamFlags, &timestamp, &sample);
        if (FAILED(hr))
        {
            error = L"解码录音文件失败（Media Foundation 0x" +
                    Hex32(static_cast<unsigned int>(hr)) + L"）";
            decodeFailed = true;
            break;
        }
        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            if (sample != nullptr)
                sample->Release();
            break;
        }
        if ((streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
        {
            IMFMediaType* current = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType(kFirstAudioStream, &current)))
            {
                if (!ParseReaderType(current, &inputType, &error))
                {
                    current->Release();
                    decodeFailed = true;
                    if (sample != nullptr)
                        sample->Release();
                    break;
                }
                current->Release();
                resampler = MonoResampler(inputType.sampleRate, kTargetSampleRate);
            }
        }
        if (sample != nullptr)
        {
            const bool appended =
                AppendSample(sample, inputType, &resampler, &chunk, &totalFrames, &error);
            sample->Release();
            if (!appended)
            {
                decodeFailed = true;
                break;
            }
            if (static_cast<long long>(chunk.size()) >= kChunkSamples)
                processChunk();
            if (fatalWriteError)
                break;
        }
    }
    reader->Release();
    reader = nullptr;

    if (!fatalWriteError && static_cast<long long>(chunk.size()) >= kTargetSampleRate / 2)
        processChunk();

    const int durationSeconds =
        static_cast<int>((totalFrames + kTargetSampleRate / 2) / kTargetSampleRate);

    // ---- transcript footer (kept in the same file, after all chunk text) ----
    if (transcriptOpened)
    {
        std::wstring footer;
        footer += L"\r\n---\r\n\r\n";
        footer += L"- 录音时长：" + FormatDuration(durationSeconds) + L"\r\n";
        footer += L"- 分块数量：" + std::to_wstring(chunkIndex) + L"（成功 " +
                  std::to_wstring(transcribedChunks) + L"，保留 " +
                  std::to_wstring(static_cast<int>(pendingFailures.size())) +
                  L"，静音忽略 " + std::to_wstring(droppedSilentChunks) + L"）\r\n";
        footer += L"- 转写状态：" +
                  std::wstring(pendingFailures.empty() ? L"完整"
                                                       : L"部分完成（失败片段见 转写失败说明.txt）") +
                  L"\r\n";
        transcriptFile.Write(footer);
        transcriptFile.Close();
    }

    // ---- failure note for retained speech chunks ----
    bool failureNoteWritten = false;
    std::wstring failureSummary;
    if (!pendingFailures.empty())
    {
        for (const ChunkFailure& failure : pendingFailures)
        {
            if (!failureSummary.empty())
                failureSummary += L"；";
            failureSummary += L"片段 " + std::to_wstring(failure.index) + L"（" +
                              failure.pendingPath + L"）";
        }
        if (!failureNotePath.empty() &&
            EnsureDirectory(DirectoryOf(failureNotePath), nullptr))
        {
            failureNoteWritten =
                WriteTextFileUtf8(failureNotePath,
                                  WideToUtf8(BuildFailureNote(sessionName, pendingFailures)),
                                  nullptr);
        }
    }

    if (transcriptOut != nullptr)
        *transcriptOut = transcript;
    if (resultOut != nullptr)
    {
        resultOut->transcriptWritten = transcriptOpened;
        resultOut->transcribedChunks = transcribedChunks;
        resultOut->pendingChunks = static_cast<int>(pendingFailures.size());
        resultOut->droppedSilentChunks = droppedSilentChunks;
        resultOut->pendingDirectory = pendingFailures.empty() ? std::wstring()
                                                              : pendingDirectory;
        resultOut->failureSummary = failureSummary;
    }

    if (decodeFailed || fatalWriteError)
    {
        if (decodeFailed)
            SetErrorMessage(errorOut, error);
        else
            SetErrorMessage(errorOut, L"写入转写文本失败");
        return resultOut != nullptr && (resultOut->transcribedChunks > 0 ||
                                        resultOut->pendingChunks > 0);
    }

    // ---- summarize the (possibly partial) transcript ----
    bool markdownWritten = false;
    if (!markdownPath.empty() && !transcript.empty())
    {
        const std::string chatBody = BuildChatBody(WideToUtf8(settings.summaryModel),
                                                   WideToUtf8(transcript));
        HttpResponse chatResponse;
        std::wstring httpError;
        if (!HttpPost(chatUrl, settings.apiKey, "application/json; charset=utf-8",
                      chatBody, &chatResponse, &httpError))
        {
            SetErrorMessage(errorOut, httpError);
        }
        else if (chatResponse.status < 200 || chatResponse.status >= 300)
        {
            SetErrorMessage(errorOut, HttpFailureMessage(L"总结请求失败", chatResponse));
        }
        else
        {
            std::string contentUtf8;
            if (!ExtractChatContent(chatResponse.body, &contentUtf8) ||
                TrimAscii(contentUtf8).empty())
            {
                SetErrorMessage(errorOut,
                                L"总结响应解析失败：缺少 choices[0].message.content");
            }
            else
            {
                std::wstring markdown;
                markdown += L"# 会议纪要\r\n\r\n";
                markdown += L"- 生成时间：" + NowText() + L"\r\n";
                markdown += L"- 转写稿：转写.md\r\n";
                markdown += L"- 录音时长：" + FormatDuration(durationSeconds) + L"\r\n";
                markdown += L"- 转写模型：" + settings.sttModel + L"\r\n";
                markdown += L"- 总结模型：" + settings.summaryModel + L"\r\n";
                if (!pendingFailures.empty())
                    markdown += L"- 注意：部分片段转写失败，音频已保留在临时目录\r\n";
                markdown += L"\r\n---\r\n\r\n";
                markdown += Utf8ToWide(contentUtf8);
                if (markdown.empty() || markdown.back() != L'\n')
                    markdown += L"\r\n";
                if (EnsureDirectory(DirectoryOf(markdownPath), errorOut) &&
                    WriteTextFileUtf8(markdownPath, WideToUtf8(markdown), errorOut))
                {
                    markdownWritten = true;
                    errorOut->clear();
                }
            }
        }
    }
    if (resultOut != nullptr)
        resultOut->markdownWritten = markdownWritten;

    const bool producedAnyText = transcriptOpened || markdownWritten ||
                                 failureNoteWritten ||
                                 (resultOut != nullptr && resultOut->pendingChunks > 0);
    if (!producedAnyText)
    {
        std::wstring message = lastChunkError.empty()
                                   ? L"没有可用的转写文本（所有片段均转写失败或为静音）"
                                   : lastChunkError;
        if (droppedSilentChunks > 0)
            message += L"（未识别到可转写语音，未保留音频）";
        SetErrorMessage(errorOut, message);
        return false;
    }
    return true;
}
