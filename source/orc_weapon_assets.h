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

/// Local player HUD: if Guns/replacement dictionary has `<weapon>icon` (e.g. desert_eagleicon), use that TXD as current during `DrawWeaponIcon`.
bool OrcWeaponHudTryGetIconOverrideTxdSlot(CPed* ped, int* outTxdSlot);
/// Resolved `CTxdStore` slot → `RwTexDictionary*` (streaming pool). For HUD sprite override via `FindNamedTexture`.
RwTexDictionary* OrcWeaponStreamingGetRwTxdDictionary(int txdSlot);
/// During `DrawWeaponIcon` only: if Guns TXD has the icon under the replacement/texture basename (`desert_eagleicon`) but
/// not under the vanilla `weapon.dat` name (`colt45icon`), map requested sprite `from` → `to` inside the Orc dictionary.
const char* OrcWeaponHudGetIconSpriteRemapFrom();
const char* OrcWeaponHudGetIconSpriteRemapTo();
/// When the game draws the weapon slot without `CHud::DrawWeaponIcon`, refresh once per `drawingEvent` (+ lazy from
/// `SetTexture` if still null) so `CSprite2d::SetTexture` can resolve `*icon` from the Orc TXD.
void OrcWeaponHudRefreshSampSpriteInterceptCache();
RwTexDictionary* OrcWeaponHudGetSampSpriteInterceptDict();
/// `RwTexDictionaryFindNamedTexture`, then scan with `_stricmp` on texture names (TXD Workshop often changes case).
RwTexture* OrcWeaponHudResolveSpriteTexture(RwTexDictionary* dict, const char* name);
/// Same suffix rules as HUD icon matching: ends with `icon` case-insensitive, excludes SA:MP skipicon etc.
bool OrcWeaponHudSpriteNamePassesSetTextureConvention(const char* name);

/// Used from global `RwTexDictionaryFindNamedTexture` hook (`orc_texture_remap.cpp`): for `hud.txd` lookups matching
/// the current local HUD weapon icon basename + "icon", return the Guns/replacement texture so clients that bypass
/// `CSprite2d::SetTexture` still draw custom art.
RwTexture* OrcWeaponHudTryRwTexDictionaryFindOverride(RwTexDictionary* dict, const char* name, RwTexture* foundInDict);

/// Random replacement roll for this ped/weapon pinned to vanilla (game model); suppresses stray "no mapping" logs.
bool OrcWeaponReplacementIsStickyVanillaChoice(CPed* ped, int wt);

size_t OrcWeaponAssetsDbgReplacementNickKeys();
size_t OrcWeaponAssetsDbgRandomReplacementSkinPools();
size_t OrcWeaponAssetsDbgRandomReplacementWeaponPools();
