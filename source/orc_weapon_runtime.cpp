#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CHud.h"
#include "CSprite2d.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "external/MinHook/include/MinHook.h"

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_render.h"
#include "orc_types.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_runtime.h"

#include "samp_bridge.h"

using namespace plugin;

static void SyncWeaponReplacementMuzzleFlashAlpha(CPed* ped, RwObject* obj);

static bool g_pedWeaponModelHooksInstalled = false;
using AddWeaponModel_t = void(__thiscall*)(CPed*, int);
using RemoveWeaponModel_t = void(__thiscall*)(CPed*, int);
static AddWeaponModel_t g_AddWeaponModel_Orig = nullptr;
static RemoveWeaponModel_t g_RemoveWeaponModel_Orig = nullptr;
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
                    CStreaming::RequestModel(mid, 0);
                    CStreaming::LoadAllRequestedModels(false);
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

    static std::unordered_set<int> logged;
    if (!secondary && logged.insert(wt).second) {
    }
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
    if (!r.replacementKey.empty())
        SyncWeaponReplacementMuzzleFlashAlpha(ped, r.rwObject);

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

static RwFrame* GetRwObjectRootFrame(RwObject* object) {
    if (!object)
        return nullptr;
    if (object->type == rpATOMIC)
        return RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(object));
    if (object->type == rpCLUMP)
        return RpClumpGetFrame(reinterpret_cast<RpClump*>(object));
    return nullptr;
}

// Align replacement clone root to stock weapon pose before handing it to CPed as m_pWeaponObject,
// so the engine continues IK from the same basis as vanilla (reduces “floating” vs hand).
static void CopyRwObjectRootMatrix(RwObject* src, RwObject* dst) {
    if (!src || !dst)
        return;
    RwFrame* sf = GetRwObjectRootFrame(src);
    RwFrame* df = GetRwObjectRootFrame(dst);
    if (!sf || !df)
        return;
    __try {
        std::memcpy(RwFrameGetMatrix(df), RwFrameGetMatrix(sf), sizeof(RwMatrix));
        RwMatrixUpdate(RwFrameGetMatrix(df));
        RwFrameUpdateObjects(df);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("CopyRwObjectRootMatrix: SEH ex=0x%08X", GetExceptionCode());
    }
}

// Replacement DFFs often include a muzzle-flash mesh with the same texture names as vanilla.
// The game drives visibility via CPed::m_nWeaponGunflashAlphaMP1 (and MP2) during held render;
// our InitAttachmentAtomicCB forces material alpha to 255, so sync flash materials each frame.
static bool IsWeaponMuzzleFlashTextureName(const char* name) {
    if (!name || !name[0])
        return false;
    const std::string s = OrcToLowerAscii(name);
    if (s.find("gunflash") != std::string::npos)
        return true;
    if (s.find("muzzleflash") != std::string::npos)
        return true;
    if (s.find("muzzle_flash") != std::string::npos)
        return true;
    if (s.find("muzzle_texture") != std::string::npos)
        return true;
    return false;
}

static RpMaterial* SyncHeldReplacementMuzzleFlashMatCB(RpMaterial* m, void* data) {
    const int* alpha = reinterpret_cast<int*>(data);
    if (!m || !alpha)
        return m;
    if (!m->texture || !IsWeaponMuzzleFlashTextureName(m->texture->name))
        return m;
    int a = *alpha;
    if (a < 0)
        a = 0;
    if (a > 255)
        a = 255;
    m->color.alpha = static_cast<RwUInt8>(a);
    return m;
}

static RpAtomic* SyncHeldReplacementMuzzleFlashAtomicCB(RpAtomic* atomic, void* data) {
    if (!atomic || !atomic->geometry)
        return atomic;
    RpGeometryForAllMaterials(atomic->geometry, SyncHeldReplacementMuzzleFlashMatCB, data);
    return atomic;
}

static void SyncWeaponReplacementMuzzleFlashAlpha(CPed* ped, RwObject* obj) {
    if (!ped || !obj)
        return;
    int a = static_cast<int>(ped->m_nWeaponGunflashAlphaMP1);
    __try {
        if (obj->type == rpCLUMP)
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(obj), SyncHeldReplacementMuzzleFlashAtomicCB, &a);
        else if (obj->type == rpATOMIC)
            SyncHeldReplacementMuzzleFlashAtomicCB(reinterpret_cast<RpAtomic*>(obj), &a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SyncWeaponReplacementMuzzleFlashAlpha: SEH ex=0x%08X", GetExceptionCode());
    }
}

struct HeldWeaponReplacementState {
    int weaponType = 0;
    std::string replacementKey;
    RwObject* rwObject = nullptr;
    RwObject* originalObject = nullptr;
    bool captureActive = false;
    bool hideBaseMode = false;
};

static std::unordered_map<int, HeldWeaponReplacementState> g_heldWeaponReplacements;

/// True when `obj` is the held-weapon clone from Guns/GunsNick replacement (`g_heldWeaponReplacements`).
static bool HeldWeaponRwObjectIsReplacementClone(CPed* ped, RwObject* obj) {
    if (!ped || !obj)
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return false;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end())
        return false;
    return it->second.rwObject == obj;
}

// `Events::pedRenderEvent` wraps an inner CALL inside `CPed::Render`, not the whole function. Restoring the
// stock `m_pWeaponObject` in the event's "after" callback runs *before* the rest of `CPed::Render` draws the
// vanilla held weapon, so the engine paints stock on top. Queue the stock pointer and write it in
// `OrcFlushDeferredHeldWeaponSlotRestore` (D3D EndScene = after the scene is submitted for the frame).
// Same timing applies to Guns TXD RwMaterial swaps: they are queued in a held defer list and restored at flush.
// Deferred restore stores stock + clone: blind overwrite with stock can clash if weapon changed/was cleared
// between pedRender.after and flush → refcount corruption (`CBaseModelInfo::RemoveRef`).
struct DeferredHeldWeaponSlotRestore {
    RwObject* stock = nullptr;
    RwObject* clone = nullptr;
};
static std::unordered_map<int, DeferredHeldWeaponSlotRestore> g_deferredHeldWeaponStockRestore;

