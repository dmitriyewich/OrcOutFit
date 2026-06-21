// Stem / path resolve, negative cache, suppress helpers.

#include "plugin.h"

#include "CGame.h"
#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CPhysical.h"
#include "CVector.h"
#include "CWeaponInfo.h"
#include "eEntityType.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "CAEAudioEntity.h"
#include "CAEWeaponAudioEntity.h"
#include "CEntity.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_internal.h"
#include "orc_weapon_audio_names.h"

enum OrcPathCacheState : uint8_t { kUnknown = 0, kMissing = 1, kPresent = 2 };

static std::unordered_map<std::string, OrcPathCacheState> g_pathCache;
static std::mutex g_pathCacheMutex;

static const char* kAudioExts[] = {".wav", ".mp3", ".flac", ".ogg"};

static bool OrcAudioPointerLooksReadable(const void* ptr) {
    return reinterpret_cast<uintptr_t>(ptr) >= 0x10000u;
}

CPed* OrcWeaponAudioValidatePedCandidate(CPed* ped, const char* source) {
    if (!OrcAudioPointerLooksReadable(ped)) {
        if (ped)
            OrcLogInfoThrottled(916, 4000u, "weapon audio: skip bad ped pointer source=%s ped=%p", source, ped);
        return nullptr;
    }

    __try {
        if ((static_cast<unsigned>(ped->m_nType) & 7u) != static_cast<unsigned>(ENTITY_TYPE_PED))
            return nullptr;

        const int ref = OrcSafeGetPedRef(ped);
        if (ref <= 0)
            return nullptr;

        return CPools::GetPed(ref) == ped ? ped : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogInfoThrottled(917,
            4000u,
            "weapon audio: ped candidate SEH ex=0x%08X source=%s ped=%p",
            GetExceptionCode(),
            source,
            ped);
        return nullptr;
    }
}

static std::string OrcDirNameA(const std::string& p) {
    size_t slash = p.find_last_of("\\/");
    if (slash == std::string::npos)
        return std::string(".");
    return p.substr(0, slash);
}

static bool OrcPathExistsInDir(const std::string& dir, const std::string& baseName, std::string& outPath) {
    if (dir.empty() || baseName.empty())
        return false;
    for (const char* ext : kAudioExts) {
        const std::string p = OrcJoinPath(dir, baseName + ext);
        if (OrcWeaponAudioPathExistsCached(p)) {
            outPath = p;
            return true;
        }
    }
    return false;
}

void OrcWeaponAudioInvalidateCaches() {
    OrcWeaponAudioConfigClearStemOverrides();
    std::lock_guard<std::mutex> lock(g_pathCacheMutex);
    g_pathCache.clear();
    OrcWeaponAudioLoopsStopAll();
    OrcWeaponAudioHooksClearShootThrottleState();
}

bool OrcWeaponAudioPathExistsCached(const std::string& path) {
    if (path.empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(g_pathCacheMutex);
        auto it = g_pathCache.find(path);
        if (it != g_pathCache.end()) {
            if (it->second == kPresent)
                return true;
            if (it->second == kMissing)
                return false;
        }
    }
    const bool exists = OrcFileExistsA(path.c_str());
    std::lock_guard<std::mutex> lock(g_pathCacheMutex);
    g_pathCache[path] = exists ? kPresent : kMissing;
    return exists;
}

CPed* OrcWeaponAudioPedFromPhysical(CPhysical* physical) {
    if (!OrcAudioPointerLooksReadable(physical))
        return nullptr;
    return OrcWeaponAudioValidatePedCandidate(static_cast<CPed*>(physical), "physical");
}

bool OrcWeaponAudioIsLocalPlayerPed(CPed* ped) {
    if (!ped)
        return false;
    CPlayerPed* local = FindPlayerPed(0);
    if (!local)
        return false;
    if (ped == local)
        return true;

    const int pedRef = OrcSafeGetPedRef(ped);
    const int localRef = OrcSafeGetPedRef(local);
    return pedRef > 0 && pedRef == localRef;
}

