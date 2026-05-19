#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CHud.h"
#include "CSprite2d.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CClumpModelInfo.h"
#include "CModelInfo.h"
#include "CTimer.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"
#include "eWeaponType.h"
#include "CEntity.h"
#include "CWeapon.h"
#include "CVector.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <climits>
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

static RwFrame* OrcRwFrameGetParent(RwFrame* f);
static void OrcHeldGunflashMuzzleDeltaResetForSimTick();
static void OrcHeldMaybeApplyGunflashFrameMuzzleDelta(CPed* ped, RpClump* clump, int wt, RwFrame* gfOverride = nullptr);
static bool OrcTryGetRwObjectRootWorldPos(RwObject* rwObject, CVector& out);

static bool g_pedWeaponModelHooksInstalled = false;
using AddWeaponModel_t = void(__thiscall*)(CPed*, int);
using RemoveWeaponModel_t = void(__thiscall*)(CPed*, int);
static AddWeaponModel_t g_AddWeaponModel_Orig = nullptr;
static RemoveWeaponModel_t g_RemoveWeaponModel_Orig = nullptr;
static RwFrame* GetRwObjectRootFrame(RwObject* object) {
    if (!object)
        return nullptr;
    if (object->type == rpATOMIC)
        return RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(object));
    if (object->type == rpCLUMP)
        return RpClumpGetFrame(reinterpret_cast<RpClump*>(object));
    return nullptr;
}

// Align replacement clone root to stock weapon world pose before draw-time replacement.
// Stock weapon root is usually parented to the hand; the standalone clone needs LTM, not local matrix.
static void CopyRwObjectRootMatrix(RwObject* src, RwObject* dst) {
    if (!src || !dst)
        return;
    RwFrame* sf = GetRwObjectRootFrame(src);
    RwFrame* df = GetRwObjectRootFrame(dst);
    if (!sf || !df)
        return;
    __try {
        const RwMatrix* sm = RwFrameGetLTM(sf);
        if (!sm)
            sm = RwFrameGetMatrix(sf);
        if (!sm)
            return;
        std::memcpy(RwFrameGetMatrix(df), sm, sizeof(RwMatrix));
        RwMatrixUpdate(RwFrameGetMatrix(df));
        RwFrameUpdateObjects(df);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("CopyRwObjectRootMatrix: SEH ex=0x%08X", GetExceptionCode());
    }
}

struct OrcAtomicFrameListCtx {
    std::vector<RwFrame*> frames;
};

static RpAtomic* OrcCollectAtomicRootFrameCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<OrcAtomicFrameListCtx*>(data);
    if (atomic && RpAtomicGetFrame(atomic))
        ctx->frames.push_back(RpAtomicGetFrame(atomic));
    return atomic;
}

// Stock clump gets full IK hierarchy each frame; replacement clone only received the root matrix.
// Copy per-atomic RwFrame modelling matrices when atomic counts match (same DFF layout).
/// Оружие в руках часто с `RpHAnimHierarchy` на корневом RwFrame — копируем узлы по `nodeID`, как `CopySkinHierarchyPose` для скинов.
static RpHAnimHierarchy* OrcGetHAnimHierarchyFromClumpRoot(RpClump* clump) {
    if (!clump)
        return nullptr;
    RwFrame* root = RpClumpGetFrame(clump);
    if (!root)
        return nullptr;
    __try {
        return RpHAnimFrameGetHierarchy(root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcGetHAnimHierarchyFromClumpRoot: SEH ex=0x%08X", GetExceptionCode());
        return nullptr;
    }
}

static bool CopyWeaponClumpPoseViaHAnimFromStock(RpClump* stock, RpClump* clone) {
    if (!stock || !clone)
        return false;
    RpHAnimHierarchy* sh = OrcGetHAnimHierarchyFromClumpRoot(stock);
    RpHAnimHierarchy* dh = OrcGetHAnimHierarchyFromClumpRoot(clone);
    if (!sh || !dh || !sh->pMatrixArray || !dh->pMatrixArray || !sh->pNodeInfo || !dh->pNodeInfo)
        return false;
    __try {
        for (int i = 0; i < sh->numNodes; ++i) {
            const int nodeId = sh->pNodeInfo[i].nodeID;
            if (nodeId < 0)
                continue;
            const int di = RpHAnimIDGetIndex(dh, nodeId);
            if (di < 0 || di >= dh->numNodes)
                continue;
            dh->pMatrixArray[di] = sh->pMatrixArray[i];
        }
        RpHAnimHierarchyUpdateMatrices(dh);
        for (int i = 0; i < sh->numNodes; ++i) {
            const int nodeId = sh->pNodeInfo[i].nodeID;
            if (nodeId < 0)
                continue;
            const int di = RpHAnimIDGetIndex(dh, nodeId);
            if (di < 0 || di >= dh->numNodes)
                continue;
            RwFrame* sf = sh->pNodeInfo[i].pFrame;
            RwFrame* df = dh->pNodeInfo[di].pFrame;
            if (!sf || !df)
                continue;
            std::memcpy(RwFrameGetMatrix(df), RwFrameGetMatrix(sf), sizeof(RwMatrix));
            RwMatrixUpdate(RwFrameGetMatrix(df));
        }
        RwFrame* cloneRoot = RpClumpGetFrame(clone);
        if (cloneRoot)
            RwFrameUpdateObjects(cloneRoot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("CopyWeaponClumpPoseViaHAnimFromStock: SEH ex=0x%08X", GetExceptionCode());
        return false;
    }
}

static void CopyRwClumpMatchingAtomicFrameMatricesFromStock(RwObject* stock, RwObject* clone) {
    if (!stock || !clone || stock->type != rpCLUMP || clone->type != rpCLUMP)
        return;
    RpClump* sc = reinterpret_cast<RpClump*>(stock);
    RpClump* dc = reinterpret_cast<RpClump*>(clone);
    OrcAtomicFrameListCtx stockAll{}, cloneAll{};
    RpClumpForAllAtomics(sc, OrcCollectAtomicRootFrameCb, &stockAll);
    RpClumpForAllAtomics(dc, OrcCollectAtomicRootFrameCb, &cloneAll);
    __try {
        if (!stockAll.frames.empty() && stockAll.frames.size() == cloneAll.frames.size()) {
            for (size_t i = 0; i < stockAll.frames.size(); ++i) {
                RwFrame* sf = stockAll.frames[i];
                RwFrame* df = cloneAll.frames[i];
                if (!sf || !df)
                    continue;
                std::memcpy(RwFrameGetMatrix(df), RwFrameGetMatrix(sf), sizeof(RwMatrix));
                RwMatrixUpdate(RwFrameGetMatrix(df));
            }
        }
        RwFrame* root = RpClumpGetFrame(dc);
        if (root)
            RwFrameUpdateObjects(root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("CopyRwClumpMatchingAtomicFrameMatricesFromStock: SEH ex=0x%08X", GetExceptionCode());
    }
}

static void CopyStockAtomicLtmToCloneRoot(RpAtomic* stockAtomic, RwObject* clone) {
    if (!stockAtomic || !clone)
        return;
    RwFrame* sf = RpAtomicGetFrame(stockAtomic);
    RwFrame* df = GetRwObjectRootFrame(clone);
    if (!sf || !df)
        return;
    __try {
        const RwMatrix* sm = RwFrameGetLTM(sf);
        if (!sm)
            sm = RwFrameGetMatrix(sf);
        if (!sm)
            return;
        std::memcpy(RwFrameGetMatrix(df), sm, sizeof(RwMatrix));
        RwMatrixUpdate(RwFrameGetMatrix(df));
        RwFrameUpdateObjects(df);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("CopyStockAtomicLtmToCloneRoot: SEH ex=0x%08X", GetExceptionCode());
    }
}

static void CopyStockHeldWeaponRwMatricesToClone(RwObject* stock, RwObject* clone, bool copyDeepPose) {
    CopyRwObjectRootMatrix(stock, clone);
    if (!copyDeepPose) {
        if (g_orcLogLevel >= OrcLogLevel::Info) {
            OrcLogInfoThrottled(596, 2500u,
                "held wr posecopy: root-only stock=%p clone=%p st=%d ct=%d",
                stock, clone, stock ? (int)stock->type : -1, clone ? (int)clone->type : -1);
        }
        return;
    }
    if (stock->type == rpCLUMP && clone->type == rpCLUMP &&
        CopyWeaponClumpPoseViaHAnimFromStock(reinterpret_cast<RpClump*>(stock), reinterpret_cast<RpClump*>(clone))) {
        return;
    }
    CopyRwClumpMatchingAtomicFrameMatricesFromStock(stock, clone);
}

struct HeldWeaponReplacementState {
    int weaponType = 0;
    std::string replacementKey;
    RwObject* rwObject = nullptr;
    /// PRO dual-wield: отдельный клон для левой руки (как `RenderedWeapon.secondary` на теле).
    RwObject* rwObjectSecondary = nullptr;
    /// Последний stock-атом primary draw-swap (исключить из поиска L-hand stock).
    RpAtomic* lastPrimaryStockAtomic = nullptr;
    /// Кэш L-hand stock-атома на `originalObject` (twin pistols: оба атома в одном clump).
    RpAtomic* lastSecondaryStockAtomic = nullptr;
    RwObject* originalObject = nullptr;
    bool poseSynced = false;
    bool captureActive = false;
    bool hideBaseMode = false;
};

static std::unordered_map<int, HeldWeaponReplacementState> g_heldWeaponReplacements;

/// SA:MP: запись replacement в `g_heldWeaponReplacements` может жить под ref, отличным от `FindPlayerPed(0)`, при том же GTA-педе.
static int OrcSampMirrorReplacementOwnerPedRef(CPed* firingPed) {
    if (!firingPed)
        return 0;
    CPlayerPed* fp = FindPlayerPed(0);
    if (!fp)
        return 0;
    const int fireRef = CPools::GetPedRef(firingPed);
    const int fpRef = CPools::GetPedRef(fp);
    if (fireRef <= 0 || fpRef <= 0 || fireRef == fpRef)
        return 0;
    if (firingPed == fp)
        return 0;
    if (!samp_bridge::IsSampBuildKnown())
        return 0;
    if (!samp_bridge::IsLocalPlayerGtaPed(firingPed))
        return 0;
    return fpRef;
}

static void OrcHeldPoseInvalidateBaselineForRwFrame(RwFrame* f);
static bool OrcTryApplyHeldPoseOneFrame(RwFrame* frame, const HeldWeaponPoseCfg& h);
static bool OrcCallRenderWeaponCbOrigSafe(RpAtomic* atomic, const char* phaseTag);

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
    return it->second.rwObject == obj || it->second.rwObjectSecondary == obj;
}

static bool OrcHeldReplacementStateMatchesResolvedChoice(CPed* ped, int wt, const HeldWeaponReplacementState& st) {
    if (!ped || wt <= 0 || !st.rwObject || st.weaponType != wt || st.replacementKey.empty())
        return false;
    const std::string resolvedKey = OrcResolveUsableWeaponReplacementKeyForPed(ped, wt, true);
    return !resolvedKey.empty() && OrcToLowerAscii(resolvedKey) == OrcToLowerAscii(st.replacementKey);
}

static RwObject* OrcResolveActiveReplacementWeaponObject(CPed* ped) {
    if (!ped)
        return nullptr;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return nullptr;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it != g_heldWeaponReplacements.end()) {
        const HeldWeaponReplacementState& st = it->second;
        if (st.rwObject && st.rwObject->type == rpCLUMP)
            return st.rwObject;
    }
    CPlayerPed* fp = FindPlayerPed(0);
    if (!fp || ped != fp || !samp_bridge::IsSampBuildKnown())
        return nullptr;
    for (const auto& kv : g_heldWeaponReplacements) {
        CPed* other = CPools::GetPed(kv.first);
        if (!other)
            continue;
        if (CPools::GetPedRef(other) != kv.first)
            continue;
        if (OrcSampMirrorReplacementOwnerPedRef(other) != pedRef)
            continue;
        const HeldWeaponReplacementState& st = kv.second;
        if (st.rwObject && st.rwObject->type == rpCLUMP)
            return st.rwObject;
    }
    return nullptr;
}

static bool OrcHeldIsAnyReplacementCloneObject(RwObject* obj) {
    if (!obj)
        return false;
    for (const auto& kv : g_heldWeaponReplacements) {
        if ((kv.second.rwObject && kv.second.rwObject == obj) ||
            (kv.second.rwObjectSecondary && kv.second.rwObjectSecondary == obj))
            return true;
    }
    return false;
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

RpClump* OrcPedResolveGunflashTargetClump(CPed* ped, int wtHint) {
    if (!ped)
        return nullptr;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return nullptr;
    // Видимый меш — клон `rwObject`; слот может указывать на новый/старый сток, NULL или сам клон.
    // Сравнение только с `originalObject` ломается после смены инстанса стока движком (лог: nudge на BA10 при rebound BA50).
    auto pickReplacementClone = [&](const HeldWeaponReplacementState& st, CPed* choicePed) -> RpClump* {
        if (!st.rwObject || st.rwObject->type != rpCLUMP)
            return nullptr;
        RwObject* slot = ped->m_pWeaponObject;
        if (slot == st.rwObject)
            return reinterpret_cast<RpClump*>(st.rwObject);
        if (!slot)
            return reinterpret_cast<RpClump*>(st.rwObject);
        if (st.captureActive)
            return reinterpret_cast<RpClump*>(st.rwObject);
        // `Fire` often runs after EndScene restored `m_pWeaponObject` to stock, while
        // the current visual choice is still the held replacement clone. In that
        // resting state DoGunFlash must stay bound to the replacement clump, not the stock slot.
        if (st.poseSynced && wtHint > 0 &&
            (OrcHeldReplacementStateMatchesResolvedChoice(ped, wtHint, st) ||
                (choicePed && choicePed != ped && OrcHeldReplacementStateMatchesResolvedChoice(choicePed, wtHint, st)))) {
            return reinterpret_cast<RpClump*>(st.rwObject);
        }
        return nullptr;
    };
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it != g_heldWeaponReplacements.end()) {
        if (RpClump* c = pickReplacementClone(it->second, ped))
            return c;
    }
    CPlayerPed* fp = FindPlayerPed(0);
    if (fp && ped != fp && samp_bridge::IsSampBuildKnown()) {
        for (const auto& kv : g_heldWeaponReplacements) {
            CPed* owner = CPools::GetPed(kv.first);
            if (!owner)
                continue;
            if (OrcSampMirrorReplacementOwnerPedRef(owner) != pedRef)
                continue;
            if (RpClump* c = pickReplacementClone(kv.second, owner))
                return c;
        }
    }
    RwObject* wo = ped->m_pWeaponObject;
    if (wo && wo->type == rpCLUMP)
        return reinterpret_cast<RpClump*>(wo);
    RwObject* repl = OrcResolveActiveReplacementWeaponObject(ped);
    if (repl && repl->type == rpCLUMP)
        return reinterpret_cast<RpClump*>(repl);
    return nullptr;
}

// `Events::pedRenderEvent` fires before GTA draws the held `m_pWeaponObject`. For SA:MP the stock object must
// stay in the slot for the whole frame, so held replacement is done as a draw-time swap in RenderWeaponCB /
// AtomicDefaultRenderCallBack: stock draw is suppressed there and the clone is rendered at the same final pose.
struct DeferredHeldWeaponSlotRestore {
    RwObject* stock = nullptr;
    RwObject* clone = nullptr;
    int weaponModelId = 0;
    unsigned char selectedSlot = 0;
};
static std::unordered_map<int, DeferredHeldWeaponSlotRestore> g_deferredHeldWeaponStockRestore;

static bool OrcDeferredHeldRestoreAppliesToCurrentSlot(CPed* ped, const DeferredHeldWeaponSlotRestore& r) {
    if (!ped || !r.stock || !r.clone)
        return false;
    if (ped->m_pWeaponObject == r.clone)
        return true;
    if (ped->m_pWeaponObject)
        return false;
    return ped->m_nWeaponModelId == r.weaponModelId && ped->m_nSelectedWepSlot == r.selectedSlot;
}

// If old builds ever leave the clone/null in the slot before weapon model ops, restore stock before vanilla code.
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
    if (OrcDeferredHeldRestoreAppliesToCurrentSlot(ped, r)) {
        ped->m_pWeaponObject = r.stock;
        OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
    }
    g_deferredHeldWeaponStockRestore.erase(pedRef);
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
    for (auto& kv : g_deferredHeldWeaponStockRestore) {
        CPed* ped = CPools::GetPed(kv.first);
        DeferredHeldWeaponSlotRestore& r = kv.second;
        if (ped && OrcDeferredHeldRestoreAppliesToCurrentSlot(ped, r)) {
            ped->m_pWeaponObject = r.stock;
            OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
        }
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

/// Восстанавливает `originalObject` после defer/смены инстанса стока движком (лог: pinReplSlot noOriginal / notStockPointer).
static void OrcHeldTryRepairReplacementOriginalObject(CPed* ped, int pedRef, HeldWeaponReplacementState& st) {
    if (!ped || pedRef <= 0 || !st.rwObject)
        return;
    if (st.originalObject)
        return;
    auto d = g_deferredHeldWeaponStockRestore.find(pedRef);
    if (d != g_deferredHeldWeaponStockRestore.end() && d->second.stock && d->second.clone == st.rwObject) {
        st.originalObject = d->second.stock;
        return;
    }
    RwObject* wo = ped->m_pWeaponObject;
    if (wo && wo != st.rwObject && !OrcHeldIsAnyReplacementCloneObject(wo))
        st.originalObject = wo;
}

/// Перед `CPed::AddWeaponModel` / `RemoveWeaponModel` движок обходит `CBaseModelInfo` текущего оружия (`RemoveRef` и т.д.).
/// В слоте должен быть **стоковый** клумп; если остался replacement-клон — refcount/указатели ломаются (AV в `RemoveRef`).
static void OrcEnsureStockWeaponClumpInHeldSlotBeforeVanillaWeaponModelOp(CPed* ped) {
    if (!ped)
        return;
    OrcRestoreDeferredHeldStockIfSlotStillHasClone(ped);

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end())
        return;
    HeldWeaponReplacementState& st = it->second;
    if (!st.rwObject || ped->m_pWeaponObject != st.rwObject)
        return;

    RwObject* stock = st.originalObject;
    if (!stock) {
        OrcHeldTryRepairReplacementOriginalObject(ped, pedRef, st);
        stock = st.originalObject;
    }
    if (!stock) {
        auto d = g_deferredHeldWeaponStockRestore.find(pedRef);
        if (d != g_deferredHeldWeaponStockRestore.end() && d->second.stock && d->second.clone == st.rwObject)
            stock = d->second.stock;
    }
    if (!stock) {
        if (g_orcLogLevel >= OrcLogLevel::Info) {
            OrcLogInfoThrottled(840, 400u,
                "held wr: preVanillaWeaponModelOp skip restore (no stock) pedRef=%d clone=%p orig=%p defer=%d",
                pedRef, st.rwObject, st.originalObject,
                g_deferredHeldWeaponStockRestore.count(pedRef) ? 1 : 0);
        }
        return;
    }

    ped->m_pWeaponObject = stock;
    OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
    g_deferredHeldWeaponStockRestore.erase(pedRef);
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

bool OrcGetHeldReplacementKeyForPed(CPed* ped, int wt, std::string& outKeyLower) {
    outKeyLower.clear();
    if (!ped || wt <= 0)
        return false;
    const int pref = CPools::GetPedRef(ped);
    if (pref <= 0)
        return false;
    auto it = g_heldWeaponReplacements.find(pref);
    if (it == g_heldWeaponReplacements.end())
        return false;
    const HeldWeaponReplacementState& st = it->second;
    if (!st.captureActive || st.weaponType != wt || st.replacementKey.empty())
        return false;
    outKeyLower = OrcToLowerAscii(st.replacementKey);
    return !outKeyLower.empty();
}

bool OrcPedWantsDualWieldHeld(CPed* ped, int wt) {
    if (!ped || wt <= 0 || !g_considerWeaponSkills)
        return false;
    if (OrcResolveWeaponHeldVisualWeaponType(ped) != wt)
        return false;
    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    CWeaponInfo* twinInfo = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 2);
    if (!twinInfo)
        twinInfo = wi;
    if (!twinInfo || !twinInfo->m_nFlags.bTwinPistol)
        return false;
    return ped->GetWeaponSkill(static_cast<eWeaponType>(wt)) == WEAPSKILL_PRO;
}

RwFrame* OrcPedResolveGunflashFrameForDualHand(CPed* ped, int wtHint, bool isLeftHand) {
    if (!ped)
        return nullptr;
    const int wt = wtHint > 0 ? wtHint : OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wt <= 0 || !OrcPedWantsDualWieldHeld(ped, wt))
        return nullptr;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return nullptr;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end())
        return nullptr;
    const HeldWeaponReplacementState& st = it->second;
    RwObject* mesh = isLeftHand ? st.rwObjectSecondary : st.rwObject;
    if (!mesh)
        mesh = st.rwObject;
    if (!mesh || mesh->type != rpCLUMP)
        return nullptr;
    return CClumpModelInfo::GetFrameFromName(reinterpret_cast<RpClump*>(mesh), "gunflash");
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

