#include "plugin.h"

#include "CPed.h"
#include "CPools.h"
#include "CTimer.h"
#include "CWeaponInfo.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include "external/MinHook/include/MinHook.h"

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
#include "orc_weapons.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

struct HeldReplacement {
    RwObject* object = nullptr;
    OrcHeldGunflash flash{};
    bool drawFailed = false;
    std::uint64_t drawnFrame = 0;
};

struct HeldWeapon {
    RwObject* stock = nullptr;
    int weaponType = 0;
    bool dual = false;
    std::uint64_t prepared = 0;
    std::string replacementKey;
    HeldReplacement right;
    HeldReplacement left;
    HeldWeaponPoseCfg stockPose[2]{};
    HeldWeaponPoseCfg replacementPose[2]{};
    WeaponTextureAsset* stockTexture = nullptr;
    WeaponTextureAsset* replacementTexture = nullptr;
};

std::unordered_map<int, HeldWeapon> g_heldWeapons;
std::unordered_map<RwObject*, int> g_heldOwners;
std::uint64_t g_heldFrame = 1;
const std::string g_stockKey;

constexpr uintptr_t kAddWeaponModel = 0x5E5ED0;
constexpr uintptr_t kRemoveWeaponModel = 0x5E3990;
constexpr uintptr_t kClumpRender = 0x749B20;
constexpr uintptr_t kDefaultAtomicRender = 0x7491C0;
using WeaponModelOp = void(__thiscall*)(CPed*, int);
using ClumpRender = RpClump*(__cdecl*)(RpClump*);
using AtomicRender = RpAtomic*(__cdecl*)(RpAtomic*);
using SilentPatchWeaponDraw = int(__thiscall*)(CPed**, bool, bool, bool, int);

WeaponModelOp g_addWeaponModel = nullptr;
WeaponModelOp g_removeWeaponModel = nullptr;
ClumpRender g_clumpRender = nullptr;
AtomicRender g_defaultAtomicRender = nullptr;
SilentPatchWeaponDraw g_silentPatchWeaponDraw = nullptr;

RwFrame* WeaponFrame(RwObject* object) {
    if (!object)
        return nullptr;
    if (object->type == rpCLUMP)
        return RpClumpGetFrame(reinterpret_cast<RpClump*>(object));
    if (object->type == rpATOMIC)
        return RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(object));
    return nullptr;
}

bool WeaponTypeMatchesModel(int wt, int modelId) {
    const auto index = static_cast<std::size_t>(wt);
    return modelId > 0 && wt > 0 &&
        ((index < g_weaponModelId.size() && g_weaponModelId[index] == modelId) ||
         (index < g_weaponModelId2.size() && g_weaponModelId2[index] == modelId));
}

int SelectedWeaponType(const CPed* ped) {
    return ped->m_nSelectedWepSlot < 13
        ? static_cast<int>(ped->m_aWeapons[ped->m_nSelectedWepSlot].m_eWeaponType)
        : 0;
}

bool CanModifyHeldWeapon(CPed* ped) {
    if (!g_enabled || !ped)
        return false;
    CPed* player = FindPlayerPed(0);
    if (ped == player)
        return true;
    if (!player || !g_renderAllPedsWeapons)
        return false;
    const CVector d = ped->GetPosition() - player->GetPosition();
    return d.x * d.x + d.y * d.y + d.z * d.z <= g_renderAllPedsRadius * g_renderAllPedsRadius;
}

bool IsNativeDualWield(CPed* ped, int wt) {
    if (!OrcWeaponTypeCanUseVanillaInfoContract(wt, static_cast<int>(WEAPONTYPE_PARACHUTE)))
        return false;
    const auto weapon = static_cast<eWeaponType>(wt);
    const CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon, ped->GetWeaponSkill(weapon));
    return info && info->m_nFlags.bTwinPistol;
}

bool FiniteMatrix(const RwMatrix& matrix) {
    for (const RwV3d* v : {&matrix.right, &matrix.up, &matrix.at, &matrix.pos}) {
        if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z))
            return false;
    }
    return true;
}

void SetFrameMatrix(RwFrame* frame, const RwMatrix& source) {
    RwMatrix* matrix = RwFrameGetMatrix(frame);
    *matrix = source;
    RwMatrixUpdate(matrix);
    RwFrameUpdateObjects(frame);
}

