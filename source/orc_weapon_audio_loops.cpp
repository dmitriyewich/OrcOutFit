// Loop weapons (flamethrower, minigun, chainsaw, spray, extinguisher) — EarShotOpenAL 1.0 US RVAs.

#include "plugin.h"

#include "CAEAudioEntity.h"
#include "CAEWeaponAudioEntity.h"
#include "CAESound.h"
#include "CEntity.h"
#include "CPed.h"
#include "CPhysical.h"
#include "CWeapon.h"
#include "CPools.h"
#include "CTimer.h"
#include "eEntityType.h"
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
static constexpr uintptr_t kAddr_PlayMiniGunFireSounds = 0x5047C0;
static constexpr uintptr_t kAddr_PlayMiniGunStopSound = 0x504960;

static constexpr uintptr_t kCallSite_PlayWeaponLoopSoundA = 0x505196;
static constexpr uintptr_t kCallSite_PlayWeaponLoopSoundB = 0x5051CB;
static constexpr uintptr_t kCallSite_StopFlamethrowerFire = 0x504BD4;
static constexpr uintptr_t kCallSite_StopSpraycan = 0x504C4D;
static constexpr uintptr_t kCallSite_StopExtinguisher = 0x504C71;
static constexpr uintptr_t kCallSite_StopMinigunA = 0x504CEC;
static constexpr uintptr_t kCallSite_StopMinigunB = 0x504D22;
static constexpr uintptr_t kCallSite_StopChainsaw = 0x504D8F;
// GTA SA 1.0 US — CAESound::CalculateVolume (plugin-sdk). EarShot RVA 0x4F041A is not a call site on US 1.0.
static constexpr uintptr_t kAddr_CAESound_CalculateVolume = 0x4EFA10;

static constexpr int kChainsawStateStopping = 3;
static constexpr int kChainsawStateStopped = 4;

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
using PlayMiniGunFireSounds_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*, int);
using PlayMiniGunStopSound_t = void(__thiscall*)(CAEWeaponAudioEntity*, CPhysical*);
using CAESound_StopSoundAndForget_t = void(__thiscall*)(CAESound*);

static PlayFlameThrowerSounds_t g_PlayFlameThrowerSounds_Orig = nullptr;
static PlayFlameThrowerIdleGasLoop_t g_PlayFlameIdleGas_Orig = nullptr;
static StopFlameThrowerIdleGasLoop_t g_StopFlameIdleGas_Orig = nullptr;
static PlayWeaponLoopSound_t g_PlayWeaponLoopSound_Orig = nullptr;
static ReportChainsawEvent_t g_ReportChainsaw_Orig = nullptr;
static PlayMiniGunFireSounds_t g_PlayMiniGunFire_Orig = nullptr;
static PlayMiniGunStopSound_t g_PlayMiniGunStop_Orig = nullptr;
static CAESound_StopSoundAndForget_t g_StopSound_Orig = nullptr;

static bool g_loopHooksInstalled = false;
static bool g_loopRedirectsInstalled = false;

using PedLoops = std::array<ALuint, OrcLoop_Count>;
static std::unordered_map<CPed*, PedLoops> g_pedLoops;
static std::unordered_map<CPed*, DWORD> g_minigunFireHoldUntilTick;
static constexpr DWORD kMinigunFireHoldMs = 280;

using CAESound_CalculateVolume_t = void(__thiscall*)(CAESound*);
static CAESound_CalculateVolume_t g_CalculateVolume_Orig = nullptr;
static bool g_calculateVolumeHookInstalled = false;

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

static void OrcTouchMinigunFireHold(CPed* ped) {
    if (ped)
        g_minigunFireHoldUntilTick[ped] = GetTickCount() + kMinigunFireHoldMs;
}

static void OrcClearMinigunFireHold(CPed* ped) {
    if (ped)
        g_minigunFireHoldUntilTick.erase(ped);
}

static bool OrcPedHasMinigunEquipped(CPed* ped) {
    if (!ped)
        return false;
    const int slot = ped->m_nSelectedWepSlot;
    if (slot < 0 || slot >= 13)
        return false;
    return ped->m_aWeapons[slot].m_eWeaponType == WEAPONTYPE_MINIGUN;
}