bool OrcHeldTryGetPoseEngineBaselineForFrame(RwFrame* frame, RwMatrix& out) {
    if (!frame)
        return false;
    const auto it = s_heldPoseEngineBaselineByFrame.find(reinterpret_cast<uintptr_t>(frame));
    if (it == s_heldPoseEngineBaselineByFrame.end())
        return false;
    out = it->second;
    return OrcRwMatrixFinite(&out) && OrcRwMatrixAxesNonDegenerate(&out);
}

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
    RwFrame* gunflashRoot = nullptr;
    int nAtom = 0;
    int nOk = 0;
    float logAtX = 0.0f;
    float logAtY = 0.0f;
    float logAtZ = 0.0f;
};

struct HeldRwcbFrameRestore {
    RwFrame* frame = nullptr;
    RwMatrix matrix{};
    bool active = false;
};

static int s_heldRenderWeaponCbDepth = 0;
static RpAtomic* s_heldRenderWeaponCbAtomic = nullptr;

static void OrcHeldRestoreRwcbFrame(HeldRwcbFrameRestore& restore) {
    if (!restore.active || !restore.frame)
        return;
    __try {
        RwMatrix* m = RwFrameGetMatrix(restore.frame);
        if (m) {
            *m = restore.matrix;
            RwMatrixUpdate(m);
            RwFrameUpdateObjects(restore.frame);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcHeldRestoreRwcbFrame: SEH ex=0x%08X", GetExceptionCode());
    }
    restore.active = false;
}

/// Кадр атомика — сам dummy `gunflash` или потомок: на них **не** крутим `OrcTryApplyHeldPoseOneFrame`
/// (локальные LTMs ≠ база «оружие в руке» — ломает DoGunFlash / альфу). Смещение вспышки — отдельно через muzzle-дельту.
static bool OrcHeldAtomicFrameIsUnderGunflashDummy(RwFrame* atomicFrame, RwFrame* gunflashFrame) {
    if (!atomicFrame || !gunflashFrame)
        return false;
    constexpr int kMaxSteps = 64;
    int steps = 0;
    for (RwFrame* x = atomicFrame; x && steps < kMaxSteps; x = OrcRwFrameGetParent(x), ++steps) {
        if (x == gunflashFrame)
            return true;
    }
    return false;
}

static RpAtomic* OrcHeldPoseApplyEachAtomicCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<HeldClumpApplyCtx*>(data);
    ctx->nAtom++;
    RwFrame* f = RpAtomicGetFrame(atomic);
    if (!f || !ctx->h)
        return atomic;
    if (ctx->gunflashRoot && OrcHeldAtomicFrameIsUnderGunflashDummy(f, ctx->gunflashRoot))
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
static bool OrcApplyHeldPoseToWeaponObject(CPed* ped,
    RwObject* obj,
    const char* phase,
    int wtOverride = -1,
    bool secondaryHand = false) {
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
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, secondaryHand);
    const bool isReplClone = HeldWeaponRwObjectIsReplacementClone(ped, obj);
    if (g_heldPoseDebug || g_heldWeaponTrace >= 2) {
        const bool fromRepl = (ped->m_pWeaponObject != obj);
        OrcLogInfoThrottled(
            465 + wt % 7, 1600u,
            "held chain: cfg phase=%s pedRef=%d wt=%d secondary=%d heldEn=%d obj=%p objT=%d meshRepl=%d resolve=%s",
            phase, pedRefEarly, wt, secondaryHand ? 1 : 0, h.enabled ? 1 : 0, obj, (int)obj->type,
            isReplClone ? 1 : 0, fromRepl ? "replBuf" : "slot");
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
    bool applyGunflashMuzzleDeltaToClump = false;
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
        // Replacement DFF: старая ветка "root-only" давала расхождение с vanilla-путем (eachAtomic),
        // из-за чего часть моделей визуально почти не реагировала на live custom-слайдеры.
        // Делаем тот же алгоритм, что у vanilla clump: eachAtomic -> fallback root.
        if (isReplClone) {
            HeldClumpApplyCtx ctx{};
            ctx.h = &h;
            ctx.gunflashRoot = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
            RpClumpForAllAtomics(clump, OrcHeldPoseApplyEachAtomicCb, &ctx);
            heldPoseAtomCount = ctx.nAtom;
            heldPoseOkCount = ctx.nOk;
            logAtX = ctx.logAtX;
            logAtY = ctx.logAtY;
            logAtZ = ctx.logAtZ;
            if (ctx.nOk == 0) {
                RwFrame* rootF = RpClumpGetFrame(clump);
                if (!rootF) {
                    OrcLogInfoThrottled(434, 3000u, "held pose: skip no root repl phase=%s wt=%d", phase, wt);
                    return false;
                }
                OrcHeldPoseInvalidateBaselineForRwFrame(rootF);
                if (!OrcTryApplyHeldPoseOneFrame(rootF, h)) {
                    OrcLogInfoThrottled(442, 2500u, "held pose: skip apply failed (repl root) phase=%s wt=%d", phase, wt);
                    return false;
                }
                heldPoseOkCount = 1;
                heldPoseTgt = "replRootFallback";
                RwMatrix* rm = RwFrameGetMatrix(rootF);
                if (rm) {
                    logAtX = rm->pos.x;
                    logAtY = rm->pos.y;
                    logAtZ = rm->pos.z;
                }
            } else {
                heldPoseTgt = "replEachAtomic";
                applyGunflashMuzzleDeltaToClump = true;
            }
        } else {
            HeldClumpApplyCtx ctx{};
            ctx.h = &h;
            ctx.gunflashRoot = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
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
                applyGunflashMuzzleDeltaToClump = true;
            }
        }
    } else {
        OrcLogInfoThrottled(434, 3000u, "held pose: skip unknown obj type phase=%s t=%d wt=%d", phase, (int)obj->type, wt);
        return false;
    }

    if (applyGunflashMuzzleDeltaToClump) {
        if (RpClump* gfClump = OrcPedResolveGunflashTargetClump(ped, wt))
            OrcHeldMaybeApplyGunflashFrameMuzzleDelta(ped, gfClump, wt);
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
static void __cdecl RenderWeaponCB_Detour(RpAtomic* atomic);
static RpClumpRender_fn g_RpClumpRender_Orig = nullptr;
static AtomicDefaultRender_fn g_AtomicDefaultRender_Orig = nullptr;
static RenderWeaponPedsForPC_fn g_RenderWeaponPedsForPC_Orig = nullptr;
static RenderWeaponCB_fn g_RenderWeaponCB_Orig = nullptr;
static bool s_renderingHeldReplacementClone = false;
struct HeldReplDrawFrameState {
    unsigned ms = 0;
    RpAtomic* drewPrimaryAtomic = nullptr;
    RpAtomic* drewSecondaryAtomic = nullptr;
};
static std::unordered_map<int, HeldReplDrawFrameState> s_heldReplDrawFrameByPedRef;
/// PRO twin: один вызов orig RWCB за кадр (обновляет LTM левого), затем оба клона.
static std::unordered_map<int, unsigned> s_heldDualRwcbOrigDoneFrameMsByPedRef;
/// Клон левой руки рисуем один раз за батч `RenderWeaponPedsForPC`, не по frameMs (иначе ложный dedup).
static std::unordered_map<int, uint64_t> s_heldSecCloneDrawnBatchByPedRef;
/// Twin pistols: счётчик `RpClumpRender` stock-clump за кадр (1-й проход — правый RWCB, 2-й — левый LTM).
static std::unordered_map<int, uint8_t> s_heldDualStockClumpPassByPedRef;
/// Dedup левого клона, когда `RenderWeaponPedsForPC` не в цепочке (`s_heldRwpfcBatchCounter==0`).
static std::unordered_map<int, unsigned> s_heldDualSecDrawnFrameMsByPedRef;

static void OrcHeldDestroySecondaryClone(HeldWeaponReplacementState& state);
static void OrcHeldEnsureSecondaryClone(CPed* ped, HeldWeaponReplacementState& state, WeaponReplacementAsset& asset);
static void OrcHeldRenderDualSecondaryClone(CPed* ped, HeldWeaponReplacementState& state, const char* phase);
static RpAtomic* OrcHeldFindOtherWeaponAtomicOnStock(RwObject* stockWo, RpAtomic* exclude, CPed* ped, int wt);
static void OrcHeldSyncStockWeaponClumpMatrices(CPed* ped, RwObject* stockWo);
static void OrcHeldLockDualStockAtomicsByHands(CPed* ped, HeldWeaponReplacementState& st, RwObject* stockWo);
static void OrcHeldSanitizeDualStockRoles(HeldWeaponReplacementState& st, RwObject* stockWo, CPed* ped);
static void OrcHeldTryDrawPendingDualSecondaryClonesAfterWeaponBatch();
static bool OrcHeldTryDrawLockedSecondaryClone(CPed* ped, HeldWeaponReplacementState& st, const char* phase);
static RwObject* OrcHeldResolveDualStockClumpForPed(CPed* ped, HeldWeaponReplacementState& st);
static bool OrcHeldIsDualWieldStockClumpForPed(CPed* ped, HeldWeaponReplacementState& st, RpClump* clump);
static void OrcHeldResetDualStockClumpPassForPed(int pedRef);
static uint8_t OrcHeldBumpDualStockClumpPass(CPed* ped, HeldWeaponReplacementState& st, RpClump* clump);
static void OrcHeldTryDrawDualSecondaryAfterStockClumpPass(CPed* ped, HeldWeaponReplacementState& st, uint8_t passNum);
static bool OrcHeldSecondaryAlreadyDrawnThisPass(int pedRef);
static void OrcHeldMarkSecondaryDrawnThisPass(int pedRef);
static bool OrcHeldDualSecondaryDrawSucceededForPed(int pedRef);
static bool OrcHeldClassifyDualStockAtomicDraw(CPed* ped,
    int wt,
    HeldWeaponReplacementState& st,
    RpAtomic* stockAtomic,
    RwObject* stock,
    bool matchesSlotStock,
    bool* outSecondaryHand);

static void OrcHeldReplacementInvalidateDrawStateForPed(int pedRef) {
    if (pedRef <= 0)
        return;
    s_heldReplDrawFrameByPedRef.erase(pedRef);
    s_heldSecCloneDrawnBatchByPedRef.erase(pedRef);
    s_heldDualSecDrawnFrameMsByPedRef.erase(pedRef);
    OrcHeldResetDualStockClumpPassForPed(pedRef);
}

static void OrcHeldReplacementInvalidateAllDrawState() {
    s_heldReplDrawFrameByPedRef.clear();
}

static bool OrcAtomicUsesRenderWeaponCb(RpAtomic* atomic) {
    if (!atomic || !atomic->renderCallBack)
        return false;
    const void* cb = reinterpret_cast<const void*>(atomic->renderCallBack);
    if (cb == reinterpret_cast<const void*>(kAddr_RenderWeaponCB))
        return true;
    if (g_RenderWeaponCB_Orig && cb == reinterpret_cast<const void*>(g_RenderWeaponCB_Orig))
        return true;
    if (cb == reinterpret_cast<const void*>(&RenderWeaponCB_Detour))
        return true;
    return false;
}

static bool OrcCallRenderWeaponCbOrigSafe(RpAtomic* atomic, const char* phaseTag) {
    if (!g_RenderWeaponCB_Orig || !atomic) {
        if (g_orcLogLevel >= OrcLogLevel::Info) {
            OrcLogInfoThrottled(828, 120u,
                "held rwcb: RenderWeaponCBOrig skip reason=%s phase=%s atomic=%p",
                !g_RenderWeaponCB_Orig ? "noOrigHook" : "nullAtomic", phaseTag ? phaseTag : "?", atomic);
        }
        return false;
    }
    __try {
        g_RenderWeaponCB_Orig(atomic);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("RenderWeaponCB orig crash phase=%s ex=0x%08X atomic=%p", phaseTag ? phaseTag : "?",
            GetExceptionCode(), atomic);
        if (g_AtomicDefaultRender_Orig) {
            __try {
                g_AtomicDefaultRender_Orig(atomic);
                if (g_orcLogLevel >= OrcLogLevel::Info) {
                    OrcLogInfoThrottled(806, 120u,
                        "held rwcb: fallback AtomicDefaultRender phase=%s atomic=%p",
                        phaseTag ? phaseTag : "?", atomic);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                OrcLogError("AtomicDefaultRender fallback crash phase=%s ex=0x%08X atomic=%p",
                    phaseTag ? phaseTag : "?", GetExceptionCode(), atomic);
            }
        }
        return false;
    }
}

static uint64_t s_heldRwpfcBatchCounter = 0;

/// Один раз за игровой тик на педа: несколько вызовов RpClumpRender за кадр (тени/зеркала) не накапливают дельту.
static std::unordered_set<int> s_heldPreRwDrawAppliedPedRefs;

/// Педы в радиусе для held pre-Rw / RWCB при `RenderAllPedsWeapons` — строится в `OrcHeldPoseBeginSimFrame`, без O(n) на каждый `RpClumpRender`.
static std::vector<CPed*> s_heldPreRwNearbyPeds;

/// Дедуп второго `pedRenderEvent.before` на том же `gameProcess`-тике (ветка `repl:reSync`).
static unsigned s_heldPrepSimSerial = 0;
static int s_heldReSyncDupPedRef = -1;
static unsigned s_heldReSyncDupSerial = 0;

/// Защита от цикла/битой иерархии `RwFrame` при обходе родителя.
static constexpr int kOrcMaxRwFrameAncestors = 64;

static RwFrame* OrcRwFrameGetParent(RwFrame* f) {
    if (!f)
        return nullptr;
    return reinterpret_cast<RwFrame*>(plugin::GetObjectParent(reinterpret_cast<RwObject*>(f)));
}

static bool OrcHeldRwFrameIsDescendantOf(RwFrame* frame, RwFrame* ancestor) {
    if (!frame || !ancestor)
        return false;
    int steps = 0;
    for (RwFrame* x = frame; x && steps < kOrcMaxRwFrameAncestors; x = OrcRwFrameGetParent(x), ++steps) {
        if (x == ancestor)
            return true;
    }
    return false;
}

/// `dw` в мировых осях → приращение `pos` дочернего `RwFrame` в локали родителя (ортогональная часть `parentLtm`).
static RwV3d OrcRwDeltaWorldToParentLocalPos(const RwMatrix* parentLtm, const RwV3d& dw) {
    RwV3d o{};
    if (!parentLtm) {
        o = dw;
        return o;
    }
    o.x = parentLtm->right.x * dw.x + parentLtm->right.y * dw.y + parentLtm->right.z * dw.z;
    o.y = parentLtm->up.x * dw.x + parentLtm->up.y * dw.y + parentLtm->up.z * dw.z;
    o.z = parentLtm->at.x * dw.x + parentLtm->at.y * dw.y + parentLtm->at.z * dw.z;
    return o;
}

// За один `gameProcess`-тик не дублируем nudge между preRw / RWCB, но baseline dummy держим стабильным.
// Иначе следующий выстрел берет `orig` из уже сдвинутого frame и вспышка начинает уплывать.
static std::unordered_set<uintptr_t> s_gunflashMuzzleNudgeAppliedClumps;
static std::unordered_map<uintptr_t, RwV3d> s_gunflashOrigLocalByGfFrame;

static void OrcHeldGunflashMuzzleDeltaResetForSimTick() {
    s_gunflashMuzzleNudgeAppliedClumps.clear();
}

void OrcHeldNudgeGunflashMuzzleDeltaAfterFrameSync(CPed* ped, int wt) {
    if (!g_enabled || !ped || wt <= 0)
        return;
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    if (!h.enabled)
        return;
    RpClump* clump = OrcPedResolveGunflashTargetClump(ped, wt);
    RwFrame* root = clump ? RpClumpGetFrame(clump) : nullptr;
    RwFrame* syncGf = ped->m_pGunflashObject;
    RwFrame* gf = nullptr;
    if (syncGf && root && OrcHeldRwFrameIsDescendantOf(syncGf, root))
        gf = syncGf;
    else if (clump)
        gf = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
    if (!gf)
        return;
    const uintptr_t gfk = reinterpret_cast<uintptr_t>(gf);
    s_gunflashMuzzleNudgeAppliedClumps.erase(gfk);
    OrcHeldMaybeApplyGunflashFrameMuzzleDelta(ped, clump, wt, gf);
}

static void OrcHeldMaybeApplyGunflashFrameMuzzleDelta(CPed* ped, RpClump* clump, int wt, RwFrame* gfOverride) {
    if (!g_enabled || !ped || wt <= 0)
        return;
    RwFrame* gf = gfOverride;
    if (!gf) {
        if (!clump)
            return;
        gf = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
    }
    if (!gf)
        return;
    const uintptr_t gfk = reinterpret_cast<uintptr_t>(gf);
    if (s_gunflashMuzzleNudgeAppliedClumps.count(gfk) != 0)
        return;
    RwV3d dw{};
    if (!OrcHeldTryGetMuzzleWorldDeltaHeldMinusVanilla(ped, wt, &dw))
        return;
    const float d2 = dw.x * dw.x + dw.y * dw.y + dw.z * dw.z;
    if (d2 < 1e-12f)
        return;
    RwFrame* par = OrcRwFrameGetParent(gf);
    const RwMatrix* parLtm = par ? RwFrameGetLTM(par) : nullptr;
    const RwV3d dpar = OrcRwDeltaWorldToParentLocalPos(parLtm, dw);
    RwMatrix* Lm = RwFrameGetMatrix(gf);
    // У dummy gunflash / FX-атомиков матрица часто почти вырождена по осям — `OrcRwMatrixAxesNonDegenerate` режет nudge целиком.
    if (!Lm || !OrcRwMatrixFinite(Lm)) {
        if (g_orcLogLevel >= OrcLogLevel::Info) {
            OrcLogInfoThrottled(941u, 4000u,
                "held gunflash: muzzle delta skip (bad Lm) pedRef=%d wt=%d clump=%p gf=%p dW=(%.4f,%.4f,%.4f)",
                CPools::GetPedRef(ped), wt, static_cast<void*>(clump), gf, dw.x, dw.y, dw.z);
        }
        return;
    }
    if (s_gunflashOrigLocalByGfFrame.find(gfk) == s_gunflashOrigLocalByGfFrame.end()) {
        RwV3d o{ Lm->pos.x, Lm->pos.y, Lm->pos.z };
        s_gunflashOrigLocalByGfFrame.emplace(gfk, o);
    }
    const RwV3d& orig = s_gunflashOrigLocalByGfFrame[gfk];
    __try {
        Lm->pos.x = orig.x + dpar.x;
        Lm->pos.y = orig.y + dpar.y;
        Lm->pos.z = orig.z + dpar.z;
        RwMatrixUpdate(Lm);
        RwFrameUpdateObjects(gf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcHeldMaybeApplyGunflashFrameMuzzleDelta: SEH ex=0x%08X", GetExceptionCode());
        return;
    }
    s_gunflashMuzzleNudgeAppliedClumps.insert(gfk);
    if (g_orcLogLevel >= OrcLogLevel::Info) {
        OrcLogInfoThrottled(940u, 1200u,
            "held gunflash: muzzle delta nudge pedRef=%d wt=%d clump=%p gf=%p dW=(%.4f,%.4f,%.4f) dPar=(%.4f,%.4f,%.4f)",
            CPools::GetPedRef(ped), wt, static_cast<void*>(clump), gf, dw.x, dw.y, dw.z, dpar.x, dpar.y, dpar.z);
    }
}

static bool OrcAtomicBelongsToClump(RpAtomic* atomic, RpClump* clump) {
    if (!atomic || !clump)
        return false;
    RwFrame* root = RpClumpGetFrame(clump);
    RwFrame* af = RpAtomicGetFrame(atomic);
    if (!root || !af)
        return false;
    int steps = 0;
    for (RwFrame* x = af; x && steps < kOrcMaxRwFrameAncestors; x = OrcRwFrameGetParent(x), ++steps) {
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
    int steps = 0;
    for (RwFrame* x = f; x && steps < kOrcMaxRwFrameAncestors; x = OrcRwFrameGetParent(x), ++steps) {
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

static void OrcClearHeldWeaponReplacementStateForPed(int pedRef, int wt, const char* reason) {
    if (pedRef <= 0)
        return;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end())
        return;
    HeldWeaponReplacementState& st = it->second;
    if (g_orcLogLevel >= OrcLogLevel::Info && (st.rwObject || st.weaponType > 0)) {
        OrcLogInfoThrottled(38, 1200u,
            "held wr: clear stale state reason=%s pedRef=%d curWt=%d oldWt=%d key=%s clone=%p",
            reason ? reason : "?",
            pedRef,
            wt,
            st.weaponType,
            st.replacementKey.c_str(),
            st.rwObject);
    }
    g_deferredHeldWeaponStockRestore.erase(pedRef);
    OrcDestroyRwObjectInstance(st.rwObject);
    OrcHeldDestroySecondaryClone(st);
    st.lastPrimaryStockAtomic = nullptr;
    st.lastSecondaryStockAtomic = nullptr;
    OrcHeldReplacementInvalidateDrawStateForPed(pedRef);
    g_heldWeaponReplacements.erase(it);
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

static bool OrcHeldAtomicMatchesWeaponModelIdTemplate(RpAtomic* atomic, int modelId) {
    if (!atomic || modelId <= 0 || !atomic->geometry)
        return false;
    CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
    if (!mi || !mi->m_pRwObject)
        return false;
    return OrcAtomicSharesGeometryWithWeaponObject(atomic, mi->m_pRwObject);
}

static bool OrcTryGetAtomicWorldPos(RpAtomic* atomic, CVector& out) {
    if (!atomic)
        return false;
    RwFrame* f = RpAtomicGetFrame(atomic);
    if (!f)
        return false;
    const RwMatrix* m = RwFrameGetLTM(f);
    if (!m)
        m = RwFrameGetMatrix(f);
    if (!m)
        return false;
    out.x = m->pos.x;
    out.y = m->pos.y;
    out.z = m->pos.z;
    return true;
}

static bool OrcAtomicWorldPosUsable(const CVector& p) {
    return (std::fabs(p.x) + std::fabs(p.y) + std::fabs(p.z)) > 0.05f;
}

static void OrcCopyPedHandLtmToCloneRoot(CPed* ped, bool leftHand, RwObject* clone) {
    if (!ped || !clone)
        return;
    RwMatrix* hand = OrcGetBoneMatrix(ped, leftHand ? BONE_L_HAND : BONE_R_HAND);
    RwFrame* df = GetRwObjectRootFrame(clone);
    if (!hand || !df)
        return;
    __try {
        std::memcpy(RwFrameGetMatrix(df), hand, sizeof(RwMatrix));
        RwMatrixUpdate(RwFrameGetMatrix(df));
        RwFrameUpdateObjects(df);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcCopyPedHandLtmToCloneRoot: SEH ex=0x%08X", GetExceptionCode());
    }
}

static float OrcHeldDistSqToBonePos(const CVector& p, const RwMatrix& bone) {
    const float dx = p.x - bone.pos.x;
    const float dy = p.y - bone.pos.y;
    const float dz = p.z - bone.pos.z;
    return dx * dx + dy * dy + dz * dz;
}

static void OrcHeldRotateCloneRoot(RwObject* clone, const RwV3d& axis, float radians) {
    if (!clone)
        return;
    RwFrame* df = GetRwObjectRootFrame(clone);
    RwMatrix* dm = df ? RwFrameGetMatrix(df) : nullptr;
    if (!dm)
        return;
    RwV3d ax = axis;
    const float len2 = ax.x * ax.x + ax.y * ax.y + ax.z * ax.z;
    if (len2 < 1e-8f)
        return;
    const float invLen = 1.0f / std::sqrt(len2);
    ax.x *= invLen;
    ax.y *= invLen;
    ax.z *= invLen;
    __try {
        RwMatrixRotate(dm, &ax, radians, rwCOMBINEPRECONCAT);
        RwMatrixUpdate(dm);
        RwFrameUpdateObjects(df);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcHeldRotateCloneRoot: SEH ex=0x%08X", GetExceptionCode());
    }
}

/// PRO twin: кастом-clone2 = тот же DFF, что справа → rot с pri + зеркало в ладони; ваниль — LTM sec stock.
static void OrcHeldCopyDualSecondaryLtmToCloneRoot(CPed* ped,
    RpAtomic* priStock,
    RpAtomic* secStock,
    RwObject* clone) {
    if (!ped || !secStock || !clone)
        return;

    const char* mode = "?";
    CVector secAp{};
    const bool hasSecAp = OrcTryGetAtomicWorldPos(secStock, secAp);
    RwMatrix* lHand = OrcGetBoneMatrix(ped, BONE_L_HAND);
    RwMatrix* rHand = OrcGetBoneMatrix(ped, BONE_R_HAND);

    if (priStock && HeldWeaponRwObjectIsReplacementClone(ped, clone)) {
        CopyStockAtomicLtmToCloneRoot(priStock, clone);
        mode = "priStock";
        if (lHand) {
            OrcHeldRotateCloneRoot(clone, RwV3d{ lHand->up.x, lHand->up.y, lHand->up.z }, kOrcPi);
            RwFrame* df = GetRwObjectRootFrame(clone);
            RwMatrix* dm = df ? RwFrameGetMatrix(df) : nullptr;
            if (dm) {
                dm->pos.x = lHand->pos.x;
                dm->pos.y = lHand->pos.y;
                dm->pos.z = lHand->pos.z;
                RwMatrixUpdate(dm);
                RwFrameUpdateObjects(df);
                mode = "priLHandYawMirror";
            }
        }
    } else {
        CopyStockAtomicLtmToCloneRoot(secStock, clone);
        mode = "stock";
        if (hasSecAp && lHand && rHand) {
            const float dl = OrcHeldDistSqToBonePos(secAp, *lHand);
            const float dr = OrcHeldDistSqToBonePos(secAp, *rHand);
            if (dr <= dl + 0.04f) {
                RwFrame* df = GetRwObjectRootFrame(clone);
                RwMatrix* dm = df ? RwFrameGetMatrix(df) : nullptr;
                if (dm) {
                    dm->pos.x = lHand->pos.x;
                    dm->pos.y = lHand->pos.y;
                    dm->pos.z = lHand->pos.z;
                    RwMatrixUpdate(dm);
                    RwFrameUpdateObjects(df);
                    mode = "stockPosLHand";
                }
            }
        }
    }

    if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
        OrcLogInfoThrottled(54, 3000u,
            "held wr: dual sec LTM mode=%s pedRef=%d secStock=%p priStock=%p clone2=%p ap=(%.2f,%.2f,%.2f)",
            mode,
            CPools::GetPedRef(ped),
            secStock,
            priStock,
            clone,
            secAp.x,
            secAp.y,
            secAp.z);
    }
}

static void OrcHeldCopyStockOrHandLtmToCloneRoot(CPed* ped,
    int wt,
    RpAtomic* priStock,
    RpAtomic* stockAtomic,
    RwObject* clone,
    bool secondaryHand) {
    if (!stockAtomic || !clone)
        return;
    if (secondaryHand && ped && wt > 0 && OrcPedWantsDualWieldHeld(ped, wt)) {
        OrcHeldCopyDualSecondaryLtmToCloneRoot(ped, priStock, stockAtomic, clone);
        return;
    }
    CopyStockAtomicLtmToCloneRoot(stockAtomic, clone);
}

static void OrcHeldPumpSecondaryStockAtomicMatrix(RpAtomic* secStock) {
    if (!secStock)
        return;
    __try {
        RwFrame* sf = RpAtomicGetFrame(secStock);
        if (sf)
            RwFrameUpdateObjects(sf);
        if (g_AtomicDefaultRender_Orig && !OrcAtomicUsesRenderWeaponCb(secStock))
            g_AtomicDefaultRender_Orig(secStock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcHeldPumpSecondaryStockAtomicMatrix: SEH ex=0x%08X", GetExceptionCode());
    }
}

static bool OrcHeldInferSecondaryHandFromAtomic(CPed* ped, RpAtomic* atomic, int wt) {
    if (!ped || !atomic || wt <= 0)
        return false;
    CWeaponInfo* wi1 = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    CWeaponInfo* wi2 = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 2);
    const int mid1 = wi1 ? wi1->m_nModelId : 0;
    int mid2 = wi2 ? wi2->m_nModelId2 : 0;
    if (mid2 <= 0 && wi1)
        mid2 = wi1->m_nModelId2;
    const bool match1 = mid1 > 0 && OrcHeldAtomicMatchesWeaponModelIdTemplate(atomic, mid1);
    const bool match2 = mid2 > 0 && mid2 != mid1 && OrcHeldAtomicMatchesWeaponModelIdTemplate(atomic, mid2);
    if (match2 && !match1)
        return true;
    if (match1 && !match2)
        return false;

    CVector apos{};
    if (!OrcTryGetAtomicWorldPos(atomic, apos))
        return false;
    RwMatrix* lHand = OrcGetBoneMatrix(ped, BONE_L_HAND);
    RwMatrix* rHand = OrcGetBoneMatrix(ped, BONE_R_HAND);
    if (!lHand || !rHand)
        return false;
    const float dl = (apos.x - lHand->pos.x) * (apos.x - lHand->pos.x) + (apos.y - lHand->pos.y) * (apos.y - lHand->pos.y) +
                     (apos.z - lHand->pos.z) * (apos.z - lHand->pos.z);
    const float dr = (apos.x - rHand->pos.x) * (apos.x - rHand->pos.x) + (apos.y - rHand->pos.y) * (apos.y - rHand->pos.y) +
                     (apos.z - rHand->pos.z) * (apos.z - rHand->pos.z);
    return dl < dr;
}

/// RWCB рисует атомы **того инстанса**, что сейчас в visibility-пайплайне.
/// Для replacement-клона применяем Held именно здесь как single-point late apply
/// (чтобы не накапливать матрицы в ранних `repl:*` фазах).
/// `wtSel` / `wo` снаружи (RWCB вызывается очень часто — не дублируем `OrcResolveHeldWeaponRwObject`).
static bool OrcHeldRwcbShouldApplyAtomicImpl(CPed* ped, RpAtomic* atomic, int wtSel, RwObject* wo) {
    if (!ped || !atomic || wtSel <= 0)
        return false;

    // Replacement clone: late apply только на его атомиках в RWCB.
    if (wo && HeldWeaponRwObjectIsReplacementClone(ped, wo) && OrcHeldAtomicMatchesPedWeaponObject(atomic, wo))
        return true;

    // Прямой матч / сток repl / геометрия слота — без `OrcAtomicAttachedUnderPedRwClump`: у RWCB атомов
    // иерархия `RwFrame` часто не поднимается до `RpClumpGetFrame(ped->m_pRwClump)` (оружие на кости вне этого
    // обхода родителей), иначе match всегда 0 и Held не работает.

    if (wo && OrcHeldAtomicMatchesPedWeaponObject(atomic, wo))
        return true;

    if (wo && HeldWeaponRwObjectIsReplacementClone(ped, wo))
        return false;

    if (wo && !HeldWeaponRwObjectIsReplacementClone(ped, wo) && OrcAtomicSharesGeometryWithWeaponObject(atomic, wo))
        return true;

    if (!OrcHeldAtomicMatchesWeaponModelTemplate(atomic, wtSel))
        return false;
    if (OrcPedWantsDualWieldHeld(ped, wtSel) && OrcHeldInferSecondaryHandFromAtomic(ped, atomic, wtSel))
        return true;
    return OrcAtomicAttachedUnderPedRwClump(atomic, ped);
}

static bool OrcApplyHeldPoseToRwcbAtomic(CPed* ped, RpAtomic* atomic, int wtSel, HeldRwcbFrameRestore* restore) {
    if (!g_enabled || !ped || !atomic || wtSel <= 0)
        return false;
    RwFrame* af = RpAtomicGetFrame(atomic);
    if (!af)
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    const bool secondaryHand =
        OrcPedWantsDualWieldHeld(ped, wtSel) && OrcHeldInferSecondaryHandFromAtomic(ped, atomic, wtSel);
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wtSel, secondaryHand);
    if (g_heldPoseDebug || g_heldWeaponTrace >= 2) {
        OrcLogInfoThrottled(
            492 + wtSel % 5, 15000u,
            "held chain: cfg phase=renderWeaponCbAtomic pedRef=%d wt=%d secondary=%d heldEn=%d atomic=%p frame=%p",
            pedRef, wtSel, secondaryHand ? 1 : 0, h.enabled ? 1 : 0, static_cast<void*>(atomic),
            static_cast<void*>(af));
    }
    if (!h.enabled) {
        OrcLogHeldPoseCfgDisabled(ped, wtSel);
        return false;
    }
    RpClump* gfClump = OrcPedResolveGunflashTargetClump(ped, wtSel);
    if (gfClump)
        OrcHeldMaybeApplyGunflashFrameMuzzleDelta(ped, gfClump, wtSel);
    RwFrame* gunf = gfClump ? CClumpModelInfo::GetFrameFromName(gfClump, "gunflash") : nullptr;
    if (gunf && OrcHeldAtomicFrameIsUnderGunflashDummy(af, gunf))
        return false;
    RwObject* wo = OrcResolveHeldWeaponRwObject(ped);
    const bool stockVisual = !wo || !HeldWeaponRwObjectIsReplacementClone(ped, wo);
    if (restore && stockVisual) {
        if (RwMatrix* m = RwFrameGetMatrix(af)) {
            restore->frame = af;
            restore->matrix = *m;
            restore->active = OrcRwMatrixFinite(&restore->matrix);
        }
        OrcHeldPoseInvalidateBaselineForRwFrame(af);
    }
    if (!OrcTryApplyHeldPoseOneFrame(af, h)) {
        if (restore)
            restore->active = false;
        OrcLogInfoThrottled(493, 2500u, "held pose: skip apply failed (rwcbAtomic) wt=%d pedRef=%d", wtSel, pedRef);
        return false;
    }
    if (g_heldPoseDebug) {
        RwMatrix* mm = RwFrameGetMatrix(af);
        const float r2d = 180.0f / kOrcPi;
        OrcLogInfoThrottled(437, 4000u,
            "held pose: apply phase=renderWeaponCbAtomic wt=%d tgt=rwcbAtomic xyz=%.3f %.3f %.3f deg=%.2f %.2f %.2f sc=%.3f | at=%.3f %.3f %.3f",
            wtSel, h.x, h.y, h.z, h.rx * r2d, h.ry * r2d, h.rz * r2d, h.scale, mm ? mm->pos.x : 0.0f, mm ? mm->pos.y : 0.0f,
            mm ? mm->pos.z : 0.0f);
    }
    if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 2) {
        OrcLogInfoThrottled(711, 5000u,
            "held rwcb: atomic phase pedRef=%d wt=%d bFiring=%d slotWo=%p",
            pedRef, wtSel, ped->bFiringWeapon ? 1 : 0, ped->m_pWeaponObject);
    }
    return true;
}

static void OrcHeldLogPreRwSkipThrottled(int slot, CPed* ped, const char* reason) {
    if (!g_heldPoseDebug)
        return;
    const int pr = ped ? CPools::GetPedRef(ped) : 0;
    OrcLogInfoThrottled(slot, 8000u, "held preRw: skip reason=%s ped=%p pedRef=%d", reason ? reason : "?", ped, pr);
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
        reinterpret_cast<RpClump*>(pl->m_pWeaponObject) == clump &&
        !HeldWeaponRwObjectIsReplacementClone(pl, pl->m_pWeaponObject))
        return true;
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    if (!s_heldPreRwNearbyPeds.empty()) {
        for (CPed* p : s_heldPreRwNearbyPeds) {
            if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpCLUMP &&
                !HeldWeaponRwObjectIsReplacementClone(p, p->m_pWeaponObject) &&
                reinterpret_cast<RpClump*>(p->m_pWeaponObject) == clump)
                return true;
        }
        return false;
    }
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpCLUMP &&
            !HeldWeaponRwObjectIsReplacementClone(p, p->m_pWeaponObject) &&
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
        reinterpret_cast<RpAtomic*>(pl->m_pWeaponObject) == atomic &&
        !HeldWeaponRwObjectIsReplacementClone(pl, pl->m_pWeaponObject))
        return true;
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    if (!s_heldPreRwNearbyPeds.empty()) {
        for (CPed* p : s_heldPreRwNearbyPeds) {
            if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpATOMIC &&
                !HeldWeaponRwObjectIsReplacementClone(p, p->m_pWeaponObject) &&
                reinterpret_cast<RpAtomic*>(p->m_pWeaponObject) == atomic)
                return true;
        }
        return false;
    }
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpATOMIC &&
            !HeldWeaponRwObjectIsReplacementClone(p, p->m_pWeaponObject) &&
            reinterpret_cast<RpAtomic*>(p->m_pWeaponObject) == atomic)
            return true;
    }
    return false;
}

