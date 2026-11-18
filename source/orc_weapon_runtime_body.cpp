#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPointLights.h"
#include "CPools.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "RenderWare.h"
#include "eWeaponType.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_log.h"
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

static int FindFree(RenderedWeapon* arr) {
    for (int i = 0; i < OrcWeaponSlotMax; i++) if (!arr[i].active) return i;
    return -1;
}

static RwObject* CreateStockWeaponObject(int modelId, RwMatrix& matrix) {
    auto* modelInfo = CModelInfo::GetModelInfo(modelId);
    if (!modelInfo)
        return nullptr;
    if (!modelInfo->m_pRwObject) {
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

static void LogWeaponInstance(CPed* ped, const RenderedWeapon& r, OrcWeaponBodyInstanceSource source) {
    if (g_orcLogLevel < OrcLogLevel::Info)
        return;
    const char* sourceName = source == OrcWeaponBodyInstanceSource::Replacement
        ? "orcReplacement"
        : (source == OrcWeaponBodyInstanceSource::StockFallback ? "stockFallback" : "stock");
    OrcLogInfo("weapon body instance: pedRef=%d wt=%d model=%d source=%s stockClone=%s key=%s object=%p",
        CPools::GetPedRef(ped),
        r.weaponType,
        r.modelId,
        sourceName,
        r.usesReplacementMesh ? "-" : "direct",
        r.replacementKey.empty() ? "-" : r.replacementKey.c_str(),
        r.rwObject);
}

static void TryCreateStockInstance(CPed* ped, RenderedWeapon& r, int boneId) {
    const DWORD now = GetTickCount();
    if (r.nextStockRetryMs != 0 && static_cast<LONG>(now - r.nextStockRetryMs) < 0)
        return;
    r.nextStockRetryMs = now + 1000u;
    const RwMatrix* bone = OrcGetBoneMatrix(ped, boneId);
    if (!bone)
        return;

    RwMatrix matrix = *bone;
    const auto source = OrcSelectWeaponBodyInstanceSource(!r.replacementKey.empty(), false);
    RwObject* object = CreateStockWeaponObject(r.modelId, matrix);
    if (!object)
        return;
    __try {
        if (!OrcPrepareWeaponObjectForBodyAttachment(object, r.modelId, source, r.pinnedMaterials))
            return;
        r.rwObject = object;
    } __finally {
        if (!r.rwObject)
            OrcDestroyRwObjectInstance(object);
    }
    LogWeaponInstance(ped, r, source);
}

static bool TryCreateReplacementInstance(RenderedWeapon& r, WeaponReplacementAsset& asset) {
    RwObject* object = OrcCloneWeaponReplacementObject(asset);
    if (!object)
        return false;
    __try {
        if (!OrcPrepareWeaponObjectForBodyAttachment(
                object, r.modelId, OrcWeaponBodyInstanceSource::Replacement, r.pinnedMaterials)) {
            return false;
        }
        r.rwObject = object;
        r.usesReplacementMesh = true;
        return true;
    } __finally {
        if (!r.rwObject)
            OrcDestroyRwObjectInstance(object);
    }
}

static void CreateWeaponInstance(RenderedWeapon* arr, CPed* ped, int wt, int mid,
    bool secondary, int boneId, WeaponReplacementAsset* asset) {
    static_assert(OrcSelectWeaponCloneRenderContract(OrcWeaponCloneUsage::BodyAttachment) ==
        OrcWeaponCloneRenderContract::DedicatedBodyAttachment);
    const int freeIndex = FindFree(arr);
    if (freeIndex < 0) {
        OrcLogInfoThrottled(923, 5000u, "weapon body: no free slot wt=%d", wt);
        return;
    }

    RenderedWeapon& r = arr[freeIndex];
    r.active = true;
    r.weaponType = wt;
    r.modelId = mid;
    r.secondary = secondary;
    if (asset) {
        r.replacementKey = asset->key;
        if (TryCreateReplacementInstance(r, *asset)) {
            LogWeaponInstance(ped, r, OrcWeaponBodyInstanceSource::Replacement);
            return;
        }
        OrcLogInfoThrottled(923, 5000u,
            "weapon body: replacement rejected, stock fallback wt=%d model=%d key=%s",
            wt, mid, r.replacementKey.c_str());
    }
    TryCreateStockInstance(ped, r, boneId);
}

static bool RenderBodyWeaponWithLighting(CPed* ped, RenderedWeapon& r,
    const CVector& lightPos, WeaponTextureAsset* textureAsset) {
    __try {
        OrcApplyAttachmentLightingForPed(ped, lightPos);
        return OrcRenderBodyWeaponObjectWithTextures(
            ped, r.weaponType, r.rwObject, textureAsset, r.usesReplacementMesh, g_weaponTexturesEnabled);
    } __finally {
        // GenerateLightsAffectingObject appends to the world light list. Reset it before the next weapon.
        CPointLights::RemoveLightsAffectingObject();
    }
}

static bool RenderOneWeapon(CPed* ped, RenderedWeapon& r) {
    if (!r.rwObject || r.rwObject->type != rpCLUMP) return false;

    const WeaponCfg& wc = r.secondary ? GetWeaponCfg2ForPed(ped, r.weaponType) : GetWeaponCfgForPed(ped, r.weaponType);
    if (!wc.enabled || wc.boneId == 0) return false;
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

    const CVector lightPos = { mtx.pos.x, mtx.pos.y, mtx.pos.z };
    static const std::string noReplacementKey;
    WeaponTextureAsset* textureAsset = g_weaponTexturesEnabled
        ? OrcResolveUsableWeaponTextureAssetForPed(ped,
              r.weaponType,
              true,
              r.usesReplacementMesh ? &r.replacementKey : &noReplacementKey)
        : nullptr;
    const bool rendered = RenderBodyWeaponWithLighting(ped, r, lightPos, textureAsset);
    r.drawFailed = !rendered;
    return rendered;
}
struct WantedBodyWeapon {
    int weaponType;
    int modelId;
    int primaryBone;
    int secondaryBone;
    WeaponReplacementAsset* replacement = nullptr;
    bool hasPrimary = false;
    bool hasSecondary = false;
};

static bool CanDrawBodyWeapons(const CPed* ped) {
    return ped && ped->m_pRwClump && ped->bIsVisible && !ped->bDontRender;
}

static int GetWantedSecondaryBone(CPed* ped, int weaponType) {
    if (!g_considerWeaponSkills || weaponType >= static_cast<int>(g_cfg2.size()) ||
        !OrcWeaponTypeCanUseVanillaInfoContract(weaponType, static_cast<int>(WEAPONTYPE_PARACHUTE))) {
        return 0;
    }
    const auto type = static_cast<eWeaponType>(weaponType);
    if (ped->GetWeaponSkill(type) != WEAPSKILL_PRO)
        return 0;
    const CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(type, WEAPSKILL_PRO);
    if (!info || !info->m_nFlags.bTwinPistol)
        return 0;
    const WeaponCfg& cfg = GetWeaponCfg2ForPed(ped, weaponType);
    return cfg.enabled ? cfg.boneId : 0;
}

void OrcSyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress) {
    if (!CanDrawBodyWeapons(ped)) return;
    constexpr auto inventorySize = std::extent_v<decltype(CPed::m_aWeapons)>;
    const auto selectedSlot = ped->m_nSelectedWepSlot;
    const int selectedType = selectedSlot < inventorySize ? ped->m_aWeapons[selectedSlot].m_eWeaponType : 0;
    const int maxWt = static_cast<int>(std::min(g_cfg.size(), g_weaponModelId.size()));
    // Work is bounded by the actual inventory, including on clients with extended weapon IDs.
    std::array<WantedBodyWeapon, inventorySize> wanted;
    auto wantedEnd = wanted.begin();
    for (std::size_t s = 0; s < inventorySize; ++s) {
        const CWeapon& weapon = ped->m_aWeapons[s];
        const int wt = weapon.m_eWeaponType;
        if (wt <= 0 || wt >= maxWt || wt == selectedType || g_weaponModelId[wt] <= 0)
            continue;
        if (suppress && wt < static_cast<int>(suppress->size()) && (*suppress)[wt])
            continue;
        if (s >= 2 && s <= 9 && weapon.m_nAmmoTotal == 0)
            continue;
        const auto existing = std::find_if(wanted.begin(), wantedEnd,
            [wt](const WantedBodyWeapon& entry) { return entry.weaponType == wt; });
        if (existing != wantedEnd)
            continue;
        const WeaponCfg& cfg = GetWeaponCfgForPed(ped, wt);
        if (!cfg.enabled || cfg.boneId == 0)
            continue;
        const int primaryBone = cfg.boneId;
        *wantedEnd++ = { wt, g_weaponModelId[wt], primaryBone, GetWantedSecondaryBone(ped, wt) };
    }

    std::sort(wanted.begin(), wantedEnd, [](const WantedBodyWeapon& a, const WantedBodyWeapon& b) {
        return a.weaponType < b.weaponType;
    });
    if (g_weaponReplacementEnabled && g_weaponReplacementOnBody) {
        for (auto it = wanted.begin(); it != wantedEnd; ++it)
            it->replacement = OrcResolveUsableWeaponReplacementAssetForPed(ped, it->weaponType, true);
    }
    for (int i = 0; i < OrcWeaponSlotMax; ++i) {
        RenderedWeapon& r = arr[i];
        if (!r.active)
            continue;
        auto wantedWeapon = std::lower_bound(wanted.begin(), wantedEnd, r.weaponType,
            [](const WantedBodyWeapon& entry, int wt) { return entry.weaponType < wt; });
        if (wantedWeapon == wantedEnd || wantedWeapon->weaponType != r.weaponType ||
            wantedWeapon->modelId != r.modelId || (r.secondary && wantedWeapon->secondaryBone == 0) ||
            (wantedWeapon->replacement ? wantedWeapon->replacement->key != r.replacementKey : !r.replacementKey.empty())) {
            OrcDestroyRenderedWeapon(r);
            continue;
        }
        (r.secondary ? wantedWeapon->hasSecondary : wantedWeapon->hasPrimary) = true;
        if (r.drawFailed && r.usesReplacementMesh) {
            OrcDestroyRwObjectInstance(r.rwObject);
            r.usesReplacementMesh = false;
            r.drawFailed = false;
        }
        if (!r.rwObject) {
            TryCreateStockInstance(ped, r, r.secondary ? wantedWeapon->secondaryBone : wantedWeapon->primaryBone);
        }
    }
    for (auto it = wanted.begin(); it != wantedEnd; ++it) {
        if (!it->hasPrimary)
            CreateWeaponInstance(arr, ped, it->weaponType, it->modelId, false, it->primaryBone, it->replacement);
    }
    for (auto it = wanted.begin(); it != wantedEnd; ++it) {
        if (it->secondaryBone != 0 && !it->hasSecondary)
            CreateWeaponInstance(arr, ped, it->weaponType, it->modelId, true, it->secondaryBone, it->replacement);
    }
}

static bool IsBodyWeaponDrawable(const RenderedWeapon& weapon) {
    return weapon.rwObject && !weapon.drawFailed;
}

bool OrcWeaponHasCachedBodyWeapons() {
    for (const RenderedWeapon& weapon : g_rendered) {
        if (IsBodyWeaponDrawable(weapon))
            return true;
    }
    if (!g_renderAllPedsWeapons)
        return false;
    for (const auto& entry : g_otherPedsRendered) {
        for (const RenderedWeapon& weapon : entry.second.weapons) {
            if (IsBodyWeaponDrawable(weapon))
                return true;
        }
    }
    return false;
}

static int RenderPedWeapons(CPed* ped, RenderedWeapon* arr) {
    int rendered = 0;
    for (int i = 0; i < OrcWeaponSlotMax; i++) {
        if (!IsBodyWeaponDrawable(arr[i])) continue;
        __try {
            if (RenderOneWeapon(ped, arr[i]))
                ++rendered;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr[i].drawFailed = true;
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
    if (!CanDrawBodyWeapons(ped))
        return 0;
    bool hasActiveWeapon = false;
    for (int i = 0; i < OrcWeaponSlotMax; ++i) {
        if (IsBodyWeaponDrawable(arr[i])) {
            hasActiveWeapon = true;
            break;
        }
    }
    if (!hasActiveWeapon)
        return 0;

    int rendered = 0;
    __try {
        rendered = RenderPedWeapons(ped, arr);
    } __finally {
        SetAmbientColours();
        DeActivateDirectional();
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