bool OrcWeaponAudioPedIsMinigunFiring(CPed* ped) {
    if (!OrcPedHasMinigunEquipped(ped))
        return false;
    return ped->m_aWeapons[ped->m_nSelectedWepSlot].m_nState == WEAPONSTATE_FIRING;
}

bool OrcWeaponAudioPedHasCustomMinigunFireloop(CPed* ped) {
    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !ped)
        return false;
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(WEAPONTYPE_MINIGUN), ctx))
        return false;
    std::string tmp;
    return OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_minigun_fireloop", tmp);
}

static bool OrcPedIsMinigunFiring(CPed* ped) {
    return OrcWeaponAudioPedIsMinigunFiring(ped);
}

bool OrcWeaponAudioPedWantsMinigunFireLoop(CPed* ped) {
    if (OrcWeaponAudioPedIsMinigunFiring(ped))
        return true;
    const auto it = g_minigunFireHoldUntilTick.find(ped);
    return it != g_minigunFireHoldUntilTick.end() && GetTickCount() < it->second;
}

static bool OrcPedWantsMinigunFireLoop(CPed* ped) {
    return OrcWeaponAudioPedWantsMinigunFireLoop(ped);
}

static bool OrcTryStartLoopSuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, OrcLoopSlot slot) {
    std::string path;
    if (!OrcWeaponAudioResolveFirstExistingAudioPath(ctx, suffix, path))
        return false;
    if (!OrcWeaponAudioOpenAlInit()) {
        OrcLogError("weapon audio: OpenAL init failed for loop %s", path.c_str());
        return false;
    }
    const ALuint buf = OrcGetOrCreateBufferForPath(path.c_str());
    if (!buf) {
        OrcLogError("weapon audio: decode failed for loop %s", path.c_str());
        return false;
    }
    OrcWeaponAudioPlayParams lp = OrcWeaponAudioBuildPlayParams(&ctx, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed, OrcWeaponSoundClass::Loop);
    ALuint& src = OrcLoopRef(ctx.ped, slot);
    if (OrcWeaponAudioStartLoopSource(buf, lp, ctx.ped, src)) {
        OrcWeaponAudioMarkSuppressVanilla();
        return true;
    }
    return false;
}

static void OrcStopLoopSlot(CPed* ped, OrcLoopSlot slot);
static void OrcStopChainsawLoops(CPed* ped);

static CPed* OrcWeaponAudioResolvePedFromBase(CAEAudioEntity* base) {
    if (!base || !base->m_pEntity)
        return nullptr;
    CEntity* ent = base->m_pEntity;
    if ((static_cast<unsigned>(ent->m_nType) & 7u) != static_cast<unsigned>(ENTITY_TYPE_PED))
        return nullptr;
    CPed* ped = static_cast<CPed*>(ent);
    if (reinterpret_cast<uintptr_t>(ped) < 0x10000u)
        return nullptr;
    const int ref = CPools::GetPedRef(ped);
    if (ref < 0 || CPools::GetPed(ref) != ped)
        return nullptr;
    return ped;
}

static CPed* OrcWeaponAudioResolvePedFromWeaponAudio(CAEAudioEntity* base) {
    if (!base)
        return nullptr;
    return OrcWeaponAudioPedFromWeaponAudio(reinterpret_cast<CAEWeaponAudioEntity*>(base));
}

