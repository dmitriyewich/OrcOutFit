#include "orc_skin_native.h"

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "CClumpModelInfo.h"
#include "CTxdStore.h"
#include "CVisibilityPlugins.h"
#include "ePedState.h"
#include "eModelInfoType.h"
#include "RenderWare.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "orc_app.h"
#include "orc_log.h"
#include "orc_path.h"
#include "samp_bridge.h"
#include "external/MinHook/include/MinHook.h"

namespace {

enum class NativeSkinTargetKind : unsigned char {
    Custom,
    Standard,
};

struct PedNativeSkinState {
    CPed* ped = nullptr;
    int pedRef = 0;
    int baseModelId = -1;
    NativeSkinTargetKind targetKind = NativeSkinTargetKind::Custom;
    int standardModelId = -1;
    std::string skinName;
    std::string skinNameLower;
    std::string remapKey;
    std::string remapFallbackKey;
    int txdSlot = -1;
    int baseTxdSlot = -1;
    RpClump* appliedClump = nullptr;
    bool failed = false;
    bool applyDeferred = false;
    bool restoreDeferred = false;
    bool overlayFallback = false;
    bool baseTxdRefAdded = false;
    bool targetTxdRefAdded = false;
    bool lateSetModelAllowed = false;
    DWORD retryAfterMs = 0;
    DWORD lastLogMs = 0;
};

struct NativeSkinTarget {
    NativeSkinTargetKind kind = NativeSkinTargetKind::Custom;
    int standardModelId = -1;
    std::string name;
    std::string nameLower;
    std::string remapKey;
    std::string remapFallbackKey;
    int txdSlot = -1;
    RwObject* rwObject = nullptr;
    bool prepareTemplate = false;
};

using NativeSkinStateMap = std::unordered_map<int, PedNativeSkinState>;

static NativeSkinStateMap g_nativeSkinStates;
static bool g_nativeSetModelInProgress = false;
static bool g_nativeSetModelHookInstalled = false;
static int g_nativeSetModelHookDepth = 0;
static constexpr DWORD kNativeSkinRetryMs = 500;
static constexpr DWORD kNativeSkinDeferLogMs = 3000;
static constexpr uintptr_t kAddr_CPed_SetModelIndex = 0x5E4880;
static constexpr uintptr_t kAddr_GetFirstTexture = 0x734940;
static constexpr uintptr_t kAddr_RwTexDictionaryFindHashNamedTexture = 0x734E50;

using CPedSetModelIndexFn = void(__thiscall*)(CPed*, unsigned int);
using GetFirstTextureFn = RwTexture*(__cdecl*)(RwTexDictionary*);
using FindHashNamedTextureFn = RwTexture*(__cdecl*)(RwTexDictionary*, unsigned int);
static CPedSetModelIndexFn g_CPedSetModelIndex_Orig = nullptr;
static GetFirstTextureFn g_GetFirstTexture_Orig = nullptr;
static FindHashNamedTextureFn g_RwTexDictionaryFindHashNamedTexture_Orig = nullptr;

static void ReleaseNativeBaseTxdRef(PedNativeSkinState& state);
static void ReleaseNativeTargetTxdRef(PedNativeSkinState& state);

static RwTexture* __cdecl GetFirstTexture_Detour(RwTexDictionary* txd) {
    if (!txd) {
        OrcLogInfoThrottled(509, 3000u, "native skin guard: GetFirstTexture null TXD");
        return nullptr;
    }
    return g_GetFirstTexture_Orig ? g_GetFirstTexture_Orig(txd) : nullptr;
}

static RwTexture* __cdecl RwTexDictionaryFindHashNamedTexture_Detour(RwTexDictionary* txd, unsigned int hash) {
    if (!txd) {
        OrcLogInfoThrottled(510, 3000u, "native skin guard: FindHashNamedTexture null TXD hash=0x%08X", hash);
        return nullptr;
    }
    return g_RwTexDictionaryFindHashNamedTexture_Orig ? g_RwTexDictionaryFindHashNamedTexture_Orig(txd, hash) : nullptr;
}

static void InstallNativeTextureDictionaryGuards() {
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(kAddr_GetFirstTexture),
        reinterpret_cast<void*>(&GetFirstTexture_Detour),
        reinterpret_cast<void**>(&g_GetFirstTexture_Orig));
    if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_GetFirstTexture));
        if (st != MH_OK && st != MH_ERROR_ENABLED)
            OrcLogError("native skin guard: MH_EnableHook GetFirstTexture -> %s", MH_StatusToString(st));
    } else {
        OrcLogError("native skin guard: MH_CreateHook GetFirstTexture -> %s", MH_StatusToString(st));
    }

    st = MH_CreateHook(reinterpret_cast<void*>(kAddr_RwTexDictionaryFindHashNamedTexture),
        reinterpret_cast<void*>(&RwTexDictionaryFindHashNamedTexture_Detour),
        reinterpret_cast<void**>(&g_RwTexDictionaryFindHashNamedTexture_Orig));
    if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_RwTexDictionaryFindHashNamedTexture));
        if (st != MH_OK && st != MH_ERROR_ENABLED)
            OrcLogError("native skin guard: MH_EnableHook FindHashNamedTexture -> %s", MH_StatusToString(st));
    } else {
        OrcLogError("native skin guard: MH_CreateHook FindHashNamedTexture -> %s", MH_StatusToString(st));
    }
}

