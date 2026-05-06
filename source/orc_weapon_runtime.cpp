#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CHud.h"
#include "CSprite2d.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "CTimer.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include <algorithm>
#include <cmath>
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

// alphaOverride < 0: use ped gunflash MP1 (held weapon). Else explicit alpha for holstered body clones (0 = hide flash).
static void SyncWeaponReplacementMuzzleFlashAlpha(CPed* ped, RwObject* obj, int alphaOverride = -1);

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
        SyncWeaponReplacementMuzzleFlashAlpha(nullptr, r.rwObject, 0);

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
// Holster/body clones (RenderOneWeapon) must force alpha 0 — MP1 reflects the held gun, not the carried one.
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

static void SyncWeaponReplacementMuzzleFlashAlpha(CPed* ped, RwObject* obj, int alphaOverride) {
    if (!obj)
        return;
    int a;
    if (alphaOverride >= 0)
        a = alphaOverride;
    else {
        if (!ped)
            return;
        a = static_cast<int>(ped->m_nWeaponGunflashAlphaMP1);
    }
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

/// Слот `m_pWeaponObject` или клон замены, если SA:MP/фаза рендера обнулила слот, но меш всё ещё в `g_heldWeaponReplacements`.
static RwObject* OrcResolveHeldWeaponRwObject(CPed* ped) {
    if (!ped)
        return nullptr;
    if (ped->m_pWeaponObject)
        return ped->m_pWeaponObject;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return nullptr;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end() || !it->second.rwObject)
        return nullptr;
    return it->second.rwObject;
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

static bool OrcRwMatrixFinite(const RwMatrix* mat) {
    if (!mat) return false;
    const float v[12] = { mat->right.x, mat->right.y, mat->right.z, mat->up.x,    mat->up.y,    mat->up.z,
                          mat->at.x,    mat->at.y,    mat->at.z,    mat->pos.x,   mat->pos.y,   mat->pos.z };
    for (float f : v) {
        if (!std::isfinite(f)) return false;
    }
    return true;
}

static bool OrcRwMatrixAxesNonDegenerate(const RwMatrix* mat) {
    if (!mat) return false;
    auto len2 = [](float x, float y, float z) { return x * x + y * y + z * z; };
    return len2(mat->right.x, mat->right.y, mat->right.z) > 1e-12f && len2(mat->up.x, mat->up.y, mat->up.z) > 1e-12f &&
           len2(mat->at.x, mat->at.y, mat->at.z) > 1e-12f;
}

/// Жёсткое тело: ортонормальный базис (right, up, at), позиция не трогается. Снижает разнос от IK/иерархии перед HeldRotate/Scale.
static bool OrcRwMatrixRenormalizeRigidBasis(RwMatrix* m) {
    if (!m) return false;
    RwV3d& r = m->right;
    RwV3d& u = m->up;
    RwV3d& a = m->at;
    auto normalize = [](RwV3d& v) -> bool {
        const float L2 = v.x * v.x + v.y * v.y + v.z * v.z;
        if (L2 < 1e-16f) return false;
        const float inv = 1.0f / sqrtf(L2);
        v.x *= inv;
        v.y *= inv;
        v.z *= inv;
        return true;
    };
    if (!normalize(r)) return false;
    const RwV3d u0 = u;
    a.x = r.y * u0.z - r.z * u0.y;
    a.y = r.z * u0.x - r.x * u0.z;
    a.z = r.x * u0.y - r.y * u0.x;
    if (!normalize(a)) return false;
    u.x = a.y * r.z - a.z * r.y;
    u.y = a.z * r.x - a.x * r.z;
    u.z = a.x * r.y - a.y * r.x;
    return normalize(u);
}

// База движка на RwFrame за игровой тик: `pedRenderEvent.before` срабатывает дважды на кадр — без этого Held
// накладывался повторно на уже изменённую матрицу (см. лог: два repl:firstSwap, скачок at=).
static std::unordered_map<uintptr_t, RwMatrix> s_heldPoseEngineBaselineByFrame;

static RpAtomic* OrcHeldPoseInvalidateAtomicBaselineCb(RpAtomic* atomic, void*) {
    RwFrame* f = RpAtomicGetFrame(atomic);
    if (f)
        s_heldPoseEngineBaselineByFrame.erase(reinterpret_cast<uintptr_t>(f));
    return atomic;
}

static void OrcHeldPoseInvalidateBaselineForRwFrame(RwFrame* f) {
    if (f)
        s_heldPoseEngineBaselineByFrame.erase(reinterpret_cast<uintptr_t>(f));
}

static void OrcHeldPoseInvalidateBaselineForRwObject(RwObject* obj) {
    if (!obj)
        return;
    if (obj->type == rpATOMIC) {
        OrcHeldPoseInvalidateAtomicBaselineCb(reinterpret_cast<RpAtomic*>(obj), nullptr);
        return;
    }
    if (obj->type == rpCLUMP) {
        RpClump* c = reinterpret_cast<RpClump*>(obj);
        RwFrame* root = RpClumpGetFrame(c);
        OrcHeldPoseInvalidateBaselineForRwFrame(root);
        RpClumpForAllAtomics(c, OrcHeldPoseInvalidateAtomicBaselineCb, nullptr);
    }
}

static bool OrcTryApplyHeldPoseOneFrame(RwFrame* frame, const HeldWeaponPoseCfg& h) {
    RwMatrix* m = RwFrameGetMatrix(frame);
    if (!m || !OrcRwMatrixFinite(m) || !OrcRwMatrixAxesNonDegenerate(m))
        return false;
    const uintptr_t fk = reinterpret_cast<uintptr_t>(frame);
    RwMatrix base{};
    auto bit = s_heldPoseEngineBaselineByFrame.find(fk);
    if (bit == s_heldPoseEngineBaselineByFrame.end()) {
        base = *m;
        if (!OrcRwMatrixFinite(&base) || !OrcRwMatrixAxesNonDegenerate(&base))
            return false;
        s_heldPoseEngineBaselineByFrame.emplace(fk, base);
    } else {
        base = bit->second;
    }
    __try {
        RwMatrix work = base;
        if (!OrcRwMatrixRenormalizeRigidBasis(&work))
            return false;
        OrcApplyAttachmentOffset(&work, h.x, h.y, h.z);
        OrcRotateAttachmentMatrix(&work, h.rx, h.ry, h.rz);
        if (h.scale > 0.0f && h.scale != 1.0f) {
            RwV3d s = { h.scale, h.scale, h.scale };
            RwMatrixScale(&work, &s, rwCOMBINEPRECONCAT);
        }
        if (!OrcRwMatrixFinite(&work))
            return false;
        *m = work;
        RwMatrixUpdate(m);
        RwFrameUpdateObjects(frame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcTryApplyHeldPoseOneFrame: SEH ex=0x%08X", GetExceptionCode());
        return false;
    }
}

struct HeldClumpApplyCtx {
    const HeldWeaponPoseCfg* h = nullptr;
    int nAtom = 0;
    int nOk = 0;
    float logAtX = 0.0f;
    float logAtY = 0.0f;
    float logAtZ = 0.0f;
};

static RpAtomic* OrcHeldPoseApplyEachAtomicCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<HeldClumpApplyCtx*>(data);
    ctx->nAtom++;
    RwFrame* f = RpAtomicGetFrame(atomic);
    if (!f || !ctx->h)
        return atomic;
    if (OrcTryApplyHeldPoseOneFrame(f, *ctx->h)) {
        ctx->nOk++;
        RwMatrix* mm = RwFrameGetMatrix(f);
        if (mm) {
            ctx->logAtX = mm->pos.x;
            ctx->logAtY = mm->pos.y;
            ctx->logAtZ = mm->pos.z;
        }
    }
    return atomic;
}

/// Единая точка математики «В руке»: `phase` в логе различает **repl:sync** (после копии IK→клон) и **vanillaPC**.
/// `wtOverride >= 0` — тип оружия для пресета Held (иначе `OrcResolveWeaponHeldVisualWeaponType`).
static bool OrcApplyHeldPoseToWeaponObject(CPed* ped, RwObject* obj, const char* phase, int wtOverride = -1) {
    if (!g_enabled || !ped || !obj || !phase)
        return false;
    const int pedRefEarly = CPools::GetPedRef(ped);
    const int wt = (wtOverride >= 0) ? wtOverride : OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wt <= 0) {
        OrcLogInfoThrottled(
            433, g_heldPoseDebug ? 600u : 2200u,
            "held pose: skip wt<=0 phase=%s mid=%d slot=%d savedWt=%d pedRef=%d", phase, ped->m_nWeaponModelId,
            (int)ped->m_nSelectedWepSlot, (int)ped->m_nSavedWeapon, pedRefEarly);
        return false;
    }
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    const bool isReplClone = HeldWeaponRwObjectIsReplacementClone(ped, obj);
    if (g_heldPoseDebug) {
        const bool fromRepl = (ped->m_pWeaponObject != obj);
        OrcLogInfoThrottled(
            465 + wt % 7, 400u,
            "held chain: cfg phase=%s pedRef=%d wt=%d heldEn=%d obj=%p objT=%d meshRepl=%d resolve=%s", phase,
            pedRefEarly, wt, h.enabled ? 1 : 0, obj, (int)obj->type, isReplClone ? 1 : 0, fromRepl ? "replBuf" : "slot");
    }
    if (!h.enabled) {
        OrcLogHeldPoseCfgDisabled(ped, wt);
        if (g_heldPoseDebug) {
            OrcLogInfoThrottled(476, 600u,
                "held pose: cfg off phase=%s wt=%d (enable Held* in Weapons\\<skin>.ini)", phase ? phase : "?", wt);
        }
        return false;
    }
    int heldPoseAtomCount = 0;
    int heldPoseOkCount = 0;
    const char* heldPoseTgt = "atomic";
    float logAtX = 0.0f;
    float logAtY = 0.0f;
    float logAtZ = 0.0f;

    if (obj->type == rpATOMIC) {
        RwFrame* frame = RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(obj));
        if (!frame) {
            OrcLogInfoThrottled(434, 3000u, "held pose: skip no RwFrame phase=%s obj=%p type=%d wt=%d", phase, obj,
                (int)obj->type, wt);
            return false;
        }
        if (!OrcTryApplyHeldPoseOneFrame(frame, h)) {
            OrcLogInfoThrottled(442, 2500u, "held pose: skip apply failed (atomic) phase=%s wt=%d", phase, wt);
            return false;
        }
        heldPoseAtomCount = 1;
        heldPoseOkCount = 1;
        RwMatrix* m = RwFrameGetMatrix(frame);
        if (m) {
            logAtX = m->pos.x;
            logAtY = m->pos.y;
            logAtZ = m->pos.z;
        }
    } else if (obj->type == rpCLUMP) {
        RpClump* clump = reinterpret_cast<RpClump*>(obj);
        HeldClumpApplyCtx ctx{};
        ctx.h = &h;
        RpClumpForAllAtomics(clump, OrcHeldPoseApplyEachAtomicCb, &ctx);
        heldPoseAtomCount = ctx.nAtom;
        heldPoseOkCount = ctx.nOk;
        logAtX = ctx.logAtX;
        logAtY = ctx.logAtY;
        logAtZ = ctx.logAtZ;
        if (ctx.nOk == 0) {
            RwFrame* rootF = RpClumpGetFrame(clump);
            if (rootF && OrcTryApplyHeldPoseOneFrame(rootF, h)) {
                heldPoseOkCount = 1;
                heldPoseTgt = "root_fallback";
                RwMatrix* rm = RwFrameGetMatrix(rootF);
                if (rm) {
                    logAtX = rm->pos.x;
                    logAtY = rm->pos.y;
                    logAtZ = rm->pos.z;
                }
            } else {
                OrcLogInfoThrottled(443, 2500u, "held pose: skip no frame applied phase=%s wt=%d atomics=%d", phase, wt,
                    ctx.nAtom);
                return false;
            }
        } else {
            heldPoseTgt = "eachAtomic";
        }
    } else {
        OrcLogInfoThrottled(434, 3000u, "held pose: skip unknown obj type phase=%s t=%d wt=%d", phase, (int)obj->type, wt);
        return false;
    }

    if (g_heldPoseDebug) {
        OrcLogInfoThrottled(
            437, 450u,
            "held pose: apply phase=%s wt=%d mesh=%s objT=%d atomics=%d applied=%d tgt=%s xyz=%.3f %.3f %.3f deg=%.2f %.2f %.2f sc=%.3f | at=%.3f %.3f %.3f",
            phase, wt, isReplClone ? "repl" : "vanilla", (int)obj->type, heldPoseAtomCount, heldPoseOkCount, heldPoseTgt,
            h.x, h.y, h.z, h.rx * (180.0f / kOrcPi), h.ry * (180.0f / kOrcPi), h.rz * (180.0f / kOrcPi),
            h.scale, logAtX, logAtY, logAtZ);
    }
    return true;
}

