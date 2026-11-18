#include "orc_skin_native.h"

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CAnimBlendAssociation.h"
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

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "orc_app.h"
#include "orc_log.h"
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
    std::string remapKey;
    std::string remapFallbackKey;
    int txdSlot = -1;
    int baseTxdSlot = -1;
    RpClump* appliedClump = nullptr;
    bool failed = false;
    bool overlayFallback = false;
    bool baseTxdRefAdded = false;
    bool targetTxdRefAdded = false;
    bool lateSetModelAllowed = false;
    unsigned int assetsRevision = 0;
    DWORD retryAfterMs = 0;
    DWORD lastLogMs = 0;
};

struct NativeSkinTarget {
    NativeSkinTargetKind kind = NativeSkinTargetKind::Custom;
    int standardModelId = -1;
    std::string_view name;
    std::string_view remapKey;
    std::string_view remapFallbackKey;
    int txdSlot = -1;
    RwObject* rwObject = nullptr;
};

using NativeSkinStateMap = std::unordered_map<int, PedNativeSkinState>;

static NativeSkinStateMap g_nativeSkinStates;
static bool g_nativeSetModelInProgress = false;
static bool g_nativeSetModelHookInstalled = false;
static bool g_insideNativeSetModelHook = false;
static unsigned int g_nativeAssetsRevision = 0;
static DWORD g_nativePruneAfterMs = 0;
static constexpr DWORD kNativeSkinRetryMs = 500;
static constexpr DWORD kNativeSkinPruneMs = 500;
static constexpr DWORD kNativeSkinFailureRetryMs = 3000;
static constexpr DWORD kNativeSkinDeferLogMs = 3000;
static constexpr uintptr_t kAddr_CPed_SetModelIndex = 0x5E4880;
static constexpr uintptr_t kAddr_RenderPed = 0x7335B0;
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
    if (st == MH_OK || (st == MH_ERROR_ALREADY_CREATED && g_GetFirstTexture_Orig)) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_GetFirstTexture));
        if (st != MH_OK && st != MH_ERROR_ENABLED)
            OrcLogError("native skin guard: MH_EnableHook GetFirstTexture -> %s", MH_StatusToString(st));
    } else {
        OrcLogError("native skin guard: MH_CreateHook GetFirstTexture -> %s", MH_StatusToString(st));
    }

    st = MH_CreateHook(reinterpret_cast<void*>(kAddr_RwTexDictionaryFindHashNamedTexture),
        reinterpret_cast<void*>(&RwTexDictionaryFindHashNamedTexture_Detour),
        reinterpret_cast<void**>(&g_RwTexDictionaryFindHashNamedTexture_Orig));
    if (st == MH_OK || (st == MH_ERROR_ALREADY_CREATED && g_RwTexDictionaryFindHashNamedTexture_Orig)) {
        st = MH_EnableHook(reinterpret_cast<void*>(kAddr_RwTexDictionaryFindHashNamedTexture));
        if (st != MH_OK && st != MH_ERROR_ENABLED)
            OrcLogError("native skin guard: MH_EnableHook FindHashNamedTexture -> %s", MH_StatusToString(st));
    } else {
        OrcLogError("native skin guard: MH_CreateHook FindHashNamedTexture -> %s", MH_StatusToString(st));
    }
}

static CPed* GetNativePedAtRef(int ref) {
    auto* pool = CPools::ms_pPedPool;
    if (!pool || !pool->m_pObjects || !pool->m_byteMap || ref < 0 || (ref & 0x80) != 0)
        return nullptr;
    return pool->GetAtRef(ref);
}

static int NativePedRef(CPed* ped) {
    if (!ped)
        return -1;
    const int ref = OrcSafeGetPedRef(ped);
    return GetNativePedAtRef(ref) == ped ? ref : -1;
}

static bool NativeStateTargetMatches(const PedNativeSkinState& state, const NativeSkinTarget& target, int baseModelId) {
    return state.baseModelId == baseModelId &&
        state.targetKind == target.kind &&
        state.standardModelId == target.standardModelId &&
        state.skinName == target.name;
}

