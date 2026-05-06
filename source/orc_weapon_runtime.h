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
/// Same weapon-type resolution path as held replacement / Guns TXD (`m_aWeapons`, `m_nWeaponModelId`, saved weapon).
int OrcResolveWeaponHeldVisualWeaponType(CPed* ped);
/// If ped slots read empty but Guns held clone exists (SA:MP), reuse its weapon type for HUD icon resolution.
int OrcWeaponHudGetHeldReplacementWeaponTypeIfAny(CPed* ped);
void OrcWeaponHudEnsureDrawWeaponIconHookInstalled();

void OrcPrepareHeldWeaponTextureBefore(CPed* ped);
void OrcPrepareHeldWeaponReplacementBefore(CPed* ped);
void OrcRestoreHeldWeaponReplacementAfter(CPed* ped);
/// Сброс «уже применили preRwDraw Held» на игровой тик (`gameProcessEvent`).
void OrcHeldPoseBeginSimFrame();
/// Периодический `held status:` + при `HeldWeaponTrace` уже логируются хуки из `orc_weapon_runtime.cpp`.
void OrcHeldWeaponTraceGameProcessTick();
bool OrcApplyHeldWeaponPoseAdjust(CPed* ped);
