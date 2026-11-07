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

void DiscoverWeaponReplacements(bool rerollStickyChoices);
void DiscoverWeaponTextures();

WeaponReplacementStats OrcGetWeaponReplacementStats();
WeaponTextureStats OrcGetWeaponTextureStats();

void OrcRestoreWeaponTextureOverrides();
/// Applies only entries queued while `OrcWeaponHeldTextureDeferBegin` active (held weapon clone path).
void OrcRestoreWeaponHeldTextureOverrides();

/// Route texture override recording to the held defer buffer until paired `OrcWeaponHeldTextureDeferEnd()` (fixes
/// premature restore inside `pedRenderEvent.after`, before GTA draws `m_pWeaponObject`).
void OrcWeaponHeldTextureDeferBegin();
void OrcWeaponHeldTextureDeferEnd();
void OrcWeaponAssetsShutdown();

std::string OrcGetWeaponModelBaseNameLower(int wt);

WeaponReplacementAsset* OrcResolveWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom);
WeaponReplacementAsset* OrcResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom);
std::string OrcResolveUsableWeaponReplacementKeyForPed(CPed* ped, int wt, bool allowRandom);
void OrcCollectWeaponReplacementVariantKeys(int wt, std::vector<std::string>& outKeys);
bool OrcPinWeaponReplacementChoiceForPed(CPed* ped, int wt, const std::string& replacementKey);

RwObject* OrcCloneWeaponReplacementObject(WeaponReplacementAsset& asset);

void OrcApplyWeaponTextureToRwObject(RwObject* object, WeaponTextureAsset* asset);
/// Applies vanilla TXD `*_remap` (only if weapon mesh is standard / not Guns replacement),
/// Guns/GunsNick TXD internal `*_remap` variants (only replacement mesh),
/// then same-name texture overlay from custom TXD (`customAsset`).
void OrcApplyWeaponTexturesCombined(CPed* ped,
    int wt,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement);
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
