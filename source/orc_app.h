#pragma once

#include "orc_types.h"
#include "orc_log.h"
#include "orc_ini.h"

#include <array>
#include <string>
#include <vector>

#include <windows.h>

class CPed;
class CPlayerPed;

extern char g_iniPath[MAX_PATH];
extern char g_gameObjDir[MAX_PATH];
extern char g_gameWeaponsDir[MAX_PATH];
extern char g_gameSkinDir[MAX_PATH];
extern char g_gameTextureDir[MAX_PATH];
extern char g_gameWeaponGunsDir[MAX_PATH];
extern char g_gameWeaponGunsNickDir[MAX_PATH];
extern char g_gameWeaponTexturesDir[MAX_PATH];

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
extern bool g_renderStandardObjects;
extern bool g_weaponReplacementEnabled;
extern bool g_weaponReplacementOnBody;
extern bool g_weaponReplacementInHands;
extern bool g_weaponReplacementHideBaseHeld;
extern bool g_weaponTexturesEnabled;
extern bool g_weaponTextureNickMode;
extern bool g_weaponTextureRandomMode;

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
extern std::vector<StandardObjectSlotCfg> g_standardObjects;
extern std::vector<CustomSkinCfg> g_customSkins;
extern std::vector<StandardSkinCfg> g_standardSkins;
extern bool g_livePreviewObjectActive;
extern std::string g_livePreviewObjectIniPath;
extern std::string g_livePreviewObjectSkinDff;
extern CustomObjectSkinParams g_livePreviewObjectParams;
extern bool g_livePreviewStandardObjectActive;
extern int g_livePreviewStandardObjectModelId;
extern int g_livePreviewStandardObjectSlot;
extern std::string g_livePreviewStandardObjectSkinDff;
extern CustomObjectSkinParams g_livePreviewStandardObjectParams;

extern bool g_skinModeEnabled;
extern bool g_skinHideBasePed;
extern bool g_skinNickMode;
extern bool g_skinLocalPreferSelected;
extern bool g_skinTextureRemapEnabled;
extern bool g_skinTextureRemapNickMode;
extern bool g_skinTextureRemapAutoNickMode;
extern int g_skinTextureRemapRandomMode;
// SKINS/random/<dffBaseName>/*.dff — случайный вариант на каждого педа с этим model id.
extern bool g_skinRandomFromPools;
extern int g_skinRandomPoolModels;
extern int g_skinRandomPoolVariants;
extern std::string g_skinSelectedName;
extern int g_skinSelectedSource;
extern int g_standardSkinSelectedModelId;

// LoadPedObject cache (model id -> ped.dat DFF name); shared with orc_skins.
extern std::vector<std::string> g_pedModelNameById;

bool OrcIsValidStandardSkinModel(int modelId);
void OrcAppendSkinFeatureIniValues(std::vector<OrcIniValue>& values);
void OrcAppendSkinModeIniValues(std::vector<OrcIniValue>& values);
void OrcSkinsRegisterPreviewHook();
void OrcSkinsRenderForPeds(CPlayerPed* localPlayer);
void OrcSkinsDestroyPreview();
void OrcSkinsReleaseAllInstancesAndPreview();
bool OrcSkinsLocalSelectionAddsActiveWork();
void OrcSkinsOnPedRenderBefore(CPed* ped);
void OrcSkinsOnPedRenderAfter(CPed* ped);
void OrcSkinsShutdown();

// Shared with orc_ui.cpp (skin list selection)
extern int g_uiSkinIdx;
extern int g_uiSkinEditIdx;
extern int g_uiCustomIdx;

void LoadConfig();
void RefreshActivationRouting();
void DiscoverCustomObjectsAndEnsureIni();
void DiscoverCustomSkins();
void DiscoverWeaponReplacements();
void DiscoverWeaponTextures();
void LoadStandardObjectsFromIni();
void LoadStandardSkinsFromIni();
void SaveStandardObjectListToIni();
bool AddStandardObjectSlot(int modelId);
void RemoveStandardObjectSlot(size_t index);
bool IsValidStandardObjectModel(int modelId);

