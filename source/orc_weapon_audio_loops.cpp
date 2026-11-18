// Continuous weapon effects follow the lifetime of the GTA 1.0 US sound that they replace.

#include "plugin.h"

#include "CAEWeaponAudioEntity.h"
#include "CAESound.h"
#include "CPed.h"
#include "CTimer.h"
#include "eAudioEvents.h"
#include "eWeaponType.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "Patch.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_internal.h"

using namespace plugin;

static constexpr uintptr_t kRequestSoundAddress = 0x4EFB10;
static constexpr uintptr_t kCalculateVolumeAddress = 0x4EFA10;
static constexpr uintptr_t kCalculateVolumeCall = 0x4F041A;
static constexpr std::array<uint8_t, 9> kRequestBody = {0x33, 0xF6, 0x0F, 0xBF, 0xC6, 0x6B, 0xC0, 0x74, 0x90};
static constexpr std::array<uint8_t, 8> kCalculateVolumePrologue = {0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1, 0xF6, 0x46};

// These calls construct known weapon sounds. Requests for all other game audio stay on their normal path.
static constexpr std::array<uintptr_t, 9> kRequestCallSites = {
    0x5038FB, // Flamethrower gas
    0x503A50, // Chainsaw active / cutting
    0x503AFA, // Chainsaw idle
    0x50454C, // Flamethrower ignition
    0x504600, // Flamethrower fire
    0x5046DF, // Spraycan / extinguisher
    0x5048F3, // Minigun spin
    0x504A84, // Minigun spin end
    0x504B58, // Chainsaw stop
};
static std::array<bool, kRequestCallSites.size()> g_redirected{};

using RequestSound = CAESound* (__thiscall*)(void*, CAESound*);
using CalculateVolume = void (__thiscall*)(CAESound*);
static bool g_calculateVolumeRedirected = false;

struct OrcSoundEvent {
    int bank;
    int sfx;
    unsigned event;
    eWeaponType weapon;
    const char* suffix;
    bool loop;
    const char* cuttingSuffix = nullptr;
};

static constexpr OrcSoundEvent kFlameFire{19, 26, 2, WEAPONTYPE_FTHROWER, "_flamethrower_fire", true};
static constexpr OrcSoundEvent kSpray{5, 28, 3, WEAPONTYPE_SPRAYCAN, "_spraycan_sprayloop", true};
static constexpr OrcSoundEvent kExtinguisher{5, 9, 4, WEAPONTYPE_EXTINGUISHER, "_extinguisher_loop", true};
static constexpr unsigned kNoAudioEvent = static_cast<unsigned>(-1);
static constexpr OrcSoundEvent kEvents[] = {
    kFlameFire, kSpray, kExtinguisher,
    {5, 10, kNoAudioEvent, WEAPONTYPE_FTHROWER, "_flamethrower_idlegasloop", true},
    {5, 83, kNoAudioEvent, WEAPONTYPE_FTHROWER, "_flamethrower_start", false},
    {5, 14, 5, WEAPONTYPE_MINIGUN, "_minigun_barrelspinloop", true},
    {5, 63, 8, WEAPONTYPE_MINIGUN, "_minigun_barrelspinend", false},
    {40, 1, 9, WEAPONTYPE_CHAINSAW, "_chainsaw_idle", true},
    {40, 0, 10, WEAPONTYPE_CHAINSAW, "_chainsaw_active", true, "_chainsaw_cuttingflesh"},
    {40, 2, 12, WEAPONTYPE_CHAINSAW, "_chainsaw_stop", false},
};

struct OrcManagedSound {
    const OrcSoundEvent* event = nullptr;
    CAEAudioEntity* owner = nullptr;
    CPed* ped = nullptr;
    int pedRef = 0;
    ALuint source = 0;
    ALuint buffer = 0;
    ALuint cuttingBuffer = 0;
    ALuint playingBuffer = 0;
    float classPitch = 1.0f;
    OrcWeaponAudioPlayParams params{};
};

