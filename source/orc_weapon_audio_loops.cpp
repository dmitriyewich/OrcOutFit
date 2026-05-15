// Loop weapons (flamethrower, minigun, chainsaw, spray, extinguisher) — EarShotOpenAL 1.0 US RVAs.

#include "plugin.h"

#include "CAEWeaponAudioEntity.h"
#include "CAESound.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CTimer.h"
#include "eWeaponType.h"

#include <array>
#include <cstring>
#include <unordered_map>

#include "external/MinHook/include/MinHook.h"

#include "Patch.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_internal.h"

using namespace plugin;

// GTA SA 1.0 US — EarShotOpenAL Loaders.cpp
static constexpr uintptr_t kAddr_PlayFlameThrowerSounds = 0x504470;
static constexpr uintptr_t kAddr_PlayFlameThrowerIdleGasLoop = 0x503870;
static constexpr uintptr_t kAddr_StopFlameThrowerIdleGasLoop = 0x5034E0;
static constexpr uintptr_t kAddr_ReportChainsawEvent = 0x5039F0;

static constexpr uintptr_t kCallSite_PlayWeaponLoopSoundA = 0x505196;
static constexpr uintptr_t kCallSite_PlayWeaponLoopSoundB = 0x5051CB;
static constexpr uintptr_t kCallSite_StopFlamethrowerFire = 0x504BD4;
static constexpr uintptr_t kCallSite_StopSpraycan = 0x504C4D;
static constexpr uintptr_t kCallSite_StopExtinguisher = 0x504C71;
static constexpr uintptr_t kCallSite_StopMinigunA = 0x504CEC;
static constexpr uintptr_t kCallSite_StopMinigunB = 0x504D22;
static constexpr uintptr_t kCallSite_StopChainsaw = 0x504D8F;

enum OrcLoopSlot : int {
    OrcLoop_FlameFire = 0,
    OrcLoop_FlameGasIdle,
    OrcLoop_MinigunFire,
    OrcLoop_MinigunBarrelSpin,
    OrcLoop_ChainsawIdle,
    OrcLoop_ChainsawActive,
    OrcLoop_ChainsawCutting,
    OrcLoop_Spraycan,
    OrcLoop_Extinguisher,
    OrcLoop_Count,
};

static constexpr int kChainsawStateIdle = 0;
static constexpr int kChainsawStateActive = 1;
static constexpr int kChainsawStateCutting = 2;

using PlayFlameThrowerSounds_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*, short, short, int, float, float);
using PlayFlameThrowerIdleGasLoop_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*);
using StopFlameThrowerIdleGasLoop_t = void(__thiscall*)(CAEWeaponAudioEntity*);
using PlayWeaponLoopSound_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*, short, int, float, float, unsigned);
using ReportChainsawEvent_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPed*, int);
using CAESound_StopSoundAndForget_t = void(__thiscall*)(CAESound*);

static PlayFlameThrowerSounds_t g_PlayFlameThrowerSounds_Orig = nullptr;
static PlayFlameThrowerIdleGasLoop_t g_PlayFlameIdleGas_Orig = nullptr;
static StopFlameThrowerIdleGasLoop_t g_StopFlameIdleGas_Orig = nullptr;
static PlayWeaponLoopSound_t g_PlayWeaponLoopSound_Orig = nullptr;
static ReportChainsawEvent_t g_ReportChainsaw_Orig = nullptr;
static CAESound_StopSoundAndForget_t g_StopSound_Orig = nullptr;

static bool g_loopHooksInstalled = false;
static bool g_loopRedirectsInstalled = false;

using PedLoops = std::array<ALuint, OrcLoop_Count>;
static std::unordered_map<CPed*, PedLoops> g_pedLoops;

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

static PedLoops& OrcPedLoops(CPed* ped) {
    return g_pedLoops[ped];
}

static ALuint& OrcLoopRef(CPed* ped, OrcLoopSlot slot) {
    return OrcPedLoops(ped)[static_cast<size_t>(slot)];
}

static float OrcLoopGain() {
    return std::max(0.0f, g_weaponCustomSoundGain);
}

static bool OrcTryStartLoopSuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, OrcLoopSlot slot) {
    std::string path;
    if (!OrcWeaponAudioResolveFirstExistingAudioPath(ctx, suffix, path))
        return false;
    if (!OrcWeaponAudioOpenAlInit())
        return false;
    const ALuint buf = OrcGetOrCreateBufferForPath(path.c_str());
    if (!buf)
        return false;
    OrcWeaponAudioPlayParams lp = OrcWeaponAudioBuildPlayParams(&ctx, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed, OrcWeaponSoundClass::Loop);
    ALuint& src = OrcLoopRef(ctx.ped, slot);
    if (OrcWeaponAudioStartLoopSource(buf, lp, ctx.ped, src)) {
        OrcWeaponAudioMarkSuppressVanilla();
        return true;
    }
    return false;
}