void DestroyReplacement(HeldReplacement& replacement) {
    OrcDestroyRwObjectInstance(replacement.object);
    replacement = {};
}

void ClearReplacement(HeldWeapon& held) {
    DestroyReplacement(held.right);
    DestroyReplacement(held.left);
    held.replacementKey.clear();
}

void ForgetHeldWeapon(int ref) {
    auto it = g_heldWeapons.find(ref);
    if (it == g_heldWeapons.end())
        return;
    auto owner = g_heldOwners.find(it->second.stock);
    if (owner != g_heldOwners.end() && owner->second == ref)
        g_heldOwners.erase(owner);
    ClearReplacement(it->second);
    g_heldWeapons.erase(it);
}

HeldWeapon* RegisterStock(CPed* ped, int ref) {
    RwObject* stock = ped->m_pWeaponObject;
    auto it = g_heldWeapons.find(ref);
    if (it != g_heldWeapons.end()) {
        if (it->second.stock == stock)
            return &it->second;
        ForgetHeldWeapon(ref);
    }
    if (!stock || (stock->type != rpCLUMP && stock->type != rpATOMIC))
        return nullptr;

    HeldWeapon& held = g_heldWeapons[ref];
    held.stock = stock;
    g_heldOwners.insert_or_assign(stock, ref);
    return &held;
}

void CreateReplacement(HeldReplacement& target, CPed* ped, int wt, WeaponReplacementAsset& asset) {
    static_assert(OrcSelectWeaponCloneRenderContract(OrcWeaponCloneUsage::HeldReplacement) ==
        OrcWeaponCloneRenderContract::DefaultAtomicCallback);
    RwObject* clone = OrcCloneWeaponReplacementObject(asset);
    if (!clone || !OrcNormalizeWeaponObjectToClumpForDefaultRender(clone)) {
        OrcDestroyRwObjectInstance(clone);
        target.drawFailed = true;
        return;
    }
    if (!OrcHeldPrepareGunflash(clone, ped, wt, asset, target.flash)) {
        OrcDestroyRwObjectInstance(clone);
        target.flash = {};
        target.drawFailed = true;
        OrcLogError("weapon fx: cannot prepare inherited flash, using stock weapon wt=%d key=%s",
            wt, asset.key.c_str());
        return;
    }
    target.object = clone;
}

bool PrepareHeldWeapon(CPed* ped, HeldWeapon& held) {
    if (held.prepared == g_heldFrame)
        return held.weaponType > 0;
    held.prepared = g_heldFrame;
    held.weaponType = CanModifyHeldWeapon(ped) ? OrcResolveWeaponHeldVisualWeaponType(ped) : 0;
    held.dual = held.weaponType > 0 && IsNativeDualWield(ped, held.weaponType);
    held.stockTexture = nullptr;
    held.replacementTexture = nullptr;

    WeaponReplacementAsset* asset = nullptr;
    if (held.weaponType > 0 && g_weaponReplacementEnabled && g_weaponReplacementInHands)
        asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, held.weaponType, true);
    if (!asset) {
        if (!held.replacementKey.empty())
            ClearReplacement(held);
    } else if (held.replacementKey != asset->key) {
        ClearReplacement(held);
        held.replacementKey = asset->key;
        CreateReplacement(held.right, ped, held.weaponType, *asset);
        if (held.dual)
            CreateReplacement(held.left, ped, held.weaponType, *asset);
    } else if (held.dual) {
        if (held.right.object && !held.left.object && !held.left.drawFailed)
            CreateReplacement(held.left, ped, held.weaponType, *asset);
    } else if (held.left.object || held.left.drawFailed) {
        DestroyReplacement(held.left);
    }
    if (held.weaponType <= 0)
        return false;
    const int hands = held.dual ? 2 : 1;
    for (int hand = 0; hand < hands; ++hand) {
        const bool secondary = hand == 1 && g_considerWeaponSkills;
        held.stockPose[hand] = GetHeldPoseForPed(ped, held.weaponType, secondary, &g_stockKey);
        const HeldReplacement& clone = hand == 1 ? held.left : held.right;
        if (clone.object && !clone.drawFailed)
            held.replacementPose[hand] = GetHeldPoseForPed(ped, held.weaponType, secondary, &held.replacementKey);
    }
    if (g_weaponTexturesEnabled) {
        held.stockTexture = OrcResolveUsableWeaponTextureAssetForPed(ped, held.weaponType, true, &g_stockKey);
        if ((held.right.object && !held.right.drawFailed) || (held.left.object && !held.left.drawFailed))
            held.replacementTexture =
                OrcResolveUsableWeaponTextureAssetForPed(ped, held.weaponType, true, &held.replacementKey);
    }
    return true;
}

