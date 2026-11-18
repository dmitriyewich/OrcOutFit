// Stem / path resolve and cached audio candidates.

#include "plugin.h"

#include "CCamera.h"
#include "CGame.h"
#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CPhysical.h"
#include "CVector.h"
#include "eEntityType.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "CAEWeaponAudioEntity.h"
#include "CEntity.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_internal.h"

using OrcAudioPathCandidates = std::vector<std::string>;
struct OrcNumberedAudioVariants {
    uint16_t mask = 0;
    int checked = 0;
};

struct OrcAudioStemPaths {
    std::map<std::string, OrcAudioPathCandidates, std::less<>> bySuffix;
    std::map<std::string, OrcNumberedAudioVariants, std::less<>> byPrefix;
};

static std::unordered_map<std::string, OrcAudioStemPaths> g_audioPathsByDff;
static std::mutex g_audioPathsMutex;

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

static void OrcAppendExistingAudioPaths(const std::string& dir, const std::string& baseName,
    OrcAudioPathCandidates& paths) {
    constexpr const char* extensions[] = {".wav", ".mp3", ".flac", ".ogg"};
    for (const char* ext : extensions) {
        std::string path = OrcJoinPath(dir, baseName + ext);
        if (OrcFileExistsA(path.c_str()))
            paths.push_back(std::move(path));
    }
}

// Callers hold g_audioPathsMutex while using the returned candidate list.
static const OrcAudioPathCandidates& OrcFindAudioPaths(const OrcWeaponAudioStemContext& ctx, const char* suffix) {
    static const OrcAudioPathCandidates empty;
    if (!ctx.asset || !suffix || suffix[0] != '_' || !suffix[1] || ctx.stem.empty())
        return empty;

    auto& bySuffix = g_audioPathsByDff[ctx.asset->dffPath].bySuffix;
    const auto cached = bySuffix.find(suffix);
    if (cached != bySuffix.end())
        return cached->second;

    OrcAudioPathCandidates paths;
    OrcAppendExistingAudioPaths(ctx.dir, ctx.stem + suffix, paths);
    OrcAppendExistingAudioPaths(ctx.dir, suffix + 1, paths);
    if (strcmp(suffix, "_shoot") == 0)
        OrcAppendExistingAudioPaths(ctx.dir, ctx.stem, paths);
    return bySuffix.emplace(suffix, std::move(paths)).first->second;
}

void OrcWeaponAudioInvalidateCaches() {
    OrcWeaponAudioConfigClearStemOverrides();
    {
        std::lock_guard<std::mutex> lock(g_audioPathsMutex);
        g_audioPathsByDff.clear();
    }
    OrcWeaponAudioLoopsStopAll();
    OrcWeaponAudioClearBufferCache();
    OrcWeaponAudioHooksClearShootThrottleState();
}

CPed* OrcWeaponAudioPedFromPhysical(CPhysical* physical) {
    if (!OrcAudioPointerLooksReadable(physical))
        return nullptr;
    return OrcWeaponAudioValidatePedCandidate(static_cast<CPed*>(physical), "physical");
}

bool OrcWeaponAudioIsLocalPlayerPed(CPed* ped) {
    return ped && ped == FindPlayerPed(0);
}

float OrcWeaponAudioCameraDistanceSquared(CPed* ped) {
    const CVector* camera = TheCamera.GetGameCamPosition();
    if (!ped || !camera)
        return 0.0f;

    const CVector& position = ped->GetPosition();
    const float dx = position.x - camera->x;
    const float dy = position.y - camera->y;
    const float dz = position.z - camera->z;
    return dx * dx + dy * dy + dz * dz;
}

bool OrcWeaponAudioIsBeyondMaxDistance(CPed* ped, float maxDist) {
    return maxDist > 0.0f && maxDist < 999999.0f &&
        OrcWeaponAudioCameraDistanceSquared(ped) > maxDist * maxDist;
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

bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out) {
    out = {};
    if (!OrcWeaponAudioCanPlayCustom() || weaponType < 0)
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

uint16_t OrcWeaponAudioNumberedVariants(const OrcWeaponAudioStemContext& ctx, const char* prefix, int maxAlternatives) {
    const int count = std::clamp(maxAlternatives, 0, 10);
    if (!ctx.asset || !prefix || count == 0)
        return 0;

    std::lock_guard<std::mutex> lock(g_audioPathsMutex);
    auto& byPrefix = g_audioPathsByDff[ctx.asset->dffPath].byPrefix;
    auto it = byPrefix.find(prefix);
    if (it == byPrefix.end())
        it = byPrefix.try_emplace(prefix).first;

    auto& variants = it->second;
    if (variants.checked < count) {
        std::string suffix(prefix);
        suffix.push_back('0');
        for (int i = variants.checked; i < count; ++i) {
            suffix.back() = static_cast<char>('0' + i);
            if (!OrcFindAudioPaths(ctx, suffix.c_str()).empty())
                variants.mask |= static_cast<uint16_t>(1u << i);
        }
        variants.checked = count;
    }
    return static_cast<uint16_t>(variants.mask & ((1u << count) - 1u));
}

ALuint OrcWeaponAudioResolveFirstPlayableAudioBuffer(const OrcWeaponAudioStemContext& ctx, const char* suffix) {
    std::lock_guard<std::mutex> lock(g_audioPathsMutex);
    const auto& paths = OrcFindAudioPaths(ctx, suffix);
    if (paths.empty() || !OrcWeaponAudioOpenAlInit())
        return 0;
    for (const std::string& path : paths) {
        if (const ALuint buffer = OrcGetOrCreateBufferForPath(path))
            return buffer;
    }
    return 0;
}

bool OrcWeaponAudioTryPlaySuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, float gainScale,
    OrcWeaponSpatial spatial, uint32_t delayMs) {
    const ALuint buffer = OrcWeaponAudioResolveFirstPlayableAudioBuffer(ctx, suffix);
    if (!buffer)
        return false;

    const OrcWeaponSoundClass cls = OrcWeaponInferSoundClassFromSuffix(suffix);
    const OrcWeaponAudioPlayParams params = OrcWeaponAudioBuildPlayParams(&ctx, gainScale, spatial, cls);
    if (delayMs == 0 && spatial == OrcWeaponSpatial::WorldAtPed &&
        OrcWeaponAudioIsBeyondMaxDistance(ctx.ped, params.att.maxDist))
        return true;

    return OrcWeaponAudioPlayBuffer(buffer, params, ctx.ped, delayMs);
}

bool OrcWeaponAudioIsInterior() {
    return CGame::currArea > 0;
}
