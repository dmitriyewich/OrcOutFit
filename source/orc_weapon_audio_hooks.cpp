// MinHook detours: WeaponFire, WeaponReload, PlayGunSounds, HandlePedHit, HandlePedSwing.

#include "plugin.h"

#include "CAEWeaponAudioEntity.h"
#include "CAEPedAudioEntity.h"
#include "CGame.h"
#include "CMenuManager.h"
#include "CPed.h"
#include "CPhysical.h"
#include "CTimer.h"
#include "CVector.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "eAudioEvents.h"
#include "eEntityType.h"
#include "eWeaponType.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "external/MinHook/include/MinHook.h"

#include "Patch.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio.h"
#include "orc_weapon_audio_internal.h"

using namespace plugin;

class SurfaceInfos_c;

// GTA SA 1.0 US — EarShotOpenAL Loaders.cpp
static constexpr uintptr_t kAddr_CAEWeaponAudioEntity_WeaponFire = 0x504F80;
static constexpr uintptr_t kAddr_CAEWeaponAudioEntity_WeaponReload = 0x503690;
static constexpr uintptr_t kAddr_CAEPedAudioEntity_HandlePedHit = 0x4E1CC0;
static constexpr uintptr_t kAddr_CAEPedAudioEntity_HandlePedSwing = 0x4E1A40;
static constexpr uintptr_t kCallSite_PlayGunSounds = 0x50493D;