// After `pedRenderEvent.after` we queue stock+clone and leave the clone in `m_pWeaponObject` until
// EndScene/Present so vanilla can draw the held mesh. If the game swaps weapons before flush
// (`SetCurrentWeapon` → `AddWeaponModel` → internal `RemoveWeaponModel`), vanilla must see the stock
// clump in the slot — otherwise `RemoveWeaponModel` can hit a null `CBaseModelInfo` in `AddRef`.
static void OrcRestoreDeferredHeldStockIfSlotStillHasClone(CPed* ped) {
    if (!ped)
        return;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    auto d = g_deferredHeldWeaponStockRestore.find(pedRef);
    if (d == g_deferredHeldWeaponStockRestore.end())
        return;
    const DeferredHeldWeaponSlotRestore& r = d->second;
    if (r.stock && r.clone && ped->m_pWeaponObject == r.clone) {
        ped->m_pWeaponObject = r.stock;
        g_deferredHeldWeaponStockRestore.erase(pedRef);
    }
}

void OrcFlushDeferredHeldWeaponSlotRestore() {
    /// `pedRenderEvent.after` fires before GTA draws held `m_pWeaponObject`; restores here (EndScene/Present).
    OrcRestoreWeaponHeldTextureOverrides();
    if (g_deferredHeldWeaponStockRestore.empty())
        return;
    if (!CPools::ms_pPedPool) {
        g_deferredHeldWeaponStockRestore.clear();
        return;
    }
    for (const auto& kv : g_deferredHeldWeaponStockRestore) {
        CPed* ped = CPools::GetPed(kv.first);
        if (!ped)
            continue;
        const DeferredHeldWeaponSlotRestore& r = kv.second;
        if (r.stock && r.clone && ped->m_pWeaponObject == r.clone)
            ped->m_pWeaponObject = r.stock;
    }
    g_deferredHeldWeaponStockRestore.clear();
}

static int GetPedCurrentWeaponType(CPed* ped) {
    if (!ped)
        return 0;
    const unsigned char slot = ped->m_nSelectedWepSlot;
    if (slot >= 13)
        return 0;
    const int wt = (int)ped->m_aWeapons[slot].m_eWeaponType;
    if (wt <= 0)
        return 0;
    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    const bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
    if (needsAmmo && ped->m_aWeapons[slot].m_nAmmoTotal == 0)
        return 0;
    return wt;
}

// Selected-slot weapon type without ammo filtering. Used for held visual replacement / textures:
// SA:MP (and similar) may leave m_nAmmoTotal at 0 on the client while the pistol model is still held.
static int GetPedSelectedWeaponTypeForReplace(CPed* ped) {
    if (!ped)
        return 0;
    const unsigned char slot = ped->m_nSelectedWepSlot;
    if (slot >= 13)
        return 0;
    const int wt = (int)ped->m_aWeapons[slot].m_eWeaponType;
    if (wt <= 0)
        return 0;
    return wt;
}

static int WeaponTypeFromModelId(int modelId) {
    if (modelId <= 0)
        return 0;
    // Covers vanilla + typical weapon.dat extensions (LoadWeaponObject cache).
    for (int wt = 1; wt < 96; ++wt) {
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
        if (wi && wi->m_nModelId == modelId)
            return wt;
    }
    return 0;
}

// Resolve visible held weapon for replacement: slot-based first, then CPed::m_nWeaponModelId and slot scan.
// pedRenderEvent.before often runs when m_aWeapons/m_nSelectedWepSlot are cleared or stale but m_nWeaponModelId
// still matches the weapon being drawn (common in SA:MP).
int OrcResolveWeaponHeldVisualWeaponType(CPed* ped) {
    if (!ped)
        return 0;
    int wt = GetPedSelectedWeaponTypeForReplace(ped);
    if (wt > 0)
        return wt;

    const int mid = ped->m_nWeaponModelId;
    wt = WeaponTypeFromModelId(mid);
    if (wt > 0)
        return wt;

    for (int s = 0; s < 13; ++s) {
        const int t = (int)ped->m_aWeapons[s].m_eWeaponType;
        if (t <= 0)
            continue;
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(t), 1);
        if (wi && wi->m_nModelId == mid && mid > 0)
            return t;
    }

    int singleWt = 0;
    int nonZeroSlots = 0;
    for (int s = 0; s < 13; ++s) {
        const int t = (int)ped->m_aWeapons[s].m_eWeaponType;
        if (t <= 0)
            continue;
        ++nonZeroSlots;
        singleWt = t;
    }
    if (nonZeroSlots == 1)
        return singleWt;

    if ((int)ped->m_nSavedWeapon > (int)WEAPONTYPE_UNARMED)
        return (int)ped->m_nSavedWeapon;

    return 0;
}

int OrcWeaponHudGetHeldReplacementWeaponTypeIfAny(CPed* ped) {
    if (!ped)
        return 0;
    const int pref = CPools::GetPedRef(ped);
    if (pref <= 0)
        return 0;
    auto it = g_heldWeaponReplacements.find(pref);
    if (it == g_heldWeaponReplacements.end())
        return 0;
    const HeldWeaponReplacementState& st = it->second;
    if (st.weaponType > 0 && st.rwObject)
        return st.weaponType;
    return 0;
}

static bool ShouldReplaceHeldWeaponForPed(CPed* ped) {
    if (!g_enabled || !g_weaponReplacementEnabled || !g_weaponReplacementInHands || !ped)
        return false;
    CPlayerPed* player = FindPlayerPed(0);
    if (player && ped == player)
        return true;
    if (!g_renderAllPedsWeapons || !player)
        return false;
    const CVector& pp = player->GetPosition();
    const CVector& p = ped->GetPosition();
    const float dx = p.x - pp.x;
    const float dy = p.y - pp.y;
    const float dz = p.z - pp.z;
    return (dx * dx + dy * dy + dz * dz) <= (g_renderAllPedsRadius * g_renderAllPedsRadius);
}