static int NativePedKey(CPed* ped) {
    const int ref = OrcSafeGetPedRef(ped);
    if (ref > 0)
        return ref;
    if (!ped)
        return 0;
    return (int)(reinterpret_cast<uintptr_t>(ped) & 0x7fffffff);
}

static bool NativeStateTargetMatches(const PedNativeSkinState& state,
    CPed* ped,
    const NativeSkinTarget& target,
    int baseModelId) {
    return state.ped == ped &&
        state.baseModelId == baseModelId &&
        state.targetKind == target.kind &&
        state.standardModelId == target.standardModelId &&
        state.skinNameLower == target.nameLower;
}

static const char* NativeTargetKindName(NativeSkinTargetKind kind) {
    return kind == NativeSkinTargetKind::Standard ? "standard" : "custom";
}

static bool NativeStateOwnsCurrentClump(const PedNativeSkinState& state, CPed* ped) {
    return ped &&
        state.ped == ped &&
        !state.failed &&
        state.appliedClump &&
        ped->m_pRwClump == state.appliedClump;
}

static bool ShouldAllowLateNativeSetModel(const OrcResolvedPedSkin& resolved) {
    return resolved.isLocalPed && !resolved.fromRandomPool;
}

static bool TickElapsed(DWORD tick) {
    return tick == 0 || static_cast<LONG>(GetTickCount() - tick) >= 0;
}

static bool IsVehicleOrTransitionState(ePedState state) {
    switch (state) {
    case PEDSTATE_ENTER_TRAIN:
    case PEDSTATE_EXIT_TRAIN:
    case PEDSTATE_DRIVING:
    case PEDSTATE_PASSENGER:
    case PEDSTATE_TAXI_PASSENGER:
    case PEDSTATE_OPEN_DOOR:
    case PEDSTATE_CARJACK:
    case PEDSTATE_DRAGGED_FROM_CAR:
    case PEDSTATE_ENTER_CAR:
    case PEDSTATE_STEAL_CAR:
    case PEDSTATE_EXIT_CAR:
        return true;
    default:
        return false;
    }
}

static bool IsDeathState(ePedState state) {
    return state == PEDSTATE_DIE || state == PEDSTATE_DEAD || state == PEDSTATE_DIE_BY_STEALTH;
}

static bool IsNativeSetModelUnsafe(CPed* ped) {
    if (!ped)
        return true;
    bool unsafe = true;
    __try {
        unsafe = ped->bInVehicle ||
            ped->m_pVehicle != nullptr ||
            IsVehicleOrTransitionState(ped->m_ePedState) ||
            IsDeathState(ped->m_ePedState);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        unsafe = true;
    }
    return unsafe;
}

static void LogNativeSetModelDeferred(PedNativeSkinState& state, const char* action) {
    const DWORD now = GetTickCount();
    if (state.lastLogMs != 0 && now - state.lastLogMs < kNativeSkinDeferLogMs)
        return;
    state.lastLogMs = now;

    int pedState = -1;
    int inVehicle = 0;
    void* vehicle = nullptr;
    __try {
        if (state.ped) {
            pedState = (int)state.ped->m_ePedState;
            inVehicle = state.ped->bInVehicle ? 1 : 0;
            vehicle = state.ped->m_pVehicle;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    OrcLogInfo("native skin %s deferred: ped=%p base=%d skin=%s state=%d inVehicle=%d vehicle=%p",
        action ? action : "?",
        state.ped,
        state.baseModelId,
        state.skinName.c_str(),
        pedState,
        inVehicle,
        vehicle);
}

static bool IsPedAliveInPool(CPed* ped) {
    if (!ped || !CPools::ms_pPedPool)
        return false;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
        if (CPools::ms_pPedPool->GetAt(i) == ped)
            return true;
    }
    return false;
}

static void PruneNativeStates() {
    std::unordered_set<CPed*> alive;
    if (CPools::ms_pPedPool) {
        for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
            if (CPed* ped = CPools::ms_pPedPool->GetAt(i))
                alive.insert(ped);
        }
    }

    for (auto it = g_nativeSkinStates.begin(); it != g_nativeSkinStates.end();) {
        if (!it->second.ped || alive.find(it->second.ped) == alive.end()) {
            ReleaseNativeBaseTxdRef(it->second);
            ReleaseNativeTargetTxdRef(it->second);
            it = g_nativeSkinStates.erase(it);
        } else {
            ++it;
        }
    }
}

static RpAtomic* NativePedAtomicCB(RpAtomic* atomic, void*) {
    if (!atomic)
        return atomic;
    CClumpModelInfo::SetAtomicRendererCB(atomic, reinterpret_cast<void*>(&CVisibilityPlugins::RenderPedCB));
    if (atomic->geometry)
        atomic->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
    return atomic;
}