static bool OrcWeaponAudioCustomPathForVanillaSfx(int bank, int sfx, const char** outSuffix, OrcWeaponSoundClass* outClass) {
    if (!outSuffix || !outClass)
        return false;
    *outSuffix = nullptr;
    const int sfxId = sfx;
    const int bankId = bank;
    switch (sfxId) {
    case 28:
        if (bankId == 5) {
            *outSuffix = "_spraycan_sprayloop";
            *outClass = OrcWeaponSoundClass::Loop;
            return true;
        }
        break;
    case 9:
        if (bankId == 5) {
            *outSuffix = "_extinguisher_loop";
            *outClass = OrcWeaponSoundClass::Loop;
            return true;
        }
        break;
    case 10:
    case 83:
    case 26:
        if (bankId == 5 || bankId == 19) {
            if (sfxId == 26) {
                *outSuffix = "_flamethrower_fire";
                *outClass = OrcWeaponSoundClass::Loop;
                return true;
            }
            if (sfxId == 83) {
                *outSuffix = "_flamethrower_start";
                *outClass = OrcWeaponSoundClass::Shoot;
                return true;
            }
            if (sfxId == 10) {
                *outSuffix = "_flamethrower_idlegasloop";
                *outClass = OrcWeaponSoundClass::Loop;
                return true;
            }
        }
        break;
    case 14:
        if (bankId == 5) {
            *outSuffix = "_minigun_barrelspinloop";
            *outClass = OrcWeaponSoundClass::MinigunSpin;
            return true;
        }
        break;
    case 63:
        if (bankId == 5) {
            *outSuffix = "_minigun_barrelspinend";
            *outClass = OrcWeaponSoundClass::MinigunSpinEnd;
            return true;
        }
        break;
    case 15:
    case 16:
    case 11:
    case 12:
    case 13:
        if (bankId == 5) {
            *outSuffix = "_minigun_fireloop";
            *outClass = OrcWeaponSoundClass::Loop;
            return true;
        }
        break;
    case 1:
        if (bankId == 40) {
            *outSuffix = "_chainsaw_idle";
            *outClass = OrcWeaponSoundClass::Loop;
            return true;
        }
        break;
    case 0:
        if (bankId == 40) {
            *outSuffix = "_chainsaw_active";
            *outClass = OrcWeaponSoundClass::Loop;
            return true;
        }
        break;
    case 7:
    case 8:
        if (bankId == 3) {
            *outSuffix = "_chainsaw_cuttingflesh";
            *outClass = OrcWeaponSoundClass::Loop;
            return true;
        }
        break;
    case 2:
        if (bankId == 40) {
            *outSuffix = "_chainsaw_stop";
            *outClass = OrcWeaponSoundClass::ChainsawStop;
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

bool OrcWeaponAudioHandleVanillaSfx(CAESound* snd) {
    if (!snd || !g_weaponCustomSounds || !g_weaponReplacementEnabled)
        return false;

    const char* suffix = nullptr;
    OrcWeaponSoundClass cls = OrcWeaponSoundClass::Loop;
    if (!OrcWeaponAudioCustomPathForVanillaSfx(snd->m_nBankSlotId, snd->m_nSoundIdInSlot, &suffix, &cls))
        return false;

    CAEAudioEntity* base = snd->m_pBaseAudio;
    if (!base)
        return false;
    CPed* ped = OrcWeaponAudioResolvePedFromWeaponAudio(base);
    if (!ped)
        return false;

    int weaponType = static_cast<int>(WEAPONTYPE_UNARMED);
    if (ped->m_nSelectedWepSlot >= 0 && ped->m_nSelectedWepSlot < 13)
        weaponType = static_cast<int>(ped->m_aWeapons[ped->m_nSelectedWepSlot].m_eWeaponType);

    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, weaponType, ctx)) {
        static const eWeaponType kFallback[] = {WEAPONTYPE_MINIGUN, WEAPONTYPE_CHAINSAW, WEAPONTYPE_FTHROWER, WEAPONTYPE_SPRAYCAN,
            WEAPONTYPE_EXTINGUISHER};
        bool ok = false;
        for (eWeaponType wt : kFallback) {
            if (OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(wt), ctx)) {
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }

    std::string path;
    if (!OrcWeaponAudioResolveFirstExistingAudioPath(ctx, suffix, path))
        return false;

    if (strcmp(suffix, "_minigun_fireloop") == 0) {
        if (OrcPedIsMinigunFiring(ped)) {
            if (!OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_MinigunFire)))
                OrcTryStartLoopSuffix(ctx, suffix, OrcLoop_MinigunFire);
            OrcTouchMinigunFireHold(ped);
        } else if (!OrcPedWantsMinigunFireLoop(ped)) {
            OrcClearMinigunFireHold(ped);
            OrcStopLoopSlot(ped, OrcLoop_MinigunFire);
        }
        OrcWeaponAudioMarkSuppressVanilla();
        return true;
    }

    if (cls == OrcWeaponSoundClass::MinigunSpin) {
        (void)OrcTryStartLoopSuffix(ctx, suffix, OrcLoop_MinigunBarrelSpin);
        OrcWeaponAudioMarkSuppressVanilla();
        return true;
    }
    if (cls == OrcWeaponSoundClass::MinigunSpinEnd) {
        if (OrcWeaponAudioTryPlaySuffix(ctx, suffix, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed)) {
            OrcStopLoopSlot(ped, OrcLoop_MinigunBarrelSpin);
            OrcWeaponAudioMarkSuppressVanilla();
            return true;
        }
        return false;
    }
    if (cls == OrcWeaponSoundClass::ChainsawStop) {
        if (OrcWeaponAudioTryPlaySuffix(ctx, suffix, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed)) {
            OrcStopChainsawLoops(ped);
            OrcWeaponAudioMarkSuppressVanilla();
            return true;
        }
        return false;
    }
    if (cls == OrcWeaponSoundClass::Shoot) {
        if (OrcWeaponAudioTryPlaySuffix(ctx, suffix, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed)) {
            OrcWeaponAudioMarkSuppressVanilla();
            return true;
        }
        return false;
    }
    if (cls == OrcWeaponSoundClass::Loop) {
        struct Map {
            const char* suffix;
            OrcLoopSlot slot;
        };
        static const Map kMap[] = {
            {"_spraycan_sprayloop", OrcLoop_Spraycan},
            {"_extinguisher_loop", OrcLoop_Extinguisher},
            {"_flamethrower_fire", OrcLoop_FlameFire},
            {"_flamethrower_idlegasloop", OrcLoop_FlameGasIdle},
            {"_chainsaw_idle", OrcLoop_ChainsawIdle},
            {"_chainsaw_active", OrcLoop_ChainsawActive},
            {"_chainsaw_cuttingflesh", OrcLoop_ChainsawCutting},
        };
        for (const Map& m : kMap) {
            if (strcmp(suffix, m.suffix) == 0) {
                if (OrcTryStartLoopSuffix(ctx, suffix, m.slot)) {
                    OrcWeaponAudioMarkSuppressVanilla();
                    return true;
                }
                OrcLogInfoThrottled(402, 800, "weapon audio: loop start failed stem=%s suffix=%s path=%s", ctx.stem.c_str(), suffix,
                    path.c_str());
                return false;
            }
        }
        return false;
    }

    return false;
}