static bool ShouldTextureHeldWeaponForPed(CPed* ped) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped)
        return false;
    CPlayerPed* player = FindPlayerPed(0);
    if (player && ped == player)
        return true;
    if (!g_renderAllPedsWeapons || !player)
        return false;
    const CVector& pp = player->GetPosition();
    const CVector& p = ped->GetPosition();
    const float dx = p.x - pp.x;
    const float dy = p.y - pp.y;
    const float dz = p.z - pp.z;
    return (dx * dx + dy * dy + dz * dz) <= (g_renderAllPedsRadius * g_renderAllPedsRadius);
}

void OrcPrepareHeldWeaponTextureBefore(CPed* ped) {
    if (!ShouldTextureHeldWeaponForPed(ped) || !ped->m_pWeaponObject)
        return;
    const int wt = OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wt <= 0)
        return;
    const std::string* heldReplHint = nullptr;
    const int heldPref = CPools::GetPedRef(ped);
    if (heldPref > 0) {
        auto hit = g_heldWeaponReplacements.find(heldPref);
        if (hit != g_heldWeaponReplacements.end() &&
            hit->second.rwObject == ped->m_pWeaponObject &&
            !hit->second.replacementKey.empty()) {
            heldReplHint = &hit->second.replacementKey;
        }
    }
    WeaponTextureAsset* asset = OrcResolveUsableWeaponTextureAssetForPed(ped, wt, true, heldReplHint);
    const bool meshIsReplacement = HeldWeaponRwObjectIsReplacementClone(ped, ped->m_pWeaponObject);
    OrcWeaponHeldTextureDeferBegin();
    OrcApplyWeaponTexturesCombined(ped, wt, ped->m_pWeaponObject, asset, meshIsReplacement);
    OrcWeaponHeldTextureDeferEnd();
}

void OrcDestroyAllHeldWeaponReplacementInstances() {
    OrcFlushDeferredHeldWeaponSlotRestore();
    for (auto& kv : g_heldWeaponReplacements) {
        HeldWeaponReplacementState& st = kv.second;
        if (st.captureActive) {
            CPed* ped = CPools::GetPed(kv.first);
            if (ped) {
                if (st.hideBaseMode && st.originalObject)
                    ped->m_pWeaponObject = st.originalObject;
                else if (st.rwObject && ped->m_pWeaponObject == st.rwObject && st.originalObject)
                    ped->m_pWeaponObject = st.originalObject;
            }
        }
        st.captureActive = false;
        st.hideBaseMode = false;
        st.originalObject = nullptr;
        OrcDestroyRwObjectInstance(st.rwObject);
    }
    g_heldWeaponReplacements.clear();
    g_deferredHeldWeaponStockRestore.clear();
}

void OrcPruneHeldWeaponReplacementInstances() {
    if (!CPools::ms_pPedPool) {
        OrcDestroyAllHeldWeaponReplacementInstances();
        return;
    }
    std::unordered_set<int> alive;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
        CPed* ped = CPools::ms_pPedPool->GetAt(i);
        if (ped)
            alive.insert(CPools::GetPedRef(ped));
    }
    for (auto it = g_heldWeaponReplacements.begin(); it != g_heldWeaponReplacements.end();) {
        if (alive.find(it->first) == alive.end()) {
            g_deferredHeldWeaponStockRestore.erase(it->first);
            OrcDestroyRwObjectInstance(it->second.rwObject);
            it = g_heldWeaponReplacements.erase(it);
        } else {
            ++it;
        }
    }
}

// R_Hand (RpHAnim node id) — fallback when weapon has no enabled body slot in OrcOutFit.ini.
static constexpr int kHeldReplacementFallbackBoneId = 24;

// Same placement math as RenderOneWeapon / body attachments (no SetupLighting — avoids breaking scene lights).
// If [weapon] is disabled or Bone=0 in INI, uses kHeldReplacementFallbackBoneId so hide-base still draws.
static bool PositionHeldReplacementLikeBodyWeapon(CPed* ped, int wt, RwObject* rwObject) {
    if (!ped || !rwObject || wt <= 0 || wt >= (int)g_cfg.size())
        return false;
    const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
    const bool useIni = wc.enabled && wc.boneId != 0;
    const int boneId = useIni ? wc.boneId : kHeldReplacementFallbackBoneId;
    RwMatrix* bone = OrcGetBoneMatrix(ped, boneId);
    if (!bone)
        return false;

    RwFrame* frame = nullptr;
    if (rwObject->type == rpATOMIC) {
        frame = RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(rwObject));
    } else if (rwObject->type == rpCLUMP) {
        frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(rwObject));
    }
    if (!frame)
        return false;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    if (useIni) {
        OrcApplyAttachmentOffset(&mtx, wc.x, wc.y, wc.z);
        OrcRotateAttachmentMatrix(&mtx, wc.rx, wc.ry, wc.rz);
    }
    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    if (useIni && wc.scale != 1.0f) {
        RwV3d s = { wc.scale, wc.scale, wc.scale };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);
    return true;
}

