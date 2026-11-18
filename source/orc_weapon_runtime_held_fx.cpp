#include "plugin.h"

#include "CClumpModelInfo.h"
#include "CPed.h"
#include "CVisibilityPlugins.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "Fx_c.h"

#include <cmath>
#include <cstring>
#include <intrin.h>
#include <windows.h>

#include "external/MinHook/include/MinHook.h"
#include "orc_app.h"
#include "orc_log.h"
#include "orc_render.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_gunflash_state.h"
#include "orc_weapon_metadata_cache.h"
#include "orc_weapon_runtime.h"
#include "samp_bridge.h"

namespace {
constexpr uintptr_t kDoGunFlash = 0x5DF340;
constexpr uintptr_t kFireInstantHit = 0x73FB10;
constexpr uintptr_t kFireHeldGun = 0x61EB10;
constexpr uintptr_t kFireHeldRightCall = 0x624F0D;
constexpr uintptr_t kFireHeldLeftCall = 0x624F1D;

// Only the three visual calls in GTA SA 1.0 US FireInstantHit are redirected.
// The same muzzle argument also participates in hit detection inside the original function.
constexpr uintptr_t kAddLightCall = 0x740D68;
constexpr uintptr_t kTriggerGunshotCall = 0x740DA7;
constexpr uintptr_t kAddGunshellCall = 0x740E35;
constexpr uintptr_t kAddLight = 0x7000E0;
constexpr uintptr_t kTriggerGunshot = 0x4A0DE0;
constexpr uintptr_t kAddGunshell = 0x73A3E0;

constexpr uintptr_t kGunflashProgressMax = 0x8D1370;
constexpr uintptr_t kGunflashAlphaMax = 0x8D21DC;
constexpr uintptr_t kGunflashDuration = 0x8D2E90;

using DoGunFlashFn = bool(__thiscall*)(CPed*, int, bool);
using FireInstantHitFn = bool(__thiscall*)(
    CWeapon*, CEntity*, CVector*, CVector*, CEntity*, CVector*, CVector*, bool, bool);
using FireHeldGunFn = unsigned char(__thiscall*)(void*, CPed*, bool);
using AddLightFn = void(__cdecl*)(
    unsigned char, RwV3d, RwV3d, float, float, float, float, unsigned char, bool, CEntity*);

DoGunFlashFn g_doGunFlashOriginal = nullptr;
FireInstantHitFn g_fireInstantHitOriginal = nullptr;
FireHeldGunFn g_fireHeldGunOriginal = nullptr;

struct FiringHand {
    CPed* ped;
    bool left;
};
const FiringHand* g_firingHand = nullptr;

struct ShotFx {
    CPed* ped = nullptr;
    int weaponType = 0;
    RwV3d delta{};
    bool hasReplacementFlashMesh = false;
    bool sampDirect = false;
};
ShotFx* g_shotFx = nullptr;
const std::string g_stockPoseKey;

bool IsFinitePoint(const RwV3d& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool IsFiniteTransform(const RwMatrix& matrix) {
    return IsFinitePoint(matrix.right) && IsFinitePoint(matrix.up) &&
        IsFinitePoint(matrix.at) && IsFinitePoint(matrix.pos);
}

RwV3d TransformPoint(const RwV3d& point, const RwMatrix& matrix) {
    RwV3d result{};
    RwV3dTransformPoints(&result, &point, 1, &matrix);
    return result;
}

float DistanceSquared(const RwV3d& a, const RwV3d& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool IsLeftHandShot(CPed* ped, int wt, const RwV3d& muzzle, const RwV3d& fireOffset) {
    if (g_firingHand && g_firingHand->ped == ped)
        return g_firingHand->left;
    if (!ped->bCalledPreRender)
        return false;
    if (!OrcPedWantsDualWieldHeld(ped, wt))
        return false;
    const RwMatrix* left = OrcGetBoneMatrix(ped, BONE_L_HAND);
    const RwMatrix* right = OrcGetBoneMatrix(ped, BONE_R_HAND);
    if (!left || !right)
        return false;

    const RwV3d leftMuzzle = TransformPoint(fireOffset, *left);
    const RwV3d rightMuzzle = TransformPoint(fireOffset, *right);
    return DistanceSquared(muzzle, leftMuzzle) < DistanceSquared(muzzle, rightMuzzle);
}

bool IsDirectSampShot(const void* returnAddress) {
    // All inspected SA:MP wrappers call the GTA entry via mov edx, 73FB10h; call edx.
    constexpr unsigned char call[] = {0xBA, 0x10, 0xFB, 0x73, 0x00, 0xFF, 0xD2};
    static const void* verified = nullptr;
    if (returnAddress == verified)
        return verified != nullptr;
    const auto* instruction = static_cast<const unsigned char*>(returnAddress) - sizeof(call);
    if (std::memcmp(instruction, call, sizeof(call)) != 0 || !samp_bridge::IsSampBuildKnown())
        return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(returnAddress), &owner) || owner != GetModuleHandleA("samp.dll"))
        return false;
    verified = returnAddress;
    return true;
}

const CWeaponInfo* WeaponInfoForShot(CPed* ped, int wt) {
    if (!OrcWeaponTypeCanUseVanillaInfoContract(wt, static_cast<int>(WEAPONTYPE_PARACHUTE)))
        return nullptr;
    const auto type = static_cast<eWeaponType>(wt);
    return CWeaponInfo::GetWeaponInfo(type, ped->GetWeaponSkill(type));
}

void ResolveShotFx(ShotFx& shot, const RwV3d& muzzle) {
    CPed* ped = shot.ped;
    const CWeaponInfo* info = WeaponInfoForShot(ped, shot.weaponType);
    if (!IsFinitePoint(muzzle) || !info)
        return;

    const RwV3d& fireOffset = info->m_vecFireOffset;
    const bool left = !shot.sampDirect && IsLeftHandShot(ped, shot.weaponType, muzzle, fireOffset);
    const OrcHeldGunflash* gunflash = nullptr;
    RwObject* clone = nullptr;
    const bool hasReplacement = OrcTryResolveHeldReplacementGunflash(ped, shot.weaponType, left, clone, gunflash);
    shot.hasReplacementFlashMesh = gunflash && !gunflash->atomics.empty();
    if (!ped->bCalledPreRender)
        return;
    const RwMatrix* firingBone = OrcGetBoneMatrix(ped, left ? BONE_L_HAND : BONE_R_HAND);
    if (!firingBone || !OrcCanRelocateGunshotFx(true,
        DistanceSquared(muzzle, TransformPoint(fireOffset, *firingBone))))
        return;

    const HeldWeaponPoseCfg& pose = GetHeldPoseForPed(ped, shot.weaponType,
        left && g_considerWeaponSkills, hasReplacement ? nullptr : &g_stockPoseKey);
    const bool hasMuzzleLocator = gunflash && gunflash->hasMuzzleOffset;
    if (!hasMuzzleLocator && !pose.enabled)
        return;

    RwMatrix basis{};
    if (!OrcBuildHeldWeaponHandMatrix(ped, left, basis))
        return;
    RwMatrix held = basis;
    if (pose.enabled && !OrcApplyHeldWeaponPoseToMatrix(held, pose) && !hasMuzzleLocator)
        return;

    RwV3d localMuzzle{};
    if (hasMuzzleLocator) {
        localMuzzle = gunflash->muzzleOffset;
    } else {
        RwMatrix inverse{};
        RwMatrixInvert(&inverse, &basis);
        localMuzzle = TransformPoint(muzzle, inverse);
    }
    const RwV3d adjusted = TransformPoint(localMuzzle, held);
    if (!IsFinitePoint(adjusted))
        return;
    shot.delta = { adjusted.x - muzzle.x, adjusted.y - muzzle.y, adjusted.z - muzzle.z };
    if (g_orcLogLevel >= OrcLogLevel::Info && g_heldWeaponTrace >= 2) {
        OrcLogInfoThrottled(left ? 702u : 701u, 1000u,
            "weapon fx: ped=%p wt=%d clone=%p hand=%s source=%s delta=(%.3f,%.3f,%.3f)",
            ped, shot.weaponType, clone, left ? "left" : "right", hasMuzzleLocator ? "muzzle" : "held",
            shot.delta.x, shot.delta.y, shot.delta.z);
    }
}

RwV3d OffsetPoint(const RwV3d& point, const RwV3d& delta) {
    return { point.x + delta.x, point.y + delta.y, point.z + delta.z };
}

void __cdecl AddShotLight(unsigned char type, RwV3d point, RwV3d direction, float radius,
    float red, float green, float blue, unsigned char fogType, bool shadows, CEntity* affected) {
    if (ShotFx* shot = g_shotFx) {
        __try {
            ResolveShotFx(*shot, point);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shot->delta = {};
            shot->hasReplacementFlashMesh = false;
            static bool logged = false;
            if (!logged) {
                logged = true;
                OrcLogError("weapon fx: muzzle calculation SEH ex=0x%08X", GetExceptionCode());
            }
        }
        point = OffsetPoint(point, shot->delta);
    }
    reinterpret_cast<AddLightFn>(kAddLight)(
        type, point, direction, radius, red, green, blue, fogType, shadows, affected);
}

void __fastcall TriggerShotParticles(
    Fx_c* fx, void*, CEntity* creator, CVector& point, CVector& direction, bool particleFlash) {
    if (const ShotFx* shot = g_shotFx; shot && shot->ped == creator) {
        CVector adjusted(OffsetPoint(point, shot->delta));
        bool meshFlash = shot->hasReplacementFlashMesh;
        if (meshFlash && shot->sampDirect) {
            // The network wrapper supplies BONE_R_HAND and returns without calling DoGunFlash.
            const auto duration = *reinterpret_cast<const unsigned short*>(kGunflashDuration);
            meshFlash = duration > 0 && shot->ped->DoGunFlash(duration, false);
        }
        particleFlash = particleFlash && !meshFlash;
        fx->TriggerGunshot(creator, adjusted, direction, particleFlash);
        return;
    }
    fx->TriggerGunshot(creator, point, direction, particleFlash);
}

void __fastcall AddShotShell(CWeapon* weapon, void*, CEntity* creator,
    const CVector& point, const CVector2D& direction, float size) {
    if (const ShotFx* shot = g_shotFx; shot && shot->ped == creator) {
        const CVector adjusted(OffsetPoint(point, shot->delta));
        weapon->AddGunshell(creator, adjusted, direction, size);
        return;
    }
    weapon->AddGunshell(creator, point, direction, size);
}

bool __fastcall FireInstantHitDetour(CWeapon* self, void*, CEntity* firingEntity, CVector* origin,
    CVector* muzzlePosn, CEntity* targetEntity, CVector* target, CVector* originForDriveBy, bool arg6, bool muzzle) {
    ShotFx shot{};
    if (muzzle && !originForDriveBy && firingEntity && firingEntity->m_nType == ENTITY_TYPE_PED &&
        !OrcIsRuntimeShuttingDown() && OrcCanModifyHeldWeapon(static_cast<CPed*>(firingEntity))) {
        shot.ped = static_cast<CPed*>(firingEntity);
        shot.weaponType = static_cast<int>(self->m_eWeaponType);
        shot.sampDirect = (!g_firingHand || g_firingHand->ped != shot.ped) && IsDirectSampShot(_ReturnAddress());
    }
    ShotFx* const previous = g_shotFx;
    g_shotFx = shot.ped ? &shot : nullptr;
    __try {
        return g_fireInstantHitOriginal(
            self, firingEntity, origin, muzzlePosn, targetEntity, target, originForDriveBy, arg6, muzzle);
    } __finally {
        g_shotFx = previous;
    }
}

unsigned char __fastcall FireHeldGunDetour(void* task, void*, CPed* ped, bool left) {
    const FiringHand hand{ped, left};
    const FiringHand* const previous = g_firingHand;
    g_firingHand = &hand;
    __try {
        return g_fireHeldGunOriginal(task, ped, left);
    } __finally {
        g_firingHand = previous;
    }
}

bool __fastcall DoGunFlashDetour(CPed* ped, void*, int duration, bool left) {
    if (g_enabled && ped && duration > 0 && !OrcIsRuntimeShuttingDown()) {
        const int wt = OrcResolveWeaponHeldVisualWeaponType(ped);
        RwObject* clone = nullptr;
        const OrcHeldGunflash* gunflash = nullptr;
        if (OrcTryResolveHeldReplacementGunflash(ped, wt, left, clone, gunflash) &&
            clone && gunflash && gunflash->frame && !gunflash->atomics.empty()) {
            RwObject* const stock = ped->m_pWeaponObject;
            RwFrame* const stockGunflash = ped->m_pGunflashObject;
            ped->m_pWeaponObject = clone;
            ped->m_pGunflashObject = gunflash->frame;
            __try {
                const bool started = g_doGunFlashOriginal(ped, duration, left);
                if (started)
                    RwFrameUpdateObjects(gunflash->frame);
                return started;
            } __finally {
                ped->m_pWeaponObject = stock;
                ped->m_pGunflashObject = stockGunflash;
            }
        }
    }
    return g_doGunFlashOriginal(ped, duration, left);
}

bool IsFrameUnder(RwFrame* frame, RwFrame* ancestor) {
    for (int depth = 0; frame && depth < 64; ++depth, frame = OrcGetRwFrameParentSafe(frame)) {
        if (frame == ancestor)
            return true;
    }
    return false;
}

bool FrameRelativeTo(RwFrame* frame, RwFrame* root, RwMatrix& matrix) {
    RwMatrixSetIdentity(&matrix);
    for (int depth = 0; frame && frame != root && depth < 64; ++depth) {
        RwMatrix next{};
        RwMatrixMultiply(&next, &matrix, RwFrameGetMatrix(frame));
        matrix = next;
        frame = OrcGetRwFrameParentSafe(frame);
    }
    return frame == root && IsFiniteTransform(matrix);
}

struct MuzzleSample {
    RwFrame* root;
    RwFrame* flash;
    OrcGunflashMuzzleTip<RwV3d> tip;
};

RpAtomic* SampleMuzzleAtomic(RpAtomic* atomic, void* data) {
    auto& sample = *static_cast<MuzzleSample*>(data);
    RwFrame* frame = RpAtomicGetFrame(atomic);
    const RpGeometry* geometry = RpAtomicGetGeometry(atomic);
    if (!frame || !geometry || !(RpAtomicGetFlags(atomic) & rpATOMICRENDER) ||
        (sample.flash && IsFrameUnder(frame, sample.flash)) ||
        (geometry->flags & rpGEOMETRYNATIVE) || geometry->numVertices <= 0 || geometry->numVertices > 65536 ||
        geometry->numMorphTargets < 1 || !geometry->morphTarget)
        return atomic;
    const RwV3d* vertices = RpMorphTargetGetVertices(geometry->morphTarget);
    if (!vertices)
        return atomic;
    RwMatrix relative{};
    if (frame != sample.root && !FrameRelativeTo(frame, sample.root, relative))
        return atomic;
    for (int i = 0; i < geometry->numVertices; ++i) {
        const RwV3d& v = vertices[i];
        if (frame == sample.root) {
            sample.tip.Add(v);
        } else {
            sample.tip.Add({
                relative.pos.x + v.x * relative.right.x + v.y * relative.up.x + v.z * relative.at.x,
                relative.pos.y + v.x * relative.right.y + v.y * relative.up.y + v.z * relative.at.y,
                relative.pos.z + v.x * relative.right.z + v.y * relative.up.z + v.z * relative.at.z,
            });
        }
    }
    return atomic;
}

bool SampleMuzzle(RpClump* clump, RwFrame* flash, const RwV3d& reference, float& x) {
    MuzzleSample sample{RpClumpGetFrame(clump), flash, OrcGunflashMuzzleTip<RwV3d>(reference)};
    RpClumpForAllAtomics(clump, SampleMuzzleAtomic, &sample);
    if (!sample.tip.Found())
        return false;
    x = sample.tip.Front();
    return true;
}

bool IsFlashMesh(const RpGeometry* geometry) {
    return geometry && geometry->numTriangles > 0 &&
        geometry->matList.materials && geometry->matList.numMaterials > 0;
}

RpAtomic* PrepareGunflashAtomic(RpAtomic* atomic, void* data) {
    auto& gunflash = *static_cast<OrcHeldGunflash*>(data);
    if (!atomic || !IsFrameUnder(RpAtomicGetFrame(atomic), gunflash.frame))
        return atomic;

    RpAtomicSetFlags(atomic, OrcInitialGunflashAtomicFlags(RpAtomicGetFlags(atomic)));
    RpGeometry* geometry = RpAtomicGetGeometry(atomic);
    if (IsFlashMesh(geometry)) {
        gunflash.atomics.push_back(atomic);
        const auto materialCount = static_cast<std::size_t>(geometry->matList.numMaterials);
        if (materialCount > gunflash.alphaScratch.size())
            gunflash.alphaScratch.resize(materialCount);
    }
    return atomic;
}

bool InheritStockGunflash(CPed* ped, RpClump* clump, const RwV3d& reference, OrcHeldGunflash& gunflash) {
    RwObject* stock = ped->m_pWeaponObject;
    RwFrame* stockFlash = ped->m_pGunflashObject;
    if (!stock || stock->type != rpCLUMP || !stockFlash)
        return true;
    auto* stockClump = reinterpret_cast<RpClump*>(stock);
    RwFrame* stockRoot = RpClumpGetFrame(stockClump);
    RwMatrix matrix{};
    if (!stockRoot || stockFlash == stockRoot || !FrameRelativeTo(stockFlash, stockRoot, matrix))
        return true;

    // The game's SetGunFlashAlpha addresses the first object attached directly to this frame.
    struct NativeFlash {
        RwFrame* frame;
        RpAtomic* atomic = nullptr;
    } source{stockFlash};
    RpClumpForAllAtomics(stockClump, [](RpAtomic* atomic, void* data) -> RpAtomic* {
        auto& source = *static_cast<NativeFlash*>(data);
        if (!source.atomic && RpAtomicGetFrame(atomic) == source.frame && IsFlashMesh(RpAtomicGetGeometry(atomic)))
            source.atomic = atomic;
        return atomic;
    }, &source);
    if (!source.atomic)
        return true;

    if (gunflash.hasMuzzleOffset) {
        float nativeX = reference.x;
        SampleMuzzle(stockClump, stockFlash, reference, nativeX);
        matrix.pos.x += gunflash.muzzleOffset.x - nativeX;
        matrix.pos.y += gunflash.muzzleOffset.y - reference.y;
        matrix.pos.z += gunflash.muzzleOffset.z - reference.z;
    }
    RwMatrixUpdate(&matrix);

    RpAtomic* copy = RpAtomicClone(source.atomic);
    if (!copy)
        return false;
    RwFrame* frame = RwFrameCreate();
    if (!frame) {
        RpAtomicDestroy(copy);
        return false;
    }
    *RwFrameGetMatrix(frame) = matrix;
    if (!RwFrameAddChild(RpClumpGetFrame(clump), frame)) {
        RwFrameDestroy(frame);
        RpAtomicDestroy(copy);
        return false;
    }
    CVisibilityPlugins::SetAtomicRenderCallback(copy, nullptr);
    RpAtomicSetFrame(copy, frame);
    if (!RpClumpAddAtomic(clump, copy)) {
        RpAtomicDestroy(copy);
        RwFrameRemoveChild(frame);
        RwFrameDestroy(frame);
        return false;
    }

    gunflash.frame = frame;
    PrepareGunflashAtomic(copy, &gunflash);
    OrcLogInfoThrottled(940u, 4000u,
        "weapon fx: inherited stock flash ped=%p clone=%p mesh=%p muzzleX=%.3f",
        ped, clump, copy, gunflash.hasMuzzleOffset ? gunflash.muzzleOffset.x : reference.x);
    return true;
}

void DrawGunflashAtomic(RpAtomic* atomic, RwUInt8 alpha, std::vector<RwUInt8>& scratch) {
    RpGeometry* geometry = RpAtomicGetGeometry(atomic);
    if (!geometry)
        return;
    const int count = geometry->matList.numMaterials;
    RpMaterial** materials = geometry->matList.materials;
    if (!materials || count <= 0 || static_cast<std::size_t>(count) > scratch.size())
        return;

    // A geometry may share materials with another atomic or have duplicate material slots.
    // Capture the complete original state before changing the first slot.
    for (int i = 0; i < count; ++i) {
        if (materials[i])
            scratch[i] = materials[i]->color.alpha;
    }

    const auto oldGeometryFlags = RpGeometryGetFlags(geometry);
    __try {
        RpGeometrySetFlags(geometry, oldGeometryFlags | rpGEOMETRYMODULATEMATERIALCOLOR);
        for (int i = 0; i < count; ++i) {
            if (materials[i])
                materials[i]->color.alpha = alpha;
        }
        // The ordinary clump pass skips all flash atomics. Direct draw uses the same native callback.
        RpAtomicRender(atomic);
    } __finally {
        for (int i = 0; i < count; ++i) {
            if (materials[i])
                materials[i]->color.alpha = scratch[i];
        }
        RpGeometrySetFlags(geometry, oldGeometryFlags);
    }
}

bool IsOriginalCall(uintptr_t site, uintptr_t target) {
    return plugin::patch::GetUChar(site) == 0xE8 &&
        site + 5 + plugin::patch::GetInt(site + 1) == target;
}

bool InstallHook(uintptr_t address, void* detour, void** original, const char* name) {
    auto* target = reinterpret_cast<void*>(address);
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK) {
        OrcLogError("weapon fx: %s MH_CreateHook -> %s", name, MH_StatusToString(created));
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        MH_RemoveHook(target);
        *original = nullptr;
        OrcLogError("weapon fx: %s MH_EnableHook -> %s", name, MH_StatusToString(enabled));
        return false;
    }
    return true;
}
} // namespace

