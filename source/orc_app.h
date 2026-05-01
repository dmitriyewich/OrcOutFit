#pragma once

#include "orc_types.h"
#include "orc_log.h"

#include <array>
#include <string>
#include <vector>

#include <windows.h>

class CPed;

extern char g_iniPath[MAX_PATH];
extern char g_gameObjDir[MAX_PATH];
extern char g_gameWeaponsDir[MAX_PATH];
extern char g_gameSkinDir[MAX_PATH];
extern char g_gameTextureDir[MAX_PATH];

extern bool g_enabled;
extern bool g_renderAllPedsWeapons;
extern bool g_renderAllPedsObjects;
extern float g_renderAllPedsRadius;
extern int g_activationVk;
extern bool g_sampAllowActivationKey;
extern std::string g_toggleCommand;
extern bool g_uiAutoScale;
extern float g_uiScale;
extern float g_uiFontSize;
extern bool g_considerWeaponSkills;
extern bool g_renderCustomObjects;

extern std::vector<WeaponCfg> g_cfg;
extern std::vector<WeaponCfg> g_cfg2;
extern bool g_livePreviewWeaponsActive;
extern std::string g_livePreviewWeaponSkinDff;
extern std::vector<WeaponCfg> g_livePreviewWeapon1;
extern std::vector<WeaponCfg> g_livePreviewWeapon2;
// Weapon types discovered in current game (weapon.dat / modded weapon.dat).
extern std::vector<int> g_availableWeaponTypes;
// Cached model ids for each weapon type (same indexing as g_cfg).
extern std::vector<int> g_weaponModelId;
extern std::vector<int> g_weaponModelId2;

extern std::vector<CustomObjectCfg> g_customObjects;
extern std::vector<CustomSkinCfg> g_customSkins;
extern bool g_livePreviewObjectActive;
extern std::string g_livePreviewObjectIniPath;
extern std::string g_livePreviewObjectSkinDff;
extern CustomObjectSkinParams g_livePreviewObjectParams;

extern bool g_skinModeEnabled;
extern bool g_skinHideBasePed;
extern bool g_skinNickMode;
extern bool g_skinLocalPreferSelected;
extern bool g_skinTextureRemapEnabled;
extern bool g_skinTextureRemapNickMode;
extern int g_skinTextureRemapRandomMode;
// SKINS/random/<dffBaseName>/*.dff — случайный вариант на каждого педа с этим model id.
extern bool g_skinRandomFromPools;
extern int g_skinRandomPoolModels;
extern int g_skinRandomPoolVariants;
extern std::string g_skinSelectedName;

// Shared with orc_ui.cpp (skin list selection)
extern int g_uiSkinIdx;
extern int g_uiSkinEditIdx;

void LoadConfig();
void RefreshActivationRouting();
void DiscoverCustomObjectsAndEnsureIni();
void DiscoverCustomSkins();

void SaveWeaponSection(int weaponIndex);
void SaveWeaponSection2(int weaponIndex);
void SaveAllWeaponsToIniFile(const char* iniPath, const std::vector<WeaponCfg>& w1, const std::vector<WeaponCfg>& w2);
void SaveSkinCfgToIni(const CustomSkinCfg& s);
void SaveSkinModeIni();
void SaveMainIni();

void InvalidatePerSkinWeaponCache();
void InvalidateCustomSkinLookupCache();
void InvalidateObjectSkinParamCache();

void OrcLoadWeaponPresetFile(const char* fullPath, std::vector<WeaponCfg>& w1, std::vector<WeaponCfg>& w2);

// ped.dat DFF basename for ped (LoadPedObject hook); empty if unknown.
const char* OrcTryGetPedModelNameById(int modelId);
std::string GetPedStdSkinDffName(CPed* ped);
bool ResolveWeaponsIniForSkinDff(const char* skinDffName, char* outPath, size_t outPathChars);
bool OrcApplyLocalPlayerModelById(int modelId);

// Ped.dat DFF names with model ids (for UI lists).
void OrcCollectPedSkins(std::vector<std::pair<std::string, int>>& out);
void OrcCollectPedTextureRemapStats(std::vector<TextureRemapPedInfo>& out);
bool OrcGetLocalPedTextureRemaps(TextureRemapPedInfo& out);
bool OrcSetLocalPedTextureRemap(int slot, int remap);
bool OrcRandomizeLocalPedTextureRemaps();
bool OrcSetAllLocalPedTextureRemaps(int remap);
void OrcReloadTextureRemapNickBindings();
void OrcCollectLocalPedTextureRemapNickBindings(std::vector<TextureRemapNickBindingInfo>& out);
bool OrcSaveLocalPedTextureRemapNickBinding(const char* nickCsv);
bool OrcDeleteLocalPedTextureRemapNickBinding(int bindingId);

bool LoadObjectSkinParamsFromIni(const char* iniPath, const char* skinDffName, CustomObjectSkinParams& out);
void SaveObjectSkinParamsToIni(const char* iniPath, const char* skinDffName, const CustomObjectSkinParams& p);

std::vector<std::string> ParseNickCsv(const std::string& csv);
const char* VkToString(int vk);
int ParseActivationVk(const char* text);
