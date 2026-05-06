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
extern bool g_weaponReplacementRandomIncludeVanilla;
extern bool g_weaponTexturesEnabled;
extern bool g_weaponTextureNickMode;
extern bool g_weaponTextureRandomMode;
/// Use `*_remap` pairs inside the game's loaded weapon model TXD (PedFuncs-style per slot).
extern bool g_weaponTextureStandardRemap;
/// HUD `DrawWeaponIcon`: if Guns/replacement dictionary has `<weapon>icon`, use as current TXD for the call (local player).
extern bool g_weaponHudIconFromGunsTxd;
/// Подробный лог «В руке»: `held chain:` / `held pose:` (в т.ч. phase=preRwDraw у RpClumpRender / AtomicDefaultRender), throttle короче.
/// `[Features] HeldPoseDebug=1` и `DebugLogLevel=2` в `OrcOutFit.ini`.
extern bool g_heldPoseDebug;
/// Трассировка оружия в руке: статус раз в N мс + детальный лог хуков `RenderWeaponPedsForPC` / `RenderWeaponCB`.
/// `[Features] HeldWeaponTrace=0` выкл; `1` — интервал статуса + хуки с throttle; `2` — чаще строки по хукам.
extern int g_heldWeaponTrace;
/// Интервал строки `held status:` (мс), по умолчанию 10000. Минимум 3000 при включённом HeldWeaponTrace.
extern int g_heldWeaponStatusIntervalMs;

extern std::vector<WeaponCfg> g_cfg;
extern std::vector<WeaponCfg> g_cfg2;
extern bool g_livePreviewWeaponsActive;
extern std::string g_livePreviewWeaponSkinDff;
extern std::vector<WeaponCfg> g_livePreviewWeapon1;
extern std::vector<WeaponCfg> g_livePreviewWeapon2;
extern bool g_livePreviewHeldActive;
extern std::string g_livePreviewHeldSkinDff;
extern std::vector<HeldWeaponPoseCfg> g_livePreviewHeld1;
extern std::vector<HeldWeaponPoseCfg> g_livePreviewHeld2;
/// Вкладка «В руке»: пользователь крутит Held для строки «… 2» / второго набора — live preview читает `g_livePreviewHeld2`.
extern bool g_livePreviewHeldUseSecondary;
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
void OrcSkinsRenderForPeds(CPlayerPed* localPlayer);
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
void SaveAllWeaponsToIniFile(const char* iniPath, const std::vector<WeaponCfg>& w1, const std::vector<WeaponCfg>& w2,
    const std::vector<HeldWeaponPoseCfg>* held1 = nullptr, const std::vector<HeldWeaponPoseCfg>* held2 = nullptr);
void SaveSkinCfgToIni(const CustomSkinCfg& s);
void SaveStandardSkinCfgToIni(const StandardSkinCfg& s);
void SaveSkinModeIni();
void SaveMainIni();

void InvalidatePerSkinWeaponCache();
void InvalidateCustomSkinLookupCache();
void InvalidateObjectSkinParamCache();
void InvalidateStandardObjectSkinParamCache();
void InvalidateStandardSkinLookupCache();

void OrcLoadWeaponPresetFile(const char* fullPath, std::vector<WeaponCfg>& w1, std::vector<WeaponCfg>& w2,
    std::vector<HeldWeaponPoseCfg>* outHeld1 = nullptr, std::vector<HeldWeaponPoseCfg>* outHeld2 = nullptr);

// ped.dat DFF basename for ped (LoadPedObject hook); empty if unknown.
const char* OrcTryGetPedModelNameById(int modelId);
std::string GetPedStdSkinDffName(CPed* ped);
/// Имя для пресета оружия / выбранного скина в UI. Для секций `[Skin.*]` объектов рантайм сначала пробует
/// `GetPedStdSkinDffName`, затем (если отличается) это значение — как `ResolveWeaponsPresetIniForPed`.
std::string GetWeaponSkinIniLookupName(CPed* ped);
bool ResolveWeaponsIniForSkinDff(const char* skinDffName, char* outPath, size_t outPathChars);
/// `Weapons\<>.ini` для пресета педа: сначала DFF из LoadPedObject (`GetPedStdSkinDffName`), иначе `GetWeaponSkinIniLookupName`.
/// `outKeyLower` — нижний регистр базового имени найденного файла (ключ кеша пер-скин пресетов).
bool ResolveWeaponsPresetIniForPed(CPed* ped, char* outPath, size_t outPathChars, std::string* outKeyLower);
bool OrcApplyLocalPlayerModelById(int modelId);
int OrcGetLocalPlayerModelId();

// Ped.dat DFF names with model ids (for UI lists).
void OrcCollectPedSkins(std::vector<std::pair<std::string, int>>& out);
void OrcCollectRandomSkinPools(std::vector<SkinRandomPoolInfo>& out);
WeaponReplacementStats OrcGetWeaponReplacementStats();
WeaponTextureStats OrcGetWeaponTextureStats();
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
/// `secondary`: пресет с диска — вторая строка (twin / секции `*_2` в Weapons\<skin>.ini). Рантайм RWCB/слот пока всегда `false`.
const HeldWeaponPoseCfg& GetHeldPoseForPed(CPed* ped, int wt, bool secondary);
/// Сброс live-preview оружия/«В руке», когда меню закрыто (иначе INI с диска может игнорироваться).
void OrcClearWeaponUiLivePreviewWhenMenuClosed();
/// Детальный лог, почему пресет held не активен (throttled внутри).
void OrcLogHeldPoseCfgDisabled(CPed* ped, int wt);
const char* VkToString(int vk);
int ParseActivationVk(const char* text);