static bool OrcTryStartLoopSuffixForPed(CPed* ped, int weaponType, const char* suffix, OrcLoopSlot slot) {
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, weaponType, ctx))
        return false;
    return OrcTryStartLoopSuffix(ctx, suffix, slot);
}

void OrcWeaponAudioLoopsStopForPed(CPed* ped) {
    if (!ped)
        return;
    auto it = g_pedLoops.find(ped);
    if (it == g_pedLoops.end())
        return;
    for (ALuint& src : it->second)
        OrcWeaponAudioStopLoopSource(src);
    g_pedLoops.erase(it);
}

void OrcWeaponAudioLoopsStopAll() {
    for (auto& kv : g_pedLoops) {
        for (ALuint& src : kv.second)
            OrcWeaponAudioStopLoopSource(src);
    }
    g_pedLoops.clear();
}

static void OrcStopLoopSlot(CPed* ped, OrcLoopSlot slot) {
    if (!ped)
        return;
    auto it = g_pedLoops.find(ped);
    if (it == g_pedLoops.end())
        return;
    OrcWeaponAudioStopLoopSource(it->second[static_cast<size_t>(slot)]);
}

static void OrcStopFlamethrowerLoops(CPed* ped) {
    OrcStopLoopSlot(ped, OrcLoop_FlameFire);
    OrcStopLoopSlot(ped, OrcLoop_FlameGasIdle);
}

static void OrcStopMinigunLoops(CPed* ped) {
    OrcStopLoopSlot(ped, OrcLoop_MinigunFire);
    OrcStopLoopSlot(ped, OrcLoop_MinigunBarrelSpin);
}

static void OrcStopChainsawLoops(CPed* ped) {
    OrcStopLoopSlot(ped, OrcLoop_ChainsawIdle);
    OrcStopLoopSlot(ped, OrcLoop_ChainsawActive);
    OrcStopLoopSlot(ped, OrcLoop_ChainsawCutting);
}

static void __fastcall PlayFlameThrowerSounds_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity, short sfx1,
    short sfx2, int audioEventId, float audability, float speed) {
    (void)entity;
    (void)audioEventId;
    (void)audability;
    (void)speed;

    bool handled = false;
    if (g_weaponCustomSounds && g_weaponReplacementEnabled && self && self->m_pPed && OrcIsLocalPlayerPed(self->m_pPed)) {
        const int wt = static_cast<int>(WEAPONTYPE_FTHROWER);
        OrcWeaponAudioStemContext ctx;
        if (OrcWeaponAudioTryBuildStemContext(self->m_pPed, wt, ctx)) {
            if (sfx1 == 83 && OrcWeaponAudioTryPlaySuffix(ctx, "_flamethrower_start", OrcLoopGain(), OrcWeaponSpatial::WorldAtPed))
                handled = true;
            if (sfx2 == 26 && OrcTryStartLoopSuffix(ctx, "_flamethrower_fire", OrcLoop_FlameFire))
                handled = true;
        }
    }

    if (!handled && g_PlayFlameThrowerSounds_Orig)
        g_PlayFlameThrowerSounds_Orig(self, entity, sfx1, sfx2, audioEventId, audability, speed);
}

static void __fastcall PlayFlameIdleGas_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity) {
    bool handled = false;
    if (g_weaponCustomSounds && g_weaponReplacementEnabled && self && self->m_pPed && OrcIsLocalPlayerPed(self->m_pPed)) {
        if (OrcTryStartLoopSuffixForPed(self->m_pPed, static_cast<int>(WEAPONTYPE_FTHROWER), "_flamethrower_idlegasloop",
                OrcLoop_FlameGasIdle))
            handled = true;
    }
    if (!handled && g_PlayFlameIdleGas_Orig)
        g_PlayFlameIdleGas_Orig(self, entity);
}

static void __fastcall StopFlameIdleGas_Detour(CAEWeaponAudioEntity* self, void* /*edx*/) {
    if (self && self->m_pPed)
        OrcStopLoopSlot(self->m_pPed, OrcLoop_FlameGasIdle);
    if (g_StopFlameIdleGas_Orig)
        g_StopFlameIdleGas_Orig(self);
}

static void __fastcall PlayWeaponLoopSound_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity, short sfxId,
    int audioEventId, float audability, float speed, unsigned finalEvent) {
  bool custom = false;
    if (g_weaponCustomSounds && g_weaponReplacementEnabled && self && self->m_pPed && OrcIsLocalPlayerPed(self->m_pPed)) {
        if (sfxId == 28) {
            custom = OrcTryStartLoopSuffixForPed(self->m_pPed, static_cast<int>(WEAPONTYPE_SPRAYCAN), "_spraycan_sprayloop",
                OrcLoop_Spraycan);
        } else if (sfxId == 9) {
            custom = OrcTryStartLoopSuffixForPed(self->m_pPed, static_cast<int>(WEAPONTYPE_EXTINGUISHER), "_extinguisher_loop",
                OrcLoop_Extinguisher);
        }
    }

    if (!custom && g_PlayWeaponLoopSound_Orig)
        g_PlayWeaponLoopSound_Orig(self, entity, sfxId, audioEventId, audability, speed, finalEvent);
}

