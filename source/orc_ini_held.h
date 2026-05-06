#pragma once

#include "orc_ini_document.h"
#include "orc_types.h"

/// Секция пресета оружия (`Weapons\*.ini`): есть ли непустые Held*-ключи (как WinAPI `GetPrivateProfileString`).
bool OrcIniSectionHasAnyHeldKey(const OrcIniDocument& doc, const char* section);

/// Есть ли смещения/поворот/scale в руках без явного `HeldEnabled`.
bool OrcIniSectionHasHeldTweakKey(const OrcIniDocument& doc, const char* section);

/// Читает Held-позу из уже загруженного INI (семантика как `ReadHeldSectionFromIni` в main до этапа 5).
void OrcReadHeldWeaponSectionFromIni(HeldWeaponPoseCfg& h, const OrcIniDocument& doc, const char* section);
