#include "plugin.h"

#include "CPed.h"
#include "CPools.h"
#include "CWeaponInfo.h"
#include "CEntity.h"
#include "CWeapon.h"
#include "CVector.h"
#include "CClumpModelInfo.h"
#include "CTimer.h"
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

static constexpr int kOrcMuzzleDeltaCacheMs = 160;
static constexpr int kOrcMaxRwFrameAncestorsFx = 64;

struct OrcHeldMuzzleDeltaCache {
    int pedRef = 0;
    int wt = 0;
    unsigned timeMs = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rx = 0.0f;
    float ry = 0.0f;
    float rz = 0.0f;
    float scale = 1.0f;
    RwV3d dw{};
};

static OrcHeldMuzzleDeltaCache s_lastHeldMuzzleDelta;

static bool OrcFxVecFinite(const RwV3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool OrcFxMatrixUsable(const RwMatrix* m) {
    if (!m)
        return false;
    if (!OrcFxVecFinite(m->right) || !OrcFxVecFinite(m->up) || !OrcFxVecFinite(m->at) || !OrcFxVecFinite(m->pos))
        return false;
    const float r2 = m->right.x * m->right.x + m->right.y * m->right.y + m->right.z * m->right.z;
    const float u2 = m->up.x * m->up.x + m->up.y * m->up.y + m->up.z * m->up.z;
    const float a2 = m->at.x * m->at.x + m->at.y * m->at.y + m->at.z * m->at.z;
    return r2 > 1e-8f && u2 > 1e-8f && a2 > 1e-8f;
}

static RwFrame* OrcFxRwFrameGetParent(RwFrame* f) {
    if (!f)
        return nullptr;
    return reinterpret_cast<RwFrame*>(plugin::GetObjectParent(reinterpret_cast<RwObject*>(f)));
}

static bool OrcFxRwFrameIsDescendantOf(RwFrame* frame, RwFrame* ancestor) {
    if (!frame || !ancestor)
        return false;
    int steps = 0;
    for (RwFrame* x = frame; x && steps < kOrcMaxRwFrameAncestorsFx; x = OrcFxRwFrameGetParent(x), ++steps) {
        if (x == ancestor)
            return true;
    }
    return false;
}

struct OrcFxWeaponBasisCtx {
    RwFrame* gunflash = nullptr;
    RwFrame* basis = nullptr;
};

static RpAtomic* OrcFxFindWeaponBasisAtomicCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<OrcFxWeaponBasisCtx*>(data);
    if (!ctx || ctx->basis || !atomic)
        return atomic;
    RwFrame* frame = RpAtomicGetFrame(atomic);
    if (!frame)
        return atomic;
    if (ctx->gunflash && OrcFxRwFrameIsDescendantOf(frame, ctx->gunflash))
        return atomic;
    ctx->basis = frame;
    return atomic;
}

static RwFrame* OrcFxResolveWeaponBasisFrame(CPed* ped, int wt) {
    if (!ped)
        return nullptr;
    if (RpClump* clump = OrcPedResolveGunflashTargetClump(ped, wt)) {
        OrcFxWeaponBasisCtx ctx{};
        ctx.gunflash = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
        RpClumpForAllAtomics(clump, OrcFxFindWeaponBasisAtomicCb, &ctx);
        if (ctx.basis)
            return ctx.basis;
        return RpClumpGetFrame(clump);
    }
    RwObject* wo = ped->m_pWeaponObject;
    if (!wo)
        return nullptr;
    if (wo->type == rpATOMIC)
        return RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(wo));
    if (wo->type == rpCLUMP) {
        RpClump* clump = reinterpret_cast<RpClump*>(wo);
        OrcFxWeaponBasisCtx ctx{};
        ctx.gunflash = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
        RpClumpForAllAtomics(clump, OrcFxFindWeaponBasisAtomicCb, &ctx);
        if (ctx.basis)
            return ctx.basis;
        return RpClumpGetFrame(clump);
    }
    return nullptr;
}