static void RenderHeldReplacementHideBaseForPed(CPed* ped, HeldWeaponReplacementState& state) {
    if (!ped || !state.rwObject || state.weaponType <= 0)
        return;
    const int wt = state.weaponType;
    if (!PositionHeldReplacementLikeBodyWeapon(ped, wt, state.rwObject)) {
        OrcLogInfoThrottled(29, 4000u,
            "held wr hide-base: position failed pedRef=%d wt=%d (bone/skeleton?)",
            CPools::GetPedRef(ped), wt);
        return;
    }

    const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
    const bool useIni = wc.enabled && wc.boneId != 0;
    const int boneId = useIni ? wc.boneId : kHeldReplacementFallbackBoneId;
    RwMatrix* bone = OrcGetBoneMatrix(ped, boneId);
    CVector lightPos{};
    if (bone)
        lightPos = CVector(bone->pos.x, bone->pos.y, bone->pos.z);
    else {
        CVector bc{};
        ped->GetBoundCentre(bc);
        lightPos = bc;
    }

    if (g_weaponTexturesEnabled) {
        WeaponTextureAsset* tex = OrcResolveUsableWeaponTextureAssetForPed(ped,
            wt,
            true,
            state.replacementKey.empty() ? nullptr : &state.replacementKey);
        OrcApplyWeaponTexturesCombined(ped, wt, state.rwObject, tex, true);
    }
    SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
    OrcApplyAttachmentLightingForPed(ped, lightPos, 0.5f);

    if (state.rwObject->type == rpCLUMP) {
        RpClump* clump = reinterpret_cast<RpClump*>(state.rwObject);
        RpClumpForAllAtomics(clump, OrcPrepAtomicCB, nullptr);
        OrcTryRpClumpRender(clump);
    } else {
        RpAtomic* atomic = reinterpret_cast<RpAtomic*>(state.rwObject);
        OrcPrepAtomicCB(atomic, nullptr);
        if (atomic->renderCallBack)
            atomic->renderCallBack(atomic);
    }
    OrcRestoreWeaponTextureOverrides();
}

void OrcPrepareHeldWeaponReplacementBefore(CPed* ped) {
    const int pedRefEarly = ped ? CPools::GetPedRef(ped) : 0;

    if (!ShouldReplaceHeldWeaponForPed(ped)) {
        OrcLogInfoThrottled(20, 3000u,
            "held wr: gated pedRef=%d en=%d wrEn=%d inHands=%d allPeds=%d r=%.1f",
            pedRefEarly, g_enabled ? 1 : 0, g_weaponReplacementEnabled ? 1 : 0,
            g_weaponReplacementInHands ? 1 : 0, g_renderAllPedsWeapons ? 1 : 0, g_renderAllPedsRadius);
        return;
    }

    const int wt = OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wt <= 0) {
        OrcLogInfoThrottled(22, 1200u, "held wr: wtRes<=0 pedRef=%d mid=%d", pedRefEarly, ped ? ped->m_nWeaponModelId : -1);
        return;
    }

    WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, wt, true);
    if (!asset) {
        WeaponReplacementAsset* resolvedOnly = OrcResolveWeaponReplacementAssetForPed(ped, wt, true);
        const std::string wLower = OrcGetWeaponModelBaseNameLower(wt);
        const std::string skinRaw = GetPedStdSkinDffName(ped);
        char nick[64] = {};
        bool isLocal = false;
        const bool gotNick = samp_bridge::IsSampBuildKnown() &&
            samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal);
        if (resolvedOnly) {
            OrcLogInfoThrottled(24, 2500u,
                "held wr: mapping present but unusable (load?) pedRef=%d wt=%d key=%s weapon=%s skin=\"%s\"",
                pedRefEarly, wt, resolvedOnly->key.c_str(), wLower.c_str(), skinRaw.c_str());
        } else if (!OrcWeaponReplacementIsStickyVanillaChoice(ped, wt)) {
            OrcLogInfoThrottled(23, 2500u,
                "held wr: no mapping pedRef=%d wt=%d weapon=%s skin=\"%s\" sampKnown=%d nick=%d nick=\"%.48s\" wrPools=%zu srPools=%zu nickKeys=%zu",
                pedRefEarly, wt, wLower.c_str(), skinRaw.c_str(),
                samp_bridge::IsSampBuildKnown() ? 1 : 0, gotNick ? 1 : 0, nick,
                OrcWeaponAssetsDbgRandomReplacementWeaponPools(), OrcWeaponAssetsDbgRandomReplacementSkinPools(),
                OrcWeaponAssetsDbgReplacementNickKeys());
        }
        return;
    }

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0) {
        OrcLogInfoThrottled(27, 4000u, "held wr: pedRef<=0 wt=%d key=%s", wt, asset->key.c_str());
        return;
    }
    HeldWeaponReplacementState& state = g_heldWeaponReplacements[pedRef];
    if (!state.rwObject || state.weaponType != wt || state.replacementKey != asset->key) {
        OrcDestroyRwObjectInstance(state.rwObject);
        state.rwObject = OrcCloneWeaponReplacementObject(*asset);
        state.weaponType = wt;
        state.replacementKey = asset->key;
        if (!state.rwObject) {
            OrcLogError("held wr: CloneWeaponReplacementObject failed pedRef=%d wt=%d key=%s",
                pedRef, wt, asset->key.c_str());
            return;
        }
        OrcLogInfo("held wr: clone ok pedRef=%d wt=%d key=%s skin=%s",
            pedRef, wt, asset->key.c_str(), GetPedStdSkinDffName(ped).c_str());
    }
    if (!state.rwObject)
        return;

    // After CPed::Render ends we restore the stock mesh into the slot; next frame `m_pWeaponObject` is stock again.
    // Same-frame AddWeaponModel hook may leave the clone in the slot — sync pose from stock either way.
    if (state.rwObject && state.originalObject && state.weaponType == wt && state.replacementKey == asset->key) {
        if (ped->m_pWeaponObject == state.originalObject) {
            CopyRwObjectRootMatrix(state.originalObject, state.rwObject);
            SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
            state.hideBaseMode = false;
            state.captureActive = true;
            ped->m_pWeaponObject = state.rwObject;
            return;
        }
        if (ped->m_pWeaponObject == state.rwObject) {
            CopyRwObjectRootMatrix(state.originalObject, state.rwObject);
            SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
            return;
        }
    }

    // SA:MP may leave `m_pWeaponObject` null during ped render while wt still resolves.
    // Re-use after-draw path (clone in pedRenderEvent.after) until a stock clump exists.
    if (state.hideBaseMode && !state.originalObject && state.rwObject && state.weaponType == wt &&
        state.replacementKey == asset->key) {
        if (!ped->m_pWeaponObject) {
            state.captureActive = true;
            return;
        }
        state.hideBaseMode = false;
    }

    if (!ped->m_pWeaponObject) {
        const int wtStrict = GetPedCurrentWeaponType(ped);
        const int wtRel = GetPedSelectedWeaponTypeForReplace(ped);
        const int mid = ped->m_nWeaponModelId;
        const int wtMid = WeaponTypeFromModelId(mid);
        const int wtRes = OrcResolveWeaponHeldVisualWeaponType(ped);
        OrcLogInfoThrottled(21, 1200u,
            "held wr: m_pWeaponObject=null pedRef=%d wt=%d wtRel=%d mid=%d wtMid=%d wtRes=%d",
            pedRefEarly, wtStrict, wtRel, mid, wtMid, wtRes);
        // After-draw on bone (same as hide-base render); no stock clump to copy pose from.
        state.originalObject = nullptr;
        state.hideBaseMode = true;
        state.captureActive = true;
        OrcLogInfoThrottled(30, 5000u,
            "held wr: empty held slot -> after-draw pedRef=%d wt=%d key=%s",
            pedRefEarly, wt, asset->key.c_str());
        return;
    }

    // Do not assign stock weapon renderCallBack onto the clone: CVisibilityPlugins::RenderWeaponCB
    // expects weapon visibility-plugin data on each RpAtomic; replacement clones don't have it → AV (+0x18).
    // InitAttachmentAtomicCB clears callbacks so RpClumpRender uses the default atomic path.
    CopyRwObjectRootMatrix(ped->m_pWeaponObject, state.rwObject);
    SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
    state.originalObject = ped->m_pWeaponObject;
    state.hideBaseMode = false;
    state.captureActive = true;
    ped->m_pWeaponObject = state.rwObject;
}

