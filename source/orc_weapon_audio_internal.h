#pragma once

#include "orc_weapon_assets.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

class CPed;
struct CVector;

using ALuint = unsigned int;

enum class OrcWeaponSpatial : uint8_t {
    ListenerRelative,
    WorldAtPed,
};

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

ALuint OrcGetOrCreateBufferForWav(const char* path);
void OrcWeaponAudioStopAllLoopSources();
bool OrcWeaponAudioStartLoopSource(ALuint buffer, float gain, CPed* ped, ALuint& inOutSource);
void OrcWeaponAudioStopLoopSource(ALuint& inOutSource);
void OrcWeaponAudioUpdateLoopSources();
void OrcWeaponAudioSyncLoopSourceWorldPos(ALuint source, CPed* ped, float gain);

bool OrcWeaponAudioOpenAlInit();
void OrcWeaponAudioOpenAlShutdown();
bool OrcWeaponAudioHasActiveContext();
bool OrcWeaponAudioEnsureAlContextCurrent();
void OrcWeaponAudioPruneEphemeralSources();
void OrcWeaponAudioStopEphemeralSources();
bool OrcWeaponAudioPlayBuffer(ALuint buffer, float gain, OrcWeaponSpatial spatial, CPed* ped);

bool OrcWeaponAudioTryBuildStemContext(CPed* ped, int weaponType, OrcWeaponAudioStemContext& out);
std::string OrcWeaponAudioResolvePath(const OrcWeaponAudioStemContext& ctx, const char* suffix);
bool OrcWeaponAudioPathExistsCached(const std::string& path);
bool OrcWeaponAudioTryPlaySuffix(const OrcWeaponAudioStemContext& ctx, const char* suffix, float gainScale,
    OrcWeaponSpatial spatial);
bool OrcWeaponAudioTryPlayPath(const char* path, float gainScale, OrcWeaponSpatial spatial, CPed* ped);
void OrcWeaponAudioMarkSuppressVanilla();
bool OrcWeaponAudioHasFireRelatedCustomWav(const OrcWeaponAudioStemContext& ctx);
bool OrcWeaponAudioShouldSuppressVanillaGun(class CAEWeaponAudioEntity* self);
bool OrcWeaponAudioHasLoopCustomWav(const OrcWeaponAudioStemContext& ctx);
bool OrcWeaponAudioShouldSkipWeaponFireOneShot(int weaponType, const OrcWeaponAudioStemContext& ctx);

void OrcWeaponAudioLoopsEnsureInstalled();
void OrcWeaponAudioLoopsShutdown();
void OrcWeaponAudioLoopsOnGameProcess();
void OrcWeaponAudioLoopsStopForPed(CPed* ped);
void OrcWeaponAudioLoopsStopAll();
void OrcWeaponAudioLoopsOnPlayGunSounds(class CAEWeaponAudioEntity* self);

float OrcWeaponAudioCamPedDistance(CPed* ped);
bool OrcWeaponAudioIsInterior();