// --- Ванильное смещение «В руке»: сразу перед отрисовкой RW (после того как движок выставил IK/матрицу оружия).
// `plugin_sa/game_sa/RenderWare.cpp`: RpClumpRender @ 0x749B20, AtomicDefaultRenderCallBack @ 0x7491C0 (GTA SA 1.0 US).
// `plugin_sa/game_sa/CVisibilityPlugins.cpp`: RenderWeaponCB @ 0x733670 — фактический колбэк отрисовки оружия у PC-педов
// (атомы клумпа часто не проходят через AtomicDefaultRenderCallBack / полный RpClumpRender того же указателя).
// До этого пробовали `pedRenderEvent` (два CALL / один CALL) и `RenderWeaponPedsForPC` — матрица могла
// пересчитываться позже или проход не вызывался; pre-draw совпадает с тем, что уходит в GPU.
// Клоны Guns: Held по-прежнему в `repl:*` после CopyRwObjectRootMatrix; здесь пропускаем repl-clone (избежать двойной дельты).
// Пресет: `GetHeldPoseForPed` → `Weapons\<skin>.ini`.
static constexpr uintptr_t kAddr_RpClumpRender = 0x749B20;
static constexpr uintptr_t kAddr_AtomicDefaultRenderCallBack = 0x7491C0;
static constexpr uintptr_t kAddr_RenderWeaponPedsForPC = 0x732F30;
static constexpr uintptr_t kAddr_RenderWeaponCB = 0x733670;
using RpClumpRender_fn = RpClump*(__cdecl*)(RpClump*);
using AtomicDefaultRender_fn = RpAtomic*(__cdecl*)(RpAtomic*);
using RenderWeaponPedsForPC_fn = void(__cdecl*)();
using RenderWeaponCB_fn = void(__cdecl*)(RpAtomic*);
static RpClumpRender_fn g_RpClumpRender_Orig = nullptr;
static AtomicDefaultRender_fn g_AtomicDefaultRender_Orig = nullptr;
static RenderWeaponPedsForPC_fn g_RenderWeaponPedsForPC_Orig = nullptr;
static RenderWeaponCB_fn g_RenderWeaponCB_Orig = nullptr;

