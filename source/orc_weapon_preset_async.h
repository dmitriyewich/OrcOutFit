#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "orc_ini_document.h"
#include "orc_types.h"

/// Собрать пресет скина из уже распарсенного INI (без RW/D3D/ImGui).
void OrcBuildWeaponSkinPresetFromIniDocument(
    const OrcIniDocument& doc,
    const std::vector<WeaponCfg>& baseCfg1,
    const std::vector<WeaponCfg>& baseCfg2,
    std::vector<WeaponCfg>& outW1,
    std::vector<WeaponCfg>& outW2,
    std::vector<HeldWeaponPoseCfg>& outH1,
    std::vector<HeldWeaponPoseCfg>& outH2);

struct OrcWeaponSkinPresetLoaded {
    uint64_t enqueueEpoch = 0;
    std::string skinKey;
    uint64_t writeTicks = 0;
    std::vector<WeaponCfg> w1;
    std::vector<WeaponCfg> w2;
    std::vector<HeldWeaponPoseCfg> h1;
    std::vector<HeldWeaponPoseCfg> h2;
};

uint64_t OrcWeaponSkinPresetGetInvalidateEpoch();

void OrcWeaponSkinPresetEnqueueLoad(
    std::string skinKeyLower,
    std::string iniPathUtf8,
    std::vector<WeaponCfg> baseW1,
    std::vector<WeaponCfg> baseW2);

/// Снять один готовый результат (главный поток). `false` — очередь пуста.
bool OrcWeaponSkinPresetTryPopCompleted(OrcWeaponSkinPresetLoaded& out);

void OrcWeaponSkinPresetInvalidateAsyncState();
void OrcWeaponSkinPresetAsyncShutdown();

/// Есть ожидающая загрузка или результат в очереди до `Drain` (анти-дубликат постановки).
bool OrcWeaponSkinPresetLoadInFlightForKey(const std::string& skinKeyLower);

/// Главный поток: переносит готовые пресеты в `g_weaponSkin*Ov*`.
void OrcWeaponSkinPresetDrainCompletedLoads();
