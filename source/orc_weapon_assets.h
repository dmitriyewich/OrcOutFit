#pragma once

#include "orc_types.h"

#include "RenderWare.h"

#include <cstddef>
#include <string>
#include <vector>

class CPed;

struct WeaponReplacementAsset {
    std::string key;
    std::string weaponNameLower;
    std::string matchNameLower;
    std::string displayName;
    std::string dffPath;
    std::string txdPath;
    RwTexDictionary* txd = nullptr;
    RwObject* rwObject = nullptr;
    bool loadAttempted = false;
    bool muzzleSampled = false;
    bool muzzleFound = false;
    float muzzleX = 0.0f;
};

struct WeaponTextureAsset {
    std::string key;
    std::string weaponNameLower;
    std::string matchNameLower;
    std::string displayName;
    std::string txdPath;
    RwTexDictionary* txd = nullptr;
    bool loadAttempted = false;
};

void DiscoverWeaponReplacements(bool rerollStickyChoices);
void DiscoverWeaponTextures();

WeaponReplacementStats OrcGetWeaponReplacementStats();
WeaponTextureStats OrcGetWeaponTextureStats();

/// Capture before applying draw-local overrides; pass the result to restore only that draw's changes.
std::size_t OrcWeaponTextureRestorePoint();
void OrcRestoreWeaponTextureOverrides(std::size_t restorePoint = 0);
void OrcWeaponAssetsShutdown();

std::string OrcGetWeaponModelBaseNameLower(int wt);

WeaponReplacementAsset* OrcResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom);
std::string OrcResolveUsableWeaponReplacementKeyForPed(CPed* ped, int wt, bool allowRandom);
void OrcCollectWeaponReplacementVariantKeys(int wt, std::vector<std::string>& outKeys);
bool OrcPinWeaponReplacementChoiceForPed(CPed* ped, int wt, const std::string& replacementKey);

RwObject* OrcCloneWeaponReplacementObject(WeaponReplacementAsset& asset);

/// Applies vanilla TXD `*_remap` (only if weapon mesh is standard / not Guns replacement),
/// Guns/GunsNick TXD internal `*_remap` variants (only replacement mesh),
/// then same-name texture overlay from custom TXD (`customAsset`).
void OrcApplyWeaponTexturesCombined(CPed* ped,
    int wt,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement);
/// `nullptr` resolves the replacement automatically. An explicit empty key is a stock mesh.
WeaponTextureAsset* OrcResolveUsableWeaponTextureAssetForPed(CPed* ped,
    int wt,
    bool allowRandom,
    const std::string* replacementKeyHint = nullptr);

/// Borrow the chosen Guns/replacement icon for the current native HUD draw.
RwTexture* OrcWeaponHudResolveIconTexture(CPed* ped, int weaponType, unsigned int stockHash);

/// Random replacement roll for this ped/weapon pinned to vanilla (game model); suppresses stray "no mapping" logs.
bool OrcWeaponReplacementIsStickyVanillaChoice(CPed* ped, int wt);

size_t OrcWeaponAssetsDbgReplacementNickKeys();
size_t OrcWeaponAssetsDbgRandomReplacementSkinPools();
size_t OrcWeaponAssetsDbgRandomReplacementWeaponPools();
