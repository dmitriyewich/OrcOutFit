// Stem / path resolve, negative cache, suppress helpers.

#include "plugin.h"

#include "CCamera.h"
#include "CGame.h"
#include "CPed.h"
#include "CPlayerPed.h"
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
#include "orc_path.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_internal.h"

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

void OrcWeaponAudioInvalidateCaches() {
    OrcWeaponAudioConfigClearStemOverrides();
    std::lock_guard<std::mutex> lock(g_pathCacheMutex);
    g_pathCache.clear();
    OrcWeaponAudioLoopsStopAll();
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

bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out) {
    out = {};
    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !ped || weaponType < 0)
        return false;
    WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, weaponType, true);
    if (!asset || asset->dffPath.empty())
        return false;
    out.ped = ped;
    out.weaponType = weaponType;
    out.asset = asset;
    out.stem = OrcBaseNameNoExt(asset->dffPath);
    out.dir = OrcDirNameA(asset->dffPath);
    return true;
}

bool OrcWeaponAudioResolveFirstExistingAudioPath(const OrcWeaponAudioStemContext& ctx, const char* suffix, std::string& outPath) {
    outPath.clear();
    if (!suffix || !suffix[0] || ctx.stem.empty())
        return false;
    for (const char* ext : kAudioExts) {
        const std::string p = OrcJoinPath(ctx.dir, ctx.stem + std::string(suffix) + ext);
        if (OrcWeaponAudioPathExistsCached(p)) {
            outPath = p;
            return true;
        }
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

static bool OrcIsLocalPlayerPed(CPed* ped) {
    CPlayerPed* pl = FindPlayerPed(0);
    return pl && ped == pl;
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
    if (!self || !self->m_pPed)
        return false;
    CPed* ped = self->m_pPed;
    if (!OrcIsLocalPlayerPed(ped))
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
    return OrcWeaponAudioHasFireRelatedCustomAudio(ctx) || OrcWeaponAudioHasLoopCustomAudio(ctx);
}