struct OrcMinigunLoop {
    int pedRef = 0;
    ALuint source = 0;
    bool spin = false;
    unsigned lastEventTime = 0;
};

static std::unordered_map<CAESound*, OrcManagedSound> g_managedSounds;
static std::unordered_map<CPed*, OrcMinigunLoop> g_minigunLoops;

static uintptr_t OrcCallTarget(uintptr_t site) {
    if (patch::GetUChar(site) != 0xE8)
        return 0;
    return site + 5 + static_cast<int32_t>(patch::GetUInt(site + 1));
}

// AudioRuntime v2.1.2 hotpatches the first two bytes of RequestNewSound. For an OrcOutFit
// replacement only, replay the displaced prologue so GTA can keep its original sound state.
static __declspec(naked) CAESound* __fastcall OrcRequestNativeHotpatched(void*, void*, CAESound*) {
    __asm {
        push esi
        push edi
        push 004EFB12h
        ret
    }
}

static bool OrcIsAudioRuntimeDetour(uintptr_t address) {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const wchar_t*>(address), &owner))
        return false;

    wchar_t module[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(owner, module, _countof(module));
    if (!length || length >= _countof(module))
        return false;
    const wchar_t* name = wcsrchr(module, L'\\');
    return _wcsicmp(name ? name + 1 : module, L"AudioRuntime.asi") == 0;
}

static RequestSound OrcNativeRequestSound() {
    const auto* code = reinterpret_cast<const uint8_t*>(kRequestSoundAddress);
    if (std::memcmp(code + 2, kRequestBody.data(), kRequestBody.size()) != 0)
        return nullptr;
    if (code[0] == 0x56 && code[1] == 0x57)
        return reinterpret_cast<RequestSound>(kRequestSoundAddress);

    if (code[0] != 0xEB || code[1] != 0xF9 || code[-5] != 0xE9)
        return nullptr;
    // The hotpatch uses JMP rel32 in its padding, rather than CALL rel32.
    int32_t displacement = 0;
    std::memcpy(&displacement, code - 4, sizeof(displacement));
    const uintptr_t target = kRequestSoundAddress + displacement;
    static uintptr_t lastTarget = 0;
    static bool audioRuntime = false;
    if (target != lastTarget) {
        lastTarget = target;
        audioRuntime = OrcIsAudioRuntimeDetour(target);
    }
    return audioRuntime ? reinterpret_cast<RequestSound>(&OrcRequestNativeHotpatched) : nullptr;
}

static const OrcSoundEvent* OrcFindSoundEvent(const CAESound& sound) {
    for (const OrcSoundEvent& event : kEvents) {
        if (sound.m_nBankSlotId == event.bank && sound.m_nSoundIdInSlot == event.sfx && sound.m_nEvent == event.event)
            return &event;
    }
    return nullptr;
}

static bool OrcSoundStillAlive(const CAESound& sound, const OrcManagedSound& managed) {
    return sound.m_nIsUsed && sound.m_nPlayingState == 0 && sound.m_pBaseAudio == managed.owner &&
        sound.m_nBankSlotId == managed.event->bank && sound.m_nSoundIdInSlot == managed.event->sfx &&
        sound.m_nEvent == managed.event->event;
}

static bool OrcPedStillEquipped(CPed* ped, int pedRef, eWeaponType weapon) {
    if (pedRef <= 0 || OrcSafeGetPed(pedRef) != ped)
        return false;
    const auto slot = ped->m_nSelectedWepSlot;
    return slot < 13 && ped->m_aWeapons[slot].m_eWeaponType == weapon;
}

static float OrcLoopGain() {
    return std::max(0.0f, g_weaponCustomSoundGain);
}

static float OrcTimeScale() {
    const float scale = CTimer::ms_fTimeScale;
    return std::isfinite(scale) ? std::clamp(scale, 0.01f, 4.0f) : 1.0f;
}