static RwV3d OrcFxWorldPointToFrameLocal(const RwMatrix* frameLtm, const CVector& world) {
    RwV3d d{};
    d.x = world.x - frameLtm->pos.x;
    d.y = world.y - frameLtm->pos.y;
    d.z = world.z - frameLtm->pos.z;
    RwV3d local{};
    local.x = frameLtm->right.x * d.x + frameLtm->right.y * d.y + frameLtm->right.z * d.z;
    local.y = frameLtm->up.x * d.x + frameLtm->up.y * d.y + frameLtm->up.z * d.z;
    local.z = frameLtm->at.x * d.x + frameLtm->at.y * d.y + frameLtm->at.z * d.z;
    return local;
}

static CVector OrcFxFrameLocalPointToWorld(const RwMatrix& frameLtm, const RwV3d& local) {
    CVector out{};
    out.x = frameLtm.right.x * local.x + frameLtm.up.x * local.y + frameLtm.at.x * local.z + frameLtm.pos.x;
    out.y = frameLtm.right.y * local.x + frameLtm.up.y * local.y + frameLtm.at.y * local.z + frameLtm.pos.y;
    out.z = frameLtm.right.z * local.x + frameLtm.up.z * local.y + frameLtm.at.z * local.z + frameLtm.pos.z;
    return out;
}

static bool OrcFxTryGetPoseBaselineLtm(RwFrame* frame, RwMatrix& out) {
    RwMatrix local{};
    if (!OrcHeldTryGetPoseEngineBaselineForFrame(frame, local))
        return false;
    if (RwFrame* parent = OrcFxRwFrameGetParent(frame)) {
        const RwMatrix* parentLtm = RwFrameGetLTM(parent);
        if (!OrcFxMatrixUsable(parentLtm))
            return false;
        RwMatrixMultiply(&out, &local, parentLtm);
    } else {
        out = local;
    }
    return OrcFxMatrixUsable(&out);
}

static bool OrcComputeAdjustedMuzzleFromWeaponFrame(CPed* ped, int wt, const CVector& vanillaMuzzle, CVector& out) {
    if (!ped || wt <= 0)
        return false;
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    if (!h.enabled)
        return false;
    RwFrame* basisFrame = OrcFxResolveWeaponBasisFrame(ped, wt);
    const RwMatrix* basisLtm = basisFrame ? RwFrameGetLTM(basisFrame) : nullptr;
    if (!OrcFxMatrixUsable(basisLtm))
        return false;
    RwMatrix baseline{};
    const RwMatrix* basis = basisLtm;
    if (OrcFxTryGetPoseBaselineLtm(basisFrame, baseline))
        basis = &baseline;
    const RwV3d localMuzzle = OrcFxWorldPointToFrameLocal(basis, vanillaMuzzle);
    RwMatrix held = *basis;
    OrcApplyAttachmentOffset(&held, h.x, h.y, h.z);
    OrcRotateAttachmentMatrix(&held, h.rx, h.ry, h.rz);
    if (h.scale > 0.0f && h.scale != 1.0f) {
        RwV3d s = { h.scale, h.scale, h.scale };
        RwMatrixScale(&held, &s, rwCOMBINEPRECONCAT);
    }
    if (!OrcFxMatrixUsable(&held))
        return false;
    out = OrcFxFrameLocalPointToWorld(held, localMuzzle);
    return true;
}

static void OrcStoreHeldMuzzleDelta(CPed* ped, int wt, const CVector& oldMuzzle, const CVector& newMuzzle) {
    const int pedRef = ped ? CPools::GetPedRef(ped) : 0;
    if (pedRef <= 0 || wt <= 0)
        return;
    s_lastHeldMuzzleDelta.pedRef = pedRef;
    s_lastHeldMuzzleDelta.wt = wt;
    s_lastHeldMuzzleDelta.timeMs = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    s_lastHeldMuzzleDelta.x = h.x;
    s_lastHeldMuzzleDelta.y = h.y;
    s_lastHeldMuzzleDelta.z = h.z;
    s_lastHeldMuzzleDelta.rx = h.rx;
    s_lastHeldMuzzleDelta.ry = h.ry;
    s_lastHeldMuzzleDelta.rz = h.rz;
    s_lastHeldMuzzleDelta.scale = h.scale;
    s_lastHeldMuzzleDelta.dw.x = newMuzzle.x - oldMuzzle.x;
    s_lastHeldMuzzleDelta.dw.y = newMuzzle.y - oldMuzzle.y;
    s_lastHeldMuzzleDelta.dw.z = newMuzzle.z - oldMuzzle.z;
}