bool FindStockOwner(RwObject* stock, CPed*& ped, HeldWeapon*& held) {
    const auto owner = g_heldOwners.find(stock);
    if (owner == g_heldOwners.end())
        return false;
    ped = OrcSafeGetPed(owner->second);
    if (!ped || ped->m_pWeaponObject != stock)
        return false;
    auto it = g_heldWeapons.find(owner->second);
    if (it == g_heldWeapons.end() || it->second.stock != stock)
        return false;
    held = &it->second;
    return PrepareHeldWeapon(ped, *held);
}

float DistanceSquared(const RwV3d& first, const RwV3d& second) {
    const float x = first.x - second.x;
    const float y = first.y - second.y;
    const float z = first.z - second.z;
    return x * x + y * y + z * z;
}

bool IsLeftHand(CPed* ped, const RwMatrix& stockPose) {
    const RwMatrix* right = OrcGetBoneMatrix(ped, BONE_R_HAND);
    RwMatrix left{};
    return right && OrcBuildHeldWeaponHandMatrix(ped, true, left) &&
        DistanceSquared(stockPose.pos, left.pos) < DistanceSquared(stockPose.pos, right->pos);
}

struct HeldDrawScope {
    CPed* ped = nullptr;
    HeldWeapon* held = nullptr;
    RwFrame* frame = nullptr;
    RwMatrix world{};
    RwMatrix originalLocal{};
    std::size_t textureRestorePoint = 0;
    bool left = false;
    bool drawGunflash = true;
    bool stockPrepared = false;
    bool restoreFrame = false;
    OrcHeldStockDrawPass pass;
};

HeldDrawScope* g_activeClumpDraw = nullptr;

bool InitHeldDraw(HeldDrawScope& scope, RwObject* stock, int nativeHand = -1) {
    __try {
        if (!FindStockOwner(stock, scope.ped, scope.held))
            return false;
        scope.frame = WeaponFrame(stock);
        if (!scope.frame)
            return false;
        // SilentPatch writes the root matrix before entering its per-hand helper.
        // An unattached root already contains the world pose, even before the helper dirties its LTM.
        const RwMatrix* world = nativeHand >= 0 && !OrcGetRwFrameParentSafe(scope.frame)
            ? RwFrameGetMatrix(scope.frame)
            : RwFrameGetLTM(scope.frame);
        if (!world || !FiniteMatrix(*world))
            return false;
        scope.world = *world;
        scope.left = nativeHand >= 0 ? nativeHand != 0 : scope.held->dual && IsLeftHand(scope.ped, scope.world);
        scope.textureRestorePoint = OrcWeaponTextureRestorePoint();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("held weapon: draw setup SEH ex=0x%08X stock=%p", GetExceptionCode(), stock);
        return false;
    }
}