void OrcRestoreHeldWeaponReplacementAfter(CPed* ped) {
    if (!ped)
        return;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end() || !it->second.captureActive)
        return;

    HeldWeaponReplacementState& state = it->second;
    if (state.hideBaseMode && state.rwObject)
        RenderHeldReplacementHideBaseForPed(ped, state);

    RwObject* stock = state.originalObject;
    state.originalObject = nullptr;
    state.captureActive = false;
    state.hideBaseMode = false;

    if (pedRef > 0 && stock && state.rwObject) {
        DeferredHeldWeaponSlotRestore defer{};
        defer.stock = stock;
        defer.clone = state.rwObject;
        g_deferredHeldWeaponStockRestore[pedRef] = defer;
    }
}

static bool ModelIdMatchesWeaponType(int modelId, int wt) {
    if (!modelId || wt <= 0)
        return false;
    for (int skill = 1; skill <= 2; ++skill) {
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), skill);
        if (!wi)
            continue;
        if (wi->m_nModelId == modelId || wi->m_nModelId2 == modelId)
            return true;
    }
    return false;
}

// Runs immediately after the engine attached `m_pWeaponObject` (stock) so replacement wins over late rebuilds of the slot.
static void OrcApplyHeldWeaponReplacementAfterAddWeaponModel(CPed* ped, int modelIndex) {
    if (!ped || modelIndex <= 0)
        return;
    if (!ShouldReplaceHeldWeaponForPed(ped))
        return;

    int wt = WeaponTypeFromModelId(modelIndex);
    if (wt <= 0)
        wt = OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wt <= 0)
        return;

    WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, wt, true);
    if (!asset)
        return;

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;

    HeldWeaponReplacementState& state = g_heldWeaponReplacements[pedRef];
    if (!state.rwObject || state.weaponType != wt || state.replacementKey != asset->key) {
        OrcDestroyRwObjectInstance(state.rwObject);
        state.rwObject = nullptr;
        state.originalObject = nullptr;
        state.captureActive = false;
        state.hideBaseMode = false;
        state.rwObject = OrcCloneWeaponReplacementObject(*asset);
        state.weaponType = wt;
        state.replacementKey = asset->key;
        if (!state.rwObject)
            return;
    }

    if (!ped->m_pWeaponObject) {
        state.originalObject = nullptr;
        state.hideBaseMode = true;
        state.captureActive = true;
        return;
    }

    CopyRwObjectRootMatrix(ped->m_pWeaponObject, state.rwObject);
    SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
    state.originalObject = ped->m_pWeaponObject;
    state.hideBaseMode = false;
    state.captureActive = true;
    ped->m_pWeaponObject = state.rwObject;
}

