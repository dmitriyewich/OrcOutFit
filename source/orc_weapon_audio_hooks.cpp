// Weapon audio events from GTA SA 1.0 US.

#include "plugin.h"

#include "CAEWeaponAudioEntity.h"
#include "CAEPedAudioEntity.h"
#include "CMenuManager.h"
#include "CPed.h"
#include "CPhysical.h"
#include "CTimer.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "eAudioEvents.h"
#include "eSurfaceType.h"
#include "eWeaponType.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "external/MinHook/include/MinHook.h"


#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio.h"
#include "orc_weapon_audio_internal.h"

using namespace plugin;

class SurfaceInfos_c;

namespace {

using CAEWeaponAudioEntity_WeaponFire_t = void(__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using CAEWeaponAudioEntity_WeaponReload_t = void(__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using CAEPedAudioEntity_HandlePedHit_t = void(__thiscall*)(CAEPedAudioEntity*, int, CPhysical*, uint8_t, float, uint32_t);
using CAEPedAudioEntity_HandlePedSwing_t = void(__thiscall*)(CAEPedAudioEntity*, int, int, uint32_t);

CAEWeaponAudioEntity_WeaponFire_t g_WeaponFire_Orig = nullptr;
CAEWeaponAudioEntity_WeaponFire_t g_ArizonaWeaponFire_Orig = nullptr;
CAEWeaponAudioEntity_WeaponReload_t g_WeaponReload_Orig = nullptr;
CAEPedAudioEntity_HandlePedHit_t g_HandlePedHit_Orig = nullptr;
CAEPedAudioEntity_HandlePedSwing_t g_HandlePedSwing_Orig = nullptr;

DWORD g_gameThreadId = 0;
bool g_hooksInstallAttempted = false;
bool g_forwardingArizonaFire = false;
HMODULE g_arizonaCore = nullptr;
ULONGLONG g_nextArizonaProbe = 0;

struct MinigunShot {
    int pedRef = 0;
    const WeaponReplacementAsset* asset = nullptr;
    DWORD tick = 0;
};
std::vector<MinigunShot> g_minigunShots;

bool IsPaused() {
    return FrontEndMenuManager.m_bMenuActive || CTimer::m_UserPause || CTimer::m_CodePause || CTimer::ms_fTimeScale <= 0.0f;
}

bool IsCustomAudioEnabled() {
    return g_enabled && g_weaponCustomSounds && g_weaponReplacementEnabled && !OrcIsRuntimeShuttingDown();
}

}  // namespace

bool OrcWeaponAudioCanPlayCustom() {
    return g_gameThreadId == GetCurrentThreadId() && IsCustomAudioEnabled() && !IsPaused();
}

void OrcWeaponAudioHooksClearShootThrottleState() {
    g_minigunShots.clear();
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

static float OrcWeaponAudioCloseGain() {
    return std::max(0.0f, g_weaponCustomSoundGain);
}

static float OrcWeaponAudioDistantGain() {
    const float d = g_weaponCustomSoundDistantGain > 0.0f ? g_weaponCustomSoundDistantGain : g_weaponCustomSoundGain;
    return std::max(0.0f, d);
}

static bool OrcTryFirstExistingNumbered(const OrcWeaponAudioStemContext& ctx, const char* prefix, int maxAlt, float gain,
    OrcWeaponSpatial spatial, int* outIndex) {
    const uint16_t variants = OrcWeaponAudioNumberedVariants(ctx, prefix, maxAlt);
    if (variants == 0)
        return false;
    int indices[10];
    int count = 0;
    for (int i = 0; i < maxAlt; ++i) {
        if (variants & (1u << i))
            indices[count++] = i;
    }
    for (int remaining = count; remaining > 0; --remaining) {
        const int next = rand() % remaining;
        const int i = indices[next];
        indices[next] = indices[remaining - 1];
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
    const int maxAlt = std::clamp(g_weaponCustomSoundMaxAlternatives, 1, 10);
    return OrcTryFirstExistingNumbered(ctx, "_distant", maxAlt, gain, OrcWeaponSpatial::WorldAtPed, nullptr);
}

static bool OrcTryShoot(const OrcWeaponAudioStemContext& ctx, OrcWeaponSpatial spatial) {
    const float gain = OrcWeaponAudioCloseGain();
    const int maxAlt = std::clamp(g_weaponCustomSoundMaxAlternatives, 1, 10);

    int shootIndex = -1;
    if (OrcTryFirstExistingNumbered(ctx, "_shoot", maxAlt, gain, spatial, &shootIndex)) {
        if (!OrcWeaponAudioIsInterior()) {
            char afterSuf[32];
            sprintf_s(afterSuf, "_after%d", shootIndex);
            if (!OrcWeaponAudioTryPlaySuffix(ctx, afterSuf, gain, spatial))
                OrcWeaponAudioTryPlaySuffix(ctx, "_after", gain, spatial);
        }
        return true;
    }

    if (OrcWeaponAudioTryPlaySuffix(ctx, "_shoot", gain, spatial)) {
        if (!OrcWeaponAudioIsInterior())
            OrcWeaponAudioTryPlaySuffix(ctx, "_after", gain, spatial);
        return true;
    }
    return false;
}

static bool OrcTryAmmoShot(const OrcWeaponAudioStemContext& ctx, eWeaponType weaponType, OrcWeaponSpatial spatial) {
    CPed* ped = ctx.ped;
    const unsigned slot = ped->m_nSelectedWepSlot;
    // Read weapon.dat clip size only for vanilla guns with the matching selected slot.
    if (weaponType < WEAPONTYPE_PISTOL || weaponType > WEAPONTYPE_SNIPERRIFLE ||
        slot >= std::size(ped->m_aWeapons))
        return false;
    const CWeapon& weapon = ped->m_aWeapons[slot];
    if (weapon.m_eWeaponType != weaponType || weapon.m_nAmmoInClip == 0)
        return false;
    const CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weaponType, ped->GetWeaponSkill());
    if (!info || info->m_nAmmoClip == 0)
        return false;

    // Fire is reported before GTA decrements the current clip.
    const unsigned ammoLeft = weapon.m_nAmmoInClip - 1;
    const float gain = OrcWeaponAudioCloseGain();
    if (ammoLeft < info->m_nAmmoClip / 3u && OrcWeaponAudioTryPlaySuffix(ctx, "_low_ammo", gain, spatial))
        return true;
    return ammoLeft == 0 && OrcWeaponAudioTryPlaySuffix(ctx, "_dryfire", gain, spatial);
}

static bool OrcTryCustomWeaponFire(const OrcWeaponAudioStemContext& ctx, eWeaponType weaponType,
    CAEWeaponAudioEntity* self = nullptr) {
    const OrcWeaponSpatial spatial = OrcWeaponAudioSpatialForPed(ctx.ped);
    const bool world = spatial == OrcWeaponSpatial::WorldAtPed;
    const bool useNativeCadence = self && world &&
        ((weaponType >= WEAPONTYPE_PISTOL && weaponType <= WEAPONTYPE_SNIPERRIFLE) || weaponType == WEAPONTYPE_DETONATOR);
    const uint32_t now = useNativeCadence ? CTimer::m_snTimeInMilliseconds : 0;
    // PlayGunSounds coalesces repeated events from the same audio entity for 25 ms.
    // Dual-wield tasks can send both hands in the same simulation frame.
    if (useNativeCadence && now - static_cast<uint32_t>(self->m_dwTimeLastFired) < 25u)
        return true;

    const float threshold = g_weaponCustomSoundDistantThreshold;
    const bool distant = world && OrcWeaponAudioCameraDistanceSquared(ctx.ped) >= threshold * threshold;
    const bool preferred = distant ? OrcTryDistantShot(ctx) : OrcTryAmmoShot(ctx, weaponType, spatial);
    const bool handled = preferred || OrcTryShoot(ctx, spatial);
    if (handled && useNativeCadence)
        self->m_dwTimeLastFired = static_cast<int>(now);
    if (world && !preferred && (distant || !handled)) {
        OrcLogInfoThrottled(402, 2000, "weapon audio: world shot fallback=%s wt=%d stem=%s",
            handled ? "shoot" : "original", ctx.weaponType, ctx.stem.c_str());
    }
    return handled;
}

static bool OrcTryMinigunOneShot(const OrcWeaponAudioStemContext& ctx) {
    const int ref = OrcSafeGetPedRef(ctx.ped);
    if (ref <= 0)
        return OrcTryCustomWeaponFire(ctx, WEAPONTYPE_MINIGUN);

    // GTA's pool reference holds the slot index above its 8-bit generation.
    const auto slot = static_cast<unsigned>(ref) >> 8;
    if (slot >= g_minigunShots.size())
        g_minigunShots.resize(slot + 1);
    MinigunShot& last = g_minigunShots[slot];
    const DWORD now = GetTickCount();
    if (last.pedRef == ref && last.asset == ctx.asset && now - last.tick < 95u)
        return true;
    if (!OrcTryCustomWeaponFire(ctx, WEAPONTYPE_MINIGUN))
        return false;
    last = {ref, ctx.asset, now};
    return true;
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
    if (surface < TOTAL_NUM_SURFACE_TYPES) {
        if (OrcIsAudioMetal(surface))
            return "_hitmetal";
        if (OrcIsAudioWood(surface))
            return "_hitwood";
    }
    return "_hit";
}

static bool OrcTryMeleeSuffixForPed(CPed* ped, const char* suffix, uint32_t delayMs) {
    if (!ped || !suffix)
        return false;
    const unsigned slot = ped->m_nSelectedWepSlot;
    if (slot >= std::size(ped->m_aWeapons))
        return false;
    const int wt = static_cast<int>(ped->m_aWeapons[slot].m_eWeaponType);
    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, wt, ctx))
        return false;
    const OrcWeaponSpatial spatial = OrcWeaponAudioSpatialForPed(ped);
    const float gain = OrcWeaponAudioCloseGain();
    if (OrcWeaponAudioTryPlaySuffix(ctx, suffix, gain, spatial, delayMs))
        return true;
    return strcmp(suffix, "_swing") != 0 && strcmp(suffix, "_hit") != 0 &&
        OrcWeaponAudioTryPlaySuffix(ctx, "_hit", gain, spatial, delayMs);
}

static bool OrcTryReplaceFire(CAEWeaponAudioEntity* self, eWeaponType weaponType, CPhysical* entity, int audioEventId) {
    const bool minigun = weaponType == WEAPONTYPE_MINIGUN;
    if (!OrcWeaponAudioCanPlayCustom() || g_forwardingArizonaFire || !entity ||
        (audioEventId != AE_WEAPON_FIRE &&
            !(minigun && (audioEventId == AE_WEAPON_FIRE_MINIGUN_AMMO || audioEventId == AE_WEAPON_FIRE_MINIGUN_NO_AMMO))))
        return false;

    CPed* ped = OrcWeaponAudioPedFromPhysical(entity);
    if (!ped)
        ped = OrcWeaponAudioPedFromWeaponAudio(self);
    OrcWeaponAudioStemContext ctx;
    if (!ped || !OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(weaponType), ctx))
        return false;