/// `outLocalPlayerRwcbMatch` — для `HeldWeaponTrace`: совпадение атома с локальным `wo` без второго прохода `ShouldApply`.
static bool OrcTryApplyHeldPoseForRenderWeaponAtomic(RpAtomic* atomic, bool* outLocalPlayerRwcbMatch,
    HeldRwcbFrameRestore* restore) {
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
        return OrcApplyHeldPoseToRwcbAtomic(pl, atomic, wtSel, restore);
    }
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    auto tryPed = [&](CPed* p) -> bool {
        if (!p)
            return false;
        const int wtSel = GetPedSelectedWeaponTypeForReplace(p);
        if (wtSel <= 0)
            return false;
        const int pr = CPools::GetPedRef(p);
        if (pr <= 0)
            return false;
        RwObject* wo = OrcResolveHeldWeaponRwObject(p);
        if (!OrcHeldRwcbShouldApplyAtomicImpl(p, atomic, wtSel, wo))
            return false;
        return OrcApplyHeldPoseToRwcbAtomic(p, atomic, wtSel, restore);
    };
    if (!s_heldPreRwNearbyPeds.empty()) {
        for (CPed* p : s_heldPreRwNearbyPeds) {
            if (tryPed(p))
                return true;
        }
        return false;
    }
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (tryPed(p))
            return true;
    }
    return false;
}