bool DrawReplacement(HeldDrawScope& scope, bool drawMesh = true) {
    HeldWeapon& held = *scope.held;
    HeldReplacement& replacement = scope.left ? held.left : held.right;
    if (!replacement.object || replacement.drawFailed ||
        !g_weaponReplacementEnabled || !g_weaponReplacementInHands)
        return false;
    RwFrame* frame = WeaponFrame(replacement.object);
    if (!frame)
        return false;
    RwMatrix matrix = scope.world;
    OrcApplyHeldWeaponPoseToMatrix(matrix, held.replacementPose[scope.left]);
    SetFrameMatrix(frame, matrix);

    const bool drew = OrcRenderWeaponObjectLikeVanillaWithTextures(scope.ped, held.weaponType,
        replacement.object, held.replacementTexture, true, g_weaponTexturesEnabled,
        scope.drawGunflash ? &replacement.flash : nullptr, scope.left, drawMesh);
    if (!drew) {
        replacement.drawFailed = drawMesh;
        OrcLogError("held weapon: replacement draw failed wt=%d key=%s left=%d",
            held.weaponType, held.replacementKey.c_str(), scope.left);
    } else if (drawMesh) {
        replacement.drawnFrame = g_heldFrame;
        if (g_heldWeaponTrace >= 1 && g_orcLogLevel >= OrcLogLevel::Info)
            OrcLogInfoThrottled(scope.left ? 538u : 37u, 2500u,
                "held weapon: replacement draw wt=%d key=%s left=%d stock=%p",
                held.weaponType, held.replacementKey.c_str(), scope.left, held.stock);
    }
    return drew;
}

void PrepareStockDraw(HeldDrawScope& scope, bool applyTextures = true) {
    if (scope.stockPrepared)
        return;
    scope.stockPrepared = true;
    HeldWeapon& held = *scope.held;
    const HeldWeaponPoseCfg& pose = held.stockPose[scope.left];
    scope.originalLocal = *RwFrameGetMatrix(scope.frame);
    RwMatrix adjusted = scope.originalLocal;
    if (OrcApplyHeldWeaponPoseToMatrix(adjusted, pose)) {
        scope.restoreFrame = true;
        SetFrameMatrix(scope.frame, adjusted);
    }
    if (applyTextures && g_weaponTexturesEnabled) {
        OrcApplyWeaponTexturesCombined(scope.ped, held.weaponType, held.stock, held.stockTexture, false);
    }
}

void RestoreStockDraw(HeldDrawScope& scope) {
    __try {
        __try {
            if (scope.restoreFrame)
                SetFrameMatrix(scope.frame, scope.originalLocal);
        } __finally {
            OrcRestoreWeaponTextureOverrides(scope.textureRestorePoint);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("held weapon: stock draw restore SEH ex=0x%08X", GetExceptionCode());
    }
    scope.restoreFrame = false;
    scope.stockPrepared = false;
}

RpAtomic* DrawStockAtomic(HeldDrawScope& scope, RpAtomic* atomic) {
    __try {
        if (scope.pass.TryReplace([&scope] { return DrawReplacement(scope); }))
            return atomic;
        PrepareStockDraw(scope);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        (scope.left ? scope.held->left : scope.held->right).drawFailed = true;
        OrcLogError("held weapon: draw extension SEH ex=0x%08X stock=%p",
            GetExceptionCode(), scope.held->stock);
        RestoreStockDraw(scope);
        scope.stockPrepared = true;
    }
    return g_defaultAtomicRender(atomic);
}

bool SkipHeldHooks() {
    return !g_enabled || OrcIsRuntimeShuttingDown() ||
        OrcSelectWeaponAtomicDispatch(OrcIsVanillaCustomWeaponDrawActive(), OrcIsBodyAttachmentDrawActive()) ==
            OrcWeaponAtomicDispatch::VanillaWeaponCallback;
}

RpClump* __cdecl RenderClump(RpClump* clump) {
    if (!clump || SkipHeldHooks() || !g_defaultAtomicRender)
        return g_clumpRender(clump);
    HeldDrawScope scope;
    if (!InitHeldDraw(scope, reinterpret_cast<RwObject*>(clump)))
        return g_clumpRender(clump);

    // The original visibility callback still decides whether to enter AtomicDefaultRenderCallBack.
    // A fresh scope per invocation handles twin hands, reflections and repeated draws while paused.
    HeldDrawScope* previous = g_activeClumpDraw;
    g_activeClumpDraw = &scope;
    RpClump* result = nullptr;
    __try {
        __try {
            if (!(scope.left ? scope.held->left.object : scope.held->right.object))
                PrepareStockDraw(scope);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("held weapon: stock preparation SEH ex=0x%08X stock=%p", GetExceptionCode(), clump);
            RestoreStockDraw(scope);
            scope.stockPrepared = true;
        }
        result = g_clumpRender(clump);
    } __finally {
        g_activeClumpDraw = previous;
        RestoreStockDraw(scope);
    }
    return result;
}

RpAtomic* __cdecl RenderDefaultAtomic(RpAtomic* atomic) {
    if (!atomic || (!g_activeClumpDraw && atomic->clump) || SkipHeldHooks())
        return g_defaultAtomicRender(atomic);
    if (g_activeClumpDraw && reinterpret_cast<RwObject*>(atomic->clump) == g_activeClumpDraw->held->stock)
        return DrawStockAtomic(*g_activeClumpDraw, atomic);

    // A stock slot can also own a standalone atomic. Never infer ownership from shared geometry.
    auto* object = reinterpret_cast<RwObject*>(atomic);
    if (atomic->clump || g_heldOwners.find(object) == g_heldOwners.end())
        return g_defaultAtomicRender(atomic);
    HeldDrawScope scope;
    if (!InitHeldDraw(scope, object))
        return g_defaultAtomicRender(atomic);
    RpAtomic* result = nullptr;
    __try {
        result = DrawStockAtomic(scope, atomic);
    } __finally {
        RestoreStockDraw(scope);
    }
    return result;
}

bool DrawSilentPatchFlash(HeldDrawScope& scope) {
    const HeldReplacement& replacement = scope.left ? scope.held->left : scope.held->right;
    if (replacement.flash.atomics.empty() ||
        (scope.left ? scope.ped->m_nWeaponGunflashAlphaMP2 : scope.ped->m_nWeaponGunflashAlphaMP1) <= 0)
        return true;
    void* zWrite = nullptr;
    if (!RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &zWrite))
        return false;
    bool drew = false;
    __try {
        __try {
            RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, nullptr);
            scope.drawGunflash = true;
            drew = DrawReplacement(scope, false);
        } __finally {
            RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, zWrite);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("held weapon: SilentPatch flash SEH ex=0x%08X", GetExceptionCode());
    }
    return drew;
}

