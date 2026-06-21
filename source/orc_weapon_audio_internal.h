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
class CAEAudioEntity;
class CAEWeaponAudioEntity;
class CAESound;
struct CVector;

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
};

extern HMODULE g_pluginModule;
extern bool g_hooksInstalled;
extern bool g_hitHookInstalled;
extern DWORD g_suppressVanillaGunSoundsUntilTick;

extern std::vector<OrcWeaponAudioSourceSlot> g_ephemeralSources;
extern std::mutex g_ephemeralMutex;
extern std::vector<ALuint> g_loopSources;
extern std::mutex g_loopMutex;
extern std::unordered_map<std::string, ALuint> g_bufferByPath;
extern std::mutex g_bufferMutex;

ALuint OrcGetOrCreateBufferForPath(const char* pathUtf8);

void OrcWeaponAudioStopAllLoopSources();
bool OrcWeaponAudioStartLoopSource(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped, ALuint& inOutSource);
void OrcWeaponAudioStopLoopSource(ALuint& inOutSource);
bool OrcWeaponAudioIsLoopSourcePlaying(ALuint source);
void OrcWeaponAudioUpdateLoopSources();
void OrcWeaponAudioSyncLoopSourceWorldPos(ALuint source, CPed* ped, float gain);

bool OrcWeaponAudioOpenAlInit();
void OrcWeaponAudioOpenAlShutdown();
bool OrcWeaponAudioHasActiveContext();
bool OrcWeaponAudioEnsureAlContextCurrent();
void OrcWeaponAudioPruneEphemeralSources();
void OrcWeaponAudioStopEphemeralSources();
bool OrcWeaponAudioPlayBuffer(ALuint buffer, const OrcWeaponAudioPlayParams& params, CPed* ped);

CPed* OrcWeaponAudioPedFromPhysical(CPhysical* physical);
CPed* OrcWeaponAudioPedFromWeaponAudio(CAEWeaponAudioEntity* self);
CPed* OrcWeaponAudioValidatePedCandidate(CPed* ped, const char* source);
/// Пед из `CAESound::m_pBaseAudio` (сначала `m_pEntity`, затем weapon `m_pPed`).
CPed* OrcWeaponAudioResolvePedFromSoundBase(CAEAudioEntity* base);
bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out);
bool OrcWeaponAudioPedHasReplacementAudio(CPed* ped, int weaponType);
bool OrcWeaponAudioHandleVanillaSfx(CAESound* snd);
/// Первый существующий файл `stem+suffix` с расширением .wav / .mp3 / .flac / .ogg.
bool OrcWeaponAudioResolveFirstExistingAudioPath(const OrcWeaponAudioStemContext& ctx, const char* suffix, std::string& outPath);
bool OrcWeaponAudioPathExistsCached(const std::string& path);
bool OrcWeaponAudioTryPlaySuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, float gainScale, OrcWeaponSpatial spatial);
bool OrcWeaponAudioTryPlayPath(const char* pathUtf8, const OrcWeaponAudioPlayParams& params, CPed* ped);
void OrcWeaponAudioMarkSuppressVanilla();
bool OrcWeaponAudioHasFireRelatedCustomAudio(const OrcWeaponAudioStemContext& ctx);
bool OrcWeaponAudioShouldSuppressVanillaGun(class CAEWeaponAudioEntity* self);
bool OrcWeaponAudioHasLoopCustomAudio(const OrcWeaponAudioStemContext& ctx);
bool OrcWeaponAudioShouldSkipWeaponFireOneShot(int weaponType, const OrcWeaponAudioStemContext& ctx);
bool OrcWeaponAudioIsLocalPlayerPed(CPed* ped);
float OrcWeaponAudioLocalPedDistance(CPed* ped);
bool OrcWeaponAudioIsBeyondMaxDistance(CPed* ped, float maxDist);
float OrcWeaponAudioApplyDistanceCullGain(CPed* ped, float maxDist, float gain);
OrcWeaponSpatial OrcWeaponAudioSpatialForPed(CPed* ped);

void OrcWeaponAudioLoopsEnsureInstalled();
void OrcWeaponAudioLoopsShutdown();
void OrcWeaponAudioLoopsOnGameProcess();
void OrcWeaponAudioLoopsStopForPed(CPed* ped);
void OrcWeaponAudioLoopsStopAll();
bool OrcWeaponAudioLoopsTryPlayMinigunFireForPed(CPed* ped, class CAEWeaponAudioEntity* audioEntity = nullptr);
void OrcWeaponAudioLoopsStopMinigunForPed(CPed* ped);
bool OrcWeaponAudioPedHasCustomMinigunFireloop(CPed* ped);
bool OrcWeaponAudioPedIsMinigunFiring(CPed* ped);
bool OrcWeaponAudioPedWantsMinigunFireLoop(CPed* ped);
void OrcWeaponAudioHooksClearShootThrottleState();

bool OrcWeaponAudioIsInterior();