static void OrcHeldTraceLogRenderWeaponCb(RpAtomic* atomic, bool appliedHeldPose, bool rwcbMatchLocalPl) {
    if (g_heldWeaponTrace < 1 || g_orcLogLevel < OrcLogLevel::Info)
        return;
    const unsigned iv = (g_heldWeaponTrace >= 2) ? 5000u : 10000u;
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
    bool traceSecondary = false;
    float hx = 0.0f, hy = 0.0f, hz = 0.0f, hrx = 0.0f, hry = 0.0f, hrz = 0.0f, hsc = 1.0f;
    if (wt > 0) {
        traceSecondary =
            OrcPedWantsDualWieldHeld(pl, wt) && OrcHeldInferSecondaryHandFromAtomic(pl, atomic, wt);
        const HeldWeaponPoseCfg& h = GetHeldPoseForPed(pl, wt, traceSecondary);
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
        "wo=%p woT=%d repl=%d slot=%u slotWt=%d visWt=%d secondary=%d heldCfgEn=%d off=(%.3f,%.3f,%.3f) rotDeg=(%.1f,%.1f,%.1f) "
        "sc=%.3f appliedPose=%d",
        static_cast<void*>(atomic), static_cast<void*>(fr), rdcb, pr, skinIni.c_str(), skinRaw.c_str(),
        rwcbMatchLocalPl ? 1 : 0,
        woP, woT, repl ? 1 : 0, slot, slotWt, wt, traceSecondary ? 1 : 0, heldCfgEn ? 1 : 0, hx, hy, hz, hrx * r2d,
        hry * r2d, hrz * r2d, hsc, appliedHeldPose ? 1 : 0);
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
        // Replacement clone тоже обязан проходить preRw-point: это фактическая late draw-точка.
        // Без этого у части custom-моделей GetHeldPoseForPed резолвится, но pose не попадает в их реальный render-pass.
        OrcHeldLogPreRwSkipThrottled(475, ped, "repl_clone_preRw_apply");
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
    if (OrcHeldWeaponClumpUsesRenderWeaponCbOnly(clump)) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(490, 2000u, "held preRw: skip clump=%p reason=rwcbOnly_nonReplacement", clump);
        return;
    }
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && pl->m_pWeaponObject && pl->m_pWeaponObject->type == rpCLUMP &&
        reinterpret_cast<RpClump*>(pl->m_pWeaponObject) == clump) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(488, 4000u, "held preRw: rwMatch mesh=clump clump=%p pedRef=%d", clump,
                CPools::GetPedRef(pl));
        OrcTryApplyHeldPoseBeforeRwDrawForPed(pl);
        return;
    }
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return;
    if (!s_heldPreRwNearbyPeds.empty()) {
        for (CPed* p : s_heldPreRwNearbyPeds) {
            if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpCLUMP &&
                reinterpret_cast<RpClump*>(p->m_pWeaponObject) == clump) {
                OrcTryApplyHeldPoseBeforeRwDrawForPed(p);
                return;
            }
        }
        return;
    }
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

static void OrcTryApplyHeldPoseIfAtomicMatches(RpAtomic* atomic, HeldRwcbFrameRestore* restore) {
    if (!atomic)
        return;
    // Unified atomic matching/apply path (same matcher as RenderWeaponCB):
    // needed for replacement clump atomics that may skip RWCB and go through AtomicDefaultRenderCallBack.
    if (OrcTryApplyHeldPoseForRenderWeaponAtomic(atomic, nullptr, restore))
        return;
    if (OrcHeldWeaponAtomicUsesRenderWeaponCbOnly(atomic)) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(491, 2000u, "held preRw: skip atomic=%p reason=rwcbOnly_nonReplacement", atomic);
        return;
    }
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && pl->m_pWeaponObject && pl->m_pWeaponObject->type == rpATOMIC &&
        reinterpret_cast<RpAtomic*>(pl->m_pWeaponObject) == atomic) {
        if (g_heldPoseDebug)
            OrcLogInfoThrottled(489, 4000u, "held preRw: rwMatch mesh=atomic atomic=%p pedRef=%d", atomic,
                CPools::GetPedRef(pl));
        OrcTryApplyHeldPoseBeforeRwDrawForPed(pl);
        return;
    }
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return;
    if (!s_heldPreRwNearbyPeds.empty()) {
        for (CPed* p : s_heldPreRwNearbyPeds) {
            if (p->m_pWeaponObject && p->m_pWeaponObject->type == rpATOMIC &&
                reinterpret_cast<RpAtomic*>(p->m_pWeaponObject) == atomic) {
                OrcTryApplyHeldPoseBeforeRwDrawForPed(p);
                return;
            }
        }
        return;
    }
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

struct HeldReplacementDrawContext {
    CPed* ped = nullptr;
    int pedRef = 0;
    HeldWeaponReplacementState* state = nullptr;
    RwObject* stock = nullptr;
    RpAtomic* stockAtomic = nullptr;
    int weaponType = 0;
    bool secondaryHand = false;
};

static bool OrcTryDrawDualHeldReplacementViaRwcb(RpAtomic* atomic, HeldReplacementDrawContext& ctx);

static bool OrcTryGetHeldReplacementDrawContextForPed(CPed* ped, RpAtomic* stockAtomic, HeldReplacementDrawContext& out) {
    if (!ped || !stockAtomic || s_renderingHeldReplacementClone)
        return false;
    if (!ShouldReplaceHeldWeaponForPed(ped))
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return false;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end())
        return false;
    HeldWeaponReplacementState& st = it->second;
    if (!st.rwObject || st.weaponType <= 0)
        return false;
    const int wtCur = OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wtCur <= 0 || wtCur != st.weaponType)
        return false;
    if (!OrcHeldReplacementStateMatchesResolvedChoice(ped, wtCur, st))
        return false;

    RwObject* stock = OrcHeldGetReplacementStockObject(pedRef, st);
    if (!stock && ped->m_pWeaponObject && ped->m_pWeaponObject != st.rwObject &&
        !OrcHeldIsAnyReplacementCloneObject(ped->m_pWeaponObject)) {
        stock = ped->m_pWeaponObject;
    }

    bool matchesSlotStock = false;
    if (stock && stock != st.rwObject && !OrcHeldIsAnyReplacementCloneObject(stock)) {
        matchesSlotStock = OrcHeldAtomicMatchesPedWeaponObject(stockAtomic, stock) ||
                           OrcAtomicSharesGeometryWithWeaponObject(stockAtomic, stock);
    }

    if (!matchesSlotStock) {
        if (OrcPedWantsDualWieldHeld(ped, wtCur) && g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(41, 5000u,
                "held wr: dual ctx reject pedRef=%d wt=%d stockAtomic=%p slotStock=%d rwcb=%d",
                pedRef,
                wtCur,
                stockAtomic,
                matchesSlotStock ? 1 : 0,
                OrcAtomicUsesRenderWeaponCb(stockAtomic) ? 1 : 0);
        }
        return false;
    }

    if (OrcPedWantsDualWieldHeld(ped, wtCur) && stock && stock->type == rpCLUMP)
        OrcHeldLockDualStockAtomicsByHands(ped, st, stock);

    bool secondaryHand = false;
    if (!OrcHeldClassifyDualStockAtomicDraw(ped, wtCur, st, stockAtomic, stock, true, &secondaryHand))
        secondaryHand = false;

    out.ped = ped;
    out.pedRef = pedRef;
    out.state = &st;
    out.stock = stock;
    out.stockAtomic = stockAtomic;
    out.weaponType = wtCur;
    out.secondaryHand = secondaryHand;
    return true;
}