static uint64_t s_heldRwpfcBatchCounter = 0;

/// Один раз за игровой тик на педа: несколько вызовов RpClumpRender за кадр (тени/зеркала) не накапливают дельту.
static std::unordered_set<int> s_heldPreRwDrawAppliedPedRefs;

static RwFrame* OrcRwFrameGetParent(RwFrame* f) {
    if (!f)
        return nullptr;
    return reinterpret_cast<RwFrame*>(plugin::GetObjectParent(reinterpret_cast<RwObject*>(f)));
}

static bool OrcAtomicBelongsToClump(RpAtomic* atomic, RpClump* clump) {
    if (!atomic || !clump)
        return false;
    RwFrame* root = RpClumpGetFrame(clump);
    RwFrame* af = RpAtomicGetFrame(atomic);
    if (!root || !af)
        return false;
    for (RwFrame* x = af; x; x = OrcRwFrameGetParent(x)) {
        if (x == root)
            return true;
    }
    return false;
}

static bool OrcHeldAtomicMatchesPedWeaponObject(RpAtomic* atomic, RwObject* wo) {
    if (!atomic || !wo)
        return false;
    if (wo->type == rpATOMIC)
        return reinterpret_cast<RpAtomic*>(wo) == atomic;
    if (wo->type == rpCLUMP)
        return OrcAtomicBelongsToClump(atomic, reinterpret_cast<RpClump*>(wo));
    return false;
}

static bool OrcRwFrameHasAncestor(RwFrame* f, RwFrame* ancestor) {
    if (!f || !ancestor)
        return false;
    for (RwFrame* x = f; x; x = OrcRwFrameGetParent(x)) {
        if (x == ancestor)
            return true;
    }
    return false;
}

/// Оружие в руке цепляется к кости внутри `ped->m_pRwClump` — отсекаем RWCB на чужих педах при обходе пула.
static bool OrcAtomicAttachedUnderPedRwClump(RpAtomic* atomic, CPed* ped) {
    if (!atomic || !ped || !ped->m_pRwClump)
        return false;
    RwFrame* af = RpAtomicGetFrame(atomic);
    RwFrame* pedRoot = RpClumpGetFrame(ped->m_pRwClump);
    if (!af || !pedRoot)
        return false;
    return OrcRwFrameHasAncestor(af, pedRoot);
}

struct OrcGeomEqCtx {
    RpGeometry* geom = nullptr;
    bool found = false;
};

static RpAtomic* OrcAtomicFindEqualGeometryCb(RpAtomic* a, void* data) {
    auto* ctx = reinterpret_cast<OrcGeomEqCtx*>(data);
    if (a && ctx && ctx->geom && a->geometry == ctx->geom)
        ctx->found = true;
    return a;
}

