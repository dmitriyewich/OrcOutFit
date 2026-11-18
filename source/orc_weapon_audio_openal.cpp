// OpenAL Soft (static): device, buffers, decode, EFX reverb, 3D attenuation.

#include "plugin.h"

#include "CAEAudioHardware.h"
#include "CCamera.h"
#include "CMenuManager.h"
#include "CTimer.h"
#include "CVector.h"
#include "CPed.h"

/* alext.h включает efx.h внутри #ifndef ALC_EXT_EFX — прототипы EFX видны только если
 * AL_ALEXT_PROTOTYPES задан ДО <AL/alext.h> (иначе AL_EFX_H уже закрыт повторным include). */
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_decode.h"
#include "orc_weapon_audio_internal.h"

static ALCdevice* g_alcDevice = nullptr;
static ALCcontext* g_alcContext = nullptr;
static ULONGLONG g_nextDeviceCheck = 0;
static ULONGLONG g_nextDeviceOpen = 0;
static bool g_deviceConnected = false;

struct OrcListenerState {
    std::array<ALfloat, 3> position{};
    std::array<ALfloat, 6> orientation{};
    float gain = -1.0f;
};

static OrcListenerState g_listener;

struct OrcLoopSource {
    ALuint buffer;
    float classPitch;
    float maxDist;
    std::optional<bool> reverbEnabled;
    std::optional<bool> reverbInteriorOnly;
    bool useReverb;
};

static std::unordered_map<ALuint, OrcLoopSource> g_loopSources;

std::vector<OrcWeaponAudioSourceSlot> g_ephemeralSources;
std::mutex g_ephemeralMutex;
std::unordered_map<std::string, ALuint> g_bufferByPath;
std::mutex g_bufferMutex;

static bool g_efxSupported = false;
static bool g_efxReady = false;
static ALuint g_efxEffectSlot = 0;
static ALuint g_reverbEffect = 0;

static bool OrcWeaponAudioMakeContextCurrent() {
    return g_alcContext && (alcGetCurrentContext() == g_alcContext || alcMakeContextCurrent(g_alcContext));
}

static void OrcWeaponAudioEfxShutdown() {
    if (g_efxEffectSlot)
        alDeleteAuxiliaryEffectSlots(1, &g_efxEffectSlot);
    if (g_reverbEffect)
        alDeleteEffects(1, &g_reverbEffect);
    g_efxEffectSlot = 0;
    g_reverbEffect = 0;
    g_efxReady = false;
}

