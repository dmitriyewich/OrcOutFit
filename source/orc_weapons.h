#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Populated by LoadWeaponObject hook (weapon IDE objects, model id -> DFF name).
extern std::vector<int> g_weaponDatModelId;
extern std::vector<std::string> g_weaponDatIdeName;

void OrcWeaponsEnsureWeaponDatHookInstalled();
const char* OrcTryGetWeaponObjectDffNameByModelId(int modelId);
void OrcWeaponsMapLoadedModelIdToType(int wt, int modelId);
/// Build once after weapon.dat/IDE discovery. The live render path only reads this cache.
std::size_t OrcWeaponsRebuildRuntimeModelCache(const std::vector<int>& availableTypes,
    const std::vector<int>& primaryModelIds,
    const std::vector<int>& secondaryModelIds);
int OrcWeaponsFindCachedTypeByModelId(int modelId);