/// Тот же DFF после `CreateInstance`: другие `RpAtomic*`, общий `RpGeometry` с шаблоном/старым стоком.
static bool OrcAtomicSharesGeometryWithWeaponObject(RpAtomic* atomic, RwObject* wo) {
    if (!atomic || !wo || !atomic->geometry)
        return false;
    if (wo->type == rpATOMIC) {
        RpAtomic* wa = reinterpret_cast<RpAtomic*>(wo);
        return wa->geometry && wa->geometry == atomic->geometry;
    }
    if (wo->type == rpCLUMP) {
        OrcGeomEqCtx ctx{};
        ctx.geom = atomic->geometry;
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(wo), OrcAtomicFindEqualGeometryCb, &ctx);
        return ctx.found;
    }
    return false;
}

static RwObject* OrcHeldGetReplacementStockObject(int pedRef, const HeldWeaponReplacementState& st) {
    if (st.originalObject)
        return st.originalObject;
    if (pedRef <= 0 || !st.rwObject)
        return nullptr;
    auto d = g_deferredHeldWeaponStockRestore.find(pedRef);
    if (d == g_deferredHeldWeaponStockRestore.end() || !d->second.stock || d->second.clone != st.rwObject)
        return nullptr;
    return d->second.stock;
}

static bool OrcHeldAtomicMatchesWeaponModelTemplate(RpAtomic* atomic, int wt) {
    if (!atomic || wt <= 0 || !atomic->geometry)
        return false;
    for (int skill = 1; skill <= 2; ++skill) {
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), skill);
        if (!wi)
            continue;
        const int mids[2] = { wi->m_nModelId, wi->m_nModelId2 };
        for (int mid : mids) {
            if (mid <= 0)
                continue;
            CBaseModelInfo* mi = CModelInfo::GetModelInfo(mid);
            if (!mi || !mi->m_pRwObject)
                continue;
            if (OrcAtomicSharesGeometryWithWeaponObject(atomic, mi->m_pRwObject))
                return true;
        }
    }
    return false;
}

/// RWCB рисует атомы **того инстанса**, что сейчас в visibility-пайплайне (часто ванильный сток при клоне в слоте).
/// Held сюда кладём **только на `RpAtomicGetFrame(atomic)`** этого колбэка — не на корень клумпа в слоте и не на клон
/// (клон уже в `repl:*`), иначе «летит» ваниль увеличенная + клон в руке не виден.
/// `wtSel` / `wo` снаружи (RWCB вызывается очень часто — не дублируем `OrcResolveHeldWeaponRwObject`).
static bool OrcHeldRwcbShouldApplyAtomicImpl(CPed* ped, RpAtomic* atomic, int wtSel, RwObject* wo) {
    if (!ped || !atomic || wtSel <= 0)
        return false;

    // Прямой матч / сток repl / геометрия слота — без `OrcAtomicAttachedUnderPedRwClump`: у RWCB атомов
    // иерархия `RwFrame` часто не поднимается до `RpClumpGetFrame(ped->m_pRwClump)` (оружие на кости вне этого
    // обхода родителей), иначе match всегда 0 и Held не работает.

    if (wo && OrcHeldAtomicMatchesPedWeaponObject(atomic, wo))
        return true;

    if (wo && HeldWeaponRwObjectIsReplacementClone(ped, wo)) {
        const int pr = CPools::GetPedRef(ped);
        if (pr <= 0)
            return false;
        auto it = g_heldWeaponReplacements.find(pr);
        if (it == g_heldWeaponReplacements.end() || it->second.rwObject != wo)
            return false;
        RwObject* stock = OrcHeldGetReplacementStockObject(pr, it->second);
        if (stock) {
            if (OrcHeldAtomicMatchesPedWeaponObject(atomic, stock) || OrcAtomicSharesGeometryWithWeaponObject(atomic, stock))
                return true;
        }
    }

    if (wo && !HeldWeaponRwObjectIsReplacementClone(ped, wo) && OrcAtomicSharesGeometryWithWeaponObject(atomic, wo))
        return true;

    if (!OrcHeldAtomicMatchesWeaponModelTemplate(atomic, wtSel))
        return false;
    return OrcAtomicAttachedUnderPedRwClump(atomic, ped);
}

static bool OrcApplyHeldPoseToRwcbAtomic(CPed* ped, RpAtomic* atomic, int wtSel) {
    if (!g_enabled || !ped || !atomic || wtSel <= 0)
        return false;
    RwFrame* af = RpAtomicGetFrame(atomic);
    if (!af)
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wtSel, false);
    if (g_heldPoseDebug) {
        OrcLogInfoThrottled(
            492 + wtSel % 5, 400u,
            "held chain: cfg phase=renderWeaponCbAtomic pedRef=%d wt=%d heldEn=%d atomic=%p frame=%p", pedRef, wtSel,
            h.enabled ? 1 : 0, static_cast<void*>(atomic), static_cast<void*>(af));
    }
    if (!h.enabled) {
        OrcLogHeldPoseCfgDisabled(ped, wtSel);
        return false;
    }
    OrcHeldPoseInvalidateBaselineForRwFrame(af);
    if (!OrcTryApplyHeldPoseOneFrame(af, h)) {
        OrcLogInfoThrottled(493, 2500u, "held pose: skip apply failed (rwcbAtomic) wt=%d pedRef=%d", wtSel, pedRef);
        return false;
    }
    if (g_heldPoseDebug) {
        RwMatrix* mm = RwFrameGetMatrix(af);
        const float r2d = 180.0f / kOrcPi;
        OrcLogInfoThrottled(437, 450u,
            "held pose: apply phase=renderWeaponCbAtomic wt=%d tgt=rwcbAtomic xyz=%.3f %.3f %.3f deg=%.2f %.2f %.2f sc=%.3f | at=%.3f %.3f %.3f",
            wtSel, h.x, h.y, h.z, h.rx * r2d, h.ry * r2d, h.rz * r2d, h.scale, mm ? mm->pos.x : 0.0f, mm ? mm->pos.y : 0.0f,
            mm ? mm->pos.z : 0.0f);
    }
    return true;
}