static bool OrcTryGetHeldReplacementDrawContext(RpAtomic* stockAtomic, HeldReplacementDrawContext& out) {
    if (!stockAtomic)
        return false;
    CPlayerPed* pl = FindPlayerPed(0);
    if (pl && OrcTryGetHeldReplacementDrawContextForPed(pl, stockAtomic, out))
        return true;
    if (!g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return false;
    if (!s_heldPreRwNearbyPeds.empty()) {
        for (CPed* p : s_heldPreRwNearbyPeds) {
            if (p != pl && OrcTryGetHeldReplacementDrawContextForPed(p, stockAtomic, out))
                return true;
        }
        return false;
    }
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || p == pl || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        if (OrcTryGetHeldReplacementDrawContextForPed(p, stockAtomic, out))
            return true;
    }
    return false;
}

static bool OrcRenderHeldReplacementCloneAtStockDraw(const HeldReplacementDrawContext& ctx, const char* phase) {
    if (!ctx.ped || ctx.pedRef <= 0 || !ctx.state || !ctx.state->rwObject || !ctx.stockAtomic || ctx.weaponType <= 0)
        return false;

    if (ctx.secondaryHand) {
        if (WeaponReplacementAsset* asset =
                OrcResolveUsableWeaponReplacementAssetForPed(ctx.ped, ctx.weaponType, true))
            OrcHeldEnsureSecondaryClone(ctx.ped, *ctx.state, *asset);
    }

    RwObject* drawClone = ctx.secondaryHand ? ctx.state->rwObjectSecondary : ctx.state->rwObject;
    if (!drawClone)
        return false;

    const unsigned frameMs = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    HeldReplDrawFrameState& drawFrame = s_heldReplDrawFrameByPedRef[ctx.pedRef];
    if (drawFrame.ms != frameMs) {
        drawFrame.ms = frameMs;
        drawFrame.drewPrimaryAtomic = nullptr;
        drawFrame.drewSecondaryAtomic = nullptr;
    }
    if (!ctx.secondaryHand) {
        if (drawFrame.drewPrimaryAtomic == ctx.stockAtomic) {
            if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
                OrcLogInfoThrottled(39, 2500u,
                    "held wr: draw-swap dedup suppress orig phase=%s pedRef=%d wt=%d stockAtomic=%p secondary=0 frameMs=%u",
                    phase ? phase : "?",
                    ctx.pedRef,
                    ctx.weaponType,
                    ctx.stockAtomic,
                    frameMs);
            }
            return true;
        }
    } else if (drawFrame.drewSecondaryAtomic == ctx.stockAtomic) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(40, 2500u,
                "held wr: draw-swap dedup suppress orig phase=%s pedRef=%d wt=%d stockAtomic=%p secondary=1 frameMs=%u",
                phase ? phase : "?",
                ctx.pedRef,
                ctx.weaponType,
                ctx.stockAtomic,
                frameMs);
        }
        return true;
    }

    if (ctx.stock)
        OrcHeldSyncStockWeaponClumpMatrices(ctx.ped, ctx.stock);
    if (ctx.stockAtomic)
        OrcHeldCopyStockOrHandLtmToCloneRoot(ctx.ped,
            ctx.weaponType,
            ctx.state ? ctx.state->lastPrimaryStockAtomic : nullptr,
            ctx.stockAtomic,
            drawClone,
            ctx.secondaryHand);
    else if (ctx.stock)
        CopyStockHeldWeaponRwMatricesToClone(ctx.stock, drawClone, false);
    OrcHeldPoseInvalidateBaselineForRwObject(drawClone);
    OrcApplyHeldPoseToWeaponObject(ctx.ped,
        drawClone,
        phase ? phase : "repl:drawSwap",
        ctx.weaponType,
        ctx.secondaryHand);
    if (!ctx.secondaryHand)
        ctx.state->poseSynced = true;

    CVector lightPos{};
    if (!OrcTryGetRwObjectRootWorldPos(drawClone, lightPos)) {
        CVector bc{};
        ctx.ped->GetBoundCentre(bc);
        lightPos = bc;
    }

    if (g_weaponTexturesEnabled) {
        WeaponTextureAsset* tex = OrcResolveUsableWeaponTextureAssetForPed(ctx.ped,
            ctx.weaponType,
            true,
            ctx.state->replacementKey.empty() ? nullptr : &ctx.state->replacementKey);
        OrcApplyWeaponTexturesCombined(ctx.ped, ctx.weaponType, drawClone, tex, true);
    }
    OrcApplyAttachmentLightingForPed(ctx.ped, lightPos, 0.5f);

    bool drew = false;
    s_renderingHeldReplacementClone = true;
    __try {
        if (drawClone->type == rpCLUMP) {
            RpClump* clump = reinterpret_cast<RpClump*>(drawClone);
            RpClumpForAllAtomics(clump, OrcPrepAtomicCB, nullptr);
            OrcTryRpClumpRender(clump);
            drew = true;
        } else if (drawClone->type == rpATOMIC) {
            RpAtomic* atomic = reinterpret_cast<RpAtomic*>(drawClone);
            OrcPrepAtomicCB(atomic, nullptr);
            if (atomic->renderCallBack) {
                atomic->renderCallBack(atomic);
                drew = true;
            } else if (g_AtomicDefaultRender_Orig) {
                g_AtomicDefaultRender_Orig(atomic);
                drew = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcRenderHeldReplacementCloneAtStockDraw: SEH ex=0x%08X", GetExceptionCode());
        drew = false;
    }
    s_renderingHeldReplacementClone = false;
    OrcRestoreWeaponTextureOverrides();

    if (!drew) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(49, 3000u,
                "held wr: draw-swap drew=0 phase=%s pedRef=%d wt=%d stockAtomic=%p secondary=%d clone=%p",
                phase ? phase : "?",
                ctx.pedRef,
                ctx.weaponType,
                ctx.stockAtomic,
                ctx.secondaryHand ? 1 : 0,
                drawClone);
        }
        return false;
    }

    if (ctx.secondaryHand) {
        drawFrame.drewSecondaryAtomic = ctx.stockAtomic;
        OrcHeldMarkSecondaryDrawnThisPass(ctx.pedRef);
    } else {
        drawFrame.drewPrimaryAtomic = ctx.stockAtomic;
    }
    drawFrame.ms = frameMs;
    if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
        const unsigned logId = ctx.secondaryHand ? 538u : 37u;
        OrcLogInfoThrottled(logId, 2500u,
            "held wr: draw-swap phase=%s pedRef=%d wt=%d stock=%p stockAtomic=%p secondary=%d clone=%p drew=1 frameMs=%u",
            phase ? phase : "?",
            ctx.pedRef,
            ctx.weaponType,
            ctx.stock,
            ctx.stockAtomic,
            ctx.secondaryHand ? 1 : 0,
            drawClone,
            frameMs);
    }
    return true;
}

static bool OrcTryDrawHeldReplacementInsteadOfStockAtomic(RpAtomic* atomic, const char* phase) {
    HeldReplacementDrawContext ctx{};
    if (!OrcTryGetHeldReplacementDrawContext(atomic, ctx))
        return false;
    return OrcRenderHeldReplacementCloneAtStockDraw(ctx, phase);
}

static bool OrcHeldDualRwcbOrigDoneThisFrame(int pedRef) {
    if (pedRef <= 0)
        return false;
    const unsigned frameMs = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    const auto it = s_heldDualRwcbOrigDoneFrameMsByPedRef.find(pedRef);
    return it != s_heldDualRwcbOrigDoneFrameMsByPedRef.end() && it->second == frameMs;
}

/// Twin PRO: первый RWCB за кадр (на pri или sec атоме) — orig на primary RWCB, затем clone1+clone2.
static bool OrcTryDrawDualHeldReplacementViaRwcb(RpAtomic* atomic, HeldReplacementDrawContext& ctx) {
    if (!atomic || !ctx.ped || !ctx.state || !ctx.stock || !ctx.state->rwObjectSecondary)
        return false;
    if (!OrcPedWantsDualWieldHeld(ctx.ped, ctx.weaponType))
        return false;
    if (!OrcAtomicUsesRenderWeaponCb(atomic))
        return false;
    if (OrcHeldDualRwcbOrigDoneThisFrame(ctx.pedRef))
        return true;

    RpAtomic* lockedPri = nullptr;
    RpAtomic* lockedSec = nullptr;
    if (ctx.stock->type == rpCLUMP) {
        OrcHeldLockDualStockAtomicsByHands(ctx.ped, *ctx.state, ctx.stock);
        lockedPri = ctx.state->lastPrimaryStockAtomic;
        lockedSec = ctx.state->lastSecondaryStockAtomic;
    }

    RpAtomic* priAtomic = lockedPri;
    if (!priAtomic || !OrcAtomicUsesRenderWeaponCb(priAtomic)) {
        priAtomic = atomic;
        ctx.state->lastPrimaryStockAtomic = atomic;
    }
    if (!lockedSec || lockedSec == priAtomic) {
        lockedSec = OrcHeldFindOtherWeaponAtomicOnStock(ctx.stock, priAtomic, ctx.ped, ctx.weaponType);
    }
    if (lockedSec && lockedSec != priAtomic)
        ctx.state->lastSecondaryStockAtomic = lockedSec;

    ++s_heldRenderWeaponCbDepth;
    RpAtomic* prevRwcbAtomic = s_heldRenderWeaponCbAtomic;
    s_heldRenderWeaponCbAtomic = priAtomic;
    OrcCallRenderWeaponCbOrigSafe(priAtomic, "rwcb:dualOrig");
    s_heldRenderWeaponCbAtomic = prevRwcbAtomic;
    --s_heldRenderWeaponCbDepth;

    s_heldDualRwcbOrigDoneFrameMsByPedRef[ctx.pedRef] = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    OrcHeldSyncStockWeaponClumpMatrices(ctx.ped, ctx.stock);
    ctx.state->lastPrimaryStockAtomic = priAtomic;
    if (lockedSec && lockedSec != priAtomic)
        ctx.state->lastSecondaryStockAtomic = lockedSec;
    else if (!ctx.state->lastSecondaryStockAtomic || ctx.state->lastSecondaryStockAtomic == priAtomic) {
        ctx.state->lastSecondaryStockAtomic =
            OrcHeldFindOtherWeaponAtomicOnStock(ctx.stock, priAtomic, ctx.ped, ctx.weaponType);
    }

    HeldReplDrawFrameState& drawFrame = s_heldReplDrawFrameByPedRef[ctx.pedRef];
    drawFrame.ms = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    drawFrame.drewPrimaryAtomic = nullptr;
    drawFrame.drewSecondaryAtomic = nullptr;

    HeldReplacementDrawContext priCtx = ctx;
    priCtx.stockAtomic = ctx.state->lastPrimaryStockAtomic;
    priCtx.secondaryHand = false;
      OrcRenderHeldReplacementCloneAtStockDraw(priCtx, "repl:rwcbDualPri");

    RpAtomic* secAtomic = lockedSec;
    if (!secAtomic || secAtomic == priAtomic)
        secAtomic = ctx.state->lastSecondaryStockAtomic;
    if ((!secAtomic || secAtomic == priAtomic) && ctx.stock->type == rpCLUMP) {
        secAtomic = OrcHeldFindOtherWeaponAtomicOnStock(ctx.stock, priAtomic, ctx.ped, ctx.weaponType);
        if (secAtomic)
            ctx.state->lastSecondaryStockAtomic = secAtomic;
    }
    if (secAtomic && secAtomic != priAtomic) {
        OrcHeldPumpSecondaryStockAtomicMatrix(secAtomic);
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(53, 2000u,
                "held wr: rwcbDualSec begin pedRef=%d wt=%d sec=%p pri=%p clone2=%p",
                ctx.pedRef,
                ctx.weaponType,
                secAtomic,
                priAtomic,
                ctx.state->rwObjectSecondary);
        }
        HeldReplacementDrawContext secCtx = ctx;
        secCtx.stockAtomic = secAtomic;
        secCtx.secondaryHand = true;
        if (!OrcRenderHeldReplacementCloneAtStockDraw(secCtx, "repl:rwcbDualSec") &&
            g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(51, 2000u,
                "held wr: rwcbDualSec draw failed pedRef=%d wt=%d sec=%p pri=%p clone2=%p",
                ctx.pedRef,
                ctx.weaponType,
                secAtomic,
                ctx.state->lastPrimaryStockAtomic,
                ctx.state->rwObjectSecondary);
        }
    } else if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
        OrcLogInfoThrottled(52, 2000u,
            "held wr: rwcbDualSec skip pedRef=%d wt=%d sec=%p pri=%p lockedSec=%p",
            ctx.pedRef,
            ctx.weaponType,
            secAtomic,
            priAtomic,
            lockedSec);
    }
    return true;
}

static bool OrcHeldTryDrawLockedSecondaryClone(CPed* ped, HeldWeaponReplacementState& st, const char* phase) {
    if (!ped || !st.rwObjectSecondary || st.weaponType <= 0)
        return false;
    RwObject* stockWo = OrcHeldResolveDualStockClumpForPed(ped, st);
    if (stockWo) {
        st.lastPrimaryStockAtomic = nullptr;
        st.lastSecondaryStockAtomic = nullptr;
        OrcHeldLockDualStockAtomicsByHands(ped, st, stockWo);
    }
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return false;
    OrcHeldRenderDualSecondaryClone(ped, st, phase);
    return OrcHeldDualSecondaryDrawSucceededForPed(pedRef);
}

static bool OrcHeldDualAtomicIsLeftStockHand(CPed* ped,
    HeldWeaponReplacementState& st,
    RpAtomic* atomic,
    RwObject* stockWo) {
    if (!ped || !atomic || !stockWo || OrcAtomicUsesRenderWeaponCb(atomic))
        return false;
    if (!st.lastPrimaryStockAtomic || !st.lastSecondaryStockAtomic)
        OrcHeldLockDualStockAtomicsByHands(ped, st, stockWo);
    if (st.lastSecondaryStockAtomic && atomic == st.lastSecondaryStockAtomic)
        return true;
    if (atomic == st.lastPrimaryStockAtomic)
        return false;
    if (!OrcHeldAtomicMatchesPedWeaponObject(atomic, stockWo) &&
        !OrcAtomicSharesGeometryWithWeaponObject(atomic, stockWo))
        return false;
    if (st.lastSecondaryStockAtomic && atomic != st.lastSecondaryStockAtomic) {
        RpAtomic* other = OrcHeldFindOtherWeaponAtomicOnStock(stockWo, st.lastPrimaryStockAtomic, ped, st.weaponType);
        if (other && other == st.lastSecondaryStockAtomic)
            return false;
    }
    return true;
}

/// Левый twin-pistol в покое: AtomicDefault (без RWCB) — подавить ваниль и нарисовать clone2 в том же колбэке.
static bool OrcHeldSuppressAndDrawDualSecondaryViaAtomicDefault(RpAtomic* atomic) {
    if (!atomic || !g_enabled || !g_weaponReplacementEnabled)
        return false;
    for (auto& kv : g_heldWeaponReplacements) {
        CPed* ped = CPools::GetPed(kv.first);
        HeldWeaponReplacementState& st = kv.second;
        if (!ped || !st.rwObjectSecondary || st.weaponType <= 0)
            continue;
        if (!OrcPedWantsDualWieldHeld(ped, st.weaponType))
            continue;
        RwObject* stockWo = OrcHeldResolveDualStockClumpForPed(ped, st);
        if (!stockWo || OrcAtomicUsesRenderWeaponCb(atomic))
            continue;
        if (!OrcHeldDualAtomicIsLeftStockHand(ped, st, atomic, stockWo))
            continue;
        st.lastSecondaryStockAtomic = atomic;
        OrcHeldRenderDualSecondaryClone(ped, st, "repl:atomicSec");
        return true;
    }
    return false;
}

static bool OrcHeldIsDualWieldStockClumpForPed(CPed* ped, HeldWeaponReplacementState& st, RpClump* clump) {
    if (!ped || !clump || st.weaponType <= 0 || !OrcPedWantsDualWieldHeld(ped, st.weaponType))
        return false;
    RwObject* stockWo = OrcHeldResolveDualStockClumpForPed(ped, st);
    if (stockWo && stockWo->type == rpCLUMP && reinterpret_cast<RpClump*>(stockWo) == clump)
        return true;
    RwObject* slotWo = ped->m_pWeaponObject;
    if (slotWo && slotWo->type == rpCLUMP && !OrcHeldIsAnyReplacementCloneObject(slotWo) &&
        reinterpret_cast<RpClump*>(slotWo) == clump)
        return true;
    return false;
}

