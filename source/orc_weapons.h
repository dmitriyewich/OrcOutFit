#pragma once

#include <string>
#include <vector>

// Populated by LoadWeaponObject hook (weapon IDE objects, model id -> DFF name).
extern std::vector<int> g_weaponDatModelId;
extern std::vector<std::string> g_weaponDatIdeName;

void OrcWeaponsEnsureWeaponDatHookInstalled();
const char* OrcTryGetWeaponObjectDffNameByModelId(int modelId);
void OrcWeaponsMapLoadedModelIdToType(int wt, int modelId);
