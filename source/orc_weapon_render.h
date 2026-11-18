#pragma once

#include "RenderWare.h"
#include "orc_weapon_render_policy.h"

#include <vector>

class CPed;
struct OrcHeldGunflash;
struct WeaponTextureAsset;

/// Normalizes and validates a detached body clone once. All body atomics use GTA's default callback so the
/// ordinary attachment scene never depends on weapon-model plugin ownership.
bool OrcPrepareWeaponObjectForBodyAttachment(RwObject*& object,
    int stockWeaponModelId,
    OrcWeaponBodyInstanceSource source,
    std::vector<RpMaterial*>& outPinnedMaterials);
/// Balance the exact material pointers pinned by stock body preparation.
void OrcReleaseWeaponBodyMaterialPins(std::vector<RpMaterial*>& pinnedMaterials);

/// Normalizes a held replacement to a clump without assigning `RenderWeaponCB` or clump model-info.
/// Existing atomic callbacks stay unchanged (normally `AtomicDefaultRenderCallBack`).
bool OrcNormalizeWeaponObjectToClumpForDefaultRender(RwObject*& object);

/// Handles a held draw with scoped textures. Pass only the requested mesh and flash for split batches.
bool OrcRenderWeaponObjectLikeVanillaWithTextures(CPed* ped,
    int weaponType,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement,
    bool applyTextureOverrides,
    OrcHeldGunflash* gunflash = nullptr,
    bool left = false,
    bool drawMesh = true);

/// Renders a prepared body clone through AtomicDefault while bypassing held interception.
/// The caller owns the attachment lighting and RenderWare states.
bool OrcRenderBodyWeaponObjectWithTextures(CPed* ped,
    int weaponType,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement,
    bool applyTextureOverrides);

/// Used by the Full held AtomicDefault detour to pass body atomics straight to its original trampoline.
bool OrcIsBodyAttachmentDrawActive();

/// Detours use this guard to bypass replacement matching while prepared custom atomics call original GTA callbacks.
bool OrcIsVanillaCustomWeaponDrawActive();
