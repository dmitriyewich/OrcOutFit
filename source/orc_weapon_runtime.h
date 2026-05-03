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

void OrcPrepareHeldWeaponTextureBefore(CPed* ped);
void OrcPrepareHeldWeaponReplacementBefore(CPed* ped);
void OrcRestoreHeldWeaponReplacementAfter(CPed* ped);