static void OrcHeldLogPreRwSkipThrottled(int slot, CPed* ped, const char* reason) {
    if (!g_heldPoseDebug)
        return;
    const int pr = ped ? CPools::GetPedRef(ped) : 0;
    OrcLogInfoThrottled(slot, 750u, "held preRw: skip reason=%s ped=%p pedRef=%d", reason ? reason : "?", ped, pr);
}

static bool OrcHeldPosePedEligibleForPreRwDraw(CPed* ped) {
    if (!ped)
        return false;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && ped == pl)
        return true;
    if (!g_renderAllPedsWeapons || !pl)
        return false;
    const CVector& pp = pl->GetPosition();
    const CVector& p = ped->GetPosition();
    const float dx = p.x - pp.x;
    const float dy = p.y - pp.y;
    const float dz = p.z - pp.z;
    return (dx * dx + dy * dy + dz * dz) <= (g_renderAllPedsRadius * g_renderAllPedsRadius);
}

static bool OrcHeldWeaponClumpUsesRenderWeaponCbOnly(RpClump* clump) {
    if (!clump)
        return false;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && OrcHeldPosePedEligibleForPreRwDraw(pl) && pl->m_pWeaponObject && pl->m_pWeaponObject->type == rpCLUMP &&
        reinterpret_cast<RpClump*>(pl->m_pWeaponObject) == clump)
        return true;
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpCLUMP &&
            reinterpret_cast<RpClump*>(p->m_pWeaponObject) == clump)
            return true;
    }
    return false;
}

static bool OrcHeldWeaponAtomicUsesRenderWeaponCbOnly(RpAtomic* atomic) {
    if (!atomic)
        return false;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && OrcHeldPosePedEligibleForPreRwDraw(pl) && pl->m_pWeaponObject && pl->m_pWeaponObject->type == rpATOMIC &&
        reinterpret_cast<RpAtomic*>(pl->m_pWeaponObject) == atomic)
        return true;
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpATOMIC &&
            reinterpret_cast<RpAtomic*>(p->m_pWeaponObject) == atomic)
            return true;
    }
    return false;
}

/// `outLocalPlayerRwcbMatch` — для `HeldWeaponTrace`: совпадение атома с локальным `wo` без второго прохода `ShouldApply`.
static bool OrcTryApplyHeldPoseForRenderWeaponAtomic(RpAtomic* atomic, bool* outLocalPlayerRwcbMatch) {
    if (outLocalPlayerRwcbMatch)
        *outLocalPlayerRwcbMatch = false;
    if (!atomic)
        return false;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl) {
        // Только реально выбранное оружие в слоте — иначе m_nWeaponModelId / m_nSavedWeapon тянут wt при кулаках,
        // Held крутит «зависший» m_pWeaponObject → левитирующий искажённый меш.
        const int wtSel = GetPedSelectedWeaponTypeForReplace(pl);
        if (wtSel <= 0)
            return false;
        if (!OrcHeldPosePedEligibleForPreRwDraw(pl))
            return false;
        const int pr = CPools::GetPedRef(pl);
        if (pr <= 0)
            return false;
        RwObject* wo = OrcResolveHeldWeaponRwObject(pl);
        if (!OrcHeldRwcbShouldApplyAtomicImpl(pl, atomic, wtSel, wo))
            return false;
        if (outLocalPlayerRwcbMatch)
            *outLocalPlayerRwcbMatch = true;
        return OrcApplyHeldPoseToRwcbAtomic(pl, atomic, wtSel);
    }
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        const int wtSel = GetPedSelectedWeaponTypeForReplace(p);
        if (wtSel <= 0)
            continue;
        const int pr = CPools::GetPedRef(p);
        if (pr <= 0)
            continue;
        RwObject* wo = OrcResolveHeldWeaponRwObject(p);
        if (!OrcHeldRwcbShouldApplyAtomicImpl(p, atomic, wtSel, wo))
            continue;
        return OrcApplyHeldPoseToRwcbAtomic(p, atomic, wtSel);
    }
    return false;
}

static void OrcHeldTraceLogRenderWeaponCb(RpAtomic* atomic, bool appliedHeldPose, bool rwcbMatchLocalPl) {
    if (g_heldWeaponTrace < 1 || g_orcLogLevel < OrcLogLevel::Info)
        return;
    const unsigned iv = (g_heldWeaponTrace >= 2) ? 80u : 350u;
    if (!atomic) {
        OrcLogInfoThrottled(504, iv, "held hook RenderWeaponCB(0x733670): atomic=null");
        return;
    }
    RwFrame* fr = RpAtomicGetFrame(atomic);
    const void* rdcb = atomic->renderCallBack ? reinterpret_cast<const void*>(atomic->renderCallBack) : nullptr;
    CPlayerPed* pl = FindPlayerPed(0);
    if (!pl) {
        OrcLogInfoThrottled(505, iv, "held hook RWCB: atomic=%p frame=%p rdcb=%p localPed=null", static_cast<void*>(atomic),
            static_cast<void*>(fr), rdcb);
        return;
    }
    RwObject* wo = OrcResolveHeldWeaponRwObject(pl);
    const int woT = wo ? static_cast<int>(wo->type) : -1;
    const void* woP = wo ? static_cast<const void*>(wo) : nullptr;
    const int wt = OrcResolveWeaponHeldVisualWeaponType(pl);
    const bool repl = wo && HeldWeaponRwObjectIsReplacementClone(pl, wo);
    const int pr = CPools::GetPedRef(pl);
    const unsigned slot = pl->m_nSelectedWepSlot;
    int slotWt = -1;
    if (slot < 13)
        slotWt = static_cast<int>(pl->m_aWeapons[slot].m_eWeaponType);
    bool heldCfgEn = false;
    float hx = 0.0f, hy = 0.0f, hz = 0.0f, hrx = 0.0f, hry = 0.0f, hrz = 0.0f, hsc = 1.0f;
    if (wt > 0) {
        const HeldWeaponPoseCfg& h = GetHeldPoseForPed(pl, wt, false);
        heldCfgEn = h.enabled;
        hx = h.x;
        hy = h.y;
        hz = h.z;
        hrx = h.rx;
        hry = h.ry;
        hrz = h.rz;
        hsc = h.scale;
    }
    const std::string skinRaw = GetPedStdSkinDffName(pl);
    const std::string skinIni = GetWeaponSkinIniLookupName(pl);
    const float r2d = 180.0f / kOrcPi;
    OrcLogInfoThrottled(506, iv,
        "held hook RWCB(0x733670): atomic=%p frame=%p rdcb=%p pedRef=%d skinIni=\"%s\" skinRaw=\"%s\" matchWo=%d "
        "wo=%p woT=%d repl=%d slot=%u slotWt=%d visWt=%d heldCfgEn=%d off=(%.3f,%.3f,%.3f) rotDeg=(%.1f,%.1f,%.1f) "
        "sc=%.3f appliedPose=%d",
        static_cast<void*>(atomic), static_cast<void*>(fr), rdcb, pr, skinIni.c_str(), skinRaw.c_str(),
        rwcbMatchLocalPl ? 1 : 0,
        woP, woT, repl ? 1 : 0, slot, slotWt, wt, heldCfgEn ? 1 : 0, hx, hy, hz, hrx * r2d, hry * r2d, hrz * r2d, hsc,
        appliedHeldPose ? 1 : 0);
}

