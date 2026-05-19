#pragma once

#include "RenderWare.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

class CPed;

constexpr int OrcWeaponSlotMax = 20;

struct RenderedWeapon {
    bool active = false;
    int weaponType = 0;
    bool secondary = false;
    int modelId = 0;
    int slot = 0;
    RwObject* rwObject = nullptr;
    std::string replacementKey;
};

extern RenderedWeapon g_rendered[OrcWeaponSlotMax];
extern std::unordered_map<int, std::array<RenderedWeapon, OrcWeaponSlotMax>> g_otherPedsRendered;

void OrcDestroyRenderedWeapon(RenderedWeapon& r);

void OrcWeaponClearLocalRendered();
void OrcWeaponClearOtherPedsRendered();
void OrcClearAllWeaponReplacementInstances();

void OrcPruneHeldWeaponReplacementInstances();

void OrcDestroyAllHeldWeaponReplacementInstances();

void OrcSyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress = nullptr);
int OrcRenderPedWeapons(CPed* ped, RenderedWeapon* arr);

void OrcWeaponEnsurePedModelHooksInstalled();
/// Хуки `CWeapon::Fire` / `FireInstantHit`: смещение `muzzlePosn` по Held-позе; синхронизация `m_pGunflashObject` в `orc_weapon_runtime_held_fx.cpp`.
void OrcWeaponEnsureFireFxHooksInstalled();
void OrcWeaponEnsureGunflashHooksInstalled();
bool OrcPedWantsDualWieldHeld(CPed* ped, int wt);
RwFrame* OrcPedResolveGunflashFrameForDualHand(CPed* ped, int wtHint, bool isLeftHand);
/// Мировая дельта точки `m_vecFireOffset` (как в `muzzlePosn`): Held − ванильная кость R_Hand; `false` если Held выкл. или нет данных.
bool OrcHeldTryGetMuzzleWorldDeltaHeldMinusVanilla(CPed* ped, int wt, RwV3d* outDw);
/// Клумп, в котором реально ищется dummy `gunflash` при Guns held replacement: если в слоте сток, а меш — клон, возвращается клон (RWCB / DoGunFlash).
RpClump* OrcPedResolveGunflashTargetClump(CPed* ped, int wtHint = 0);
/// После подмены `m_pWeaponObject` (клон/сток) перевыставить `m_pGunflashObject` на кадр `"gunflash"` в текущем клумпе (как в ванильном `AddWeaponModel`).
void OrcPedSyncGunflashFrameFromCurrentWeaponObject(CPed* ped, int wtHint = 0);
/// Сдвиг кадра `gunflash` на muzzle-дельту Held (сброс дедупа на клумпе); после `OrcPedSyncGunflashFrameFromCurrentWeaponObject` в `Fire` / RWCB.
void OrcHeldNudgeGunflashMuzzleDeltaAfterFrameSync(CPed* ped, int wt);
/// Same weapon-type resolution path as held replacement / Guns TXD (`m_aWeapons`, `m_nWeaponModelId`, saved weapon).
int OrcResolveWeaponHeldVisualWeaponType(CPed* ped);
/// Не рисовать body-attachment для wt, который сейчас в руках (SA:MP: visWt может != curSlot).
void OrcWeaponSuppressBodyForHeldVisualWeapon(CPed* ped, std::vector<char>* suppress);
/// If ped slots read empty but Guns held clone exists (SA:MP), reuse its weapon type for HUD icon resolution.
int OrcWeaponHudGetHeldReplacementWeaponTypeIfAny(CPed* ped);
/// Active held replacement key currently captured for ped/weapon (stable key from runtime clone state).
bool OrcGetHeldReplacementKeyForPed(CPed* ped, int wt, std::string& outKeyLower);
void OrcWeaponHudEnsureDrawWeaponIconHookInstalled();

void OrcPrepareHeldWeaponTextureBefore(CPed* ped);
void OrcPrepareHeldWeaponReplacementBefore(CPed* ped);
void OrcRestoreHeldWeaponReplacementAfter(CPed* ped);
void OrcHeldDrainDeferredDualSecondaryDraws(CPed* ped);
/// Сброс «уже применили preRwDraw Held» на игровой тик (`gameProcessEvent`).
void OrcHeldPoseBeginSimFrame();
/// Периодический `held status:` + при `HeldWeaponTrace` уже логируются хуки из `orc_weapon_runtime_held*.cpp`.
void OrcHeldWeaponTraceGameProcessTick();
bool OrcApplyHeldWeaponPoseAdjust(CPed* ped);
bool OrcHeldTryGetPoseEngineBaselineForFrame(RwFrame* frame, RwMatrix& out);

/// `__DATE__`/`__TIME__` из `orc_weapon_runtime.cpp` (тонкий TU; при инкрементальной сборке может отличаться от строки в DllMain).
const char* OrcWeaponRuntimeCompileStamp();