int DrawSilentPatchStock(HeldDrawScope& scope, CPed** pedSlot, bool mesh, bool flash, bool shadow, int left) {
    __try {
        __try {
            PrepareStockDraw(scope, !shadow);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("held weapon: SilentPatch stock preparation SEH ex=0x%08X", GetExceptionCode());
            RestoreStockDraw(scope);
        }
        return g_silentPatchWeaponDraw(pedSlot, mesh, flash, shadow, left);
    } __finally {
        RestoreStockDraw(scope);
    }
}

int __fastcall RenderSilentPatchWeapon(CPed** pedSlot, void*, bool mesh, bool flash, bool shadow, int left) {
    CPed* ped = pedSlot ? *pedSlot : nullptr;
    if ((!mesh && !flash && !shadow) || !ped || !ped->m_pWeaponObject || SkipHeldHooks() || !g_defaultAtomicRender)
        return g_silentPatchWeaponDraw(pedSlot, mesh, flash, shadow, left);

    HeldDrawScope scope;
    if (!InitHeldDraw(scope, ped->m_pWeaponObject, left) || scope.ped != ped)
        return g_silentPatchWeaponDraw(pedSlot, mesh, flash, shadow, left);
    HeldReplacement& replacement = scope.left ? scope.held->left : scope.held->right;
    if (!replacement.object || replacement.drawFailed ||
        !g_weaponReplacementEnabled || !g_weaponReplacementInHands)
        return DrawSilentPatchStock(scope, pedSlot, mesh, flash, shadow, left);

    if (shadow) {
        // This helper also draws the ped's silhouette into the real-time shadow raster.
        // It only reads the slot during the verified shadow branch and uses native depth rendering.
        RwFrame* frame = WeaponFrame(replacement.object);
        if (!frame)
            return DrawSilentPatchStock(scope, pedSlot, mesh, flash, true, left);
        RwMatrix matrix = scope.world;
        OrcApplyHeldWeaponPoseToMatrix(matrix, scope.held->replacementPose[scope.left]);
        SetFrameMatrix(frame, matrix);
        RwObject* stock = ped->m_pWeaponObject;
        ped->m_pWeaponObject = replacement.object;
        __try {
            return g_silentPatchWeaponDraw(pedSlot, mesh, flash, true, left);
        } __finally {
            ped->m_pWeaponObject = stock;
        }
    }

    int result = 0;
    if (mesh) {
        replacement.drawnFrame = 0;
        scope.drawGunflash = false;
        HeldDrawScope* previous = g_activeClumpDraw;
        g_activeClumpDraw = &scope;
        __try {
            result = g_silentPatchWeaponDraw(pedSlot, true, false, false, left);
        } __finally {
            g_activeClumpDraw = previous;
            RestoreStockDraw(scope);
        }
    }
    if (flash && (replacement.drawnFrame != g_heldFrame || !DrawSilentPatchFlash(scope)))
        return DrawSilentPatchStock(scope, pedSlot, false, true, false, left);
    return result;
}