static void OrcWeaponAudioEfxTryInit() {
    g_efxSupported = false;
    g_efxReady = false;
    if (!alcIsExtensionPresent(g_alcDevice, ALC_EXT_EFX_NAME)) {
        if (g_orcLogLevel >= OrcLogLevel::Info)
            OrcLogInfo("weapon audio: ALC_EXT_EFX not available");
        return;
    }
    g_efxSupported = true;

    alGetError();
    alGenAuxiliaryEffectSlots(1, &g_efxEffectSlot);
    alGenEffects(1, &g_reverbEffect);
    if (alGetError() != AL_NO_ERROR) {
        OrcWeaponAudioEfxShutdown();
        return;
    }

    alEffecti(g_reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alEffectf(g_reverbEffect, AL_REVERB_DENSITY, 0.2f);
    alEffectf(g_reverbEffect, AL_REVERB_DECAY_TIME, 0.6f);
    alEffectf(g_reverbEffect, AL_REVERB_GAIN, 0.3f);
    alEffectf(g_reverbEffect, AL_REVERB_GAINHF, 0.3f);
    alEffectf(g_reverbEffect, AL_REVERB_DECAY_HFRATIO, 0.3f);
    alEffectf(g_reverbEffect, AL_REVERB_REFLECTIONS_GAIN, 0.4f);
    alEffectf(g_reverbEffect, AL_REVERB_LATE_REVERB_GAIN, 0.3f);
    alEffectf(g_reverbEffect, AL_REVERB_LATE_REVERB_DELAY, 0.005f);
    alEffectf(g_reverbEffect, AL_REVERB_ROOM_ROLLOFF_FACTOR, 0.3f);
    alAuxiliaryEffectSloti(g_efxEffectSlot, AL_EFFECTSLOT_EFFECT, g_reverbEffect);

    if (alGetError() != AL_NO_ERROR) {
        OrcLogError("weapon audio: EFX reverb setup failed");
        OrcWeaponAudioEfxShutdown();
        return;
    }

    g_efxReady = true;
    if (g_orcLogLevel >= OrcLogLevel::Info)
        OrcLogInfo("weapon audio: EFX reverb initialized");
}

static float OrcWeaponAudioTimeScale() {
    const float scale = CTimer::ms_fTimeScale;
    return std::isfinite(scale) ? std::clamp(scale, 0.01f, 4.0f) : 1.0f;
}

static bool OrcWeaponAudioUsesReverb(std::optional<bool> enabled, std::optional<bool> interiorOnly) {
    return g_efxReady && enabled.value_or(g_weaponAudioEfxReverb) &&
        (!interiorOnly.value_or(g_weaponAudioEfxInteriorOnly) || OrcWeaponAudioIsInterior());
}

static void OrcApplyPlayParamsToSource(ALuint src, const OrcWeaponAudioPlayParams& p, CPed* ped) {
    const bool world = p.spatial == OrcWeaponSpatial::WorldAtPed;
    const float maxDist = p.att.maxDist > 0.0f ? p.att.maxDist : 1.0e6f;
    if (world) {
        alSourcef(src, AL_REFERENCE_DISTANCE, p.att.refDist);
        alSourcef(src, AL_MAX_DISTANCE, std::max(p.att.refDist, maxDist));
        alSourcef(src, AL_ROLLOFF_FACTOR, p.att.rolloffFactor);
        if (g_efxSupported)
            alSourcef(src, AL_AIR_ABSORPTION_FACTOR, p.att.airAbsorption);
        if (OrcWeaponAudioUsesReverb(p.efxReverbEnabled, p.efxReverbInteriorOnly))
            alSource3i(src, AL_AUXILIARY_SEND_FILTER, g_efxEffectSlot, 0, AL_FILTER_NULL);
        const CVector pos = ped ? ped->GetPosition() : CVector{0.0f, 0.0f, 0.0f};
        alSource3f(src, AL_POSITION, pos.x, pos.y, pos.z);
    } else {
        alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(src, AL_ROLLOFF_FACTOR, 0.0f);
    }

    alSourcef(src, AL_GAIN, world ? OrcWeaponAudioApplyDistanceCullGain(ped, p.att.maxDist, p.gain) : p.gain);
    // OpenAL's default max gain is 1.0, but the custom sound controls allow amplification.
    alSourcef(src, AL_MAX_GAIN, std::numeric_limits<ALfloat>::max());
    alSourcef(src, AL_PITCH, p.pitch);
}

// Чистим очередь ошибок AL и генерим ровно один источник. На любой неудаче гарантированно
// не оставляем утёкший хэндл (иначе пул источников OpenAL исчерпывается и звук глохнет навсегда).
static bool OrcWeaponAudioGenOneSource(ALuint& outSrc) {
    outSrc = 0;
    alGetError();
    ALuint src = 0;
    alGenSources(1, &src);
    if (alGetError() != AL_NO_ERROR || !src) {
        if (src && alIsSource(src))
            alDeleteSources(1, &src);
        return false;
    }
    outSrc = src;
    return true;
}

ALuint OrcGetOrCreateBufferForPath(const std::string& path) {
    if (path.empty() || !OrcWeaponAudioMakeContextCurrent())
        return 0;

    std::lock_guard<std::mutex> lock(g_bufferMutex);
    const auto it = g_bufferByPath.find(path);
    if (it != g_bufferByPath.end())
        return it->second;

    OrcAudioPcm pcm;
    std::string decErr;
    if (!OrcAudioDecodeFile(path.c_str(), pcm, &decErr) || pcm.samples.empty() || pcm.sampleRate == 0 ||
        pcm.samples.size() > std::numeric_limits<ALsizei>::max() / sizeof(float) ||
        pcm.sampleRate > static_cast<unsigned>(std::numeric_limits<ALsizei>::max())) {
        g_bufferByPath.emplace(path, 0);
        OrcLogError("weapon audio: decode failed %s (%s)", path.c_str(), decErr.c_str());
        return 0;
    }

    alGetError();
    ALuint buf = 0;
    alGenBuffers(1, &buf);
    const ALenum genError = alGetError();
    if (genError != AL_NO_ERROR || !buf) {
        if (buf && alIsBuffer(buf))
            alDeleteBuffers(1, &buf);
        OrcLogInfoThrottled(410, 2000, "weapon audio: alGenBuffers err=0x%X", static_cast<unsigned>(genError));
        return 0;
    }

    const ALsizei bytes = static_cast<ALsizei>(pcm.samples.size() * sizeof(float));
    alBufferData(buf, AL_FORMAT_MONO_FLOAT32, pcm.samples.data(), bytes, static_cast<ALsizei>(pcm.sampleRate));
    const ALenum uploadError = alGetError();
    if (uploadError != AL_NO_ERROR) {
        alDeleteBuffers(1, &buf);
        if (uploadError != AL_OUT_OF_MEMORY)
            g_bufferByPath.emplace(path, 0);
        OrcLogError("weapon audio: alBufferData err=0x%X path=%s", static_cast<unsigned>(uploadError), path.c_str());
        return 0;
    }
    g_bufferByPath.emplace(path, buf);
    return buf;
}

static void OrcWeaponAudioSyncListener() {
    const CVector* cam = TheCamera.GetGameCamPosition();
    const auto finite = [](float x) { return std::isfinite(x); };
    if (cam) {
        const std::array<ALfloat, 3> pos = {cam->x, cam->y, cam->z};
        if (pos != g_listener.position && std::all_of(pos.begin(), pos.end(), finite)) {
            alListenerfv(AL_POSITION, pos.data());
            g_listener.position = pos;
        }
    }
    const CVector& fwd = TheCamera.m_mCameraMatrix.GetForward();
    const CVector& up = TheCamera.m_mCameraMatrix.GetUp();
    const std::array<ALfloat, 6> orient = {fwd.x, fwd.y, fwd.z, up.x, up.y, up.z};
    if (orient != g_listener.orientation && std::all_of(orient.begin(), orient.end(), finite) &&
        fwd.MagnitudeSqr() > 0.0f && up.MagnitudeSqr() > 0.0f) {
        alListenerfv(AL_ORIENTATION, orient.data());
        g_listener.orientation = orient;
    }

    // The plugin has its own device, so the game's SFX gain and fades need to be applied explicitly.
    const float gain = AEAudioHardware.m_fEffectMasterScalingFactor * AEAudioHardware.m_fEffectsFaderScalingFactor *
        AEAudioHardware.m_fNonStreamFaderScalingFactor;
    if (gain != g_listener.gain && std::isfinite(gain) && gain >= 0.0f) {
        alListenerf(AL_GAIN, gain);
        g_listener.gain = gain;
    }
}

static bool OrcWeaponAudioCheckDevice() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextDeviceCheck)
        return g_deviceConnected;

    g_nextDeviceCheck = now + 1000;
    ALCint connected = ALC_FALSE;
    alcGetIntegerv(g_alcDevice, ALC_CONNECTED, 1, &connected);
    if (connected == ALC_FALSE) {
        if (g_deviceConnected)
            OrcLogError("weapon audio: OpenAL device disconnected");
        g_deviceConnected = alcReopenDeviceSOFT(g_alcDevice, nullptr, nullptr) == ALC_TRUE;
        if (g_deviceConnected)
            OrcLogInfo("weapon audio: OpenAL device reopened");
    } else {
        g_deviceConnected = true;
    }
    return g_deviceConnected;
}

