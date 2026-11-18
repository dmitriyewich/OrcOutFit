#pragma once

#include "RenderWare.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

class CPed;
class CPlayerPed;
struct HeldWeaponPoseCfg;
struct WeaponReplacementAsset;

struct OrcHeldGunflash {
    RwFrame* frame = nullptr;
    RwV3d muzzleOffset{};
    bool hasMuzzleOffset = false;
    std::vector<RpAtomic*> atomics;
    std::vector<RwUInt8> alphaScratch;
};

constexpr int OrcWeaponSlotMax = 20;

struct RenderedWeapon {
    bool active = false;
    int weaponType = 0;
    bool secondary = false;
    int modelId = 0;
    RwObject* rwObject = nullptr;
    bool usesReplacementMesh = false;
    /// Selected replacement key remains set for a stock fallback, preventing per-frame destroy/retry churn.
    std::string replacementKey;
    /// Exact stock-material pins owned by this body instance; duplicates are intentional and ref-balanced.
    std::vector<RpMaterial*> pinnedMaterials;
    unsigned nextStockRetryMs = 0;
    bool drawFailed = false;
};

struct PedWeaponCache {
    std::array<RenderedWeapon, OrcWeaponSlotMax> weapons{};
    unsigned lastSyncSerial = 0u;
};

extern RenderedWeapon g_rendered[OrcWeaponSlotMax];
extern std::unordered_map<int, PedWeaponCache> g_otherPedsRendered;

void OrcDestroyRenderedWeapon(RenderedWeapon& r);

void OrcWeaponClearLocalRendered();
void OrcWeaponClearOtherPedsRendered();
void OrcClearAllWeaponReplacementInstances();

void OrcPruneHeldWeaponReplacementInstances();

void OrcDestroyAllHeldWeaponReplacementInstances();

void OrcSyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress = nullptr);
bool OrcWeaponHasCachedBodyWeapons();
/// Draw cached local/remote body weapons as ordinary attachment objects from `drawingEvent`.
/// The caller owns the scoped RenderWare states; each weapon's extra point lights are cleared after its draw.
int OrcRenderCachedBodyWeaponsForAttachmentScene(CPlayerPed* player);
void OrcWeaponEnsureHeldHooksInstalled();
bool OrcCanModifyHeldWeapon(CPed* ped);
void OrcWeaponEnsureFireFxHooksInstalled();
bool OrcPedWantsDualWieldHeld(CPed* ped, int wt);
bool OrcHeldPrepareGunflash(RwObject* object, CPed* ped, int wt,
    WeaponReplacementAsset& asset, OrcHeldGunflash& gunflash);
RwUInt8 OrcHeldGunflashAlpha(const CPed* ped, const OrcHeldGunflash* gunflash, bool left);
void OrcHeldDrawGunflash(OrcHeldGunflash& gunflash, RwUInt8 alpha);
bool OrcTryResolveHeldReplacementGunflash(
    CPed* ped, int weaponType, bool left, RwObject*& clone, const OrcHeldGunflash*& gunflash);
bool OrcBuildHeldWeaponHandMatrix(CPed* ped, bool left, RwMatrix& matrix);
bool OrcApplyHeldWeaponPoseToMatrix(RwMatrix& matrix, const HeldWeaponPoseCfg& pose);
/// Visual weapon type for the current engine-owned held object.
int OrcResolveWeaponHeldVisualWeaponType(CPed* ped);
/// Не рисовать body-attachment для wt, который сейчас в руках (SA:MP: visWt может != curSlot).
void OrcWeaponSuppressBodyForHeldVisualWeapon(CPed* ped, std::vector<char>* suppress);
/// Active held replacement key currently captured for ped/weapon (stable key from runtime clone state).
bool OrcGetHeldReplacementKeyForPed(CPed* ped, int wt, std::string& outKeyLower);
void OrcWeaponHudEnsureDrawWeaponIconHookInstalled();

void OrcPrepareHeldWeaponBeforeRender(CPed* ped);
void OrcInvalidateHeldWeaponDrawPreparation();
void OrcHeldPoseBeginSimFrame();
/// Периодический `held status:` + при `HeldWeaponTrace` уже логируются хуки из `orc_weapon_runtime_held*.cpp`.
void OrcHeldWeaponTraceGameProcessTick();

/// `__DATE__`/`__TIME__` из `orc_weapon_runtime.cpp` (тонкий TU; при инкрементальной сборке может отличаться от строки в DllMain).
const char* OrcWeaponRuntimeCompileStamp();