void SaveWeaponSection(int weaponIndex);
void SaveWeaponSection2(int weaponIndex);
void SaveAllWeaponsToIniFile(const char* iniPath, const std::vector<WeaponCfg>& w1, const std::vector<WeaponCfg>& w2);
void SaveSkinCfgToIni(const CustomSkinCfg& s);
void SaveStandardSkinCfgToIni(const StandardSkinCfg& s);
void SaveSkinModeIni();
void SaveMainIni();

void InvalidatePerSkinWeaponCache();
void InvalidateCustomSkinLookupCache();
void InvalidateObjectSkinParamCache();
void InvalidateStandardObjectSkinParamCache();
void InvalidateStandardSkinLookupCache();

void OrcLoadWeaponPresetFile(const char* fullPath, std::vector<WeaponCfg>& w1, std::vector<WeaponCfg>& w2);

// ped.dat DFF basename for ped (LoadPedObject hook); empty if unknown.
const char* OrcTryGetPedModelNameById(int modelId);
std::string GetPedStdSkinDffName(CPed* ped);
bool ResolveWeaponsIniForSkinDff(const char* skinDffName, char* outPath, size_t outPathChars);
bool OrcApplyLocalPlayerModelById(int modelId);
int OrcGetLocalPlayerModelId();

// Ped.dat DFF names with model ids (for UI lists).
void OrcCollectPedSkins(std::vector<std::pair<std::string, int>>& out);
void OrcCollectRandomSkinPools(std::vector<SkinRandomPoolInfo>& out);
void OrcCollectRandomSkinPreviewVariants(std::vector<SkinPreviewRandomVariantInfo>& out);
WeaponReplacementStats OrcGetWeaponReplacementStats();
WeaponTextureStats OrcGetWeaponTextureStats();
void OrcRequestSkinPreview(int source, int modelId, int variantIndex, const char* name, int width, int height, float yawDeg);
void* OrcGetSkinPreviewTexture();
void OrcCollectPedTextureRemapStats(std::vector<TextureRemapPedInfo>& out);
bool OrcGetLocalPedTextureRemaps(TextureRemapPedInfo& out);
bool OrcSetLocalPedTextureRemap(int slot, int remap);
bool OrcRandomizeLocalPedTextureRemaps();
bool OrcSetAllLocalPedTextureRemaps(int remap);
void OrcReloadTextureRemapNickBindings();
void OrcCollectLocalPedTextureRemapNickBindings(std::vector<TextureRemapNickBindingInfo>& out);
void OrcFlushDeferredHeldWeaponSlotRestore();
bool OrcSaveLocalPedTextureRemapNickBinding(const char* nickCsv);
bool OrcDeleteLocalPedTextureRemapNickBinding(int bindingId);

bool LoadObjectSkinParamsFromIni(const char* iniPath, const char* skinDffName, CustomObjectSkinParams& out);
void SaveObjectSkinParamsToIni(const char* iniPath, const char* skinDffName, const CustomObjectSkinParams& p);
bool LoadStandardObjectSkinParamsFromIni(int modelId, int slot, const char* skinDffName, CustomObjectSkinParams& out);
void SaveStandardObjectSkinParamsToIni(int modelId, int slot, const char* skinDffName, const CustomObjectSkinParams& p);

bool OrcIsValidStandardPedModelForLocalApply(int modelId);
void OrcObjectsBeginFrame();
void OrcObjectsReleaseAllInstances();
void OrcObjectsDestroyAllStandardInstances();
void OrcObjectsApplyWeaponSuppression(CPed* ped, std::vector<char>* suppress);
void OrcObjectsPrepassLocalPlayer(CPlayerPed* player, int& active, std::vector<char>& objectUsed);
void OrcObjectsRenderLocalPlayer(CPlayerPed* player, std::vector<char>& objectUsed);
void OrcObjectsRenderForRemotePed(CPed* ped, std::vector<char>& objectUsed);
void OrcObjectsFinalizeFrame(std::vector<char>& objectUsed);
void OrcObjectsWhenSkippingRenderNoActive();
void OrcObjectsShutdown();
StandardSkinCfg* OrcGetStandardSkinCfgByModelId(int modelId, bool createIfMissing);

std::vector<std::string> ParseNickCsv(const std::string& csv);

const WeaponCfg& GetWeaponCfgForPed(CPed* ped, int wt);
const WeaponCfg& GetWeaponCfg2ForPed(CPed* ped, int wt);
const char* VkToString(int vk);
int ParseActivationVk(const char* text);
