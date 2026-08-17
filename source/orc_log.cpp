#include "orc_log.h"

#include "orc_ini_document.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cwchar>
#include <mutex>
#include <vector>

OrcLogLevel g_orcLogLevel = OrcLogLevel::Off;

static char s_logPath[MAX_PATH] = {};
static wchar_t s_logPathWide[MAX_PATH] = {};
static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static bool s_sessionResetComplete = false;
static std::mutex s_logMutex;

namespace {

constexpr DWORD kLogShareMode = FILE_SHARE_READ;

void OrcCloseLogFileLocked() {
    if (s_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(s_logFile);
        s_logFile = INVALID_HANDLE_VALUE;
    }
}

bool OrcOpenLogFileLocked(bool reset) {
    if (!s_logPathWide[0])
        return false;

    HANDLE file = CreateFileW(
        s_logPathWide,
        FILE_APPEND_DATA,
        kLogShareMode,
        nullptr,
        reset ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    s_logFile = file;
    if (reset)
        s_sessionResetComplete = true;
    return true;
}

bool OrcEnsureLogFileOpenLocked() {
    return s_logFile != INVALID_HANDLE_VALUE || OrcOpenLogFileLocked(!s_sessionResetComplete);
}

bool OrcWriteBytesLocked(const char* bytes, DWORD byteCount, bool allowRetry) {
    if (!bytes || byteCount == 0 || !OrcEnsureLogFileOpenLocked())
        return false;

    DWORD bytesWritten = 0;
    const BOOL writeSucceeded = WriteFile(s_logFile, bytes, byteCount, &bytesWritten, nullptr);
    if (writeSucceeded && bytesWritten == byteCount)
        return true;

    OrcCloseLogFileLocked();
    if (!allowRetry || bytesWritten != 0)
        return false;

    return OrcOpenLogFileLocked(!s_sessionResetComplete)
        && OrcWriteBytesLocked(bytes, byteCount, false);
}

bool OrcBuildLogPath(const char* iniPath, char (&path)[MAX_PATH]) {
    path[0] = 0;
    if (!iniPath || !iniPath[0])
        return false;

    const char* dot = strrchr(iniPath, '.');
    if (dot && _stricmp(dot, ".ini") == 0) {
        const size_t baseLen = static_cast<size_t>(dot - iniPath);
        if (baseLen + sizeof(".log") > MAX_PATH)
            return false;
        std::memcpy(path, iniPath, baseLen);
        std::memcpy(path + baseLen, ".log", sizeof(".log"));
        return true;
    }

    return _snprintf_s(path, _TRUNCATE, "%s.log", iniPath) >= 0;
}

bool OrcBuildWidePath(const char* path, wchar_t (&widePath)[MAX_PATH]) {
    widePath[0] = L'\0';
    if (!path || !path[0])
        return false;
    return MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, MAX_PATH) > 0;
}

} // namespace

const char* OrcLogGetPath() {
    return s_logPath;
}

void OrcLogSetIniPath(const char* iniPath) {
    char logPath[MAX_PATH]{};
    wchar_t logPathWide[MAX_PATH]{};
    const bool pathValid = OrcBuildLogPath(iniPath, logPath)
        && OrcBuildWidePath(logPath, logPathWide);

    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!pathValid) {
        OrcCloseLogFileLocked();
        s_logPath[0] = 0;
        s_logPathWide[0] = L'\0';
        s_sessionResetComplete = false;
        return;
    }

    if (std::strcmp(s_logPath, logPath) != 0 || std::wcscmp(s_logPathWide, logPathWide) != 0) {
        OrcCloseLogFileLocked();
        std::memcpy(s_logPath, logPath, sizeof(s_logPath));
        std::memcpy(s_logPathWide, logPathWide, sizeof(s_logPathWide));
        s_sessionResetComplete = false;
    }

    if (!OrcEnsureLogFileOpenLocked()) {
        char message[160]{};
        _snprintf_s(
            message,
            _TRUNCATE,
            "[OrcOutFit] failed to reset/open log: win32=%lu\n",
            static_cast<unsigned long>(GetLastError()));
        OutputDebugStringA(message);
    }
}

static void OrcLogApplyLevelFromFeaturesDoc(const OrcIniDocument& doc) {
    constexpr int kMissing = 99999;
    const int lvl = doc.GetInt("Features", "DebugLogLevel", kMissing);
    if (lvl != kMissing) {
        if (lvl <= 0) g_orcLogLevel = OrcLogLevel::Off;
        else if (lvl == 1) g_orcLogLevel = OrcLogLevel::Error;
        else g_orcLogLevel = OrcLogLevel::Info;
        return;
    }
    if (doc.GetInt("Features", "DebugLog", 0) != 0)
        g_orcLogLevel = OrcLogLevel::Info;
    else
        g_orcLogLevel = OrcLogLevel::Off;
}