static bool OrcHeldSecondaryAlreadyDrawnThisPass(int pedRef) {
    if (pedRef <= 0)
        return false;
    if (s_heldRwpfcBatchCounter != 0)
        return s_heldSecCloneDrawnBatchByPedRef[pedRef] == s_heldRwpfcBatchCounter;
    const unsigned frameMs = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    const auto it = s_heldDualSecDrawnFrameMsByPedRef.find(pedRef);
    return it != s_heldDualSecDrawnFrameMsByPedRef.end() && it->second == frameMs;
}

static void OrcHeldMarkSecondaryDrawnThisPass(int pedRef) {
    if (pedRef <= 0)
        return;
    if (s_heldRwpfcBatchCounter != 0)
        s_heldSecCloneDrawnBatchByPedRef[pedRef] = s_heldRwpfcBatchCounter;
    else
        s_heldDualSecDrawnFrameMsByPedRef[pedRef] = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
}

static bool OrcHeldDualSecondaryDrawSucceededForPed(int pedRef) {
    return OrcHeldSecondaryAlreadyDrawnThisPass(pedRef);
}

static void OrcHeldResetDualStockClumpPassForPed(int pedRef) {
    if (pedRef > 0)
        s_heldDualStockClumpPassByPedRef.erase(pedRef);
}

static uint8_t OrcHeldBumpDualStockClumpPass(CPed* ped, HeldWeaponReplacementState& st, RpClump* clump) {
    if (!ped || !clump)
        return 0;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0 || !OrcHeldIsDualWieldStockClumpForPed(ped, st, clump))
        return 0;
    uint8_t& pass = s_heldDualStockClumpPassByPedRef[pedRef];
    if (pass < 255)
        ++pass;
    if (pass >= 2) {
        s_heldDualSecDrawnFrameMsByPedRef.erase(pedRef);
        s_heldSecCloneDrawnBatchByPedRef.erase(pedRef);
    }
    return pass;
}

static void OrcHeldTryDrawDualSecondaryAfterStockClumpPass(CPed* ped, HeldWeaponReplacementState& st, uint8_t passNum) {
    if (!ped || passNum < 2 || !st.rwObjectSecondary)
        return;
    RwObject* stockWo = OrcHeldResolveDualStockClumpForPed(ped, st);
    if (stockWo) {
        if (!st.lastPrimaryStockAtomic || !st.lastSecondaryStockAtomic)
            OrcHeldLockDualStockAtomicsByHands(ped, st, stockWo);
        OrcHeldSanitizeDualStockRoles(st, stockWo, ped);
        OrcHeldSyncStockWeaponClumpMatrices(ped, stockWo);
    }
    OrcHeldRenderDualSecondaryClone(ped, st, "repl:dualSecClumpPass2");
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef > 0)
        s_heldDualStockClumpPassByPedRef[pedRef] = 0;
}

static RpClump* __cdecl RpClumpRender_Detour(RpClump* clump) {
    CPed* dualPassPed = nullptr;
    HeldWeaponReplacementState* dualPassState = nullptr;
    uint8_t dualPassNum = 0;
    if (g_enabled && !s_renderingHeldReplacementClone && clump) {
        const bool allPeds = g_renderAllPedsWeapons;
        CPlayerPed* pl = FindPlayerPed(0);
        if (allPeds || (pl && pl->m_pWeaponObject)) {
            __try {
                OrcTryApplyHeldPoseIfClumpMatches(clump);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                OrcLogError("RpClumpRender_Detour: SEH ex=0x%08X", GetExceptionCode());
            }
        }
        if (g_enabled && g_weaponReplacementEnabled) {
            for (auto& kv : g_heldWeaponReplacements) {
                HeldWeaponReplacementState& st = kv.second;
                if (!st.rwObjectSecondary)
                    continue;
                CPed* p = CPools::GetPed(kv.first);
                if (!p)
                    continue;
                dualPassNum = OrcHeldBumpDualStockClumpPass(p, st, clump);
                if (dualPassNum > 0) {
                    dualPassPed = p;
                    dualPassState = &st;
                    break;
                }
            }
        }
    }
    RpClump* result = g_RpClumpRender_Orig ? g_RpClumpRender_Orig(clump) : nullptr;
    if (dualPassPed && dualPassState) {
        if (dualPassNum >= 2)
            OrcHeldTryDrawDualSecondaryAfterStockClumpPass(dualPassPed, *dualPassState, dualPassNum);
        else if (dualPassNum == 1) {
            const int pr = CPools::GetPedRef(dualPassPed);
            if (pr > 0 && !OrcHeldSecondaryAlreadyDrawnThisPass(pr))
                OrcHeldTryDrawLockedSecondaryClone(dualPassPed, *dualPassState, "repl:dualSecClumpPass1");
        }
    }
    return result;
}

static RpAtomic* __cdecl AtomicDefaultRenderCallBack_Detour(RpAtomic* atomic) {
    if (s_renderingHeldReplacementClone)
        return g_AtomicDefaultRender_Orig ? g_AtomicDefaultRender_Orig(atomic) : nullptr;
    HeldRwcbFrameRestore restore{};
    if (g_enabled) {
        const bool allPeds = g_renderAllPedsWeapons;
        CPlayerPed* pl = FindPlayerPed(0);
        const bool insideRenderWeaponCb = s_heldRenderWeaponCbDepth > 0 && s_heldRenderWeaponCbAtomic == atomic;
        if (!insideRenderWeaponCb && (allPeds || (pl && pl->m_pWeaponObject))) {
            __try {
                // Twin pistol: левый атом без RWCB — suppress + clone2 здесь (SA:MP часто без 2-го RpClumpRender).
                if (OrcHeldSuppressAndDrawDualSecondaryViaAtomicDefault(atomic)) {
                    OrcHeldRestoreRwcbFrame(restore);
                    return atomic;
                }
                OrcTryApplyHeldPoseIfAtomicMatches(atomic, &restore);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                OrcLogError("AtomicDefaultRenderCallBack_Detour: SEH ex=0x%08X", GetExceptionCode());
            }
        }
    }
    RpAtomic* result = g_AtomicDefaultRender_Orig ? g_AtomicDefaultRender_Orig(atomic) : nullptr;
    OrcHeldRestoreRwcbFrame(restore);
    return result;
}

static void __cdecl RenderWeaponCB_Detour(RpAtomic* atomic) {
    if (s_renderingHeldReplacementClone) {
        if (g_AtomicDefaultRender_Orig)
            g_AtomicDefaultRender_Orig(atomic);
        return;
    }
    bool appliedPose = false;
    bool rwcbMatchLocal = false;
    HeldRwcbFrameRestore restore{};
    if (g_enabled) {
        const bool needRemote = g_renderAllPedsWeapons && CPools::ms_pPedPool != nullptr;
        CPlayerPed* plrw = FindPlayerPed(0);
        const bool localHeldGate =
            plrw && GetPedSelectedWeaponTypeForReplace(plrw) > 0 && OrcHeldPosePedEligibleForPreRwDraw(plrw);
        if (localHeldGate || needRemote) {
            __try {
                HeldReplacementDrawContext drawCtx{};
                const bool hasDrawCtx = OrcTryGetHeldReplacementDrawContext(atomic, drawCtx);
                if (hasDrawCtx && drawCtx.state && drawCtx.state->rwObjectSecondary &&
                    OrcPedWantsDualWieldHeld(drawCtx.ped, drawCtx.weaponType)) {
                    if (OrcTryDrawDualHeldReplacementViaRwcb(atomic, drawCtx))
                        return;
                }
                if (OrcTryDrawHeldReplacementInsteadOfStockAtomic(atomic, "repl:rwcbSwap"))
                    return;
                const bool needMatchForTrace = g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info;
                appliedPose =
                    OrcTryApplyHeldPoseForRenderWeaponAtomic(atomic, needMatchForTrace ? &rwcbMatchLocal : nullptr, &restore);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                OrcLogError("RenderWeaponCB_Detour: SEH ex=0x%08X", GetExceptionCode());
            }
        }
    }
    if (g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info)
        OrcHeldTraceLogRenderWeaponCb(atomic, g_enabled && appliedPose, rwcbMatchLocal);
    ++s_heldRenderWeaponCbDepth;
    RpAtomic* prevRwcbAtomic = s_heldRenderWeaponCbAtomic;
    s_heldRenderWeaponCbAtomic = atomic;
    OrcCallRenderWeaponCbOrigSafe(atomic, "rwcb:tail");
    s_heldRenderWeaponCbAtomic = prevRwcbAtomic;
    --s_heldRenderWeaponCbDepth;
    OrcHeldRestoreRwcbFrame(restore);
}

void OrcHeldWeaponTraceGameProcessTick() {
    if (g_heldWeaponTrace < 2 || g_orcLogLevel < OrcLogLevel::Info)
        return;
    if (g_heldWeaponStatusIntervalMs <= 0)
        return;
    static int s_lastHeldStatusMs = 0;
    const int now = CTimer::m_snTimeInMilliseconds;
    if (s_lastHeldStatusMs != 0 && (now - s_lastHeldStatusMs) < g_heldWeaponStatusIntervalMs)
        return;
    s_lastHeldStatusMs = now;

    CPlayerPed* pl = FindPlayerPed(0);
    if (!pl)
        return;
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
    ++s_heldPrepSimSerial;
    OrcHeldGunflashMuzzleDeltaResetForSimTick();
    s_heldPreRwDrawAppliedPedRefs.clear();
    s_heldPoseEngineBaselineByFrame.clear();
    s_heldPreRwNearbyPeds.clear();
    s_heldDualStockClumpPassByPedRef.clear();
    s_heldDualSecDrawnFrameMsByPedRef.clear();
    s_heldDualRwcbOrigDoneFrameMsByPedRef.clear();
    s_heldReSyncDupPedRef = -1;
    s_heldReSyncDupSerial = 0;

    if (!g_enabled || !g_renderAllPedsWeapons || !CPools::ms_pPedPool)
        return;
    CPlayerPed* pl = FindPlayerPed(0);
    if (!pl)
        return;
    s_heldPreRwNearbyPeds.reserve(32);
    const int n = CPools::ms_pPedPool->m_nSize;
    for (int i = 0; i < n; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || !OrcHeldPosePedEligibleForPreRwDraw(p))
            continue;
        s_heldPreRwNearbyPeds.push_back(p);
    }
}

static void __cdecl RenderWeaponPedsForPC_Detour() {
    // Каждый вызов — батч RenderWeaponCB; сбрасываем кеш базы Held, чтобы каждый колбэк брал свежий IK-снимок
    // (ваниль может пересчитать матрицу между вызовами на один клумп).
    ++s_heldRwpfcBatchCounter;
    for (auto& kv : g_heldWeaponReplacements) {
        CPed* p = CPools::GetPed(kv.first);
        if (!p || kv.second.weaponType <= 0)
            continue;
        if (OrcPedWantsDualWieldHeld(p, kv.second.weaponType)) {
            const int pr = CPools::GetPedRef(p);
            if (pr > 0) {
                s_heldSecCloneDrawnBatchByPedRef.erase(pr);
                OrcHeldResetDualStockClumpPassForPed(pr);
            }
        }
    }
    if (g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info) {
        const unsigned iv = (g_heldWeaponTrace >= 2) ? 5000u : 10000u;
        OrcLogInfoThrottled(502, iv,
            "held hook RenderWeaponPedsForPC(0x732F30): batch=%llu t=%ums -> clear held baseline cache then Orig()",
            static_cast<unsigned long long>(s_heldRwpfcBatchCounter), static_cast<unsigned>(CTimer::m_snTimeInMilliseconds));
    }
    s_heldPoseEngineBaselineByFrame.clear();
    // Разрешаем повторный nudge gunflash в следующем RWCB-проходе (зеркала/доп. батчи), не сбрасывая snap `orig` за тик.
    s_gunflashMuzzleNudgeAppliedClumps.clear();
    if (g_RenderWeaponPedsForPC_Orig)
        g_RenderWeaponPedsForPC_Orig();
    OrcHeldTryDrawPendingDualSecondaryClonesAfterWeaponBatch();
}

static RwObject* OrcHeldResolveDualStockClumpForPed(CPed* ped, HeldWeaponReplacementState& st) {
    if (!ped)
        return nullptr;
    RwObject* stockWo = st.originalObject;
    if (stockWo && OrcHeldIsAnyReplacementCloneObject(stockWo))
        stockWo = nullptr;
    if (!stockWo && ped->m_pWeaponObject && !OrcHeldIsAnyReplacementCloneObject(ped->m_pWeaponObject))
        stockWo = ped->m_pWeaponObject;
    if (stockWo && stockWo->type == rpCLUMP)
        return stockWo;
    return nullptr;
}

static void OrcHeldTryDrawPendingDualSecondaryClonesAfterWeaponBatch() {
    for (auto& kv : g_heldWeaponReplacements) {
        HeldWeaponReplacementState& st = kv.second;
        if (!st.rwObjectSecondary || st.weaponType <= 0)
            continue;
        CPed* ped = CPools::GetPed(kv.first);
        if (!ped || !OrcPedWantsDualWieldHeld(ped, st.weaponType))
            continue;
        RwObject* stockWo = OrcHeldResolveDualStockClumpForPed(ped, st);
        if (stockWo) {
            st.lastPrimaryStockAtomic = nullptr;
            st.lastSecondaryStockAtomic = nullptr;
            OrcHeldLockDualStockAtomicsByHands(ped, st, stockWo);
        }
        OrcHeldRenderDualSecondaryClone(ped, st, "repl:dualSecPostBatch");
    }
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
        CPed* ped = CPools::GetPed(kv.first);
        if (ped && st.originalObject) {
            const bool slotIsClone = st.rwObject && ped->m_pWeaponObject == st.rwObject;
            // Слот с клоном: восстанавливаем сток даже если `captureActive` уже false (окно между after и EndScene).
            // hideBase + captureActive: как раньше — вернуть сток, даже если слот уже не указывает на клон.
            if (slotIsClone || (st.captureActive && st.hideBaseMode)) {
                ped->m_pWeaponObject = st.originalObject;
                OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
            }
        }
        st.captureActive = false;
        st.hideBaseMode = false;
        st.poseSynced = false;
        st.originalObject = nullptr;
        OrcDestroyRwObjectInstance(st.rwObject);
        OrcHeldDestroySecondaryClone(st);
    }
    g_heldWeaponReplacements.clear();
    g_deferredHeldWeaponStockRestore.clear();
    OrcHeldReplacementInvalidateAllDrawState();
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
            OrcHeldDestroySecondaryClone(it->second);
            it->second.poseSynced = false;
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

static bool OrcDualStockAtomicMatchesHeldWeapon(CPed* ped, int wt, RwObject* refWo, RpAtomic* atomic) {
    if (!ped || !atomic || wt <= 0)
        return false;
    bool matchesWeapon = OrcHeldAtomicMatchesWeaponModelTemplate(atomic, wt);
    if (!matchesWeapon) {
        CWeaponInfo* wi2 = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 2);
        int mid2 = wi2 ? wi2->m_nModelId2 : 0;
        if (mid2 <= 0) {
            CWeaponInfo* wi1 = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
            if (wi1)
                mid2 = wi1->m_nModelId2;
        }
        if (mid2 > 0)
            matchesWeapon = OrcHeldAtomicMatchesWeaponModelIdTemplate(atomic, mid2);
    }
    if (!matchesWeapon && refWo)
        matchesWeapon = OrcAtomicSharesGeometryWithWeaponObject(atomic, refWo);
    return matchesWeapon;
}

struct OrcCollectDualStockCtx {
    CPed* ped = nullptr;
    int wt = 0;
    RwObject* refWo = nullptr;
    RpAtomic* atomics[4]{};
    float handBias[4]{};
    int n = 0;
};

static RpAtomic* OrcCollectDualStockAtomicCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<OrcCollectDualStockCtx*>(data);
    if (!ctx || !ctx->ped || !atomic || ctx->n >= 4)
        return atomic;
    if (!OrcDualStockAtomicMatchesHeldWeapon(ctx->ped, ctx->wt, ctx->refWo, atomic))
        return atomic;
    CVector apos{};
    if (!OrcTryGetAtomicWorldPos(atomic, apos))
        return atomic;
    RwMatrix* lHand = OrcGetBoneMatrix(ctx->ped, BONE_L_HAND);
    RwMatrix* rHand = OrcGetBoneMatrix(ctx->ped, BONE_R_HAND);
    if (!lHand || !rHand)
        return atomic;
    const float dl = (apos.x - lHand->pos.x) * (apos.x - lHand->pos.x) + (apos.y - lHand->pos.y) * (apos.y - lHand->pos.y) +
                     (apos.z - lHand->pos.z) * (apos.z - lHand->pos.z);
    const float dr = (apos.x - rHand->pos.x) * (apos.x - rHand->pos.x) + (apos.y - rHand->pos.y) * (apos.y - rHand->pos.y) +
                     (apos.z - rHand->pos.z) * (apos.z - rHand->pos.z);
    ctx->atomics[ctx->n] = atomic;
    ctx->handBias[ctx->n] = dr - dl;
    ++ctx->n;
    return atomic;
}

