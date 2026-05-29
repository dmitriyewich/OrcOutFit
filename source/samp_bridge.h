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
/// Локальный игрок SA:MP, в т.ч. второй `CPed*` при `Fire` (когда `idFind` не находит пулевой id).
bool IsLocalPlayerGtaPed(const void* gtaPed);

// SA:MP CGame::SetCursorMode — как в MyAsiMod: UI → mode 2 (lock cam+control)+true, иначе 0+false.
void SyncSampOverlayCursor(bool wantUiCursor);

// SA:MP UI state (только для известных сборок). Для неизвестных сборок / одиночки → false.
bool IsSampChatOpen();
bool IsSampDialogActive();

// Disable hooks and release MinHook state (safe to call multiple times).
void Shutdown();

} // namespace samp_bridge