void __fastcall AddWeaponModel(CPed* ped, void*, int modelId) {
    g_addWeaponModel(ped, modelId);
    if (!OrcIsRuntimeShuttingDown()) {
        const int ref = OrcSafeGetPedRef(ped);
        if (ref > 0)
            RegisterStock(ped, ref);
    }
}

void __fastcall RemoveWeaponModel(CPed* ped, void*, int modelId) {
    RwObject* old = ped ? ped->m_pWeaponObject : nullptr;
    g_removeWeaponModel(ped, modelId);
    if (ped && old != ped->m_pWeaponObject)
        ForgetHeldWeapon(OrcSafeGetPedRef(ped));
}

template<class Fn>
bool InstallHeldHook(uintptr_t address, void* detour, Fn& original) {
    void* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
    if (status == MH_OK) {
        status = MH_EnableHook(target);
        if (status == MH_OK)
            return true;
        MH_RemoveHook(target);
    }
    original = nullptr;
    OrcLogError("held weapon: hook 0x%08X -> %s", address, MH_StatusToString(status));
    return false;
}

uintptr_t ReadRelativeTarget(uintptr_t address, unsigned char opcode) {
    if (*reinterpret_cast<const unsigned char*>(address) != opcode)
        return 0;
    std::int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(address + 1), sizeof(offset));
    return address + 5 + offset;
}

uintptr_t FindSilentPatchWeaponDraw() {
    constexpr uintptr_t weaponBatch = 0x732F30;
    struct Profile {
        uintptr_t batch;
        uintptr_t firstHandCall;
        uintptr_t secondHandCall;
        uintptr_t draw;
    };
    constexpr Profile profiles[] = {
        {0xAE30, 0x96A4, 0x9789, 0x9800}, // GTA San Andreas
        {0x9FD0, 0x8586, 0x865D, 0x86D0}, // GTA SAMP
        {0xBF80, 0x80BC, 0x81E4, 0x8250}, // Arizona
    };
    constexpr unsigned char prologue[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68};
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"SilentPatchSA.asi"));
    if (!base)
        return 0;
    __try {
        const uintptr_t batch = ReadRelativeTarget(weaponBatch, 0xE9);
        for (const Profile& profile : profiles) {
            const uintptr_t draw = base + profile.draw;
            if (batch == base + profile.batch &&
                ReadRelativeTarget(base + profile.firstHandCall, 0xE8) == draw &&
                ReadRelativeTarget(base + profile.secondHandCall, 0xE8) == draw &&
                std::memcmp(reinterpret_cast<const void*>(draw), prologue, sizeof(prologue)) == 0)
                return draw;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("held weapon: SilentPatch verification SEH ex=0x%08X", GetExceptionCode());
    }
    return 0;
}

} // namespace

int OrcResolveWeaponHeldVisualWeaponType(CPed* ped) {
    if (!ped || !ped->m_pWeaponObject)
        return 0;
    const int selected = SelectedWeaponType(ped);
    const int modelId = ped->m_nWeaponModelId;
    if (modelId <= 0 || WeaponTypeMatchesModel(selected, modelId))
        return selected;
    const int byModel = OrcWeaponsFindCachedTypeByModelId(modelId);
    if (byModel > 0) {
        for (const CWeapon& weapon : ped->m_aWeapons) {
            const int wt = static_cast<int>(weapon.m_eWeaponType);
            if (WeaponTypeMatchesModel(wt, modelId))
                return wt;
        }
        return byModel;
    }
    return selected;
}

