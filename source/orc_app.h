#pragma once

#include "orc_types.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

extern char g_iniPath[MAX_PATH];
extern char g_gameObjDir[MAX_PATH];
extern char g_gameSkinDir[MAX_PATH];

extern bool g_enabled;
extern bool g_renderAllPedsWeapons;
extern float g_renderAllPedsRadius;
extern int g_activationVk;
extern bool g_sampAllowActivationKey;
extern std::string g_toggleCommand;
extern bool g_considerWeaponSkills;

extern std::vector<WeaponCfg> g_cfg;
extern std::vector<WeaponCfg> g_cfg2;
// Weapon types discovered in current game (weapon.dat / modded weapon.dat).
extern std::vector<int> g_availableWeaponTypes;
// Cached model ids for each weapon type (same indexing as g_cfg).
extern std::vector<int> g_weaponModelId;
extern std::vector<int> g_weaponModelId2;

extern std::vector<CustomObjectCfg> g_customObjects;
extern std::vector<CustomSkinCfg> g_customSkins;

extern bool g_skinModeEnabled;
extern bool g_skinHideBasePed;
extern bool g_skinNickMode;
extern bool g_skinLocalPreferSelected;
// SKINS/random/<dffBaseName>/*.dff — случайный вариант на каждого педа с этим model id.
extern bool g_skinRandomFromPools;
extern int g_skinRandomPoolModels;
extern int g_skinRandomPoolVariants;
extern std::string g_skinSelectedName;

// Shared with orc_ui.cpp (skin list selection)
extern int g_uiSkinIdx;
extern int g_uiSkinEditIdx;

extern std::unordered_map<unsigned int, SkinOtherOverrides> g_otherByModelKey;

void LoadConfig();
void DiscoverCustomObjectsAndEnsureIni();
void DiscoverCustomSkins();
void DiscoverOtherOverridesAndObjects();

// For UI: ensure per-skin overrides entry for currently local player model.
SkinOtherOverrides* EnsureOtherOverridesForLocalSkin();

// Persist `so.weaponCfg[]` into `so.weaponsIniPath`.
void SaveOtherSkinWeaponsIni(const SkinOtherOverrides& so);

void SaveWeaponSection(int weaponIndex);
void SaveWeaponSection2(int weaponIndex);
void SaveCustomObjectIni(const CustomObjectCfg& o);
void SaveSkinCfgToIni(const CustomSkinCfg& s);
void SaveSkinModeIni();
void SaveMainIni();

std::vector<std::string> ParseNickCsv(const std::string& csv);
const char* VkToString(int vk);
