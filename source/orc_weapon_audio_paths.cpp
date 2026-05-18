// Stem / path resolve, negative cache, suppress helpers.

#include "plugin.h"

#include "CCamera.h"
#include "CGame.h"
#include "CPed.h"
#include "CPools.h"
#include "CPhysical.h"
#include "CVector.h"
#include "CWeaponInfo.h"
#include "eEntityType.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "CAEWeaponAudioEntity.h"

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
    if (!physical)
        return nullptr;
    if ((static_cast<unsigned>(physical->m_nType) & 7u) != static_cast<unsigned>(ENTITY_TYPE_PED))
        return nullptr;
    CPed* ped = static_cast<CPed*>(physical);
    if (reinterpret_cast<uintptr_t>(ped) < 0x10000u)
        return nullptr;
    const int ref = CPools::GetPedRef(ped);
    if (ref < 0 || CPools::GetPed(ref) != ped)
        return nullptr;
    return ped;
}

CPed* OrcWeaponAudioPedFromWeaponAudio(CAEWeaponAudioEntity* self) {
    if (!self)
        return nullptr;
    if (self->m_pPed) {
        CPed* ped = self->m_pPed;
        if (reinterpret_cast<uintptr_t>(ped) >= 0x10000u) {
            const int ref = CPools::GetPedRef(ped);
            if (ref >= 0 && CPools::GetPed(ref) == ped)
                return ped;
        }
    }
    return OrcWeaponAudioPedFromPhysical(reinterpret_cast<CPhysical*>(self->m_pEntity));
}

bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out) {
    out = {};
    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !ped || weaponType < 0)
        return false;
    if (ped->m_nType != ENTITY_TYPE_PED)
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

    if (!OrcWeaponAudioTryPlayPath(path.c_str(), params, ctx.ped))
        return false;
    OrcWeaponAudioMarkSuppressVanilla();
    return true;
}

void OrcWeaponAudioMarkSuppressVanilla() {
    g_suppressVanillaGunSoundsUntilTick = GetTickCount() + 120;
}

float OrcWeaponAudioCamPedDistance(CPed* ped) {
    if (!ped)
        return 0.0f;
    const CVector cam = *TheCamera.GetGameCamPosition();
    const CVector p = ped->GetPosition();
    const float dx = cam.x - p.x;
    const float dy = cam.y - p.y;
    const float dz = cam.z - p.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
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
