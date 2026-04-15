#pragma once

namespace samp_bridge {

using ToggleCallback = void(*)();

// Tries to detect SA:MP and install command hook once.
// Safe to call every frame.
void Poll(const char* command, ToggleCallback onToggle);

bool IsSampPresent();
// Известная samp.dll (entry point из списка): чат, ники, SetCursorMode. Иначе — как одиночка.
bool IsSampBuildKnown();
bool IsCommandHookReady();
const char* GetVersionName();
bool GetPedNickname(const void* gtaPed, char* outName, int outNameLen, bool* isLocal);

// SA:MP CGame::SetCursorMode — как в MyAsiMod: UI → mode 3+true, иначе 0+false.
void SyncSampOverlayCursor(bool wantUiCursor);

} // namespace samp_bridge

