#pragma once

#include "orc_types.h"

#include "RenderWare.h"

#include <cstddef>
#include <string>

class CPed;

struct WeaponReplacementAsset {
    std::string key;
    std::string weaponNameLower;
    std::string matchNameLower;
    std::string displayName;
    std::string dffPath;
    std::string txdPath;
    int txdSlot = -1;
    RwObject* rwObject = nullptr;
    bool loadAttempted = false;
    bool loadFailedLogged = false;
};

struct WeaponTextureAsset {
    std::string key;
    std::string weaponNameLower;
    std::string matchNameLower;
    std::string displayName;
    std::string txdPath;
    int txdSlot = -1;
    bool loadAttempted = false;
    bool loadFailedLogged = false;
};

void DiscoverWeaponReplacements();
void DiscoverWeaponTextures();

WeaponReplacementStats OrcGetWeaponReplacementStats();
WeaponTextureStats OrcGetWeaponTextureStats();

void OrcRestoreWeaponTextureOverrides();
void OrcWeaponAssetsShutdown();

std::string OrcGetWeaponModelBaseNameLower(int wt);

WeaponReplacementAsset* OrcResolveWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom);
WeaponReplacementAsset* OrcResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom);
std::string OrcResolveUsableWeaponReplacementKeyForPed(CPed* ped, int wt, bool allowRandom);

RwObject* OrcCloneWeaponReplacementObject(WeaponReplacementAsset& asset);

void OrcApplyWeaponTextureToRwObject(RwObject* object, WeaponTextureAsset* asset);
WeaponTextureAsset* OrcResolveUsableWeaponTextureAssetForPed(CPed* ped, int wt, bool allowRandom);

size_t OrcWeaponAssetsDbgReplacementSkinKeys();
size_t OrcWeaponAssetsDbgReplacementNickKeys();
size_t OrcWeaponAssetsDbgRandomReplacementPools();
