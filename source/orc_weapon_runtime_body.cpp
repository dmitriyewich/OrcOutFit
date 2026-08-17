#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"
#include "eWeaponType.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_render.h"
#include "orc_types.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_metadata_cache.h"
#include "orc_weapon_render.h"
#include "orc_weapon_render_policy.h"
#include "orc_weapon_runtime.h"

using namespace plugin;

RenderedWeapon g_rendered[OrcWeaponSlotMax] = {};
std::unordered_map<int, PedWeaponCache> g_otherPedsRendered;

static int FindSlotByType(RenderedWeapon* arr, int wt, bool secondary) {
    for (int i = 0; i < OrcWeaponSlotMax; i++)
        if (arr[i].active && arr[i].weaponType == wt && arr[i].secondary == secondary) return i;
    return -1;
}
static int FindFree(RenderedWeapon* arr) {
    for (int i = 0; i < OrcWeaponSlotMax; i++) if (!arr[i].active) return i;
    return -1;
}

static RwObject* CreateStockWeaponObject(int modelId, RwMatrix& matrix) {
    auto* modelInfo = CModelInfo::GetModelInfo(modelId);
    if (!modelInfo || !modelInfo->m_pRwObject) {
        // Streaming can evict a model after an earlier successful request. Retry at a bounded cadence instead of
        // remembering the model id forever and making the body weapon disappear for the rest of the session.
        static std::unordered_map<int, DWORD> lastRequestMs;
        const DWORD nowMs = GetTickCount();
        const auto it = lastRequestMs.find(modelId);
        if (it == lastRequestMs.end() || static_cast<DWORD>(nowMs - it->second) >= 1000u) {
            CStreaming::RequestModel(modelId, 0);
            lastRequestMs[modelId] = nowMs;
        }
        return nullptr;
    }
    return modelInfo->CreateInstance(&matrix);
}

void OrcDestroyRenderedWeapon(RenderedWeapon& r) {
    OrcReleaseWeaponBodyMaterialPins(r.pinnedMaterials);
    if (!r.rwObject) {
        r = {};
        return;
    }
    OrcDestroyRwObjectInstance(r.rwObject);
    r = {};
}

void OrcWeaponClearLocalRendered() {
    for (int i = 0; i < OrcWeaponSlotMax; i++) OrcDestroyRenderedWeapon(g_rendered[i]);
}


void OrcWeaponClearOtherPedsRendered() {
    for (auto& kv : g_otherPedsRendered) {
        for (int i = 0; i < OrcWeaponSlotMax; i++) OrcDestroyRenderedWeapon(kv.second.weapons[i]);
    }
    g_otherPedsRendered.clear();
}