static bool OrcTryStartLoop(const OrcWeaponAudioStemContext& ctx, const char* suffix, ALuint& source) {
    const ALuint buffer = OrcWeaponAudioResolveFirstPlayableAudioBuffer(ctx, suffix);
    if (!buffer)
        return false;
    const auto params = OrcWeaponAudioBuildPlayParams(&ctx, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed,
        OrcWeaponInferSoundClassFromSuffix(suffix));
    return OrcWeaponAudioStartLoopSource(buffer, params, ctx.ped, source);
}

static CAESound* __fastcall OrcRequestSoundDetour(void* manager, void*, CAESound* sound) {
    const auto original = reinterpret_cast<RequestSound>(kRequestSoundAddress);
    if (!sound || !OrcWeaponAudioCanPlayCustom())
        return original(manager, sound);

    const OrcSoundEvent* event = OrcFindSoundEvent(*sound);
    if (!event)
        return original(manager, sound);

    // These verified request sites all register the emitting entity before the call.
    CPed* ped = OrcWeaponAudioPedFromPhysical(reinterpret_cast<CPhysical*>(sound->m_pPhysicalEntity));
    OrcWeaponAudioStemContext ctx;
    if (!ped || !OrcWeaponAudioTryBuildStemContext(ped, event->weapon, ctx))
        return original(manager, sound);

    // Resolve the runtime hook only for recognized sounds that can actually be replaced.
    const ALuint buffer = OrcWeaponAudioResolveFirstPlayableAudioBuffer(ctx, event->suffix);
    const bool ownedByPed = sound->m_pBaseAudio == &ped->m_weaponAudio;
    const ALuint cuttingBuffer = event->cuttingSuffix && ownedByPed
        ? OrcWeaponAudioResolveFirstPlayableAudioBuffer(ctx, event->cuttingSuffix) : 0;
    if (!buffer && !cuttingBuffer)
        return original(manager, sound);

    const RequestSound native = OrcNativeRequestSound();
    if (!native)
        return original(manager, sound);

    OrcManagedSound managed{};
    managed.event = event;
    managed.owner = sound->m_pBaseAudio;
    managed.ped = ped;
    managed.pedRef = OrcSafeGetPedRef(ped);
    managed.buffer = buffer;
    managed.cuttingBuffer = cuttingBuffer;
    if (event->loop) {
        managed.params = OrcWeaponAudioBuildPlayParams(&ctx, OrcLoopGain(), OrcWeaponSpatial::WorldAtPed,
            OrcWeaponInferSoundClassFromSuffix(event->suffix));
        managed.classPitch = managed.params.pitch / OrcTimeScale();
        if (buffer) {
            if (OrcWeaponAudioStartLoopSource(buffer, managed.params, ped, managed.source))
                managed.playingBuffer = buffer;
            else if (!cuttingBuffer)
                return original(manager, sound);
        }
    } else if (!OrcWeaponAudioTryPlaySuffix(ctx, event->suffix, OrcLoopGain(), OrcWeaponAudioSpatialForPed(ped))) {
        return original(manager, sound);
    }

    CAESound* slot = native(manager, sound);
    if (!slot) {
        OrcWeaponAudioStopLoopSource(managed.source);
        // The stock allocator has already released the descriptor's entity reference.
        return nullptr;
    }
    if (const auto it = g_managedSounds.find(slot); it != g_managedSounds.end())
        OrcWeaponAudioStopLoopSource(it->second.source);
    g_managedSounds.insert_or_assign(slot, managed);
    return slot;
}

static void __fastcall OrcCalculateVolumeDetour(CAESound* sound, void*) {
    reinterpret_cast<CalculateVolume>(kCalculateVolumeAddress)(sound);
    if (!OrcWeaponAudioCanPlayCustom() || g_managedSounds.empty())
        return;
    const auto it = g_managedSounds.find(sound);
    if (it != g_managedSounds.end() && OrcSoundStillAlive(*sound, it->second) &&
        (!it->second.event->loop || it->second.source))
        sound->m_fFinalVolume = -100.0f;
}

