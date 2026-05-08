#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CClumpModelInfo.h"
#include "CModelInfo.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"
#include "eWeaponType.h"
#include "CVector.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_render.h"
#include "orc_types.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_runtime.h"

using namespace plugin;

RenderedWeapon g_rendered[OrcWeaponSlotMax] = {};
using PedWeaponCache = std::array<RenderedWeapon, OrcWeaponSlotMax>;
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

static bool OrcRwFrameIsDescendantOf(RwFrame* frame, RwFrame* ancestor) {
    if (!frame || !ancestor)
        return false;
    constexpr int kMaxAnc = 64;
    int steps = 0;
    for (RwFrame* x = frame; x && steps < kMaxAnc;
         x = reinterpret_cast<RwFrame*>(plugin::GetObjectParent(reinterpret_cast<RwObject*>(x))),
            ++steps) {
        if (x == ancestor)
            return true;
    }
    return false;
}

void OrcDestroyRenderedWeapon(RenderedWeapon& r) {
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
        for (int i = 0; i < OrcWeaponSlotMax; i++) OrcDestroyRenderedWeapon(kv.second[i]);
    }
    g_otherPedsRendered.clear();
}

static bool CreateWeaponInstance(RenderedWeapon* arr, int wt, bool secondary, int slot, CPed* ped) {
    if (wt <= 0) return false;
    if (secondary) {
        if (g_cfg2.empty() || wt >= (int)g_cfg2.size()) return false;
    } else {
        if (g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
    }
    const WeaponCfg& wc = secondary ? GetWeaponCfg2ForPed(ped, wt) : GetWeaponCfgForPed(ped, wt);
    if (!wc.enabled || wc.boneId == 0) return false;
    if (FindSlotByType(arr, wt, secondary) >= 0) return true;

    CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    if (!info) return false;
    int mid = info->m_nModelId;
    if (mid <= 0) return false;

    RwMatrix* bone = OrcGetBoneMatrix(ped, wc.boneId);
    if (!bone) return false;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));

    std::string replacementKey;
    RwObject* inst = nullptr;
    if (g_weaponReplacementEnabled && g_weaponReplacementOnBody) {
        if (WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, wt, true)) {
            inst = OrcCloneWeaponReplacementObject(*asset);
            if (inst)
                replacementKey = asset->key;
        }
    }

    if (!inst) {
        auto* mi = CModelInfo::GetModelInfo(mid);
        if (!mi || !mi->m_pRwObject) {
            // Auto-request weapon model streaming (supports modded weapon.dat setups).
            if (mid > 0 && !CStreaming::HasModelLoaded(mid)) {
                static std::unordered_set<int> requested;
                if (requested.insert(mid).second) {
                    // Только запрос: синхронный LoadAllRequestedModels на drawingEvent давал длинный стоп-кадр.
                    CStreaming::RequestModel(mid, 0);
                }
            }
            return false;
        }
        inst = mi->CreateInstance(&mtx);
    }
    if (!inst) return false;

    // Сброс render-callback на дефолтный RW + leak материалов (см. InitAtomicCB).
    if (inst->type == rpCLUMP) {
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), OrcInitAttachmentAtomicCB, nullptr);
    } else if (inst->type == rpATOMIC) {
        OrcInitAttachmentAtomicCB(reinterpret_cast<RpAtomic*>(inst), nullptr);
    } else {
        OrcDestroyRwObjectInstance(inst);
        return false;
    }

    int fi = FindFree(arr);
    if (fi < 0) {
        OrcLogError("CreateWeaponInstance: no free slot (weapon type %d)", wt);
        OrcDestroyRwObjectInstance(inst);
        return false;
    }
    arr[fi] = { true, wt, secondary, mid, slot, inst, replacementKey };
    return true;
}

static void RenderOneWeapon(CPed* ped, RenderedWeapon& r) {
    if (!r.rwObject) return;

    const WeaponCfg& wc = r.secondary ? GetWeaponCfg2ForPed(ped, r.weaponType) : GetWeaponCfgForPed(ped, r.weaponType);
    RwMatrix* bone = OrcGetBoneMatrix(ped, wc.boneId);
    if (!bone) return;

    RpAtomic* atomic = nullptr;
    RwFrame*  frame  = nullptr;
    if (r.rwObject->type == rpATOMIC) {
        atomic = reinterpret_cast<RpAtomic*>(r.rwObject);
        frame  = RpAtomicGetFrame(atomic);
    } else if (r.rwObject->type == rpCLUMP) {
        frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(r.rwObject));
    }
    if (!frame) return;

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

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    OrcApplyAttachmentLightingForPed(ped, lightPos);
    WeaponTextureAsset* textureAsset = OrcResolveUsableWeaponTextureAssetForPed(ped,
        r.weaponType,
        true,
        r.replacementKey.empty() ? nullptr : &r.replacementKey);
    const bool meshIsReplacement = !r.replacementKey.empty();
    OrcApplyWeaponTexturesCombined(ped, r.weaponType, r.rwObject, textureAsset, meshIsReplacement);

    if (r.rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(r.rwObject);
        if (!clump) {
            OrcRestoreWeaponTextureOverrides();
            return;
        }
        RpClumpForAllAtomics(clump, OrcPrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        OrcPrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
    OrcRestoreWeaponTextureOverrides();
}
void OrcSyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress) {
    if (!ped) return;
    unsigned char curSlot = ped->m_nSelectedWepSlot;
    int curType = 0;
    if (curSlot < 13) curType = (int)ped->m_aWeapons[curSlot].m_eWeaponType;
    if (g_cfg.empty()) return;
    const int maxWt = (int)g_cfg.size();
    std::vector<char> want(maxWt, 0);
    std::vector<char> want2(maxWt, 0);
    for (int s = 0; s < 13; s++) {
        auto& w = ped->m_aWeapons[s];
        int wt = (int)w.m_eWeaponType;
        if (wt <= 0 || wt >= maxWt) continue;
        if (suppress && wt < (int)suppress->size() && (*suppress)[wt]) continue;
        if (wt == curType) continue;
        const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
        if (!wc.enabled || wc.boneId == 0) continue;
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
        bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
        if (needsAmmo && w.m_nAmmoTotal == 0) continue;
        want[wt] = true;

        CWeaponInfo* twinInfo = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 2);
        if (!twinInfo) twinInfo = wi;
        if (g_considerWeaponSkills && twinInfo && twinInfo->m_nFlags.bTwinPistol) {
            const char skill = ped->GetWeaponSkill(static_cast<eWeaponType>(wt));
            if (skill == WEAPSKILL_PRO) {
                const WeaponCfg& wc2 = GetWeaponCfg2ForPed(ped, wt);
                if (wc2.enabled && wc2.boneId != 0) want2[wt] = true;
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

int OrcRenderPedWeapons(CPed* ped, RenderedWeapon* arr) {
    int active = 0;
    for (int i = 0; i < OrcWeaponSlotMax; i++) {
        if (!arr[i].active) continue;
        active++;
        RenderOneWeapon(ped, arr[i]);
    }
    return active;
}