static void PreparePedClump(RpClump* clump, CClumpModelInfo* modelInfo) {
    if (!clump || !modelInfo)
        return;
    CVisibilityPlugins::SetClumpModelInfo(clump, modelInfo);
    RpClumpForAllAtomics(clump, NativePedAtomicCB, nullptr);
}

struct ScopedModelInfoOverride {
    CBaseModelInfo* modelInfo = nullptr;
    RwObject* oldObject = nullptr;
    short oldTxdIndex = -1;

    ScopedModelInfoOverride(CBaseModelInfo* mi, RwObject* object, int txdIndex) : modelInfo(mi) {
        if (!modelInfo)
            return;
        oldObject = modelInfo->m_pRwObject;
        oldTxdIndex = modelInfo->m_nTxdIndex;
        modelInfo->m_pRwObject = object;
        modelInfo->m_nTxdIndex = static_cast<short>(txdIndex);
    }

    ~ScopedModelInfoOverride() {
        if (!modelInfo)
            return;
        modelInfo->m_pRwObject = oldObject;
        modelInfo->m_nTxdIndex = oldTxdIndex;
    }
};

static bool LoadBaseModelForSetModel(int modelId) {
    if (CStreaming::HasModelLoaded(modelId))
        return true;
    CStreaming::RequestModel(modelId, 0);
    for (int i = 0; i < 32 && !CStreaming::HasModelLoaded(modelId); ++i)
        CStreaming::LoadAllRequestedModels(false);
    return CStreaming::HasModelLoaded(modelId);
}

static void CallPedSetModelIndexOriginal(CPed* ped, unsigned int modelId) {
    if (g_CPedSetModelIndex_Orig) {
        g_CPedSetModelIndex_Orig(ped, modelId);
    } else if (ped) {
        ped->SetModelIndex(modelId);
    }
}

static TxdDef* GetTxdDefByIndex(int txdIndex) {
    if (txdIndex < 0 || !CTxdStore::ms_pTxdPool || !CTxdStore::ms_pTxdPool->m_pObjects)
        return nullptr;
    if (txdIndex >= CTxdStore::ms_pTxdPool->m_nSize)
        return nullptr;
    return CTxdStore::ms_pTxdPool->GetAt(txdIndex);
}

static RwTexDictionary* GetTxdDictionaryByIndex(int txdIndex) {
    TxdDef* txd = GetTxdDefByIndex(txdIndex);
    return txd ? txd->m_pRwDictionary : nullptr;
}