static void OrcStopLoopsByVanillaSfx(CAESound* snd) {
    if (!snd)
        return;
    const int sfx = snd->m_nSoundIdInSlot;
    const int bank = snd->m_nBankSlotId;

    for (auto& kv : g_pedLoops) {
        CPed* ped = kv.first;
        if (!ped)
            continue;

        if (bank == 5 && (sfx == 15 || sfx == 16 || sfx == 11 || sfx == 12 || sfx == 13))
            OrcStopLoopSlot(ped, OrcLoop_MinigunFire);
        if (bank == 5 && (sfx == 14 || sfx == 63))
            OrcStopLoopSlot(ped, OrcLoop_MinigunBarrelSpin);
        if (bank == 5 && sfx == 28)
            OrcStopLoopSlot(ped, OrcLoop_Spraycan);
        if (bank == 5 && sfx == 9)
            OrcStopLoopSlot(ped, OrcLoop_Extinguisher);
        if ((bank == 5 || bank == 19) && sfx == 26)
            OrcStopLoopSlot(ped, OrcLoop_FlameFire);
        if (bank == 40 && sfx == 1)
            OrcStopLoopSlot(ped, OrcLoop_ChainsawIdle);
        if (bank == 40 && sfx == 0) {
            OrcStopLoopSlot(ped, OrcLoop_ChainsawActive);
            OrcStopLoopSlot(ped, OrcLoop_ChainsawCutting);
        }
    }
}

static void __fastcall StopVanillaSound_Detour(CAESound* self, void* /*edx*/) {
    OrcStopLoopsByVanillaSfx(self);
    if (g_StopSound_Orig)
        g_StopSound_Orig(self);
}

static void OrcUpdateChainsawLoops(CAEWeaponAudioEntity* self, CPed* ped) {
    if (!self || !ped)
        return;
    const int wt = static_cast<int>(WEAPONTYPE_CHAINSAW);
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, wt, ctx))
        return;

    const int st = self->m_nChainsawSoundState;
    if (st != kChainsawStateCutting)
        OrcStopLoopSlot(ped, OrcLoop_ChainsawCutting);

    switch (st) {
    case kChainsawStateIdle:
        OrcTryStartLoopSuffix(ctx, "_chainsaw_idle", OrcLoop_ChainsawIdle);
        break;
    case kChainsawStateActive:
        OrcTryStartLoopSuffix(ctx, "_chainsaw_active", OrcLoop_ChainsawActive);
        break;
    case kChainsawStateCutting:
        OrcTryStartLoopSuffix(ctx, "_chainsaw_cuttingflesh", OrcLoop_ChainsawCutting);
        break;
    default:
        break;
    }
}

static void __fastcall ReportChainsawEvent_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPed* ped, int aevent) {
    (void)aevent;
    if (g_ReportChainsaw_Orig)
        g_ReportChainsaw_Orig(self, ped, aevent);

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !self || !ped || !OrcIsLocalPlayerPed(ped))
        return;

    OrcUpdateChainsawLoops(self, ped);
}

void OrcWeaponAudioLoopsOnPlayGunSounds(CAEWeaponAudioEntity* self) {
    if (!self || !self->m_pPed || !OrcIsLocalPlayerPed(self->m_pPed))
        return;
    OrcTryStartLoopSuffixForPed(self->m_pPed, static_cast<int>(WEAPONTYPE_MINIGUN), "_minigun_fireloop", OrcLoop_MinigunFire);
}

bool OrcWeaponAudioShouldSkipWeaponFireOneShot(int weaponType, const OrcWeaponAudioStemContext& ctx) {
    switch (weaponType) {
    case WEAPONTYPE_MINIGUN: {
        std::string tmp;
        return OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_minigun_fireloop", tmp);
    }
    case WEAPONTYPE_FTHROWER: {
        std::string tmp;
        return OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_flamethrower_fire", tmp);
    }
    case WEAPONTYPE_CHAINSAW:
        return OrcWeaponAudioHasLoopCustomAudio(ctx);
    case WEAPONTYPE_SPRAYCAN: {
        std::string tmp;
        return OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_spraycan_sprayloop", tmp);
    }
    case WEAPONTYPE_EXTINGUISHER: {
        std::string tmp;
        return OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_extinguisher_loop", tmp);
    }
    default:
        return false;
    }
}

