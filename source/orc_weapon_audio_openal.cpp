// OpenAL Soft: device, buffers, play, listener.

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

#include "orc_app.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_weapon_audio_internal.h"

using ALenum = int;
using ALboolean = unsigned char;
using ALCboolean = unsigned char;
using ALuint = unsigned int;
using ALsizei = int;
using ALfloat = float;
using ALint = int;
using ALCchar = char;
using ALCdevice = struct ALCdevice_struct;
using ALCcontext = struct ALCcontext_struct;

static constexpr ALenum AL_NONE = 0;
static constexpr ALenum AL_FALSE = 0;
static constexpr ALenum AL_TRUE = 1;
static constexpr ALenum AL_FORMAT_MONO16 = 0x1101;
static constexpr ALenum AL_SOURCE_RELATIVE = 0x202;
static constexpr ALenum AL_BUFFER = 0x1009;
static constexpr ALenum AL_GAIN = 0x100A;
static constexpr ALenum AL_PITCH = 0x1003;
static constexpr ALenum AL_SOURCE_STATE = 0x1010;
static constexpr ALenum AL_PLAYING = 0x1012;
static constexpr ALenum AL_PAUSED = 0x1013;
static constexpr ALenum AL_STOPPED = 0x1014;
static constexpr ALenum AL_POSITION = 0x1004;
static constexpr ALenum AL_VELOCITY = 0x1006;
static constexpr ALenum AL_ORIENTATION = 0x100F;
static constexpr ALenum AL_INVERSE_DISTANCE_CLAMPED = 0xD002;
static constexpr ALenum AL_REFERENCE_DISTANCE = 0x1020;
static constexpr ALenum AL_MAX_DISTANCE = 0x1023;
static constexpr ALenum AL_LOOPING = 0x1007;

#define AL_APIENTRY __cdecl
#define ALC_APIENTRY __cdecl

using PFN_alBufferData = void(AL_APIENTRY*)(ALuint, ALenum, const void*, ALsizei, ALsizei);
using PFN_alDeleteBuffers = void(AL_APIENTRY*)(ALsizei, const ALuint*);
using PFN_alDeleteSources = void(AL_APIENTRY*)(ALsizei, const ALuint*);
using PFN_alDistanceModel = void(AL_APIENTRY*)(ALenum);
using PFN_alGenBuffers = void(AL_APIENTRY*)(ALsizei, ALuint*);
using PFN_alGenSources = void(AL_APIENTRY*)(ALsizei, ALuint*);
using PFN_alGetError = ALenum(AL_APIENTRY*)(void);
using PFN_alGetSourcei = void(AL_APIENTRY*)(ALuint, ALenum, ALint*);
using PFN_alIsBuffer = ALboolean(AL_APIENTRY*)(ALuint);
using PFN_alIsSource = ALboolean(AL_APIENTRY*)(ALuint);
using PFN_alListener3f = void(AL_APIENTRY*)(ALenum, ALfloat, ALfloat, ALfloat);
using PFN_alListenerfv = void(AL_APIENTRY*)(ALenum, const ALfloat*);
using PFN_alSource3f = void(AL_APIENTRY*)(ALuint, ALenum, ALfloat, ALfloat, ALfloat);
using PFN_alSourcePlay = void(AL_APIENTRY*)(ALuint);
using PFN_alSourceStop = void(AL_APIENTRY*)(ALuint);
using PFN_alSourcei = void(AL_APIENTRY*)(ALuint, ALenum, ALint);
using PFN_alSourcef = void(AL_APIENTRY*)(ALuint, ALenum, ALfloat);

using PFN_alcCloseDevice = ALCboolean(ALC_APIENTRY*)(ALCdevice*);
using PFN_alcCreateContext = ALCcontext*(ALC_APIENTRY*)(ALCdevice*, const ALint*);
using PFN_alcDestroyContext = void(ALC_APIENTRY*)(ALCcontext*);
using PFN_alcGetCurrentContext = ALCcontext*(ALC_APIENTRY*)(void);
using PFN_alcMakeContextCurrent = ALCboolean(ALC_APIENTRY*)(ALCcontext*);
using PFN_alcOpenDevice = ALCdevice*(ALC_APIENTRY*)(const ALCchar*);