// `__thiscall` cannot be used on static free functions in MSVC; `__fastcall` matches the register ABI
// (this in ECX) and matches the pattern used in `samp_bridge.cpp` for thiscall detours.
static void __fastcall AddWeaponModel_Hook(CPed* ped, void* /*edx*/, int modelIndex) {
    OrcRestoreDeferredHeldStockIfSlotStillHasClone(ped);
    if (g_AddWeaponModel_Orig)
        g_AddWeaponModel_Orig(ped, modelIndex);
    __try {
        OrcApplyHeldWeaponReplacementAfterAddWeaponModel(ped, modelIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("AddWeaponModel hook: SEH ex=0x%08X", GetExceptionCode());
    }
}

static void __fastcall RemoveWeaponModel_Hook(CPed* ped, void* /*edx*/, int modelIndex) {
    if (!ped) {
        if (g_RemoveWeaponModel_Orig)
            g_RemoveWeaponModel_Orig(ped, modelIndex);
        return;
    }
    OrcRestoreDeferredHeldStockIfSlotStillHasClone(ped);
    const int pedRef = CPools::GetPedRef(ped);
    auto it = (pedRef > 0) ? g_heldWeaponReplacements.find(pedRef) : g_heldWeaponReplacements.end();
    if (it != g_heldWeaponReplacements.end()) {
        HeldWeaponReplacementState& st = it->second;
        if (st.captureActive && st.weaponType > 0 && ModelIdMatchesWeaponType(modelIndex, st.weaponType)) {
            if (!st.hideBaseMode && st.rwObject && ped->m_pWeaponObject == st.rwObject) {
                RwObject* stock = st.originalObject;
                if (!stock && pedRef > 0) {
                    auto d = g_deferredHeldWeaponStockRestore.find(pedRef);
                    if (d != g_deferredHeldWeaponStockRestore.end())
                        stock = d->second.stock;
                }
                if (stock)
                    ped->m_pWeaponObject = stock;
            }
        }
    }

    if (g_RemoveWeaponModel_Orig)
        g_RemoveWeaponModel_Orig(ped, modelIndex);

    it = (pedRef > 0) ? g_heldWeaponReplacements.find(pedRef) : g_heldWeaponReplacements.end();
    if (it == g_heldWeaponReplacements.end())
        return;
    HeldWeaponReplacementState& st = it->second;
    if (!st.captureActive || !ModelIdMatchesWeaponType(modelIndex, st.weaponType))
        return;

    if (pedRef > 0)
        g_deferredHeldWeaponStockRestore.erase(pedRef);
    st.originalObject = nullptr;
    OrcDestroyRwObjectInstance(st.rwObject);
    st.rwObject = nullptr;
    st.captureActive = false;
    st.hideBaseMode = false;
    st.weaponType = 0;
    st.replacementKey.clear();
}

void OrcWeaponEnsurePedModelHooksInstalled() {
    if (g_pedWeaponModelHooksInstalled)
        return;
    g_pedWeaponModelHooksInstalled = true;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("PedWeaponModel hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(0x5E5ED0),
            reinterpret_cast<void*>(&AddWeaponModel_Hook),
            reinterpret_cast<void**>(&g_AddWeaponModel_Orig)) != MH_OK) {
        OrcLogError("AddWeaponModel hook: MH_CreateHook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(0x5E3990),
            reinterpret_cast<void*>(&RemoveWeaponModel_Hook),
            reinterpret_cast<void**>(&g_RemoveWeaponModel_Orig)) != MH_OK) {
        OrcLogError("RemoveWeaponModel hook: MH_CreateHook failed");
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(0x5E5ED0));
    if (st != MH_OK)
        OrcLogError("AddWeaponModel hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("AddWeaponModel hook installed (0x5E5ED0)");
    st = MH_EnableHook(reinterpret_cast<void*>(0x5E3990));
    if (st != MH_OK)
        OrcLogError("RemoveWeaponModel hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("RemoveWeaponModel hook installed (0x5E3990)");
}

void OrcClearAllWeaponReplacementInstances() {
    OrcWeaponClearLocalRendered();
    OrcWeaponClearOtherPedsRendered();
    OrcDestroyAllHeldWeaponReplacementInstances();
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

// ---------------------------------------------------------------------------
// HUD weapon icon (local ped): `CHud::DrawAmmo` / `DrawWeaponIcon` push Orc TXD scope; `CSprite2d::SetTexture` (+mask)
// resolves `*icon` from that dictionary. `OrcWeaponHudTryRwTexDictionaryFindOverride` + multi-raster borrow across TXD
// slots in `orc_weapon_assets.cpp` covers SA:MP clients that cache sprites / split icons across dictionaries.
// ---------------------------------------------------------------------------

static bool g_drawWeaponIconHookInstalled = false;
static bool g_hudSprTexMonoHookInstalled = false;
static bool g_hudSprTexMaskHookInstalled = false;
static void(__cdecl* g_CHud_DrawWeaponIcon_Orig)(CPed*, int, int, float) = nullptr;
using Sprite2dSetTexture_orig_fn = void(__thiscall*)(CSprite2d*, char*);
static Sprite2dSetTexture_orig_fn g_CSprite2d_SetTexture_Orig = nullptr;
using Sprite2dSetTextureMasked_orig_fn = void(__thiscall*)(CSprite2d*, char*, char*);
static Sprite2dSetTextureMasked_orig_fn g_CSprite2d_SetTextureMasked_Orig = nullptr;

// Non-null while inside `DrawAmmo` / `DrawWeaponIcon` when Orc Guns/replacement dict resolves for local ped.
static RwTexDictionary* g_hudWeaponOrcTexDictDuringDrawWeaponIcon = nullptr;
static RwTexDictionary* g_hudOrcSpriteScopeStack[8];
static unsigned g_hudOrcSpriteScopeStackSz = 0;

static void HudPushOrcSpriteDictScope(RwTexDictionary* d) {
    if (!d || g_hudOrcSpriteScopeStackSz >= 8u)
        return;
    g_hudOrcSpriteScopeStack[g_hudOrcSpriteScopeStackSz++] = d;
    g_hudWeaponOrcTexDictDuringDrawWeaponIcon = d;
}

static void HudPopOrcSpriteDictScope() {
    if (!g_hudOrcSpriteScopeStackSz)
        return;
    --g_hudOrcSpriteScopeStackSz;
    g_hudWeaponOrcTexDictDuringDrawWeaponIcon =
        g_hudOrcSpriteScopeStackSz ? g_hudOrcSpriteScopeStack[g_hudOrcSpriteScopeStackSz - 1u] : nullptr;
}

// `__thiscall` hook entry must use `__fastcall` trampoline (this in ECX); see AddWeaponModel_Hook above.
static void __fastcall CSprite2d_SetTexture_HudWeaponOverlay(CSprite2d* self, void* /*edx*/, char* name) {
    if (!g_CSprite2d_SetTexture_Orig)
        return;
    if (!name || !g_weaponHudIconFromGunsTxd || !g_enabled ||
        !OrcWeaponHudSpriteNamePassesSetTextureConvention(name)) {
        g_CSprite2d_SetTexture_Orig(self, name);
        return;
    }
    RwTexDictionary* cacheBefore = OrcWeaponHudGetSampSpriteInterceptDict();
    OrcLogInfoThrottled(
        431,
        4000u,
        "hud icon: SetTexture *icon name=\"%s\" ctxDict=%p cacheDict=%p",
        name,
        g_hudWeaponOrcTexDictDuringDrawWeaponIcon,
        cacheBefore);
    RwTexDictionary* orcDict = g_hudWeaponOrcTexDictDuringDrawWeaponIcon;
    if (!orcDict)
        orcDict = cacheBefore;
    // SA:MP can render the weapon slot before `drawingEvent` fills the intercept cache — build lazily once.
    if (!orcDict) {
        OrcWeaponHudRefreshSampSpriteInterceptCache();
        orcDict = OrcWeaponHudGetSampSpriteInterceptDict();
    }
    if (!orcDict) {
        OrcLogInfoThrottled(
            433,
            6000u,
            "hud icon: SetTexture \"%s\": no Orc dict after lazy refresh — client may not use this hook for ammo icon",
            name);
        g_CSprite2d_SetTexture_Orig(self, name);
        return;
    }
    __try {
        RwTexture* t = OrcWeaponHudResolveSpriteTexture(orcDict, name);
        if (!t) {
            const char* rf = OrcWeaponHudGetIconSpriteRemapFrom();
            const char* rt = OrcWeaponHudGetIconSpriteRemapTo();
            if (rf && rt && _stricmp(name, rf) == 0)
                t = OrcWeaponHudResolveSpriteTexture(orcDict, rt);
        }
        if (t) {
            self->m_pTexture = t;
            OrcLogInfoThrottled(
                404, 45000u, "hud weapon icon: sprite from Orc TXD (SetTexture \"%s\")", name);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    OrcLogInfoThrottled(
        434,
        6000u,
        "hud icon: SetTexture \"%s\": texture not in Orc dict=%p remap %s->%s (falling back to vanilla SetTexture)",
        name,
        orcDict,
        OrcWeaponHudGetIconSpriteRemapFrom() ? OrcWeaponHudGetIconSpriteRemapFrom() : "-",
        OrcWeaponHudGetIconSpriteRemapTo() ? OrcWeaponHudGetIconSpriteRemapTo() : "-");
    g_CSprite2d_SetTexture_Orig(self, name);
}

static void __fastcall CSprite2d_SetTextureMasked_HudWeaponOverlay(CSprite2d* self,
    void* /*edx*/,
    char* name,
    char* maskName) {
    if (!g_CSprite2d_SetTextureMasked_Orig)
        return;
    if (!name || !g_weaponHudIconFromGunsTxd || !g_enabled ||
        !OrcWeaponHudSpriteNamePassesSetTextureConvention(name)) {
        g_CSprite2d_SetTextureMasked_Orig(self, name, maskName);
        return;
    }
    OrcLogInfoThrottled(
        435,
        4000u,
        "hud icon: SetTexture(masked) *icon rgb=\"%s\" ctxDict=%p cacheDict=%p",
        name,
        g_hudWeaponOrcTexDictDuringDrawWeaponIcon,
        OrcWeaponHudGetSampSpriteInterceptDict());
    RwTexDictionary* orcDict = g_hudWeaponOrcTexDictDuringDrawWeaponIcon;
    if (!orcDict)
        orcDict = OrcWeaponHudGetSampSpriteInterceptDict();
    if (!orcDict) {
        OrcWeaponHudRefreshSampSpriteInterceptCache();
        orcDict = OrcWeaponHudGetSampSpriteInterceptDict();
    }
    if (!orcDict) {
        OrcLogInfoThrottled(
            436,
            6000u,
            "hud icon: SetTexture(mask) \"%s\": no Orc dict after lazy refresh", name);
        g_CSprite2d_SetTextureMasked_Orig(self, name, maskName);
        return;
    }
    __try {
        RwTexture* t = OrcWeaponHudResolveSpriteTexture(orcDict, name);
        if (!t) {
            const char* rf = OrcWeaponHudGetIconSpriteRemapFrom();
            const char* rt = OrcWeaponHudGetIconSpriteRemapTo();
            if (rf && rt && _stricmp(name, rf) == 0)
                t = OrcWeaponHudResolveSpriteTexture(orcDict, rt);
        }
        if (t) {
            self->m_pTexture = t;
            (void)maskName;
            OrcLogInfoThrottled(
                405, 45000u, "hud weapon icon: Orc TXD (SetTexture masked rgb \"%s\")", name);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    OrcLogInfoThrottled(
        442,
        6000u,
        "hud icon: SetTexture(mask) \"%s\": miss Orc dict=%p remap %s->%s",
        name,
        orcDict,
        OrcWeaponHudGetIconSpriteRemapFrom() ? OrcWeaponHudGetIconSpriteRemapFrom() : "-",
        OrcWeaponHudGetIconSpriteRemapTo() ? OrcWeaponHudGetIconSpriteRemapTo() : "-");
    g_CSprite2d_SetTextureMasked_Orig(self, name, maskName);
}

static void __cdecl CHud_DrawWeaponIcon_Detour(CPed* ped, int x, int y, float alpha) {
    if (!g_CHud_DrawWeaponIcon_Orig)
        return;

    CPed* tryPed = ped;
    if (!tryPed) {
        CPlayerPed* lp = FindPlayerPed(0);
        tryPed = lp ? static_cast<CPed*>(lp) : nullptr;
    }

    int txdSlot = -1;
    RwTexDictionary* orcSpriteDict = nullptr;
    OrcWeaponHudTryGetIconOverrideTxdSlot(ped, &txdSlot);
    if (txdSlot >= 0)
        orcSpriteDict = OrcWeaponStreamingGetRwTxdDictionary(txdSlot);

    CPlayerPed* localPl = FindPlayerPed(0);
    const int refIn = ped ? CPools::GetPedRef(ped) : 0;
    const int refTry = tryPed ? CPools::GetPedRef(tryPed) : 0;
    const int refLoc = localPl ? CPools::GetPedRef(localPl) : 0;
    int hudLastWt = 0;
    __try {
        volatile const int* pLast = reinterpret_cast<const volatile int*>(0xBAA410);
        hudLastWt = *pLast;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int visualWt = tryPed ? OrcResolveWeaponHeldVisualWeaponType(tryPed) : 0;
    const int heldReplWt = tryPed ? OrcWeaponHudGetHeldReplacementWeaponTypeIfAny(tryPed) : 0;
    OrcLogInfoThrottled(
        407,
        15000u,
        "hud icon: DrawWeaponIcon ped=%p tryPed=%p local=%p ref(in/try/local)=%d/%d/%d slot=%d orcDict=%p "
        "visualWt=%d heldReplWt=%d hudLastWt=%d",
        ped,
        tryPed,
        localPl,
        refIn,
        refTry,
        refLoc,
        txdSlot,
        orcSpriteDict,
        visualWt,
        heldReplWt,
        hudLastWt);

    // `DrawAmmo` / `DrawWeaponIcon` paths re-bind hud.txd and call `CSprite2d::SetTexture`; keep Orc dict scoped for
    // the whole ammo draw — SA:MP R1 sometimes never hits `DrawWeaponIcon` or binds textures earlier in `DrawAmmo`.
    HudPushOrcSpriteDictScope(orcSpriteDict);
    g_CHud_DrawWeaponIcon_Orig(ped, x, y, alpha);
    HudPopOrcSpriteDictScope();
}

static bool g_drawAmmoHookInstalled = false;
static void(__cdecl* g_CHud_DrawAmmo_Orig)(CPed*, int, int, float) = nullptr;

static void __cdecl CHud_DrawAmmo_Detour(CPed* ped, int x, int y, float alpha) {
    if (!g_CHud_DrawAmmo_Orig)
        return;

    int txdSlot = -1;
    RwTexDictionary* orcSpriteDict = nullptr;
    OrcWeaponHudTryGetIconOverrideTxdSlot(ped, &txdSlot);
    if (txdSlot >= 0)
        orcSpriteDict = OrcWeaponStreamingGetRwTxdDictionary(txdSlot);

    OrcLogInfoThrottled(
        470,
        15000u,
        "hud icon: DrawAmmo ped=%p slot=%d orcDict=%p",
        ped,
        txdSlot,
        orcSpriteDict);

    HudPushOrcSpriteDictScope(orcSpriteDict);
    g_CHud_DrawAmmo_Orig(ped, x, y, alpha);
    HudPopOrcSpriteDictScope();
}

void OrcWeaponHudEnsureDrawWeaponIconHookInstalled() {
    if (g_drawWeaponIconHookInstalled && g_drawAmmoHookInstalled && g_hudSprTexMonoHookInstalled &&
        g_hudSprTexMaskHookInstalled)
        return;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("HUD weapon icon hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }

    if (!g_hudSprTexMonoHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x727270),
                reinterpret_cast<void*>(&CSprite2d_SetTexture_HudWeaponOverlay),
                reinterpret_cast<void**>(&g_CSprite2d_SetTexture_Orig)) != MH_OK) {
            OrcLogError("CSprite2d::SetTexture HUD hook: MH_CreateHook failed (0x727270)");
            return;
        }
        st = MH_EnableHook(reinterpret_cast<void*>(0x727270));
        if (st != MH_OK) {
            OrcLogError("CSprite2d::SetTexture HUD hook: MH_EnableHook -> %s", MH_StatusToString(st));
            return;
        }
        g_hudSprTexMonoHookInstalled = true;
        OrcLogInfo("CSprite2d::SetTexture hooked (0x727270) for Hud weapon sprite (Orc dictionary)");
    }

    if (!g_hudSprTexMaskHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x7272B0),
                reinterpret_cast<void*>(&CSprite2d_SetTextureMasked_HudWeaponOverlay),
                reinterpret_cast<void**>(&g_CSprite2d_SetTextureMasked_Orig)) != MH_OK) {
            OrcLogError("CSprite2d::SetTexture(mask) HUD hook: MH_CreateHook failed (0x7272B0)");
        } else {
            st = MH_EnableHook(reinterpret_cast<void*>(0x7272B0));
            if (st != MH_OK)
                OrcLogError(
                    "CSprite2d::SetTexture(mask) HUD hook: MH_EnableHook -> %s", MH_StatusToString(st));
            else {
                g_hudSprTexMaskHookInstalled = true;
                OrcLogInfo("CSprite2d::SetTexture mask hooked (0x7272B0) for Hud weapon sprite");
            }
        }
    }

    if (!g_drawWeaponIconHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x58D7D0),
                reinterpret_cast<void*>(&CHud_DrawWeaponIcon_Detour),
                reinterpret_cast<void**>(&g_CHud_DrawWeaponIcon_Orig)) != MH_OK) {
            OrcLogError("DrawWeaponIcon hook: MH_CreateHook failed");
            return;
        }
        st = MH_EnableHook(reinterpret_cast<void*>(0x58D7D0));
        if (st != MH_OK) {
            OrcLogError("DrawWeaponIcon hook: MH_EnableHook -> %s", MH_StatusToString(st));
            return;
        }
        g_drawWeaponIconHookInstalled = true;
        OrcLogInfo("CHud::DrawWeaponIcon hooked (0x58D7D0) for Guns/replacement HUD icon context");
    }

    if (!g_drawAmmoHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x5893B0),
                reinterpret_cast<void*>(&CHud_DrawAmmo_Detour),
                reinterpret_cast<void**>(&g_CHud_DrawAmmo_Orig)) != MH_OK) {
            OrcLogError("DrawAmmo hook: MH_CreateHook failed");
            return;
        }
        st = MH_EnableHook(reinterpret_cast<void*>(0x5893B0));
        if (st != MH_OK) {
            OrcLogError("DrawAmmo hook: MH_EnableHook -> %s", MH_StatusToString(st));
            return;
        }
        g_drawAmmoHookInstalled = true;
        OrcLogInfo("CHud::DrawAmmo hooked (0x5893B0) for Guns/replacement HUD icon context");
    }
}