static const char* NativeTargetKindName(NativeSkinTargetKind kind) {
    return kind == NativeSkinTargetKind::Standard ? "standard" : "custom";
}

static bool NativeStateOwnsCurrentClump(const PedNativeSkinState& state, const CPed* ped) {
    return ped &&
        state.ped == ped &&
        !state.failed &&
        state.appliedClump &&
        ped->m_pRwClump == state.appliedClump &&
        ped->m_nModelIndex == state.baseModelId;
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
            ped->m_pAttachedTo != nullptr ||
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

static void PruneNativeStates() {
    for (auto it = g_nativeSkinStates.begin(); it != g_nativeSkinStates.end();) {
        auto& state = it->second;
        if (GetNativePedAtRef(state.pedRef) != state.ped ||
            (state.appliedClump && !NativeStateOwnsCurrentClump(state, state.ped))) {
            ReleaseNativeBaseTxdRef(state);
            ReleaseNativeTargetTxdRef(state);
            it = g_nativeSkinStates.erase(it);
        } else {
            ++it;
        }
    }
}

static RpAtomic* NativePedAtomicCB(RpAtomic* atomic, void*) {
    if (!atomic)
        return atomic;
    CVisibilityPlugins::SetAtomicRenderCallback(atomic, reinterpret_cast<RpAtomic*(__cdecl*)(RpAtomic*)>(kAddr_RenderPed));
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
    CBaseModelInfo* modelInfo;
    RwObject* oldObject;

    ScopedModelInfoOverride(CBaseModelInfo* mi, RwObject* object)
        : modelInfo(mi), oldObject(mi->m_pRwObject) {
        // CEntity pairs AddRef/RemoveRef through the base model's TXD index.
        // The cloned materials keep their textures from the pinned target TXD.
        modelInfo->m_pRwObject = object;
    }

    ~ScopedModelInfoOverride() {
        modelInfo->m_pRwObject = oldObject;
    }
};

struct ScopedBaseModelRef {
    CBaseModelInfo* modelInfo;

    explicit ScopedBaseModelRef(CBaseModelInfo* mi) : modelInfo(mi) {
        if (modelInfo)
            modelInfo->AddRef();
    }

    ~ScopedBaseModelRef() {
        if (modelInfo)
            modelInfo->RemoveRef();
    }
};

static bool LoadBaseModelForSetModel(int modelId) {
    if (!CStreaming::HasModelLoaded(modelId)) {
        CStreaming::RequestModel(modelId, DEFAULT);
        CStreaming::LoadAllRequestedModels(false);
    }
    return CStreaming::HasModelLoaded(modelId);
}

static bool TrySetPedModel(CPed* ped, unsigned int modelId) {
    bool ok = false;
    g_nativeSetModelInProgress = true;
    __try {
        if (g_CPedSetModelIndex_Orig)
            g_CPedSetModelIndex_Orig(ped, modelId);
        else
            ped->SetModelIndex(modelId);
        ok = ped->m_pRwObject && ped->m_pRwObject->type == rpCLUMP &&
            ped->m_nModelIndex == static_cast<int>(modelId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: SetModelIndex SEH ex=0x%08X ped=%p base=%u", GetExceptionCode(), ped, modelId);
    }
    g_nativeSetModelInProgress = false;
    return ok;
}

static bool TryDeletePedClump(CPed* ped) {
    __try {
        if (ped->m_pRwClump)
            ped->DeleteRwObject();
        return ped->m_pRwClump == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: DeleteRwObject SEH ex=0x%08X ped=%p", GetExceptionCode(), ped);
        return false;
    }
}

static bool HaveSameSkeleton(RpClump* previous, RpClump* replacement) {
    if (!previous || !replacement)
        return false;
    const auto* oldHierarchy = GetAnimHierarchyFromSkinClump(previous);
    const auto* newHierarchy = GetAnimHierarchyFromSkinClump(replacement);
    if (!oldHierarchy || !newHierarchy || oldHierarchy->numNodes != newHierarchy->numNodes ||
        !oldHierarchy->pNodeInfo || !newHierarchy->pNodeInfo)
        return false;
    for (int index = 0; index < oldHierarchy->numNodes; ++index) {
        if (oldHierarchy->pNodeInfo[index].nodeID != newHierarchy->pNodeInfo[index].nodeID)
            return false;
    }
    return true;
}

static CAnimBlendAssociation* ExtractPedAnimations(CPed* ped, RpClump* replacement) {
    RpClump* previous = ped->m_pRwClump;
    if (!previous || !RpAnimBlendClumpIsInitialized(previous) ||
        !RpAnimBlendClumpGetFirstAssociation(previous) || !HaveSameSkeleton(previous, replacement))
        return nullptr;
    return RpAnimBlendClumpExtractAssociations(previous);
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
    if (state.targetKind == NativeSkinTargetKind::Custom) {
        OrcReleaseCustomSkinTxd(txdSlot);
        return;
    }
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
            NativeTargetKindName(target.kind), target.name.data(), txdSlot);
        return false;
    }

    __try {
        CTxdStore::AddRef(txdSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("native skin: target TXD AddRef SEH ex=0x%08X kind=%s skin=%s txd=%d",
            GetExceptionCode(), NativeTargetKindName(target.kind), target.name.data(), txdSlot);
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
            CStreaming::RequestTxdModel(txdSlot, DEFAULT);
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
    int ref,
    int baseModelId,
    const NativeSkinTarget& target,
    bool lateSetModelAllowed) {
    state.ped = ped;
    state.pedRef = ref;
    state.baseModelId = baseModelId;
    state.targetKind = target.kind;
    state.standardModelId = target.standardModelId;
    state.skinName = target.name;
    state.remapKey = target.remapKey.empty() ? target.name : target.remapKey;
    state.remapFallbackKey = target.remapFallbackKey;
    state.txdSlot = target.txdSlot;
    state.lateSetModelAllowed = lateSetModelAllowed;
    state.assetsRevision = g_nativeAssetsRevision;
}

static NativeSkinTarget DescribeNativeTarget(const OrcResolvedPedSkin& resolved) {
    NativeSkinTarget target;
    if (const auto* skin = resolved.custom) {
        target.name = skin->name;
        target.remapKey = skin->remapKey;
        target.remapFallbackKey = skin->remapFallbackKey;
    } else if (const auto* skin = resolved.standard) {
        target.kind = NativeSkinTargetKind::Standard;
        target.standardModelId = skin->modelId;
        target.name = skin->dffName;
        target.remapKey = skin->dffName;
    }
    return target;
}

static bool IsNativePedTemplate(RwObject* object) {
    if (!object || object->type != rpCLUMP)
        return false;

    auto* clump = reinterpret_cast<RpClump*>(object);
    RpAtomic* first = GetFirstAtomic(clump);
    RpSkin* skin = first && first->geometry ? RpSkinGeometryGetSkin(first->geometry) : nullptr;
    auto* hierarchy = skin ? GetAnimHierarchyFromSkinClump(clump) : nullptr;
    if (!hierarchy || !hierarchy->pNodeInfo || !hierarchy->currentAnim || !hierarchy->pMatrixArray)
        return false;

    // RpAnimBlendClumpInit (0x4D6510) uses a stack buffer for 64 bind positions.
    const unsigned int count = RpSkinGetNumBones(skin);
    if (count == 0 || count > 64 || hierarchy->numNodes != static_cast<int>(count) ||
        hierarchy->currentAnim->numNodes != hierarchy->numNodes || !RpSkinGetSkinToBoneMatrices(skin))
        return false;

    // RpAnimBlendClumpFillFrameArray uses these GTA tags without checking a missing index.
    constexpr int requiredTags[] = { 3, 5, 32, 22, 34, 24, 41, 51, 43, 53, 52, 42, 33, 23, 31, 21, 4, 8 };
    for (const int tag : requiredTags) {
        if (RpHAnimIDGetIndex(hierarchy, tag) < 0)
            return false;
    }
    return true;
}

static const char* LoadNativeTarget(const OrcResolvedPedSkin& resolved, NativeSkinTarget& target, bool& retryable) {
    if (resolved.custom) {
        CustomSkinCfg& skin = *resolved.custom;
        if (!OrcEnsureCustomSkinLoaded(skin)) {
            retryable = !skin.loadFailed;
            return retryable ? "custom DFF/TXD temporarily unavailable" : "custom DFF/TXD load failed";
        }
        target.txdSlot = skin.txdSlot;
        target.rwObject = skin.rwObject;
    } else {
        const int targetModel = target.standardModelId;
        if (!OrcIsValidStandardSkinModel(targetModel))
            return "invalid standard ped model";
        if (!LoadBaseModelForSetModel(targetModel)) {
            retryable = true;
            return "standard ped model not loaded";
        }

        CBaseModelInfo* targetMi = CModelInfo::GetModelInfo(targetModel);
        if (!targetMi)
            return "standard ped model info unavailable";
        target.txdSlot = targetMi->m_nTxdIndex;
        target.rwObject = targetMi->m_pRwObject;
    }

    if (!IsNativePedTemplate(target.rwObject))
        return "incompatible GTA ped skin hierarchy";
    return nullptr;
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
    bool lateSetModelAllowed,
    bool retryable = false) {
    PedNativeSkinState state;
    FillNativeStateFromTarget(state, ped, key, baseModelId, target, lateSetModelAllowed);
    state.failed = true;
    state.overlayFallback = overlayFallback;
    if (retryable && lateSetModelAllowed)
        state.retryAfterMs = GetTickCount() + kNativeSkinFailureRetryMs;
    const bool usesOverlayFallback = overlayFallback && g_skinNativeFallback == SKIN_NATIVE_FALLBACK_OVERLAY;
    ReplaceNativeState(key, std::move(state));

    OrcLogError("native skin: fallback=%s ped=%p base=%d kind=%s skin=%s reason=%s",
        usesOverlayFallback ? "overlay" : "vanilla",
        ped,
        baseModelId,
        NativeTargetKindName(target.kind),
        target.name.data(),
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
    state.overlayFallback = false;
    state.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;

    auto result = ReplaceNativeState(key, std::move(state));
    LogNativeSetModelDeferred(result.first->second, "apply");
}

// DeleteRwObject pairs the model reference and streaming registration added by SetModelIndex.
// The old and new clump can have the same address after the allocator reuses the freed block.
struct PedClumpChange {
    bool oldRemoved = false;
    bool replaced = false;
};

static void RestorePedAnimations(CPed* ped, CAnimBlendAssociation* saved) {
    if (!saved)
        return;
    if (ped->m_pRwClump && RpAnimBlendClumpIsInitialized(ped->m_pRwClump)) {
        RpAnimBlendClumpGiveAssociations(ped->m_pRwClump, saved);
        return;
    }

    // Destroy detached animations using the game's deleting destructor.
    auto* association = saved;
    while (association) {
        auto* next = association->m_link.next;
        using DeleteAssociation = void(__thiscall*)(CAnimBlendAssociation*, unsigned int);
        (*reinterpret_cast<DeleteAssociation*>(association->vtable))(association, 1);
        association = next
            ? reinterpret_cast<CAnimBlendAssociation*>(reinterpret_cast<char*>(next) - offsetof(CAnimBlendAssociation, m_link))
            : nullptr;
    }
}

static PedClumpChange ReplacePedClump(CPed* ped, unsigned int modelId, CBaseModelInfo* baseMi, RwObject* replacement) {
    PedClumpChange result;
    CAnimBlendAssociation* saved = nullptr;
    const auto money = ped->m_nMoneyCount;
    const auto animGroup = ped->m_nAnimGroup;
    const auto turnRate = ped->m_fHeadingChangeRate;
    const auto stats = ped->m_pStats;
    const bool wasVisible = ped->bIsVisible;
    const int alpha = CVisibilityPlugins::GetClumpAlpha(ped->m_pRwClump);
    auto* originalTemplate = baseMi->m_pRwObject;
    if (!replacement || HaveSameSkeleton(reinterpret_cast<RpClump*>(originalTemplate), reinterpret_cast<RpClump*>(replacement)))
        saved = ExtractPedAnimations(ped, reinterpret_cast<RpClump*>(originalTemplate));

    if (TryDeletePedClump(ped)) {
        result.oldRemoved = true;
        if (replacement) {
            ScopedModelInfoOverride scoped(baseMi, replacement);
            result.replaced = TrySetPedModel(ped, modelId);
        } else {
            result.replaced = TrySetPedModel(ped, modelId);
        }
        if (!result.replaced && replacement && TryDeletePedClump(ped))
            TrySetPedModel(ped, modelId);
    }

    RestorePedAnimations(ped, saved);
    if (ped->m_pRwClump)
        CVisibilityPlugins::SetClumpAlpha(ped->m_pRwClump, alpha);
    ped->m_nMoneyCount = money;
    ped->m_nAnimGroup = animGroup;
    ped->m_fHeadingChangeRate = turnRate;
    ped->m_pStats = stats;
    ped->bIsVisible = wasVisible;
    return result;
}

static bool RestoreNativeState(int ref, PedNativeSkinState& state) {
    CPed* ped = GetNativePedAtRef(ref);
    if (!NativeStateOwnsCurrentClump(state, ped)) {
        EraseNativeStateByKey(ref);
        return true;
    }
    if (!TickElapsed(state.retryAfterMs))
        return false;
    if (IsNativeSetModelUnsafe(ped)) {
        state.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;
        LogNativeSetModelDeferred(state, "restore");
        return false;
    }

    CBaseModelInfo* baseMi = CModelInfo::GetModelInfo(state.baseModelId);
    if (!baseMi || !LoadBaseModelForSetModel(state.baseModelId) || !baseMi->m_pRwObject) {
        state.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;
        return false;
    }
    const ScopedBaseModelRef pinBase(baseMi);
    const auto change = ReplacePedClump(ped, state.baseModelId, baseMi, nullptr);
    if (!change.oldRemoved) {
        state.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;
        return false;
    }
    if (change.replaced) {
        OrcLogInfo("native skin restore: ped=%p base=%d skin=%s", ped, state.baseModelId, state.skinName.c_str());
    } else {
        OrcLogError("native skin restore: no original clump ped=%p base=%d skin=%s", ped, state.baseModelId, state.skinName.c_str());
    }
    EraseNativeStateByKey(ref);
    return change.replaced;
}

static bool ApplyNativeSkin(CPed* ped, int ref, int baseModelId, CBaseModelInfo* baseMi,
    const OrcResolvedPedSkin& resolved, NativeSkinTarget& target, bool late) {
    const bool lateAllowed = ShouldAllowLateNativeSetModel(resolved);
    PedNativeSkinState pending;
    FillNativeStateFromTarget(pending, ped, ref, baseModelId, target, lateAllowed);

    const bool hasBaseTxd = AcquireNativeBaseTxdRef(pending, baseMi, baseModelId);
    const ScopedBaseModelRef pinBase(hasBaseTxd ? baseMi : nullptr);
    bool retryable = !hasBaseTxd;
    const char* failure = hasBaseTxd ? LoadNativeTarget(resolved, target, retryable) : "base TXD dictionary unavailable";
    if (!failure && !AcquireNativeTargetTxdRef(pending, target)) {
        failure = "target TXD dictionary unavailable";
        retryable = true;
    }

    if (failure) {
        ReleaseNativeBaseTxdRef(pending);
        ReleaseNativeTargetTxdRef(pending);
        if (late) {
            auto current = g_nativeSkinStates.find(ref);
            if (current != g_nativeSkinStates.end() && !RestoreNativeState(ref, current->second))
                return false;
        } else {
            TryDeletePedClump(ped);
            TrySetPedModel(ped, baseModelId);
        }
        MarkNativeFailure(ped, ref, baseModelId, target, failure, target.rwObject != nullptr, lateAllowed, retryable);
        return false;
    }

    bool applied = false;
    bool oldRemoved = true;
    if (late) {
        const auto change = ReplacePedClump(ped, baseModelId, baseMi, target.rwObject);
        oldRemoved = change.oldRemoved;
        applied = change.replaced;
    } else {
        oldRemoved = TryDeletePedClump(ped);
        if (oldRemoved) {
            {
                ScopedModelInfoOverride scoped(baseMi, target.rwObject);
                applied = TrySetPedModel(ped, baseModelId);
            }
            if (!applied && TryDeletePedClump(ped))
                TrySetPedModel(ped, baseModelId);
        } else {
            TrySetPedModel(ped, baseModelId);
        }
    }

    if (!applied) {
        ReleaseNativeBaseTxdRef(pending);
        ReleaseNativeTargetTxdRef(pending);
        if (oldRemoved)
            MarkNativeFailure(ped, ref, baseModelId, target, "SetModelIndex did not produce a replacement clump", true, lateAllowed);
        return false;
    }

    auto* clump = ped->m_pRwClump;
    PreparePedClump(clump, reinterpret_cast<CClumpModelInfo*>(baseMi));
    StoreAppliedNativeState(std::move(pending), clump);
    OrcLogInfo("native skin %s: ped=%p base=%d kind=%s skin=%s txd=%d clump=%p samp=%s",
        late ? "apply" : "hook apply", ped, baseModelId, NativeTargetKindName(target.kind),
        target.name.data(), target.txdSlot, clump, samp_bridge::GetVersionName());
    return true;
}

static void UpdateNativePed(CPed* ped, CPlayerPed* localPlayer) {
    const int ref = NativePedRef(ped);
    if (ref < 0 || !ped->m_pRwClump)
        return;

    auto current = g_nativeSkinStates.find(ref);
    if (!g_skinModeEnabled && !g_skinNickMode &&
        (current == g_nativeSkinStates.end() || !current->second.lateSetModelAllowed))
        return;

    const OrcResolvedPedSkin resolved = OrcResolveSkinForPed(ped, localPlayer);
    if ((!resolved.custom && !resolved.standard) || !ShouldAllowLateNativeSetModel(resolved)) {
        if (current != g_nativeSkinStates.end() && current->second.lateSetModelAllowed)
            RestoreNativeState(ref, current->second);
        return;
    }

    const int baseModelId = ped->m_nModelIndex;
    NativeSkinTarget target = DescribeNativeTarget(resolved);
    const bool matches = current != g_nativeSkinStates.end() &&
        current->second.assetsRevision == g_nativeAssetsRevision &&
        NativeStateTargetMatches(current->second, target, baseModelId);
    if (matches) {
        auto& state = current->second;
        if (NativeStateOwnsCurrentClump(state, ped)) {
            state.retryAfterMs = 0;
            return;
        }
        if (state.failed && state.retryAfterMs == 0)
            return;
    }
    if (current != g_nativeSkinStates.end() && !TickElapsed(current->second.retryAfterMs))
        return;
    if (IsNativeSetModelUnsafe(ped)) {
        if (current != g_nativeSkinStates.end() && NativeStateOwnsCurrentClump(current->second, ped)) {
            current->second.retryAfterMs = GetTickCount() + kNativeSkinRetryMs;
            LogNativeSetModelDeferred(current->second, "change");
        } else {
            DeferNativeApply(ped, ref, baseModelId, target);
        }
        return;
    }

    if (!OrcIsValidStandardSkinModel(baseModelId)) {
        MarkNativeFailure(ped, ref, baseModelId, target, "invalid base ped model", false, true);
        return;
    }
    if (!LoadBaseModelForSetModel(baseModelId)) {
        MarkNativeFailure(ped, ref, baseModelId, target, "base ped model unavailable", false, true, true);
        return;
    }

    CBaseModelInfo* baseMi = CModelInfo::GetModelInfo(baseModelId);
    ApplyNativeSkin(ped, ref, baseModelId, baseMi, resolved, target, true);
}

static void RunNativeSetModelHook(CPed* ped, unsigned int modelId) {
    const int ref = NativePedRef(ped);
    if (ref < 0 || !OrcIsValidStandardSkinModel(static_cast<int>(modelId))) {
        g_CPedSetModelIndex_Orig(ped, modelId);
        return;
    }

    const OrcResolvedPedSkin resolved = OrcResolveSkinForPedModel(ped, FindPlayerPed(0), static_cast<int>(modelId));
    if (!resolved.custom && !resolved.standard) {
        g_CPedSetModelIndex_Orig(ped, modelId);
        EraseNativeStateByKey(ref);
        return;
    }

    CBaseModelInfo* baseMi = CModelInfo::GetModelInfo(static_cast<int>(modelId));
    if (!baseMi->m_pRwObject || (ped->m_pRwClump &&
        (IsNativeSetModelUnsafe(ped) || !OrcIsValidStandardSkinModel(ped->m_nModelIndex)))) {
        g_CPedSetModelIndex_Orig(ped, modelId);
        EraseNativeStateByKey(ref);
        return;
    }

    NativeSkinTarget target = DescribeNativeTarget(resolved);
    ApplyNativeSkin(ped, ref, static_cast<int>(modelId), baseMi, resolved, target, false);
}

static void __fastcall CPedSetModelIndex_Detour(CPed* ped, void*, unsigned int modelId) {
    if (g_insideNativeSetModelHook || g_nativeSetModelInProgress || OrcIsRuntimeShuttingDown() || !ped) {
        g_CPedSetModelIndex_Orig(ped, modelId);
        return;
    }
    if (!g_enabled || g_skinCustomMode != SKIN_CUSTOM_MODE_NATIVE || (!g_skinModeEnabled && !g_skinRandomFromPools)) {
        g_CPedSetModelIndex_Orig(ped, modelId);
        if (!g_nativeSkinStates.empty())
            EraseNativeStateByKey(NativePedRef(ped));
        return;
    }

    g_insideNativeSetModelHook = true;
    __try {
        RunNativeSetModelHook(ped, modelId);
    } __finally {
        g_insideNativeSetModelHook = false;
    }
}

} // namespace

void OrcSkinNativeInstallHooks() {
    if (g_nativeSetModelHookInstalled)
        return;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("native skin hook: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }

    InstallNativeTextureDictionaryGuards();

    st = MH_CreateHook(reinterpret_cast<void*>(kAddr_CPed_SetModelIndex),
        reinterpret_cast<void*>(&CPedSetModelIndex_Detour),
        reinterpret_cast<void**>(&g_CPedSetModelIndex_Orig));
    if (st != MH_OK && (st != MH_ERROR_ALREADY_CREATED || !g_CPedSetModelIndex_Orig)) {
        OrcLogError("native skin hook: MH_CreateHook CPed::SetModelIndex -> %s", MH_StatusToString(st));
        return;
    }

    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CPed_SetModelIndex));
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        OrcLogError("native skin hook: MH_EnableHook CPed::SetModelIndex -> %s", MH_StatusToString(st));
        return;
    }

    g_nativeSetModelHookInstalled = true;
    OrcLogInfo("native skin hook installed (CPed::SetModelIndex 0x%08X)", (unsigned)kAddr_CPed_SetModelIndex);
}

