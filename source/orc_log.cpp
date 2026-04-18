#include "orc_log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

OrcLogLevel g_orcLogLevel = OrcLogLevel::Off;

static char s_logPath[MAX_PATH] = {};

const char* OrcLogGetPath() {
    return s_logPath;
}

void OrcLogSetIniPath(const char* iniPath) {
    s_logPath[0] = 0;
    if (!iniPath || !iniPath[0]) return;
    const char* dot = strrchr(iniPath, '.');
    if (dot && _stricmp(dot, ".ini") == 0) {
        const size_t baseLen = static_cast<size_t>(dot - iniPath);
        if (baseLen + 4 < MAX_PATH) {
            std::memcpy(s_logPath, iniPath, baseLen);
            std::memcpy(s_logPath + baseLen, ".log", 5);
        }
    } else {
        _snprintf_s(s_logPath, _TRUNCATE, "%s.log", iniPath);
    }
}

void OrcLogReloadFromIni(const char* iniPath) {
    OrcLogSetIniPath(iniPath);
    if (!iniPath || !iniPath[0]) {
        g_orcLogLevel = OrcLogLevel::Off;
        return;
    }
    constexpr int kMissing = 99999;
    const int lvl = GetPrivateProfileIntA("Features", "DebugLogLevel", kMissing, iniPath);
    if (lvl != kMissing) {
        if (lvl <= 0) g_orcLogLevel = OrcLogLevel::Off;
        else if (lvl == 1) g_orcLogLevel = OrcLogLevel::Error;
        else g_orcLogLevel = OrcLogLevel::Info;
        return;
    }
    if (GetPrivateProfileIntA("Features", "DebugLog", 0, iniPath) != 0)
        g_orcLogLevel = OrcLogLevel::Info;
    else
        g_orcLogLevel = OrcLogLevel::Off;
}

static void OrcWriteLine(char tag, const char* fmt, va_list ap) {
    if (!s_logPath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, s_logPath, "a") != 0 || !f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u [%c] ",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
        tag);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
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