bool OrcWeaponAudioHasActiveContext() {
    return g_alcContext != nullptr;
}

bool OrcWeaponAudioEnsureAlContextCurrent() {
    if (!OrcWeaponAudioMakeContextCurrent())
        return false;
    OrcWeaponAudioSyncListener();
    return OrcWeaponAudioCheckDevice();
}

bool OrcWeaponAudioOpenAlInit() {
    if (g_alcContext)
        return OrcWeaponAudioCheckDevice();
    if (!g_weaponCustomSounds)
        return false;
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextDeviceOpen)
        return false;
    g_nextDeviceOpen = now + 2000;

    g_alcDevice = alcOpenDevice(nullptr);
    if (!g_alcDevice) {
        OrcLogError("weapon audio: alcOpenDevice failed");
        return false;
    }
    g_alcContext = alcCreateContext(g_alcDevice, nullptr);
    if (!OrcWeaponAudioMakeContextCurrent()) {
        OrcLogError("weapon audio: OpenAL context initialization failed");
        if (g_alcContext)
            alcDestroyContext(g_alcContext);
        g_alcContext = nullptr;
        alcCloseDevice(g_alcDevice);
        g_alcDevice = nullptr;
        return false;
    }

    g_deviceConnected = true;
    g_nextDeviceCheck = now + 1000;
    g_listener = {};
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    OrcWeaponAudioSyncListener();
    OrcWeaponAudioEfxTryInit();
    OrcLogInfo("weapon audio: OpenAL Soft initialized (static)");
    return true;
}