float OrcWeaponAudioLocalPedDistance(CPed* ped) {
    if (!ped || OrcWeaponAudioIsLocalPlayerPed(ped))
        return 0.0f;

    CPlayerPed* local = FindPlayerPed(0);
    if (!local)
        return 0.0f;

    const CVector lp = local->GetPosition();
    const CVector p = ped->GetPosition();
    const float dx = lp.x - p.x;
    const float dy = lp.y - p.y;
    const float dz = lp.z - p.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool OrcWeaponAudioIsBeyondMaxDistance(CPed* ped, float maxDist) {
    if (!ped || maxDist <= 0.0f || maxDist >= 999999.0f || OrcWeaponAudioIsLocalPlayerPed(ped))
        return false;
    return OrcWeaponAudioLocalPedDistance(ped) > maxDist;
}

float OrcWeaponAudioApplyDistanceCullGain(CPed* ped, float maxDist, float gain) {
    return OrcWeaponAudioIsBeyondMaxDistance(ped, maxDist) ? 0.0f : gain;
}

OrcWeaponSpatial OrcWeaponAudioSpatialForPed(CPed* ped) {
    return OrcWeaponAudioIsLocalPlayerPed(ped) ? OrcWeaponSpatial::ListenerRelative : OrcWeaponSpatial::WorldAtPed;
}

CPed* OrcWeaponAudioPedFromWeaponAudio(CAEWeaponAudioEntity* self) {
    if (!OrcAudioPointerLooksReadable(self))
        return nullptr;

    CPed* directPed = nullptr;
    CEntity* entity = nullptr;
    __try {
        directPed = self->m_pPed;
        entity = self->m_pEntity;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogInfoThrottled(918, 4000u, "weapon audio: read weapon audio entity SEH ex=0x%08X self=%p", GetExceptionCode(), self);
        return nullptr;
    }

    if (CPed* ped = OrcWeaponAudioPedFromPhysical(reinterpret_cast<CPhysical*>(entity)))
        return ped;

    return OrcWeaponAudioValidatePedCandidate(directPed, "weaponAudio.m_pPed");
}

CPed* OrcWeaponAudioResolvePedFromSoundBase(CAEAudioEntity* base) {
    if (!OrcAudioPointerLooksReadable(base))
        return nullptr;

    CEntity* entity = nullptr;
    __try {
        entity = base->m_pEntity;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogInfoThrottled(919, 4000u, "weapon audio: read sound base SEH ex=0x%08X base=%p", GetExceptionCode(), base);
        return nullptr;
    }

    if (CPed* ped = OrcWeaponAudioValidatePedCandidate(static_cast<CPed*>(entity), "soundBase.m_pEntity"))
        return ped;

    return OrcWeaponAudioPedFromWeaponAudio(reinterpret_cast<CAEWeaponAudioEntity*>(base));
}

bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out) {
    out = {};
    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || weaponType < 0)
        return false;
    ped = OrcWeaponAudioValidatePedCandidate(ped, "stemContext");
    if (!ped)
        return false;
    WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, weaponType, true);
    if (!asset || asset->dffPath.empty())
        return false;
    out.ped = ped;
    out.weaponType = weaponType;
    out.asset = asset;
    out.stem = OrcBaseNameNoExt(asset->dffPath);
    out.dir = OrcDirNameA(asset->dffPath);
    return !out.stem.empty();
}

bool OrcWeaponAudioPedHasReplacementAudio(CPed* ped, int weaponType) {
    OrcWeaponAudioStemContext ctx;
    return OrcWeaponAudioTryBuildStemContext(ped, weaponType, ctx);
}

bool OrcWeaponAudioResolveFirstExistingAudioPath(const OrcWeaponAudioStemContext& ctx, const char* suffix, std::string& outPath) {
    outPath.clear();
    if (!suffix || !suffix[0] || ctx.stem.empty())
        return false;

    if (OrcPathExistsInDir(ctx.dir, ctx.stem + std::string(suffix), outPath))
        return true;

    if (const char* bare = OrcWeaponAudioBareAliasForSuffix(suffix)) {
        if (OrcPathExistsInDir(ctx.dir, bare, outPath))
            return true;
    }

    return false;
}

bool OrcWeaponAudioTryPlaySuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, float gainScale,
    OrcWeaponSpatial spatial) {
    std::string path;
    if (!OrcWeaponAudioResolveFirstExistingAudioPath(ctx, suffix, path) || path.empty())
        return false;

    const OrcWeaponSoundClass cls = OrcWeaponInferSoundClassFromSuffix(suffix);
    OrcWeaponAudioPlayParams params = OrcWeaponAudioBuildPlayParams(&ctx, gainScale, spatial, cls);

    if (params.spatial == OrcWeaponSpatial::WorldAtPed && OrcWeaponAudioIsBeyondMaxDistance(ctx.ped, params.att.maxDist)) {
        OrcWeaponAudioMarkSuppressVanilla();
        OrcLogInfoThrottled(409,
            2000u,
            "weapon audio: cull one-shot pedRef=%d suffix=%s dist=%.1f max=%.1f",
            OrcSafeGetPedRef(ctx.ped),
            suffix,
            OrcWeaponAudioLocalPedDistance(ctx.ped),
            params.att.maxDist);
        return true;
    }

    if (!OrcWeaponAudioTryPlayPath(path.c_str(), params, ctx.ped))
        return false;
    OrcWeaponAudioMarkSuppressVanilla();
    return true;
}

