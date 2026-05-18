// OpenAL Soft (static): device, buffers, decode, EFX reverb, 3D attenuation.

#include "plugin.h"

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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_audio_config.h"
#include "orc_weapon_audio_decode.h"
#include "orc_weapon_audio_internal.h"

HMODULE g_pluginModule = nullptr;
ALCdevice* g_alcDevice = nullptr;
ALCcontext* g_alcContext = nullptr;
bool g_openAlInitFailedLogged = false;

std::vector<OrcWeaponAudioSourceSlot> g_ephemeralSources;
std::mutex g_ephemeralMutex;
std::vector<ALuint> g_loopSources;
std::mutex g_loopMutex;
std::unordered_map<std::string, ALuint> g_bufferByPath;
std::mutex g_bufferMutex;

static bool g_efxSupported = false;
static bool g_efxReady = false;
static ALuint g_efxEffectSlot = 0;
static ALuint g_reverbEffect = 0;

static void OrcWeaponAudioEfxShutdown() {
    if (!g_alcContext || !alcMakeContextCurrent(g_alcContext))
        return;
    if (g_reverbEffect) {
        alDeleteEffects(1, &g_reverbEffect);
        g_reverbEffect = 0;
    }
    if (g_efxEffectSlot) {
        alDeleteAuxiliaryEffectSlots(1, &g_efxEffectSlot);
        g_efxEffectSlot = 0;
    }
    g_efxReady = false;
}