static bool OrcWeaponAudioStartSource(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, bool looping,
    ALuint& source, bool startImmediately = true) {
    if (!buffer || !OrcWeaponAudioMakeContextCurrent() || !OrcWeaponAudioCheckDevice())
        return false;
    if (FrontEndMenuManager.m_bMenuActive || CTimer::m_UserPause || CTimer::m_CodePause || CTimer::ms_fTimeScale <= 0.0f)
        return false;

    ALuint src = 0;
    if (!OrcWeaponAudioGenOneSource(src)) {
        OrcWeaponAudioPruneEphemeralSources();
        if (!OrcWeaponAudioGenOneSource(src)) {
            OrcLogInfoThrottled(411, 2000, "weapon audio: alGenSources exhausted");
            return false;
        }
    }

    alSourcei(src, AL_BUFFER, static_cast<ALint>(buffer));
    alSourcei(src, AL_LOOPING, looping ? AL_TRUE : AL_FALSE);
    OrcApplyPlayParamsToSource(src, params, ped);
    ALenum err = alGetError();
    if (err == AL_NO_ERROR && startImmediately) {
        alSourcePlay(src);
        err = alGetError();
    }
    if (err != AL_NO_ERROR) {
        alDeleteSources(1, &src);
        OrcLogInfoThrottled(412, 2000, "weapon audio: source setup/play err=0x%X", static_cast<unsigned>(err));
        return false;
    }

    source = src;
    return true;
}

bool OrcWeaponAudioStartLoopSource(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, ALuint& inOutSource) {
    if (!buffer || !ped)
        return false;

    const auto existing = g_loopSources.find(inOutSource);
    if (existing != g_loopSources.end()) {
        if (existing->second.buffer == buffer && OrcWeaponAudioIsLoopSourcePlaying(inOutSource))
            return true;
        OrcWeaponAudioStopLoopSource(inOutSource);
    }

    if (!OrcWeaponAudioStartSource(buffer, params, ped, true, inOutSource))
        return false;
    g_loopSources.emplace(inOutSource, OrcLoopSource{buffer, params.pitch / OrcWeaponAudioTimeScale(), params.att.maxDist,
        params.efxReverbEnabled, params.efxReverbInteriorOnly,
        OrcWeaponAudioUsesReverb(params.efxReverbEnabled, params.efxReverbInteriorOnly)});
    return true;
}

bool OrcWeaponAudioIsLoopSourcePlaying(ALuint source) {
    if (!source || !OrcWeaponAudioMakeContextCurrent() || !OrcWeaponAudioCheckDevice())
        return false;
    ALint state = AL_STOPPED;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

void OrcWeaponAudioStopLoopSource(ALuint& inOutSource) {
    const auto it = g_loopSources.find(inOutSource);
    if (it != g_loopSources.end() && OrcWeaponAudioMakeContextCurrent()) {
        alDeleteSources(1, &inOutSource);
        g_loopSources.erase(it);
    }
    inOutSource = 0;
}

void OrcWeaponAudioSyncLoopSourceWorldPos(ALuint& source, CPed* ped, float gain) {
    const auto it = g_loopSources.find(source);
    if (it == g_loopSources.end()) {
        source = 0;
        return;
    }
    ALint state = AL_STOPPED;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) {
        alDeleteSources(1, &source);
        g_loopSources.erase(it);
        source = 0;
        return;
    }

    OrcLoopSource& data = it->second;
    const CVector p = ped->GetPosition();
    alSource3f(source, AL_POSITION, p.x, p.y, p.z);
    alSourcef(source, AL_GAIN, OrcWeaponAudioApplyDistanceCullGain(ped, data.maxDist, gain));
    alSourcef(source, AL_PITCH, data.classPitch * OrcWeaponAudioTimeScale());

    const bool useReverb = OrcWeaponAudioUsesReverb(data.reverbEnabled, data.reverbInteriorOnly);
    if (g_efxReady && useReverb != data.useReverb) {
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, useReverb ? g_efxEffectSlot : 0, 0, AL_FILTER_NULL);
        data.useReverb = useReverb;
    }
}

