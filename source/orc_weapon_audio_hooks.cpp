// MinHook detours: WeaponFire, WeaponReload, PlayGunSounds, HandlePedHit.

#include "plugin.h"

#include "CAEWeaponAudioEntity.h"
#include "CAEPedAudioEntity.h"
#include "CGame.h"
#include "CMenuManager.h"
#include "CPed.h"
#include "CPlayerPed.h"
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

#include "external/MinHook/include/MinHook.h"

#include "Patch.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio.h"
#include "orc_weapon_audio_internal.h"

using namespace plugin;

// GTA SA 1.0 US — EarShotOpenAL Loaders.cpp
static constexpr uintptr_t kAddr_CAEWeaponAudioEntity_WeaponFire = 0x504F80;
static constexpr uintptr_t kAddr_CAEWeaponAudioEntity_WeaponReload = 0x503690;
static constexpr uintptr_t kAddr_CAEPedAudioEntity_HandlePedHit = 0x4E1CC0;
static constexpr uintptr_t kCallSite_PlayGunSounds = 0x50493D;

using CAEWeaponAudioEntity_WeaponFire_t = void(__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using CAEWeaponAudioEntity_WeaponReload_t = void(__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using CAEPedAudioEntity_HandlePedHit_t = void(__thiscall*)(CAEPedAudioEntity*, int, CPhysical*, uint8_t, float, uint32_t);
using CAEWeaponAudioEntity_PlayGunSounds_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*, short, short, short, short, short,
    int, float, float, float);

static CAEWeaponAudioEntity_WeaponFire_t g_WeaponFire_Orig = nullptr;
static CAEWeaponAudioEntity_WeaponReload_t g_WeaponReload_Orig = nullptr;
static CAEPedAudioEntity_HandlePedHit_t g_HandlePedHit_Orig = nullptr;
static CAEWeaponAudioEntity_PlayGunSounds_t g_PlayGunSounds_Orig = nullptr;

bool g_hooksInstalled = false;
bool g_hitHookInstalled = false;
bool g_playGunSoundsRedirectInstalled = false;
DWORD g_suppressVanillaGunSoundsUntilTick = 0;

static bool OrcIsLocalPlayerPed(CPed* ped) {
    CPlayerPed* pl = FindPlayerPed(0);
    return pl && ped == pl;
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

static void __fastcall WeaponFire_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, eWeaponType weaponType, CPhysical* victim,
    int audioEventId) {
    (void)victim;
    (void)audioEventId;
    if (!g_WeaponFire_Orig)
        return;

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled) {
        g_WeaponFire_Orig(self, weaponType, victim, audioEventId);
        return;
    }

    CPed* ped = self ? self->m_pPed : nullptr;
    if (!ped || ped->m_nType != ENTITY_TYPE_PED || !OrcIsLocalPlayerPed(ped)) {
        g_WeaponFire_Orig(self, weaponType, victim, audioEventId);
        return;
    }

    OrcWeaponAudioStemContext ctx;
    if (OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(weaponType), ctx)) {
        if (OrcWeaponAudioShouldSkipWeaponFireOneShot(static_cast<int>(weaponType), ctx))
            return;
        if (OrcWeaponAudioTryCustomWeaponFire(self, weaponType))
            return;
    } else if (OrcWeaponAudioTryCustomWeaponFire(self, weaponType)) {
        return;
    }

    g_WeaponFire_Orig(self, weaponType, victim, audioEventId);
}

static void __fastcall PlayGunSounds_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity, short emptySfxId,
    short farSfxId2, short highPitchSfxId3, short lowPitchSfxId4, short echoSfxId5, int nAudioEventId, float volumeChange,
    float speed1, float speed2) {
    (void)entity;
    (void)emptySfxId;
    (void)farSfxId2;
    (void)highPitchSfxId3;
    (void)lowPitchSfxId4;
    (void)echoSfxId5;
    (void)nAudioEventId;
    (void)volumeChange;
    (void)speed1;
    (void)speed2;
    if (!g_PlayGunSounds_Orig)
        return;
    if (OrcWeaponAudioShouldSuppressVanillaGun(self)) {
        OrcWeaponAudioLoopsOnPlayGunSounds(self);
        OrcLogInfoThrottled(401, 500, "weapon audio: suppress vanilla PlayGunSounds");
        return;
    }
    g_PlayGunSounds_Orig(self, entity, emptySfxId, farSfxId2, highPitchSfxId3, lowPitchSfxId4, echoSfxId5, nAudioEventId,
        volumeChange, speed1, speed2);
    OrcWeaponAudioLoopsOnPlayGunSounds(self);
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
    if (!ped || ped->m_nType != ENTITY_TYPE_PED || !OrcIsLocalPlayerPed(ped)) {
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
    if (!ped || !OrcIsLocalPlayerPed(ped) || !OrcWeaponAudioIsMeleeHitAudioEvent(audioEvent)) {
        g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }

  // Приклад / удар оружием по плоти (упрощённо: жертва — ped).
    if (!victim || victim->m_nType != ENTITY_TYPE_PED) {
        g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }

    const int slot = ped->m_nSelectedWepSlot;
    if (slot < 0 || slot >= 13) {
        g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }
    const int wt = static_cast<int>(ped->m_aWeapons[slot].m_eWeaponType);

    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, wt, ctx)) {
        g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }

    if (OrcWeaponAudioTryPlaySuffix(ctx, "_hit", OrcWeaponAudioCloseGain(), OrcWeaponSpatial::ListenerRelative))
        return;

    g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, maxVolume);
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
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponFire));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook WeaponFire -> %s", MH_StatusToString(st));
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponReload));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook WeaponReload -> %s", MH_StatusToString(st));
    st = MH_EnableHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedHit));
    if (st != MH_OK)
        OrcLogError("weapon audio: MH_EnableHook HandlePedHit -> %s", MH_StatusToString(st));
    g_hitHookInstalled = true;

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
        OrcWeaponAudioStopAllLoopSources();
        OrcWeaponAudioLoopsStopForPed(FindPlayerPed(0));
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
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponFire));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEWeaponAudioEntity_WeaponReload));
        if (g_hitHookInstalled)
            MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAEPedAudioEntity_HandlePedHit));
        g_hooksInstalled = false;
        g_hitHookInstalled = false;
        g_WeaponFire_Orig = nullptr;
        g_WeaponReload_Orig = nullptr;
        g_HandlePedHit_Orig = nullptr;
    }
    OrcWeaponAudioLoopsShutdown();
    OrcWeaponAudioOpenAlShutdown();
}
