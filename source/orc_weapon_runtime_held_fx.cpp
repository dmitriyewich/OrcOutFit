#include "plugin.h"

#include "CPed.h"
#include "CPools.h"
#include "CWeaponInfo.h"
#include "CEntity.h"
#include "CWeapon.h"
#include "CVector.h"
#include "CClumpModelInfo.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include <cmath>
#include <cstring>

#include <windows.h>

#include "external/MinHook/include/MinHook.h"

#include "orc_app.h"
#include "orc_render.h"
#include "orc_log.h"
#include "orc_types.h"
#include "orc_weapon_runtime.h"

using namespace plugin;

/// GTA SA 1.0 US — точки спавна FX выстрела (`muzzlePosn`); `origin` пули не трогаем.
static constexpr uintptr_t kAddr_CWeapon_Fire = 0x742300;
static constexpr uintptr_t kAddr_CWeapon_FireInstantHit = 0x73FB10;

static bool g_weaponFireFxHooksInstalled = false;

void OrcPedSyncGunflashFrameFromCurrentWeaponObject(CPed* ped) {
    if (!ped)
        return;
    RwObject* const woSlot = ped->m_pWeaponObject;
    RwFrame* const prevGf = ped->m_pGunflashObject;
    RpClump* clump = OrcPedResolveGunflashTargetClump(ped);
    if (!clump) {
        ped->m_pGunflashObject = nullptr;
        if (prevGf && g_orcLogLevel >= OrcLogLevel::Info) {
            OrcLogInfoThrottled(937u, 4000u, "gunflash: cleared (no target clump) pedRef=%d slotWo=%p",
                CPools::GetPedRef(ped), woSlot);
        }
        return;
    }
    RwFrame* gf = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
    ped->m_pGunflashObject = gf;
    const int pedRef = CPools::GetPedRef(ped);
    if (g_orcLogLevel >= OrcLogLevel::Info) {
        if (gf != prevGf) {
            OrcLogInfoThrottled(938u, 2500u,
                "gunflash: frame rebound pedRef=%d clump=%p slotWo=%p gf=%p (was %p)", pedRef, clump, woSlot, gf,
                prevGf);
        }
        if (!gf) {
            OrcLogInfoThrottled(939u, 12000u,
                "gunflash: no \"gunflash\" frame in weapon clump pedRef=%d clump=%p slotWo=%p (custom mesh may omit dummy)",
                pedRef, clump, woSlot);
        }
    }
}

static bool OrcComputeVanillaMuzzleWorldForPed(CPed* ped, int wt, CVector& out) {
    if (!ped || wt <= 0)
        return false;
    RwMatrix* bone = OrcGetBoneMatrix(ped, BONE_R_HAND);
    if (!bone)
        return false;
    const unsigned char skill = static_cast<unsigned char>(ped->GetWeaponSkill(static_cast<eWeaponType>(wt)));
    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), skill);
    if (!wi)
        return false;
    RwMatrix M{};
    std::memcpy(&M, bone, sizeof(RwMatrix));
    const float vx = wi->m_vecFireOffset.x;
    const float vy = wi->m_vecFireOffset.y;
    const float vz = wi->m_vecFireOffset.z;
    out.x = M.right.x * vx + M.up.x * vy + M.at.x * vz + M.pos.x;
    out.y = M.right.y * vx + M.up.y * vy + M.at.y * vz + M.pos.y;
    out.z = M.right.z * vx + M.up.z * vy + M.at.z * vz + M.pos.z;
    return true;
}

