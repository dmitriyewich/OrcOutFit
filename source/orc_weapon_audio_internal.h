#pragma once

#include "orc_weapon_assets.h"
#include "orc_weapon_audio_types.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

class CPed;
class CPhysical;
class CAEWeaponAudioEntity;

using ALuint = unsigned int;

struct OrcWeaponAudioStemContext {
    std::string stem;
    std::string dir;
    int weaponType = -1;
    CPed* ped = nullptr;
    WeaponReplacementAsset* asset = nullptr;
};

struct OrcWeaponAudioSourceSlot {
    ALuint source = 0;
    int pendingPedRef = 0;
    uint32_t dueTime = 0;
    float maxDist = 0.0f;
    float gain = 0.0f;
    bool world = false;
};

bool OrcWeaponAudioCanPlayCustom();

extern std::vector<OrcWeaponAudioSourceSlot> g_ephemeralSources;
extern std::mutex g_ephemeralMutex;
extern std::unordered_map<std::string, ALuint> g_bufferByPath;
extern std::mutex g_bufferMutex;

ALuint OrcGetOrCreateBufferForPath(const std::string& path);

void OrcWeaponAudioClearBufferCache();
bool OrcWeaponAudioStartLoopSource(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, ALuint& inOutSource);
void OrcWeaponAudioStopLoopSource(ALuint& inOutSource);
bool OrcWeaponAudioIsLoopSourcePlaying(ALuint source);
void OrcWeaponAudioSyncLoopSourceWorldPos(ALuint& source, CPed* ped, float gain);

bool OrcWeaponAudioOpenAlInit();
void OrcWeaponAudioOpenAlShutdown();
bool OrcWeaponAudioHasActiveContext();
bool OrcWeaponAudioEnsureAlContextCurrent();
void OrcWeaponAudioPruneEphemeralSources();
void OrcWeaponAudioStopEphemeralSources();
bool OrcWeaponAudioPlayBuffer(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, uint32_t delayMs = 0);

CPed* OrcWeaponAudioPedFromPhysical(CPhysical* physical);
CPed* OrcWeaponAudioPedFromWeaponAudio(CAEWeaponAudioEntity* self);
CPed* OrcWeaponAudioValidatePedCandidate(CPed* ped, const char* source);
bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out);
uint16_t OrcWeaponAudioNumberedVariants(const OrcWeaponAudioStemContext& ctx, const char* prefix, int maxAlternatives);
ALuint OrcWeaponAudioResolveFirstPlayableAudioBuffer(const OrcWeaponAudioStemContext& ctx, const char* suffix);
bool OrcWeaponAudioTryPlaySuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, float gainScale,
    OrcWeaponSpatial spatial, uint32_t delayMs = 0);
bool OrcWeaponAudioIsLocalPlayerPed(CPed* ped);
float OrcWeaponAudioCameraDistanceSquared(CPed* ped);
bool OrcWeaponAudioIsBeyondMaxDistance(CPed* ped, float maxDist);
float OrcWeaponAudioApplyDistanceCullGain(CPed* ped, float maxDist, float gain);
OrcWeaponSpatial OrcWeaponAudioSpatialForPed(CPed* ped);

void OrcWeaponAudioLoopsEnsureInstalled();
void OrcWeaponAudioLoopsShutdown();
void OrcWeaponAudioLoopsOnGameProcess();
void OrcWeaponAudioLoopsStopAll();
bool OrcWeaponAudioTryHandleMinigunEvent(const OrcWeaponAudioStemContext& ctx, CAEWeaponAudioEntity* owner, int audioEventId);
bool OrcWeaponAudioHasPlayableFireLoop(const OrcWeaponAudioStemContext& ctx);
void OrcWeaponAudioHooksClearShootThrottleState();

bool OrcWeaponAudioIsInterior();