void OrcSkinNativeUpdateForPeds(CPlayerPed* localPlayer) {
    if (OrcIsRuntimeShuttingDown())
        return;

    const bool nativeEnabled = g_enabled && g_skinCustomMode == SKIN_CUSTOM_MODE_NATIVE &&
        (g_skinModeEnabled || g_skinRandomFromPools);
    if (TickElapsed(g_nativePruneAfterMs)) {
        if (nativeEnabled)
            PruneNativeStates();
        else
            OrcSkinNativeClearRuntimeState();
        g_nativePruneAfterMs = GetTickCount() + kNativeSkinPruneMs;
    }

    if (nativeEnabled && localPlayer && (g_skinModeEnabled || g_skinNickMode || !g_nativeSkinStates.empty())) {
        UpdateNativePed(localPlayer, localPlayer);
    }
}

void OrcSkinNativeOnPedSetModel(CPed* ped, int) {
    if (!ped || g_nativeSetModelInProgress || g_insideNativeSetModelHook || OrcIsRuntimeShuttingDown())
        return;
    const int ref = NativePedRef(ped);
    auto current = g_nativeSkinStates.find(ref);
    if (current != g_nativeSkinStates.end() && !NativeStateOwnsCurrentClump(current->second, ped))
        EraseNativeState(current);
}