static RpAtomic* OrcHeldFindOtherWeaponAtomicOnStock(RwObject* stockWo, RpAtomic* exclude, CPed* ped, int wt) {
    if (!stockWo || stockWo->type != rpCLUMP || !exclude || !ped || wt <= 0)
        return nullptr;
    struct PickCtx {
        CPed* ped = nullptr;
        int wt = 0;
        RwObject* wo = nullptr;
        RpAtomic* exclude = nullptr;
        RpAtomic* other = nullptr;
    } pick{};
    pick.ped = ped;
    pick.wt = wt;
    pick.wo = stockWo;
    pick.exclude = exclude;
    auto cb = [](RpAtomic* atomic, void* data) -> RpAtomic* {
        auto* ctx = reinterpret_cast<PickCtx*>(data);
        if (!ctx || !atomic || atomic == ctx->exclude)
            return atomic;
        if (!OrcDualStockAtomicMatchesHeldWeapon(ctx->ped, ctx->wt, ctx->wo, atomic))
            return atomic;
        if (!ctx->other)
            ctx->other = atomic;
        return atomic;
    };
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(stockWo), cb, &pick);
    return pick.other;
}

static void OrcHeldSyncStockWeaponClumpMatrices(CPed* ped, RwObject* stockWo) {
    if (!ped || !stockWo || stockWo->type != rpCLUMP)
        return;
    __try {
        if (ped->m_pRwClump) {
            RwFrame* pf = RpClumpGetFrame(ped->m_pRwClump);
            if (pf)
                RwFrameUpdateObjects(pf);
        }
        RwFrame* wf = RpClumpGetFrame(reinterpret_cast<RpClump*>(stockWo));
        if (wf)
            RwFrameUpdateObjects(wf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OrcHeldSyncStockWeaponClumpMatrices: SEH ex=0x%08X", GetExceptionCode());
    }
}

/// Фиксируем, какой атом стока в какой руке (LTM), до первого draw-swap. У twin colt только один атом идёт в RenderWeaponCB;
/// второй рисуется через AtomicDefaultRender на том же clump — post-pass сразу после RWCB брал ещё не обновлённую матрицу.
static void OrcHeldLockDualStockAtomicsByHands(CPed* ped, HeldWeaponReplacementState& st, RwObject* stockWo) {
    if (!ped || !stockWo || stockWo->type != rpCLUMP)
        return;
    OrcCollectDualStockCtx collect{};
    collect.ped = ped;
    collect.wt = st.weaponType;
    collect.refWo = stockWo;
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(stockWo), OrcCollectDualStockAtomicCb, &collect);
    if (collect.n <= 0)
        return;
    if (collect.n == 1) {
        st.lastPrimaryStockAtomic = collect.atomics[0];
        st.lastSecondaryStockAtomic = nullptr;
        return;
    }
    int iRight = 0;
    int iLeft = 0;
    for (int i = 1; i < collect.n; ++i) {
        if (collect.handBias[i] < collect.handBias[iRight])
            iRight = i;
        if (collect.handBias[i] > collect.handBias[iLeft])
            iLeft = i;
    }
    if (iLeft == iRight) {
        iLeft = (collect.handBias[0] >= collect.handBias[1]) ? 0 : 1;
        iRight = 1 - iLeft;
    }
    const float biasSep = std::fabs(collect.handBias[iLeft] - collect.handBias[iRight]);

    int iRwcb = -1;
    for (int i = 0; i < collect.n; ++i) {
        if (OrcAtomicUsesRenderWeaponCb(collect.atomics[i])) {
            iRwcb = i;
            break;
        }
    }

    if (biasSep > 0.02f) {
        st.lastPrimaryStockAtomic = collect.atomics[iRight];
        st.lastSecondaryStockAtomic = collect.atomics[iLeft];
    } else if (iRwcb >= 0) {
        st.lastPrimaryStockAtomic = collect.atomics[iRwcb];
        st.lastSecondaryStockAtomic = nullptr;
        for (int i = 0; i < collect.n; ++i) {
            if (i == iRwcb)
                continue;
            if (!OrcAtomicUsesRenderWeaponCb(collect.atomics[i])) {
                st.lastSecondaryStockAtomic = collect.atomics[i];
                break;
            }
        }
        if (!st.lastSecondaryStockAtomic && collect.n >= 2) {
            for (int i = 0; i < collect.n; ++i) {
                if (i != iRwcb) {
                    st.lastSecondaryStockAtomic = collect.atomics[i];
                    break;
                }
            }
        }
    } else {
        st.lastPrimaryStockAtomic = collect.atomics[iRight];
        st.lastSecondaryStockAtomic = collect.atomics[iLeft];
    }
    if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
        CVector apR{}, apL{};
        OrcTryGetAtomicWorldPos(st.lastPrimaryStockAtomic, apR);
        OrcTryGetAtomicWorldPos(st.lastSecondaryStockAtomic, apL);
        OrcLogInfoThrottled(47, 3000u,
            "held wr: dual lock by hands pedRef=%d wt=%d n=%d pri=%p sec=%p rwcbPri=%d biasSep=%.3f priW=(%.2f,%.2f,%.2f) secW=(%.2f,%.2f,%.2f)",
            CPools::GetPedRef(ped),
            st.weaponType,
            collect.n,
            st.lastPrimaryStockAtomic,
            st.lastSecondaryStockAtomic,
            iRwcb >= 0 ? 1 : 0,
            biasSep,
            apR.x, apR.y, apR.z,
            apL.x, apL.y, apL.z);
    }
}

static RpAtomic* OrcHeldFindNonRwcbWeaponAtomicOnStock(RwObject* stockWo, RpAtomic* exclude, CPed* ped, int wt) {
    if (!stockWo || stockWo->type != rpCLUMP || !ped || wt <= 0)
        return nullptr;
    struct Ctx {
        CPed* ped = nullptr;
        int wt = 0;
        RwObject* wo = nullptr;
        RpAtomic* exclude = nullptr;
        RpAtomic* found = nullptr;
    } ctx{ped, wt, stockWo, exclude, nullptr};
    auto cb = [](RpAtomic* atomic, void* data) -> RpAtomic* {
        auto* c = reinterpret_cast<Ctx*>(data);
        if (!c || !atomic || atomic == c->exclude || OrcAtomicUsesRenderWeaponCb(atomic))
            return atomic;
        if (OrcDualStockAtomicMatchesHeldWeapon(c->ped, c->wt, c->wo, atomic))
            c->found = atomic;
        return atomic;
    };
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(stockWo), cb, &ctx);
    return ctx.found;
}

static void OrcHeldSanitizeDualStockRoles(HeldWeaponReplacementState& st, RwObject* stockWo, CPed* ped) {
    if (!ped || !stockWo || stockWo->type != rpCLUMP || !st.lastPrimaryStockAtomic)
        return;
    if (st.lastSecondaryStockAtomic == st.lastPrimaryStockAtomic)
        st.lastSecondaryStockAtomic = nullptr;

    if (st.lastSecondaryStockAtomic && OrcAtomicUsesRenderWeaponCb(st.lastSecondaryStockAtomic)) {
        RpAtomic* nonRwcb =
            OrcHeldFindNonRwcbWeaponAtomicOnStock(stockWo, st.lastPrimaryStockAtomic, ped, st.weaponType);
        if (nonRwcb && nonRwcb != st.lastPrimaryStockAtomic)
            st.lastSecondaryStockAtomic = nonRwcb;
    }
    if (!OrcAtomicUsesRenderWeaponCb(st.lastPrimaryStockAtomic)) {
        struct RwcbPickCtx {
            RpAtomic* rwcb = nullptr;
        } pick{};
        auto cb = [](RpAtomic* atomic, void* data) -> RpAtomic* {
            if (atomic && OrcAtomicUsesRenderWeaponCb(atomic))
                reinterpret_cast<RwcbPickCtx*>(data)->rwcb = atomic;
            return atomic;
        };
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(stockWo), cb, &pick);
        if (pick.rwcb && pick.rwcb != st.lastPrimaryStockAtomic) {
            if (!st.lastSecondaryStockAtomic || st.lastSecondaryStockAtomic == pick.rwcb) {
                if (!OrcAtomicUsesRenderWeaponCb(st.lastPrimaryStockAtomic))
                    st.lastSecondaryStockAtomic = st.lastPrimaryStockAtomic;
            }
            st.lastPrimaryStockAtomic = pick.rwcb;
        }
    }
    if (st.lastSecondaryStockAtomic == st.lastPrimaryStockAtomic)
        st.lastSecondaryStockAtomic = nullptr;
}

static bool OrcHeldClassifyDualStockAtomicDraw(CPed* ped,
    int wt,
    HeldWeaponReplacementState& st,
    RpAtomic* stockAtomic,
    RwObject* stock,
    bool matchesSlotStock,
    bool* outSecondaryHand) {
    if (outSecondaryHand)
        *outSecondaryHand = false;
    if (!outSecondaryHand || !ped || !stockAtomic || wt <= 0 || !OrcPedWantsDualWieldHeld(ped, wt))
        return false;

    if (matchesSlotStock && stock && stock->type == rpCLUMP)
        OrcHeldLockDualStockAtomicsByHands(ped, st, stock);

    // Twin colt: первый RWCB за проход — primary; второй RWCB-атом — secondary (оба могут иметь callback).
    if (OrcAtomicUsesRenderWeaponCb(stockAtomic)) {
        if (!st.lastPrimaryStockAtomic) {
            st.lastPrimaryStockAtomic = stockAtomic;
            if (stock && stock->type == rpCLUMP) {
                RpAtomic* other = OrcHeldFindOtherWeaponAtomicOnStock(stock, stockAtomic, ped, wt);
                if (other)
                    st.lastSecondaryStockAtomic = other;
            }
            *outSecondaryHand = false;
            return true;
        }
        if (stockAtomic != st.lastPrimaryStockAtomic) {
            st.lastSecondaryStockAtomic = stockAtomic;
            *outSecondaryHand = true;
            return true;
        }
        *outSecondaryHand = false;
        return true;
    }
    if (st.lastSecondaryStockAtomic && stockAtomic == st.lastSecondaryStockAtomic) {
        *outSecondaryHand = true;
        return true;
    }
    *outSecondaryHand = false;
    return true;
}

struct OrcFindDualSecStockCtx {
    CPed* ped = nullptr;
    int wt = 0;
    RwObject* slotWo = nullptr;
    RwObject* stockWo = nullptr;
    RpAtomic* excludeAtomic = nullptr;
    RpAtomic* best = nullptr;
    float bestHandBias = -1e30f;
};

static RpAtomic* OrcFindDualSecStockAtomicCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<OrcFindDualSecStockCtx*>(data);
    if (!ctx || !ctx->ped || !atomic)
        return atomic;
    if (atomic == ctx->excludeAtomic)
        return atomic;

    RwObject* refWo = ctx->stockWo ? ctx->stockWo : ctx->slotWo;
    bool matchesWeapon = OrcDualStockAtomicMatchesHeldWeapon(ctx->ped, ctx->wt, refWo, atomic);
    if (!matchesWeapon)
        return atomic;

    CVector apos{};
    if (!OrcTryGetAtomicWorldPos(atomic, apos))
        return atomic;
    RwMatrix* lHand = OrcGetBoneMatrix(ctx->ped, BONE_L_HAND);
    RwMatrix* rHand = OrcGetBoneMatrix(ctx->ped, BONE_R_HAND);
    if (!lHand || !rHand)
        return atomic;

    const float dl = (apos.x - lHand->pos.x) * (apos.x - lHand->pos.x) + (apos.y - lHand->pos.y) * (apos.y - lHand->pos.y) +
                     (apos.z - lHand->pos.z) * (apos.z - lHand->pos.z);
    const float dr = (apos.x - rHand->pos.x) * (apos.x - rHand->pos.x) + (apos.y - rHand->pos.y) * (apos.y - rHand->pos.y) +
                     (apos.z - rHand->pos.z) * (apos.z - rHand->pos.z);
    const float handBias = dr - dl;
    if (handBias <= 0.0f)
        return atomic;
    if (handBias > ctx->bestHandBias) {
        ctx->bestHandBias = handBias;
        ctx->best = atomic;
    }
    return atomic;
}

static void OrcScanClumpForDualSecStock(RwObject* root, OrcFindDualSecStockCtx& ctx) {
    if (!root || root->type != rpCLUMP)
        return;
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(root), OrcFindDualSecStockAtomicCb, &ctx);
}

static RpAtomic* OrcFindDualHeldSecondaryStockAtomic(CPed* ped,
    int wt,
    RwObject* slotWo,
    RpAtomic* excludeAtomic,
    RwObject* stockWo) {
    if (!ped || wt <= 0 || !OrcPedWantsDualWieldHeld(ped, wt))
        return nullptr;
    OrcFindDualSecStockCtx ctx{};
    ctx.ped = ped;
    ctx.wt = wt;
    ctx.slotWo = slotWo;
    ctx.stockWo = stockWo;
    ctx.excludeAtomic = excludeAtomic;
    OrcScanClumpForDualSecStock(slotWo, ctx);
    OrcScanClumpForDualSecStock(stockWo, ctx);
    if (ped->m_pWeaponObject && ped->m_pWeaponObject != slotWo && ped->m_pWeaponObject != stockWo &&
        !OrcHeldIsAnyReplacementCloneObject(ped->m_pWeaponObject))
        OrcScanClumpForDualSecStock(ped->m_pWeaponObject, ctx);
    if (ped->m_pRwClump)
        RpClumpForAllAtomics(ped->m_pRwClump, OrcFindDualSecStockAtomicCb, &ctx);
    if (!ctx.best && excludeAtomic) {
        RwObject* scanWo = stockWo ? stockWo : slotWo;
        if (scanWo && scanWo->type == rpCLUMP) {
            OrcCollectDualStockCtx collect{};
            collect.ped = ped;
            collect.wt = wt;
            collect.refWo = scanWo;
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(scanWo), OrcCollectDualStockAtomicCb, &collect);
            for (int i = 0; i < collect.n; ++i) {
                if (collect.atomics[i] != excludeAtomic) {
                    ctx.best = collect.atomics[i];
                    break;
                }
            }
        }
    }
    return ctx.best;
}

void OrcWeaponSuppressBodyForHeldVisualWeapon(CPed* ped, std::vector<char>* suppress) {
    if (!ped || !suppress || !g_weaponReplacementEnabled || !g_weaponReplacementInHands)
        return;
    const int wt = OrcResolveWeaponHeldVisualWeaponType(ped);
    if (wt > 0 && wt < static_cast<int>(suppress->size()))
        (*suppress)[wt] = 1;
}

static void OrcHeldDestroySecondaryClone(HeldWeaponReplacementState& state) {
    OrcDestroyRwObjectInstance(state.rwObjectSecondary);
    state.rwObjectSecondary = nullptr;
}

static void OrcHeldEnsureSecondaryClone(CPed* ped, HeldWeaponReplacementState& state, WeaponReplacementAsset& asset) {
    if (!ped || !state.rwObject || state.weaponType <= 0) {
        OrcHeldDestroySecondaryClone(state);
        return;
    }
    const int wt = state.weaponType;
    if (!OrcPedWantsDualWieldHeld(ped, wt)) {
        OrcHeldDestroySecondaryClone(state);
        return;
    }
    if (!state.rwObjectSecondary) {
        state.rwObjectSecondary = OrcCloneWeaponReplacementObject(asset);
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1 && state.rwObjectSecondary) {
            OrcLogInfoThrottled(44, 2000u,
                "held wr: dual secondary clone pedRef=%d wt=%d key=%s clone2=%p",
                CPools::GetPedRef(ped),
                wt,
                state.replacementKey.c_str(),
                state.rwObjectSecondary);
        }
    }
}

static bool OrcHeldDualSecPhaseForcesRedraw(const char* phase) {
    if (!phase)
        return false;
    return std::strcmp(phase, "repl:atomicSec") == 0 || std::strcmp(phase, "repl:rwcbDualPri") == 0 ||
           std::strcmp(phase, "repl:rwcbDualSec") == 0 || std::strcmp(phase, "repl:dualSecPostBatch") == 0 ||
           std::strcmp(phase, "repl:dualSecClumpPass2") == 0 || std::strcmp(phase, "repl:dualSecClumpPass1") == 0;
}

