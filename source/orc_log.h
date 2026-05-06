#pragma once

// Лог в OrcOutFit.log рядом с INI. `[Features] DebugLogLevel` из INI: 0 = Off (в файл ничего не пишется,
// включая OrcLogError), 1 = только ошибки, 2 = полный trace (OrcLogInfo / OrcLogInfoThrottled).
// Устаревший `[Features] DebugLog=1` без ключа DebugLogLevel включает уровень Info.
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
// Rate-limited Info (slot 0..511); used for diagnostics without spamming OrcOutFit.log per frame.
void OrcLogInfoThrottled(int slot, unsigned intervalMs, const char* fmt, ...);