bool OrcHeldPrepareGunflash(RwObject* object, CPed* ped, int wt,
    WeaponReplacementAsset& asset, OrcHeldGunflash& gunflash) {
    gunflash = {};
    if (!ped || !object || object->type != rpCLUMP)
        return false;

    auto* clump = reinterpret_cast<RpClump*>(object);
    RwFrame* const root = RpClumpGetFrame(clump);
    if (!root)
        return false;

    RwFrame* frame = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
    RwMatrix authored{};
    if (frame && frame != root && FrameRelativeTo(frame, root, authored)) {
        gunflash.frame = frame;
        RpClumpForAllAtomics(clump, PrepareGunflashAtomic, &gunflash);
    }

    const CWeaponInfo* info = WeaponInfoForShot(ped, wt);
    if (info && IsFinitePoint(info->m_vecFireOffset) &&
        (info->m_nWeaponFire == WEAPON_FIRE_INSTANT_HIT || ped->m_pGunflashObject)) {
        const RwV3d& reference = info->m_vecFireOffset;
        if (!asset.muzzleSampled) {
            asset.muzzleSampled = true;
            asset.muzzleFound = SampleMuzzle(clump, gunflash.frame, reference, asset.muzzleX);
        }
        if (asset.muzzleFound) {
            gunflash.muzzleOffset = {asset.muzzleX, reference.y, reference.z};
            gunflash.hasMuzzleOffset = true;
        }
        // Empty authored dummies can mark the muzzle. Dummies with geometry are usually its pivot.
        if (gunflash.frame && gunflash.atomics.empty() &&
            (!gunflash.hasMuzzleOffset || DistanceSquared(authored.pos, gunflash.muzzleOffset) <= 0.01f)) {
            gunflash.muzzleOffset = authored.pos;
            gunflash.hasMuzzleOffset = true;
        }
        if (gunflash.atomics.empty() && !InheritStockGunflash(ped, clump, reference, gunflash))
            return false;
    }
    if (g_orcLogLevel >= OrcLogLevel::Info) {
        OrcLogInfoThrottled(939u, 4000u,
            "weapon fx: prepared ped=%p key=%s clone=%p flashFrame=%p atomics=%zu",
            ped, asset.key.c_str(), object, gunflash.frame, gunflash.atomics.size());
    }
    return true;
}