bool OrcWeaponAudioHasPlayableFireLoop(const OrcWeaponAudioStemContext& ctx) {
    const OrcSoundEvent* event = nullptr;
    switch (ctx.weaponType) {
    case WEAPONTYPE_FTHROWER:
        if (g_redirected[4])
            event = &kFlameFire;
        break;
    case WEAPONTYPE_SPRAYCAN:
        if (g_redirected[5])
            event = &kSpray;
        break;
    case WEAPONTYPE_EXTINGUISHER:
        if (g_redirected[5])
            event = &kExtinguisher;
        break;
    default:
        break;
    }
    return event && OrcNativeRequestSound() && OrcWeaponAudioResolveFirstPlayableAudioBuffer(ctx, event->suffix);
}

bool OrcWeaponAudioTryHandleMinigunEvent(const OrcWeaponAudioStemContext& ctx, CAEWeaponAudioEntity*, int audioEventId) {
    const bool spin = audioEventId == AE_WEAPON_FIRE_MINIGUN_NO_AMMO;
    if (!spin && audioEventId != AE_WEAPON_FIRE && audioEventId != AE_WEAPON_FIRE_MINIGUN_AMMO)
        return false;

    CPed* ped = ctx.ped;
    const unsigned now = CTimer::m_snTimeInMilliseconds;
    const int pedRef = OrcSafeGetPedRef(ped);
    const auto existing = g_minigunLoops.find(ped);
    if (existing != g_minigunLoops.end()) {
        OrcMinigunLoop& loop = existing->second;
        if (loop.pedRef == pedRef && loop.spin == spin && OrcWeaponAudioIsLoopSourcePlaying(loop.source)) {
            loop.lastEventTime = now;
            return true;
        }
        OrcWeaponAudioStopLoopSource(loop.source);
        g_minigunLoops.erase(existing);
    }

    ALuint source = 0;
    const char* suffix = spin ? "_minigun_barrelspinloop" : "_minigun_fireloop";
    if (!OrcTryStartLoop(ctx, suffix, source))
        return false;
    g_minigunLoops.emplace(ped, OrcMinigunLoop{pedRef, source, spin, now});
    OrcLogInfoThrottled(406, 500, "weapon audio: OpenAL minigun %s stem=%s", spin ? "spin" : "fireloop", ctx.stem.c_str());
    return true;
}

void OrcWeaponAudioLoopsStopAll() {
    for (auto& entry : g_managedSounds)
        OrcWeaponAudioStopLoopSource(entry.second.source);
    g_managedSounds.clear();
    for (auto& entry : g_minigunLoops)
        OrcWeaponAudioStopLoopSource(entry.second.source);
    g_minigunLoops.clear();
}