struct OrcAlApi {
    HMODULE dll = nullptr;
    PFN_alBufferData alBufferData = nullptr;
    PFN_alDeleteBuffers alDeleteBuffers = nullptr;
    PFN_alDeleteSources alDeleteSources = nullptr;
    PFN_alDistanceModel alDistanceModel = nullptr;
    PFN_alGenBuffers alGenBuffers = nullptr;
    PFN_alGenSources alGenSources = nullptr;
    PFN_alGetError alGetError = nullptr;
    PFN_alGetSourcei alGetSourcei = nullptr;
    PFN_alIsBuffer alIsBuffer = nullptr;
    PFN_alIsSource alIsSource = nullptr;
    PFN_alListener3f alListener3f = nullptr;
    PFN_alListenerfv alListenerfv = nullptr;
    PFN_alSource3f alSource3f = nullptr;
    PFN_alSourcePlay alSourcePlay = nullptr;
    PFN_alSourceStop alSourceStop = nullptr;
    PFN_alSourcei alSourcei = nullptr;
    PFN_alSourcef alSourcef = nullptr;
    PFN_alcCloseDevice alcCloseDevice = nullptr;
    PFN_alcCreateContext alcCreateContext = nullptr;
    PFN_alcDestroyContext alcDestroyContext = nullptr;
    PFN_alcGetCurrentContext alcGetCurrentContext = nullptr;
    PFN_alcMakeContextCurrent alcMakeContextCurrent = nullptr;
    PFN_alcOpenDevice alcOpenDevice = nullptr;

    bool LoadFrom(const char* openAlDllPath) {
        if (dll)
            return true;
        dll = LoadLibraryA(openAlDllPath);
        if (!dll)
            return false;
        alcOpenDevice = reinterpret_cast<PFN_alcOpenDevice>(GetProcAddress(dll, "alcOpenDevice"));
        alcCloseDevice = reinterpret_cast<PFN_alcCloseDevice>(GetProcAddress(dll, "alcCloseDevice"));
        alcCreateContext = reinterpret_cast<PFN_alcCreateContext>(GetProcAddress(dll, "alcCreateContext"));
        alcDestroyContext = reinterpret_cast<PFN_alcDestroyContext>(GetProcAddress(dll, "alcDestroyContext"));
        alcMakeContextCurrent = reinterpret_cast<PFN_alcMakeContextCurrent>(GetProcAddress(dll, "alcMakeContextCurrent"));
        alcGetCurrentContext = reinterpret_cast<PFN_alcGetCurrentContext>(GetProcAddress(dll, "alcGetCurrentContext"));
        alBufferData = reinterpret_cast<PFN_alBufferData>(GetProcAddress(dll, "alBufferData"));
        alDeleteBuffers = reinterpret_cast<PFN_alDeleteBuffers>(GetProcAddress(dll, "alDeleteBuffers"));
        alDeleteSources = reinterpret_cast<PFN_alDeleteSources>(GetProcAddress(dll, "alDeleteSources"));
        alDistanceModel = reinterpret_cast<PFN_alDistanceModel>(GetProcAddress(dll, "alDistanceModel"));
        alGenBuffers = reinterpret_cast<PFN_alGenBuffers>(GetProcAddress(dll, "alGenBuffers"));
        alGenSources = reinterpret_cast<PFN_alGenSources>(GetProcAddress(dll, "alGenSources"));
        alGetError = reinterpret_cast<PFN_alGetError>(GetProcAddress(dll, "alGetError"));
        alGetSourcei = reinterpret_cast<PFN_alGetSourcei>(GetProcAddress(dll, "alGetSourcei"));
        alIsBuffer = reinterpret_cast<PFN_alIsBuffer>(GetProcAddress(dll, "alIsBuffer"));
        alIsSource = reinterpret_cast<PFN_alIsSource>(GetProcAddress(dll, "alIsSource"));
        alListener3f = reinterpret_cast<PFN_alListener3f>(GetProcAddress(dll, "alListener3f"));
        alListenerfv = reinterpret_cast<PFN_alListenerfv>(GetProcAddress(dll, "alListenerfv"));
        alSource3f = reinterpret_cast<PFN_alSource3f>(GetProcAddress(dll, "alSource3f"));
        alSourcePlay = reinterpret_cast<PFN_alSourcePlay>(GetProcAddress(dll, "alSourcePlay"));
        alSourceStop = reinterpret_cast<PFN_alSourceStop>(GetProcAddress(dll, "alSourceStop"));
        alSourcei = reinterpret_cast<PFN_alSourcei>(GetProcAddress(dll, "alSourcei"));
        alSourcef = reinterpret_cast<PFN_alSourcef>(GetProcAddress(dll, "alSourcef"));
        return alcOpenDevice && alcCreateContext && alcMakeContextCurrent && alGenBuffers && alBufferData && alGenSources &&
               alSourcei && alSourcef && alSource3f && alListener3f && alListenerfv && alSourcePlay && alGetError &&
               alDeleteBuffers && alDeleteSources && alGetSourcei && alSourceStop && alDistanceModel && alcCloseDevice &&
               alcDestroyContext && alcGetCurrentContext;
    }

