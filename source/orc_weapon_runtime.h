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
/// После `DiscoverWeaponReplacements` заранее создаёт RwObject клона в `g_heldWeaponReplacements` без подмены `m_pWeaponObject`
/// (полный `OrcPrepareHeldWeaponReplacementBefore` только из `pedRenderEvent`, иначе возможен AV в отрисовке).
void OrcHeldWeaponReplacementWarmupAfterDiscover();

void OrcPruneHeldWeaponReplacementInstances();

void OrcDestroyAllHeldWeaponReplacementInstances();

void OrcSyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress = nullptr);
int OrcRenderPedWeapons(CPed* ped, RenderedWeapon* arr);

void OrcWeaponEnsurePedModelHooksInstalled();
/// Хуки `CWeapon::Fire` / `FireInstantHit`: смещение `muzzlePosn` по Held-позе (FX выстрела: дым, гильзы и т.п.).
void OrcWeaponEnsureFireFxHooksInstalled();
/// Same weapon-type resolution path as held replacement / Guns TXD (`m_aWeapons`, `m_nWeaponModelId`, saved weapon).
int OrcResolveWeaponHeldVisualWeaponType(CPed* ped);
/// If ped slots read empty but Guns held clone exists (SA:MP), reuse its weapon type for HUD icon resolution.
int OrcWeaponHudGetHeldReplacementWeaponTypeIfAny(CPed* ped);
/// Active held replacement key currently captured for ped/weapon (stable key from runtime clone state).
bool OrcGetHeldReplacementKeyForPed(CPed* ped, int wt, std::string& outKeyLower);
void OrcWeaponHudEnsureDrawWeaponIconHookInstalled();

void OrcPrepareHeldWeaponTextureBefore(CPed* ped);
void OrcPrepareHeldWeaponReplacementBefore(CPed* ped);
void OrcRestoreHeldWeaponReplacementAfter(CPed* ped);
/// Сброс «уже применили preRwDraw Held» на игровой тик (`gameProcessEvent`).
void OrcHeldPoseBeginSimFrame();
/// Периодический `held status:` + при `HeldWeaponTrace` уже логируются хуки из `orc_weapon_runtime_held*.cpp`.
void OrcHeldWeaponTraceGameProcessTick();
bool OrcApplyHeldWeaponPoseAdjust(CPed* ped);

/// `__DATE__`/`__TIME__` из `orc_weapon_runtime.cpp` (тонкий TU; при инкрементальной сборке может отличаться от строки в DllMain).
const char* OrcWeaponRuntimeCompileStamp();