void OrcSkinNativeClearRuntimeState() {
    if (OrcIsRuntimeShuttingDown()) {
        OrcSkinNativeShutdown();
        return;
    }

    for (auto it = g_nativeSkinStates.begin(); it != g_nativeSkinStates.end();) {
        auto current = it++;
        auto& state = current->second;
        CPed* ped = GetNativePedAtRef(state.pedRef);
        if (!NativeStateOwnsCurrentClump(state, ped)) {
            EraseNativeState(current);
        } else if (state.lateSetModelAllowed) {
            RestoreNativeState(state.pedRef, state);
        }
    }
}

void OrcSkinNativeOnSkinAssetsReleased() {
    if (OrcIsRuntimeShuttingDown()) {
        OrcSkinNativeShutdown();
        return;
    }
    ++g_nativeAssetsRevision;
    for (auto it = g_nativeSkinStates.begin(); it != g_nativeSkinStates.end();) {
        auto& state = it->second;
        if (!NativeStateOwnsCurrentClump(state, GetNativePedAtRef(state.pedRef))) {
            it = EraseNativeState(it);
        } else {
            ++it;
        }
    }
}

void OrcSkinNativeShutdown() {
    for (auto& kv : g_nativeSkinStates) {
        ReleaseNativeBaseTxdRef(kv.second);
        ReleaseNativeTargetTxdRef(kv.second);
    }
    g_nativeSkinStates.clear();
    g_nativeSetModelInProgress = false;
    g_insideNativeSetModelHook = false;
    g_nativePruneAfterMs = 0;
}