    void Unload() {
        if (dll)
            FreeLibrary(dll);
        *this = OrcAlApi{};
    }
};

OrcAlApi g_al;
ALCdevice* g_alcDevice = nullptr;
ALCcontext* g_alcContext = nullptr;
HMODULE g_pluginModule = nullptr;
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
    if (it != g_bufferByPath.end() && g_al.alIsBuffer(it->second))
        return it->second;

    std::vector<uint8_t> pcm;
    unsigned sr = 0;
    if (!OrcReadMonoPcm16Wav(path, pcm, sr))
        return 0;

    ALuint buf = 0;
    g_al.alGenBuffers(1, &buf);
    if (!buf || g_al.alGetError() != AL_NONE) {
        if (buf)
            g_al.alDeleteBuffers(1, &buf);
        return 0;
    }
    g_al.alBufferData(buf, AL_FORMAT_MONO16, pcm.data(), (ALsizei)pcm.size(), (ALsizei)sr);
    if (g_al.alGetError() != AL_NONE) {
        g_al.alDeleteBuffers(1, &buf);
        return 0;
    }
    g_bufferByPath[key] = buf;
    return buf;
}

static void OrcWeaponAudioSyncListener() {
    if (!g_alcContext)
        return;
    const CVector cam = *TheCamera.GetGameCamPosition();
    g_al.alListener3f(AL_POSITION, cam.x, cam.y, cam.z);
    const CVector fwd = TheCamera.m_mCameraMatrix.GetForward();
    const CVector up = TheCamera.m_mCameraMatrix.GetUp();
    CVector fn = fwd;
    CVector un = up;
    fn.Normalize();
    un.Normalize();
    const ALfloat orient[6] = {fn.x, fn.y, fn.z, un.x, un.y, un.z};
    g_al.alListenerfv(AL_ORIENTATION, orient);
}

bool OrcWeaponAudioHasActiveContext() {
    return g_alcContext != nullptr;
}

bool OrcWeaponAudioEnsureAlContextCurrent() {
    if (!g_alcContext)
        return false;
    if (!g_al.alcMakeContextCurrent(g_alcContext))
        return false;
    OrcWeaponAudioSyncListener();
    return true;
}