static void ReleaseNativeBaseTxdRef(PedNativeSkinState& state) {
    if (!state.baseTxdRefAdded)
        return;

    const int txdSlot = state.baseTxdSlot;
    state.baseTxdRefAdded = false;
    state.baseTxdSlot = -1;

    if (!GetTxdDefByIndex(txdSlot))
        return;

    __try {
        CTxdStore::RemoveRef(txdSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: base TXD ref release SEH ex=0x%08X txd=%d", GetExceptionCode(), txdSlot);
    }
}

static void ReleaseNativeTargetTxdRef(PedNativeSkinState& state) {
    if (!state.targetTxdRefAdded)
        return;

    const int txdSlot = state.txdSlot;
    state.targetTxdRefAdded = false;
    if (!GetTxdDefByIndex(txdSlot))
        return;

    __try {
        CTxdStore::RemoveRef(txdSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: target TXD ref release SEH ex=0x%08X txd=%d", GetExceptionCode(), txdSlot);
    }
}

static NativeSkinStateMap::iterator EraseNativeState(NativeSkinStateMap::iterator it) {
    if (it == g_nativeSkinStates.end())
        return it;
    ReleaseNativeBaseTxdRef(it->second);
    ReleaseNativeTargetTxdRef(it->second);
    return g_nativeSkinStates.erase(it);
}

static void EraseNativeStateByKey(int key) {
    auto it = g_nativeSkinStates.find(key);
    if (it != g_nativeSkinStates.end())
        EraseNativeState(it);
}

static std::pair<NativeSkinStateMap::iterator, bool> ReplaceNativeState(int key, PedNativeSkinState&& state) {
    auto existing = g_nativeSkinStates.find(key);
    if (existing != g_nativeSkinStates.end()) {
        ReleaseNativeBaseTxdRef(existing->second);
        ReleaseNativeTargetTxdRef(existing->second);
    }
    return g_nativeSkinStates.insert_or_assign(key, std::move(state));
}

static bool AcquireNativeTargetTxdRef(PedNativeSkinState& state, const NativeSkinTarget& target) {
    const int txdSlot = target.txdSlot;
    if (txdSlot < 0 || !GetTxdDictionaryByIndex(txdSlot)) {
        OrcLogError("native skin: target TXD dictionary unavailable kind=%s skin=%s txd=%d",
            NativeTargetKindName(target.kind), target.name.c_str(), txdSlot);
        return false;
    }

    __try {
        CTxdStore::AddRef(txdSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: target TXD AddRef SEH ex=0x%08X kind=%s skin=%s txd=%d",
            GetExceptionCode(), NativeTargetKindName(target.kind), target.name.c_str(), txdSlot);
        return false;
    }

    state.txdSlot = txdSlot;
    state.targetTxdRefAdded = true;
    return true;
}

static bool AcquireNativeBaseTxdRef(PedNativeSkinState& state, CBaseModelInfo* baseMi, int baseModelId) {
    if (!baseMi)
        return false;

    const int txdSlot = baseMi->m_nTxdIndex;
    if (txdSlot < 0) {
        OrcLogError("native skin: base model has no TXD base=%d skin=%s", baseModelId, state.skinName.c_str());
        return false;
    }

    const bool hadDict = GetTxdDictionaryByIndex(txdSlot) != nullptr;
    if (!hadDict) {
        __try {
            CStreaming::RequestTxdModel(txdSlot, GAME_REQUIRED);
            CStreaming::LoadAllRequestedModels(false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("native skin: base TXD load SEH ex=0x%08X base=%d txd=%d skin=%s",
                GetExceptionCode(),
                baseModelId,
                txdSlot,
                state.skinName.c_str());
            return false;
        }
    }

    if (!GetTxdDictionaryByIndex(txdSlot)) {
        OrcLogError("native skin: base TXD dictionary unavailable base=%d txd=%d skin=%s",
            baseModelId,
            txdSlot,
            state.skinName.c_str());
        return false;
    }

    __try {
        CTxdStore::AddRef(txdSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: base TXD AddRef SEH ex=0x%08X base=%d txd=%d skin=%s",
            GetExceptionCode(),
            baseModelId,
            txdSlot,
            state.skinName.c_str());
        return false;
    }

    state.baseTxdSlot = txdSlot;
    state.baseTxdRefAdded = true;

    if (!hadDict) {
        OrcLogInfo("native skin: reloaded base TXD for anims base=%d txd=%d skin=%s",
            baseModelId,
            txdSlot,
            state.skinName.c_str());
    }
    return true;
}

static void FillNativeStateFromTarget(PedNativeSkinState& state,
    CPed* ped,
    int key,
    int baseModelId,
    const NativeSkinTarget& target) {
    state.ped = ped;
    state.pedRef = key;
    state.baseModelId = baseModelId;
    state.targetKind = target.kind;
    state.standardModelId = target.standardModelId;
    state.skinName = target.name;
    state.skinNameLower = target.nameLower;
    state.remapKey = target.remapKey.empty() ? target.name : target.remapKey;
    state.remapFallbackKey = target.remapFallbackKey;
    state.txdSlot = target.txdSlot;
}

static void FillNativeStateFromTarget(PedNativeSkinState& state,
    CPed* ped,
    int key,
    int baseModelId,
    const NativeSkinTarget& target,
    bool lateSetModelAllowed) {
    FillNativeStateFromTarget(state, ped, key, baseModelId, target);
    state.lateSetModelAllowed = lateSetModelAllowed;
}

static bool ResolveNativeTarget(const OrcResolvedPedSkin& resolved,
    NativeSkinTarget& target,
    const char*& failureReason) {
    failureReason = "skin target is unavailable";
    if (resolved.custom) {
        CustomSkinCfg& skin = *resolved.custom;
        target.kind = NativeSkinTargetKind::Custom;
        target.name = skin.name;
        target.nameLower = OrcToLowerAscii(skin.name);
        target.remapKey = skin.remapKey;
        target.remapFallbackKey = skin.remapFallbackKey;
        target.txdSlot = skin.txdSlot;
        target.rwObject = skin.rwObject;
        target.prepareTemplate = true;

        if (!OrcEnsureCustomSkinLoaded(skin) || !skin.rwObject || skin.rwObject->type != rpCLUMP) {
            failureReason = "custom DFF/TXD load failed";
            return false;
        }
        target.txdSlot = skin.txdSlot;
        target.rwObject = skin.rwObject;
        return true;
    }

    if (!resolved.standard)
        return false;

    StandardSkinCfg& skin = *resolved.standard;
    target.kind = NativeSkinTargetKind::Standard;
    target.standardModelId = skin.modelId;
    target.name = skin.dffName;
    target.nameLower = OrcToLowerAscii(skin.dffName);
    target.remapKey = skin.dffName;

    if (!OrcIsValidStandardSkinModel(skin.modelId)) {
        failureReason = "invalid standard ped model";
        return false;
    }
    if (!LoadBaseModelForSetModel(skin.modelId)) {
        failureReason = "standard ped model not loaded";
        return false;
    }

    CBaseModelInfo* targetMi = CModelInfo::GetModelInfo(skin.modelId);
    if (!targetMi || targetMi->GetModelType() != MODEL_INFO_PED || !targetMi->m_pRwObject ||
        targetMi->m_pRwObject->type != rpCLUMP || targetMi->m_nTxdIndex < 0) {
        failureReason = "standard ped model info is unavailable";
        return false;
    }

    target.txdSlot = targetMi->m_nTxdIndex;
    target.rwObject = targetMi->m_pRwObject;
    return true;
}

static void StoreAppliedNativeState(PedNativeSkinState&& state, RpClump* clump) {
    state.appliedClump = clump;
    ReplaceNativeState(state.pedRef, std::move(state));
}

static void MarkNativeFailure(CPed* ped,
    int key,
    int baseModelId,
    const NativeSkinTarget& target,
    const char* reason,
    bool overlayFallback,
    bool lateSetModelAllowed) {
    PedNativeSkinState state;
    FillNativeStateFromTarget(state, ped, key, baseModelId, target, lateSetModelAllowed);
    state.failed = true;
    state.overlayFallback = overlayFallback && g_skinNativeFallback == SKIN_NATIVE_FALLBACK_OVERLAY;
    state.lastLogMs = GetTickCount();
    const bool usesOverlayFallback = state.overlayFallback;
    ReplaceNativeState(key, std::move(state));

    OrcLogError("native skin: fallback=%s ped=%p base=%d kind=%s skin=%s reason=%s",
        usesOverlayFallback ? "overlay" : "vanilla",
        ped,
        baseModelId,
        NativeTargetKindName(target.kind),
        target.name.c_str(),
        reason ? reason : "?");
}

static void DeferNativeApply(CPed* ped, int key, int baseModelId, const NativeSkinTarget& target) {
    PedNativeSkinState state;
    auto existing = g_nativeSkinStates.find(key);
    if (existing != g_nativeSkinStates.end() && existing->second.ped == ped && !existing->second.appliedClump) {
        state.lastLogMs = existing->second.lastLogMs;
        EraseNativeState(existing);
    }

    FillNativeStateFromTarget(state, ped, key, baseModelId, target, true);
    state.appliedClump = nullptr;
    state.failed = false;
    state.applyDeferred = true;
    state.restoreDeferred = false;
    state.overlayFallback = false;
    state.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;

    auto result = ReplaceNativeState(key, std::move(state));
    LogNativeSetModelDeferred(result.first->second, "apply");
}

static bool RestoreNativeState(int key, PedNativeSkinState& state) {
    if (!state.ped || state.baseModelId < 0) {
        EraseNativeStateByKey(key);
        return true;
    }
    if (state.failed || !state.appliedClump) {
        EraseNativeStateByKey(key);
        return true;
    }
    if (!IsPedAliveInPool(state.ped)) {
        EraseNativeStateByKey(key);
        return true;
    }
    if (IsNativeSetModelUnsafe(state.ped)) {
        state.restoreDeferred = true;
        state.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;
        LogNativeSetModelDeferred(state, "restore");
        return false;
    }

    g_nativeSetModelInProgress = true;
    __try {
        CallPedSetModelIndexOriginal(state.ped, (unsigned int)state.baseModelId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin restore: SetModelIndex SEH ex=0x%08X ped=%p base=%d skin=%s",
            GetExceptionCode(), state.ped, state.baseModelId, state.skinName.c_str());
    }
    g_nativeSetModelInProgress = false;
    OrcLogInfo("native skin restore: ped=%p base=%d skin=%s", state.ped, state.baseModelId, state.skinName.c_str());
    EraseNativeStateByKey(key);
    return true;
}

static bool ApplyNativeSkin(CPed* ped, int key, int baseModelId, const NativeSkinTarget& target) {
    if (!ped || baseModelId < 0) {
        MarkNativeFailure(ped, key, baseModelId, target, "invalid ped/base", false, true);
        return false;
    }
    if (!OrcIsValidStandardSkinModel(baseModelId)) {
        MarkNativeFailure(ped, key, baseModelId, target, "invalid base ped model", false, true);
        return false;
    }
    if (!LoadBaseModelForSetModel(baseModelId)) {
        MarkNativeFailure(ped, key, baseModelId, target, "base model not loaded", false, true);
        return false;
    }
    if (!target.rwObject || target.rwObject->type != rpCLUMP || target.txdSlot < 0) {
        MarkNativeFailure(ped, key, baseModelId, target, "skin target is unavailable", false, true);
        return false;
    }

    CBaseModelInfo* baseMi = CModelInfo::GetModelInfo(baseModelId);
    if (!baseMi || baseMi->GetModelType() != MODEL_INFO_PED) {
        MarkNativeFailure(ped, key, baseModelId, target, "base model info is not ped", false, true);
        return false;
    }

    RpClump* templateClump = reinterpret_cast<RpClump*>(target.rwObject);
    CClumpModelInfo* clumpMi = reinterpret_cast<CClumpModelInfo*>(baseMi);
    if (target.prepareTemplate)
        PreparePedClump(templateClump, clumpMi);

    if (IsNativeSetModelUnsafe(ped)) {
        DeferNativeApply(ped, key, baseModelId, target);
        return false;
    }

    PedNativeSkinState pendingState;
    FillNativeStateFromTarget(pendingState, ped, key, baseModelId, target, true);
    if (!AcquireNativeTargetTxdRef(pendingState, target)) {
        MarkNativeFailure(ped, key, baseModelId, target, "target TXD dictionary unavailable", true, true);
        return false;
    }
    if (!AcquireNativeBaseTxdRef(pendingState, baseMi, baseModelId)) {
        ReleaseNativeTargetTxdRef(pendingState);
        MarkNativeFailure(ped, key, baseModelId, target, "base TXD dictionary unavailable", true, true);
        return false;
    }

    RpClump* oldClump = ped->m_pRwClump;
    bool setOk = true;
    {
        ScopedModelInfoOverride scoped(baseMi, target.rwObject, target.txdSlot);
        g_nativeSetModelInProgress = true;
        __try {
            CallPedSetModelIndexOriginal(ped, (unsigned int)baseModelId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            setOk = false;
            OrcLogError("native skin apply: SetModelIndex SEH ex=0x%08X ped=%p base=%d skin=%s",
                GetExceptionCode(), ped, baseModelId, target.name.c_str());
        }
        g_nativeSetModelInProgress = false;
    }

    RpClump* newClump = ped->m_pRwClump;
    if (!setOk || !newClump || newClump == oldClump) {
        ReleaseNativeBaseTxdRef(pendingState);
        ReleaseNativeTargetTxdRef(pendingState);
        MarkNativeFailure(ped, key, baseModelId, target, setOk ? "SetModelIndex did not create skin clump" : "SetModelIndex failed", true, true);
        return false;
    }

    PreparePedClump(newClump, clumpMi);

    StoreAppliedNativeState(std::move(pendingState), newClump);

    OrcLogInfo("native skin apply: ped=%p base=%d kind=%s skin=%s txd=%d clump=%p samp=%s",
        ped,
        baseModelId,
        NativeTargetKindName(target.kind),
        target.name.c_str(),
        target.txdSlot,
        newClump,
        samp_bridge::GetVersionName());
    return true;
}

static void UpdateNativePed(CPed* ped, CPlayerPed* localPlayer) {
    if (!ped || !ped->m_pRwClump)
        return;

    const int key = NativePedKey(ped);
    if (!key)
        return;

    auto it = g_nativeSkinStates.find(key);
    OrcResolvedPedSkin resolved = OrcResolveSkinForPed(ped, localPlayer);
    if (!resolved.custom && !resolved.standard) {
        if (it == g_nativeSkinStates.end())
            return;
        if (resolved.isLocalPed) {
            RestoreNativeState(key, it->second);
            return;
        }
        if (NativeStateOwnsCurrentClump(it->second, ped))
            return;
        EraseNativeState(it);
        return;
    }

    int baseModelId = (int)ped->m_nModelIndex;
    if (it != g_nativeSkinStates.end() && it->second.ped == ped && it->second.baseModelId >= 0) {
        const bool stillOwnsClump = NativeStateOwnsCurrentClump(it->second, ped);
        const bool sameBaseModel = (int)ped->m_nModelIndex == it->second.baseModelId;
        if (stillOwnsClump || sameBaseModel)
            baseModelId = it->second.baseModelId;
    }

    if (!ShouldAllowLateNativeSetModel(resolved)) {
        if (it == g_nativeSkinStates.end())
            return;
        PedNativeSkinState& state = it->second;
        if (NativeStateOwnsCurrentClump(state, ped)) {
            state.applyDeferred = false;
            state.restoreDeferred = false;
            return;
        }
        if (state.ped == ped)
            EraseNativeState(it);
        return;
    }

    NativeSkinTarget target;
    const char* failureReason = nullptr;
    if (!ResolveNativeTarget(resolved, target, failureReason)) {
        if (it != g_nativeSkinStates.end() && !RestoreNativeState(key, it->second))
            return;
        MarkNativeFailure(ped, key, baseModelId, target, failureReason, false, true);
        return;
    }

    if (it != g_nativeSkinStates.end() && NativeStateTargetMatches(it->second, ped, target, baseModelId)) {
        PedNativeSkinState& state = it->second;
        if (state.failed)
            return;
        if (NativeStateOwnsCurrentClump(state, ped)) {
            state.applyDeferred = false;
            state.restoreDeferred = false;
            return;
        }
        if (state.applyDeferred) {
            if (!TickElapsed(state.retryAfterMs))
                return;
            if (IsNativeSetModelUnsafe(ped)) {
                DeferNativeApply(ped, key, baseModelId, target);
                return;
            }
        }
        EraseNativeState(it);
    } else if (it != g_nativeSkinStates.end()) {
        if (!RestoreNativeState(key, it->second))
            return;
    }

    ApplyNativeSkin(ped, key, baseModelId, target);
}

static void __fastcall CPedSetModelIndex_Detour(CPed* ped, void*, unsigned int modelId) {
    if (!g_CPedSetModelIndex_Orig)
        return;

    if (g_nativeSetModelInProgress || g_nativeSetModelHookDepth > 0 ||
        OrcIsRuntimeShuttingDown() ||
        g_skinCustomMode != SKIN_CUSTOM_MODE_NATIVE || (!g_skinModeEnabled && !g_skinRandomFromPools) ||
        !ped || !OrcIsValidStandardSkinModel((int)modelId)) {
        g_CPedSetModelIndex_Orig(ped, modelId);
        return;
    }

    ++g_nativeSetModelHookDepth;
    bool handled = false;
    CPlayerPed* localPlayer = FindPlayerPed(0);
    OrcResolvedPedSkin resolved = OrcResolveSkinForPedModel(ped, localPlayer, (int)modelId);
    if (resolved.custom || resolved.standard) {
        const int key = NativePedKey(ped);
        const bool lateSetModelAllowed = ShouldAllowLateNativeSetModel(resolved);
        CBaseModelInfo* baseMi = CModelInfo::GetModelInfo((int)modelId);
        NativeSkinTarget target;
        const char* failureReason = nullptr;
        const bool targetReady = ResolveNativeTarget(resolved, target, failureReason);
        if (!key || !baseMi || baseMi->GetModelType() != MODEL_INFO_PED) {
            g_CPedSetModelIndex_Orig(ped, modelId);
            handled = true;
        } else if (!targetReady) {
            MarkNativeFailure(ped, key, (int)modelId, target, failureReason, false, lateSetModelAllowed);
            g_nativeSetModelInProgress = true;
            __try {
                g_CPedSetModelIndex_Orig(ped, modelId);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                OrcLogError("native skin hook fallback: SetModelIndex SEH ex=0x%08X ped=%p base=%u skin=%s",
                    GetExceptionCode(), ped, modelId, target.name.c_str());
            }
            g_nativeSetModelInProgress = false;
            handled = true;
        } else {
            CClumpModelInfo* clumpMi = reinterpret_cast<CClumpModelInfo*>(baseMi);
            RpClump* templateClump = reinterpret_cast<RpClump*>(target.rwObject);
            if (target.prepareTemplate)
                PreparePedClump(templateClump, clumpMi);

            PedNativeSkinState pendingState;
            FillNativeStateFromTarget(pendingState, ped, key, (int)modelId, target, lateSetModelAllowed);
            if (!AcquireNativeTargetTxdRef(pendingState, target)) {
                MarkNativeFailure(ped, key, (int)modelId, target, "target TXD dictionary unavailable in SetModelIndex hook", true, lateSetModelAllowed);

                g_nativeSetModelInProgress = true;
                __try {
                    g_CPedSetModelIndex_Orig(ped, modelId);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    OrcLogError("native skin hook fallback: SetModelIndex SEH ex=0x%08X ped=%p base=%u skin=%s",
                        GetExceptionCode(), ped, modelId, target.name.c_str());
                }
                g_nativeSetModelInProgress = false;
            } else if (!AcquireNativeBaseTxdRef(pendingState, baseMi, (int)modelId)) {
                ReleaseNativeBaseTxdRef(pendingState);
                ReleaseNativeTargetTxdRef(pendingState);
                MarkNativeFailure(ped, key, (int)modelId, target, "base TXD dictionary unavailable in SetModelIndex hook", true, lateSetModelAllowed);

                g_nativeSetModelInProgress = true;
                __try {
                    g_CPedSetModelIndex_Orig(ped, modelId);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    OrcLogError("native skin hook fallback: SetModelIndex SEH ex=0x%08X ped=%p base=%u skin=%s",
                        GetExceptionCode(), ped, modelId, target.name.c_str());
                }
                g_nativeSetModelInProgress = false;
            } else {
                bool setOk = true;
                {
                    ScopedModelInfoOverride scoped(baseMi, target.rwObject, target.txdSlot);
                    g_nativeSetModelInProgress = true;
                    __try {
                        g_CPedSetModelIndex_Orig(ped, modelId);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        setOk = false;
                        OrcLogError("native skin hook: SetModelIndex SEH ex=0x%08X ped=%p base=%u skin=%s",
                            GetExceptionCode(), ped, modelId, target.name.c_str());
                    }
                    g_nativeSetModelInProgress = false;
                }

                RpClump* newClump = ped ? ped->m_pRwClump : nullptr;
                if (setOk && newClump) {
                    PreparePedClump(newClump, clumpMi);
                    StoreAppliedNativeState(std::move(pendingState), newClump);
                    OrcLogInfo("native skin hook apply: ped=%p base=%u kind=%s skin=%s txd=%d clump=%p samp=%s",
                        ped,
                        modelId,
                        NativeTargetKindName(target.kind),
                        target.name.c_str(),
                        target.txdSlot,
                        newClump,
                        samp_bridge::GetVersionName());
                } else {
                    ReleaseNativeBaseTxdRef(pendingState);
                    ReleaseNativeTargetTxdRef(pendingState);
                    MarkNativeFailure(ped, key, (int)modelId, target, setOk ? "SetModelIndex hook produced no clump" : "SetModelIndex hook failed", true, lateSetModelAllowed);
                }
            }
            handled = true;
        }
    }

    if (!handled)
        g_CPedSetModelIndex_Orig(ped, modelId);
    --g_nativeSetModelHookDepth;
}

} // namespace

void OrcSkinNativeInstallHooks() {
    if (g_nativeSetModelHookInstalled)
        return;
    g_nativeSetModelHookInstalled = true;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("native skin hook: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }

    InstallNativeTextureDictionaryGuards();

    st = MH_CreateHook(reinterpret_cast<void*>(kAddr_CPed_SetModelIndex),
        reinterpret_cast<void*>(&CPedSetModelIndex_Detour),
        reinterpret_cast<void**>(&g_CPedSetModelIndex_Orig));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        OrcLogError("native skin hook: MH_CreateHook CPed::SetModelIndex -> %s", MH_StatusToString(st));
        return;
    }

    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CPed_SetModelIndex));
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        OrcLogError("native skin hook: MH_EnableHook CPed::SetModelIndex -> %s", MH_StatusToString(st));
        return;
    }

    OrcLogInfo("native skin hook installed (CPed::SetModelIndex 0x%08X)", (unsigned)kAddr_CPed_SetModelIndex);
}