bool OrcSkinNativeGetActiveInfo(CPed* ped, OrcNativeSkinActiveInfo& out) {
    out = OrcNativeSkinActiveInfo{};
    const int key = NativePedRef(ped);
    if (key < 0)
        return false;
    auto it = g_nativeSkinStates.find(key);
    if (it == g_nativeSkinStates.end())
        return false;
    const PedNativeSkinState& state = it->second;
    if (!NativeStateOwnsCurrentClump(state, ped))
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
    const int key = NativePedRef(ped);
    auto it = g_nativeSkinStates.find(key);
    if (it == g_nativeSkinStates.end())
        return false;
    const PedNativeSkinState& state = it->second;
    return state.failed &&
        state.overlayFallback &&
        state.targetKind == NativeSkinTargetKind::Custom &&
        state.baseModelId == ped->m_nModelIndex && state.skinName == skin->name;
}

bool OrcSkinNativeShouldOverlayFallback(CPed* ped, const StandardSkinCfg* skin) {
    if (!ped || !skin || g_skinCustomMode != SKIN_CUSTOM_MODE_NATIVE ||
        g_skinNativeFallback != SKIN_NATIVE_FALLBACK_OVERLAY) {
        return false;
    }
    const int key = NativePedRef(ped);
    auto it = g_nativeSkinStates.find(key);
    if (it == g_nativeSkinStates.end())
        return false;
    const PedNativeSkinState& state = it->second;
    return state.failed &&
        state.overlayFallback &&
        state.targetKind == NativeSkinTargetKind::Standard &&
        state.baseModelId == ped->m_nModelIndex && state.standardModelId == skin->modelId;
}