bool OrcWeaponAudioOpenAlInit() {
    if (g_alcContext)
        return true;
    if (!g_weaponCustomSounds || !g_pluginModule)
        return false;

    char modPath[MAX_PATH]{};
    if (!GetModuleFileNameA(g_pluginModule, modPath, MAX_PATH))
        return false;
    char* slash = strrchr(modPath, '\\');
    if (!slash)
        slash = strrchr(modPath, '/');
    if (slash)
        *(slash + 1) = 0;
    const std::string dllPath = OrcJoinPath(std::string(modPath), "OpenAL32.dll");

    if (!g_al.LoadFrom(dllPath.c_str())) {
        if (!g_openAlInitFailedLogged && g_orcLogLevel >= OrcLogLevel::Error) {
            g_openAlInitFailedLogged = true;
            OrcLogError("weapon audio: LoadLibrary OpenAL32.dll failed path=%s err=%lu", dllPath.c_str(),
                (unsigned long)GetLastError());
        }
        return false;
    }

    g_alcDevice = g_al.alcOpenDevice(nullptr);
    if (!g_alcDevice) {
        OrcLogError("weapon audio: alcOpenDevice failed");
        g_al.Unload();
        return false;
    }
    g_alcContext = g_al.alcCreateContext(g_alcDevice, nullptr);
    if (!g_alcContext || !g_al.alcMakeContextCurrent(g_alcContext)) {
        OrcLogError("weapon audio: alcCreateContext failed");
        if (g_alcContext)
            g_al.alcDestroyContext(g_alcContext);
        g_alcContext = nullptr;
        g_al.alcCloseDevice(g_alcDevice);
        g_alcDevice = nullptr;
        g_al.Unload();
        return false;
    }

    g_al.alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    if (g_orcLogLevel >= OrcLogLevel::Info)
        OrcLogInfo("weapon audio: OpenAL initialized (%s)", dllPath.c_str());
    return true;
}

