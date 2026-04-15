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

extern WeaponCfg g_cfg[64];

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

extern std::unordered_map<unsigned int, std::array<WeaponCfg, 64>> g_weaponCfgByModelKey;

// Shared with orc_ui.cpp (skin list selection)
extern int g_uiSkinIdx;
extern int g_uiSkinEditIdx;

void LoadConfig();
void DiscoverCustomObjectsAndEnsureIni();
void DiscoverCustomSkins();

void SaveWeaponSection(int weaponIndex);
void SaveCustomObjectIni(const CustomObjectCfg& o);
void SaveSkinCfgToIni(const CustomSkinCfg& s);
void SaveSkinModeIni();

std::vector<std::string> ParseNickCsv(const std::string& csv);
const char* VkToString(int vk);