void OrcLogReloadFromIni(const char* iniPath) {
    OrcLogSetIniPath(iniPath);
    if (!iniPath || !iniPath[0]) {
        g_orcLogLevel = OrcLogLevel::Off;
        return;
    }
    OrcIniDocument doc;
    (void)doc.LoadFromFile(iniPath);
    OrcLogApplyLevelFromFeaturesDoc(doc);
}

void OrcLogReloadFromIniDocument(const char* iniPathForLog, const OrcIniDocument& doc) {
    OrcLogSetIniPath(iniPathForLog);
    if (!iniPathForLog || !iniPathForLog[0]) {
        g_orcLogLevel = OrcLogLevel::Off;
        return;
    }
    if (!doc.IsLoaded()) {
        OrcLogReloadFromIni(iniPathForLog);
        return;
    }
    OrcLogApplyLevelFromFeaturesDoc(doc);
}

static void OrcWriteLine(char tag, const char* fmt, va_list ap) {
    if (!fmt)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char stackLine[4096]{};
    const int prefixLength = _snprintf_s(stackLine, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u [%c] ",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
        tag);
    if (prefixLength <= 0)
        return;

    constexpr size_t kLineEndingLength = 2;
    const size_t messageCapacity = sizeof(stackLine) - static_cast<size_t>(prefixLength) - kLineEndingLength;
    va_list formatArgs;
    va_copy(formatArgs, ap);
    const int stackMessageLength = _vsnprintf_s(
        stackLine + prefixLength,
        messageCapacity,
        _TRUNCATE,
        fmt,
        formatArgs);
    va_end(formatArgs);

    const char* line = stackLine;
    size_t lineLength = 0;
    std::vector<char> dynamicLine;
    if (stackMessageLength >= 0) {
        lineLength = static_cast<size_t>(prefixLength) + static_cast<size_t>(stackMessageLength);
        stackLine[lineLength++] = '\r';
        stackLine[lineLength++] = '\n';
    } else {
        va_list measureArgs;
        va_copy(measureArgs, ap);
        const int exactMessageLength = _vscprintf(fmt, measureArgs);
        va_end(measureArgs);
        if (exactMessageLength < 0)
            return;

        lineLength = static_cast<size_t>(prefixLength) + static_cast<size_t>(exactMessageLength);
        if (lineLength > MAXDWORD - kLineEndingLength)
            return;
        dynamicLine.resize(lineLength + kLineEndingLength);
        std::memcpy(dynamicLine.data(), stackLine, static_cast<size_t>(prefixLength));

        va_list dynamicArgs;
        va_copy(dynamicArgs, ap);
        const int formattedLength = vsnprintf(
            dynamicLine.data() + prefixLength,
            static_cast<size_t>(exactMessageLength) + 1,
            fmt,
            dynamicArgs);
        va_end(dynamicArgs);
        if (formattedLength != exactMessageLength)
            return;

        dynamicLine[lineLength++] = '\r';
        dynamicLine[lineLength++] = '\n';
        line = dynamicLine.data();
    }

    std::lock_guard<std::mutex> lock(s_logMutex);
    (void)OrcWriteBytesLocked(line, static_cast<DWORD>(lineLength), true);
}

void OrcLogError(const char* fmt, ...) {
    if (g_orcLogLevel < OrcLogLevel::Error || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    OrcWriteLine('E', fmt, ap);
    va_end(ap);
}

void OrcLogInfo(const char* fmt, ...) {
    if (g_orcLogLevel < OrcLogLevel::Info || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    OrcWriteLine('I', fmt, ap);
    va_end(ap);
}

void OrcLogInfoThrottled(int slot, unsigned intervalMs, const char* fmt, ...) {
    if (g_orcLogLevel < OrcLogLevel::Info || !fmt) return;
    // Numeric slot tags are used across modules (throttle buckets). Must stay within range.
    enum { kMaxSlots = 1024 };
    if (slot < 0 || slot >= kMaxSlots)
        slot = 0;
    static DWORD s_lastTick[kMaxSlots] = {};
    static unsigned char s_seen[kMaxSlots] = {};
    const DWORD now = GetTickCount();
    const DWORD elapsed =
        (s_seen[slot] && now >= s_lastTick[slot]) ? (now - s_lastTick[slot]) : intervalMs;
    if (s_seen[slot] && elapsed < intervalMs)
        return;
    s_seen[slot] = 1;
    s_lastTick[slot] = now;
    va_list ap;
    va_start(ap, fmt);
    OrcWriteLine('I', fmt, ap);
    va_end(ap);
}