using CAEWeaponAudioEntity_WeaponFire_t = void(__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using CAEWeaponAudioEntity_WeaponReload_t = void(__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using CAEPedAudioEntity_HandlePedHit_t = void(__thiscall*)(CAEPedAudioEntity*, int, CPhysical*, uint8_t, float, uint32_t);
using CAEPedAudioEntity_HandlePedSwing_t = char(__thiscall*)(CAEPedAudioEntity*, int, int, int);
using CAEWeaponAudioEntity_PlayGunSounds_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*, short, short, short, short, short,
    int, float, float, float);

static CAEWeaponAudioEntity_WeaponFire_t g_WeaponFire_Orig = nullptr;
static CAEWeaponAudioEntity_WeaponReload_t g_WeaponReload_Orig = nullptr;
static CAEPedAudioEntity_HandlePedHit_t g_HandlePedHit_Orig = nullptr;
static CAEPedAudioEntity_HandlePedSwing_t g_HandlePedSwing_Orig = nullptr;
static CAEWeaponAudioEntity_PlayGunSounds_t g_PlayGunSounds_Orig = nullptr;

bool g_hooksInstalled = false;
bool g_hitHookInstalled = false;
bool g_swingHookInstalled = false;
bool g_playGunSoundsRedirectInstalled = false;
DWORD g_suppressVanillaGunSoundsUntilTick = 0;

static std::unordered_map<CPed*, DWORD> g_lastMinigunShootOnlyTick;

void OrcWeaponAudioHooksClearShootThrottleState() {
    g_lastMinigunShootOnlyTick.clear();
}

static SurfaceInfos_c& OrcSurfaceInfos() {
    return *reinterpret_cast<SurfaceInfos_c*>(0xB79538);
}

static bool OrcIsAudioMetal(uint32_t surfaceId) {
    return plugin::CallMethodAndReturn<bool, 0x55EAF0, SurfaceInfos_c*, uint32_t>(&OrcSurfaceInfos(), surfaceId);
}

static bool OrcIsAudioWood(uint32_t surfaceId) {
    return plugin::CallMethodAndReturn<bool, 0x55EAB0, SurfaceInfos_c*, uint32_t>(&OrcSurfaceInfos(), surfaceId);
}

static uintptr_t OrcResolveCallTarget(uintptr_t callRva) {
    if (patch::GetUChar(callRva) != 0xE8)
        return 0;
    const int32_t rel = static_cast<int32_t>(patch::GetUInt(callRva + 1));
    return static_cast<uintptr_t>(static_cast<ptrdiff_t>(callRva + 5) + rel);
}

static float OrcWeaponAudioCloseGain() {
    return std::max(0.0f, g_weaponCustomSoundGain);
}

static float OrcWeaponAudioDistantGain() {
    const float d = g_weaponCustomSoundDistantGain > 0.0f ? g_weaponCustomSoundDistantGain : g_weaponCustomSoundGain;
    return std::max(0.0f, d);
}

static bool OrcTryFirstExistingNumbered(const OrcWeaponAudioStemContext& ctx, const char* prefix, int maxAlt, float gain,
    OrcWeaponSpatial spatial, int* outIndex) {
    if (maxAlt <= 0)
        return false;
    const int start = rand() % maxAlt;
    for (int n = 0; n < maxAlt; ++n) {
        const int i = (start + n) % maxAlt;
        char suffix[32];
        sprintf_s(suffix, "%s%d", prefix, i);
        if (OrcWeaponAudioTryPlaySuffix(ctx, suffix, gain, spatial)) {
            if (outIndex)
                *outIndex = i;
            return true;
        }
    }
    return false;
}

static bool OrcTryDistantShot(const OrcWeaponAudioStemContext& ctx) {
    const float gain = OrcWeaponAudioDistantGain();
    if (OrcWeaponAudioTryPlaySuffix(ctx, "_distant", gain, OrcWeaponSpatial::WorldAtPed))
        return true;
    const int maxAlt = std::max(1, std::min(10, g_weaponCustomSoundMaxAlternatives));
    return OrcTryFirstExistingNumbered(ctx, "_distant", maxAlt, gain, OrcWeaponSpatial::WorldAtPed, nullptr);
}

static bool OrcTryAfterSuffix(const OrcWeaponAudioStemContext& ctx, const char* afterSuffix, float gain) {
    if (OrcWeaponAudioIsInterior())
        return false;
    return OrcWeaponAudioTryPlaySuffix(ctx, afterSuffix, gain, OrcWeaponSpatial::ListenerRelative);
}

static bool OrcTryCloseShoot(const OrcWeaponAudioStemContext& ctx) {
    const float gain = OrcWeaponAudioCloseGain();
    const int maxAlt = std::max(1, std::min(10, g_weaponCustomSoundMaxAlternatives));

    int shootIndex = -1;
    if (OrcTryFirstExistingNumbered(ctx, "_shoot", maxAlt, gain, OrcWeaponSpatial::ListenerRelative, &shootIndex)) {
        if (shootIndex >= 0) {
            char afterSuf[32];
            sprintf_s(afterSuf, "_after%d", shootIndex);
            OrcTryAfterSuffix(ctx, afterSuf, gain);
        }
        return true;
    }

    if (OrcWeaponAudioTryPlaySuffix(ctx, "_shoot", gain, OrcWeaponSpatial::ListenerRelative)) {
        OrcTryAfterSuffix(ctx, "_after", gain);
        return true;
    }
    return false;
}

// Minigun без _minigun_fireloop: WeaponFire вызывается десятки раз/с — без throttle заливает OpenAL _shoot.
static bool OrcWeaponAudioConsumeMinigunShootOnlyThrottle(const OrcWeaponAudioStemContext& ctx) {
    if (ctx.weaponType != WEAPONTYPE_MINIGUN || !ctx.ped)
        return false;
    std::string tmp;
    if (OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_minigun_fireloop", tmp))
        return false;
    if (!OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_shoot", tmp))
        return false;

    const DWORD now = GetTickCount();
    DWORD& last = g_lastMinigunShootOnlyTick[ctx.ped];
    if (last != 0 && now - last < 95u)
        return true;
    last = now;
    return false;
}

static bool OrcWeaponAudioTryCustomWeaponFire(CAEWeaponAudioEntity* self, eWeaponType weaponType) {
    if (!self || !self->m_pPed)
        return false;
    CPed* ped = self->m_pPed;
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(weaponType), ctx))
        return false;

    const float dist = OrcWeaponAudioCamPedDistance(ped);
    const bool farAway = dist >= g_weaponCustomSoundDistantThreshold;
    const bool close = !farAway;

    CWeapon* weap = ped->GetWeapon();
    CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weaponType, ped->GetWeaponSkill());

    if (close && weap && info && info->m_nAmmoClip > 0) {
        const unsigned ammo = weap->m_nAmmoInClip;
        const unsigned clip = info->m_nAmmoClip;
        const unsigned left = clip / 3;
        if (ammo < left) {
            if (OrcWeaponAudioTryPlaySuffix(ctx, "_low_ammo", OrcWeaponAudioCloseGain(), OrcWeaponSpatial::ListenerRelative))
                return true;
            if (ammo < 2) {
                if (OrcWeaponAudioTryPlaySuffix(ctx, "_dryfire", OrcWeaponAudioCloseGain(), OrcWeaponSpatial::ListenerRelative))
                    return true;
            }
        }
    }

    if (farAway) {
        if (OrcTryDistantShot(ctx))
            return true;
        return false;
    }

    return OrcTryCloseShoot(ctx);
}