void OrcWeaponAudioLoopsOnGameProcess() {
    const float gain = OrcLoopGain();
    for (auto it = g_managedSounds.begin(); it != g_managedSounds.end();) {
        OrcManagedSound& sound = it->second;
        if (!OrcSoundStillAlive(*it->first, sound) ||
            (sound.event->loop && !OrcPedStillEquipped(sound.ped, sound.pedRef, sound.event->weapon))) {
            OrcWeaponAudioStopLoopSource(sound.source);
            it = g_managedSounds.erase(it);
            continue;
        }

        if (sound.event->loop) {
            const bool cutting = sound.cuttingBuffer && sound.ped->m_weaponAudio.m_nChainsawSoundState == 2;
            const ALuint wantedBuffer = cutting ? sound.cuttingBuffer : sound.buffer;
            if (wantedBuffer != sound.playingBuffer) {
                OrcWeaponAudioStopLoopSource(sound.source);
                OrcWeaponAudioPlayParams params = sound.params;
                params.gain = gain;
                params.pitch = sound.classPitch * OrcTimeScale();
                OrcWeaponAudioStartLoopSource(wantedBuffer, params, sound.ped, sound.source);
                sound.playingBuffer = wantedBuffer;
            }
            if (sound.source)
                OrcWeaponAudioSyncLoopSourceWorldPos(sound.source, sound.ped, gain);
        }
        ++it;
    }

    const unsigned now = CTimer::m_snTimeInMilliseconds;
    for (auto it = g_minigunLoops.begin(); it != g_minigunLoops.end();) {
        CPed* ped = it->first;
        auto& loop = it->second;
        const bool equipped = OrcPedStillEquipped(ped, loop.pedRef, WEAPONTYPE_MINIGUN);
        const bool expired = now - loop.lastEventTime > 300;
        if (equipped && !expired) {
            OrcWeaponAudioSyncLoopSourceWorldPos(loop.source, ped, gain);
            if (loop.source) {
                ++it;
                continue;
            }
        }
        OrcWeaponAudioStopLoopSource(loop.source);
        if (equipped && expired) {
            OrcWeaponAudioStemContext ctx;
            if (OrcWeaponAudioTryBuildStemContext(ped, WEAPONTYPE_MINIGUN, ctx))
                OrcWeaponAudioTryPlaySuffix(ctx, "_minigun_barrelspinend", gain, OrcWeaponAudioSpatialForPed(ped));
        }
        it = g_minigunLoops.erase(it);
    }
}

void OrcWeaponAudioLoopsEnsureInstalled() {
    if (g_calculateVolumeRedirected)
        return;
    if (OrcCallTarget(kCalculateVolumeCall) != kCalculateVolumeAddress ||
        std::memcmp(reinterpret_cast<const void*>(kCalculateVolumeAddress), kCalculateVolumePrologue.data(),
            kCalculateVolumePrologue.size()) != 0) {
        OrcLogError("weapon audio loops: unexpected CAESound::CalculateVolume call");
        return;
    }
    const auto calculateVolumeDetour = reinterpret_cast<uintptr_t>(&OrcCalculateVolumeDetour);
    patch::RedirectCall(kCalculateVolumeCall, calculateVolumeDetour);
    g_calculateVolumeRedirected = OrcCallTarget(kCalculateVolumeCall) == calculateVolumeDetour;
    if (!g_calculateVolumeRedirected) {
        OrcLogError("weapon audio loops: CalculateVolume redirect failed");
        return;
    }

    const auto detour = reinterpret_cast<uintptr_t>(&OrcRequestSoundDetour);
    unsigned installed = 0;
    for (size_t i = 0; i < kRequestCallSites.size(); ++i) {
        const uintptr_t site = kRequestCallSites[i];
        if (OrcCallTarget(site) != kRequestSoundAddress) {
            OrcLogError("weapon audio loops: unexpected sound request call at 0x%08X", static_cast<unsigned>(site));
            continue;
        }
        patch::RedirectCall(site, detour);
        g_redirected[i] = OrcCallTarget(site) == detour;
        installed += g_redirected[i];
    }
    OrcLogInfo("weapon audio loops: installed %u sound request redirects", installed);
}

void OrcWeaponAudioLoopsShutdown() {
    const auto detour = reinterpret_cast<uintptr_t>(&OrcRequestSoundDetour);
    for (size_t i = 0; i < kRequestCallSites.size(); ++i) {
        const uintptr_t site = kRequestCallSites[i];
        if (g_redirected[i] && OrcCallTarget(site) == detour)
            patch::RedirectCall(site, kRequestSoundAddress);
        g_redirected[i] = false;
    }
    if (g_calculateVolumeRedirected &&
        OrcCallTarget(kCalculateVolumeCall) == reinterpret_cast<uintptr_t>(&OrcCalculateVolumeDetour))
        patch::RedirectCall(kCalculateVolumeCall, kCalculateVolumeAddress);
    g_calculateVolumeRedirected = false;
    OrcWeaponAudioLoopsStopAll();
}