static void __fastcall CalculateVolume_Detour(CAESound* self, void* /*edx*/) {
    if (self && OrcWeaponAudioHandleVanillaSfx(self))
        return;
    if (g_CalculateVolume_Orig)
        g_CalculateVolume_Orig(self);
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
    if (g_weaponCustomSounds && g_weaponReplacementEnabled && self && self->m_pPed) {
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
    if (g_weaponCustomSounds && g_weaponReplacementEnabled && self && self->m_pPed) {
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
    if (g_PlayWeaponLoopSound_Orig)
        g_PlayWeaponLoopSound_Orig(self, entity, sfxId, audioEventId, audability, speed, finalEvent);

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !self)
        return;

    CPed* ped = OrcWeaponAudioPedFromPhysical(entity);
    if (!ped)
        ped = OrcWeaponAudioPedFromWeaponAudio(self);
    if (!ped)
        return;

    switch (sfxId) {
    case 28:
        if (!OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_Spraycan)))
            OrcTryStartLoopSuffixForPed(ped, static_cast<int>(WEAPONTYPE_SPRAYCAN), "_spraycan_sprayloop", OrcLoop_Spraycan);
        break;
    case 9:
        if (!OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_Extinguisher)))
            OrcTryStartLoopSuffixForPed(ped, static_cast<int>(WEAPONTYPE_EXTINGUISHER), "_extinguisher_loop", OrcLoop_Extinguisher);
        break;
    case 14:
        if (!OrcPedIsMinigunFiring(ped)) {
            OrcClearMinigunFireHold(ped);
            OrcStopLoopSlot(ped, OrcLoop_MinigunFire);
        }
        if (!OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_MinigunBarrelSpin)))
            OrcTryStartLoopSuffixForPed(ped, static_cast<int>(WEAPONTYPE_MINIGUN), "_minigun_barrelspinloop", OrcLoop_MinigunBarrelSpin);
        break;
    default:
        break;
    }
}