static bool OrcComputeAdjustedMuzzleForPed(CPed* ped, int wt, CVector& out) {
    if (!ped || wt <= 0)
        return false;
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    if (!h.enabled)
        return false;
    RwMatrix* bone = OrcGetBoneMatrix(ped, BONE_R_HAND);
    if (!bone)
        return false;
    const unsigned char skill = static_cast<unsigned char>(ped->GetWeaponSkill(static_cast<eWeaponType>(wt)));
    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), skill);
    if (!wi)
        return false;
    RwMatrix M{};
    std::memcpy(&M, bone, sizeof(RwMatrix));
    OrcApplyAttachmentOffset(&M, h.x, h.y, h.z);
    OrcRotateAttachmentMatrix(&M, h.rx, h.ry, h.rz);
    if (h.scale > 0.0f && h.scale != 1.0f) {
        RwV3d s = { h.scale, h.scale, h.scale };
        RwMatrixScale(&M, &s, rwCOMBINEPRECONCAT);
    }
    const float vx = wi->m_vecFireOffset.x;
    const float vy = wi->m_vecFireOffset.y;
    const float vz = wi->m_vecFireOffset.z;
    out.x = M.right.x * vx + M.up.x * vy + M.at.x * vz + M.pos.x;
    out.y = M.right.y * vx + M.up.y * vy + M.at.y * vz + M.pos.y;
    out.z = M.right.z * vx + M.up.z * vy + M.at.z * vz + M.pos.z;
    return true;
}

bool OrcHeldTryGetMuzzleWorldDeltaHeldMinusVanilla(CPed* ped, int wt, RwV3d* outDw) {
    if (!ped || wt <= 0 || !outDw)
        return false;
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    if (!h.enabled)
        return false;
    CVector vHeld{};
    CVector vVan{};
    if (!OrcComputeAdjustedMuzzleForPed(ped, wt, vHeld))
        return false;
    if (!OrcComputeVanillaMuzzleWorldForPed(ped, wt, vVan))
        return false;
    outDw->x = vHeld.x - vVan.x;
    outDw->y = vHeld.y - vVan.y;
    outDw->z = vHeld.z - vVan.z;
    return true;
}

using CWeapon_Fire_orig_t = bool(__thiscall*)(CWeapon* self, CEntity* firingEntity, CVector* origin, CVector* muzzlePosn,
    CEntity* targetEntity, CVector* target, CVector* originForDriveBy);
using CWeapon_FireInstantHit_orig_t = bool(__thiscall*)(CWeapon* self, CEntity* firingEntity, CVector* origin,
    CVector* muzzlePosn, CEntity* targetEntity, CVector* target, CVector* originForDriveBy, bool arg6, bool muzzle);

static CWeapon_Fire_orig_t g_CWeaponFire_Orig = nullptr;
static CWeapon_FireInstantHit_orig_t g_CWeaponFireInstantHit_Orig = nullptr;

static void OrcTryPatchMuzzlePosnForPedWeaponFx(CPed* ped, CWeapon* weapon, CVector* muzzlePosn, const char* phase) {
    if (!g_enabled || !ped || !weapon || !muzzlePosn)
        return;
    const int wt = static_cast<int>(weapon->m_eWeaponType);
    CVector adj{};
    if (!OrcComputeAdjustedMuzzleForPed(ped, wt, adj)) {
        if (g_heldWeaponTrace >= 2 && g_orcLogLevel >= OrcLogLevel::Info) {
            const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
            if (!h.enabled) {
                OrcLogInfoThrottled(703, 4000u,
                    "weapon fx: muzzle skip held_off wt=%d pedRef=%d phase=%s", wt, CPools::GetPedRef(ped),
                    phase ? phase : "?");
            }
        }
        return;
    }
    if (g_orcLogLevel >= OrcLogLevel::Info) {
        const CVector oldMuzzle = *muzzlePosn;
        const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
        const unsigned char skill = static_cast<unsigned char>(ped->GetWeaponSkill(static_cast<eWeaponType>(wt)));
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), skill);
        const float r2d = 180.0f / kOrcPi;
        const unsigned logId = (phase && phase[0] == 'I') ? 702u : 701u;
        OrcLogInfoThrottled(
            logId, 800u,
            "weapon fx: muzzle adjust phase=%s wt=%d pedRef=%d h=(%.3f,%.3f,%.3f)/(%.2f,%.2f,%.2f)/sc=%.3f vfo=(%.3f,%.3f,%.3f) old=(%.2f,%.2f,%.2f) new=(%.2f,%.2f,%.2f)",
            phase ? phase : "?", wt, CPools::GetPedRef(ped), h.x, h.y, h.z, h.rx * r2d, h.ry * r2d, h.rz * r2d, h.scale,
            wi ? wi->m_vecFireOffset.x : 0.0f, wi ? wi->m_vecFireOffset.y : 0.0f, wi ? wi->m_vecFireOffset.z : 0.0f,
            oldMuzzle.x, oldMuzzle.y, oldMuzzle.z, adj.x, adj.y, adj.z);
    }
    *muzzlePosn = adj;
}