static void OrcTryApplyHeldPoseBeforeRwDrawForPed(CPed* ped) {
    if (!ped)
        return;
    if (!OrcHeldPosePedEligibleForPreRwDraw(ped)) {
        OrcHeldLogPreRwSkipThrottled(471, ped, "not_eligible");
        return;
    }
    const int pr = CPools::GetPedRef(ped);
    if (pr <= 0) {
        OrcHeldLogPreRwSkipThrottled(472, ped, "pedRef<=0");
        return;
    }
    RwObject* wo = OrcResolveHeldWeaponRwObject(ped);
    if (!wo) {
        OrcHeldLogPreRwSkipThrottled(474, ped, "no_weapon_object");
        return;
    }
    if (HeldWeaponRwObjectIsReplacementClone(ped, wo)) {
        // Ванильный preRw не нужен: дельта в repl:* (лог phase=repl:). Здесь только диагностика.
        OrcHeldLogPreRwSkipThrottled(475, ped, "repl_clone_use_repl_phase");
        return;
    }
    if (!s_heldPreRwDrawAppliedPedRefs.insert(pr).second) {
        OrcHeldLogPreRwSkipThrottled(473, ped, "alreadyAppliedThisTick");
        return;
    }
    OrcApplyHeldPoseToWeaponObject(ped, wo, "preRwDraw");
}

static void OrcTryApplyHeldPoseIfClumpMatches(RpClump* clump) {
    if (!clump)
        return;
    if (OrcHeldWeaponClumpUsesRenderWeaponCbOnly(clump))
        return;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && pl->m_pWeaponObject && pl->m_pWeaponObject->type == rpCLUMP &&
        reinterpret_cast<RpClump*>(pl->m_pWeaponObject) == clump) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(488, 400u, "held preRw: rwMatch mesh=clump clump=%p pedRef=%d", clump,
                CPools::GetPedRef(pl));
        OrcTryApplyHeldPoseBeforeRwDrawForPed(pl);
        return;
    }
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return;
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpCLUMP &&
            reinterpret_cast<RpClump*>(p->m_pWeaponObject) == clump) {
            OrcTryApplyHeldPoseBeforeRwDrawForPed(p);
            return;
        }
    }
}

static void OrcTryApplyHeldPoseIfAtomicMatches(RpAtomic* atomic) {
    if (!atomic)
        return;
    if (OrcHeldWeaponAtomicUsesRenderWeaponCbOnly(atomic))
        return;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && pl->m_pWeaponObject && pl->m_pWeaponObject->type == rpATOMIC &&
        reinterpret_cast<RpAtomic*>(pl->m_pWeaponObject) == atomic) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(489, 400u, "held preRw: rwMatch mesh=atomic atomic=%p pedRef=%d", atomic,
                CPools::GetPedRef(pl));
        OrcTryApplyHeldPoseBeforeRwDrawForPed(pl);
        return;
    }
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return;
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpATOMIC &&
            reinterpret_cast<RpAtomic*>(p->m_pWeaponObject) == atomic) {
            OrcTryApplyHeldPoseBeforeRwDrawForPed(p);
            return;
        }
    }
}

static RpClump* __cdecl RpClumpRender_Detour(RpClump* clump) {
    if (g_enabled) {
        __try {
            OrcTryApplyHeldPoseIfClumpMatches(clump);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("RpClumpRender_Detour: SEH ex=0x%08X", GetExceptionCode());
        }
    }
    return g_RpClumpRender_Orig ? g_RpClumpRender_Orig(clump) : nullptr;
}

static RpAtomic* __cdecl AtomicDefaultRenderCallBack_Detour(RpAtomic* atomic) {
    if (g_enabled) {
        __try {
            OrcTryApplyHeldPoseIfAtomicMatches(atomic);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("AtomicDefaultRenderCallBack_Detour: SEH ex=0x%08X", GetExceptionCode());
        }
    }
    return g_AtomicDefaultRender_Orig ? g_AtomicDefaultRender_Orig(atomic) : nullptr;
}