    if (minigun) {
        if (OrcWeaponAudioTryHandleMinigunEvent(ctx, self, audioEventId))
            return true;
        return audioEventId != AE_WEAPON_FIRE_MINIGUN_NO_AMMO && OrcTryMinigunOneShot(ctx);
    }

    // These stock fire functions submit independent components. The loop hook
    // substitutes each component and leaves any failed replacement audible.
    if (OrcWeaponAudioHasPlayableFireLoop(ctx))
        return false;
    return OrcTryCustomWeaponFire(ctx, weaponType, self);
}

static void __fastcall WeaponFire_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, eWeaponType weaponType, CPhysical* entity,
    int audioEventId) {
    if (!OrcTryReplaceFire(self, weaponType, entity, audioEventId))
        g_WeaponFire_Orig(self, weaponType, entity, audioEventId);
}

static void __fastcall ArizonaWeaponFire_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, eWeaponType weaponType,
    CPhysical* entity, int audioEventId) {
    if (GetCurrentThreadId() != g_gameThreadId) {
        g_ArizonaWeaponFire_Orig(self, weaponType, entity, audioEventId);
        return;
    }
    if (OrcTryReplaceFire(self, weaponType, entity, audioEventId))
        return;

    // Arizona forwards ordinary weapons to the already hooked GTA entry.
    const bool previous = g_forwardingArizonaFire;
    g_forwardingArizonaFire = true;
    g_ArizonaWeaponFire_Orig(self, weaponType, entity, audioEventId);
    g_forwardingArizonaFire = previous;
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

static CPed* OrcWeaponAudioPedFromPedAudio(CAEPedAudioEntity* self, const char* source) {
    CPed* ped = nullptr;
    __try {
        ped = self ? self->m_pPed : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogInfoThrottled(920, 4000u, "weapon audio: read ped audio entity SEH ex=0x%08X source=%s self=%p", GetExceptionCode(), source, self);
        return nullptr;
    }
    return OrcWeaponAudioValidatePedCandidate(ped, source);
}

static void __fastcall WeaponReload_Detour(CAEWeaponAudioEntity* self, void* /*edx*/, eWeaponType weaponType, CPhysical* entity,
    int audioEventId) {
    if (!g_WeaponReload_Orig)
        return;

    if (!OrcWeaponAudioCanPlayCustom() || !entity || (audioEventId != AE_WEAPON_RELOAD_A && audioEventId != AE_WEAPON_RELOAD_B)) {
        g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    CPed* ped = OrcWeaponAudioPedFromPhysical(entity);
    if (!ped && self)
        ped = OrcWeaponAudioPedFromWeaponAudio(self);
    if (!ped) {
        g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    OrcWeaponAudioStemContext ctx;
    if (!OrcWeaponAudioTryBuildStemContext(ped, static_cast<int>(weaponType), ctx)) {
        g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
        return;
    }

    const float gain = OrcWeaponAudioCloseGain();
    const OrcWeaponSpatial spatial = OrcWeaponAudioSpatialForPed(ped);
    const char* specific = OrcReloadSuffixForEvent(audioEventId);
    if (strcmp(specific, "_reload") != 0) {
        if (OrcWeaponAudioTryPlaySuffix(ctx, specific, gain, spatial))
            return;
    }
    if (OrcWeaponAudioTryPlaySuffix(ctx, "_reload", gain, spatial))
        return;

    g_WeaponReload_Orig(self, weaponType, entity, audioEventId);
}

static void __fastcall HandlePedHit_Detour(CAEPedAudioEntity* self, void* /*edx*/, int audioEvent, CPhysical* victim,
    uint8_t surface, float volume, uint32_t delayMs) {
    if (OrcWeaponAudioCanPlayCustom() && OrcWeaponAudioIsMeleeHitAudioEvent(audioEvent)) {
        CPed* ped = OrcWeaponAudioPedFromPedAudio(self, "pedAudio.hit.m_pPed");
        if (ped && OrcTryMeleeSuffixForPed(ped, OrcMeleeHitSuffix(audioEvent, surface), delayMs))
            return;
    }
    g_HandlePedHit_Orig(self, audioEvent, victim, surface, volume, delayMs);
}

static void __fastcall HandlePedSwing_Detour(CAEPedAudioEntity* self, void* /*edx*/, int audioEvent, int attackType,
    uint32_t delayMs) {
    if (OrcWeaponAudioCanPlayCustom() && audioEvent == AE_PED_SWING) {
        CPed* ped = OrcWeaponAudioPedFromPedAudio(self, "pedAudio.swing.m_pPed");
        if (ped && OrcTryMeleeSuffixForPed(ped, "_swing", delayMs))
            return;
    }
    g_HandlePedSwing_Orig(self, audioEvent, attackType, delayMs);
}

namespace {

struct AudioHook {
    uintptr_t address;
    const char* name;
    std::array<uint8_t, 8> prologue;
    void* detour;
    void** original;
    bool installed = false;
};

AudioHook g_audioHooks[] = {
    {0x504F80, "WeaponFire", {0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x56, 0x8B},
        reinterpret_cast<void*>(&WeaponFire_Detour), reinterpret_cast<void**>(&g_WeaponFire_Orig)},
    {0x503690, "WeaponReload", {0x51, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24},
        reinterpret_cast<void*>(&WeaponReload_Detour), reinterpret_cast<void**>(&g_WeaponReload_Orig)},
    {0x4E1CC0, "HandlePedHit", {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x8B, 0xF1},
        reinterpret_cast<void*>(&HandlePedHit_Detour), reinterpret_cast<void**>(&g_HandlePedHit_Orig)},
    {0x4E1A40, "HandlePedSwing", {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF1},
        reinterpret_cast<void*>(&HandlePedSwing_Detour), reinterpret_cast<void**>(&g_HandlePedSwing_Orig)},
};

AudioHook g_arizonaHook = {0, "Arizona WeaponFire", {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xED, 0x5F},
    reinterpret_cast<void*>(&ArizonaWeaponFire_Detour), reinterpret_cast<void**>(&g_ArizonaWeaponFire_Orig)};

bool MatchesPrologue(const AudioHook& hook) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(hook.address), hook.prologue.data(), hook.prologue.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InstallHook(AudioHook& hook) {
    if (!MatchesPrologue(hook)) {
        OrcLogError("weapon audio: %s at 0x%08X has an unexpected prologue", hook.name, static_cast<unsigned>(hook.address));
        return false;
    }
    void* entry = reinterpret_cast<void*>(hook.address);
    const MH_STATUS create = MH_CreateHook(entry, hook.detour, hook.original);
    if (create != MH_OK) {
        OrcLogError("weapon audio: MH_CreateHook %s -> %s", hook.name, MH_StatusToString(create));
        return false;
    }
    const MH_STATUS enable = MH_EnableHook(entry);
    if (enable != MH_OK) {
        MH_RemoveHook(entry);
        *hook.original = nullptr;
        OrcLogError("weapon audio: MH_EnableHook %s -> %s", hook.name, MH_StatusToString(enable));
        return false;
    }
    hook.installed = true;
    return true;
}

void RemoveHook(AudioHook& hook) {
    if (!hook.installed)
        return;
    const MH_STATUS remove = MH_RemoveHook(reinterpret_cast<void*>(hook.address));
    if (remove == MH_OK) {
        hook.installed = false;
        *hook.original = nullptr;
    } else {
        OrcLogError("weapon audio: MH_RemoveHook %s -> %s", hook.name, MH_StatusToString(remove));
    }
}

bool IsCallTo(uintptr_t site, uintptr_t target) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(site);
    int32_t displacement;
    if (bytes[0] != 0xE8)
        return false;
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    return site + 5u + displacement == target;
}

bool IsVerifiedArizonaFire(uintptr_t base, uintptr_t target) {
    __try {
        if (!IsCallTo(0x4E6A3D, target) || !IsCallTo(0x4DFAC6, target))
            return false;
        uint32_t exceptionHandler;
        const auto* code = reinterpret_cast<const uint8_t*>(target);
        std::memcpy(&exceptionHandler, code + 6, sizeof(exceptionHandler));
        const uint8_t fsLoad[] = {0x64, 0xA1, 0, 0, 0, 0};
        return exceptionHandler == base + 0x175FED && std::memcmp(code + 10, fsLoad, sizeof(fsLoad)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProbeArizonaFire() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextArizonaProbe)
        return;
    g_nextArizonaProbe = now + 1000;
    const auto base = reinterpret_cast<uintptr_t>(g_arizonaCore);
    const uintptr_t target = base + 0x3B300;
    if (!IsVerifiedArizonaFire(base, target))
        return;
    g_arizonaCore = nullptr;
    g_arizonaHook.address = target;
    if (InstallHook(g_arizonaHook))
        OrcLogInfo("weapon audio: Arizona weapon sound dispatch hooked");
}

void InstallHooks() {
    g_hooksInstallAttempted = true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("weapon audio: MH_Initialize -> %s", MH_StatusToString(init));
        return;
    }

    unsigned installed = 0;
    for (AudioHook& hook : g_audioHooks)
        installed += InstallHook(hook);
    OrcWeaponAudioLoopsEnsureInstalled();
    g_arizonaCore = GetModuleHandleW(L"core.asi");
    OrcLogInfo("weapon audio: installed %u/%u event hooks", installed, static_cast<unsigned>(std::size(g_audioHooks)));
}

}  // namespace

void OrcWeaponAudioStopPlayback() {
    OrcWeaponAudioStopEphemeralSources();
    OrcWeaponAudioLoopsStopAll();
    OrcWeaponAudioHooksClearShootThrottleState();
}

void OrcWeaponAudioOnGameProcess() {
    if (!g_hooksInstallAttempted && IsCustomAudioEnabled()) {
        g_gameThreadId = GetCurrentThreadId();
        InstallHooks();
    }

    static bool wasActive = false;
    const bool active = IsCustomAudioEnabled() && !IsPaused();
    if (active && g_arizonaCore)
        ProbeArizonaFire();
    if (!active && !wasActive)
        return;
    if (!OrcWeaponAudioHasActiveContext()) {
        wasActive = active;
        return;
    }
    const bool canProcess = active && OrcWeaponAudioEnsureAlContextCurrent();
    if (!canProcess && wasActive)
        OrcWeaponAudioStopPlayback();
    wasActive = canProcess;
    if (!canProcess)
        return;

    OrcWeaponAudioPruneEphemeralSources();
    OrcWeaponAudioLoopsOnGameProcess();
}

void OrcWeaponAudioShutdown() {
    RemoveHook(g_arizonaHook);
    for (AudioHook& hook : g_audioHooks)
        RemoveHook(hook);
    OrcWeaponAudioLoopsShutdown();
    OrcWeaponAudioOpenAlShutdown();
    OrcWeaponAudioHooksClearShootThrottleState();
}
