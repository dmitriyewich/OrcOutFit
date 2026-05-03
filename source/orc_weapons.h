#pragma once

#include <string>
#include <vector>

// Populated by LoadWeaponObject hook (weapon.dat ground truth).
extern std::vector<int> g_weaponDatModelId;
extern std::vector<std::string> g_weaponDatIdeName;

void OrcWeaponsEnsureWeaponDatHookInstalled();