static bool CreateWeaponInstance(RenderedWeapon* arr, int wt, bool secondary, int slot, CPed* ped) {
    static_assert(OrcSelectWeaponCloneRenderContract(OrcWeaponCloneUsage::BodyAttachment) ==
        OrcWeaponCloneRenderContract::DedicatedBodyAttachment);
    if (wt <= 0) return false;
    if (secondary) {
        if (g_cfg2.empty() || wt >= (int)g_cfg2.size()) return false;
    } else {
        if (g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
    }
    const WeaponCfg& wc = secondary ? GetWeaponCfg2ForPed(ped, wt) : GetWeaponCfgForPed(ped, wt);
    if (!wc.enabled || wc.boneId == 0) return false;
    if (FindSlotByType(arr, wt, secondary) >= 0) return true;

    const int mid = wt < static_cast<int>(g_weaponModelId.size()) ? g_weaponModelId[wt] : 0;
    if (mid <= 0) return false;

    RwMatrix* bone = OrcGetBoneMatrix(ped, wc.boneId);
    if (!bone) return false;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));

    const int freeIndex = FindFree(arr);
    if (freeIndex < 0) {
        OrcLogError("CreateWeaponInstance: no free slot (weapon type %d)", wt);
        return false;
    }

    std::string selectedReplacementKey;
    std::vector<RpMaterial*> pinnedMaterials;
    RwObject* replacement = nullptr;
    bool replacementPrepared = false;
    if (g_weaponReplacementEnabled && g_weaponReplacementOnBody) {
        if (WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, wt, true)) {
            selectedReplacementKey = asset->key;
            replacement = OrcCloneWeaponReplacementObject(*asset);
            replacementPrepared = replacement &&
                OrcPrepareWeaponObjectForBodyAttachment(
                    replacement,
                    mid,
                    OrcWeaponBodyInstanceSource::Replacement,
                    pinnedMaterials);
        }
    }

    const OrcWeaponBodyInstanceSource source = OrcSelectWeaponBodyInstanceSource(
        !selectedReplacementKey.empty(), replacementPrepared);
    RwObject* instance = nullptr;
    bool usesReplacementMesh = false;
    if (source == OrcWeaponBodyInstanceSource::Replacement) {
        instance = replacement;
        usesReplacementMesh = true;
    } else {
        OrcDestroyRwObjectInstance(replacement);
        if (source == OrcWeaponBodyInstanceSource::StockFallback) {
            OrcLogInfoThrottled(923,
                5000u,
                "weapon body: replacement rejected, stock fallback wt=%d model=%d key=%s",
                wt,
                mid,
                selectedReplacementKey.c_str());
        }
        instance = CreateStockWeaponObject(mid, mtx);
        if (!instance)
            return false;
        const bool preparationSucceeded =
            OrcPrepareWeaponObjectForBodyAttachment(instance, mid, source, pinnedMaterials);
        if (!preparationSucceeded) {
            OrcDestroyRwObjectInstance(instance);
            return false;
        }
    }

    arr[freeIndex] = {
        true,
        wt,
        secondary,
        mid,
        slot,
        instance,
        usesReplacementMesh,
        selectedReplacementKey,
        std::move(pinnedMaterials),
    };
    const char* sourceName = source == OrcWeaponBodyInstanceSource::Replacement
        ? "orcReplacement"
        : (source == OrcWeaponBodyInstanceSource::StockFallback ? "stockFallback" : "stock");
    OrcLogInfo("weapon body instance: pedRef=%d wt=%d model=%d source=%s stockClone=%s key=%s object=%p",
        ped ? CPools::GetPedRef(ped) : 0,
        wt,
        mid,
        sourceName,
        usesReplacementMesh ? "-" : "direct",
        selectedReplacementKey.empty() ? "-" : selectedReplacementKey.c_str(),
        instance);
    return true;
}