void OrcWeaponSuppressBodyForHeldVisualWeapon(CPed* ped, std::vector<char>* suppress) {
    if (suppress)
        OrcMarkBodyWeaponTypeSuppressed(suppress, OrcResolveWeaponHeldVisualWeaponType(ped));
}

bool OrcPedWantsDualWieldHeld(CPed* ped, int wt) {
    return ped && OrcResolveWeaponHeldVisualWeaponType(ped) == wt && IsNativeDualWield(ped, wt);
}

bool OrcCanModifyHeldWeapon(CPed* ped) {
    return CanModifyHeldWeapon(ped);
}

bool OrcBuildHeldWeaponHandMatrix(CPed* ped, bool left, RwMatrix& matrix) {
    const RwMatrix* bone = OrcGetBoneMatrix(ped, left ? BONE_L_HAND : BONE_R_HAND);
    if (!bone || !FiniteMatrix(*bone))
        return false;
    matrix = *bone;
    if (left) {
        RwV3d axis{1.0f, 0.0f, 0.0f};
        RwV3d offset{0.04f, -0.05f, 0.0f};
        RwMatrixRotate(&matrix, &axis, 180.0f, rwCOMBINEPRECONCAT);
        RwMatrixTranslate(&matrix, &offset, rwCOMBINEPRECONCAT);
    }
    return true;
}

bool OrcApplyHeldWeaponPoseToMatrix(RwMatrix& matrix, const HeldWeaponPoseCfg& pose) {
    if (!pose.enabled || !FiniteMatrix(matrix))
        return false;
    RwMatrix work = matrix;
    auto normalize = [](RwV3d& axis) {
        const float length2 = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
        if (length2 < 1e-16f || !std::isfinite(length2))
            return false;
        const float inverseLength = 1.0f / std::sqrt(length2);
        axis.x *= inverseLength;
        axis.y *= inverseLength;
        axis.z *= inverseLength;
        return true;
    };
    if (!normalize(work.right))
        return false;
    RwV3dCrossProduct(&work.at, &work.right, &work.up);
    if (!normalize(work.at))
        return false;
    RwV3dCrossProduct(&work.up, &work.at, &work.right);
    RwMatrixUpdate(&work);
    OrcApplyAttachmentOffset(&work, pose.x, pose.y, pose.z);
    OrcRotateAttachmentMatrix(&work, pose.rx, pose.ry, pose.rz);
    if (pose.scale > 0.0f && pose.scale != 1.0f) {
        RwV3d scale{pose.scale, pose.scale, pose.scale};
        RwMatrixScale(&work, &scale, rwCOMBINEPRECONCAT);
    }
    if (!FiniteMatrix(work))
        return false;
    matrix = work;
    return true;
}

bool OrcTryResolveHeldReplacementGunflash(CPed* ped, int wt, bool left,
    RwObject*& outClone, const OrcHeldGunflash*& outGunflash) {
    outClone = nullptr;
    outGunflash = nullptr;
    if (!g_weaponReplacementEnabled || !g_weaponReplacementInHands || !CanModifyHeldWeapon(ped))
        return false;
    const auto it = g_heldWeapons.find(OrcSafeGetPedRef(ped));
    if (it == g_heldWeapons.end() || it->second.stock != ped->m_pWeaponObject || it->second.weaponType != wt)
        return false;
    const HeldReplacement& replacement = left ? it->second.left : it->second.right;
    if (!replacement.object || replacement.drawFailed)
        return false;
    outClone = replacement.object;
    outGunflash = &replacement.flash;
    return true;
}

bool OrcGetHeldReplacementKeyForPed(CPed* ped, int wt, std::string& outKey) {
    outKey.clear();
    if (!g_weaponReplacementEnabled || !g_weaponReplacementInHands || !CanModifyHeldWeapon(ped))
        return false;
    const auto it = g_heldWeapons.find(OrcSafeGetPedRef(ped));
    if (it == g_heldWeapons.end() || it->second.stock != ped->m_pWeaponObject ||
        it->second.weaponType != wt || !it->second.right.object || it->second.right.drawFailed)
        return false;
    outKey = it->second.replacementKey;
    return true;
}