static void __cdecl RenderWeaponCB_Detour(RpAtomic* atomic) {
    bool appliedPose = false;
    bool rwcbMatchLocal = false;
    if (g_enabled) {
        __try {
            const bool needMatchForTrace = g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info;
            appliedPose = OrcTryApplyHeldPoseForRenderWeaponAtomic(atomic, needMatchForTrace ? &rwcbMatchLocal : nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("RenderWeaponCB_Detour: SEH ex=0x%08X", GetExceptionCode());
        }
    }
    if (g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info)
        OrcHeldTraceLogRenderWeaponCb(atomic, g_enabled && appliedPose, rwcbMatchLocal);
    if (g_RenderWeaponCB_Orig)
        g_RenderWeaponCB_Orig(atomic);
}

void OrcHeldWeaponTraceGameProcessTick() {
    if (g_heldWeaponTrace < 1 || g_orcLogLevel < OrcLogLevel::Info)
        return;
    if (g_heldWeaponStatusIntervalMs <= 0)
        return;
    static int s_lastHeldStatusMs = 0;
    const int now = CTimer::m_snTimeInMilliseconds;
    if (s_lastHeldStatusMs != 0 && (now - s_lastHeldStatusMs) < g_heldWeaponStatusIntervalMs)
        return;
    s_lastHeldStatusMs = now;

    CPlayerPed* pl = FindPlayerPed(0);
    if (!pl) {
        OrcLogInfo("held status: interval=%dms localPed=null pluginEn=%d", g_heldWeaponStatusIntervalMs, g_enabled ? 1 : 0);
        return;
    }
    const int pr = CPools::GetPedRef(pl);
    const std::string skinRaw = GetPedStdSkinDffName(pl);
    const std::string skinIni = GetWeaponSkinIniLookupName(pl);
    const int modelIndex = static_cast<int>(pl->m_nModelIndex);
    const unsigned slot = pl->m_nSelectedWepSlot;
    int slotWt = -1;
    if (slot < 13)
        slotWt = static_cast<int>(pl->m_aWeapons[slot].m_eWeaponType);
    const int visWt = OrcResolveWeaponHeldVisualWeaponType(pl);
    const int curWt = GetPedCurrentWeaponType(pl);
    RwObject* woSlot = pl->m_pWeaponObject;
    RwObject* woRes = OrcResolveHeldWeaponRwObject(pl);
    const int woSlotT = woSlot ? static_cast<int>(woSlot->type) : -1;
    const int woResT = woRes ? static_cast<int>(woRes->type) : -1;
    const bool replMesh = woRes && HeldWeaponRwObjectIsReplacementClone(pl, woRes);
    const bool shouldWr = ShouldReplaceHeldWeaponForPed(pl);
    bool replCapture = false;
    std::string replKey;
    if (pr > 0) {
        auto it = g_heldWeaponReplacements.find(pr);
        if (it != g_heldWeaponReplacements.end() && it->second.captureActive) {
            replCapture = true;
            replKey = it->second.replacementKey;
        }
    }
    bool heldCfgEn = false;
    float hx = 0.0f, hy = 0.0f, hz = 0.0f, hrx = 0.0f, hry = 0.0f, hrz = 0.0f, hsc = 1.0f;
    if (visWt > 0) {
        const HeldWeaponPoseCfg& h = GetHeldPoseForPed(pl, visWt, false);
        heldCfgEn = h.enabled;
        hx = h.x;
        hy = h.y;
        hz = h.z;
        hrx = h.rx;
        hry = h.ry;
        hrz = h.rz;
        hsc = h.scale;
    }
    const float r2d = 180.0f / kOrcPi;
    OrcLogInfo(
        "held status: every %dms | skinIni=\"%s\" skinRaw=\"%s\" modelIndex=%d pedRef=%d | slot=%u slotWt=%d curWt=%d "
        "visWt=%d wpnMid=%d savedWt=%d | woSlot=%p t=%d woRes=%p t=%d replMesh=%d | wrGate en=%d wr=%d inHands=%d "
        "shouldWr=%d replCapture=%d replKey=\"%s\" | heldPreset en=%d off=(%.3f,%.3f,%.3f) rotDeg=(%.1f,%.1f,%.1f) "
        "sc=%.3f | rwpfcBatch=%llu",
        g_heldWeaponStatusIntervalMs, skinIni.c_str(), skinRaw.c_str(), modelIndex, pr, slot, slotWt, curWt, visWt,
        pl->m_nWeaponModelId, static_cast<int>(pl->m_nSavedWeapon), static_cast<void*>(woSlot), woSlotT,
        static_cast<void*>(woRes), woResT, replMesh ? 1 : 0, g_enabled ? 1 : 0, g_weaponReplacementEnabled ? 1 : 0,
        g_weaponReplacementInHands ? 1 : 0, shouldWr ? 1 : 0, replCapture ? 1 : 0, replKey.c_str(), heldCfgEn ? 1 : 0,
        hx, hy, hz, hrx * r2d, hry * r2d, hrz * r2d, hsc,
        static_cast<unsigned long long>(s_heldRwpfcBatchCounter));
}

void OrcHeldPoseBeginSimFrame() {
    s_heldPreRwDrawAppliedPedRefs.clear();
    s_heldPoseEngineBaselineByFrame.clear();
}

static void __cdecl RenderWeaponPedsForPC_Detour() {
    // Каждый вызов — батч RenderWeaponCB; сбрасываем кеш базы Held, чтобы каждый колбэк брал свежий IK-снимок
    // (ваниль может пересчитать матрицу между вызовами на один клумп).
    ++s_heldRwpfcBatchCounter;
    if (g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info) {
        const unsigned iv = (g_heldWeaponTrace >= 2) ? 50u : 300u;
        OrcLogInfoThrottled(502, iv,
            "held hook RenderWeaponPedsForPC(0x732F30): batch=%llu t=%ums -> clear held baseline cache then Orig()",
            static_cast<unsigned long long>(s_heldRwpfcBatchCounter), static_cast<unsigned>(CTimer::m_snTimeInMilliseconds));
    }
    s_heldPoseEngineBaselineByFrame.clear();
    if (g_RenderWeaponPedsForPC_Orig)
        g_RenderWeaponPedsForPC_Orig();
}

bool OrcApplyHeldWeaponPoseAdjust(CPed* ped) {
    if (!g_enabled) {
        OrcLogInfoThrottled(431, 5000u, "held pose: skip plugin disabled");
        return false;
    }
    const int pedRefEarly = ped ? CPools::GetPedRef(ped) : 0;
    RwObject* obj = OrcResolveHeldWeaponRwObject(ped);
    if (!ped || !obj) {
        OrcLogInfoThrottled(
            432, 4000u, "held pose: skip no ped/obj ped=%p pedRef=%d slot=%p resolved=%p", ped, pedRefEarly,
            ped ? ped->m_pWeaponObject : nullptr, obj);
        return false;
    }
    if (HeldWeaponRwObjectIsReplacementClone(ped, obj)) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(449, 1800u, "held pose: skip API replClone pedRef=%d (Held после CopyRwObjectRootMatrix)", pedRefEarly);
        return false;
    }
    return OrcApplyHeldPoseToWeaponObject(ped, obj, "adjustAPI");
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
    // Кулаки / слот без оружия: не рисуем клон на кости тела (дубль с кобурой + «летающий» меш).
    if (GetPedSelectedWeaponTypeForReplace(ped) <= 0)
        return;
    const int wt = state.weaponType;
    OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
    if (!PositionHeldReplacementLikeBodyWeapon(ped, wt, state.rwObject)) {
        OrcLogInfoThrottled(29, 4000u,
            "held wr hide-base: position failed pedRef=%d wt=%d (bone/skeleton?)",
            CPools::GetPedRef(ped), wt);
        return;
    }
    OrcApplyHeldPoseToWeaponObject(ped, state.rwObject, "repl:hideBase");

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
        if (g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info && pedRefEarly > 0) {
            OrcLogInfoThrottled(508, 5000u,
                "held wr trace: gated detail pedRef=%d ped=%p m_pWeaponObject=%p visWt=%d slot=%d",
                pedRefEarly, ped, ped ? ped->m_pWeaponObject : nullptr,
                ped ? OrcResolveWeaponHeldVisualWeaponType(ped) : -1, ped ? (int)ped->m_nSelectedWepSlot : -1);
        }
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
            OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
            CopyRwObjectRootMatrix(state.originalObject, state.rwObject);
            SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
            state.hideBaseMode = false;
            state.captureActive = true;
            ped->m_pWeaponObject = state.rwObject;
            OrcApplyHeldPoseToWeaponObject(ped, state.rwObject, "repl:swapStock");
            return;
        }
        if (ped->m_pWeaponObject == state.rwObject) {
            OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
            CopyRwObjectRootMatrix(state.originalObject, state.rwObject);
            SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
            OrcApplyHeldPoseToWeaponObject(ped, state.rwObject, "repl:reSync");
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
    OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
    CopyRwObjectRootMatrix(ped->m_pWeaponObject, state.rwObject);
    SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
    state.originalObject = ped->m_pWeaponObject;
    state.hideBaseMode = false;
    state.captureActive = true;
    ped->m_pWeaponObject = state.rwObject;
    OrcApplyHeldPoseToWeaponObject(ped, state.rwObject, "repl:firstSwap");
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

    OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
    CopyRwObjectRootMatrix(ped->m_pWeaponObject, state.rwObject);
    SyncWeaponReplacementMuzzleFlashAlpha(ped, state.rwObject);
    state.originalObject = ped->m_pWeaponObject;
    state.hideBaseMode = false;
    state.captureActive = true;
    ped->m_pWeaponObject = state.rwObject;
    OrcApplyHeldPoseToWeaponObject(ped, state.rwObject, "repl:addModel");
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
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_RenderWeaponPedsForPC),
            reinterpret_cast<void*>(&RenderWeaponPedsForPC_Detour),
            reinterpret_cast<void**>(&g_RenderWeaponPedsForPC_Orig)) != MH_OK) {
        OrcLogError("RenderWeaponPedsForPC hook: MH_CreateHook failed (0x%08X)",
            (unsigned)kAddr_RenderWeaponPedsForPC);
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_RpClumpRender), reinterpret_cast<void*>(&RpClumpRender_Detour),
            reinterpret_cast<void**>(&g_RpClumpRender_Orig)) != MH_OK) {
        OrcLogError("RpClumpRender hook: MH_CreateHook failed (0x%08X)", (unsigned)kAddr_RpClumpRender);
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_AtomicDefaultRenderCallBack),
            reinterpret_cast<void*>(&AtomicDefaultRenderCallBack_Detour),
            reinterpret_cast<void**>(&g_AtomicDefaultRender_Orig)) != MH_OK) {
        OrcLogError("AtomicDefaultRenderCallBack hook: MH_CreateHook failed (0x%08X)",
            (unsigned)kAddr_AtomicDefaultRenderCallBack);
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_RenderWeaponCB), reinterpret_cast<void*>(&RenderWeaponCB_Detour),
            reinterpret_cast<void**>(&g_RenderWeaponCB_Orig)) != MH_OK) {
        OrcLogError("RenderWeaponCB hook: MH_CreateHook failed (0x%08X)", (unsigned)kAddr_RenderWeaponCB);
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
    if (g_RenderWeaponPedsForPC_Orig) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_RenderWeaponPedsForPC));
        if (st != MH_OK)
            OrcLogError("RenderWeaponPedsForPC hook: MH_EnableHook -> %s", MH_StatusToString(st));
        else
            OrcLogInfo("RenderWeaponPedsForPC hook installed (passthrough 0x%08X)", (unsigned)kAddr_RenderWeaponPedsForPC);
    }
    if (g_RpClumpRender_Orig) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_RpClumpRender));
        if (st != MH_OK)
            OrcLogError("RpClumpRender hook: MH_EnableHook -> %s", MH_StatusToString(st));
        else
            OrcLogInfo("RpClumpRender held pre-draw hook (0x%08X)", (unsigned)kAddr_RpClumpRender);
    }
    if (g_AtomicDefaultRender_Orig) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_AtomicDefaultRenderCallBack));
        if (st != MH_OK)
            OrcLogError("AtomicDefaultRenderCallBack hook: MH_EnableHook -> %s", MH_StatusToString(st));
        else
            OrcLogInfo("AtomicDefaultRenderCallBack held pre-draw hook (0x%08X)", (unsigned)kAddr_AtomicDefaultRenderCallBack);
    }
    if (g_RenderWeaponCB_Orig) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_RenderWeaponCB));
        if (st != MH_OK)
            OrcLogError("RenderWeaponCB hook: MH_EnableHook -> %s", MH_StatusToString(st));
        else
            OrcLogInfo("RenderWeaponCB held pre-draw hook (0x%08X)", (unsigned)kAddr_RenderWeaponCB);
    }
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