static void OrcHeldRenderDualSecondaryClone(CPed* ped, HeldWeaponReplacementState& state, const char* phase) {
    if (!ped || !state.rwObjectSecondary || state.weaponType <= 0)
        return;
    if (!OrcPedWantsDualWieldHeld(ped, state.weaponType))
        return;

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;

    const bool forceRedraw = OrcHeldDualSecPhaseForcesRedraw(phase);
    if (!forceRedraw && OrcHeldSecondaryAlreadyDrawnThisPass(pedRef))
        return;

    RwObject* slotWo = ped->m_pWeaponObject;
    if (slotWo == state.rwObject || slotWo == state.rwObjectSecondary)
        slotWo = state.originalObject;
    if (slotWo && OrcHeldIsAnyReplacementCloneObject(slotWo))
        slotWo = nullptr;

    RwObject* stockWo = state.originalObject;
    if (stockWo && OrcHeldIsAnyReplacementCloneObject(stockWo))
        stockWo = nullptr;
    if (!stockWo)
        stockWo = slotWo;

    RpAtomic* secStock = state.lastSecondaryStockAtomic;
    if (!secStock || secStock == state.lastPrimaryStockAtomic) {
        RwObject* scanWo = stockWo ? stockWo : slotWo;
        secStock = OrcHeldFindOtherWeaponAtomicOnStock(scanWo, state.lastPrimaryStockAtomic, ped, state.weaponType);
        if (secStock)
            state.lastSecondaryStockAtomic = secStock;
    }
    if (!secStock) {
        secStock = OrcFindDualHeldSecondaryStockAtomic(
            ped, state.weaponType, slotWo, state.lastPrimaryStockAtomic, stockWo);
        if (secStock)
            state.lastSecondaryStockAtomic = secStock;
    }
    if (state.lastSecondaryStockAtomic && !secStock)
        secStock = state.lastSecondaryStockAtomic;
    if (!secStock || secStock == state.lastPrimaryStockAtomic) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(45, 4000u,
                "held wr: dual secondary no stock atomic pedRef=%d wt=%d slotWo=%p stockWo=%p pri=%p sec=%p",
                pedRef,
                state.weaponType,
                slotWo,
                stockWo,
                state.lastPrimaryStockAtomic,
                state.lastSecondaryStockAtomic);
        }
        return;
    }

    RwObject* drawStock = stockWo ? stockWo : slotWo;
    if (!drawStock) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(46, 4000u,
                "held wr: dual secondary no drawStock phase=%s pedRef=%d wt=%d slotWo=%p stockWo=%p pri=%p sec=%p",
                phase ? phase : "?",
                pedRef,
                state.weaponType,
                slotWo,
                stockWo,
                state.lastPrimaryStockAtomic,
                state.lastSecondaryStockAtomic);
        }
        return;
    }

    HeldReplacementDrawContext ctx{};
    ctx.ped = ped;
    ctx.pedRef = pedRef;
    ctx.state = &state;
    ctx.stock = drawStock;
    ctx.stockAtomic = secStock;
    ctx.weaponType = state.weaponType;
    ctx.secondaryHand = true;
    if (secStock == state.lastPrimaryStockAtomic) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(50, 3000u,
                "held wr: dual secondary sec equals pri phase=%s pedRef=%d sec=%p pri=%p",
                phase ? phase : "?",
                pedRef,
                secStock,
                state.lastPrimaryStockAtomic);
        }
        return;
    }
    if (!OrcRenderHeldReplacementCloneAtStockDraw(ctx, phase)) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
            OrcLogInfoThrottled(48, 3000u,
                "held wr: dual secondary draw failed phase=%s pedRef=%d wt=%d clone2=%p secStock=%p pri=%p batch=%llu",
                phase ? phase : "?",
                pedRef,
                state.weaponType,
                state.rwObjectSecondary,
                secStock,
                state.lastPrimaryStockAtomic,
                static_cast<unsigned long long>(s_heldRwpfcBatchCounter));
        }
    }
}

static bool OrcTryGetRwObjectRootWorldPos(RwObject* rwObject, CVector& out) {
    RwFrame* frame = GetRwObjectRootFrame(rwObject);
    if (!frame)
        return false;
    const RwMatrix* ltm = RwFrameGetLTM(frame);
    if (!ltm)
        return false;
    out = CVector(ltm->pos.x, ltm->pos.y, ltm->pos.z);
    return true;
}

static void RenderHeldReplacementHideBaseForPed(CPed* ped, HeldWeaponReplacementState& state) {
    if (!ped || !state.rwObject || state.weaponType <= 0)
        return;
    // Кулаки / слот без оружия: не рисуем клон на кости тела (дубль с кобурой + «летающий» меш).
    if (GetPedSelectedWeaponTypeForReplace(ped) <= 0)
        return;
    const int wt = state.weaponType;
    const bool renderFromStockPose = state.originalObject && state.poseSynced;
    if (renderFromStockPose)
        CopyStockHeldWeaponRwMatricesToClone(state.originalObject, state.rwObject, false);
    OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
    if (!renderFromStockPose && !PositionHeldReplacementLikeBodyWeapon(ped, wt, state.rwObject)) {
        OrcLogInfoThrottled(29, 4000u,
            "held wr hide-base: position failed pedRef=%d wt=%d (bone/skeleton?)",
            CPools::GetPedRef(ped), wt);
        return;
    }
    OrcApplyHeldPoseToWeaponObject(ped, state.rwObject, "repl:hideBase");
    state.poseSynced = true;

    const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
    const bool useIni = !renderFromStockPose && wc.enabled && wc.boneId != 0;
    const int boneId = useIni ? wc.boneId : kHeldReplacementFallbackBoneId;
    RwMatrix* bone = renderFromStockPose ? nullptr : OrcGetBoneMatrix(ped, boneId);
    CVector lightPos{};
    const bool gotRootLightPos = renderFromStockPose && OrcTryGetRwObjectRootWorldPos(state.rwObject, lightPos);
    if (!gotRootLightPos && bone) {
        lightPos = CVector(bone->pos.x, bone->pos.y, bone->pos.z);
    } else if (!gotRootLightPos) {
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

static void OrcCaptureHeldReplacementForDrawSwap(
    CPed* ped,
    int pedRef,
    int wt,
    const std::string& key,
    HeldWeaponReplacementState& state,
    RwObject* stockForCopy,
    const char* phase) {
    if (!ped || pedRef <= 0 || !state.rwObject)
        return;
    if (stockForCopy && stockForCopy != state.rwObject && !OrcHeldIsAnyReplacementCloneObject(stockForCopy)) {
        CopyStockHeldWeaponRwMatricesToClone(stockForCopy, state.rwObject, false);
        OrcHeldPoseInvalidateBaselineForRwObject(state.rwObject);
        state.poseSynced = true;
        state.originalObject = stockForCopy;
    } else {
        state.poseSynced = false;
        state.originalObject = nullptr;
        state.lastPrimaryStockAtomic = nullptr;
        state.lastSecondaryStockAtomic = nullptr;
    }
    state.hideBaseMode = true;
    state.captureActive = true;
    OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped, wt);
    if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1) {
        OrcLogInfoThrottled(36, 5000u,
            "held wr: draw-swap capture phase=%s pedRef=%d wt=%d key=%s stock=%p clone=%p slotWo=%p",
            phase ? phase : "?",
            pedRef,
            wt,
            key.c_str(),
            stockForCopy,
            state.rwObject,
            ped->m_pWeaponObject);
    }
}

void OrcPrepareHeldWeaponReplacementBefore(CPed* ped) {
    const int pedRefEarly = ped ? CPools::GetPedRef(ped) : 0;

    if (!ShouldReplaceHeldWeaponForPed(ped)) {
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
        OrcClearHeldWeaponReplacementStateForPed(pedRefEarly, wt, "wt<=0");
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
        OrcClearHeldWeaponReplacementStateForPed(pedRefEarly, wt, "noAsset");
        return;
    }

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0) {
        OrcLogInfoThrottled(27, 4000u, "held wr: pedRef<=0 wt=%d key=%s", wt, asset->key.c_str());
        return;
    }
    HeldWeaponReplacementState& state = g_heldWeaponReplacements[pedRef];
    if (!state.rwObject || state.weaponType != wt || state.replacementKey != asset->key) {
        if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 1 && state.rwObject &&
            state.replacementKey != asset->key) {
            OrcLogInfoThrottled(43, 800u,
                "held wr: reclone pedRef=%d wt=%d oldKey=%s newKey=%s clone=%p",
                pedRef,
                wt,
                state.replacementKey.c_str(),
                asset->key.c_str(),
                state.rwObject);
        }
        OrcDestroyRwObjectInstance(state.rwObject);
        OrcHeldDestroySecondaryClone(state);
        OrcHeldReplacementInvalidateDrawStateForPed(pedRef);
        state.poseSynced = false;
        const DWORD tClone0 = GetTickCount();
        state.rwObject = OrcCloneWeaponReplacementObject(*asset);
        const DWORD cloneMs = GetTickCount() - tClone0;
        if (cloneMs > 80u)
            OrcLogInfo("held wr: slow CloneWeaponReplacementObject %ums pedRef=%d wt=%d key=%s",
                (unsigned)cloneMs, pedRef, wt, asset->key.c_str());
        state.weaponType = wt;
        state.replacementKey = asset->key;
        state.originalObject = nullptr;
        state.captureActive = false;
        state.hideBaseMode = false;
        if (!state.rwObject) {
            OrcLogError("held wr: CloneWeaponReplacementObject failed pedRef=%d wt=%d key=%s",
                pedRef, wt, asset->key.c_str());
            return;
        }
    }
    if (!state.rwObject)
        return;
    OrcHeldEnsureSecondaryClone(ped, state, *asset);

    // Replacement clone is never stored in `m_pWeaponObject`: SA:MP can query that slot outside our render window.
    // Keep the stock slot intact and swap the visual only at the vanilla stock weapon draw callback.
    if (state.rwObject && state.weaponType == wt && state.replacementKey == asset->key) {
        RwObject* stockForCopy = OrcHeldGetReplacementStockObject(pedRef, state);
        if (stockForCopy && ped->m_pWeaponObject == stockForCopy) {
            if (OrcHeldIsAnyReplacementCloneObject(stockForCopy)) {
                OrcLogInfoThrottled(32, 2000u,
                    "held wr: skip swapStock copy source is replacement pedRef=%d wt=%d key=%s src=%p clone=%p",
                    pedRefEarly, wt, asset->key.c_str(), stockForCopy, state.rwObject);
                OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, nullptr, "swapStockBadSource");
                return;
            }
            OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, stockForCopy, "swapStock");
            return;
        }
        if (stockForCopy && ped->m_pWeaponObject == state.rwObject) {
            if (OrcHeldIsAnyReplacementCloneObject(stockForCopy)) {
                OrcLogInfoThrottled(33, 2000u,
                    "held wr: skip reSync copy source is replacement pedRef=%d wt=%d key=%s src=%p clone=%p",
                    pedRefEarly, wt, asset->key.c_str(), stockForCopy, state.rwObject);
                OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, nullptr, "reSyncBadSource");
                return;
            }
            // Два `pedRenderEvent.before` на кадр — повторный reSync тот же тик не даёт новой матрицы, только лишняя работа.
            if (pedRef == s_heldReSyncDupPedRef && s_heldReSyncDupSerial == s_heldPrepSimSerial) {
                OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped, wt);
                state.captureActive = true;
                state.hideBaseMode = true;
                return;
            }
            s_heldReSyncDupPedRef = pedRef;
            s_heldReSyncDupSerial = s_heldPrepSimSerial;
            OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, stockForCopy, "reSync");
            return;
        }
        if (state.hideBaseMode && state.originalObject && !ped->m_pWeaponObject) {
            state.captureActive = true;
            return;
        }
    }

    // SA:MP may leave `m_pWeaponObject` null during ped render while wt still resolves.
    // Re-use after-draw path until a stock clump exists.
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
        state.poseSynced = false;
        state.captureActive = true;
        OrcLogInfoThrottled(30, 5000u,
            "held wr: empty held slot -> after-draw pedRef=%d wt=%d key=%s",
            pedRefEarly, wt, asset->key.c_str());
        return;
    }

    // Do not assign stock weapon renderCallBack onto the clone: CVisibilityPlugins::RenderWeaponCB
    // expects weapon visibility-plugin data on each RpAtomic; replacement clones don't have it → AV (+0x18).
    // InitAttachmentAtomicCB clears callbacks so RpClumpRender uses the default atomic path.
    RwObject* stockForCopy = ped->m_pWeaponObject;
    if (stockForCopy == state.rwObject) {
        RwObject* deferredStock = OrcHeldGetReplacementStockObject(pedRef, state);
        if (deferredStock)
            stockForCopy = deferredStock;
    }
    if (!stockForCopy || stockForCopy == state.rwObject) {
        OrcLogInfoThrottled(31, 2000u,
            "held wr: skip firstSwap copy source invalid pedRef=%d wt=%d key=%s src=%p clone=%p",
            pedRefEarly, wt, asset->key.c_str(), stockForCopy, state.rwObject);
        if (ped->m_pWeaponObject == state.rwObject)
            OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, nullptr, "firstSwapInvalidSource");
        else
            state.captureActive = true;
        return;
    }
    if (OrcHeldIsAnyReplacementCloneObject(stockForCopy)) {
        OrcLogInfoThrottled(34, 2000u,
            "held wr: skip firstSwap copy source is replacement pedRef=%d wt=%d key=%s src=%p clone=%p",
            pedRefEarly, wt, asset->key.c_str(), stockForCopy, state.rwObject);
        OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, nullptr, "firstSwapBadSource");
        return;
    }
    OrcCaptureHeldReplacementForDrawSwap(ped, pedRef, wt, asset->key, state, stockForCopy, "firstSwap");
}

void OrcHeldDrainDeferredDualSecondaryDraws(CPed* ped) {
    (void)ped;
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
    RwObject* stock = state.originalObject;
    if (state.hideBaseMode && state.rwObject && !stock)
        RenderHeldReplacementHideBaseForPed(ped, state);

    state.originalObject = nullptr;
    state.captureActive = false;
    state.hideBaseMode = false;

    if (pedRef > 0 && stock && state.rwObject) {
        DeferredHeldWeaponSlotRestore& defer = g_deferredHeldWeaponStockRestore[pedRef];
        defer.stock = stock;
        defer.clone = state.rwObject;
        defer.weaponModelId = ped->m_nWeaponModelId;
        defer.selectedSlot = ped->m_nSelectedWepSlot;
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

// `__thiscall` cannot be used on static free functions in MSVC; `__fastcall` matches the register ABI
// (this in ECX) and matches the pattern used in `samp_bridge.cpp` for thiscall detours.
static void __fastcall AddWeaponModel_Hook(CPed* ped, void* /*edx*/, int modelIndex) {
    const bool logPerf = (g_orcLogLevel >= OrcLogLevel::Info);
    LARGE_INTEGER fq{};
    LARGE_INTEGER t0{};
    LARGE_INTEGER tVan0{};
    LARGE_INTEGER tVan1{};
    LARGE_INTEGER t1{};
    if (logPerf) {
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
    }
    OrcEnsureStockWeaponClumpInHeldSlotBeforeVanillaWeaponModelOp(ped);
    if (logPerf)
        QueryPerformanceCounter(&tVan0);
    if (g_AddWeaponModel_Orig)
        g_AddWeaponModel_Orig(ped, modelIndex);
    if (logPerf)
        QueryPerformanceCounter(&tVan1);
    if (logPerf) {
        QueryPerformanceCounter(&t1);
        if (fq.QuadPart > 0) {
            const double msTotal = (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
            const double msVanilla = (tVan1.QuadPart - tVan0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
            if (msTotal >= 12.0 || msVanilla >= 12.0) {
                const int pedRef = ped ? CPools::GetPedRef(ped) : 0;
                OrcLogInfo(
                    "AddWeaponModel: SLOW ped=%p ref=%d mid=%d vanillaMs=%.1f totalMs=%.1f",
                    ped,
                    pedRef,
                    modelIndex,
                    msVanilla,
                    msTotal);
            }
        }
    }
}

static void __fastcall RemoveWeaponModel_Hook(CPed* ped, void* /*edx*/, int modelIndex) {
    if (!ped) {
        if (g_RemoveWeaponModel_Orig)
            g_RemoveWeaponModel_Orig(ped, modelIndex);
        return;
    }
    OrcEnsureStockWeaponClumpInHeldSlotBeforeVanillaWeaponModelOp(ped);
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
                if (stock) {
                    ped->m_pWeaponObject = stock;
                    OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
                }
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

    if (pedRef > 0) {
        g_deferredHeldWeaponStockRestore.erase(pedRef);
    }
    st.originalObject = nullptr;
    OrcDestroyRwObjectInstance(st.rwObject);
    st.rwObject = nullptr;
    st.poseSynced = false;
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