static void __fastcall PlayMiniGunFireSounds_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity, int audioEventId) {
    if (g_PlayMiniGunFire_Orig)
        g_PlayMiniGunFire_Orig(self, entity, audioEventId);

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !self)
        return;

    CPed* ped = OrcWeaponAudioPedFromPhysical(entity);
    if (!ped)
        ped = OrcWeaponAudioPedFromWeaponAudio(self);
    if (ped && OrcPedIsMinigunFiring(ped))
        OrcWeaponAudioLoopsTryPlayMinigunFireForPed(ped, self);
}

static void __fastcall PlayMiniGunStopSound_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPhysical* entity) {
    CPed* ped = nullptr;
    if (self) {
        ped = OrcWeaponAudioPedFromPhysical(entity);
        if (!ped)
            ped = OrcWeaponAudioPedFromWeaponAudio(self);
        if (ped) {
            OrcClearMinigunFireHold(ped);
            OrcWeaponAudioLoopsStopMinigunForPed(ped);
        }
    }
    if (g_PlayMiniGunStop_Orig)
        g_PlayMiniGunStop_Orig(self, entity);
    if (ped)
        OrcLogInfoThrottled(407, 800, "weapon audio: minigun stop");
}

static bool OrcVanillaSfxIsMinigunFireLoop(int bank, int sfx) {
    return bank == 5 && (sfx == 15 || sfx == 16 || sfx == 11 || sfx == 12 || sfx == 13);
}

static void OrcStopLoopSlotForVanillaSfx(CPed* ped, int bank, int sfx) {
    if (!ped)
        return;
    if (OrcVanillaSfxIsMinigunFireLoop(bank, sfx)) {
        if (OrcPedWantsMinigunFireLoop(ped))
            return;
        OrcClearMinigunFireHold(ped);
        OrcStopLoopSlot(ped, OrcLoop_MinigunFire);
        return;
    }
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

static void OrcStopLoopsByVanillaSfx(CAESound* snd) {
    if (!snd)
        return;
    const int sfx = snd->m_nSoundIdInSlot;
    const int bank = snd->m_nBankSlotId;
    if (bank != 5 && bank != 19 && bank != 40)
        return;

    CPed* ped = nullptr;
    if (snd->m_pBaseAudio)
        ped = OrcWeaponAudioResolvePedFromWeaponAudio(snd->m_pBaseAudio);
    if (ped) {
        OrcStopLoopSlotForVanillaSfx(ped, bank, sfx);
        return;
    }

    for (auto& kv : g_pedLoops)
        OrcStopLoopSlotForVanillaSfx(kv.first, bank, sfx);
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
    case kChainsawStateStopping:
    case kChainsawStateStopped:
        OrcWeaponAudioTryPlaySuffix(ctx, "_chainsaw_stop", OrcLoopGain(), OrcWeaponSpatial::WorldAtPed);
        OrcStopChainsawLoops(ped);
        break;
    default:
        break;
    }
}

static void __fastcall ReportChainsawEvent_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, CPed* ped, int aevent) {
    (void)aevent;
    if (g_ReportChainsaw_Orig)
        g_ReportChainsaw_Orig(self, ped, aevent);

    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !self || !ped)
        return;

    OrcUpdateChainsawLoops(self, ped);
}

bool OrcWeaponAudioLoopsTryPlayMinigunFireForPed(CPed* ped, CAEWeaponAudioEntity* audioEntity) {
    (void)audioEntity;
    if (!g_weaponCustomSounds || !g_weaponReplacementEnabled || !ped)
        return false;
    if (!OrcPedIsMinigunFiring(ped)) {
        return OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_MinigunFire));
    }
    if (OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_MinigunFire)))
        return true;
    const int wt = static_cast<int>(WEAPONTYPE_MINIGUN);
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, wt, ctx)) {
        OrcLogInfoThrottled(404, 1200, "weapon audio: minigun stem miss pedRef=%d", CPools::GetPedRef(ped));
        return false;
    }
    std::string tmp;
    if (!OrcWeaponAudioResolveFirstExistingAudioPath(ctx, "_minigun_fireloop", tmp)) {
        OrcLogInfoThrottled(405, 1200, "weapon audio: minigun fireloop file miss stem=%s dir=%s", ctx.stem.c_str(), ctx.dir.c_str());
        return false;
    }
    const bool wasPlaying = OrcWeaponAudioIsLoopSourcePlaying(OrcLoopRef(ped, OrcLoop_MinigunFire));
    if (!OrcTryStartLoopSuffix(ctx, "_minigun_fireloop", OrcLoop_MinigunFire))
        return false;
    OrcTouchMinigunFireHold(ped);
    if (!wasPlaying)
        OrcLogInfoThrottled(406, 500, "weapon audio: minigun OpenAL fireloop stem=%s path=%s", ctx.stem.c_str(), tmp.c_str());
    return true;
}