void OrcWeaponAudioStopAllLoopSources() {
    if (!g_alcContext)
        return;
    std::lock_guard<std::mutex> lock(g_loopMutex);
    for (ALuint& src : g_loopSources) {
        if (src && g_al.alIsSource(src)) {
            g_al.alSourceStop(src);
            g_al.alDeleteSources(1, &src);
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

    if (inOutSource && g_al.alIsSource(inOutSource)) {
        ALint st = AL_STOPPED;
        g_al.alGetSourcei(inOutSource, AL_SOURCE_STATE, &st);
        if (st == AL_PLAYING) {
            const CVector p = ped->GetPosition();
            g_al.alSource3f(inOutSource, AL_POSITION, p.x, p.y, p.z);
            const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
            g_al.alSourcef(inOutSource, AL_PITCH, pitch);
            g_al.alSourcef(inOutSource, AL_GAIN, gain);
            OrcWeaponAudioMarkSuppressVanilla();
            return true;
        }
        g_al.alSourceStop(inOutSource);
        g_al.alDeleteSources(1, &inOutSource);
        inOutSource = 0;
    }

    ALuint src = 0;
    g_al.alGenSources(1, &src);
    if (!src || g_al.alGetError() != AL_NONE)
        return false;

    g_al.alSourcei(src, AL_BUFFER, (ALint)buffer);
    g_al.alSourcei(src, AL_LOOPING, AL_TRUE);
    g_al.alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    g_al.alSourcef(src, AL_GAIN, gain);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    g_al.alSourcef(src, AL_PITCH, pitch);
    g_al.alSourcef(src, AL_REFERENCE_DISTANCE, 1.0f);
    g_al.alSourcef(src, AL_MAX_DISTANCE, 80.0f);
    const CVector p = ped->GetPosition();
    g_al.alSource3f(src, AL_POSITION, p.x, p.y, p.z);
    g_al.alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    g_al.alSourcePlay(src);
    if (g_al.alGetError() != AL_NONE) {
        g_al.alDeleteSources(1, &src);
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
    if (g_al.alIsSource(inOutSource)) {
        g_al.alSourceStop(inOutSource);
        g_al.alDeleteSources(1, &inOutSource);
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
    if (!g_al.alIsSource(source))
        return;
    const CVector p = ped->GetPosition();
    g_al.alSource3f(source, AL_POSITION, p.x, p.y, p.z);
    g_al.alSourcef(source, AL_GAIN, gain);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    g_al.alSourcef(source, AL_PITCH, pitch);
}

void OrcWeaponAudioUpdateLoopSources() {
    if (!g_alcContext || !OrcWeaponAudioEnsureAlContextCurrent())
        return;
    std::lock_guard<std::mutex> lock(g_loopMutex);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    const float gain = std::max(0.0f, g_weaponCustomSoundGain);
    for (auto it = g_loopSources.begin(); it != g_loopSources.end();) {
        const ALuint src = *it;
        if (!src || !g_al.alIsSource(src)) {
            it = g_loopSources.erase(it);
            continue;
        }
        ALint st = AL_STOPPED;
        g_al.alGetSourcei(src, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) {
            g_al.alDeleteSources(1, &src);
            it = g_loopSources.erase(it);
            continue;
        }
        g_al.alSourcef(src, AL_PITCH, pitch);
        g_al.alSourcef(src, AL_GAIN, gain);
        ++it;
    }
}

void OrcWeaponAudioOpenAlShutdown() {
    OrcWeaponAudioStopAllLoopSources();
    if (g_alcContext) {
        g_al.alcMakeContextCurrent(g_alcContext);
        std::lock_guard<std::mutex> lock(g_ephemeralMutex);
        for (auto& s : g_ephemeralSources) {
            if (s.source && g_al.alIsSource(s.source)) {
                g_al.alSourceStop(s.source);
                g_al.alDeleteSources(1, &s.source);
            }
            s.source = 0;
        }
        g_ephemeralSources.clear();

        std::lock_guard<std::mutex> bufLock(g_bufferMutex);
        for (auto& kv : g_bufferByPath) {
            if (kv.second && g_al.alIsBuffer(kv.second))
                g_al.alDeleteBuffers(1, &kv.second);
            kv.second = 0;
        }
        g_bufferByPath.clear();

        g_al.alcMakeContextCurrent(nullptr);
        g_al.alcDestroyContext(g_alcContext);
        g_alcContext = nullptr;
    }
    if (g_alcDevice) {
        g_al.alcCloseDevice(g_alcDevice);
        g_alcDevice = nullptr;
    }
    g_al.Unload();
}

void OrcWeaponAudioStopEphemeralSources() {
    if (!g_alcContext)
        return;
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    for (auto& s : g_ephemeralSources) {
        if (s.source && g_al.alIsSource(s.source)) {
            ALint st = AL_STOPPED;
            g_al.alGetSourcei(s.source, AL_SOURCE_STATE, &st);
            if (st == AL_PLAYING)
                g_al.alSourceStop(s.source);
        }
    }
}

void OrcWeaponAudioPruneEphemeralSources() {
    std::lock_guard<std::mutex> lock(g_ephemeralMutex);
    for (auto it = g_ephemeralSources.begin(); it != g_ephemeralSources.end();) {
        if (!it->source || !g_al.alIsSource(it->source)) {
            it = g_ephemeralSources.erase(it);
            continue;
        }
        ALint st = AL_STOPPED;
        g_al.alGetSourcei(it->source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING && st != AL_PAUSED) {
            g_al.alDeleteSources(1, &it->source);
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
    g_al.alGenSources(1, &src);
    if (!src || g_al.alGetError() != AL_NONE)
        return false;

    g_al.alSourcei(src, AL_BUFFER, (ALint)buffer);
    const bool relative = spatial == OrcWeaponSpatial::ListenerRelative;
    g_al.alSourcei(src, AL_SOURCE_RELATIVE, relative ? AL_TRUE : AL_FALSE);
    g_al.alSourcef(src, AL_GAIN, gain);
    const float pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    g_al.alSourcef(src, AL_PITCH, pitch);
    g_al.alSourcef(src, AL_REFERENCE_DISTANCE, 1.0f);
    g_al.alSourcef(src, AL_MAX_DISTANCE, 80.0f);

    if (relative) {
        g_al.alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    } else if (ped) {
        const CVector p = ped->GetPosition();
        g_al.alSource3f(src, AL_POSITION, p.x, p.y, p.z);
    } else {
        g_al.alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    }
    g_al.alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    g_al.alSourcePlay(src);
    const ALenum err = g_al.alGetError();
    if (err != AL_NONE) {
        if (g_orcLogLevel >= OrcLogLevel::Error)
            OrcLogError("weapon audio: alSourcePlay err=0x%X", (unsigned)err);
        g_al.alDeleteSources(1, &src);
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