static bool OrcWeaponAudioIsMeleeHitAudioEvent(int audioEvent) {
    switch (audioEvent) {
    case AE_PED_HIT_HIGH:
    case AE_PED_HIT_LOW:
    case AE_PED_HIT_GROUND:
    case AE_PED_HIT_GROUND_KICK:
    case AE_PED_HIT_HIGH_UNARMED:
    case AE_PED_HIT_LOW_UNARMED:
    case AE_PED_HIT_MARTIAL_PUNCH:
    case AE_PED_HIT_MARTIAL_KICK:
        return true;
    default:
        return false;
    }
}

static const char* OrcMeleeHitSuffix(int audioEvent, uint8_t surface) {
    switch (audioEvent) {
    case AE_PED_HIT_MARTIAL_PUNCH:
        return "_martial_punch";
    case AE_PED_HIT_MARTIAL_KICK:
        return "_martial_kick";
    case AE_PED_HIT_GROUND:
    case AE_PED_HIT_GROUND_KICK:
        return "_stomp";
    default:
        break;
    }
    if (OrcIsAudioMetal(surface))
        return "_hitmetal";
    if (OrcIsAudioWood(surface))
        return "_hitwood";
    return "_hit";
}

static bool OrcTryMeleeSuffixForPed(CPed* ped, const char* suffix, float gain) {
    if (!ped || !suffix)
        return false;
    const int slot = ped->m_nSelectedWepSlot;
    if (slot < 0 || slot >= 13)
        return false;
    const int wt = static_cast<int>(ped->m_aWeapons[slot].m_eWeaponType);
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, wt, ctx))
        return false;
    return OrcWeaponAudioTryPlaySuffix(ctx, suffix, gain, OrcWeaponSpatial::ListenerRelative);
}

static void __fastcall WeaponFire_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, eWeaponType weaponType, CPhysical* entity,
    int audioEventId) {
    (void)audioEventId;
    if (!g_WeaponFire_Orig)
        return;

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled) {
        g_WeaponFire_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    CPed* ped = OrcWeaponAudioPedFromPhysical(entity);
    if (!ped && self)
        ped = OrcWeaponAudioPedFromWeaponAudio(self);
    if (!ped || ped->m_nType != ENTITY_TYPE_PED) {
        g_WeaponFire_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    OrcWeaponAudioStemContext ctx;
    if (OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(weaponType), ctx)) {
        if (OrcWeaponAudioShouldSkipWeaponFireOneShot(static_cast<int>(weaponType), ctx)) {
            if (static_cast<int>(weaponType) == WEAPONTYPE_MINIGUN && OrcWeaponAudioPedHasCustomMinigunFireloop(ped)) {
                // Orig — PlayMiniGunFireSounds; OpenAL fireloop — явно (PlayGunSounds может не попасть в FIRING).
                g_WeaponFire_Orig(self, weaponType, entity, audioEventId);
                if (OrcWeaponAudioPedIsMinigunFiring(ped))
                    OrcWeaponAudioLoopsTryPlayMinigunFireForPed(ped, self);
                return;
            }
            return;
        }
        if (OrcWeaponAudioConsumeMinigunShootOnlyThrottle(ctx))
            return;
        if (OrcWeaponAudioTryCustomWeaponFire(self, weaponType))
            return;
    }

    g_WeaponFire_Orig(self, weaponType, entity, audioEventId);
}

