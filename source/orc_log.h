#pragma once

// Лог в OrcOutFit.log рядом с INI. Уровни: только ошибки или полный trace.
enum class OrcLogLevel : int {
    Off = 0,
    Error = 1,
    Info = 2,
};

extern OrcLogLevel g_orcLogLevel;

void OrcLogSetIniPath(const char* iniPath);
// Читает [Features] DebugLogLevel (0/1/2); если ключа нет — legacy DebugLog=1 -> Info.
void OrcLogReloadFromIni(const char* iniPath);
const char* OrcLogGetPath();

void OrcLogError(const char* fmt, ...);
void OrcLogInfo(const char* fmt, ...);