void OrcWeaponAudioLoopsEnsureInstalled() {
    if (g_loopHooksInstalled)
        return;

    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds), reinterpret_cast<void*>(&PlayFlameThrowerSounds_Detour),
            reinterpret_cast<void**>(&g_PlayFlameThrowerSounds_Orig)) != MH_OK) {
        OrcLogError("weapon audio loops: MH_CreateHook PlayFlameThrowerSounds failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop),
            reinterpret_cast<void*>(&PlayFlameIdleGas_Detour), reinterpret_cast<void**>(&g_PlayFlameIdleGas_Orig)) != MH_OK) {
        OrcLogError("weapon audio loops: MH_CreateHook PlayFlameThrowerIdleGasLoop failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop), reinterpret_cast<void*>(&StopFlameIdleGas_Detour),
            reinterpret_cast<void**>(&g_StopFlameIdleGas_Orig)) != MH_OK) {
        OrcLogError("weapon audio loops: MH_CreateHook StopFlameThrowerIdleGasLoop failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent), reinterpret_cast<void*>(&ReportChainsawEvent_Detour),
            reinterpret_cast<void**>(&g_ReportChainsaw_Orig)) != MH_OK) {
        OrcLogError("weapon audio loops: MH_CreateHook ReportChainsawEvent failed");
        return;
    }

    MH_EnableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent));
    g_loopHooksInstalled = true;

    if (!g_loopRedirectsInstalled) {
        const uintptr_t loopTarget = OrcResolveCallTarget(kCallSite_PlayWeaponLoopSoundA);
        if (loopTarget) {
            g_PlayWeaponLoopSound_Orig = reinterpret_cast<PlayWeaponLoopSound_t>(loopTarget);
            patch::RedirectCall(kCallSite_PlayWeaponLoopSoundA, reinterpret_cast<void*>(&PlayWeaponLoopSound_Detour));
            patch::RedirectCall(kCallSite_PlayWeaponLoopSoundB, reinterpret_cast<void*>(&PlayWeaponLoopSound_Detour));
        } else {
            OrcLogError("weapon audio loops: PlayWeaponLoopSound call site 0x%08X is not E8", (unsigned)kCallSite_PlayWeaponLoopSoundA);
        }

        const uintptr_t stopTarget = OrcResolveCallTarget(kCallSite_StopFlamethrowerFire);
        if (stopTarget) {
            g_StopSound_Orig = reinterpret_cast<CAESound_StopSoundAndForget_t>(stopTarget);
            patch::RedirectCall(kCallSite_StopFlamethrowerFire, reinterpret_cast<void*>(&StopVanillaSound_Detour));
            patch::RedirectCall(kCallSite_StopSpraycan, reinterpret_cast<void*>(&StopVanillaSound_Detour));
            patch::RedirectCall(kCallSite_StopExtinguisher, reinterpret_cast<void*>(&StopVanillaSound_Detour));
            patch::RedirectCall(kCallSite_StopMinigunA, reinterpret_cast<void*>(&StopVanillaSound_Detour));
            patch::RedirectCall(kCallSite_StopMinigunB, reinterpret_cast<void*>(&StopVanillaSound_Detour));
            patch::RedirectCall(kCallSite_StopChainsaw, reinterpret_cast<void*>(&StopVanillaSound_Detour));
            g_loopRedirectsInstalled = true;
        } else {
            OrcLogError("weapon audio loops: stop sound call site 0x%08X is not E8", (unsigned)kCallSite_StopFlamethrowerFire);
        }
    }

    if (g_loopHooksInstalled && g_loopRedirectsInstalled)
        OrcLogInfo("weapon audio loop hooks installed");
}

void OrcWeaponAudioLoopsOnGameProcess() {
    if (!g_weaponCustomSounds || !OrcWeaponAudioHasActiveContext())
        return;
    if (!OrcWeaponAudioEnsureAlContextCurrent())
        return;
    const float gain = OrcLoopGain();
    for (auto& kv : g_pedLoops) {
        CPed* ped = kv.first;
        if (!ped)
            continue;
        for (ALuint src : kv.second) {
            if (src)
                OrcWeaponAudioSyncLoopSourceWorldPos(src, ped, gain);
        }
    }
    OrcWeaponAudioUpdateLoopSources();
}

void OrcWeaponAudioLoopsShutdown() {
    g_pedLoops.clear();
    if (g_loopHooksInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent));
        g_loopHooksInstalled = false;
        g_PlayFlameThrowerSounds_Orig = nullptr;
        g_PlayFlameIdleGas_Orig = nullptr;
        g_StopFlameIdleGas_Orig = nullptr;
        g_ReportChainsaw_Orig = nullptr;
    }
    g_loopRedirectsInstalled = false;
    g_PlayWeaponLoopSound_Orig = nullptr;
    g_StopSound_Orig = nullptr;
}
