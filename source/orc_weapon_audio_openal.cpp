// OpenAL Soft (static): device, buffers, play, listener.

#include "plugin.h"

#include "CCamera.h"
#include "CMenuManager.h"
#include "CTimer.h"
#include "CVector.h"
#include "CPed.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>

#include "orc_app.h"
#include "orc_log.h"
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

static bool OrcReadMonoPcm16Wav(const char* path, std::vector<uint8_t>& outPcm, unsigned& sampleRate) {
    outPcm.clear();
    sampleRate = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    char riff[12]{};
    if (fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sr = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    long dataOffset = 0;

    for (;;) {
        char cid[4]{};
        uint32_t chunkSize = 0;
        if (fread(cid, 1, 4, f) != 4)
            break;
        if (fread(&chunkSize, 4, 1, f) != 1)
            break;

        if (std::memcmp(cid, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                fclose(f);
                return false;
            }
            if (fread(&audioFormat, 2, 1, f) != 1 || fread(&numChannels, 2, 1, f) != 1 || fread(&sr, 4, 1, f) != 1) {
                fclose(f);
                return false;
            }
            if (fseek(f, 6, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
            if (fread(&bitsPerSample, 2, 1, f) != 1) {
                fclose(f);
                return false;
            }
            const long skip = (long)chunkSize - 16;
            if (skip > 0 && fseek(f, skip, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
        } else if (std::memcmp(cid, "data", 4) == 0) {
            dataSize = chunkSize;
            dataOffset = ftell(f);
            if (dataOffset < 0 || fseek(f, (long)chunkSize, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
        } else {
            if (fseek(f, (long)chunkSize, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
        }

        if ((chunkSize & 1u) != 0)
            fseek(f, 1, SEEK_CUR);

        if (dataSize != 0 && audioFormat != 0)
            break;
    }

    if (audioFormat != 1 || numChannels != 1 || bitsPerSample != 16 || sr == 0 || dataSize == 0 || dataOffset == 0) {
        fclose(f);
        return false;
    }

    if (fseek(f, dataOffset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    outPcm.resize(dataSize);
    if (fread(outPcm.data(), 1, dataSize, f) != dataSize) {
        fclose(f);
        return false;
    }
    fclose(f);
    sampleRate = sr;
    return true;
}

ALuint OrcGetOrCreateBufferForWav(const char* path) {
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    const std::string key(path);
    auto it = g_bufferByPath.find(key);
    if (it != g_bufferByPath.end() && alIsBuffer(it->second))
        return it->second;

    std::vector<uint8_t> pcm;
    unsigned sr = 0;
    if (!OrcReadMonoPcm16Wav(path, pcm, sr))
        return 0;

    ALuint buf = 0;
    alGenBuffers(1, &buf);
    if (!buf || alGetError() != AL_NO_ERROR) {
        if (buf)
            alDeleteBuffers(1, &buf);
        return 0;
    }
    alBufferData(buf, AL_FORMAT_MONO16, pcm.data(), (ALsizei)pcm.size(), (ALsizei)sr);
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

bool OrcWeaponAudioStartLoopSource(ALuint buffer, float gain, CPed* ped, ALuint& inOutSource) {
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
            const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
            alSourcef(inOutSource, AL_PITCH, pitch);
            alSourcef(inOutSource, AL_GAIN, gain);
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
    alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcef(src, AL_GAIN, gain);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    alSourcef(src, AL_PITCH, pitch);
    alSourcef(src, AL_REFERENCE_DISTANCE, 1.0f);
    alSourcef(src, AL_MAX_DISTANCE, 80.0f);
    const CVector p = ped->GetPosition();
    alSource3f(src, AL_POSITION, p.x, p.y, p.z);
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
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

bool OrcWeaponAudioPlayBuffer(ALuint buffer, float gain, OrcWeaponSpatial spatial, CPed* ped) {
    if (!buffer || !OrcWeaponAudioEnsureAlContextCurrent())
        return false;

    ALuint src = 0;
    alGenSources(1, &src);
    if (!src || alGetError() != AL_NO_ERROR)
        return false;

    alSourcei(src, AL_BUFFER, (ALint)buffer);
    const bool relative = spatial == OrcWeaponSpatial::ListenerRelative;
    alSourcei(src, AL_SOURCE_RELATIVE, relative ? AL_TRUE : AL_FALSE);
    alSourcef(src, AL_GAIN, gain);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    alSourcef(src, AL_PITCH, pitch);
    alSourcef(src, AL_REFERENCE_DISTANCE, 1.0f);
    alSourcef(src, AL_MAX_DISTANCE, 80.0f);

    if (relative) {
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    } else if (ped) {
        const CVector p = ped->GetPosition();
        alSource3f(src, AL_POSITION, p.x, p.y, p.z);
    } else {
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    }
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
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

bool OrcWeaponAudioTryPlayPath(const char* path, float gainScale, OrcWeaponSpatial spatial, CPed* ped) {
    if (!g_weaponCustomSounds || !path || !path[0])
        return false;
    if (!OrcWeaponAudioOpenAlInit())
        return false;
    if (!OrcWeaponAudioPathExistsCached(path))
        return false;

    const ALuint buf = OrcGetOrCreateBufferForWav(path);
    if (!buf) {
        if (g_orcLogLevel >= OrcLogLevel::Error)
            OrcLogError("weapon audio: failed to decode WAV %s", path);
        return false;
    }

    const float g = std::max(0.0f, gainScale);
    return OrcWeaponAudioPlayBuffer(buf, g, spatial, ped);
}