void OrcWeaponAudioClearBufferCache() {
    if (!OrcWeaponAudioMakeContextCurrent())
        return;

    for (const auto& entry : g_loopSources)
        alDeleteSources(1, &entry.first);
    g_loopSources.clear();

    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    for (const auto& slot : g_ephemeralSources)
        alDeleteSources(1, &slot.source);
    g_ephemeralSources.clear();

    std::lock_guard<std::mutex> bufferLock(g_bufferMutex);
    for (const auto& entry : g_bufferByPath) {
        if (entry.second)
            alDeleteBuffers(1, &entry.second);
    }
    g_bufferByPath.clear();
}

void OrcWeaponAudioOpenAlShutdown() {
    if (g_alcContext) {
        if (OrcWeaponAudioMakeContextCurrent()) {
            OrcWeaponAudioClearBufferCache();
            OrcWeaponAudioEfxShutdown();
            alcMakeContextCurrent(nullptr);
        }
        alcDestroyContext(g_alcContext);
        g_alcContext = nullptr;
    }
    if (g_alcDevice) {
        alcCloseDevice(g_alcDevice);
        g_alcDevice = nullptr;
    }
    g_deviceConnected = false;
    g_nextDeviceOpen = 0;
    g_nextDeviceCheck = 0;
}

void OrcWeaponAudioStopEphemeralSources() {
    if (!OrcWeaponAudioMakeContextCurrent())
        return;
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    for (const auto& slot : g_ephemeralSources)
        alDeleteSources(1, &slot.source);
    g_ephemeralSources.clear();
}

void OrcWeaponAudioPruneEphemeralSources() {
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    const uint32_t now = CTimer::m_snTimeInMilliseconds;
    const auto newEnd = std::remove_if(g_ephemeralSources.begin(), g_ephemeralSources.end(), [now](auto& slot) {
        if (slot.pendingPedRef != 0) {
            // Compare in GTA's clock domain, including wraparound.
            if (now - slot.dueTime >= 0x80000000u)
                return false;
            CPed* ped = OrcSafeGetPed(slot.pendingPedRef);
            if (!ped || (slot.world && OrcWeaponAudioIsBeyondMaxDistance(ped, slot.maxDist))) {
                alDeleteSources(1, &slot.source);
                return true;
            }
            alGetError();
            if (slot.world) {
                const CVector p = ped->GetPosition();
                alSource3f(slot.source, AL_POSITION, p.x, p.y, p.z);
                alSourcef(slot.source, AL_GAIN, slot.gain);
            }
            alSourcePlay(slot.source);
            slot.pendingPedRef = 0;
            const ALenum error = alGetError();
            if (error == AL_NO_ERROR)
                return false;
            OrcLogInfoThrottled(413, 2000, "weapon audio: delayed source play err=0x%X", static_cast<unsigned>(error));
            alDeleteSources(1, &slot.source);
            return true;
        }
        ALint state = AL_STOPPED;
        alGetSourcei(slot.source, AL_SOURCE_STATE, &state);
        if (state == AL_PLAYING || state == AL_PAUSED)
            return false;
        alDeleteSources(1, &slot.source);
        return true;
    });
    g_ephemeralSources.erase(newEnd, g_ephemeralSources.end());
}

bool OrcWeaponAudioPlayBuffer(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, uint32_t delayMs) {
    const int pedRef = delayMs ? OrcSafeGetPedRef(ped) : 0;
    if (delayMs && (pedRef <= 0 || delayMs >= 0x80000000u))
        return false;
    ALuint src = 0;
    if (!OrcWeaponAudioStartSource(buffer, params, ped, false, src, delayMs == 0))
        return false;
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    g_ephemeralSources.push_back({src, pedRef, CTimer::m_snTimeInMilliseconds + delayMs, params.att.maxDist, params.gain,
        params.spatial == OrcWeaponSpatial::WorldAtPed});
    return true;
}