static void OrcWeaponAudioEfxTryInit() {
    g_efxSupported = false;
    g_efxReady = false;
    if (!g_alcDevice || !g_alcContext)
        return;
    if (!alcMakeContextCurrent(g_alcContext))
        return;

    if (!alcIsExtensionPresent(g_alcDevice, ALC_EXT_EFX_NAME)) {
        if (g_orcLogLevel >= OrcLogLevel::Info)
            OrcLogInfo("weapon audio: ALC_EXT_EFX not available");
        return;
    }
    g_efxSupported = true;

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

static void OrcApplyPlayParamsToSource(ALuint src, const OrcWeaponAudioPlayParams& p, CPed* ped) {
    const bool world = p.spatial == OrcWeaponSpatial::WorldAtPed;
    OrcWeaponAudioAttenuation att = p.att;
    if (!world) {
        att.refDist = 0.5f;
        att.maxDist = 1.0e6f;
        att.rolloffFactor = 0.01f;
        att.airAbsorption = 0.0f;
    }

    alSourcef(src, AL_REFERENCE_DISTANCE, att.refDist);
    alSourcef(src, AL_MAX_DISTANCE, att.maxDist);
    alSourcef(src, AL_ROLLOFF_FACTOR, att.rolloffFactor);
    alSourcef(src, AL_AIR_ABSORPTION_FACTOR, att.airAbsorption);
    alSourcef(src, AL_GAIN, p.gain);
    alSourcef(src, AL_PITCH, p.pitch);

    if (world) {
        alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
        if (ped) {
            const CVector pos = ped->GetPosition();
            alSource3f(src, AL_POSITION, pos.x, pos.y, pos.z);
        } else {
            alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
        }
        if (g_efxReady && p.useEfxReverb)
            alSource3i(src, AL_AUXILIARY_SEND_FILTER, g_efxEffectSlot, 0, AL_FILTER_NULL);
        else
            alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
    } else {
        alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
    }
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
}

ALuint OrcGetOrCreateBufferForPath(const char* path) {
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    const std::string key(path);
    auto it = g_bufferByPath.find(key);
    if (it != g_bufferByPath.end() && alIsBuffer(it->second))
        return it->second;

    OrcAudioPcm pcm;
    std::string decErr;
    if (!OrcAudioDecodeFile(path, pcm, &decErr) || pcm.samples.empty() || pcm.sampleRate == 0) {
        if (g_orcLogLevel >= OrcLogLevel::Error)
            OrcLogError("weapon audio: decode failed %s (%s)", path, decErr.c_str());
        return 0;
    }

    ALuint buf = 0;
    alGenBuffers(1, &buf);
    if (!buf || alGetError() != AL_NO_ERROR) {
        if (buf)
            alDeleteBuffers(1, &buf);
        return 0;
    }

    const ALsizei bytes = static_cast<ALsizei>(pcm.samples.size() * sizeof(float));
    alBufferData(buf, AL_FORMAT_MONO_FLOAT32, pcm.samples.data(), bytes, static_cast<ALsizei>(pcm.sampleRate));
    if (alGetError() != AL_NO_ERROR) {
        alDeleteBuffers(1, &buf);
        return 0;
    }
    g_bufferByPath[key] = buf;
    return buf;
}

static void OrcWeaponAudioSyncListener() {
    if (!g_alcContext)
        return;
    const CVector cam = *TheCamera.GetGameCamPosition();
    alListener3f(AL_POSITION, cam.x, cam.y, cam.z);
    const CVector fwd = TheCamera.m_mCameraMatrix.GetForward();
    const CVector up = TheCamera.m_mCameraMatrix.GetUp();
    CVector fn = fwd;
    CVector un = up;
    fn.Normalize();
    un.Normalize();
    const ALfloat orient[6] = {fn.x, fn.y, fn.z, un.x, un.y, un.z};
    alListenerfv(AL_ORIENTATION, orient);
}

bool OrcWeaponAudioHasActiveContext() {
    return g_alcContext != nullptr;
}

bool OrcWeaponAudioEnsureAlContextCurrent() {
    if (!g_alcContext)
        return false;
    if (!alcMakeContextCurrent(g_alcContext))
        return false;
    OrcWeaponAudioSyncListener();
    return true;
}

bool OrcWeaponAudioOpenAlInit() {
    if (g_alcContext)
        return true;
    if (!g_weaponCustomSounds)
        return false;

    g_alcDevice = alcOpenDevice(nullptr);
    if (!g_alcDevice) {
        if (!g_openAlInitFailedLogged && g_orcLogLevel >= OrcLogLevel::Error) {
            g_openAlInitFailedLogged = true;
            OrcLogError("weapon audio: alcOpenDevice failed");
        }
        return false;
    }
    g_alcContext = alcCreateContext(g_alcDevice, nullptr);
    if (!g_alcContext || !alcMakeContextCurrent(g_alcContext)) {
        if (!g_openAlInitFailedLogged && g_orcLogLevel >= OrcLogLevel::Error) {
            g_openAlInitFailedLogged = true;
            OrcLogError("weapon audio: alcCreateContext failed");
        }
        if (g_alcContext)
            alcDestroyContext(g_alcContext);
        g_alcContext = nullptr;
        alcCloseDevice(g_alcDevice);
        g_alcDevice = nullptr;
        return false;
    }

    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    OrcWeaponAudioEfxTryInit();
    if (g_orcLogLevel >= OrcLogLevel::Info)
        OrcLogInfo("weapon audio: OpenAL Soft initialized (static)");
    return true;
}

void OrcWeaponAudioStopAllLoopSources() {
    if (!g_alcContext)
        return;
    std::lock_guard<std::mutex> lock(g_loopMutex);
    for (ALuint& src : g_loopSources) {
        if (src && alIsSource(src)) {
            alSourceStop(src);
            alDeleteSources(1, &src);
        }
        src = 0;
    }
    g_loopSources.clear();
}

bool OrcWeaponAudioStartLoopSource(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, ALuint& inOutSource) {
    if (!buffer || !ped)
        return false;
    if (!OrcWeaponAudioEnsureAlContextCurrent())
        return false;

    if (inOutSource && alIsSource(inOutSource)) {
        ALint st = AL_STOPPED;
        alGetSourcei(inOutSource, AL_SOURCE_STATE, &st);
        if (st == AL_PLAYING) {
            const CVector p = ped->GetPosition();
            alSource3f(inOutSource, AL_POSITION, p.x, p.y, p.z);
            OrcApplyPlayParamsToSource(inOutSource, params, ped);
            OrcWeaponAudioMarkSuppressVanilla();
            return true;
        }
        alSourceStop(inOutSource);
        alDeleteSources(1, &inOutSource);
        inOutSource = 0;
    }

    ALuint src = 0;
    alGenSources(1, &src);
    if (!src || alGetError() != AL_NO_ERROR)
        return false;

    alSourcei(src, AL_BUFFER, (ALint)buffer);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    OrcApplyPlayParamsToSource(src, params, ped);
    alSourcePlay(src);
    if (alGetError() != AL_NO_ERROR) {
        alDeleteSources(1, &src);
        return false;
    }
    inOutSource = src;
    std::lock_guard<std::mutex> lock(g_loopMutex);
    g_loopSources.push_back(src);
    return true;
}

bool OrcWeaponAudioIsLoopSourcePlaying(ALuint source) {
    if (!source || !g_alcContext || !OrcWeaponAudioEnsureAlContextCurrent())
        return false;
    if (!alIsSource(source))
        return false;
    ALint st = AL_STOPPED;
    alGetSourcei(source, AL_SOURCE_STATE, &st);
    return st == AL_PLAYING;
}

void OrcWeaponAudioStopLoopSource(ALuint& inOutSource) {
    if (!inOutSource || !g_alcContext)
        return;
    if (!OrcWeaponAudioEnsureAlContextCurrent())
        return;
    if (alIsSource(inOutSource)) {
        alSourceStop(inOutSource);
        alDeleteSources(1, &inOutSource);
    }
    std::lock_guard<std::mutex> lock(g_loopMutex);
    const auto it = std::find(g_loopSources.begin(), g_loopSources.end(), inOutSource);
    if (it != g_loopSources.end())
        g_loopSources.erase(it);
    inOutSource = 0;
}

void OrcWeaponAudioSyncLoopSourceWorldPos(ALuint source, CPed* ped, float gain) {
    if (!source || !ped || !g_alcContext || !OrcWeaponAudioEnsureAlContextCurrent())
        return;
    if (!alIsSource(source))
        return;
    const CVector p = ped->GetPosition();
    alSource3f(source, AL_POSITION, p.x, p.y, p.z);
    alSourcef(source, AL_GAIN, gain);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    alSourcef(source, AL_PITCH, pitch);
}

void OrcWeaponAudioUpdateLoopSources() {
    if (!g_alcContext || !OrcWeaponAudioEnsureAlContextCurrent())
        return;
    std::lock_guard<std::mutex> lock(g_loopMutex);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    const float gain = std::max(0.0f, g_weaponCustomSoundGain);
    for (auto it = g_loopSources.begin(); it != g_loopSources.end();) {
        const ALuint src = *it;
        if (!src || !alIsSource(src)) {
            it = g_loopSources.erase(it);
            continue;
        }
        ALint st = AL_STOPPED;
        alGetSourcei(src, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) {
            alDeleteSources(1, &src);
            it = g_loopSources.erase(it);
            continue;
        }
        alSourcef(src, AL_PITCH, pitch);
        alSourcef(src, AL_GAIN, gain);
        ++it;
    }
}

void OrcWeaponAudioOpenAlShutdown() {
    OrcWeaponAudioStopAllLoopSources();
    if (g_alcContext) {
        alcMakeContextCurrent(g_alcContext);
        OrcWeaponAudioEfxShutdown();
        std::lock_guard<std::mutex> lock(g_ephemeralMutex);
        for (auto& s : g_ephemeralSources) {
            if (s.source && alIsSource(s.source)) {
                alSourceStop(s.source);
                alDeleteSources(1, &s.source);
            }
            s.source = 0;
        }
        g_ephemeralSources.clear();

        std::lock_guard<std::mutex> bufLock(g_bufferMutex);
        for (auto& kv : g_bufferByPath) {
            if (kv.second && alIsBuffer(kv.second))
                alDeleteBuffers(1, &kv.second);
            kv.second = 0;
        }
        g_bufferByPath.clear();

        alcMakeContextCurrent(nullptr);
        alcDestroyContext(g_alcContext);
        g_alcContext = nullptr;
    }
    if (g_alcDevice) {
        alcCloseDevice(g_alcDevice);
        g_alcDevice = nullptr;
    }
}

void OrcWeaponAudioStopEphemeralSources() {
    if (!g_alcContext)
        return;
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    for (auto& s : g_ephemeralSources) {
        if (s.source && alIsSource(s.source)) {
            ALint st = AL_STOPPED;
            alGetSourcei(s.source, AL_SOURCE_STATE, &st);
            if (st == AL_PLAYING)
                alSourceStop(s.source);
        }
    }
}

void OrcWeaponAudioPruneEphemeralSources() {
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    for (auto it = g_ephemeralSources.begin(); it != g_ephemeralSources.end();) {
        if (!it->source || !alIsSource(it->source)) {
            it = g_ephemeralSources.erase(it);
            continue;
        }
        ALint st = AL_STOPPED;
        alGetSourcei(it->source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING && st != AL_PAUSED) {
            alDeleteSources(1, &it->source);
            it = g_ephemeralSources.erase(it);
        } else {
            ++it;
        }
    }
}

bool OrcWeaponAudioPlayBuffer(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped) {
    if (!buffer || !OrcWeaponAudioEnsureAlContextCurrent())
        return false;

    ALuint src = 0;
    alGenSources(1, &src);
    if (!src || alGetError() != AL_NO_ERROR)
        return false;

    alSourcei(src, AL_BUFFER, (ALint)buffer);
    alSourcei(src, AL_LOOPING, AL_FALSE);
    OrcApplyPlayParamsToSource(src, params, ped);
    alSourcePlay(src);
    const ALenum err = alGetError();
    if (err != AL_NO_ERROR) {
        if (g_orcLogLevel >= OrcLogLevel::Error)
            OrcLogError("weapon audio: alSourcePlay err=0x%X", (unsigned)err);
        alDeleteSources(1, &src);
        return false;
    }
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    g_ephemeralSources.push_back({src});
    return true;
}

bool OrcWeaponAudioTryPlayPath(const char* path, const OrcWeaponAudioPlayParams& params, CPed* ped) {
    if (!g_weaponCustomSounds || !path || !path[0])
        return false;
    if (!OrcWeaponAudioOpenAlInit())
        return false;
    if (!OrcWeaponAudioPathExistsCached(path))
        return false;

    const ALuint buf = OrcGetOrCreateBufferForPath(path);
    if (!buf) {
        if (g_orcLogLevel >= OrcLogLevel::Error)
            OrcLogError("weapon audio: failed to decode %s", path);
        return false;
    }

    OrcWeaponAudioPlayParams p2 = params;
    p2.gain = std::max(0.0f, params.gain);
    return OrcWeaponAudioPlayBuffer(buf, p2, ped);
}