static void __fastcall PlayGunSounds_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity, short emptySfxId,
    short farSfxId2, short highPitchSfxId3, short lowPitchSfxId4, short echoSfxId5, int nAudioEventId, float volumeChange,
    float speed1, float speed2) {
    if (!g_PlayGunSounds_Orig)
        return;
    if (g_weaponCustomSounds && g_weaponReplacementEnabled && self) {
        CPed* ped = OrcWeaponAudioPedFromPhysical(entity);
        if (!ped)
            ped = OrcWeaponAudioPedFromWeaponAudio(self);
        if (ped && OrcWeaponAudioPedHasCustomMinigunFireloop(ped) && OrcWeaponAudioPedIsMinigunFiring(ped)) {
            OrcWeaponAudioMarkSuppressVanilla();
            if (OrcWeaponAudioLoopsTryPlayMinigunFireForPed(ped, self))
                return;
            OrcLogInfoThrottled(408, 1200, "weapon audio: minigun PlayGunSounds OpenAL miss pedRef=%d — vanilla fallback",
                CPools::GetPedRef(ped));
        }
    }
    g_PlayGunSounds_Orig(self, entity, emptySfxId, farSfxId2, highPitchSfxId3, lowPitchSfxId4, echoSfxId5, nAudioEventId,
        volumeChange, speed1, speed2);
}

static const char* OrcReloadSuffixForEvent(int audioEventId) {
    switch (audioEventId) {
    case AE_WEAPON_RELOAD_A:
        return "_reload_one";
    case AE_WEAPON_RELOAD_B:
        return "_reload_two";
    default:
        return "_reload";
    }
}

static void __fastcall WeaponReload_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, eWeaponType weaponType, CPhysical* entity,
    int audioEventId) {
    if (!g_WeaponReload_Orig)
        return;

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled) {
        g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    CPed* ped = self ? self->m_pPed : nullptr;
    if (!ped || ped->m_nType != ENTITY_TYPE_PED) {
        g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(weaponType), ctx)) {
        g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    const float gain = OrcWeaponAudioCloseGain();
    const char* specific = OrcReloadSuffixForEvent(audioEventId);
    if (strcmp(specific, "_reload") != 0) {
        if (OrcWeaponAudioTryPlaySuffix(ctx, specific, gain, OrcWeaponSpatial::ListenerRelative))
            return;
    }
    if (OrcWeaponAudioTryPlaySuffix(ctx, "_reload", gain, OrcWeaponSpatial::ListenerRelative))
        return;

    g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
}

static void __fastcall HandlePedHit_Detour(CAEPedAudioEntity* self, void* /*edx*/, int audioEvent, CPhysical* victim,
    uint8_t surface, float volume, uint32_t maxVolume) {
    if (!g_HandlePedHit_Orig) {
        return;
    }

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled) {
        g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }

    CPed* ped = self ? self->m_pPed : nullptr;
    if (!ped || !OrcWeaponAudioIsMeleeHitAudioEvent(audioEvent)) {
        g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }

    if (OrcTryMeleeSuffixForPed(ped, OrcMeleeHitSuffix(audioEvent, surface), OrcWeaponAudioCloseGain()))
        return;

    g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
}

static char __fastcall HandlePedSwing_Detour(CAEPedAudioEntity* self, void* /*edx*/, int a2, int a3, int a4) {
    if (!g_HandlePedSwing_Orig)
        return 0;

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled) {
        return g_HandlePedSwing_Orig(self, a2, a3, a4);
    }

    CPed* ped = self ? self->m_pPed : nullptr;
    if (ped && OrcTryMeleeSuffixForPed(ped, "_swing", OrcWeaponAudioCloseGain()))
        return 1;

    return g_HandlePedSwing_Orig(self, a2, a3, a4);
}

void OrcWeaponAudioSetPluginModule(void* module) {
    g_pluginModule = reinterpret_cast<HMODULE>(module);
}