static bool __fastcall CWeapon_Fire_Detour(CWeapon* self, void* /*edx*/, CEntity* firingEntity, CVector* origin,
    CVector* muzzlePosn, CEntity* targetEntity, CVector* target, CVector* originForDriveBy) {
    if (firingEntity && firingEntity->m_nType == ENTITY_TYPE_PED && muzzlePosn) {
        CPed* ped = reinterpret_cast<CPed*>(firingEntity);
        const int wtFire = static_cast<int>(self->m_eWeaponType);
        if (GetHeldPoseForPed(ped, wtFire, false).enabled) {
            OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
            OrcHeldNudgeGunflashMuzzleDeltaAfterFrameSync(ped, wtFire);
        }
        OrcTryPatchMuzzlePosnForPedWeaponFx(ped, self, muzzlePosn, "Fire");
    }
    if (!g_CWeaponFire_Orig)
        return false;
    return g_CWeaponFire_Orig(self, firingEntity, origin, muzzlePosn, targetEntity, target, originForDriveBy);
}

static bool __fastcall CWeapon_FireInstantHit_Detour(CWeapon* self, void* /*edx*/, CEntity* firingEntity, CVector* origin,
    CVector* muzzlePosn, CEntity* targetEntity, CVector* target, CVector* originForDriveBy, bool arg6, bool muzzle) {
    if (firingEntity && firingEntity->m_nType == ENTITY_TYPE_PED && muzzlePosn) {
        CPed* ped = reinterpret_cast<CPed*>(firingEntity);
        const int wtFire = static_cast<int>(self->m_eWeaponType);
        if (GetHeldPoseForPed(ped, wtFire, false).enabled) {
            OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped);
            OrcHeldNudgeGunflashMuzzleDeltaAfterFrameSync(ped, wtFire);
        }
        OrcTryPatchMuzzlePosnForPedWeaponFx(ped, self, muzzlePosn, "FireInstantHit");
    }
    if (!g_CWeaponFireInstantHit_Orig)
        return false;
    return g_CWeaponFireInstantHit_Orig(
        self, firingEntity, origin, muzzlePosn, targetEntity, target, originForDriveBy, arg6, muzzle);
}

void OrcWeaponEnsureFireFxHooksInstalled() {
    if (g_weaponFireFxHooksInstalled)
        return;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("CWeapon::Fire FX hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CWeapon_Fire), reinterpret_cast<void*>(&CWeapon_Fire_Detour),
            reinterpret_cast<void**>(&g_CWeaponFire_Orig)) != MH_OK) {
        OrcLogError("CWeapon::Fire hook: MH_CreateHook failed (0x%08X)", (unsigned)kAddr_CWeapon_Fire);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CWeapon_FireInstantHit),
            reinterpret_cast<void*>(&CWeapon_FireInstantHit_Detour),
            reinterpret_cast<void**>(&g_CWeaponFireInstantHit_Orig)) != MH_OK) {
        OrcLogError("CWeapon::FireInstantHit hook: MH_CreateHook failed (0x%08X)",
            (unsigned)kAddr_CWeapon_FireInstantHit);
        return;
    }
    g_weaponFireFxHooksInstalled = true;
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CWeapon_Fire));
    if (st != MH_OK)
        OrcLogError("CWeapon::Fire hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("CWeapon::Fire muzzle FX hook (0x%08X)", (unsigned)kAddr_CWeapon_Fire);
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CWeapon_FireInstantHit));
    if (st != MH_OK)
        OrcLogError("CWeapon::FireInstantHit hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("CWeapon::FireInstantHit muzzle FX hook (0x%08X)", (unsigned)kAddr_CWeapon_FireInstantHit);
}
