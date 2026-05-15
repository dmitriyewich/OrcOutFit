#pragma once

#include <cstdint>

struct CVector;

/// Вызывать из `DllMain` после `g_module = module` (до логов опционально).
void OrcWeaponAudioSetPluginModule(void* module);

/// MinHook на `CAEWeaponAudioEntity::WeaponFire` / `WeaponReload` и `CAEPedAudioEntity::HandlePedHit` (1.0 US).
void OrcWeaponAudioEnsureHooksInstalled();

/// Контекст OpenAL + слушатель; пауза при меню.
void OrcWeaponAudioShutdown();

/// Каждый кадр из `gameProcessEvent` (главный поток игры).
void OrcWeaponAudioOnGameProcess();

/// Сброс negative cache путей WAV (после rescan замены оружия).
void OrcWeaponAudioInvalidateCaches();

/// Остановить loop-источники OpenAL для ped (смена оружия / unload).
void OrcWeaponAudioLoopsStopForPed(CPed* ped);