void OrcSkinNativeUpdateForPeds(CPlayerPed* localPlayer) {
    if (OrcIsRuntimeShuttingDown())
        return;

    if (g_skinCustomMode != SKIN_CUSTOM_MODE_NATIVE || (!g_skinModeEnabled && !g_skinRandomFromPools)) {
        OrcSkinNativeClearRuntimeState();
        return;
    }
    if (!localPlayer || !CPools::ms_pPedPool) {
        OrcSkinNativeClearRuntimeState();
        return;
    }

    PruneNativeStates();
    bool localDone = false;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
        CPed* ped = CPools::ms_pPedPool->GetAt(i);
        if (!ped)
            continue;
        if (ped == localPlayer)
            localDone = true;
        UpdateNativePed(ped, localPlayer);
    }
    if (!localDone)
        UpdateNativePed(localPlayer, localPlayer);
}

void OrcSkinNativeOnPedSetModel(CPed* ped, int) {
    if (!ped || g_nativeSetModelInProgress || OrcIsRuntimeShuttingDown())
        return;
    const int key = NativePedKey(ped);
    if (key)
        EraseNativeStateByKey(key);
}

void OrcSkinNativeClearRuntimeState() {
    if (OrcIsRuntimeShuttingDown()) {
        OrcSkinNativeShutdown();
        return;
    }

    PruneNativeStates();
    for (auto it = g_nativeSkinStates.begin(); it != g_nativeSkinStates.end();) {
        if (!it->second.lateSetModelAllowed) {
            if (NativeStateOwnsCurrentClump(it->second, it->second.ped))
                ++it;
            else
                it = EraseNativeState(it);
            continue;
        }

        const int key = it->first;
        if (RestoreNativeState(key, it->second))
            it = g_nativeSkinStates.begin();
        else
            ++it;
    }
}