static bool OrcTryGetRecentHeldMuzzleDelta(CPed* ped, int wt, RwV3d* outDw) {
    if (!ped || wt <= 0 || !outDw)
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0 || s_lastHeldMuzzleDelta.pedRef != pedRef || s_lastHeldMuzzleDelta.wt != wt)
        return false;
    const unsigned now = static_cast<unsigned>(CTimer::m_snTimeInMilliseconds);
    if (now - s_lastHeldMuzzleDelta.timeMs > kOrcMuzzleDeltaCacheMs)
        return false;
    const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
    if (std::fabs(h.x - s_lastHeldMuzzleDelta.x) > 1e-5f || std::fabs(h.y - s_lastHeldMuzzleDelta.y) > 1e-5f ||
        std::fabs(h.z - s_lastHeldMuzzleDelta.z) > 1e-5f || std::fabs(h.rx - s_lastHeldMuzzleDelta.rx) > 1e-5f ||
        std::fabs(h.ry - s_lastHeldMuzzleDelta.ry) > 1e-5f || std::fabs(h.rz - s_lastHeldMuzzleDelta.rz) > 1e-5f ||
        std::fabs(h.scale - s_lastHeldMuzzleDelta.scale) > 1e-5f)
        return false;
    *outDw = s_lastHeldMuzzleDelta.dw;
    return true;
}

void OrcPedSyncGunflashFrameFromCurrentWeaponObject(CPed* ped, int wtHint) {
    if (!ped)
        return;
    RwObject* const woSlot = ped->m_pWeaponObject;
    RwFrame* const prevGf = ped->m_pGunflashObject;
    RpClump* clump = OrcPedResolveGunflashTargetClump(ped, wtHint);
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
    if (OrcTryGetRecentHeldMuzzleDelta(ped, wt, outDw))
        return true;
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
    const CVector oldMuzzle = *muzzlePosn;
    CVector adj{};
    const bool frameBasis = OrcComputeAdjustedMuzzleFromWeaponFrame(ped, wt, oldMuzzle, adj);
    if (!frameBasis && !OrcComputeAdjustedMuzzleForPed(ped, wt, adj)) {
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
    OrcStoreHeldMuzzleDelta(ped, wt, oldMuzzle, adj);
    if (g_orcLogLevel >= OrcLogLevel::Info) {
        const HeldWeaponPoseCfg& h = GetHeldPoseForPed(ped, wt, false);
        const unsigned char skill = static_cast<unsigned char>(ped->GetWeaponSkill(static_cast<eWeaponType>(wt)));
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), skill);
        const float r2d = 180.0f / kOrcPi;
        const unsigned logId = (phase && phase[0] == 'I') ? 702u : 701u;
        OrcLogInfoThrottled(
            logId, 800u,
            "weapon fx: muzzle adjust phase=%s src=%s wt=%d pedRef=%d h=(%.3f,%.3f,%.3f)/(%.2f,%.2f,%.2f)/sc=%.3f vfo=(%.3f,%.3f,%.3f) old=(%.2f,%.2f,%.2f) new=(%.2f,%.2f,%.2f)",
            phase ? phase : "?", frameBasis ? "weaponFrame" : "bone", wt, CPools::GetPedRef(ped), h.x, h.y, h.z,
            h.rx * r2d, h.ry * r2d, h.rz * r2d, h.scale,
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
            OrcTryPatchMuzzlePosnForPedWeaponFx(ped, self, muzzlePosn, "Fire");
            OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped, wtFire);
            OrcHeldNudgeGunflashMuzzleDeltaAfterFrameSync(ped, wtFire);
        } else {
            OrcTryPatchMuzzlePosnForPedWeaponFx(ped, self, muzzlePosn, "Fire");
        }
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
            OrcTryPatchMuzzlePosnForPedWeaponFx(ped, self, muzzlePosn, "FireInstantHit");
            OrcPedSyncGunflashFrameFromCurrentWeaponObject(ped, wtFire);
            OrcHeldNudgeGunflashMuzzleDeltaAfterFrameSync(ped, wtFire);
        } else {
            OrcTryPatchMuzzlePosnForPedWeaponFx(ped, self, muzzlePosn, "FireInstantHit");
        }
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