static bool RenderOneWeapon(CPed* ped, RenderedWeapon& r) {
    if (!r.rwObject || r.rwObject->type != rpCLUMP) return false;

    const WeaponCfg& wc = r.secondary ? GetWeaponCfg2ForPed(ped, r.weaponType) : GetWeaponCfgForPed(ped, r.weaponType);
    RwMatrix* bone = OrcGetBoneMatrix(ped, wc.boneId);
    if (!bone) return false;

    RwFrame* frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(r.rwObject));
    if (!frame) return false;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    OrcApplyAttachmentOffset(&mtx, wc.x, wc.y, wc.z);
    OrcRotateAttachmentMatrix(&mtx, wc.rx, wc.ry, wc.rz);

    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    if (wc.scale != 1.0f) {
        RwV3d s = { wc.scale, wc.scale, wc.scale };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    // Match the working GitHub renderer's ordinary attachment lighting. This is intentionally evaluated at the
    // attached weapon position, not inherited from Arizona/SilentPatch's held-weapon batch.
    const CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    OrcApplyAttachmentLightingForPed(ped, lightPos);

    WeaponTextureAsset* textureAsset = g_weaponTexturesEnabled
        ? OrcResolveUsableWeaponTextureAssetForPed(ped,
              r.weaponType,
              true,
              r.usesReplacementMesh ? &r.replacementKey : nullptr)
        : nullptr;
    return OrcRenderBodyWeaponObjectWithTextures(
        ped, r.weaponType, r.rwObject, textureAsset, r.usesReplacementMesh, true);
}
void OrcSyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress) {
    if (!ped) return;
    unsigned char curSlot = ped->m_nSelectedWepSlot;
    int curType = 0;
    if (curSlot < 13) curType = (int)ped->m_aWeapons[curSlot].m_eWeaponType;
    const int heldVisWt = (g_weaponReplacementEnabled && g_weaponReplacementInHands)
        ? OrcResolveActiveHeldWeaponTypeForBodySuppression(ped)
        : 0;
    if (g_cfg.empty()) return;
    const int maxWt = (int)g_cfg.size();
    // `drawingEvent` is a single game-thread path. Reuse these bounded scratch buffers instead of allocating twice
    // for the local ped and for every nearby remote ped on every frame.
    static std::vector<char> want;
    static std::vector<char> want2;
    want.assign(maxWt, 0);
    want2.assign(maxWt, 0);
    for (int s = 0; s < 13; s++) {
        auto& w = ped->m_aWeapons[s];
        int wt = (int)w.m_eWeaponType;
        if (wt <= 0 || wt >= maxWt) continue;
        if (wt >= static_cast<int>(g_weaponModelId.size()) || g_weaponModelId[wt] <= 0) continue;
        if (suppress && wt < (int)suppress->size() && (*suppress)[wt]) continue;
        if (wt == curType) continue;
        if (heldVisWt > 0 && wt == heldVisWt) continue;
        const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
        if (!wc.enabled || wc.boneId == 0) continue;
        const bool needsAmmo = s >= 2 && s <= 9;
        if (needsAmmo && w.m_nAmmoTotal == 0) continue;
        want[wt] = true;

        if (g_considerWeaponSkills) {
            const char skill = ped->GetWeaponSkill(static_cast<eWeaponType>(wt));
            if (skill == WEAPSKILL_PRO && OrcWeaponTypeCanUseVanillaInfoContract(
                    wt, static_cast<int>(WEAPONTYPE_PARACHUTE))) {
                CWeaponInfo* twinInfo = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 2);
                if (twinInfo && twinInfo->m_nFlags.bTwinPistol) {
                    const WeaponCfg& wc2 = GetWeaponCfg2ForPed(ped, wt);
                    if (wc2.enabled && wc2.boneId != 0) want2[wt] = true;
                }
            }
        }
    }
    for (int i = 0; i < OrcWeaponSlotMax; i++) {
        if (!arr[i].active) continue;
        int wt = arr[i].weaponType;
        bool keep = (wt >= 0 && wt < maxWt) && (arr[i].secondary ? want2[wt] : want[wt]);
        if (keep) {
            const std::string desiredReplacementKey =
                (g_weaponReplacementEnabled && g_weaponReplacementOnBody)
                ? OrcResolveUsableWeaponReplacementKeyForPed(ped, wt, true)
                : std::string{};
            if (desiredReplacementKey != arr[i].replacementKey)
                keep = false;
        }
        if (!keep) OrcDestroyRenderedWeapon(arr[i]);
    }
    for (int wt = 1; wt < maxWt; wt++) if (want[wt])  CreateWeaponInstance(arr, wt, false, 0, ped);
    for (int wt = 1; wt < maxWt; wt++) if (want2[wt]) CreateWeaponInstance(arr, wt, true,  0, ped);

}

bool OrcWeaponHasCachedBodyWeapons() {
    for (const RenderedWeapon& weapon : g_rendered) {
        if (weapon.active && weapon.rwObject)
            return true;
    }
    if (!g_renderAllPedsWeapons)
        return false;
    for (const auto& entry : g_otherPedsRendered) {
        for (const RenderedWeapon& weapon : entry.second.weapons) {
            if (weapon.active && weapon.rwObject)
                return true;
        }
    }
    return false;
}