void OrcPrepareHeldWeaponBeforeRender(CPed* ped) {
    if (!ped || !g_enabled)
        return;
    const int ref = OrcSafeGetPedRef(ped);
    if (ref <= 0)
        return;
    HeldWeapon* held = RegisterStock(ped, ref);
    if (held)
        PrepareHeldWeapon(ped, *held);
}

void OrcHeldPoseBeginSimFrame() {
    ++g_heldFrame;
    // SilentPatch can finish installing its render hooks after RenderWare initialization.
    if (g_heldFrame <= 3 && !g_silentPatchWeaponDraw && g_defaultAtomicRender) {
        if (const uintptr_t draw = FindSilentPatchWeaponDraw()) {
            if (InstallHeldHook(draw, reinterpret_cast<void*>(&RenderSilentPatchWeapon), g_silentPatchWeaponDraw))
                OrcLogInfo("held weapon: SilentPatch draw adapter at %p", reinterpret_cast<void*>(draw));
        }
    }
}

void OrcInvalidateHeldWeaponDrawPreparation() {
    for (auto& entry : g_heldWeapons) {
        entry.second.prepared = 0;
        entry.second.stockTexture = nullptr;
        entry.second.replacementTexture = nullptr;
    }
}

void OrcPruneHeldWeaponReplacementInstances() {
    for (auto it = g_heldWeapons.begin(); it != g_heldWeapons.end();) {
        const int ref = it->first;
        CPed* ped = OrcSafeGetPed(ref);
        HeldWeapon& held = it->second;
        ++it;
        if (!ped || ped->m_pWeaponObject != held.stock)
            ForgetHeldWeapon(ref);
        else if (!held.replacementKey.empty() &&
            (!g_weaponReplacementEnabled || !g_weaponReplacementInHands || !CanModifyHeldWeapon(ped))) {
            ClearReplacement(held);
            held.prepared = 0;
        }
    }
}

void OrcDestroyAllHeldWeaponReplacementInstances() {
    for (auto& entry : g_heldWeapons)
        ClearReplacement(entry.second);
    g_heldWeapons.clear();
    g_heldOwners.clear();
}

void OrcHeldWeaponTraceGameProcessTick() {
    const int intervalMs = g_heldWeaponStatusIntervalMs;
    if (g_heldWeaponTrace < 2 || g_orcLogLevel < OrcLogLevel::Info || intervalMs <= 0)
        return;
    static bool hasLogged = false;
    static unsigned lastStatusMs = 0;
    const unsigned now = CTimer::m_snTimeInMilliseconds;
    if (hasLogged && now - lastStatusMs < static_cast<unsigned>(intervalMs))
        return;

    CPed* ped = FindPlayerPed(0);
    if (!ped)
        return;
    hasLogged = true;
    lastStatusMs = now;
    const auto it = g_heldWeapons.find(OrcSafeGetPedRef(ped));
    const HeldWeapon* held = it != g_heldWeapons.end() ? &it->second : nullptr;
    OrcLogInfo(
        "held status: wt=%d slot=%d model=%d stock=%p key=%s right=%p left=%p",
        OrcResolveWeaponHeldVisualWeaponType(ped), ped->m_nSelectedWepSlot, ped->m_nWeaponModelId,
        ped->m_pWeaponObject, held ? held->replacementKey.c_str() : "",
        held ? held->right.object : nullptr, held ? held->left.object : nullptr);
}

void OrcWeaponEnsureHeldHooksInstalled() {
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("held weapon: MH_Initialize -> %s", MH_StatusToString(status));
        return;
    }
    const bool atomic = InstallHeldHook(kDefaultAtomicRender, reinterpret_cast<void*>(&RenderDefaultAtomic),
        g_defaultAtomicRender);
    const bool clump = atomic && InstallHeldHook(kClumpRender, reinterpret_cast<void*>(&RenderClump), g_clumpRender);
    InstallHeldHook(kAddWeaponModel, reinterpret_cast<void*>(&AddWeaponModel), g_addWeaponModel);
    InstallHeldHook(kRemoveWeaponModel, reinterpret_cast<void*>(&RemoveWeaponModel), g_removeWeaponModel);
    OrcLogInfo("held weapon hooks: stock-instance draw scope clump=%d atomic=%d", clump, atomic);
}