void OrcWeaponAudioMarkSuppressVanilla() {
    g_suppressVanillaGunSoundsUntilTick = GetTickCount() + 120;
}

bool OrcWeaponAudioIsInterior() {
    return CGame::currArea > 0;
}

static int OrcWeaponAudioActiveWeaponType(CPed* ped) {
    if (!ped)
        return -1;
    const int slot = ped->m_nSelectedWepSlot;
    if (slot < 0 || slot >= 13)
        return -1;
    return static_cast<int>(ped->m_aWeapons[slot].m_eWeaponType);
}

static bool OrcWeaponAudioPathExistsForSuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix) {
    std::string tmp;
    return OrcWeaponAudioResolveFirstExistingAudioPath(ctx, suffix, tmp);
}

bool OrcWeaponAudioHasLoopCustomAudio(const OrcWeaponAudioStemContext& ctx) {
    static const char* kSuffixes[] = {
        "_flamethrower_fire",
        "_flamethrower_idlegasloop",
        "_minigun_fireloop",
        "_minigun_barrelspinloop",
        "_chainsaw_idle",
        "_chainsaw_active",
        "_chainsaw_cuttingflesh",
        "_spraycan_sprayloop",
        "_extinguisher_loop",
    };
    for (const char* s : kSuffixes) {
        if (OrcWeaponAudioPathExistsForSuffix(ctx, s))
            return true;
    }
    return false;
}

bool OrcWeaponAudioHasFireRelatedCustomAudio(const OrcWeaponAudioStemContext& ctx) {
    static const char* kBase[] = {"_shoot", "_distant", "_low_ammo", "_dryfire"};
    for (const char* s : kBase) {
        if (OrcWeaponAudioPathExistsForSuffix(ctx, s))
            return true;
    }
    const int maxAlt = std::max(1, std::min(10, g_weaponCustomSoundMaxAlternatives));
    for (int i = 0; i < maxAlt; ++i) {
        char buf[32];
        sprintf_s(buf, "_shoot%d", i);
        if (OrcWeaponAudioPathExistsForSuffix(ctx, buf))
            return true;
        sprintf_s(buf, "_distant%d", i);
        if (OrcWeaponAudioPathExistsForSuffix(ctx, buf))
            return true;
    }
    return false;
}

bool OrcWeaponAudioShouldSuppressVanillaGun(CAEWeaponAudioEntity* self) {
    if (!self)
        return false;
    CPed* ped = OrcWeaponAudioPedFromWeaponAudio(self);
    if (!ped)
        return false;
    if (GetTickCount() < g_suppressVanillaGunSoundsUntilTick)
        return true;
    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled)
        return false;
    const int wt = OrcWeaponAudioActiveWeaponType(ped);
    if (wt < 0)
        return false;
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, wt, ctx))
        return false;

    // Loop-оружие: глушим PlayGunSounds только если есть свой loop-файл (не из-за одного _shoot).
    switch (wt) {
    case WEAPONTYPE_MINIGUN:
        if (OrcWeaponAudioPathExistsForSuffix(ctx, "_minigun_fireloop"))
            return OrcWeaponAudioPedIsMinigunFiring(ped);
        return OrcWeaponAudioPathExistsForSuffix(ctx, "_shoot");
    case WEAPONTYPE_FTHROWER:
        return OrcWeaponAudioPathExistsForSuffix(ctx, "_flamethrower_fire");
    case WEAPONTYPE_CHAINSAW:
        return OrcWeaponAudioPathExistsForSuffix(ctx, "_chainsaw_idle") ||
            OrcWeaponAudioPathExistsForSuffix(ctx, "_chainsaw_active") ||
            OrcWeaponAudioPathExistsForSuffix(ctx, "_chainsaw_cuttingflesh");
    case WEAPONTYPE_SPRAYCAN:
        return OrcWeaponAudioPathExistsForSuffix(ctx, "_spraycan_sprayloop");
    case WEAPONTYPE_EXTINGUISHER:
        return OrcWeaponAudioPathExistsForSuffix(ctx, "_extinguisher_loop");
    default:
        return OrcWeaponAudioHasFireRelatedCustomAudio(ctx);
    }
}