void OrcWeaponAudioEnsureHooksInstalled() {
    if (g_hooksInstalled)
        return;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("weapon audio hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponFire), reinterpret_cast<void*>(&WeaponFire_Detour),
            reinterpret_cast<void**>(&g_WeaponFire_Orig)) != MH_OK) {
        OrcLogError("weapon audio: MH_CreateHook WeaponFire failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponReload),
            reinterpret_cast<void*>(&WeaponReload_Detour), reinterpret_cast<void**>(&g_WeaponReload_Orig)) != MH_OK) {
        OrcLogError("weapon audio: MH_CreateHook WeaponReload failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedHit), reinterpret_cast<void*>(&HandlePedHit_Detour),
            reinterpret_cast<void**>(&g_HandlePedHit_Orig)) != MH_OK) {
        OrcLogError("weapon audio: MH_CreateHook HandlePedHit failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedSwing),
            reinterpret_cast<void*>(&HandlePedSwing_Detour), reinterpret_cast<void**>(&g_HandlePedSwing_Orig)) != MH_OK) {
        OrcLogError("weapon audio: MH_CreateHook HandlePedSwing failed");
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponFire));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook WeaponFire -> %s", MH_StatusToString(st));
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponReload));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook WeaponReload -> %s", MH_StatusToString(st));
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedHit));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook HandlePedHit -> %s", MH_StatusToString(st));
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedSwing));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook HandlePedSwing -> %s", MH_StatusToString(st));
    g_hitHookInstalled = true;
    g_swingHookInstalled = true;

    if (!g_playGunSoundsRedirectInstalled) {
        const uintptr_t playGunTarget = OrcResolveCallTarget(kCallSite_PlayGunSounds);
        if (playGunTarget) {
            g_PlayGunSounds_Orig = reinterpret_cast<CAEWeaponAudioEntity_PlayGunSounds_t>(playGunTarget);
            patch::RedirectCall(kCallSite_PlayGunSounds, reinterpret_cast<void*>(&PlayGunSounds_Detour));
            g_playGunSoundsRedirectInstalled = true;
            OrcLogInfo("weapon audio: PlayGunSounds redirect callSite=0x%08X target=0x%08X", (unsigned)kCallSite_PlayGunSounds,
                (unsigned)playGunTarget);
        } else {
            OrcLogError("weapon audio: PlayGunSounds call site 0x%08X is not E8", (unsigned)kCallSite_PlayGunSounds);
        }
    }

    OrcWeaponAudioLoopsEnsureInstalled();

    g_hooksInstalled = true;
    OrcLogInfo("weapon audio hooks installed");
}

void OrcWeaponAudioOnGameProcess() {
    if (!g_weaponCustomSounds || !OrcWeaponAudioHasActiveContext())
        return;

    if (!OrcWeaponAudioEnsureAlContextCurrent())
        return;

    const bool paused = FrontEndMenuManager.m_bMenuActive != false || CTimer::m_UserPause || CTimer::m_CodePause ||
                        CTimer::ms_fTimeScale <= 0.0f;

    if (paused) {
        OrcWeaponAudioStopEphemeralSources();
        OrcWeaponAudioLoopsStopAll();
        return;
    }

    OrcWeaponAudioPruneEphemeralSources();
    OrcWeaponAudioLoopsOnGameProcess();
}

void OrcWeaponAudioShutdown() {
    if (g_hooksInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponFire));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponReload));
        if (g_hitHookInstalled)
            MH_DisableHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedHit));
        if (g_swingHookInstalled)
            MH_DisableHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedSwing));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponFire));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponReload));
        if (g_hitHookInstalled)
            MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedHit));
        if (g_swingHookInstalled)
            MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedSwing));
        g_hooksInstalled = false;
        g_hitHookInstalled = false;
        g_swingHookInstalled = false;
        g_WeaponFire_Orig = nullptr;
        g_WeaponReload_Orig = nullptr;
        g_HandlePedHit_Orig = nullptr;
        g_HandlePedSwing_Orig = nullptr;
    }
    OrcWeaponAudioLoopsShutdown();
    OrcWeaponAudioOpenAlShutdown();
}