RwUInt8 OrcHeldGunflashAlpha(const CPed* ped, const OrcHeldGunflash* gunflash, bool left) {
    if (!ped || !gunflash || gunflash->atomics.empty())
        return 0;
    const int progress = left ? ped->m_nWeaponGunflashAlphaMP2 : ped->m_nWeaponGunflashAlphaMP1;
    if (progress <= 0)
        return 0;

    const auto maxProgress = *reinterpret_cast<const unsigned short*>(kGunflashProgressMax);
    const auto maxAlpha = *reinterpret_cast<const int*>(kGunflashAlphaMax);
    return OrcGunflashAlphaForProgress(progress, maxProgress, maxAlpha);
}

void OrcHeldDrawGunflash(OrcHeldGunflash& gunflash, RwUInt8 alpha) {
    RwFrameGetLTM(gunflash.frame);
    for (RpAtomic* atomic : gunflash.atomics)
        DrawGunflashAtomic(atomic, alpha, gunflash.alphaScratch);
}

void OrcWeaponEnsureFireFxHooksInstalled() {
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;

    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("weapon fx: MH_Initialize -> %s", MH_StatusToString(initialized));
        return;
    }
    if (InstallHook(kDoGunFlash, reinterpret_cast<void*>(&DoGunFlashDetour),
            reinterpret_cast<void**>(&g_doGunFlashOriginal), "CPed::DoGunFlash")) {
        OrcLogInfo("weapon fx: installed scoped CPed::DoGunFlash hook");
    }

    if (!IsOriginalCall(kAddLightCall, kAddLight) || !IsOriginalCall(kTriggerGunshotCall, kTriggerGunshot) ||
        !IsOriginalCall(kAddGunshellCall, kAddGunshell)) {
        OrcLogError("weapon fx: muzzle effect calls changed; visual redirects disabled");
        return;
    }
    if (!InstallHook(kFireInstantHit, reinterpret_cast<void*>(&FireInstantHitDetour),
            reinterpret_cast<void**>(&g_fireInstantHitOriginal), "CWeapon::FireInstantHit"))
        return;

    if (IsOriginalCall(kFireHeldRightCall, kFireHeldGun) && IsOriginalCall(kFireHeldLeftCall, kFireHeldGun) &&
        InstallHook(kFireHeldGun, reinterpret_cast<void*>(&FireHeldGunDetour),
            reinterpret_cast<void**>(&g_fireHeldGunOriginal), "CTaskSimpleUseGun::FireGun"))
        OrcLogInfo("weapon fx: installed native firing-hand scope");

    plugin::patch::RedirectCall(kAddLightCall, reinterpret_cast<void*>(&AddShotLight));
    plugin::patch::RedirectCall(kTriggerGunshotCall, reinterpret_cast<void*>(&TriggerShotParticles));
    plugin::patch::RedirectCall(kAddGunshellCall, reinterpret_cast<void*>(&AddShotShell));
    OrcLogInfo("weapon fx: installed visual muzzle redirects (light, particles, shell)");
}