int OrcRenderPedWeapons(CPed* ped, RenderedWeapon* arr) {
    int rendered = 0;
    for (int i = 0; i < OrcWeaponSlotMax; i++) {
        if (!arr[i].active) continue;
        __try {
            if (RenderOneWeapon(ped, arr[i]))
                ++rendered;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static DWORD lastWeaponSehLogMs = 0u;
            const DWORD nowMs = GetTickCount();
            if (lastWeaponSehLogMs == 0u ||
                static_cast<DWORD>(nowMs - lastWeaponSehLogMs) >= 10000u) {
                lastWeaponSehLogMs = nowMs;
                OrcLogError(
                    "weapon body attachment: instance SEH ex=0x%08X pedRef=%d wt=%d model=%d; remaining instances continue",
                    GetExceptionCode(),
                    OrcSafeGetPedRef(ped),
                    arr[i].weaponType,
                    arr[i].modelId);
            }
        }
    }
    return rendered;
}

static int RenderPedWeaponsForAttachmentScene(CPed* ped, RenderedWeapon* arr) {
    bool hasActiveWeapon = false;
    for (int i = 0; i < OrcWeaponSlotMax; ++i) {
        if (arr[i].active && arr[i].rwObject) {
            hasActiveWeapon = true;
            break;
        }
    }
    if (!hasActiveWeapon)
        return 0;

    bool setupSucceeded = false;
    int rendered = 0;
    __try {
        setupSucceeded = OrcTryPedSetupLighting(ped);
        if (g_orcLogLevel >= OrcLogLevel::Info) {
            static bool tracedStockLighting = false;
            if (!tracedStockLighting) {
                for (int i = 0; i < OrcWeaponSlotMax; ++i) {
                    const RenderedWeapon& weapon = arr[i];
                    if (!weapon.active || !weapon.rwObject || weapon.usesReplacementMesh) {
                        continue;
                    }
                    OrcLogInfo(
                        "weapon body attachment lighting: pedRef=%d wt=%d model=%d setupSucceeded=%d pointLights=perWeapon",
                        ped ? CPools::GetPedRef(ped) : 0,
                        weapon.weaponType,
                        weapon.modelId,
                        setupSucceeded ? 1 : 0);
                    tracedStockLighting = true;
                    break;
                }
            }
        }
        rendered = OrcRenderPedWeapons(ped, arr);
    } __finally {
        // OrcApplyAttachmentLightingForPed activates directional/point lighting even when SetupLighting returns
        // false, so cleanup is required for both results. The exact vslot-20 ABI receives the original bool.
        OrcTryPedRemoveLighting(ped, setupSucceeded);
    }
    return rendered;
}

static int TryRenderPedWeaponsForAttachmentScene(CPed* ped, RenderedWeapon* arr) {
    __try {
        return RenderPedWeaponsForAttachmentScene(ped, arr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static DWORD lastOwnerSehLogMs = 0u;
        const DWORD nowMs = GetTickCount();
        if (lastOwnerSehLogMs == 0u ||
            static_cast<DWORD>(nowMs - lastOwnerSehLogMs) >= 10000u) {
            lastOwnerSehLogMs = nowMs;
            OrcLogError(
                "weapon body attachment: owner SEH ex=0x%08X pedRef=%d; other owners continue",
                GetExceptionCode(),
                OrcSafeGetPedRef(ped));
        }
        return 0;
    }
}

int OrcRenderCachedBodyWeaponsForAttachmentScene(CPlayerPed* player) {
    if (OrcIsRuntimeShuttingDown() || !g_enabled || !player)
        return 0;
    int rendered = TryRenderPedWeaponsForAttachmentScene(player, g_rendered);

    if (!g_renderAllPedsWeapons)
        return rendered;
    for (auto& entry : g_otherPedsRendered) {
        CPed* ped = OrcSafeGetPed(entry.first);
        if (ped)
            rendered += TryRenderPedWeaponsForAttachmentScene(ped, entry.second.weapons.data());
    }
    return rendered;
}