void OrcSkinNativeOnSkinAssetsReleased() {
    if (OrcIsRuntimeShuttingDown()) {
        OrcSkinNativeShutdown();
        return;
    }

    for (auto it = g_nativeSkinStates.begin(); it != g_nativeSkinStates.end();) {
        const PedNativeSkinState& state = it->second;
        if (state.failed || state.applyDeferred || !state.appliedClump)
            it = EraseNativeState(it);
        else
            ++it;
    }
    PruneNativeStates();
}

void OrcSkinNativeShutdown() {
    for (auto& kv : g_nativeSkinStates) {
        ReleaseNativeBaseTxdRef(kv.second);
        ReleaseNativeTargetTxdRef(kv.second);
    }
    g_nativeSkinStates.clear();
    g_nativeSetModelInProgress = false;
    g_nativeSetModelHookDepth = 0;
}

bool OrcSkinNativeGetActiveInfo(CPed* ped, OrcNativeSkinActiveInfo& out) {
    out = OrcNativeSkinActiveInfo{};
    const int key = NativePedKey(ped);
    if (!key)
        return false;
    auto it = g_nativeSkinStates.find(key);
    if (it == g_nativeSkinStates.end())
        return false;
    const PedNativeSkinState& state = it->second;
    if (state.failed || !state.appliedClump || !ped || ped->m_pRwClump != state.appliedClump)
        return false;
    out.baseModelId = state.baseModelId;
    out.txdSlot = state.txdSlot;
    out.clump = state.appliedClump;
    out.dffName = state.remapKey.empty() ? state.skinName.c_str() : state.remapKey.c_str();
    out.fallbackDffName = state.remapFallbackKey.empty() ? nullptr : state.remapFallbackKey.c_str();
    return true;
}