void OrcWeaponAudioLoopsStopMinigunForPed(CPed* ped) {
    OrcClearMinigunFireHold(ped);
    OrcStopMinigunLoops(ped);
}

static void OrcPruneMinigunLoopsNotFiring() {
    std::vector<CPed*> stop;
    stop.reserve(4);
    for (auto& kv : g_pedLoops) {
        CPed* ped = kv.first;
        if (!ped)
            continue;
        if (!kv.second[static_cast<size_t>(OrcLoop_MinigunFire)])
            continue;
        if (!OrcPedHasMinigunEquipped(ped) || !OrcPedWantsMinigunFireLoop(ped))
            stop.push_back(ped);
    }
    for (CPed* ped : stop) {
        OrcClearMinigunFireHold(ped);
        OrcStopMinigunLoops(ped);
    }
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
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_PlayMiniGunFireSounds), reinterpret_cast<void*>(&PlayMiniGunFireSounds_Detour),
            reinterpret_cast<void**>(&g_PlayMiniGunFire_Orig)) != MH_OK) {
        OrcLogError("weapon audio loops: MH_CreateHook PlayMiniGunFireSounds failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(kAddr_PlayMiniGunStopSound), reinterpret_cast<void*>(&PlayMiniGunStopSound_Detour),
            reinterpret_cast<void**>(&g_PlayMiniGunStop_Orig)) != MH_OK) {
        OrcLogError("weapon audio loops: MH_CreateHook PlayMiniGunStopSound failed");
        return;
    }
    if (!g_calculateVolumeHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(kAddr_CAESound_CalculateVolume),
                reinterpret_cast<void*>(&CalculateVolume_Detour), reinterpret_cast<void**>(&g_CalculateVolume_Orig)) != MH_OK) {
            OrcLogError("weapon audio loops: MH_CreateHook CAESound::CalculateVolume failed");
            return;
        }
        g_calculateVolumeHookInstalled = true;
    }

    MH_EnableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_PlayMiniGunFireSounds));
    MH_EnableHook(reinterpret_cast<void*>(kAddr_PlayMiniGunStopSound));
    if (g_calculateVolumeHookInstalled)
        MH_EnableHook(reinterpret_cast<void*>(kAddr_CAESound_CalculateVolume));
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
    OrcPruneMinigunLoopsNotFiring();
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
    g_minigunFireHoldUntilTick.clear();
    if (g_loopHooksInstalled) {
        MH_DisableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_PlayMiniGunFireSounds));
        MH_DisableHook(reinterpret_cast<void*>(kAddr_PlayMiniGunStopSound));
        if (g_calculateVolumeHookInstalled)
            MH_DisableHook(reinterpret_cast<void*>(kAddr_CAESound_CalculateVolume));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerSounds));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_PlayFlameThrowerIdleGasLoop));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_StopFlameThrowerIdleGasLoop));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_ReportChainsawEvent));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_PlayMiniGunFireSounds));
        MH_RemoveHook(reinterpret_cast<void*>(kAddr_PlayMiniGunStopSound));
        if (g_calculateVolumeHookInstalled) {
            MH_RemoveHook(reinterpret_cast<void*>(kAddr_CAESound_CalculateVolume));
            g_calculateVolumeHookInstalled = false;
            g_CalculateVolume_Orig = nullptr;
        }
        g_loopHooksInstalled = false;
        g_PlayFlameThrowerSounds_Orig = nullptr;
        g_PlayFlameIdleGas_Orig = nullptr;
        g_StopFlameIdleGas_Orig = nullptr;
        g_ReportChainsaw_Orig = nullptr;
        g_PlayMiniGunFire_Orig = nullptr;
        g_PlayMiniGunStop_Orig = nullptr;
    }
    g_loopRedirectsInstalled = false;
    g_PlayWeaponLoopSound_Orig = nullptr;
    g_StopSound_Orig = nullptr;
}