bool OrcSkinNativeShouldOverlayFallback(CPed* ped, const CustomSkinCfg* skin) {
    if (!ped || !skin || g_skinCustomMode != SKIN_CUSTOM_MODE_NATIVE ||
        g_skinNativeFallback != SKIN_NATIVE_FALLBACK_OVERLAY) {
        return false;
    }
    const int key = NativePedKey(ped);
    auto it = g_nativeSkinStates.find(key);
    if (it == g_nativeSkinStates.end())
        return false;
    const PedNativeSkinState& state = it->second;
    return state.failed &&
        state.overlayFallback &&
        state.targetKind == NativeSkinTargetKind::Custom &&
        state.skinNameLower == OrcToLowerAscii(skin->name);
}

bool OrcSkinNativeShouldOverlayFallback(CPed* ped, const StandardSkinCfg* skin) {
    if (!ped || !skin || g_skinCustomMode != SKIN_CUSTOM_MODE_NATIVE ||
        g_skinNativeFallback != SKIN_NATIVE_FALLBACK_OVERLAY) {
        return false;
    }
    const int key = NativePedKey(ped);
    auto it = g_nativeSkinStates.find(key);
    if (it == g_nativeSkinStates.end())
        return false;
    const PedNativeSkinState& state = it->second;
    return state.failed &&
        state.overlayFallback &&
        state.targetKind == NativeSkinTargetKind::Standard &&
        state.standardModelId == skin->modelId;
}
