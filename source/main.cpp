// OrcOutFit — рисует оружие/объекты/скины на локальном игроке.
// Использует plugin-sdk и логику BaseModelRender.

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPlayerInfo.h"
#include "CWeaponInfo.h"
#include "CFileLoader.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "eModelInfoType.h"
#include "CPools.h"
#include "CVisibilityPlugins.h"
#include "CPointLights.h"
#include "CTxdStore.h"
#include "CScene.h"
#include "CEntity.h"
#include "RenderWare.h"
#include "ePedType.h"
#include "eWeaponType.h"
#include "game_sa/rw/rphanim.h"
#include "game_sa/rw/rpskin.h"
#include "extensions/ScriptCommands.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <utility>

#include "overlay.h"
#include "samp_bridge.h"
#include "orc_types.h"
#include "orc_app.h"
#include "orc_ini.h"
#include "orc_locale.h"
#include "orc_ui.h"
#include "orc_texture_remap.h"
#include "external/MinHook/include/MinHook.h"

using namespace plugin;

static CdeclEvent<AddressList<0x53EA12, H_CALL>, PRIORITY_BEFORE, ArgPickNone, void()> g_standardSkinPreviewRenderEvent;

static HMODULE g_module = nullptr;
char    g_iniPath[MAX_PATH] = {};
char    g_gameObjDir[MAX_PATH] = {};
char    g_gameWeaponsDir[MAX_PATH] = {};
char    g_gameSkinDir[MAX_PATH] = {};
char    g_gameTextureDir[MAX_PATH] = {};
char    g_gameWeaponGunsDir[MAX_PATH] = {};
char    g_gameWeaponGunsNickDir[MAX_PATH] = {};
char    g_gameWeaponTexturesDir[MAX_PATH] = {};

static void LogInit() {
    char modPath[MAX_PATH] = {};
    GetModuleFileNameA(g_module, modPath, MAX_PATH);
    char moduleDir[MAX_PATH] = {};
    _snprintf_s(moduleDir, _TRUNCATE, "%s", modPath);
    char* slash = strrchr(moduleDir, '\\');
    if (!slash) slash = strrchr(moduleDir, '/');
    if (slash) *slash = 0;

    char* dot = strrchr(modPath, '.');
    if (dot) *dot = 0;
    _snprintf_s(g_iniPath, _TRUNCATE, "%s.ini", modPath);
    OrcLogReloadFromIni(g_iniPath);
    // Relative to plugin location (modloader-friendly):
    _snprintf_s(g_gameObjDir, _TRUNCATE, "%s\\OrcOutFit\\Objects", moduleDir);
    _snprintf_s(g_gameWeaponsDir, _TRUNCATE, "%s\\OrcOutFit\\Weapons", moduleDir);
    _snprintf_s(g_gameWeaponGunsDir, _TRUNCATE, "%s\\OrcOutFit\\Weapons\\Guns", moduleDir);
    _snprintf_s(g_gameWeaponGunsNickDir, _TRUNCATE, "%s\\OrcOutFit\\Weapons\\GunsNick", moduleDir);
    _snprintf_s(g_gameWeaponTexturesDir, _TRUNCATE, "%s\\OrcOutFit\\Weapons\\Textures", moduleDir);
    _snprintf_s(g_gameSkinDir, _TRUNCATE, "%s\\OrcOutFit\\Skins", moduleDir);
    _snprintf_s(g_gameTextureDir, _TRUNCATE, "%s\\OrcOutFit\\Skins\\Textures", moduleDir);

    std::srand(static_cast<unsigned>(GetTickCount()));
}

// ----------------------------------------------------------------------------
// Config: per-weapon attachment (типы и кости: orc_types.h)
// ----------------------------------------------------------------------------
static RpAtomic* InitAtomicCB(RpAtomic* a, void*);
static RpAtomic* InitAttachmentAtomicCB(RpAtomic* a, void*);
bool g_enabled = true;
bool g_renderAllPedsWeapons = false;
bool g_renderAllPedsObjects = false;
float g_renderAllPedsRadius = 80.0f;
int  g_activationVk = VK_F7;
bool g_sampAllowActivationKey = false;
std::string g_toggleCommand = "/orcoutfit";
bool g_uiAutoScale = false;
float g_uiScale = 1.0f;
float g_uiFontSize = 15.0f;
bool g_considerWeaponSkills = true;
bool g_renderCustomObjects = true;
bool g_renderStandardObjects = true;
bool g_weaponReplacementEnabled = true;
bool g_weaponReplacementOnBody = true;
bool g_weaponReplacementInHands = true;
bool g_weaponTexturesEnabled = true;
bool g_weaponTextureNickMode = true;
bool g_weaponTextureRandomMode = true;
std::vector<WeaponCfg> g_cfg;
std::vector<WeaponCfg> g_cfg2; // secondary dual-wield placement
bool g_livePreviewWeaponsActive = false;
std::string g_livePreviewWeaponSkinDff;
std::vector<WeaponCfg> g_livePreviewWeapon1;
std::vector<WeaponCfg> g_livePreviewWeapon2;
std::vector<int> g_availableWeaponTypes;
std::vector<int> g_weaponModelId;
std::vector<int> g_weaponModelId2;
static std::vector<std::string> g_weaponNameStore;
static bool g_weaponTypesReady = false;

// --- weapon.dat ground-truth (filled by LoadWeaponObject hook) ---
static bool g_weaponDatHookInstalled = false;
static int(__cdecl* g_LoadWeaponObject_Orig)(const char* line) = nullptr;
static std::vector<int> g_weaponDatModelId; // wt -> modelId
// Optional IDE/DFF basename from hooked line (second token), indexed by weapon type.
static std::vector<std::string> g_weaponDatIdeName;

static bool g_pedDatHookInstalled = false;
static int(__cdecl* g_LoadPedObject_Orig)(const char* line) = nullptr;
static std::vector<std::string> g_pedModelNameById; // modelId -> name
static std::vector<std::string> g_pedDatTxdById; // modelId -> txd

// SA `CFileLoader::LoadWeaponObject` receives a processed line, not raw weapon.dat:
//   "<modelId> <dffName> <txdName> ..."  e.g. "346 colt45 colt45 colt45 1 30 0"
// First token is IDE model id; second is the weapon model basename (matches FindWeaponType / IDE).
static int __cdecl LoadWeaponObject_Detour(const char* line) {
    int modelId = 0;
    if (g_LoadWeaponObject_Orig) modelId = g_LoadWeaponObject_Orig(line);

    if (!line) return modelId;

    const char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    char* endNum = nullptr;
    const long parsedId = strtol(p, &endNum, 10);
    const int idFromLine = (endNum != p && parsedId > 0) ? (int)parsedId : 0;

    char dff[96] = {};
    p = endNum;
    while (*p == ' ' || *p == '\t') ++p;
    int di = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && di < (int)sizeof(dff) - 1)
        dff[di++] = *p++;
    dff[di] = 0;

    const int resolvedModel = (modelId > 0) ? modelId : idFromLine;
    if (resolvedModel <= 0) return modelId;

    int wt = WEAPONTYPE_UNARMED;
    if (dff[0]) {
        wt = (int)CWeaponInfo::FindWeaponType(dff);
        if (wt <= 0) {
            char up[96];
            strncpy_s(up, dff, _TRUNCATE);
            for (char* c = up; *c; ++c) {
                if (*c >= 'a' && *c <= 'z') *c = (char)(*c - ('a' - 'A'));
            }
            wt = (int)CWeaponInfo::FindWeaponType(up);
        }
    }

    if (wt <= 0) {
        __try {
            for (int t = 1; t <= 255; ++t) {
                CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo((eWeaponType)t, 1);
                if (!wi) continue;
                if (wi->m_nModelId == resolvedModel || wi->m_nModelId2 == resolvedModel) {
                    wt = t;
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            const int cap = MAX_WEAPON_INFOS - 1;
            __try {
                for (int t = 1; t <= cap; ++t) {
                    if (aWeaponInfo[t].m_nModelId == resolvedModel || aWeaponInfo[t].m_nModelId2 == resolvedModel) {
                        wt = t;
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    if (wt > 0 && wt <= 255) {
        if ((int)g_weaponDatModelId.size() <= wt) g_weaponDatModelId.resize(wt + 1, 0);
        g_weaponDatModelId[wt] = resolvedModel;
        if (dff[0]) {
            if ((int)g_weaponDatIdeName.size() <= wt) g_weaponDatIdeName.resize(wt + 1);
            g_weaponDatIdeName[wt] = dff;
        }
    }

    return modelId;
}

static int __cdecl LoadPedObject_Detour(const char* line) {
    int modelId = 0;
    if (g_LoadPedObject_Orig) modelId = g_LoadPedObject_Orig(line);

    // SA `LoadPedObject` line is processed as:
    //   "<modelId> <dffName> <txdName> ..."  e.g. "235 SWMORI SWMORI CIVMALE ..."
    if (!line) return modelId;
    const char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    char* endNum = nullptr;
    const long parsedId = strtol(p, &endNum, 10);
    const int idFromLine = (endNum != p && parsedId > 0) ? (int)parsedId : 0;
    const int resolvedId = (modelId > 0) ? modelId : idFromLine;
    if (resolvedId <= 0) return modelId;

    char dff[64] = {};
    char txd[64] = {};
    p = endNum;
    while (*p == ' ' || *p == '\t') ++p;
    int di = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && di < (int)sizeof(dff) - 1)
        dff[di++] = *p++;
    dff[di] = 0;
    while (*p == ' ' || *p == '\t') ++p;
    int ti = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && ti < (int)sizeof(txd) - 1)
        txd[ti++] = *p++;
    txd[ti] = 0;

    if ((dff[0] || txd[0]) && resolvedId > 0) {
        if ((int)g_pedModelNameById.size() <= resolvedId) g_pedModelNameById.resize(resolvedId + 1);
        if ((int)g_pedDatTxdById.size() <= resolvedId) g_pedDatTxdById.resize(resolvedId + 1);
        if (dff[0] && g_pedModelNameById[resolvedId].empty()) g_pedModelNameById[resolvedId] = dff;
        if (txd[0] && g_pedDatTxdById[resolvedId].empty()) g_pedDatTxdById[resolvedId] = txd;
    }
    return modelId;
}

static void EnsureWeaponDatHookInstalled() {
    if (g_weaponDatHookInstalled) return;
    g_weaponDatHookInstalled = true;
    g_weaponDatModelId.assign(256 + 1, 0);
    g_weaponDatIdeName.assign(256 + 1, {});

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("LoadWeaponObject hook: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(0x5B3FB0),
                      reinterpret_cast<void*>(&LoadWeaponObject_Detour),
                      reinterpret_cast<void**>(&g_LoadWeaponObject_Orig)) != MH_OK) {
        OrcLogError("LoadWeaponObject hook: MH_CreateHook failed");
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(0x5B3FB0));
    if (st != MH_OK)
        OrcLogError("LoadWeaponObject hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("LoadWeaponObject hook installed (0x5B3FB0)");
}

static void EnsurePedDatHookInstalled() {
    if (g_pedDatHookInstalled) return;
    g_pedDatHookInstalled = true;
    g_pedModelNameById.clear();
    g_pedModelNameById.resize(1000); // grows on demand
    g_pedDatTxdById.clear();
    g_pedDatTxdById.resize(1000); // grows on demand

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("LoadPedObject hook: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(0x5B7420),
                      reinterpret_cast<void*>(&LoadPedObject_Detour),
                      reinterpret_cast<void**>(&g_LoadPedObject_Orig)) != MH_OK) {
        OrcLogError("LoadPedObject hook: MH_CreateHook failed");
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(0x5B7420));
    if (st != MH_OK)
        OrcLogError("LoadPedObject hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("LoadPedObject hook installed (0x5B7420)");
}

const char* OrcTryGetPedModelNameById(int modelId) {
    if (modelId <= 0) return nullptr;
    if (modelId >= (int)g_pedModelNameById.size()) return nullptr;
    if (g_pedModelNameById[modelId].empty()) return nullptr;
    return g_pedModelNameById[modelId].c_str();
}

static void InitWeaponTypesAndStorage() {
    // Важно: `WeaponCfg::name` хранит `const char*` на строковые буферы `g_weaponNameStore`.
    // Поэтому скан делаем однократно, чтобы не инвалидировать эти указатели при повторных `SetupDefaults()`.
    if (g_weaponTypesReady) return;
    g_weaponTypesReady = false;

    // Ensure we capture weapon.dat loads as early as possible.
    EnsureWeaponDatHookInstalled();
    EnsurePedDatHookInstalled();

    __try {

        // Primary source of truth: weapon.dat hook results (wt -> modelId).
        // Fallback: `aWeaponInfo[]` (SDK fixed array).
        const int baseMax = MAX_WEAPON_INFOS - 1;
        int maxId = 0;
        g_availableWeaponTypes.clear();

        for (int wt = 1; wt <= 255; wt++) {
            int mid = 0;
            if (wt < (int)g_weaponDatModelId.size()) mid = g_weaponDatModelId[wt];
            if (mid <= 0 && wt <= baseMax) mid = aWeaponInfo[wt].m_nModelId;
            if (mid > 0) {
                g_availableWeaponTypes.push_back(wt);
                if (wt > maxId) maxId = wt;
            }
        }
        if (maxId <= 0) maxId = baseMax;

        std::sort(g_availableWeaponTypes.begin(), g_availableWeaponTypes.end());
        g_availableWeaponTypes.erase(std::unique(g_availableWeaponTypes.begin(), g_availableWeaponTypes.end()), g_availableWeaponTypes.end());

        g_cfg.assign(maxId + 1, {});
        g_cfg2.assign(maxId + 1, {});
        g_weaponModelId.assign(maxId + 1, 0);
        g_weaponModelId2.assign(maxId + 1, 0);
        g_weaponNameStore.assign(maxId + 1, {});

        for (int wt : g_availableWeaponTypes) {
            // Don't touch ms_aWeaponNames here: in some modpacks it can be invalid early
            // and trigger SEH; stable names are enough for UI.
            if (wt < (int)g_weaponDatIdeName.size() && !g_weaponDatIdeName[wt].empty())
                g_weaponNameStore[wt] = g_weaponDatIdeName[wt];
            else
                g_weaponNameStore[wt] = "Weapon" + std::to_string(wt);
            g_cfg[wt].name = g_weaponNameStore[wt].c_str();

            // Prefer LoadWeaponObject hook model id, fallback to aWeaponInfo / GetWeaponInfo.
            if (wt < (int)g_weaponDatModelId.size() && g_weaponDatModelId[wt] > 0)
                g_weaponModelId[wt] = g_weaponDatModelId[wt];
            else if (wt <= baseMax)
                g_weaponModelId[wt] = aWeaponInfo[wt].m_nModelId;

            if (wt <= baseMax)
                g_weaponModelId2[wt] = aWeaponInfo[wt].m_nModelId2;

            if (wt > baseMax || g_weaponModelId[wt] <= 0 || g_weaponModelId2[wt] <= 0) {
                __try {
                    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo((eWeaponType)wt, 1);
                    if (wi) {
                        if (g_weaponModelId[wt] <= 0 && wi->m_nModelId > 0)
                            g_weaponModelId[wt] = wi->m_nModelId;
                        if (g_weaponModelId2[wt] <= 0 && wi->m_nModelId2 > 0)
                            g_weaponModelId2[wt] = wi->m_nModelId2;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }

        g_weaponTypesReady = true;
        static bool s_weaponTypesLogged = false;
        if (!s_weaponTypesLogged) {
            s_weaponTypesLogged = true;
            OrcLogInfo("weapon types ready: %zu entries", g_availableWeaponTypes.size());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("InitWeaponTypesAndStorage: SEH ex=0x%08X, using fallback weapon list",
            GetExceptionCode());
        // Если список уже частично заполнен (часто бывает из-за SEH в extra-скане),
        // не обнуляем его обратно в `1..68`. Оставляем то, что успело найтись,
        // и просто строим cfg под текущий maxId.
        if (!g_availableWeaponTypes.empty()) {
            int maxId = 0;
            for (int wt : g_availableWeaponTypes) if (wt > maxId) maxId = wt;
            if (maxId <= 0) maxId = MAX_WEAPON_INFOS - 1;

            std::sort(g_availableWeaponTypes.begin(), g_availableWeaponTypes.end());
            g_availableWeaponTypes.erase(std::unique(g_availableWeaponTypes.begin(), g_availableWeaponTypes.end()), g_availableWeaponTypes.end());

            g_cfg.assign(maxId + 1, {});
            g_cfg2.assign(maxId + 1, {});
            g_weaponModelId.assign(maxId + 1, 0);
            g_weaponModelId2.assign(maxId + 1, 0);
            g_weaponNameStore.assign(maxId + 1, {});
            for (int wt : g_availableWeaponTypes) {
                if (wt < (int)g_weaponDatIdeName.size() && !g_weaponDatIdeName[wt].empty())
                    g_weaponNameStore[wt] = g_weaponDatIdeName[wt];
                else
                    g_weaponNameStore[wt] = "Weapon" + std::to_string(wt);
                g_cfg[wt].name = g_weaponNameStore[wt].c_str();
                const int baseMax2 = MAX_WEAPON_INFOS - 1;
                if (wt <= baseMax2) {
                    g_weaponModelId[wt] = aWeaponInfo[wt].m_nModelId;
                    g_weaponModelId2[wt] = aWeaponInfo[wt].m_nModelId2;
                }
            }
        } else {
            const int fallbackMax = std::min(255, MAX_WEAPON_INFOS - 1);
            g_availableWeaponTypes.clear();
            for (int wt = 1; wt <= fallbackMax; wt++) g_availableWeaponTypes.push_back(wt);

            g_cfg.assign(fallbackMax + 1, {});
            g_cfg2.assign(fallbackMax + 1, {});
            g_weaponModelId.assign(fallbackMax + 1, 0);
            g_weaponModelId2.assign(fallbackMax + 1, 0);
            g_weaponNameStore.assign(fallbackMax + 1, {});
            for (int wt : g_availableWeaponTypes) {
                g_weaponNameStore[wt] = "Weapon" + std::to_string(wt);
                g_cfg[wt].name = g_weaponNameStore[wt].c_str();
                g_weaponModelId[wt] = aWeaponInfo[wt].m_nModelId;
                g_weaponModelId2[wt] = aWeaponInfo[wt].m_nModelId2;
            }
        }
        g_weaponTypesReady = true;
        OrcLogInfo("weapon types fallback ready: %zu entries", g_availableWeaponTypes.size());
    }
}
bool g_skinModeEnabled = false;
bool g_skinHideBasePed = true;
bool g_skinNickMode = true;
bool g_skinLocalPreferSelected = false;
bool g_skinTextureRemapEnabled = false;
bool g_skinTextureRemapNickMode = true;
bool g_skinTextureRemapAutoNickMode = true;
int g_skinTextureRemapRandomMode = TEXTURE_REMAP_RANDOM_LINKED_VARIANT;
bool g_skinRandomFromPools = false;
int g_skinRandomPoolModels = 0;
int g_skinRandomPoolVariants = 0;
std::string g_skinSelectedName;
int g_skinSelectedSource = SKIN_SELECTED_CUSTOM;
int g_standardSkinSelectedModelId = -1;
static bool g_skinCanAnimate = false;
static int  g_skinBindCount = 0;

static void DestroyCustomObjectInstance(CustomObjectCfg& o);
static void DestroyAllStandardObjectInstances();
static void DestroyStandardObjectInstancesForSlot(int modelId, int slot);
static CustomSkinCfg* GetSelectedSkin();
static void DestroyStandardSkinInstance(StandardSkinCfg& s);
static bool IsValidStandardSkinModel(int modelId);
static void ClearAllWeaponReplacementInstances();
static void DestroyAllHeldWeaponReplacementInstances();
static void DestroyStandardSkinPreview();
static void RenderSkinOnPed(CPed* ped, CustomSkinCfg* sel, bool isLocalPed);

// Секция INI → индекс оружия. Дефолтные расположения в стиле тактической выкладки.
// Оси кости (наблюдение): X = вдоль "right", Y = вдоль "up" (spine) / "at" (бедро), Z = "at/up".
// Параметры подбирались так, чтобы длинные стволы шли по диагонали за спиной,
// пистолеты висели в бедренных кобурах, SMG — под левой рукой у пояса.
static void Set(int wt, const char* name, int bone,
                float x, float y, float z,
                float rxDeg = 0, float ryDeg = 0, float rzDeg = 0,
                float scale = 1.0f) {
    if (wt <= 0 || wt >= (int)g_cfg.size()) return;
    auto& c = g_cfg[wt];
    c.enabled = true;
    c.name    = name;
    c.boneId  = bone;
    c.x = x; c.y = y; c.z = z;
    c.rx = rxDeg * D2R; c.ry = ryDeg * D2R; c.rz = rzDeg * D2R;
    c.scale = scale;
}

static void Set2(int wt, int bone,
                float x, float y, float z,
                float rxDeg = 0, float ryDeg = 0, float rzDeg = 0,
                float scale = 1.0f) {
    if (wt <= 0 || wt >= (int)g_cfg2.size()) return;
    auto& c = g_cfg2[wt];
    c.enabled = true;
    c.name    = nullptr;
    c.boneId  = bone;
    c.x = x; c.y = y; c.z = z;
    c.rx = rxDeg * D2R; c.ry = ryDeg * D2R; c.rz = rzDeg * D2R;
    c.scale = scale;
}

// 10 слотов SA → распределение по разным костям.
// Slot 1 melee / Slot 2 pistols / Slot 3 shotguns / Slot 4 SMG /
// Slot 5 assault / Slot 6 rifles / Slot 7 heavy / Slot 8 thrown и т.д.
static void SetupDefaults() {
    InitWeaponTypesAndStorage();

    for (auto& c : g_cfg) { c = {}; }
    for (auto& c : g_cfg2) { c = {}; }

    // Re-apply stable names for all ids (needed for full-range combo 0..256).
    for (int wt = 0; wt < (int)g_cfg.size(); wt++) {
        if (wt >= (int)g_weaponNameStore.size()) continue;
        if (wt > 0 && wt < (int)g_weaponDatIdeName.size() && !g_weaponDatIdeName[wt].empty())
            g_weaponNameStore[wt] = g_weaponDatIdeName[wt];
        else if (g_weaponNameStore[wt].empty())
            g_weaponNameStore[wt] = "Weapon" + std::to_string(wt);
        g_cfg[wt].name = g_weaponNameStore[wt].c_str();
    }

    // --- Slot 1: холодное оружие ---
    Set(WEAPONTYPE_KNIFE,        "Knife",        BONE_R_CALF,   0.02f, -0.08f,  0.05f,   0, 0,   0);
    Set(WEAPONTYPE_BASEBALLBAT,  "BaseballBat",  BONE_SPINE1,   0.25f, -0.05f,  0.00f,  15, 0,   0);
    Set(WEAPONTYPE_GOLFCLUB,     "GolfClub",     BONE_SPINE1,   0.25f, -0.05f,  0.00f,  15, 0,   0);
    Set(WEAPONTYPE_NIGHTSTICK,   "Nightstick",   BONE_R_THIGH,  0.06f, -0.05f,  0.08f,   0, 0, -10);
    Set(WEAPONTYPE_SHOVEL,       "Shovel",       BONE_SPINE1,   0.25f, -0.05f,  0.00f, -15, 0,   0);
    Set(WEAPONTYPE_POOLCUE,      "PoolCue",      BONE_SPINE1,   0.25f, -0.05f,  0.00f, -15, 0,   0);
    Set(WEAPONTYPE_KATANA,       "Katana",       BONE_SPINE1,   0.27f, -0.08f,  0.00f, -20, 0,   0);
    Set(WEAPONTYPE_CHAINSAW,     "Chainsaw",     BONE_SPINE1,   0.28f, -0.18f,  0.00f,   0, 0,  90);

    // --- Slot 2: пистолеты — правая бедренная кобура ---
    Set(WEAPONTYPE_PISTOL,          "Pistol",          BONE_R_THIGH, 0.05f, -0.08f, 0.08f);
    Set(WEAPONTYPE_PISTOL_SILENCED, "PistolSilenced",  BONE_R_THIGH, 0.05f, -0.08f, 0.08f);
    Set(WEAPONTYPE_DESERT_EAGLE,    "DesertEagle",     BONE_R_THIGH, 0.06f, -0.08f, 0.10f);
    // Secondary (dual wield) defaults: left thigh holster.
    Set2(WEAPONTYPE_PISTOL,          BONE_L_THIGH, 0.05f, -0.08f, -0.08f);
    Set2(WEAPONTYPE_PISTOL_SILENCED, BONE_L_THIGH, 0.05f, -0.08f, -0.08f);
    Set2(WEAPONTYPE_DESERT_EAGLE,    BONE_L_THIGH, 0.06f, -0.08f, -0.10f);

    // --- Slot 3: дробовики ---
    Set(WEAPONTYPE_SHOTGUN, "Shotgun", BONE_SPINE1,  0.27f, -0.13f,  0.00f,  20, 0, 0);
    Set(WEAPONTYPE_SAWNOFF, "Sawnoff", BONE_L_CALF,  0.00f, -0.10f,  0.06f,   0, 0, 0);  // на ноге
    Set(WEAPONTYPE_SPAS12,  "Spas12",  BONE_SPINE1,  0.27f, -0.13f,  0.00f, -20, 0, 0);

    // --- Slot 4: SMG — левое плечо/бок ---
    Set(WEAPONTYPE_MICRO_UZI, "MicroUzi", BONE_L_CLAVIC, 0.05f, -0.12f,  0.08f, 0, 0, -15);
    Set(WEAPONTYPE_MP5,       "MP5",      BONE_L_UPARM,  0.00f, -0.15f,  0.08f, 0, 0,   0);
    Set(WEAPONTYPE_TEC9,      "Tec9",     BONE_L_THIGH,  0.06f, -0.08f, -0.06f, 0, 0,   0);

    // --- Slot 5: штурмовые винтовки — перевязь через спину ---
    Set(WEAPONTYPE_AK47, "AK47", BONE_SPINE1, 0.28f, -0.10f, 0.05f,  10, 0,  0);
    Set(WEAPONTYPE_M4,   "M4",   BONE_SPINE1, 0.28f, -0.10f, 0.05f,  10, 0,  0);

    // --- Slot 6: ружья/снайперки — правое плечо ---
    Set(WEAPONTYPE_COUNTRYRIFLE, "CountryRifle", BONE_R_CLAVIC, 0.05f, -0.12f, -0.06f, 0, 0,  15);
    Set(WEAPONTYPE_SNIPERRIFLE,  "SniperRifle",  BONE_R_CLAVIC, 0.05f, -0.12f, -0.06f, 0, 0,  15);

    // --- Slot 7: тяжёлое — центр спины ---
    Set(WEAPONTYPE_RLAUNCHER,    "RocketLauncher",   BONE_SPINE1, 0.30f, -0.08f, 0.00f);
    Set(WEAPONTYPE_RLAUNCHER_HS, "HeatseekLauncher", BONE_SPINE1, 0.30f, -0.08f, 0.00f);
    Set(WEAPONTYPE_FTHROWER,     "Flamethrower",     BONE_SPINE1, 0.30f, -0.08f, 0.00f);
    Set(WEAPONTYPE_MINIGUN,      "Minigun",          BONE_SPINE1, 0.32f, -0.08f, 0.00f, 0, 0, 0, 0.9f);

    // --- Slot 8: метательное — пояс ---
    Set(WEAPONTYPE_GRENADE,        "Grenade",  BONE_PELVIS, 0.10f, -0.05f,  0.08f);
    Set(WEAPONTYPE_TEARGAS,        "Teargas",  BONE_PELVIS, 0.10f, -0.05f, -0.08f);
    Set(WEAPONTYPE_MOLOTOV,        "Molotov",  BONE_PELVIS,-0.10f, -0.05f,  0.08f);
    Set(WEAPONTYPE_SATCHEL_CHARGE, "Satchel",  BONE_PELVIS,-0.10f, -0.05f, -0.08f);

    // --- Slot 9: gift (редко) — выключено по умолчанию ---
    // --- Slot 10: специальные (камера/spraycan/огнетушитель/парашют) ---
    Set(WEAPONTYPE_EXTINGUISHER,  "Extinguisher", BONE_SPINE1, 0.30f, -0.08f, 0.00f);
    Set(WEAPONTYPE_SPRAYCAN,      "Spraycan",     BONE_PELVIS,  0.00f, -0.05f, 0.10f);
    Set(WEAPONTYPE_CAMERA,        "Camera",       BONE_PELVIS,  0.00f, -0.05f, 0.10f);
    Set(WEAPONTYPE_PARACHUTE,     "Parachute",    BONE_SPINE1,  0.25f, -0.05f, 0.00f, 0, 0, 0, 1.0f);

    // --- Slot 11: детонатор — на поясе ---
    Set(WEAPONTYPE_DETONATOR, "Detonator", BONE_PELVIS, 0.12f, -0.02f, 0.00f);
}

static void ReadSectionFromIni(WeaponCfg& c, const char* section, const char* iniPath) {
    char buf[64];
    auto F = [&](const char* key, float def)->float{
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), iniPath);
        if (!buf[0]) return def;
        return (float)atof(buf);
    };
    c.enabled = GetPrivateProfileIntA(section, "Enabled", c.enabled ? 1 : 0, iniPath) != 0;
    c.boneId  = GetPrivateProfileIntA(section, "Bone", c.boneId, iniPath);
    c.x = F("OffsetX", c.x);
    c.y = F("OffsetY", c.y);
    c.z = F("OffsetZ", c.z);
    c.rx = F("RotationX", c.rx / D2R) * D2R;
    c.ry = F("RotationY", c.ry / D2R) * D2R;
    c.rz = F("RotationZ", c.rz / D2R) * D2R;
    c.scale = F("Scale", c.scale);
}

static void ReadSection(WeaponCfg& c, const char* section) {
    ReadSectionFromIni(c, section, g_iniPath);
}

static bool HasWeaponSection(const char* section, const char* iniPath) {
    if (!section || !section[0] || !iniPath || !iniPath[0]) return false;
    char buf[64] = {};
    GetPrivateProfileStringA(section, "Enabled", "", buf, sizeof(buf), iniPath);
    if (buf[0]) return true;
    GetPrivateProfileStringA(section, "Bone", "", buf, sizeof(buf), iniPath);
    if (buf[0]) return true;
    GetPrivateProfileStringA(section, "OffsetX", "", buf, sizeof(buf), iniPath);
    return buf[0] != 0;
}

static float ClampConfigFloat(float value, float minValue, float maxValue, float fallback) {
    if (!std::isfinite(value))
        value = fallback;
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

static float ReadIniFloat(const char* section, const char* key, float fallback, const char* iniPath) {
    char buf[64] = {};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), iniPath);
    if (!buf[0])
        return fallback;
    return static_cast<float>(atof(buf));
}

int ParseActivationVk(const char* text) {
    if (!text || !text[0]) return VK_F7;
    if (!lstrcmpiA(text, "F1")) return VK_F1;
    if (!lstrcmpiA(text, "F2")) return VK_F2;
    if (!lstrcmpiA(text, "F3")) return VK_F3;
    if (!lstrcmpiA(text, "F4")) return VK_F4;
    if (!lstrcmpiA(text, "F5")) return VK_F5;
    if (!lstrcmpiA(text, "F6")) return VK_F6;
    if (!lstrcmpiA(text, "F7")) return VK_F7;
    if (!lstrcmpiA(text, "F8")) return VK_F8;
    if (!lstrcmpiA(text, "F9")) return VK_F9;
    if (!lstrcmpiA(text, "F10")) return VK_F10;
    if (!lstrcmpiA(text, "F11")) return VK_F11;
    if (!lstrcmpiA(text, "F12")) return VK_F12;

    if (text[1] == '\0') {
        const char c = text[0];
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
        if (c >= '0' && c <= '9') return c;
    }
    const int vk = atoi(text);
    if (vk >= 1 && vk <= 255) return vk;
    return VK_F7;
}

const char* VkToString(int vk) {
    switch (vk) {
    case VK_F1: return "F1";
    case VK_F2: return "F2";
    case VK_F3: return "F3";
    case VK_F4: return "F4";
    case VK_F5: return "F5";
    case VK_F6: return "F6";
    case VK_F7: return "F7";
    case VK_F8: return "F8";
    case VK_F9: return "F9";
    case VK_F10: return "F10";
    case VK_F11: return "F11";
    case VK_F12: return "F12";
    default: break;
    }
    static char buf[16];
    _snprintf_s(buf, _TRUNCATE, "%d", vk);
    return buf;
}

static void NormalizeCommand() {
    if (g_toggleCommand.empty()) g_toggleCommand = "/orcoutfit";
    if (g_toggleCommand[0] != '/') g_toggleCommand.insert(g_toggleCommand.begin(), '/');
}

static void ToggleOverlayFromSamp() {
    overlay::Toggle();
}

static std::string ToLowerAscii(std::string s);

void RefreshActivationRouting() {
    overlay::SetToggleVirtualKey(g_activationVk);
    const bool hotkeyAllowed = !samp_bridge::IsSampBuildKnown() || g_sampAllowActivationKey;
    overlay::SetHotkeyEnabled(hotkeyAllowed);
}

void LoadConfig() {
    SetupDefaults();
    g_enabled = GetPrivateProfileIntA("Main", "Enabled", 1, g_iniPath) != 0;
    g_renderAllPedsWeapons = GetPrivateProfileIntA("Features", "RenderAllPedsWeapons", 0, g_iniPath) != 0;
    g_renderAllPedsObjects = GetPrivateProfileIntA("Features", "RenderAllPedsObjects", 0, g_iniPath) != 0;
    g_renderAllPedsRadius = (float)GetPrivateProfileIntA("Features", "RenderAllPedsRadius", 80, g_iniPath);
    g_considerWeaponSkills = GetPrivateProfileIntA("Features", "ConsiderWeaponSkills", 1, g_iniPath) != 0;
    g_renderCustomObjects = GetPrivateProfileIntA("Features", "CustomObjects", 1, g_iniPath) != 0;
    g_renderStandardObjects = GetPrivateProfileIntA("Features", "StandardObjects", 1, g_iniPath) != 0;
    g_skinModeEnabled = GetPrivateProfileIntA("Features", "SkinMode", 0, g_iniPath) != 0;
    g_skinHideBasePed = GetPrivateProfileIntA("Features", "SkinHideBasePed", 1, g_iniPath) != 0;
    g_skinNickMode = GetPrivateProfileIntA("Features", "SkinNickMode", 1, g_iniPath) != 0;
    g_skinLocalPreferSelected = GetPrivateProfileIntA("Features", "SkinLocalPreferSelected", 0, g_iniPath) != 0;
    g_skinTextureRemapEnabled = GetPrivateProfileIntA("Features", "SkinTextureRemap", 0, g_iniPath) != 0;
    g_skinTextureRemapNickMode = GetPrivateProfileIntA("Features", "SkinTextureRemapNickMode", 1, g_iniPath) != 0;
    g_skinTextureRemapAutoNickMode = GetPrivateProfileIntA("Features", "SkinTextureRemapAutoNickMode", 1, g_iniPath) != 0;
    g_skinTextureRemapRandomMode = GetPrivateProfileIntA("Features", "SkinTextureRemapRandomMode", TEXTURE_REMAP_RANDOM_LINKED_VARIANT, g_iniPath);
    if (g_skinTextureRemapRandomMode < TEXTURE_REMAP_RANDOM_PER_TEXTURE ||
        g_skinTextureRemapRandomMode > TEXTURE_REMAP_RANDOM_LINKED_VARIANT) {
        g_skinTextureRemapRandomMode = TEXTURE_REMAP_RANDOM_LINKED_VARIANT;
    }
    g_sampAllowActivationKey = GetPrivateProfileIntA("Main", "SampAllowActivationKey", 0, g_iniPath) != 0;
    g_uiAutoScale = GetPrivateProfileIntA("Main", "UiAutoScale", 0, g_iniPath) != 0;
    g_uiScale = ClampConfigFloat(ReadIniFloat("Main", "UiScale", 1.0f, g_iniPath), 0.75f, 1.60f, 1.0f);
    g_uiFontSize = ClampConfigFloat(ReadIniFloat("Main", "UiFontSize", 15.0f, g_iniPath), 13.0f, 22.0f, 15.0f);
    char languageBuf[16] = {};
    GetPrivateProfileStringA("Main", "Language", "ru", languageBuf, sizeof(languageBuf), g_iniPath);
    g_orcUiLanguage = OrcParseLanguage(languageBuf);
    char keyBuf[32] = {};
    GetPrivateProfileStringA("Main", "ActivationKey", "F7", keyBuf, sizeof(keyBuf), g_iniPath);
    g_activationVk = ParseActivationVk(keyBuf);
    char cmdBuf[64] = {};
    GetPrivateProfileStringA("Main", "Command", "/orcoutfit", cmdBuf, sizeof(cmdBuf), g_iniPath);
    g_toggleCommand = cmdBuf;
    NormalizeCommand();
    if (g_renderAllPedsRadius < 5.0f) g_renderAllPedsRadius = 5.0f;
    g_weaponReplacementEnabled = GetPrivateProfileIntA("Features", "WeaponReplacement", 1, g_iniPath) != 0;
    g_weaponReplacementOnBody = GetPrivateProfileIntA("Features", "WeaponReplacementOnBody", 1, g_iniPath) != 0;
    g_weaponReplacementInHands = GetPrivateProfileIntA("Features", "WeaponReplacementInHands", 1, g_iniPath) != 0;
    g_weaponTexturesEnabled = GetPrivateProfileIntA("Features", "WeaponTextures", 1, g_iniPath) != 0;
    g_weaponTextureNickMode = GetPrivateProfileIntA("Features", "WeaponTextureNickMode", 1, g_iniPath) != 0;
    g_weaponTextureRandomMode = GetPrivateProfileIntA("Features", "WeaponTextureRandomMode", 1, g_iniPath) != 0;
    for (int wt : g_availableWeaponTypes) {
        if (wt <= 0 || wt >= (int)g_cfg.size()) continue;
        auto& c = g_cfg[wt];
        if (c.name && c.name[0] && HasWeaponSection(c.name, g_iniPath)) ReadSection(c, c.name);
        // Fallback для кастомного оружия: секция [WeaponNN].
        char sec[32];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d", wt);
        if (HasWeaponSection(sec, g_iniPath)) {
            if (!c.name || !c.name[0]) { c.scale = 1.0f; c.enabled = true; }
            ReadSection(c, sec);
        }
    }
    // Secondary configs: [<Name>2] and [WeaponNN_2]
    for (int wt : g_availableWeaponTypes) {
        if (wt <= 0 || wt >= (int)g_cfg2.size()) continue;
        auto& c = g_cfg2[wt];
        if (wt < (int)g_cfg.size() && g_cfg[wt].name && g_cfg[wt].name[0]) {
            char sec2[64];
            _snprintf_s(sec2, _TRUNCATE, "%s2", g_cfg[wt].name);
            if (HasWeaponSection(sec2, g_iniPath)) {
                c.enabled = true;
                ReadSection(c, sec2);
            }
        }
        char sec[32];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d_2", wt);
        if (HasWeaponSection(sec, g_iniPath)) {
            c.enabled = true;
            ReadSection(c, sec);
        }
    }
    g_skinRandomFromPools = GetPrivateProfileIntA("SkinMode", "RandomFromPools", 0, g_iniPath) != 0;
    char skinName[128] = {};
    GetPrivateProfileStringA("SkinMode", "Selected", "", skinName, sizeof(skinName), g_iniPath);
    g_skinSelectedName = skinName;
    char selectedSource[32] = {};
    GetPrivateProfileStringA("SkinMode", "SelectedSource", "custom", selectedSource, sizeof(selectedSource), g_iniPath);
    g_skinSelectedSource = (ToLowerAscii(selectedSource) == "standard") ? SKIN_SELECTED_STANDARD : SKIN_SELECTED_CUSTOM;
    g_standardSkinSelectedModelId = GetPrivateProfileIntA("SkinMode", "StandardSelected", -1, g_iniPath);
    RefreshActivationRouting();
    InvalidatePerSkinWeaponCache();
    InvalidateObjectSkinParamCache();
    InvalidateStandardObjectSkinParamCache();
    InvalidateStandardSkinLookupCache();
    OrcLogReloadFromIni(g_iniPath);
    OrcLogInfo("LoadConfig: %s", g_iniPath);
    // Weapon types are discovered in SetupDefaults (InitWeaponTypesAndStorage).
}

static void AddIniValue(std::vector<OrcIniValue>& values, const char* section, const char* key, const char* value) {
    OrcIniValue v;
    v.section = section ? section : "";
    v.key = key ? key : "";
    v.value = value ? value : "";
    values.push_back(std::move(v));
}

static void AddIniInt(std::vector<OrcIniValue>& values, const char* section, const char* key, int value) {
    char buf[32];
    _snprintf_s(buf, _TRUNCATE, "%d", value);
    AddIniValue(values, section, key, buf);
}

static void AddIniFloat0(std::vector<OrcIniValue>& values, const char* section, const char* key, float value) {
    char buf[32];
    _snprintf_s(buf, _TRUNCATE, "%.0f", value);
    AddIniValue(values, section, key, buf);
}

static void AddIniFloat(std::vector<OrcIniValue>& values, const char* section, const char* key, float value, const char* fmt) {
    char buf[32];
    _snprintf_s(buf, _TRUNCATE, fmt, value);
    AddIniValue(values, section, key, buf);
}

static void AppendSkinFeatureIniValues(std::vector<OrcIniValue>& values) {
    AddIniInt(values, "Features", "SkinMode", g_skinModeEnabled ? 1 : 0);
    AddIniInt(values, "Features", "SkinHideBasePed", g_skinHideBasePed ? 1 : 0);
    AddIniInt(values, "Features", "SkinNickMode", g_skinNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinLocalPreferSelected", g_skinLocalPreferSelected ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemap", g_skinTextureRemapEnabled ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapNickMode", g_skinTextureRemapNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapAutoNickMode", g_skinTextureRemapAutoNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapRandomMode", g_skinTextureRemapRandomMode);
}

static void AppendSkinModeIniValues(std::vector<OrcIniValue>& values) {
    AddIniValue(values, "SkinMode", "Selected", g_skinSelectedName.c_str());
    AddIniValue(values, "SkinMode", "SelectedSource", g_skinSelectedSource == SKIN_SELECTED_STANDARD ? "standard" : "custom");
    AddIniInt(values, "SkinMode", "StandardSelected", g_standardSkinSelectedModelId);
    AddIniInt(values, "SkinMode", "RandomFromPools", g_skinRandomFromPools ? 1 : 0);
}

static void AppendMainIniValues(std::vector<OrcIniValue>& values) {
    AddIniInt(values, "Main", "Enabled", g_enabled ? 1 : 0);
    AddIniValue(values, "Main", "Language", OrcLanguageId(g_orcUiLanguage));
    AddIniValue(values, "Main", "ActivationKey", VkToString(g_activationVk));
    AddIniValue(values, "Main", "Command", g_toggleCommand.c_str());
    AddIniInt(values, "Main", "SampAllowActivationKey", g_sampAllowActivationKey ? 1 : 0);
    AddIniInt(values, "Main", "UiAutoScale", g_uiAutoScale ? 1 : 0);
    AddIniFloat(values, "Main", "UiScale", g_uiScale, "%.2f");
    AddIniFloat(values, "Main", "UiFontSize", g_uiFontSize, "%.0f");

    AddIniInt(values, "Features", "RenderAllPedsWeapons", g_renderAllPedsWeapons ? 1 : 0);
    AddIniInt(values, "Features", "RenderAllPedsObjects", g_renderAllPedsObjects ? 1 : 0);
    AddIniFloat0(values, "Features", "RenderAllPedsRadius", g_renderAllPedsRadius);
    AddIniInt(values, "Features", "ConsiderWeaponSkills", g_considerWeaponSkills ? 1 : 0);
    AddIniInt(values, "Features", "CustomObjects", g_renderCustomObjects ? 1 : 0);
    AddIniInt(values, "Features", "StandardObjects", g_renderStandardObjects ? 1 : 0);
    AddIniInt(values, "Features", "WeaponReplacement", g_weaponReplacementEnabled ? 1 : 0);
    AddIniInt(values, "Features", "WeaponReplacementOnBody", g_weaponReplacementOnBody ? 1 : 0);
    AddIniInt(values, "Features", "WeaponReplacementInHands", g_weaponReplacementInHands ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextures", g_weaponTexturesEnabled ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextureNickMode", g_weaponTextureNickMode ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextureRandomMode", g_weaponTextureRandomMode ? 1 : 0);
    AppendSkinFeatureIniValues(values);
    AddIniInt(values, "Features", "DebugLogLevel", static_cast<int>(g_orcLogLevel));
    AddIniInt(values, "Features", "DebugLog", (g_orcLogLevel >= OrcLogLevel::Info) ? 1 : 0);
    AppendSkinModeIniValues(values);
}

static void SaveDefaultConfig() {
    if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = fopen(g_iniPath, "w");
    if (!f) {
        OrcLogError("SaveDefaultConfig: cannot create %s", g_iniPath);
        return;
    }
    fputs("; OrcOutFit configuration.\n"
          "; Bone IDs (RpHAnim NODE IDs):\n"
          ";   1=Root 2=Pelvis 3=Spine1 4=Spine 5=Neck 6=Head\n"
          ";   21=R_Clavicle 22=R_UpperArm 23=R_Forearm 24=R_Hand 25=R_Finger\n"
          ";   31=L_Clavicle 32=L_UpperArm 33=L_Forearm 34=L_Hand 35=L_Finger\n"
          ";   41=L_Thigh 42=L_Calf 43=L_Foot\n"
          ";   51=R_Thigh 52=R_Calf 53=R_Foot\n"
          "; Offsets are in meters along bone local axes (X=right, Y=up, Z=at).\n"
          "; Rotations are in degrees, applied pre-concat in model space.\n"
          ";   RX tilts around bone right, RY around up, RZ around at.\n"
          "; Для нестандартного оружия (мод) можно добавить секцию [WeaponNN],\n"
          "; где NN = числовой eWeaponType (например [Weapon50]).\n\n"
          "[Main]\n"
          "Enabled=1\n"
          "Language=ru\n"
          "ActivationKey=F7\n"
          "SampAllowActivationKey=0\n"
          "Command=/orcoutfit\n"
          "UiAutoScale=0\n"
          "UiScale=1.00\n"
          "UiFontSize=15\n\n"
          "[Features]\n"
          "RenderAllPedsWeapons=0\n"
          "RenderAllPedsObjects=0\n"
          "RenderAllPedsRadius=80\n"
          "ConsiderWeaponSkills=1\n"
          "CustomObjects=1\n"
          "StandardObjects=1\n"
          "WeaponReplacement=1\n"
          "WeaponReplacementOnBody=1\n"
          "WeaponReplacementInHands=1\n"
          "WeaponTextures=1\n"
          "WeaponTextureNickMode=1\n"
          "WeaponTextureRandomMode=1\n"
          "SkinMode=0\n"
          "SkinHideBasePed=1\n"
          "SkinNickMode=1\n"
          "SkinLocalPreferSelected=0\n"
          "SkinTextureRemap=0\n"
          "SkinTextureRemapNickMode=1\n"
          "SkinTextureRemapAutoNickMode=1\n"
          "SkinTextureRemapRandomMode=1\n"
          "; DebugLogLevel: 0=off, 1=errors only, 2=info (full). Legacy DebugLog=1 equals level 2.\n"
          "DebugLogLevel=0\n"
          "DebugLog=0\n\n"
          "[SkinMode]\n"
          "Selected=\n"
          "SelectedSource=custom\n"
          "StandardSelected=-1\n"
          "RandomFromPools=0\n\n", f);
    for (int wt : g_availableWeaponTypes) {
        if (wt <= 0 || wt >= (int)g_cfg.size()) continue;
        const auto& c = g_cfg[wt];
        if (!c.name) continue;
        fprintf(f,
            "[%s]\nEnabled=%d\nBone=%d\n"
            "OffsetX=%.3f\nOffsetY=%.3f\nOffsetZ=%.3f\n"
            "RotationX=%.1f\nRotationY=%.1f\nRotationZ=%.1f\n"
            "Scale=%.3f\n\n",
            c.name, c.enabled ? 1 : 0, c.boneId,
            c.x, c.y, c.z,
            c.rx / D2R, c.ry / D2R, c.rz / D2R,
            c.scale);
    }
    fclose(f);
}

void SaveMainIni() {
    std::vector<OrcIniValue> values;
    AppendMainIniValues(values);
    if (!OrcIniWriteValues(g_iniPath, "; OrcOutFit configuration.\n\n", values))
        OrcLogError("SaveMainIni: cannot write %s", g_iniPath);
}

// ----------------------------------------------------------------------------
// Custom objects discovery (game folder) + per-object INI
// ----------------------------------------------------------------------------
std::vector<CustomObjectCfg> g_customObjects;
std::vector<StandardObjectSlotCfg> g_standardObjects;
std::vector<CustomSkinCfg> g_customSkins;
std::vector<StandardSkinCfg> g_standardSkins;

static bool g_customSkinLookupDirty = true;
static std::unordered_map<std::string, int> g_customSkinNickLookup;
static int g_selectedSkinCacheIdx = -1;
static std::string g_selectedSkinCacheNameLower;

void InvalidateCustomSkinLookupCache() {
    g_customSkinLookupDirty = true;
    g_customSkinNickLookup.clear();
    g_selectedSkinCacheIdx = -1;
    g_selectedSkinCacheNameLower.clear();
}

static void EnsureCustomSkinNickLookup() {
    if (!g_customSkinLookupDirty) return;
    g_customSkinNickLookup.clear();
    for (int i = 0; i < (int)g_customSkins.size(); ++i) {
        const CustomSkinCfg& s = g_customSkins[(size_t)i];
        if (!s.bindToNick || s.nicknames.empty()) continue;
        for (const auto& nick : s.nicknames) {
            if (!nick.empty())
                g_customSkinNickLookup.emplace(nick, i);
        }
    }
    g_customSkinLookupDirty = false;
}

struct SkinRandomPool {
    int modelId = -1;
    std::string folderName;
    std::vector<CustomSkinCfg> variants;
    std::vector<int> shuffleBag;
};

static std::vector<SkinRandomPool> g_skinRandomPools;
struct PedRandomSkinState {
    int modelId = -1;
    int variant = -1;
};
static std::unordered_map<CPed*, PedRandomSkinState> g_pedRandomSkinIdx;

static void DestroyAllRandomPoolSkins();

static void PrunePedRandomSkinMap() {
    std::unordered_set<CPed*> alive;
    if (CPools::ms_pPedPool) {
        for (int i = 0; i < CPools::ms_pPedPool->m_nSize; i++) {
            CPed* p = CPools::ms_pPedPool->GetAt(i);
            if (p) alive.insert(p);
        }
    }
    for (auto it = g_pedRandomSkinIdx.begin(); it != g_pedRandomSkinIdx.end();) {
        if (alive.find(it->first) == alive.end())
            it = g_pedRandomSkinIdx.erase(it);
        else
            ++it;
    }
}

static SkinRandomPool* FindRandomPoolForModelId(int modelId) {
    if (modelId < 0) return nullptr;
    for (auto& p : g_skinRandomPools) {
        if (p.modelId == modelId && !p.variants.empty())
            return &p;
    }
    return nullptr;
}

static int FindPedModelIdByDffName(const std::string& dffName) {
    if (dffName.empty()) return -1;
    const std::string want = ToLowerAscii(dffName);
    for (int id = 0; id < (int)g_pedModelNameById.size(); ++id) {
        if (g_pedModelNameById[id].empty()) continue;
        if (ToLowerAscii(g_pedModelNameById[id]) == want)
            return id;
    }
    return -1;
}

static int PopRandomPoolVariant(SkinRandomPool& pool) {
    const int n = (int)pool.variants.size();
    if (n <= 0) return -1;
    if (pool.shuffleBag.empty()) {
        pool.shuffleBag.reserve((size_t)n);
        for (int i = 0; i < n; ++i)
            pool.shuffleBag.push_back(i);
        for (int i = n - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(pool.shuffleBag[(size_t)i], pool.shuffleBag[(size_t)j]);
        }
    }
    const int pick = pool.shuffleBag.back();
    pool.shuffleBag.pop_back();
    return pick;
}

static CustomSkinCfg* ResolveRandomSkinForPed(CPed* ped) {
    if (!g_skinRandomFromPools || !ped)
        return nullptr;
    SkinRandomPool* pool = FindRandomPoolForModelId(ped->m_nModelIndex);
    if (!pool)
        return nullptr;
    const int n = (int)pool->variants.size();
    if (n <= 0)
        return nullptr;
    auto it = g_pedRandomSkinIdx.find(ped);
    if (it == g_pedRandomSkinIdx.end() || it->second.modelId != (int)ped->m_nModelIndex ||
        it->second.variant < 0 || it->second.variant >= n) {
        const int pick = PopRandomPoolVariant(*pool);
        if (pick < 0) return nullptr;
        g_pedRandomSkinIdx[ped] = PedRandomSkinState{ (int)ped->m_nModelIndex, pick };
        it = g_pedRandomSkinIdx.find(ped);
    }
    return &pool->variants[(size_t)it->second.variant];
}

static std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

static bool FileExistsA(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string BaseNameNoExt(const std::string& file) {
    size_t slash = file.find_last_of("\\/");
    size_t start = (slash == std::string::npos) ? 0 : (slash + 1);
    size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot < start) dot = file.size();
    return file.substr(start, dot - start);
}

static std::string LowerExt(const std::string& file) {
    size_t dot = file.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = file.substr(dot);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return ext;
}

static std::string ToLowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::string TrimAscii(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

std::vector<std::string> ParseNickCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::string token;
    auto flush = [&]() {
        std::string t = TrimAscii(token);
        token.clear();
        if (!t.empty()) out.push_back(ToLowerAscii(t));
    };
    for (size_t i = 0; i < csv.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(csv[i]);
        if (c == ',') {
            flush();
            continue;
        }
        token += static_cast<char>(c);
    }
    flush();
    return out;
}
static std::string FindBestTxdPath(const std::string& dir, const std::string& base) {
    // 1) strict same-base match (case-insensitive)
    std::string mask = JoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};

    const std::string baseLo = ToLowerAscii(base);
    std::string fallbackSingle;
    int txdCount = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (LowerExt(fname) != ".txd") continue;
        txdCount++;
        const std::string fbase = BaseNameNoExt(fname);
        if (ToLowerAscii(fbase) == baseLo) {
            FindClose(h);
            return JoinPath(dir, fname);
        }
        if (fallbackSingle.empty()) fallbackSingle = JoinPath(dir, fname);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    // 2) if there is exactly one TXD in folder, use it as fallback
    if (txdCount == 1) return fallbackSingle;
    return {};
}

static void CreateObjectIniStubIfMissing(const std::string& iniPath, const std::string& baseName) {
    if (FileExistsA(iniPath.c_str())) return;
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "; OrcOutFit object \"%s\"\n"
        "; Add one section per standard ped skin (ped.dat DFF name), e.g.:\n"
        "; [Skin.wmyclot]\n"
        "; Enabled=1\n"
        "; Bone=%d\n"
        "; OffsetX=0.000\n"
        "; ...\n",
        baseName.c_str(), BONE_R_THIGH);
    fclose(f);
}

static std::vector<int> ParseWeaponTypesCsv(const char* csv) {
    // Parses only commas/whitespace as separators.
    std::vector<int> out;
    if (!csv || !csv[0]) return out;

    const char* p = csv;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') ++p;
        if (!*p) break;
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p) { ++p; continue; }
        if (v > 0 && v < (long)g_cfg.size()) out.push_back((int)v);
        p = end;
    }
    // De-dup (small list; keep it simple).
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static void WeaponTypesToCsv(const std::vector<int>& types, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    bool first = true;
    for (int wt : types) {
        char tmp[16];
        _snprintf_s(tmp, _TRUNCATE, "%d", wt);
        if (!first) strncat_s(out, outSize, ",", _TRUNCATE);
        strncat_s(out, outSize, tmp, _TRUNCATE);
        first = false;
    }
}

static void CreateDefaultSkinIniIfMissing(const std::string& iniPath, const std::string& baseName) {
    if (FileExistsA(iniPath.c_str())) return;
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "; OrcOutFit custom skin config for %s\n"
        "; Nicks: one per line and/or comma-separated (case-insensitive).\n\n"
        "[NickBinding]\n"
        "Enabled=0\n"
        "Nicks=\n",
        baseName.c_str());
    fclose(f);
}

static std::string ObjectSkinIniSection(const char* skinDffName) {
    return std::string("Skin.") + (skinDffName ? skinDffName : "");
}

static bool ObjectSkinIniSectionExists(const char* iniPath, const char* sec) {
    if (!iniPath || !sec) return false;
    char b1[8] = {}, b2[8] = {};
    GetPrivateProfileStringA(sec, "Bone", "", b1, sizeof(b1), iniPath);
    GetPrivateProfileStringA(sec, "Enabled", "", b2, sizeof(b2), iniPath);
    return (b1[0] != 0 || b2[0] != 0);
}

bool LoadObjectSkinParamsFromIni(const char* iniPath, const char* skinDffName, CustomObjectSkinParams& out) {
    if (!iniPath || !iniPath[0] || !skinDffName || !skinDffName[0]) return false;
    const std::string sec = ObjectSkinIniSection(skinDffName);
    if (!ObjectSkinIniSectionExists(iniPath, sec.c_str())) return false;

    char buf[64];
    auto F = [&](const char* key, float def)->float {
        GetPrivateProfileStringA(sec.c_str(), key, "", buf, sizeof(buf), iniPath);
        if (!buf[0]) return def;
        return (float)atof(buf);
    };
    out.enabled = GetPrivateProfileIntA(sec.c_str(), "Enabled", out.enabled ? 1 : 0, iniPath) != 0;
    out.boneId = GetPrivateProfileIntA(sec.c_str(), "Bone", out.boneId, iniPath);
    out.x = F("OffsetX", out.x);
    out.y = F("OffsetY", out.y);
    out.z = F("OffsetZ", out.z);
    out.rx = F("RotationX", out.rx / D2R) * D2R;
    out.ry = F("RotationY", out.ry / D2R) * D2R;
    out.rz = F("RotationZ", out.rz / D2R) * D2R;
    out.scale = F("Scale", out.scale);
    out.scaleX = F("ScaleX", out.scaleX);
    out.scaleY = F("ScaleY", out.scaleY);
    out.scaleZ = F("ScaleZ", out.scaleZ);

    char wcsv[256] = {};
    GetPrivateProfileStringA(sec.c_str(), "Weapons", "", wcsv, sizeof(wcsv), iniPath);
    out.weaponTypes = ParseWeaponTypesCsv(wcsv);
    char mode[16] = {};
    GetPrivateProfileStringA(sec.c_str(), "WeaponsMode", "any", mode, sizeof(mode), iniPath);
    out.weaponRequireAll = (ToLowerAscii(mode) == "all");
    out.hideSelectedWeapons = GetPrivateProfileIntA(sec.c_str(), "HideWeapons", out.hideSelectedWeapons ? 1 : 0, iniPath) != 0;
    return true;
}

struct ObjectSkinParamCacheEntry {
    bool found = false;
    CustomObjectSkinParams params{};
};

static std::unordered_map<std::string, ObjectSkinParamCacheEntry> g_objectSkinParamCache;
bool g_livePreviewObjectActive = false;
std::string g_livePreviewObjectIniPath;
std::string g_livePreviewObjectSkinDff;
CustomObjectSkinParams g_livePreviewObjectParams{};

void InvalidateObjectSkinParamCache() {
    g_objectSkinParamCache.clear();
}

void SaveObjectSkinParamsToIni(const char* iniPath, const char* skinDffName, const CustomObjectSkinParams& p) {
    if (!iniPath || !iniPath[0] || !skinDffName || !skinDffName[0]) return;
    const std::string sec = ObjectSkinIniSection(skinDffName);
    std::vector<OrcIniValue> values;
    AddIniInt(values, sec.c_str(), "Enabled", p.enabled ? 1 : 0);
    AddIniInt(values, sec.c_str(), "Bone", p.boneId);
    AddIniFloat(values, sec.c_str(), "OffsetX", p.x, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetY", p.y, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetZ", p.z, "%.3f");
    AddIniFloat(values, sec.c_str(), "RotationX", p.rx / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationY", p.ry / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationZ", p.rz / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "Scale", p.scale, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleX", p.scaleX, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleY", p.scaleY, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleZ", p.scaleZ, "%.3f");
    char wcsv[256];
    WeaponTypesToCsv(p.weaponTypes, wcsv, sizeof(wcsv));
    AddIniValue(values, sec.c_str(), "Weapons", wcsv);
    AddIniValue(values, sec.c_str(), "WeaponsMode", p.weaponRequireAll ? "all" : "any");
    AddIniInt(values, sec.c_str(), "HideWeapons", p.hideSelectedWeapons ? 1 : 0);
    if (!OrcIniWriteValues(iniPath, "; OrcOutFit custom object config.\n\n", values))
        OrcLogError("SaveObjectSkinParamsToIni: cannot write %s", iniPath);
    InvalidateObjectSkinParamCache();
}

static bool ResolveObjectSkinParamsCached(const std::string& iniPath, const std::string& skinDff, CustomObjectSkinParams& out) {
    if (iniPath.empty() || skinDff.empty()) return false;
    if (g_livePreviewObjectActive &&
        _stricmp(g_livePreviewObjectIniPath.c_str(), iniPath.c_str()) == 0 &&
        _stricmp(g_livePreviewObjectSkinDff.c_str(), skinDff.c_str()) == 0) {
        out = g_livePreviewObjectParams;
        return true;
    }
    const std::string key = ToLowerAscii(iniPath) + "\x1e" + ToLowerAscii(skinDff);
    auto it = g_objectSkinParamCache.find(key);
    if (it != g_objectSkinParamCache.end()) {
        if (!it->second.found) return false;
        out = it->second.params;
        return true;
    }

    ObjectSkinParamCacheEntry entry{};
    if (!LoadObjectSkinParamsFromIni(iniPath.c_str(), skinDff.c_str(), entry.params)) {
        g_objectSkinParamCache[key] = entry;
        return false;
    }

    entry.found = true;
    out = entry.params;
    g_objectSkinParamCache[key] = entry;
    return true;
}

static std::vector<std::string> ParseCsvTokens(const char* csv) {
    std::vector<std::string> out;
    if (!csv || !csv[0]) return out;
    std::string token;
    auto flush = [&]() {
        std::string t = TrimAscii(token);
        token.clear();
        if (!t.empty()) out.push_back(t);
    };
    for (const char* p = csv; *p; ++p) {
        if (*p == ',' || *p == ';' || *p == '\r' || *p == '\n') {
            flush();
        } else {
            token += *p;
        }
    }
    flush();
    return out;
}

static std::string StandardObjectsIniPath() {
    return JoinPath(std::string(g_gameObjDir), "StandardObjects.ini");
}

static std::string StandardSkinsIniPath() {
    return JoinPath(std::string(g_gameSkinDir), "StandardSkins.ini");
}

static std::string StandardObjectSlotKey(int modelId, int slot) {
    char buf[40];
    _snprintf_s(buf, _TRUNCATE, "%d#%d", modelId, slot);
    return std::string(buf);
}

static bool ParseStandardObjectSlotKey(const std::string& token, int& modelId, int& slot) {
    modelId = -1;
    slot = 1;
    const std::string t = TrimAscii(token);
    if (t.empty()) return false;
    const char* p = t.c_str();
    char* end = nullptr;
    const long id = strtol(p, &end, 10);
    if (end == p || id < 0 || id > 30000) return false;
    modelId = (int)id;
    if (*end == '#') {
        char* slotEnd = nullptr;
        const long parsedSlot = strtol(end + 1, &slotEnd, 10);
        if (slotEnd == end + 1 || *slotEnd != '\0' || parsedSlot <= 0 || parsedSlot >= 1000)
            return false;
        slot = (int)parsedSlot;
    } else if (*end != '\0') {
        return false;
    }
    return true;
}

static std::string StandardObjectSkinIniSection(int modelId, int slot, const char* skinDffName) {
    return std::string("Object.") + StandardObjectSlotKey(modelId, slot) + ".Skin." + (skinDffName ? skinDffName : "");
}

static CBaseModelInfo* GetExistingStandardModelInfo(int modelId) {
    if (modelId < 0 || modelId >= CModelInfo::ms_modelInfoCount) return nullptr;
    if (!CModelInfo::ms_modelInfoPtrs) return nullptr;
    CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
    if (!mi) return nullptr;
    if (mi->m_pRwObject) return mi;
    if (!CStreaming::ms_aInfoForModel) return nullptr;
    __try {
        const CStreamingInfo& info = CStreaming::ms_aInfoForModel[modelId];
        if (info.m_nCdSize > 0 || info.m_nLoadState == LOADSTATE_LOADED)
            return mi;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("GetExistingStandardModelInfo: streaming info SEH ex=0x%08X model=%d", GetExceptionCode(), modelId);
    }
    return nullptr;
}

static CBaseModelInfo* GetExistingStandardObjectModelInfo(int modelId) {
    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    if (!mi) return nullptr;
    const eModelInfoType type = mi->GetModelType();
    if (type == MODEL_INFO_ATOMIC ||
        type == MODEL_INFO_TIME ||
        type == MODEL_INFO_WEAPON ||
        type == MODEL_INFO_CLUMP ||
        type == MODEL_INFO_LOD) {
        return mi;
    }
    return nullptr;
}

bool IsValidStandardObjectModel(int modelId) {
    return GetExistingStandardObjectModelInfo(modelId) != nullptr;
}

void SaveStandardObjectListToIni() {
    const std::string path = StandardObjectsIniPath();
    std::string entries;
    for (const auto& o : g_standardObjects) {
        if (!entries.empty()) entries += ",";
        entries += StandardObjectSlotKey(o.modelId, o.slot);
    }
    std::vector<OrcIniValue> values;
    AddIniValue(values, "Objects", "Entries", entries.c_str());
    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit standard game object config.\n\n", values))
        OrcLogError("SaveStandardObjectListToIni: cannot write %s", path.c_str());
}

void LoadStandardObjectsFromIni() {
    g_standardObjects.clear();
    InvalidateStandardObjectSkinParamCache();
    const std::string path = StandardObjectsIniPath();
    if (!FileExistsA(path.c_str())) return;

    char entries[4096] = {};
    GetPrivateProfileStringA("Objects", "Entries", "", entries, sizeof(entries), path.c_str());
    std::unordered_set<std::string> seen;
    for (const std::string& token : ParseCsvTokens(entries)) {
        int modelId = -1;
        int slot = 1;
        if (!ParseStandardObjectSlotKey(token, modelId, slot)) continue;
        if (!IsValidStandardObjectModel(modelId)) continue;
        const std::string key = StandardObjectSlotKey(modelId, slot);
        if (!seen.insert(key).second) continue;
        StandardObjectSlotCfg cfg;
        cfg.modelId = modelId;
        cfg.slot = slot;
        g_standardObjects.push_back(cfg);
    }
    OrcLogInfo("LoadStandardObjectsFromIni: %zu entries from %s", g_standardObjects.size(), path.c_str());
}

bool AddStandardObjectSlot(int modelId) {
    if (!IsValidStandardObjectModel(modelId))
        return false;
    int nextSlot = 1;
    for (const auto& o : g_standardObjects) {
        if (o.modelId == modelId && o.slot >= nextSlot)
            nextSlot = o.slot + 1;
    }
    StandardObjectSlotCfg cfg;
    cfg.modelId = modelId;
    cfg.slot = nextSlot;
    g_standardObjects.push_back(cfg);
    SaveStandardObjectListToIni();
    return true;
}

void RemoveStandardObjectSlot(size_t index) {
    if (index >= g_standardObjects.size()) return;
    const int modelId = g_standardObjects[index].modelId;
    const int slot = g_standardObjects[index].slot;
    g_standardObjects.erase(g_standardObjects.begin() + static_cast<long>(index));
    DestroyStandardObjectInstancesForSlot(modelId, slot);
    SaveStandardObjectListToIni();
}

static bool StandardObjectSkinIniSectionExists(int modelId, int slot, const char* skinDffName) {
    const std::string path = StandardObjectsIniPath();
    const std::string sec = StandardObjectSkinIniSection(modelId, slot, skinDffName);
    return ObjectSkinIniSectionExists(path.c_str(), sec.c_str());
}

bool LoadStandardObjectSkinParamsFromIni(int modelId, int slot, const char* skinDffName, CustomObjectSkinParams& out) {
    if (modelId < 0 || slot <= 0 || !skinDffName || !skinDffName[0]) return false;
    if (!IsValidStandardObjectModel(modelId)) return false;
    const std::string path = StandardObjectsIniPath();
    if (!StandardObjectSkinIniSectionExists(modelId, slot, skinDffName)) return false;
    const std::string sec = StandardObjectSkinIniSection(modelId, slot, skinDffName);

    char buf[64];
    auto F = [&](const char* key, float def)->float {
        GetPrivateProfileStringA(sec.c_str(), key, "", buf, sizeof(buf), path.c_str());
        if (!buf[0]) return def;
        return (float)atof(buf);
    };
    out.enabled = GetPrivateProfileIntA(sec.c_str(), "Enabled", out.enabled ? 1 : 0, path.c_str()) != 0;
    out.boneId = GetPrivateProfileIntA(sec.c_str(), "Bone", out.boneId, path.c_str());
    out.x = F("OffsetX", out.x);
    out.y = F("OffsetY", out.y);
    out.z = F("OffsetZ", out.z);
    out.rx = F("RotationX", out.rx / D2R) * D2R;
    out.ry = F("RotationY", out.ry / D2R) * D2R;
    out.rz = F("RotationZ", out.rz / D2R) * D2R;
    out.scale = F("Scale", out.scale);
    out.scaleX = F("ScaleX", out.scaleX);
    out.scaleY = F("ScaleY", out.scaleY);
    out.scaleZ = F("ScaleZ", out.scaleZ);

    char wcsv[256] = {};
    GetPrivateProfileStringA(sec.c_str(), "Weapons", "", wcsv, sizeof(wcsv), path.c_str());
    out.weaponTypes = ParseWeaponTypesCsv(wcsv);
    char mode[16] = {};
    GetPrivateProfileStringA(sec.c_str(), "WeaponsMode", "any", mode, sizeof(mode), path.c_str());
    out.weaponRequireAll = (ToLowerAscii(mode) == "all");
    out.hideSelectedWeapons = GetPrivateProfileIntA(sec.c_str(), "HideWeapons", out.hideSelectedWeapons ? 1 : 0, path.c_str()) != 0;
    return true;
}

struct StandardObjectSkinParamCacheEntry {
    bool found = false;
    CustomObjectSkinParams params{};
};

static std::unordered_map<std::string, StandardObjectSkinParamCacheEntry> g_standardObjectSkinParamCache;
bool g_livePreviewStandardObjectActive = false;
int g_livePreviewStandardObjectModelId = -1;
int g_livePreviewStandardObjectSlot = -1;
std::string g_livePreviewStandardObjectSkinDff;
CustomObjectSkinParams g_livePreviewStandardObjectParams{};

void InvalidateStandardObjectSkinParamCache() {
    g_standardObjectSkinParamCache.clear();
}

static bool ResolveStandardObjectSkinParamsCached(int modelId, int slot, const std::string& skinDff, CustomObjectSkinParams& out) {
    if (modelId < 0 || slot <= 0 || skinDff.empty()) return false;
    if (!IsValidStandardObjectModel(modelId)) return false;
    if (g_livePreviewStandardObjectActive &&
        g_livePreviewStandardObjectModelId == modelId &&
        g_livePreviewStandardObjectSlot == slot &&
        _stricmp(g_livePreviewStandardObjectSkinDff.c_str(), skinDff.c_str()) == 0) {
        out = g_livePreviewStandardObjectParams;
        return true;
    }

    const std::string key = StandardObjectSlotKey(modelId, slot) + "\x1e" + ToLowerAscii(skinDff);
    auto it = g_standardObjectSkinParamCache.find(key);
    if (it != g_standardObjectSkinParamCache.end()) {
        if (!it->second.found) return false;
        out = it->second.params;
        return true;
    }

    StandardObjectSkinParamCacheEntry entry{};
    if (!LoadStandardObjectSkinParamsFromIni(modelId, slot, skinDff.c_str(), entry.params)) {
        g_standardObjectSkinParamCache[key] = entry;
        return false;
    }

    entry.found = true;
    out = entry.params;
    g_standardObjectSkinParamCache[key] = entry;
    return true;
}

void SaveStandardObjectSkinParamsToIni(int modelId, int slot, const char* skinDffName, const CustomObjectSkinParams& p) {
    if (modelId < 0 || slot <= 0 || !skinDffName || !skinDffName[0]) return;
    if (!IsValidStandardObjectModel(modelId)) return;
    const std::string path = StandardObjectsIniPath();
    const std::string sec = StandardObjectSkinIniSection(modelId, slot, skinDffName);
    std::vector<OrcIniValue> values;
    AddIniInt(values, sec.c_str(), "Enabled", p.enabled ? 1 : 0);
    AddIniInt(values, sec.c_str(), "Bone", p.boneId);
    AddIniFloat(values, sec.c_str(), "OffsetX", p.x, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetY", p.y, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetZ", p.z, "%.3f");
    AddIniFloat(values, sec.c_str(), "RotationX", p.rx / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationY", p.ry / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationZ", p.rz / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "Scale", p.scale, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleX", p.scaleX, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleY", p.scaleY, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleZ", p.scaleZ, "%.3f");
    char wcsv[256];
    WeaponTypesToCsv(p.weaponTypes, wcsv, sizeof(wcsv));
    AddIniValue(values, sec.c_str(), "Weapons", wcsv);
    AddIniValue(values, sec.c_str(), "WeaponsMode", p.weaponRequireAll ? "all" : "any");
    AddIniInt(values, sec.c_str(), "HideWeapons", p.hideSelectedWeapons ? 1 : 0);
    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit standard game object config.\n\n", values))
        OrcLogError("SaveStandardObjectSkinParamsToIni: cannot write %s", path.c_str());
    InvalidateStandardObjectSkinParamCache();
}

static void DestroyCustomObjectInstance(CustomObjectCfg& o) {
    if (!o.rwObject) return;
    if (o.rwObject->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(o.rwObject));
    } else if (o.rwObject->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(o.rwObject);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    o.rwObject = nullptr;
}

static void DestroyCustomSkinInstance(CustomSkinCfg& s) {
    if (!s.rwObject) return;
    if (s.rwObject->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(s.rwObject));
    } else if (s.rwObject->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(s.rwObject);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    s.rwObject = nullptr;
}

static void DestroyStandardSkinInstance(StandardSkinCfg& s) {
    if (!s.rwObject) return;
    if (s.rwObject->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(s.rwObject));
    } else if (s.rwObject->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(s.rwObject);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    s.rwObject = nullptr;
}

static void DestroyAllRandomPoolSkins() {
    for (auto& pool : g_skinRandomPools)
        for (auto& v : pool.variants)
            DestroyCustomSkinInstance(v);
    g_skinRandomPools.clear();
    g_pedRandomSkinIdx.clear();
    g_skinRandomPoolModels = 0;
    g_skinRandomPoolVariants = 0;
}

static bool EnsureCustomModelLoaded(CustomObjectCfg& o) {
    if (o.rwObject) return true;
    if (!FileExistsA(o.dffPath.c_str())) {
        static std::unordered_set<std::string> s_once;
        if (s_once.insert(o.name).second)
            OrcLogError("object \"%s\": DFF missing %s", o.name.c_str(), o.dffPath.c_str());
        return false;
    }
    if (o.txdPath.empty() || !FileExistsA(o.txdPath.c_str())) {
        if (!o.txdMissingLogged) {
            o.txdMissingLogged = true;
            OrcLogError("object \"%s\": TXD missing or invalid path", o.name.c_str());
        }
        return false;
    }

    int txdSlot = CTxdStore::FindTxdSlot(o.name.c_str());
    if (txdSlot == -1) txdSlot = CTxdStore::AddTxdSlot(o.name.c_str());
    if (txdSlot == -1) {
        OrcLogError("object \"%s\": CTxdStore::AddTxdSlot failed", o.name.c_str());
        return false;
    }
    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)o.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        if (!o.txdMissingLogged) {
            o.txdMissingLogged = true;
            OrcLogError("object \"%s\": LoadTxd failed", o.name.c_str());
        }
        return false;
    }
    o.txdSlot = txdSlot;
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);

    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)o.dffPath.c_str());
    if (!stream) {
        CTxdStore::PopCurrentTxd();
        OrcLogError("object \"%s\": RwStreamOpen DFF failed", o.name.c_str());
        return false;
    }

    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* readClump = RpClumpStreamRead(stream);
        if (readClump) {
            o.rwObject = reinterpret_cast<RwObject*>(readClump);
            RpClumpForAllAtomics(readClump, InitAttachmentAtomicCB, nullptr);
            ok = true;
            OrcLogInfo("object \"%s\": clump loaded", o.name.c_str());
        }
    } else
        OrcLogError("object \"%s\": DFF has no CLUMP chunk", o.name.c_str());
    RwStreamClose(stream, nullptr);
    CTxdStore::PopCurrentTxd();
    return ok;
}

static bool EnsureCustomSkinLoaded(CustomSkinCfg& s) {
    if (s.rwObject) return true;
    if (!FileExistsA(s.dffPath.c_str())) {
        static std::unordered_set<std::string> s_once;
        if (s_once.insert(s.name).second)
            OrcLogError("skin \"%s\": DFF missing %s", s.name.c_str(), s.dffPath.c_str());
        return false;
    }
    if (s.txdPath.empty() || !FileExistsA(s.txdPath.c_str())) {
        if (!s.txdMissingLogged) {
            s.txdMissingLogged = true;
            OrcLogError("skin \"%s\": TXD missing or invalid path", s.name.c_str());
        }
        return false;
    }

    int txdSlot = CTxdStore::FindTxdSlot(s.name.c_str());
    if (txdSlot == -1) txdSlot = CTxdStore::AddTxdSlot(s.name.c_str());
    if (txdSlot == -1) {
        OrcLogError("skin \"%s\": CTxdStore::AddTxdSlot failed", s.name.c_str());
        return false;
    }
    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)s.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        OrcLogError("skin \"%s\": LoadTxd failed", s.name.c_str());
        return false;
    }
    s.txdSlot = txdSlot;

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)s.dffPath.c_str());
    if (!stream) {
        CTxdStore::PopCurrentTxd();
        OrcLogError("skin \"%s\": RwStreamOpen DFF failed", s.name.c_str());
        return false;
    }
    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* c = RpClumpStreamRead(stream);
        if (c) {
            s.rwObject = reinterpret_cast<RwObject*>(c);
            RpClumpForAllAtomics(c, InitAtomicCB, nullptr);
            ok = true;
            OrcLogInfo("skin \"%s\": clump loaded", s.name.c_str());
        }
    } else
        OrcLogError("skin \"%s\": DFF has no CLUMP chunk", s.name.c_str());
    RwStreamClose(stream, nullptr);
    CTxdStore::PopCurrentTxd();
    return ok;
}

static bool EnsureStandardSkinLoaded(StandardSkinCfg& s) {
    if (s.rwObject && !IsValidStandardSkinModel(s.modelId)) {
        DestroyStandardSkinInstance(s);
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: model no longer exists", s.dffName.c_str(), s.modelId);
        }
        return false;
    }
    if (s.rwObject) return true;
    if (!IsValidStandardSkinModel(s.modelId)) {
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: invalid ped model", s.dffName.c_str(), s.modelId);
        }
        return false;
    }

    if (!CStreaming::HasModelLoaded(s.modelId)) {
        CStreaming::RequestModel(s.modelId, 0);
        for (int i = 0; i < 16 && !CStreaming::HasModelLoaded(s.modelId); ++i)
            CStreaming::LoadAllRequestedModels(false);
        if (!CStreaming::HasModelLoaded(s.modelId))
            return false;
    }

    CBaseModelInfo* mi = CModelInfo::GetModelInfo(s.modelId);
    if (!mi || !mi->m_pRwObject)
        return false;

    RwObject* inst = mi->CreateInstance();
    if (!inst || inst->type != rpCLUMP) {
        if (inst && inst->type == rpATOMIC) {
            auto* a = reinterpret_cast<RpAtomic*>(inst);
            RwFrame* f = RpAtomicGetFrame(a);
            RpAtomicDestroy(a);
            if (f) RwFrameDestroy(f);
        }
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: CreateInstance failed or non-clump", s.dffName.c_str(), s.modelId);
        }
        return false;
    }

    s.rwObject = inst;
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), InitAtomicCB, nullptr);
    s.loadFailedLogged = false;
    OrcLogInfo("standard skin \"%s\" [%d]: clump loaded", s.dffName.c_str(), s.modelId);
    return true;
}

static void LoadSkinCfgFromIni(CustomSkinCfg& s) {
    if (s.iniPath.empty() || !FileExistsA(s.iniPath.c_str())) {
        s.bindToNick = false;
        s.nickListCsv.clear();
        s.nicknames.clear();
        return;
    }
    s.bindToNick = GetPrivateProfileIntA("NickBinding", "Enabled", 0, s.iniPath.c_str()) != 0;
    char buf[512] = {};
    GetPrivateProfileStringA("NickBinding", "Nicks", "", buf, sizeof(buf), s.iniPath.c_str());
    s.nickListCsv = buf;
    s.nicknames = ParseNickCsv(s.nickListCsv);
}

void SaveSkinCfgToIni(const CustomSkinCfg& s) {
    if (s.iniPath.empty()) return;
    std::string text;
    text.reserve(128 + s.nickListCsv.size());
    text += "; OrcOutFit custom skin config for ";
    text += s.name;
    text += "\n";
    text += "; Nicks: one per line and/or comma-separated (case-insensitive).\n\n";
    text += "[NickBinding]\n";
    text += "Enabled=";
    text += s.bindToNick ? "1\n" : "0\n";
    text += "Nicks=";
    text += s.nickListCsv;
    text += "\n";
    if (!OrcWriteTextFileAtomic(s.iniPath.c_str(), text)) {
        OrcLogError("SaveSkinCfgToIni: cannot write %s", s.iniPath.c_str());
        return;
    }
    InvalidateCustomSkinLookupCache();
}

static bool g_standardSkinLookupDirty = true;
static std::unordered_map<std::string, int> g_standardSkinNickLookup;

void InvalidateStandardSkinLookupCache() {
    g_standardSkinLookupDirty = true;
    g_standardSkinNickLookup.clear();
}

static std::string StandardSkinIniSection(const char* dffName) {
    return std::string("Skin.") + (dffName ? dffName : "");
}

static bool IsValidStandardSkinModel(int modelId) {
    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    return mi && mi->GetModelType() == MODEL_INFO_PED;
}

StandardSkinCfg* OrcGetStandardSkinCfgByModelId(int modelId, bool createIfMissing) {
    for (auto& s : g_standardSkins) {
        if (s.modelId == modelId) {
            if (!IsValidStandardSkinModel(modelId)) {
                DestroyStandardSkinInstance(s);
                return nullptr;
            }
            return &s;
        }
    }
    if (!createIfMissing || !IsValidStandardSkinModel(modelId))
        return nullptr;
    const char* dff = OrcTryGetPedModelNameById(modelId);
    if (!dff || !dff[0])
        return nullptr;
    StandardSkinCfg s;
    s.modelId = modelId;
    s.dffName = dff;
    g_standardSkins.push_back(std::move(s));
    return &g_standardSkins.back();
}

void LoadStandardSkinsFromIni() {
    for (auto& s : g_standardSkins)
        DestroyStandardSkinInstance(s);
    g_standardSkins.clear();
    InvalidateStandardSkinLookupCache();

    const std::string path = StandardSkinsIniPath();
    if (!FileExistsA(path.c_str())) return;

    char entries[4096] = {};
    GetPrivateProfileStringA("StandardSkins", "Entries", "", entries, sizeof(entries), path.c_str());
    std::unordered_set<std::string> seen;
    for (const std::string& token : ParseCsvTokens(entries)) {
        const std::string dff = TrimAscii(token);
        if (dff.empty()) continue;
        const std::string dffLower = ToLowerAscii(dff);
        if (!seen.insert(dffLower).second) continue;

        const std::string sec = StandardSkinIniSection(dff.c_str());
        const int modelId = GetPrivateProfileIntA(sec.c_str(), "ModelId", -1, path.c_str());
        if (!IsValidStandardSkinModel(modelId)) continue;

        StandardSkinCfg cfg;
        cfg.modelId = modelId;
        cfg.dffName = dff;
        cfg.bindToNick = GetPrivateProfileIntA(sec.c_str(), "Enabled", 0, path.c_str()) != 0;
        char nicks[512] = {};
        GetPrivateProfileStringA(sec.c_str(), "Nicks", "", nicks, sizeof(nicks), path.c_str());
        cfg.nickListCsv = nicks;
        cfg.nicknames = ParseNickCsv(cfg.nickListCsv);
        g_standardSkins.push_back(std::move(cfg));
    }
    OrcLogInfo("LoadStandardSkinsFromIni: %zu entries from %s", g_standardSkins.size(), path.c_str());
}

static void EnsureStandardSkinNickLookup() {
    if (!g_standardSkinLookupDirty) return;
    g_standardSkinNickLookup.clear();
    for (const auto& s : g_standardSkins) {
        if (!IsValidStandardSkinModel(s.modelId)) continue;
        if (!s.bindToNick || s.nicknames.empty()) continue;
        for (const auto& nick : s.nicknames) {
            if (!nick.empty())
                g_standardSkinNickLookup.emplace(nick, s.modelId);
        }
    }
    g_standardSkinLookupDirty = false;
}

void SaveStandardSkinCfgToIni(const StandardSkinCfg& s) {
    if (s.modelId < 0 || s.dffName.empty()) return;
    if (!IsValidStandardSkinModel(s.modelId)) return;
    const std::string path = StandardSkinsIniPath();

    char entriesBuf[4096] = {};
    GetPrivateProfileStringA("StandardSkins", "Entries", "", entriesBuf, sizeof(entriesBuf), path.c_str());
    std::vector<std::string> entries = ParseCsvTokens(entriesBuf);
    const std::string wantLower = ToLowerAscii(s.dffName);
    bool found = false;
    for (std::string& entry : entries) {
        if (ToLowerAscii(TrimAscii(entry)) == wantLower) {
            entry = s.dffName;
            found = true;
            break;
        }
    }
    if (!found)
        entries.push_back(s.dffName);

    std::string entriesCsv;
    for (const std::string& entry : entries) {
        const std::string trimmed = TrimAscii(entry);
        if (trimmed.empty()) continue;
        if (!entriesCsv.empty()) entriesCsv += ",";
        entriesCsv += trimmed;
    }

    const std::string sec = StandardSkinIniSection(s.dffName.c_str());
    std::vector<OrcIniValue> values;
    AddIniValue(values, "StandardSkins", "Entries", entriesCsv.c_str());
    AddIniInt(values, sec.c_str(), "ModelId", s.modelId);
    AddIniInt(values, sec.c_str(), "Enabled", s.bindToNick ? 1 : 0);
    AddIniValue(values, sec.c_str(), "Nicks", s.nickListCsv.c_str());
    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit standard game skin config.\n\n", values))
        OrcLogError("SaveStandardSkinCfgToIni: cannot write %s", path.c_str());
    InvalidateStandardSkinLookupCache();
}

static void DiscoverRandomSkinPools() {
    DestroyAllRandomPoolSkins();
    const std::string randomDir = JoinPath(std::string(g_gameSkinDir), "Random");
    DWORD attr = GetFileAttributesA(randomDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;

    std::string mask = JoinPath(randomDir, "*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string folder = fd.cFileName;
        if (folder == "." || folder == "..") continue;

        const int modelId = FindPedModelIdByDffName(folder);
        if (!IsValidStandardSkinModel(modelId)) continue;

        const std::string poolDir = JoinPath(randomDir, folder);
        std::string fileMask = JoinPath(poolDir, "*.*");
        WIN32_FIND_DATAA fileData{};
        HANDLE hf = FindFirstFileA(fileMask.c_str(), &fileData);
        if (hf == INVALID_HANDLE_VALUE) continue;

        SkinRandomPool pool;
        pool.modelId = modelId;
        pool.folderName = folder;

        do {
            if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string fname = fileData.cFileName;
            if (LowerExt(fname) != ".dff") continue;
            const std::string base = BaseNameNoExt(fname);
            CustomSkinCfg s;
            s.name = folder + "/" + base;
            s.dffPath = JoinPath(poolDir, fname);
            s.txdPath = FindBestTxdPath(poolDir, base);
            s.iniPath.clear();
            s.remapKey = base;
            s.remapFallbackKey = folder;
            pool.variants.push_back(std::move(s));
        } while (FindNextFileA(hf, &fileData));
        FindClose(hf);

        if (!pool.variants.empty())
            g_skinRandomPools.push_back(std::move(pool));
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    g_skinRandomPoolModels = (int)g_skinRandomPools.size();
    g_skinRandomPoolVariants = 0;
    for (const auto& pool : g_skinRandomPools)
        g_skinRandomPoolVariants += (int)pool.variants.size();
    OrcLogInfo("DiscoverRandomSkinPools: %d pools, %d variants in %s",
        g_skinRandomPoolModels, g_skinRandomPoolVariants, randomDir.c_str());
}

void DiscoverCustomObjectsAndEnsureIni() {
    for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
    g_customObjects.clear();
    InvalidateObjectSkinParamCache();

    DWORD attr = GetFileAttributesA(g_gameObjDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            OrcLogInfo("Objects folder missing or not a directory: %s", g_gameObjDir);
        }
        return;
    }

    std::string dir = g_gameObjDir;
    std::string mask = JoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    int foundDff = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (LowerExt(fname) != ".dff") continue;

        const std::string base = BaseNameNoExt(fname);
        CustomObjectCfg o;
        o.name = base;
        o.dffPath = JoinPath(dir, base + ".dff");
        o.txdPath = FindBestTxdPath(dir, base);
        o.iniPath = JoinPath(dir, base + ".ini");
        CreateObjectIniStubIfMissing(o.iniPath, base);
        g_customObjects.push_back(o);
        foundDff++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    OrcLogInfo("DiscoverCustomObjects: %d DFF in %s", foundDff, g_gameObjDir);
}

void DiscoverCustomSkins() {
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    g_customSkins.clear();
    InvalidateCustomSkinLookupCache();
    DestroyAllRandomPoolSkins();
    DWORD attr = GetFileAttributesA(g_gameSkinDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            OrcLogInfo("Skins folder missing or not a directory: %s", g_gameSkinDir);
        }
        return;
    }
    std::string dir = g_gameSkinDir;
    std::string mask = JoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (LowerExt(fname) != ".dff") continue;
        const std::string base = BaseNameNoExt(fname);
        CustomSkinCfg s;
        s.name = base;
        s.dffPath = JoinPath(dir, base + ".dff");
        s.txdPath = FindBestTxdPath(dir, base);
        s.iniPath = JoinPath(dir, base + ".ini");
        s.remapKey = base;
        CreateDefaultSkinIniIfMissing(s.iniPath, base);
        LoadSkinCfgFromIni(s);
        g_customSkins.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    OrcLogInfo("DiscoverCustomSkins: %zu skins in %s", g_customSkins.size(), g_gameSkinDir);
    DiscoverRandomSkinPools();
    if (!g_skinSelectedName.empty()) {
        for (int i = 0; i < (int)g_customSkins.size(); i++) {
            if (ToLowerAscii(g_customSkins[i].name) == ToLowerAscii(g_skinSelectedName)) {
                g_uiSkinIdx = i;
                break;
            }
        }
    }
    if (g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
    g_uiSkinEditIdx = -1;
}

void OrcCollectRandomSkinPools(std::vector<SkinRandomPoolInfo>& out) {
    out.clear();
    for (const auto& pool : g_skinRandomPools) {
        SkinRandomPoolInfo info;
        info.dffName = pool.folderName;
        info.modelId = pool.modelId;
        info.variants = (int)pool.variants.size();
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SkinRandomPoolInfo& a, const SkinRandomPoolInfo& b) {
        return a.modelId < b.modelId;
    });
}

void OrcCollectRandomSkinPreviewVariants(std::vector<SkinPreviewRandomVariantInfo>& out) {
    out.clear();
    for (const auto& pool : g_skinRandomPools) {
        for (int i = 0; i < (int)pool.variants.size(); ++i) {
            const CustomSkinCfg& skin = pool.variants[(size_t)i];
            SkinPreviewRandomVariantInfo info;
            info.label = skin.name.empty() ? (pool.folderName + "/" + std::to_string(i + 1)) : skin.name;
            info.poolDffName = pool.folderName;
            info.modelId = pool.modelId;
            info.variantIndex = i;
            out.push_back(std::move(info));
        }
    }
    std::sort(out.begin(), out.end(), [](const SkinPreviewRandomVariantInfo& a, const SkinPreviewRandomVariantInfo& b) {
        if (a.modelId != b.modelId) return a.modelId < b.modelId;
        return a.label < b.label;
    });
}

void SaveSkinModeIni() {
    std::vector<OrcIniValue> values;
    AppendSkinFeatureIniValues(values);
    AppendSkinModeIniValues(values);
    if (!OrcIniWriteValues(g_iniPath, "; OrcOutFit configuration.\n\n", values))
        OrcLogError("SaveSkinModeIni: cannot write %s", g_iniPath);
}

static bool LoadWeaponOverridesFromIni(const char* fullIni, std::vector<WeaponCfg>* outCfg) {
    if (!fullIni || !outCfg) return false;
    if (!FileExistsA(fullIni)) return false;
    *outCfg = g_cfg;
    bool any = false;
    for (int i = 0; i < (int)outCfg->size(); i++) {
        auto& c = (*outCfg)[i];
        if (c.name && c.name[0]) ReadSectionFromIni(c, c.name, fullIni);
        char sec[32];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d", i);
        if (GetPrivateProfileIntA(sec, "Bone", 0, fullIni) != 0) {
            if (!c.name || !c.name[0]) { c.scale = 1.0f; c.enabled = true; }
            ReadSectionFromIni(c, sec, fullIni);
            any = true;
        }
    }
    return any;
}

static bool LoadWeaponOverridesFromIni2(const char* fullIni,
                                       std::vector<WeaponCfg>* outCfg,
                                       std::vector<WeaponCfg>* outCfg2) {
    if (!outCfg || !outCfg2) return false;
    const bool any1 = LoadWeaponOverridesFromIni(fullIni, outCfg);
    *outCfg2 = g_cfg2;
    if (!fullIni || !FileExistsA(fullIni)) return any1;
    bool any2 = false;
    for (int i = 0; i < (int)outCfg2->size(); i++) {
        auto& c = (*outCfg2)[i];
        if (g_cfg.size() > (size_t)i && g_cfg[i].name && g_cfg[i].name[0]) {
            char sec2[64];
            _snprintf_s(sec2, _TRUNCATE, "%s2", g_cfg[i].name);
            if (GetPrivateProfileIntA(sec2, "Bone", 0, fullIni) != 0) {
                c.enabled = true;
                ReadSectionFromIni(c, sec2, fullIni);
                any2 = true;
            }
        }
        char sec[20];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d_2", i);
        if (GetPrivateProfileIntA(sec, "Bone", 0, fullIni) != 0) {
            c.enabled = true;
            ReadSectionFromIni(c, sec, fullIni);
            any2 = true;
        }
    }
    return any1 || any2;
}

void OrcLoadWeaponPresetFile(const char* fullPath, std::vector<WeaponCfg>& w1, std::vector<WeaponCfg>& w2) {
    w1 = g_cfg;
    w2 = g_cfg2;
    if (!fullPath || !fullPath[0] || !FileExistsA(fullPath)) return;
    LoadWeaponOverridesFromIni2(fullPath, &w1, &w2);
    OrcLogInfo("weapon preset loaded: %s", fullPath);
}

static std::unordered_map<std::string, std::vector<WeaponCfg>> g_weaponSkinOv1;
static std::unordered_map<std::string, std::vector<WeaponCfg>> g_weaponSkinOv2;

struct WeaponSkinIniPathCacheEntry {
    bool found = false;
    std::string path;
};

static std::unordered_map<std::string, WeaponSkinIniPathCacheEntry> g_weaponSkinIniPathCache;

void InvalidatePerSkinWeaponCache() {
    g_weaponSkinOv1.clear();
    g_weaponSkinOv2.clear();
    g_weaponSkinIniPathCache.clear();
}

std::string GetPedStdSkinDffName(CPed* ped) {
    if (!ped) return {};
    if (const char* hook = OrcTryGetPedModelNameById((int)ped->m_nModelIndex))
        return std::string(hook);
    if (ped->IsPlayer()) {
        auto* pl = reinterpret_cast<CPlayerPed*>(ped);
        CPlayerInfo* pi = pl->GetPlayerInfoForThisPlayerPed();
        if (pi && pi->m_szSkinName[0])
            return std::string(pi->m_szSkinName);
    }
    return {};
}

void OrcCollectPedSkins(std::vector<std::pair<std::string, int>>& out) {
    out.clear();
    for (int id = 0; id < (int)g_pedModelNameById.size(); id++) {
        if (g_pedModelNameById[id].empty()) continue;
        if (!IsValidStandardSkinModel(id)) continue;
        out.push_back({ g_pedModelNameById[id], id });
    }
    std::sort(out.begin(), out.end(), [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
        return a.second < b.second;
    });
}

bool ResolveWeaponsIniForSkinDff(const char* skinDffName, char* outPath, size_t outPathChars) {
    if (!skinDffName || !skinDffName[0] || !outPath || outPathChars == 0) return false;
    outPath[0] = 0;
    const std::string want = ToLowerAscii(std::string(skinDffName));
    auto cached = g_weaponSkinIniPathCache.find(want);
    if (cached != g_weaponSkinIniPathCache.end()) {
        if (!cached->second.found)
            return false;
        _snprintf_s(outPath, outPathChars, _TRUNCATE, "%s", cached->second.path.c_str());
        return outPath[0] != 0;
    }

    DWORD da = GetFileAttributesA(g_gameWeaponsDir);
    if (da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) {
        g_weaponSkinIniPathCache[want] = WeaponSkinIniPathCacheEntry{};
        return false;
    }

    std::string mask = JoinPath(std::string(g_gameWeaponsDir), "*.ini");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        g_weaponSkinIniPathCache[want] = WeaponSkinIniPathCacheEntry{};
        return false;
    }
    bool ok = false;
    std::string foundPath;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (LowerExt(fname) != ".ini") continue;
        if (ToLowerAscii(BaseNameNoExt(fname)) == want) {
            foundPath = JoinPath(std::string(g_gameWeaponsDir), fname);
            _snprintf_s(outPath, outPathChars, _TRUNCATE, "%s", foundPath.c_str());
            ok = true;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    WeaponSkinIniPathCacheEntry entry{};
    entry.found = ok;
    if (ok) entry.path = foundPath;
    g_weaponSkinIniPathCache[want] = std::move(entry);
    return ok;
}

static void EnsureWeaponSkinOverrideLoaded(const std::string& skinKeyLower, const char* iniPath) {
    if (skinKeyLower.empty() || !iniPath || !iniPath[0]) return;
    if (g_weaponSkinOv1.find(skinKeyLower) != g_weaponSkinOv1.end()) return;
    std::vector<WeaponCfg> a = g_cfg;
    std::vector<WeaponCfg> b = g_cfg2;
    (void)LoadWeaponOverridesFromIni2(iniPath, &a, &b);
    g_weaponSkinOv1[skinKeyLower] = std::move(a);
    g_weaponSkinOv2[skinKeyLower] = std::move(b);
}

static const WeaponCfg& GetWeaponCfgForPed(CPed* ped, int wt) {
    if (!ped || wt < 0 || wt >= (int)g_cfg.size()) return g_cfg[0];
    const std::string dff = GetPedStdSkinDffName(ped);
    if (g_livePreviewWeaponsActive &&
        !dff.empty() &&
        _stricmp(g_livePreviewWeaponSkinDff.c_str(), dff.c_str()) == 0 &&
        wt < (int)g_livePreviewWeapon1.size()) {
        return g_livePreviewWeapon1[wt];
    }
    if (dff.empty()) return g_cfg[wt];
    char wpath[MAX_PATH];
    if (!ResolveWeaponsIniForSkinDff(dff.c_str(), wpath, sizeof(wpath))) return g_cfg[wt];
    const std::string key = ToLowerAscii(dff);
    EnsureWeaponSkinOverrideLoaded(key, wpath);
    auto it = g_weaponSkinOv1.find(key);
    if (it == g_weaponSkinOv1.end() || wt >= (int)it->second.size()) return g_cfg[wt];
    return it->second[wt];
}

static const WeaponCfg& GetWeaponCfg2ForPed(CPed* ped, int wt) {
    if (!ped || wt < 0 || wt >= (int)g_cfg2.size()) return g_cfg2[0];
    const std::string dff = GetPedStdSkinDffName(ped);
    if (g_livePreviewWeaponsActive &&
        !dff.empty() &&
        _stricmp(g_livePreviewWeaponSkinDff.c_str(), dff.c_str()) == 0 &&
        wt < (int)g_livePreviewWeapon2.size()) {
        return g_livePreviewWeapon2[wt];
    }
    if (dff.empty()) return g_cfg2[wt];
    char wpath[MAX_PATH];
    if (!ResolveWeaponsIniForSkinDff(dff.c_str(), wpath, sizeof(wpath))) return g_cfg2[wt];
    const std::string key = ToLowerAscii(dff);
    EnsureWeaponSkinOverrideLoaded(key, wpath);
    auto it = g_weaponSkinOv2.find(key);
    if (it == g_weaponSkinOv2.end() || wt >= (int)it->second.size()) return g_cfg2[wt];
    return it->second[wt];
}

struct WeaponReplacementAsset {
    std::string key;
    std::string weaponNameLower;
    std::string matchNameLower;
    std::string displayName;
    std::string dffPath;
    std::string txdPath;
    int txdSlot = -1;
    RwObject* rwObject = nullptr;
    bool loadAttempted = false;
    bool loadFailedLogged = false;
};

static std::vector<WeaponReplacementAsset> g_weaponReplacementAssets;
static std::unordered_map<std::string, int> g_weaponReplacementByNick;
static std::unordered_map<std::string, int> g_weaponReplacementBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBags;
static std::unordered_map<std::string, int> g_weaponReplacementRandomChoiceByPed;
static WeaponReplacementStats g_weaponReplacementStats;

static std::string MakeWeaponReplacementKey(const std::string& weaponLower, const std::string& matchLower) {
    return weaponLower + "|" + matchLower;
}

static std::string StripSampColorCodes(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        if (value[i] == '{' && i + 8 <= value.size()) {
            size_t j = i + 1;
            int hex = 0;
            while (j < value.size() && hex < 8) {
                const char c = value[j];
                const bool isHex =
                    (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
                if (!isHex) break;
                ++j;
                ++hex;
            }
            if ((hex == 6 || hex == 8) && j < value.size() && value[j] == '}') {
                i = j + 1;
                continue;
            }
        }
        out.push_back(value[i++]);
    }
    return out;
}

static std::string GetWeaponModelBaseNameLower(int wt) {
    if (wt > 0 && wt < (int)g_weaponDatIdeName.size() && !g_weaponDatIdeName[(size_t)wt].empty())
        return ToLowerAscii(g_weaponDatIdeName[(size_t)wt]);
    if (wt > 0 && wt < (int)g_cfg.size() && g_cfg[(size_t)wt].name && g_cfg[(size_t)wt].name[0])
        return ToLowerAscii(g_cfg[(size_t)wt].name);
    return {};
}

static std::vector<std::string> KnownWeaponModelNamesLower() {
    std::vector<std::string> names;
    for (int wt : g_availableWeaponTypes) {
        std::string name = GetWeaponModelBaseNameLower(wt);
        if (!name.empty())
            names.push_back(name);
        if (wt > 0 && wt < (int)g_cfg.size() && g_cfg[(size_t)wt].name && g_cfg[(size_t)wt].name[0])
            names.push_back(ToLowerAscii(g_cfg[(size_t)wt].name));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() > b.size();
        return a < b;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

static void DestroyRwObjectInstance(RwObject*& obj) {
    if (!obj) return;
    if (obj->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(obj));
    } else if (obj->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(obj);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    obj = nullptr;
}

static void DestroyWeaponReplacementAssets() {
    for (auto& asset : g_weaponReplacementAssets) {
        DestroyRwObjectInstance(asset.rwObject);
        asset.txdSlot = -1;
    }
    g_weaponReplacementAssets.clear();
    g_weaponReplacementByNick.clear();
    g_weaponReplacementBySkin.clear();
    g_weaponReplacementRandomBySkin.clear();
    g_weaponReplacementRandomBags.clear();
    g_weaponReplacementRandomChoiceByPed.clear();
    g_weaponReplacementStats = {};
}

static bool EnsureWeaponReplacementAssetLoaded(WeaponReplacementAsset& asset) {
    if (asset.rwObject)
        return true;
    if (asset.loadAttempted)
        return false;
    asset.loadAttempted = true;

    if (!FileExistsA(asset.dffPath.c_str())) {
        if (!asset.loadFailedLogged) {
            asset.loadFailedLogged = true;
            OrcLogError("weapon replacement \"%s\": DFF missing %s", asset.displayName.c_str(), asset.dffPath.c_str());
        }
        return false;
    }
    if (asset.txdPath.empty() || !FileExistsA(asset.txdPath.c_str())) {
        if (!asset.loadFailedLogged) {
            asset.loadFailedLogged = true;
            OrcLogError("weapon replacement \"%s\": TXD missing or invalid path", asset.displayName.c_str());
        }
        return false;
    }

    std::string txdName = "orc_wr_" + asset.key;
    for (char& c : txdName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    int txdSlot = CTxdStore::FindTxdSlot(txdName.c_str());
    if (txdSlot == -1)
        txdSlot = CTxdStore::AddTxdSlot(txdName.c_str());
    if (txdSlot == -1) {
        OrcLogError("weapon replacement \"%s\": CTxdStore::AddTxdSlot failed", asset.displayName.c_str());
        return false;
    }

    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        OrcLogError("weapon replacement \"%s\": LoadTxd failed", asset.displayName.c_str());
        return false;
    }
    asset.txdSlot = txdSlot;

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.dffPath.c_str());
    if (!stream) {
        CTxdStore::PopCurrentTxd();
        OrcLogError("weapon replacement \"%s\": RwStreamOpen DFF failed", asset.displayName.c_str());
        return false;
    }

    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* c = RpClumpStreamRead(stream);
        if (c) {
            asset.rwObject = reinterpret_cast<RwObject*>(c);
            RpClumpForAllAtomics(c, InitAttachmentAtomicCB, nullptr);
            ok = true;
        }
    }
    RwStreamClose(stream, nullptr);

    if (!ok) {
        stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.dffPath.c_str());
        if (stream) {
            if (RwStreamFindChunk(stream, rwID_ATOMIC, nullptr, nullptr)) {
                RpAtomic* a = RpAtomicStreamRead(stream);
                if (a) {
                    if (!RpAtomicGetFrame(a)) {
                        RwFrame* frame = RwFrameCreate();
                        if (frame) RpAtomicSetFrame(a, frame);
                    }
                    InitAttachmentAtomicCB(a, nullptr);
                    asset.rwObject = reinterpret_cast<RwObject*>(a);
                    ok = true;
                }
            }
            RwStreamClose(stream, nullptr);
        }
    }
    CTxdStore::PopCurrentTxd();

    if (!ok) {
        OrcLogError("weapon replacement \"%s\": DFF has no readable clump/atomic", asset.displayName.c_str());
        return false;
    }

    asset.loadFailedLogged = false;
    OrcLogInfo("weapon replacement \"%s\": loaded", asset.displayName.c_str());
    return true;
}

static RwObject* CloneWeaponReplacementObject(WeaponReplacementAsset& asset) {
    if (!EnsureWeaponReplacementAssetLoaded(asset) || !asset.rwObject)
        return nullptr;
    if (asset.rwObject->type == rpCLUMP) {
        RpClump* clone = RpClumpClone(reinterpret_cast<RpClump*>(asset.rwObject));
        if (!clone) return nullptr;
        RpClumpForAllAtomics(clone, InitAttachmentAtomicCB, nullptr);
        return reinterpret_cast<RwObject*>(clone);
    }
    if (asset.rwObject->type == rpATOMIC) {
        RpAtomic* clone = RpAtomicClone(reinterpret_cast<RpAtomic*>(asset.rwObject));
        if (!clone) return nullptr;
        RwFrame* frame = RwFrameCreate();
        if (frame) RpAtomicSetFrame(clone, frame);
        InitAttachmentAtomicCB(clone, nullptr);
        return reinterpret_cast<RwObject*>(clone);
    }
    return nullptr;
}

static void AddWeaponReplacementAsset(const std::string& key,
                                      const std::string& weaponLower,
                                      const std::string& matchLower,
                                      const std::string& displayName,
                                      const std::string& dffPath,
                                      const std::string& txdPath,
                                      std::unordered_map<std::string, int>* directMap,
                                      std::unordered_map<std::string, std::vector<int>>* randomMap) {
    WeaponReplacementAsset asset;
    asset.key = key;
    asset.weaponNameLower = weaponLower;
    asset.matchNameLower = matchLower;
    asset.displayName = displayName;
    asset.dffPath = dffPath;
    asset.txdPath = txdPath;
    const int index = (int)g_weaponReplacementAssets.size();
    g_weaponReplacementAssets.push_back(std::move(asset));

    const std::string mapKey = MakeWeaponReplacementKey(weaponLower, matchLower);
    if (directMap) {
        if (directMap->find(mapKey) == directMap->end())
            (*directMap)[mapKey] = index;
    }
    if (randomMap)
        (*randomMap)[mapKey].push_back(index);
}

void DiscoverWeaponReplacements() {
    ClearAllWeaponReplacementInstances();
    DestroyWeaponReplacementAssets();

    const std::string gunsDir = g_gameWeaponGunsDir;
    DWORD gunsAttr = GetFileAttributesA(gunsDir.c_str());
    if (gunsAttr != INVALID_FILE_ATTRIBUTES && (gunsAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string weaponMask = JoinPath(gunsDir, "*");
        WIN32_FIND_DATAA weaponData{};
        HANDLE hw = FindFirstFileA(weaponMask.c_str(), &weaponData);
        if (hw != INVALID_HANDLE_VALUE) {
            do {
                if (!(weaponData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::string weaponFolder = weaponData.cFileName;
                if (weaponFolder == "." || weaponFolder == "..") continue;
                const std::string weaponLower = ToLowerAscii(weaponFolder);
                const std::string weaponDir = JoinPath(gunsDir, weaponFolder);

                std::string fileMask = JoinPath(weaponDir, "*.*");
                WIN32_FIND_DATAA fileData{};
                HANDLE hf = FindFirstFileA(fileMask.c_str(), &fileData);
                if (hf != INVALID_HANDLE_VALUE) {
                    do {
                        if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        const std::string fname = fileData.cFileName;
                        if (LowerExt(fname) != ".dff") continue;
                        const std::string base = BaseNameNoExt(fname);
                        const std::string skinLower = ToLowerAscii(base);
                        AddWeaponReplacementAsset(
                            "skin:" + weaponLower + ":" + skinLower,
                            weaponLower,
                            skinLower,
                            weaponFolder + "/" + base,
                            JoinPath(weaponDir, fname),
                            FindBestTxdPath(weaponDir, base),
                            &g_weaponReplacementBySkin,
                            nullptr);
                    } while (FindNextFileA(hf, &fileData));
                    FindClose(hf);
                }

                std::string skinDirMask = JoinPath(weaponDir, "*");
                WIN32_FIND_DATAA skinDirData{};
                HANDLE hs = FindFirstFileA(skinDirMask.c_str(), &skinDirData);
                if (hs != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(skinDirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        const std::string skinFolder = skinDirData.cFileName;
                        if (skinFolder == "." || skinFolder == "..") continue;
                        const std::string skinLower = ToLowerAscii(skinFolder);
                        const std::string skinDir = JoinPath(weaponDir, skinFolder);
                        std::string variantMask = JoinPath(skinDir, "*.*");
                        WIN32_FIND_DATAA variantData{};
                        HANDLE hv = FindFirstFileA(variantMask.c_str(), &variantData);
                        if (hv == INVALID_HANDLE_VALUE) continue;
                        do {
                            if (variantData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                            const std::string fname = variantData.cFileName;
                            if (LowerExt(fname) != ".dff") continue;
                            const std::string base = BaseNameNoExt(fname);
                            AddWeaponReplacementAsset(
                                "skinrandom:" + weaponLower + ":" + skinLower + ":" + ToLowerAscii(base),
                                weaponLower,
                                skinLower,
                                weaponFolder + "/" + skinFolder + "/" + base,
                                JoinPath(skinDir, fname),
                                FindBestTxdPath(skinDir, base),
                                nullptr,
                                &g_weaponReplacementRandomBySkin);
                        } while (FindNextFileA(hv, &variantData));
                        FindClose(hv);
                    } while (FindNextFileA(hs, &skinDirData));
                    FindClose(hs);
                }
            } while (FindNextFileA(hw, &weaponData));
            FindClose(hw);
        }
    }

    const std::vector<std::string> knownWeapons = KnownWeaponModelNamesLower();
    const std::string nickDir = g_gameWeaponGunsNickDir;
    DWORD nickAttr = GetFileAttributesA(nickDir.c_str());
    if (nickAttr != INVALID_FILE_ATTRIBUTES && (nickAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string nickMask = JoinPath(nickDir, "*.*");
        WIN32_FIND_DATAA nickData{};
        HANDLE hn = FindFirstFileA(nickMask.c_str(), &nickData);
        if (hn != INVALID_HANDLE_VALUE) {
            do {
                if (nickData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string fname = nickData.cFileName;
                if (LowerExt(fname) != ".dff") continue;
                const std::string base = BaseNameNoExt(fname);
                const std::string baseLower = ToLowerAscii(base);
                std::string weaponLower;
                std::string nickLower;
                for (const std::string& known : knownWeapons) {
                    const std::string prefix = known + "_";
                    if (baseLower.rfind(prefix, 0) == 0 && baseLower.size() > prefix.size()) {
                        weaponLower = known;
                        nickLower = baseLower.substr(prefix.size());
                        break;
                    }
                }
                if (weaponLower.empty()) {
                    const size_t underscore = baseLower.find('_');
                    if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= baseLower.size())
                        continue;
                    weaponLower = baseLower.substr(0, underscore);
                    nickLower = baseLower.substr(underscore + 1);
                }
                AddWeaponReplacementAsset(
                    "nick:" + weaponLower + ":" + nickLower,
                    weaponLower,
                    nickLower,
                    base,
                    JoinPath(nickDir, fname),
                    FindBestTxdPath(nickDir, base),
                    &g_weaponReplacementByNick,
                    nullptr);
            } while (FindNextFileA(hn, &nickData));
            FindClose(hn);
        }
    }

    g_weaponReplacementStats.uniqueSkinWeapons = (int)g_weaponReplacementBySkin.size();
    g_weaponReplacementStats.randomSkinWeapons = 0;
    for (const auto& kv : g_weaponReplacementRandomBySkin)
        g_weaponReplacementStats.randomSkinWeapons += (int)kv.second.size();
    g_weaponReplacementStats.nickWeapons = (int)g_weaponReplacementByNick.size();
    OrcLogInfo("DiscoverWeaponReplacements: skin=%d random=%d nick=%d",
        g_weaponReplacementStats.uniqueSkinWeapons,
        g_weaponReplacementStats.randomSkinWeapons,
        g_weaponReplacementStats.nickWeapons);
}

WeaponReplacementStats OrcGetWeaponReplacementStats() {
    return g_weaponReplacementStats;
}

struct WeaponTextureAsset {
    std::string key;
    std::string weaponNameLower;
    std::string matchNameLower;
    std::string displayName;
    std::string txdPath;
    int txdSlot = -1;
    bool loadAttempted = false;
    bool loadFailedLogged = false;
};

struct WeaponTextureRestoreEntry {
    RpMaterial* material = nullptr;
    RwTexture* texture = nullptr;
};

static std::vector<WeaponTextureAsset> g_weaponTextureAssets;
static std::unordered_map<std::string, int> g_weaponTextureByNick;
static std::unordered_map<std::string, int> g_weaponTextureBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponTextureRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponTextureRandomBags;
static std::unordered_map<std::string, int> g_weaponTextureRandomChoiceByPed;
static std::vector<WeaponTextureRestoreEntry> g_weaponTextureRestoreEntries;
static WeaponTextureStats g_weaponTextureStats;

static RwTexDictionary* GetTxdDictionaryByIndexMain(int txdIndex) {
    if (txdIndex < 0 || !CTxdStore::ms_pTxdPool || !CTxdStore::ms_pTxdPool->m_pObjects)
        return nullptr;
    if (txdIndex >= CTxdStore::ms_pTxdPool->m_nSize)
        return nullptr;
    return CTxdStore::ms_pTxdPool->m_pObjects[txdIndex].m_pRwDictionary;
}

static void RestoreWeaponTextureOverrides() {
    if (g_weaponTextureRestoreEntries.empty())
        return;
    for (auto it = g_weaponTextureRestoreEntries.rbegin(); it != g_weaponTextureRestoreEntries.rend(); ++it) {
        if (!it->material) continue;
        __try {
            it->material->texture = it->texture;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_weaponTextureRestoreEntries.clear();
}

static void DestroyWeaponTextureAssets() {
    RestoreWeaponTextureOverrides();
    g_weaponTextureAssets.clear();
    g_weaponTextureByNick.clear();
    g_weaponTextureBySkin.clear();
    g_weaponTextureRandomBySkin.clear();
    g_weaponTextureRandomBags.clear();
    g_weaponTextureRandomChoiceByPed.clear();
    g_weaponTextureStats = {};
}

static bool EnsureWeaponTextureAssetLoaded(WeaponTextureAsset& asset) {
    if (asset.txdSlot >= 0 && GetTxdDictionaryByIndexMain(asset.txdSlot))
        return true;
    if (asset.loadAttempted)
        return false;
    asset.loadAttempted = true;

    if (asset.txdPath.empty() || !FileExistsA(asset.txdPath.c_str())) {
        if (!asset.loadFailedLogged) {
            asset.loadFailedLogged = true;
            OrcLogError("weapon texture \"%s\": TXD missing %s", asset.displayName.c_str(), asset.txdPath.c_str());
        }
        return false;
    }

    std::string txdName = "orc_wtex_" + asset.key;
    for (char& c : txdName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    int txdSlot = CTxdStore::FindTxdSlot(txdName.c_str());
    if (txdSlot == -1)
        txdSlot = CTxdStore::AddTxdSlot(txdName.c_str());
    if (txdSlot == -1) {
        OrcLogError("weapon texture \"%s\": CTxdStore::AddTxdSlot failed", asset.displayName.c_str());
        return false;
    }

    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        OrcLogError("weapon texture \"%s\": LoadTxd failed", asset.displayName.c_str());
        return false;
    }

    asset.txdSlot = txdSlot;
    asset.loadFailedLogged = false;
    OrcLogInfo("weapon texture \"%s\": loaded", asset.displayName.c_str());
    return true;
}

static void AddWeaponTextureAsset(const std::string& key,
                                  const std::string& weaponLower,
                                  const std::string& matchLower,
                                  const std::string& displayName,
                                  const std::string& txdPath,
                                  std::unordered_map<std::string, int>* directMap,
                                  std::unordered_map<std::string, std::vector<int>>* randomMap) {
    WeaponTextureAsset asset;
    asset.key = key;
    asset.weaponNameLower = weaponLower;
    asset.matchNameLower = matchLower;
    asset.displayName = displayName;
    asset.txdPath = txdPath;
    const int index = (int)g_weaponTextureAssets.size();
    g_weaponTextureAssets.push_back(std::move(asset));

    const std::string mapKey = MakeWeaponReplacementKey(weaponLower, matchLower);
    if (directMap && directMap->find(mapKey) == directMap->end())
        (*directMap)[mapKey] = index;
    if (randomMap)
        (*randomMap)[mapKey].push_back(index);
}

void DiscoverWeaponTextures() {
    DestroyWeaponTextureAssets();
    const std::string texturesDir = g_gameWeaponTexturesDir;
    DWORD attr = GetFileAttributesA(texturesDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;

    std::string weaponMask = JoinPath(texturesDir, "*");
    WIN32_FIND_DATAA weaponData{};
    HANDLE hw = FindFirstFileA(weaponMask.c_str(), &weaponData);
    if (hw != INVALID_HANDLE_VALUE) {
        do {
            if (!(weaponData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::string weaponFolder = weaponData.cFileName;
            if (weaponFolder == "." || weaponFolder == ".." || _stricmp(weaponFolder.c_str(), "Nick") == 0)
                continue;
            const std::string weaponLower = ToLowerAscii(weaponFolder);
            const std::string weaponDir = JoinPath(texturesDir, weaponFolder);

            std::string fileMask = JoinPath(weaponDir, "*.*");
            WIN32_FIND_DATAA fileData{};
            HANDLE hf = FindFirstFileA(fileMask.c_str(), &fileData);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::string fname = fileData.cFileName;
                    if (LowerExt(fname) != ".txd") continue;
                    const std::string base = BaseNameNoExt(fname);
                    const std::string skinLower = ToLowerAscii(base);
                    AddWeaponTextureAsset(
                        "skin:" + weaponLower + ":" + skinLower,
                        weaponLower,
                        skinLower,
                        weaponFolder + "/" + base,
                        JoinPath(weaponDir, fname),
                        &g_weaponTextureBySkin,
                        nullptr);
                } while (FindNextFileA(hf, &fileData));
                FindClose(hf);
            }

            std::string skinDirMask = JoinPath(weaponDir, "*");
            WIN32_FIND_DATAA skinDirData{};
            HANDLE hs = FindFirstFileA(skinDirMask.c_str(), &skinDirData);
            if (hs != INVALID_HANDLE_VALUE) {
                do {
                    if (!(skinDirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    const std::string skinFolder = skinDirData.cFileName;
                    if (skinFolder == "." || skinFolder == "..") continue;
                    const std::string skinLower = ToLowerAscii(skinFolder);
                    const std::string skinDir = JoinPath(weaponDir, skinFolder);
                    std::string variantMask = JoinPath(skinDir, "*.*");
                    WIN32_FIND_DATAA variantData{};
                    HANDLE hv = FindFirstFileA(variantMask.c_str(), &variantData);
                    if (hv == INVALID_HANDLE_VALUE) continue;
                    do {
                        if (variantData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        const std::string fname = variantData.cFileName;
                        if (LowerExt(fname) != ".txd") continue;
                        const std::string base = BaseNameNoExt(fname);
                        AddWeaponTextureAsset(
                            "skinrandom:" + weaponLower + ":" + skinLower + ":" + ToLowerAscii(base),
                            weaponLower,
                            skinLower,
                            weaponFolder + "/" + skinFolder + "/" + base,
                            JoinPath(skinDir, fname),
                            nullptr,
                            &g_weaponTextureRandomBySkin);
                    } while (FindNextFileA(hv, &variantData));
                    FindClose(hv);
                } while (FindNextFileA(hs, &skinDirData));
                FindClose(hs);
            }
        } while (FindNextFileA(hw, &weaponData));
        FindClose(hw);
    }

    const std::vector<std::string> knownWeapons = KnownWeaponModelNamesLower();
    const std::string nickDir = JoinPath(texturesDir, "Nick");
    DWORD nickAttr = GetFileAttributesA(nickDir.c_str());
    if (nickAttr != INVALID_FILE_ATTRIBUTES && (nickAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string nickMask = JoinPath(nickDir, "*.*");
        WIN32_FIND_DATAA nickData{};
        HANDLE hn = FindFirstFileA(nickMask.c_str(), &nickData);
        if (hn != INVALID_HANDLE_VALUE) {
            do {
                if (nickData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string fname = nickData.cFileName;
                if (LowerExt(fname) != ".txd") continue;
                const std::string base = BaseNameNoExt(fname);
                const std::string baseLower = ToLowerAscii(base);
                std::string weaponLower;
                std::string nickLower;
                for (const std::string& known : knownWeapons) {
                    const std::string prefix = known + "_";
                    if (baseLower.rfind(prefix, 0) == 0 && baseLower.size() > prefix.size()) {
                        weaponLower = known;
                        nickLower = baseLower.substr(prefix.size());
                        break;
                    }
                }
                if (weaponLower.empty()) {
                    const size_t underscore = baseLower.find('_');
                    if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= baseLower.size())
                        continue;
                    weaponLower = baseLower.substr(0, underscore);
                    nickLower = baseLower.substr(underscore + 1);
                }
                AddWeaponTextureAsset(
                    "nick:" + weaponLower + ":" + nickLower,
                    weaponLower,
                    nickLower,
                    base,
                    JoinPath(nickDir, fname),
                    &g_weaponTextureByNick,
                    nullptr);
            } while (FindNextFileA(hn, &nickData));
            FindClose(hn);
        }
    }

    g_weaponTextureStats.uniqueSkinTextures = (int)g_weaponTextureBySkin.size();
    g_weaponTextureStats.randomSkinTextures = 0;
    for (const auto& kv : g_weaponTextureRandomBySkin)
        g_weaponTextureStats.randomSkinTextures += (int)kv.second.size();
    g_weaponTextureStats.nickTextures = (int)g_weaponTextureByNick.size();
    OrcLogInfo("DiscoverWeaponTextures: skin=%d random=%d nick=%d",
        g_weaponTextureStats.uniqueSkinTextures,
        g_weaponTextureStats.randomSkinTextures,
        g_weaponTextureStats.nickTextures);
}

WeaponTextureStats OrcGetWeaponTextureStats() {
    return g_weaponTextureStats;
}

static WeaponTextureAsset* PickRandomWeaponTextureAsset(const std::string& mapKey) {
    auto it = g_weaponTextureRandomBySkin.find(mapKey);
    if (it == g_weaponTextureRandomBySkin.end() || it->second.empty())
        return nullptr;
    std::vector<int>& bag = g_weaponTextureRandomBags[mapKey];
    if (bag.empty()) {
        bag = it->second;
        for (int i = (int)bag.size() - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(bag[(size_t)i], bag[(size_t)j]);
        }
    }
    const int assetIndex = bag.back();
    bag.pop_back();
    if (assetIndex < 0 || assetIndex >= (int)g_weaponTextureAssets.size())
        return nullptr;
    return &g_weaponTextureAssets[(size_t)assetIndex];
}

static WeaponTextureAsset* PickStickyRandomWeaponTextureAsset(CPed* ped, const std::string& mapKey) {
    if (!ped)
        return nullptr;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return PickRandomWeaponTextureAsset(mapKey);

    const std::string choiceKey = std::to_string(pedRef) + "|" + mapKey;
    auto chosen = g_weaponTextureRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponTextureRandomChoiceByPed.end()) {
        const int assetIndex = chosen->second;
        if (assetIndex >= 0 && assetIndex < (int)g_weaponTextureAssets.size())
            return &g_weaponTextureAssets[(size_t)assetIndex];
    }

    WeaponTextureAsset* asset = PickRandomWeaponTextureAsset(mapKey);
    if (!asset)
        return nullptr;
    const int assetIndex = (int)(asset - g_weaponTextureAssets.data());
    if (assetIndex >= 0)
        g_weaponTextureRandomChoiceByPed[choiceKey] = assetIndex;
    return asset;
}

static WeaponTextureAsset* ResolveWeaponTextureAssetForPed(CPed* ped, int wt, bool allowRandom) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped || wt <= 0)
        return nullptr;
    const std::string weaponLower = GetWeaponModelBaseNameLower(wt);
    if (weaponLower.empty())
        return nullptr;

    if (g_weaponTextureNickMode && samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = ToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                const std::string nickKey = MakeWeaponReplacementKey(weaponLower, nickLower);
                auto nickIt = g_weaponTextureByNick.find(nickKey);
                if (nickIt != g_weaponTextureByNick.end() &&
                    nickIt->second >= 0 && nickIt->second < (int)g_weaponTextureAssets.size()) {
                    return &g_weaponTextureAssets[(size_t)nickIt->second];
                }
            }
        }
    }

    const std::string skinLower = ToLowerAscii(GetPedStdSkinDffName(ped));
    if (skinLower.empty())
        return nullptr;
    const std::string skinKey = MakeWeaponReplacementKey(weaponLower, skinLower);
    auto skinIt = g_weaponTextureBySkin.find(skinKey);
    if (skinIt != g_weaponTextureBySkin.end() &&
        skinIt->second >= 0 && skinIt->second < (int)g_weaponTextureAssets.size()) {
        return &g_weaponTextureAssets[(size_t)skinIt->second];
    }
    if (allowRandom && g_weaponTextureRandomMode)
        return PickStickyRandomWeaponTextureAsset(ped, skinKey);
    return nullptr;
}

static WeaponTextureAsset* ResolveUsableWeaponTextureAssetForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponTextureAsset* asset = ResolveWeaponTextureAssetForPed(ped, wt, allowRandom);
    if (!asset)
        return nullptr;
    if (EnsureWeaponTextureAssetLoaded(*asset))
        return asset;
    if (asset->key.rfind("skinrandom:", 0) == 0) {
        const int failedIndex = (int)(asset - g_weaponTextureAssets.data());
        for (auto it = g_weaponTextureRandomChoiceByPed.begin(); it != g_weaponTextureRandomChoiceByPed.end();) {
            if (it->second == failedIndex)
                it = g_weaponTextureRandomChoiceByPed.erase(it);
            else
                ++it;
        }
    }
    return nullptr;
}

struct WeaponTextureApplyCtx {
    RwTexDictionary* dict = nullptr;
};

static RpMaterial* WeaponTextureApplyMaterialCB(RpMaterial* material, void* data) {
    if (!material || !material->texture || !data)
        return material;
    WeaponTextureApplyCtx* ctx = reinterpret_cast<WeaponTextureApplyCtx*>(data);
    if (!ctx->dict)
        return material;
    RwTexture* replacement = RwTexDictionaryFindNamedTexture(ctx->dict, material->texture->name);
    if (!replacement || replacement == material->texture)
        return material;
    g_weaponTextureRestoreEntries.push_back({ material, material->texture });
    material->texture = replacement;
    return material;
}

static RpAtomic* WeaponTextureApplyAtomicCB(RpAtomic* atomic, void* data) {
    if (!atomic || !atomic->geometry)
        return atomic;
    RpGeometryForAllMaterials(atomic->geometry, WeaponTextureApplyMaterialCB, data);
    return atomic;
}

static void ApplyWeaponTextureToRwObject(RwObject* object, WeaponTextureAsset* asset) {
    if (!object || !asset || !EnsureWeaponTextureAssetLoaded(*asset))
        return;
    WeaponTextureApplyCtx ctx;
    ctx.dict = GetTxdDictionaryByIndexMain(asset->txdSlot);
    if (!ctx.dict)
        return;
    __try {
        if (object->type == rpCLUMP) {
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(object), WeaponTextureApplyAtomicCB, &ctx);
        } else if (object->type == rpATOMIC) {
            WeaponTextureApplyAtomicCB(reinterpret_cast<RpAtomic*>(object), &ctx);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon texture apply: SEH ex=0x%08X asset=%s", GetExceptionCode(), asset->displayName.c_str());
    }
}

static WeaponReplacementAsset* PickRandomWeaponReplacementAsset(const std::string& mapKey) {
    auto it = g_weaponReplacementRandomBySkin.find(mapKey);
    if (it == g_weaponReplacementRandomBySkin.end() || it->second.empty())
        return nullptr;
    std::vector<int>& bag = g_weaponReplacementRandomBags[mapKey];
    if (bag.empty()) {
        bag = it->second;
        for (int i = (int)bag.size() - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(bag[(size_t)i], bag[(size_t)j]);
        }
    }
    const int assetIndex = bag.back();
    bag.pop_back();
    if (assetIndex < 0 || assetIndex >= (int)g_weaponReplacementAssets.size())
        return nullptr;
    return &g_weaponReplacementAssets[(size_t)assetIndex];
}

static WeaponReplacementAsset* PickStickyRandomWeaponReplacementAsset(CPed* ped, const std::string& mapKey) {
    if (!ped)
        return nullptr;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return PickRandomWeaponReplacementAsset(mapKey);

    const std::string choiceKey = std::to_string(pedRef) + "|" + mapKey;
    auto chosen = g_weaponReplacementRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponReplacementRandomChoiceByPed.end()) {
        const int assetIndex = chosen->second;
        if (assetIndex >= 0 && assetIndex < (int)g_weaponReplacementAssets.size())
            return &g_weaponReplacementAssets[(size_t)assetIndex];
    }

    WeaponReplacementAsset* asset = PickRandomWeaponReplacementAsset(mapKey);
    if (!asset)
        return nullptr;
    const int assetIndex = (int)(asset - g_weaponReplacementAssets.data());
    if (assetIndex >= 0)
        g_weaponReplacementRandomChoiceByPed[choiceKey] = assetIndex;
    return asset;
}

static WeaponReplacementAsset* ResolveWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
    if (!g_weaponReplacementEnabled || !ped || wt <= 0)
        return nullptr;
    const std::string weaponLower = GetWeaponModelBaseNameLower(wt);
    if (weaponLower.empty())
        return nullptr;

    if (samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = ToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                const std::string nickKey = MakeWeaponReplacementKey(weaponLower, nickLower);
                auto nickIt = g_weaponReplacementByNick.find(nickKey);
                if (nickIt != g_weaponReplacementByNick.end() &&
                    nickIt->second >= 0 && nickIt->second < (int)g_weaponReplacementAssets.size()) {
                    return &g_weaponReplacementAssets[(size_t)nickIt->second];
                }
            }
        }
    }

    const std::string skinLower = ToLowerAscii(GetPedStdSkinDffName(ped));
    if (skinLower.empty())
        return nullptr;
    const std::string skinKey = MakeWeaponReplacementKey(weaponLower, skinLower);
    auto skinIt = g_weaponReplacementBySkin.find(skinKey);
    if (skinIt != g_weaponReplacementBySkin.end() &&
        skinIt->second >= 0 && skinIt->second < (int)g_weaponReplacementAssets.size()) {
        return &g_weaponReplacementAssets[(size_t)skinIt->second];
    }
    if (allowRandom)
        return PickStickyRandomWeaponReplacementAsset(ped, skinKey);
    return nullptr;
}

static WeaponReplacementAsset* ResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponReplacementAsset* asset = ResolveWeaponReplacementAssetForPed(ped, wt, allowRandom);
    if (!asset)
        return nullptr;
    if (!EnsureWeaponReplacementAssetLoaded(*asset)) {
        if (asset->key.rfind("skinrandom:", 0) == 0) {
            const int failedIndex = (int)(asset - g_weaponReplacementAssets.data());
            for (auto it = g_weaponReplacementRandomChoiceByPed.begin(); it != g_weaponReplacementRandomChoiceByPed.end();) {
                if (it->second == failedIndex)
                    it = g_weaponReplacementRandomChoiceByPed.erase(it);
                else
                    ++it;
            }
        }
        return nullptr;
    }
    return asset;
}

static std::string ResolveUsableWeaponReplacementKeyForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponReplacementAsset* asset = ResolveUsableWeaponReplacementAssetForPed(ped, wt, allowRandom);
    return asset ? asset->key : std::string{};
}

// Queued from UI; applied at the start of drawingEvent (see ApplyPendingLocalPlayerModel).
static int g_pendingLocalPedModelId = -1;

bool OrcApplyLocalPlayerModelById(int modelId) {
    if (!IsValidStandardSkinModel(modelId)) return false;
    g_pendingLocalPedModelId = modelId;
    return true;
}

int OrcGetLocalPlayerModelId() {
    CPlayerPed* ped = FindPlayerPed(0);
    return ped ? (int)ped->m_nModelIndex : -1;
}

struct StandardSkinPreviewRuntime {
    int requestedSource = SKIN_PREVIEW_STANDARD;
    int requestedModelId = -1;
    int requestedVariantIndex = -1;
    std::string requestedName;
    int requestedWidth = 0;
    int requestedHeight = 0;
    float requestedYawDeg = 0.0f;
    bool requestDirty = false;

    int renderedSource = SKIN_PREVIEW_STANDARD;
    int renderedModelId = -1;
    int renderedVariantIndex = -1;
    std::string renderedName;
    int renderedWidth = 0;
    int renderedHeight = 0;
    float renderedYawDeg = 0.0f;

    RwCamera* camera = nullptr;
    RwFrame* cameraFrame = nullptr;
    RwRaster* raster = nullptr;
    RwRaster* zRaster = nullptr;
    RwTexture* texture = nullptr;
    RpLight* light = nullptr;
    RwFrame* lightFrame = nullptr;
    bool lightInWorld = false;
};

static StandardSkinPreviewRuntime g_standardSkinPreview;

static RwV3d NormalizeRwV3d(RwV3d v, const RwV3d& fallback) {
    const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (lenSq <= 0.000001f)
        return fallback;
    const float inv = 1.0f / sqrtf(lenSq);
    v.x *= inv;
    v.y *= inv;
    v.z *= inv;
    return v;
}

static RwV3d CrossRwV3d(const RwV3d& a, const RwV3d& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static void SetPreviewCameraLookAt(RwFrame* frame) {
    if (!frame)
        return;
    const RwV3d pos = { 0.0f, -7.0f, 1.05f };
    const RwV3d target = { 0.0f, 0.0f, 0.86f };
    const RwV3d worldUp = { 0.0f, 0.0f, 1.0f };
    RwV3d at = { target.x - pos.x, target.y - pos.y, target.z - pos.z };
    at = NormalizeRwV3d(at, { 0.0f, 1.0f, 0.0f });
    RwV3d right = NormalizeRwV3d(CrossRwV3d(at, worldUp), { 1.0f, 0.0f, 0.0f });
    RwV3d up = NormalizeRwV3d(CrossRwV3d(right, at), worldUp);

    RwMatrix* m = RwFrameGetMatrix(frame);
    m->right = right;
    m->up = up;
    m->at = at;
    m->pos = pos;
    RwMatrixUpdate(m);
    RwFrameUpdateObjects(frame);
}

static void DestroyStandardSkinPreview() {
    if (g_standardSkinPreview.camera && Scene.m_pWorld)
        RpWorldRemoveCamera(Scene.m_pWorld, g_standardSkinPreview.camera);
    if (g_standardSkinPreview.light && g_standardSkinPreview.lightInWorld && Scene.m_pWorld) {
        RpWorldRemoveLight(Scene.m_pWorld, g_standardSkinPreview.light);
        g_standardSkinPreview.lightInWorld = false;
    }
    if (g_standardSkinPreview.camera) {
        RwCameraSetRaster(g_standardSkinPreview.camera, nullptr);
        RwCameraSetZRaster(g_standardSkinPreview.camera, nullptr);
        RwCameraSetFrame(g_standardSkinPreview.camera, nullptr);
        RwCameraDestroy(g_standardSkinPreview.camera);
    }
    if (g_standardSkinPreview.cameraFrame)
        RwFrameDestroy(g_standardSkinPreview.cameraFrame);
    if (g_standardSkinPreview.light) {
        RpLightSetFrame(g_standardSkinPreview.light, nullptr);
        RpLightDestroy(g_standardSkinPreview.light);
    }
    if (g_standardSkinPreview.lightFrame)
        RwFrameDestroy(g_standardSkinPreview.lightFrame);
    if (g_standardSkinPreview.texture) {
        RwTextureDestroy(g_standardSkinPreview.texture);
    } else if (g_standardSkinPreview.raster) {
        RwRasterDestroy(g_standardSkinPreview.raster);
    }
    if (g_standardSkinPreview.zRaster)
        RwRasterDestroy(g_standardSkinPreview.zRaster);
    g_standardSkinPreview.camera = nullptr;
    g_standardSkinPreview.cameraFrame = nullptr;
    g_standardSkinPreview.raster = nullptr;
    g_standardSkinPreview.zRaster = nullptr;
    g_standardSkinPreview.texture = nullptr;
    g_standardSkinPreview.light = nullptr;
    g_standardSkinPreview.lightFrame = nullptr;
    g_standardSkinPreview.lightInWorld = false;
    g_standardSkinPreview.renderedSource = SKIN_PREVIEW_STANDARD;
    g_standardSkinPreview.renderedModelId = -1;
    g_standardSkinPreview.renderedVariantIndex = -1;
    g_standardSkinPreview.renderedName.clear();
    g_standardSkinPreview.renderedWidth = 0;
    g_standardSkinPreview.renderedHeight = 0;
}

static bool EnsureStandardSkinPreviewResources(int width, int height) {
    width = std::max(64, std::min(width, 1024));
    height = std::max(64, std::min(height, 1024));
    if (g_standardSkinPreview.camera &&
        g_standardSkinPreview.raster &&
        g_standardSkinPreview.zRaster &&
        g_standardSkinPreview.texture &&
        g_standardSkinPreview.renderedWidth == width &&
        g_standardSkinPreview.renderedHeight == height) {
        return true;
    }

    DestroyStandardSkinPreview();

    g_standardSkinPreview.raster = RwRasterCreate(width, height, 0, rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
    g_standardSkinPreview.zRaster = RwRasterCreate(width, height, 0, rwRASTERTYPEZBUFFER);
    if (!g_standardSkinPreview.raster || !g_standardSkinPreview.zRaster)
        return false;
    g_standardSkinPreview.texture = RwTextureCreate(g_standardSkinPreview.raster);
    if (!g_standardSkinPreview.texture)
        return false;
    RwTextureSetFilterMode(g_standardSkinPreview.texture, rwFILTERLINEAR);

    g_standardSkinPreview.camera = RwCameraCreate();
    g_standardSkinPreview.cameraFrame = RwFrameCreate();
    if (!g_standardSkinPreview.camera || !g_standardSkinPreview.cameraFrame)
        return false;
    RwCameraSetFrame(g_standardSkinPreview.camera, g_standardSkinPreview.cameraFrame);
    SetPreviewCameraLookAt(g_standardSkinPreview.cameraFrame);
    RwCameraSetRaster(g_standardSkinPreview.camera, g_standardSkinPreview.raster);
    RwCameraSetZRaster(g_standardSkinPreview.camera, g_standardSkinPreview.zRaster);
    RwCameraSetNearClipPlane(g_standardSkinPreview.camera, 0.01f);
    RwCameraSetFarClipPlane(g_standardSkinPreview.camera, 300.0f);
    const float aspect = height > 0 ? (float)height / (float)width : 1.0f;
    RwV2d viewWindow = { 0.42f, 0.42f * aspect };
    RwCameraSetViewWindow(g_standardSkinPreview.camera, &viewWindow);
    RwCameraSetProjection(g_standardSkinPreview.camera, rwPERSPECTIVE);
    if (Scene.m_pWorld)
        RpWorldAddCamera(Scene.m_pWorld, g_standardSkinPreview.camera);

    g_standardSkinPreview.light = RpLightCreate(rpLIGHTDIRECTIONAL);
    if (g_standardSkinPreview.light) {
        g_standardSkinPreview.lightFrame = RwFrameCreate();
        if (g_standardSkinPreview.lightFrame) {
            const RwV3d worldUp = { 0.0f, 0.0f, 1.0f };
            RwV3d at = NormalizeRwV3d({ 0.0f, 1.0f, -0.35f }, { 0.0f, 1.0f, 0.0f });
            RwV3d right = NormalizeRwV3d(CrossRwV3d(at, worldUp), { 1.0f, 0.0f, 0.0f });
            RwV3d up = NormalizeRwV3d(CrossRwV3d(right, at), worldUp);
            RwMatrix* m = RwFrameGetMatrix(g_standardSkinPreview.lightFrame);
            m->right = right;
            m->up = up;
            m->at = at;
            m->pos = { 0.0f, -4.0f, 3.0f };
            RwMatrixUpdate(m);
            RwFrameUpdateObjects(g_standardSkinPreview.lightFrame);
            RpLightSetFrame(g_standardSkinPreview.light, g_standardSkinPreview.lightFrame);
        } else {
            RpLightDestroy(g_standardSkinPreview.light);
            g_standardSkinPreview.light = nullptr;
        }
    }
    if (g_standardSkinPreview.light) {
        RwRGBAReal color{ 1.0f, 1.0f, 1.0f, 1.0f };
        RpLightSetColor(g_standardSkinPreview.light, &color);
    }
    g_standardSkinPreview.renderedWidth = width;
    g_standardSkinPreview.renderedHeight = height;
    return g_standardSkinPreview.camera && g_standardSkinPreview.texture;
}

static CustomSkinCfg* FindCustomSkinForPreview(const std::string& name) {
    if (name.empty())
        return nullptr;
    const std::string want = ToLowerAscii(name);
    for (auto& skin : g_customSkins) {
        if (ToLowerAscii(skin.name) == want)
            return &skin;
    }
    return nullptr;
}

static CustomSkinCfg* FindRandomSkinForPreview(int modelId, int variantIndex) {
    SkinRandomPool* pool = FindRandomPoolForModelId(modelId);
    if (!pool || variantIndex < 0 || variantIndex >= (int)pool->variants.size())
        return nullptr;
    return &pool->variants[(size_t)variantIndex];
}

static int GetPreviewBasePedModelId(int source, int requestedModelId) {
    if (source == SKIN_PREVIEW_STANDARD || source == SKIN_PREVIEW_RANDOM) {
        if (IsValidStandardSkinModel(requestedModelId))
            return requestedModelId;
    }
    const int localModelId = OrcGetLocalPlayerModelId();
    if (IsValidStandardSkinModel(localModelId))
        return localModelId;
    if (IsValidStandardSkinModel(g_standardSkinSelectedModelId))
        return g_standardSkinSelectedModelId;
    std::vector<std::pair<std::string, int>> skins;
    OrcCollectPedSkins(skins);
    if (!skins.empty())
        return skins.front().second;
    return -1;
}

static void RenderStandardSkinPreviewRequest() {
    if (!g_standardSkinPreview.requestDirty)
        return;
    g_standardSkinPreview.requestDirty = false;
    const int source = g_standardSkinPreview.requestedSource;
    const int requestedModelId = g_standardSkinPreview.requestedModelId;
    CustomSkinCfg* overlaySkin = nullptr;
    if (source == SKIN_PREVIEW_CUSTOM)
        overlaySkin = FindCustomSkinForPreview(g_standardSkinPreview.requestedName);
    else if (source == SKIN_PREVIEW_RANDOM)
        overlaySkin = FindRandomSkinForPreview(requestedModelId, g_standardSkinPreview.requestedVariantIndex);

    const int modelId = GetPreviewBasePedModelId(source, requestedModelId);
    const int width = std::max(64, std::min(g_standardSkinPreview.requestedWidth, 1024));
    const int height = std::max(64, std::min(g_standardSkinPreview.requestedHeight, 1024));
    if (!IsValidStandardSkinModel(modelId) ||
        ((source == SKIN_PREVIEW_CUSTOM || source == SKIN_PREVIEW_RANDOM) && !overlaySkin) ||
        !EnsureStandardSkinPreviewResources(width, height)) {
        g_standardSkinPreview.renderedModelId = -1;
        return;
    }
    static int s_previewLogLeft = 12;
    if (s_previewLogLeft > 0) {
        OrcLogInfo("SkinPreview: render request source=%d model=%d variant=%d name=%s size=%dx%d",
                   source,
                   modelId,
                   g_standardSkinPreview.requestedVariantIndex,
                   g_standardSkinPreview.requestedName.c_str(),
                   width,
                   height);
        --s_previewLogLeft;
    }

    const bool wasLoaded = CStreaming::HasModelLoaded(modelId) != 0;
    if (!wasLoaded) {
        CStreaming::RequestModel(modelId, 0);
        for (int i = 0; i < 32 && !CStreaming::HasModelLoaded(modelId); ++i)
            CStreaming::LoadAllRequestedModels(false);
        if (!CStreaming::HasModelLoaded(modelId)) {
            g_standardSkinPreview.renderedModelId = -1;
            return;
        }
    }

    std::uint32_t handle = 0;
    Command<Commands::CREATE_CHAR>(0, modelId, 0.0f, 0.0f, 0.0f, &handle);
    CPed* previewPed = CPools::GetPed(handle);
    if (!previewPed || !previewPed->m_pRwClump) {
        if (handle)
            Command<Commands::DELETE_CHAR>(handle);
        if (!wasLoaded)
            CStreaming::RemoveModel(modelId);
        g_standardSkinPreview.renderedModelId = -1;
        return;
    }

    previewPed->SetIdle();
    previewPed->Teleport({ 0.0f, 0.0f, 0.0f }, false);
    previewPed->bCollisionProcessed = false;
    previewPed->bUsesCollision = false;
    previewPed->SetHeading(g_standardSkinPreview.requestedYawDeg * D2R);
    previewPed->UpdateRwFrame();
    RpAnimBlendClumpUpdateAnimations(previewPed->m_pRwClump, 100.0f, true);

    CVisibilityPlugins::SetRenderWareCamera(g_standardSkinPreview.camera);
    RwRGBA clearColor = { 18, 20, 24, 255 };

    bool rendered = false;
    RwCameraClear(g_standardSkinPreview.camera, &clearColor, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
    if (RwCameraBeginUpdate(g_standardSkinPreview.camera)) {
        if (g_standardSkinPreview.light && g_standardSkinPreview.lightFrame && Scene.m_pWorld) {
            RpWorldAddLight(Scene.m_pWorld, g_standardSkinPreview.light);
            g_standardSkinPreview.lightInWorld = true;
        }

        int oldCull = 0, oldZT = 0, oldZW = 0, oldShade = 0, oldFog = 0;
        RwRenderStateGet(rwRENDERSTATECULLMODE,     &oldCull);
        RwRenderStateGet(rwRENDERSTATEZTESTENABLE,  &oldZT);
        RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &oldZW);
        RwRenderStateGet(rwRENDERSTATESHADEMODE,    &oldShade);
        RwRenderStateGet(rwRENDERSTATEFOGENABLE,    &oldFog);
        int v = rwCULLMODECULLBACK; RwRenderStateSet(rwRENDERSTATECULLMODE, reinterpret_cast<void*>(v));
        v = TRUE; RwRenderStateSet(rwRENDERSTATEZTESTENABLE, reinterpret_cast<void*>(v));
        v = TRUE; RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(v));
        v = rwSHADEMODEGOURAUD; RwRenderStateSet(rwRENDERSTATESHADEMODE, reinterpret_cast<void*>(v));
        v = FALSE; RwRenderStateSet(rwRENDERSTATEFOGENABLE, reinterpret_cast<void*>(v));

        __try {
            previewPed->Add();
            previewPed->PreRender();
            bool overlayRendered = false;
            if (source == SKIN_PREVIEW_STANDARD)
                CVisibilityPlugins::RenderEntity(previewPed, false, 999.0f);
            else if (EnsureCustomSkinLoaded(*overlaySkin)) {
                RenderSkinOnPed(previewPed, overlaySkin, false);
                overlayRendered = true;
            }
            previewPed->Remove();
            rendered = (source == SKIN_PREVIEW_STANDARD) || overlayRendered;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("SkinPreview render: SEH ex=0x%08X source=%d model=%d", GetExceptionCode(), source, modelId);
            __try { previewPed->Remove(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            rendered = false;
        }

        RwRenderStateSet(rwRENDERSTATECULLMODE,     reinterpret_cast<void*>(oldCull));
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE,  reinterpret_cast<void*>(oldZT));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(oldZW));
        RwRenderStateSet(rwRENDERSTATESHADEMODE,    reinterpret_cast<void*>(oldShade));
        RwRenderStateSet(rwRENDERSTATEFOGENABLE,    reinterpret_cast<void*>(oldFog));

        if (g_standardSkinPreview.light && g_standardSkinPreview.lightInWorld && Scene.m_pWorld) {
            RpWorldRemoveLight(Scene.m_pWorld, g_standardSkinPreview.light);
            g_standardSkinPreview.lightInWorld = false;
        }
        __try {
            RwCameraEndUpdate(g_standardSkinPreview.camera);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("StandardSkinPreview CameraEndUpdate: SEH ex=0x%08X model=%d", GetExceptionCode(), modelId);
            DestroyStandardSkinPreview();
            rendered = false;
        }
    }

    if (Scene.m_pCamera)
        CVisibilityPlugins::SetRenderWareCamera(Scene.m_pCamera);
    Command<Commands::DELETE_CHAR>(handle);
    if (!wasLoaded)
        CStreaming::RemoveModel(modelId);

    g_standardSkinPreview.renderedSource = source;
    g_standardSkinPreview.renderedModelId = rendered ? modelId : -1;
    g_standardSkinPreview.renderedVariantIndex = rendered ? g_standardSkinPreview.requestedVariantIndex : -1;
    g_standardSkinPreview.renderedName = rendered ? g_standardSkinPreview.requestedName : std::string{};
    g_standardSkinPreview.renderedYawDeg = g_standardSkinPreview.requestedYawDeg;
}

void OrcRequestSkinPreview(int source, int modelId, int variantIndex, const char* name, int width, int height, float yawDeg) {
    if (source < SKIN_PREVIEW_STANDARD || source > SKIN_PREVIEW_RANDOM)
        source = SKIN_PREVIEW_STANDARD;
    const std::string requestedName = name ? name : "";
    width = std::max(64, std::min(width, 1024));
    height = std::max(64, std::min(height, 1024));
    const int expectedRenderedModelId = GetPreviewBasePedModelId(source, modelId);
    if (g_standardSkinPreview.requestedSource == source &&
        g_standardSkinPreview.requestedModelId == modelId &&
        g_standardSkinPreview.requestedVariantIndex == variantIndex &&
        g_standardSkinPreview.requestedName == requestedName &&
        g_standardSkinPreview.requestedWidth == width &&
        g_standardSkinPreview.requestedHeight == height &&
        fabsf(g_standardSkinPreview.requestedYawDeg - yawDeg) < 0.01f &&
        g_standardSkinPreview.renderedSource == source &&
        g_standardSkinPreview.renderedVariantIndex == variantIndex &&
        g_standardSkinPreview.renderedName == requestedName &&
        g_standardSkinPreview.renderedModelId == expectedRenderedModelId) {
        return;
    }
    g_standardSkinPreview.requestedSource = source;
    g_standardSkinPreview.requestedModelId = modelId;
    g_standardSkinPreview.requestedVariantIndex = variantIndex;
    g_standardSkinPreview.requestedName = requestedName;
    g_standardSkinPreview.requestedWidth = width;
    g_standardSkinPreview.requestedHeight = height;
    g_standardSkinPreview.requestedYawDeg = yawDeg;
    g_standardSkinPreview.requestDirty = true;
    if (g_standardSkinPreview.renderedSource != source ||
        g_standardSkinPreview.renderedModelId != expectedRenderedModelId ||
        g_standardSkinPreview.renderedVariantIndex != variantIndex ||
        g_standardSkinPreview.renderedName != requestedName ||
        g_standardSkinPreview.renderedWidth != width ||
        g_standardSkinPreview.renderedHeight != height) {
        g_standardSkinPreview.renderedModelId = -1;
    }
}

void* OrcGetSkinPreviewTexture() {
    if (g_standardSkinPreview.renderedModelId < 0 ||
        !g_standardSkinPreview.texture ||
        !g_standardSkinPreview.texture->raster) {
        return nullptr;
    }
    return *reinterpret_cast<void**>(g_standardSkinPreview.texture->raster + 1);
}

static void OnStandardSkinPreviewRenderEvent() {
    __try {
        RenderStandardSkinPreviewRequest();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("StandardSkinPreview before-main-render: SEH ex=0x%08X", GetExceptionCode());
        if (Scene.m_pCamera)
            CVisibilityPlugins::SetRenderWareCamera(Scene.m_pCamera);
        DestroyStandardSkinPreview();
        g_standardSkinPreview.renderedModelId = -1;
        g_standardSkinPreview.requestDirty = false;
    }
}

static void AppendFormat(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    out += buf;
}

static void AppendMainIniText(std::string& out) {
    out += "; OrcOutFit configuration.\n\n";
    out += "[Main]\n";
    AppendFormat(out, "Enabled=%d\n", g_enabled ? 1 : 0);
    AppendFormat(out, "Language=%s\n", OrcLanguageId(g_orcUiLanguage));
    AppendFormat(out, "ActivationKey=%s\n", VkToString(g_activationVk));
    AppendFormat(out, "SampAllowActivationKey=%d\n", g_sampAllowActivationKey ? 1 : 0);
    AppendFormat(out, "Command=%s\n", g_toggleCommand.c_str());
    AppendFormat(out, "UiAutoScale=%d\n", g_uiAutoScale ? 1 : 0);
    AppendFormat(out, "UiScale=%.2f\n", g_uiScale);
    AppendFormat(out, "UiFontSize=%.0f\n\n", g_uiFontSize);

    out += "[Features]\n";
    AppendFormat(out, "RenderAllPedsWeapons=%d\n", g_renderAllPedsWeapons ? 1 : 0);
    AppendFormat(out, "RenderAllPedsObjects=%d\n", g_renderAllPedsObjects ? 1 : 0);
    AppendFormat(out, "RenderAllPedsRadius=%.0f\n", g_renderAllPedsRadius);
    AppendFormat(out, "ConsiderWeaponSkills=%d\n", g_considerWeaponSkills ? 1 : 0);
    AppendFormat(out, "CustomObjects=%d\n", g_renderCustomObjects ? 1 : 0);
    AppendFormat(out, "StandardObjects=%d\n", g_renderStandardObjects ? 1 : 0);
    AppendFormat(out, "WeaponReplacement=%d\n", g_weaponReplacementEnabled ? 1 : 0);
    AppendFormat(out, "WeaponReplacementOnBody=%d\n", g_weaponReplacementOnBody ? 1 : 0);
    AppendFormat(out, "WeaponReplacementInHands=%d\n", g_weaponReplacementInHands ? 1 : 0);
    AppendFormat(out, "WeaponTextures=%d\n", g_weaponTexturesEnabled ? 1 : 0);
    AppendFormat(out, "WeaponTextureNickMode=%d\n", g_weaponTextureNickMode ? 1 : 0);
    AppendFormat(out, "WeaponTextureRandomMode=%d\n", g_weaponTextureRandomMode ? 1 : 0);
    AppendFormat(out, "SkinMode=%d\n", g_skinModeEnabled ? 1 : 0);
    AppendFormat(out, "SkinHideBasePed=%d\n", g_skinHideBasePed ? 1 : 0);
    AppendFormat(out, "SkinNickMode=%d\n", g_skinNickMode ? 1 : 0);
    AppendFormat(out, "SkinLocalPreferSelected=%d\n", g_skinLocalPreferSelected ? 1 : 0);
    AppendFormat(out, "SkinTextureRemap=%d\n", g_skinTextureRemapEnabled ? 1 : 0);
    AppendFormat(out, "SkinTextureRemapNickMode=%d\n", g_skinTextureRemapNickMode ? 1 : 0);
    AppendFormat(out, "SkinTextureRemapAutoNickMode=%d\n", g_skinTextureRemapAutoNickMode ? 1 : 0);
    AppendFormat(out, "SkinTextureRemapRandomMode=%d\n", g_skinTextureRemapRandomMode);
    AppendFormat(out, "DebugLogLevel=%d\n", static_cast<int>(g_orcLogLevel));
    AppendFormat(out, "DebugLog=%d\n\n", (g_orcLogLevel >= OrcLogLevel::Info) ? 1 : 0);

    out += "[SkinMode]\n";
    AppendFormat(out, "Selected=%s\n", g_skinSelectedName.c_str());
    AppendFormat(out, "SelectedSource=%s\n", g_skinSelectedSource == SKIN_SELECTED_STANDARD ? "standard" : "custom");
    AppendFormat(out, "StandardSelected=%d\n", g_standardSkinSelectedModelId);
    AppendFormat(out, "RandomFromPools=%d\n\n", g_skinRandomFromPools ? 1 : 0);
}

static void AppendWeaponSectionText(std::string& out, const char* section, const WeaponCfg& c) {
    out += "[";
    out += section ? section : "";
    out += "]\n";
    AppendFormat(out,
        "Enabled=%d\n"
        "Bone=%d\n"
        "OffsetX=%.3f\n"
        "OffsetY=%.3f\n"
        "OffsetZ=%.3f\n"
        "RotationX=%.2f\n"
        "RotationY=%.2f\n"
        "RotationZ=%.2f\n"
        "Scale=%.3f\n\n",
        c.enabled ? 1 : 0,
        c.boneId,
        c.x, c.y, c.z,
        c.rx / D2R, c.ry / D2R, c.rz / D2R,
        c.scale);
}

void SaveAllWeaponsToIniFile(const char* iniPath, const std::vector<WeaponCfg>& w1, const std::vector<WeaponCfg>& w2) {
    if (!iniPath || !iniPath[0]) return;
    std::string text;
    text.reserve(8192 + (w1.size() + w2.size()) * 256);
    if (_stricmp(iniPath, g_iniPath) == 0)
        AppendMainIniText(text);
    else
        text += "; OrcOutFit weapon preset (same section layout as OrcOutFit.ini).\n\n";

    for (int wt = 1; wt < (int)w1.size() && wt < (int)w2.size(); wt++) {
        const WeaponCfg& c = w1[wt];
        char sec[96];
        if (c.name && c.name[0]) _snprintf_s(sec, _TRUNCATE, "%s", c.name);
        else _snprintf_s(sec, _TRUNCATE, "Weapon%d", wt);
        char secNum[32];
        _snprintf_s(secNum, _TRUNCATE, "Weapon%d", wt);
        AppendWeaponSectionText(text, sec, c);
        if (lstrcmpiA(sec, secNum) != 0)
            AppendWeaponSectionText(text, secNum, c);

        const WeaponCfg& c2 = w2[wt];
        char sec2[96];
        if (w1[wt].name && w1[wt].name[0]) _snprintf_s(sec2, _TRUNCATE, "%s2", w1[wt].name);
        else _snprintf_s(sec2, _TRUNCATE, "Weapon%d_2", wt);
        char secNum2[32];
        _snprintf_s(secNum2, _TRUNCATE, "Weapon%d_2", wt);
        AppendWeaponSectionText(text, sec2, c2);
        if (lstrcmpiA(sec2, secNum2) != 0)
            AppendWeaponSectionText(text, secNum2, c2);
    }
    if (!OrcWriteTextFileAtomic(iniPath, text))
        OrcLogError("SaveAllWeaponsToIniFile: cannot write %s", iniPath);
}

// ----------------------------------------------------------------------------
// Save single weapon section
// ----------------------------------------------------------------------------
void SaveWeaponSection(int wt) {
    if (wt <= 0 || wt >= (int)g_cfg.size()) return;
    auto& c = g_cfg[wt];
    char sec[96];
    if (c.name) _snprintf_s(sec, _TRUNCATE, "%s", c.name);
    else        _snprintf_s(sec, _TRUNCATE, "Weapon%d", wt);
    char secNum[32];
    _snprintf_s(secNum, _TRUNCATE, "Weapon%d", wt);

    std::vector<OrcIniValue> values;
    auto AddSection = [&](const char* section) {
        AddIniInt(values, section, "Enabled", c.enabled ? 1 : 0);
        AddIniInt(values, section, "Bone", c.boneId);
        AddIniFloat(values, section, "OffsetX", c.x, "%.3f");
        AddIniFloat(values, section, "OffsetY", c.y, "%.3f");
        AddIniFloat(values, section, "OffsetZ", c.z, "%.3f");
        AddIniFloat(values, section, "RotationX", c.rx / D2R, "%.2f");
        AddIniFloat(values, section, "RotationY", c.ry / D2R, "%.2f");
        AddIniFloat(values, section, "RotationZ", c.rz / D2R, "%.2f");
        AddIniFloat(values, section, "Scale", c.scale, "%.3f");
    };
    AddSection(sec);
    if (lstrcmpiA(sec, secNum) != 0)
        AddSection(secNum);
    if (!OrcIniWriteValues(g_iniPath, "; OrcOutFit configuration.\n\n", values))
        OrcLogError("SaveWeaponSection: cannot write %s", g_iniPath);
}

void SaveWeaponSection2(int wt) {
    if (wt <= 0 || wt >= (int)g_cfg2.size()) return;
    auto& c = g_cfg2[wt];
    char sec[96];
    if (g_cfg[wt].name) _snprintf_s(sec, _TRUNCATE, "%s2", g_cfg[wt].name);
    else               _snprintf_s(sec, _TRUNCATE, "Weapon%d_2", wt);
    char secNum[32];
    _snprintf_s(secNum, _TRUNCATE, "Weapon%d_2", wt);

    std::vector<OrcIniValue> values;
    auto AddSection = [&](const char* section) {
        AddIniInt(values, section, "Enabled", c.enabled ? 1 : 0);
        AddIniInt(values, section, "Bone", c.boneId);
        AddIniFloat(values, section, "OffsetX", c.x, "%.3f");
        AddIniFloat(values, section, "OffsetY", c.y, "%.3f");
        AddIniFloat(values, section, "OffsetZ", c.z, "%.3f");
        AddIniFloat(values, section, "RotationX", c.rx / D2R, "%.2f");
        AddIniFloat(values, section, "RotationY", c.ry / D2R, "%.2f");
        AddIniFloat(values, section, "RotationZ", c.rz / D2R, "%.2f");
        AddIniFloat(values, section, "Scale", c.scale, "%.3f");
    };
    AddSection(sec);
    if (lstrcmpiA(sec, secNum) != 0)
        AddSection(secNum);
    if (!OrcIniWriteValues(g_iniPath, "; OrcOutFit configuration.\n\n", values))
        OrcLogError("SaveWeaponSection2: cannot write %s", g_iniPath);
}

// ----------------------------------------------------------------------------
// Render state
// ----------------------------------------------------------------------------
struct RenderedWeapon {
    bool      active;
    int       weaponType;
    bool      secondary;
    int       modelId;
    int       slot;
    RwObject* rwObject;
    std::string replacementKey;
};
static constexpr int kMax = 20;
static RenderedWeapon g_rendered[kMax] = {};
using PedWeaponCache = std::array<RenderedWeapon, kMax>;
static std::unordered_map<int, PedWeaponCache> g_otherPedsRendered;

static int FindSlotByType(RenderedWeapon* arr, int wt, bool secondary) {
    for (int i = 0; i < kMax; i++)
        if (arr[i].active && arr[i].weaponType == wt && arr[i].secondary == secondary) return i;
    return -1;
}
static int FindFree(RenderedWeapon* arr) {
    for (int i = 0; i < kMax; i++) if (!arr[i].active) return i;
    return -1;
}

static void DestroyRendered(RenderedWeapon& r) {
    if (!r.rwObject) {
        r = {};
        return;
    }
    DestroyRwObjectInstance(r.rwObject);
    r = {};
}

static void ClearAll() {
    for (int i = 0; i < kMax; i++) DestroyRendered(g_rendered[i]);
}

struct RenderedStandardObject {
    int modelId = -1;
    int slot = 1;
    CPed* ped = nullptr;
    RwObject* rwObject = nullptr;
    unsigned int lastSeenFrame = 0;
};

struct StandardObjectRuntimeKey {
    CPed* ped = nullptr;
    int modelId = -1;
    int slot = 1;

    bool operator==(const StandardObjectRuntimeKey& other) const {
        return ped == other.ped && modelId == other.modelId && slot == other.slot;
    }
};

struct StandardObjectRuntimeKeyHash {
    size_t operator()(const StandardObjectRuntimeKey& key) const {
        size_t h = reinterpret_cast<size_t>(key.ped) >> 4;
        h ^= static_cast<size_t>(key.modelId) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(key.slot) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<StandardObjectRuntimeKey, RenderedStandardObject, StandardObjectRuntimeKeyHash> g_renderedStandardObjects;
static std::unordered_map<int, DWORD> g_standardObjectLastRequestTick;
static unsigned int g_standardObjectRenderFrame = 0;

static void DestroyRenderedStandardObject(RenderedStandardObject& r) {
    if (!r.rwObject) {
        r = {};
        return;
    }
    if (r.rwObject->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(r.rwObject));
    } else if (r.rwObject->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(r.rwObject);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    r = {};
}

static void DestroyAllStandardObjectInstances() {
    for (auto& kv : g_renderedStandardObjects)
        DestroyRenderedStandardObject(kv.second);
    g_renderedStandardObjects.clear();
}

static void DestroyStandardObjectInstancesForSlot(int modelId, int slot) {
    for (auto it = g_renderedStandardObjects.begin(); it != g_renderedStandardObjects.end();) {
        if (it->second.modelId == modelId && it->second.slot == slot) {
            DestroyRenderedStandardObject(it->second);
            it = g_renderedStandardObjects.erase(it);
        } else {
            ++it;
        }
    }
}

static void ApplyPendingLocalPlayerModel() {
    if (g_pendingLocalPedModelId < 0) return;
    const int modelId = g_pendingLocalPedModelId;
    g_pendingLocalPedModelId = -1;

    CPlayerPed* p = FindPlayerPed(0);
    if (!p) {
        OrcLogError("ApplyPendingLocalPlayerModel: no local player ped");
        return;
    }

    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    if (!mi || mi->GetModelType() != MODEL_INFO_PED) {
        OrcLogError("ApplyPendingLocalPlayerModel: model %d is missing or is not MODEL_INFO_PED", modelId);
        return;
    }

    ClearAll();

    CStreaming::RequestModel(modelId, 0);
    for (int i = 0; i < 64 && !CStreaming::HasModelLoaded(modelId); i++)
        CStreaming::LoadAllRequestedModels(false);
    if (!CStreaming::HasModelLoaded(modelId)) {
        OrcLogError("ApplyPendingLocalPlayerModel: model %d did not load in time", modelId);
        return;
    }

    using Fn = void(__thiscall*)(CPed*, int);
    Fn f = reinterpret_cast<Fn>(0x5E4880);
    f(p, modelId);
    InvalidatePerSkinWeaponCache();
    InvalidateObjectSkinParamCache();
    InvalidateStandardObjectSkinParamCache();
    DestroyAllStandardObjectInstances();
    OrcLogInfo("ApplyPendingLocalPlayerModel: set local ped model id=%d", modelId);
}

static void ClearAllOtherPeds() {
    for (auto& kv : g_otherPedsRendered) {
        for (int i = 0; i < kMax; i++) DestroyRendered(kv.second[i]);
    }
    g_otherPedsRendered.clear();
}

// ----------------------------------------------------------------------------
// Bone matrix (через RpHAnim)
// ----------------------------------------------------------------------------
static RwMatrix* GetBoneMatrix(CPed* ped, int boneNodeId) {
    if (!ped || !ped->m_pRwClump) return nullptr;
    // drawingEvent срабатывает уже после CPed::Render, значит RpHAnim обновлён.
    RpHAnimHierarchy* h = GetAnimHierarchyFromSkinClump(ped->m_pRwClump);
    if (!h) return nullptr;
    RwInt32 id = RpHAnimIDGetIndex(h, boneNodeId);
    if (id < 0) return nullptr;
    return &h->pMatrixArray[id];
}

// Освещение как у педа/прикреплённых объектов: ambient по времени суток + directional,
// затем вклад точечных источников. Третий аргумент GenerateLightsAffectingObject — CEntity*
// (см. plugin_sa/game_sa/CPointLights.h), иначе движок не привязывает свет к педу.
// colourScale: оружие/объекты — 0.5 (меньше пересвет); кастомный скин — 1.0 (ближе к штатному педу).
static void ApplyAttachmentLightingForPed(CPed* ped, const CVector& sampleWorldPos, float colourScale = 0.5f) {
    if (!ped) return;
    ActivateDirectional();
    SetAmbientColours();
    float totalLighting = 0.0f;
    const float mult = CPointLights::GenerateLightsAffectingObject(&sampleWorldPos, &totalLighting, ped);
    (void)totalLighting;
    float v = mult * colourScale;
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    SetLightColoursForPedsCarsAndObjects(v);
}

static bool OrcTryPedSetupLighting(CPed* ped) {
    if (!ped) return false;
    __try {
        return ped->SetupLighting();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SEH CPed::SetupLighting ex=0x%08X ped=%p", GetExceptionCode(), ped);
        return false;
    }
}

static void OrcTryPedRemoveLighting(CPed* ped) {
    if (!ped) return;
    __try {
        ped->RemoveLighting();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SEH CPed::RemoveLighting ex=0x%08X ped=%p", GetExceptionCode(), ped);
    }
}

static void OrcTryRpClumpRender(RpClump* clump) {
    if (!clump) return;
    __try {
        RpClumpRender(clump);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SEH RpClumpRender ex=0x%08X clump=%p", GetExceptionCode(), clump);
    }
}

// ----------------------------------------------------------------------------
// Render callbacks (из BaseModelRender::ClumpsForAtomic / GeometryForMaterials)
// ----------------------------------------------------------------------------
// Whitens material color — в паре с rpGEOMETRYMODULATEMATERIALCOLOR даёт
// чистое lighting * 1.0 без родного тинта оружия.
static RpMaterial* AddRefMatCB(RpMaterial* m, void*) {
    if (!m) return m;
    m->refCount++;
    return m;
}

static RpMaterial* WhiteMatCB(RpMaterial* m, void*) {
    if (!m) return m;
    m->color = { 255, 255, 255, 255 };
    return m;
}

// Per-frame prep: форсим modulate-флаг и белый цвет материалов перед рендером.
static RpAtomic* PrepAtomicCB(RpAtomic* a, void*) {
    if (!a) return a;
    if (a->geometry) {
        a->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
    }
    return a;
}

// One-shot init после CreateInstance:
//  1) сбрасываем render callback на штатный RW (иначе SA-рендер пробует тащить
//     оружие через свои пайплайны для держащей руки);
//  2) AddRef материалов — чтобы при teardown движка плагин-деструкторы
//     (CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB) не упали на уже
//     невалидных плагин-данных. Утечка безопасна — процесс всё равно выходит.
static RpAtomic* InitAtomicCB(RpAtomic* a, void*) {
    if (!a) return a;
    CVisibilityPlugins::SetAtomicRenderCallback(a, nullptr);
    if (a->geometry) {
        RpGeometryForAllMaterials(a->geometry, AddRefMatCB, nullptr);
    }
    return a;
}

static RpAtomic* InitAttachmentAtomicCB(RpAtomic* a, void*) {
    InitAtomicCB(a, nullptr);
    if (a && a->geometry) {
        a->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        RpGeometryForAllMaterials(a->geometry, WhiteMatCB, nullptr);
    }
    return a;
}

// ----------------------------------------------------------------------------
// Apply matrix
// ----------------------------------------------------------------------------
static void ApplyOffset(RwMatrix* m, float ox, float oy, float oz) {
    m->pos.x += m->right.x * ox + m->up.x * oy + m->at.x * oz;
    m->pos.y += m->right.y * ox + m->up.y * oy + m->at.y * oz;
    m->pos.z += m->right.z * ox + m->up.z * oy + m->at.z * oz;
}

// M = M * R  (локально в bone space), R = Rx * Ry * Rz (ZYX Euler).
static void RotateMatrix(RwMatrix* m, float rx, float ry, float rz) {
    if (rx == 0 && ry == 0 && rz == 0) return;
    float cx = cosf(rx), sx = sinf(rx);
    float cy = cosf(ry), sy = sinf(ry);
    float cz = cosf(rz), sz = sinf(rz);
    // Row-major rotation; columns = rotated basis in source frame.
    float r00 =  cy*cz,            r01 = -cy*sz,           r02 =  sy;
    float r10 =  sx*sy*cz + cx*sz, r11 = -sx*sy*sz + cx*cz, r12 = -sx*cy;
    float r20 = -cx*sy*cz + sx*sz, r21 =  cx*sy*sz + sx*cz, r22 =  cx*cy;

    RwV3d rg = m->right, up = m->up, at = m->at;
    m->right.x = rg.x*r00 + up.x*r10 + at.x*r20;
    m->right.y = rg.y*r00 + up.y*r10 + at.y*r20;
    m->right.z = rg.z*r00 + up.z*r10 + at.z*r20;
    m->up.x    = rg.x*r01 + up.x*r11 + at.x*r21;
    m->up.y    = rg.y*r01 + up.y*r11 + at.y*r21;
    m->up.z    = rg.z*r01 + up.z*r11 + at.z*r21;
    m->at.x    = rg.x*r02 + up.x*r12 + at.x*r22;
    m->at.y    = rg.y*r02 + up.y*r12 + at.y*r22;
    m->at.z    = rg.z*r02 + up.z*r12 + at.z*r22;
}

// ----------------------------------------------------------------------------
// Create / render
// ----------------------------------------------------------------------------
static bool CreateWeaponInstance(RenderedWeapon* arr, int wt, bool secondary, int slot, CPed* ped) {
    if (wt <= 0) return false;
    if (secondary) {
        if (g_cfg2.empty() || wt >= (int)g_cfg2.size()) return false;
    } else {
        if (g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
    }
    const WeaponCfg& wc = secondary ? GetWeaponCfg2ForPed(ped, wt) : GetWeaponCfgForPed(ped, wt);
    if (!wc.enabled || wc.boneId == 0) return false;
    if (FindSlotByType(arr, wt, secondary) >= 0) return true;

    CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    if (!info) return false;
    int mid = info->m_nModelId;
    if (mid <= 0) return false;

    RwMatrix* bone = GetBoneMatrix(ped, wc.boneId);
    if (!bone) return false;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));

    std::string replacementKey;
    RwObject* inst = nullptr;
    if (g_weaponReplacementEnabled && g_weaponReplacementOnBody) {
        if (WeaponReplacementAsset* asset = ResolveUsableWeaponReplacementAssetForPed(ped, wt, true)) {
            inst = CloneWeaponReplacementObject(*asset);
            if (inst)
                replacementKey = asset->key;
        }
    }

    if (!inst) {
        auto* mi = CModelInfo::GetModelInfo(mid);
        if (!mi || !mi->m_pRwObject) {
            // Auto-request weapon model streaming (supports modded weapon.dat setups).
            if (mid > 0 && !CStreaming::HasModelLoaded(mid)) {
                static std::unordered_set<int> requested;
                if (requested.insert(mid).second) {
                    CStreaming::RequestModel(mid, 0);
                    CStreaming::LoadAllRequestedModels(false);
                }
            }
            return false;
        }
        inst = mi->CreateInstance(&mtx);
    }
    if (!inst) return false;

    // Сброс render-callback на дефолтный RW + leak материалов (см. InitAtomicCB).
    if (inst->type == rpCLUMP) {
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), InitAttachmentAtomicCB, nullptr);
    } else if (inst->type == rpATOMIC) {
        InitAttachmentAtomicCB(reinterpret_cast<RpAtomic*>(inst), nullptr);
    } else {
        DestroyRwObjectInstance(inst);
        return false;
    }

    int fi = FindFree(arr);
    if (fi < 0) {
        OrcLogError("CreateWeaponInstance: no free slot (weapon type %d)", wt);
        DestroyRwObjectInstance(inst);
        return false;
    }
    arr[fi] = { true, wt, secondary, mid, slot, inst, replacementKey };

    static std::unordered_set<int> logged;
    if (!secondary && logged.insert(wt).second) {
    }
    return true;
}

static void RenderOneWeapon(CPed* ped, RenderedWeapon& r) {
    if (!r.rwObject) return;

    const WeaponCfg& wc = r.secondary ? GetWeaponCfg2ForPed(ped, r.weaponType) : GetWeaponCfgForPed(ped, r.weaponType);
    RwMatrix* bone = GetBoneMatrix(ped, wc.boneId);
    if (!bone) return;

    RpAtomic* atomic = nullptr;
    RwFrame*  frame  = nullptr;
    if (r.rwObject->type == rpATOMIC) {
        atomic = reinterpret_cast<RpAtomic*>(r.rwObject);
        frame  = RpAtomicGetFrame(atomic);
    } else if (r.rwObject->type == rpCLUMP) {
        frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(r.rwObject));
    }
    if (!frame) return;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    ApplyOffset(&mtx, wc.x, wc.y, wc.z);
    RotateMatrix(&mtx, wc.rx, wc.ry, wc.rz);

    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    if (wc.scale != 1.0f) {
        RwV3d s = { wc.scale, wc.scale, wc.scale };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    ApplyAttachmentLightingForPed(ped, lightPos);
    WeaponTextureAsset* textureAsset = ResolveUsableWeaponTextureAssetForPed(ped, r.weaponType, true);
    ApplyWeaponTextureToRwObject(r.rwObject, textureAsset);

    if (r.rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(r.rwObject);
        if (!clump) {
            RestoreWeaponTextureOverrides();
            return;
        }
        RpClumpForAllAtomics(clump, PrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        PrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
    RestoreWeaponTextureOverrides();
}

static RwFrame* GetRwObjectFrameForRender(RwObject* object) {
    if (!object)
        return nullptr;
    if (object->type == rpATOMIC)
        return RpAtomicGetFrame(reinterpret_cast<RpAtomic*>(object));
    if (object->type == rpCLUMP)
        return RpClumpGetFrame(reinterpret_cast<RpClump*>(object));
    return nullptr;
}

static void SetRwObjectMatrixForRender(RwObject* object, const RwMatrix& matrix) {
    RwFrame* frame = GetRwObjectFrameForRender(object);
    if (!frame)
        return;
    std::memcpy(RwFrameGetMatrix(frame), &matrix, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    RwFrameUpdateObjects(frame);
}

static void RenderPreparedRwObject(RwObject* object) {
    if (!object)
        return;
    if (object->type == rpCLUMP) {
        RpClump* clump = reinterpret_cast<RpClump*>(object);
        RpClumpForAllAtomics(clump, PrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else if (object->type == rpATOMIC) {
        RpAtomic* atomic = reinterpret_cast<RpAtomic*>(object);
        PrepAtomicCB(atomic, nullptr);
        if (atomic->renderCallBack)
            atomic->renderCallBack(atomic);
        else
            RpAtomicRender(atomic);
    }
}

struct HeldWeaponReplacementState {
    int weaponType = 0;
    std::string replacementKey;
    RwObject* rwObject = nullptr;
    RwObject* originalObject = nullptr;
    RwMatrix matrix{};
    bool captureActive = false;
};

static std::unordered_map<int, HeldWeaponReplacementState> g_heldWeaponReplacements;

static int GetPedCurrentWeaponType(CPed* ped) {
    if (!ped)
        return 0;
    const unsigned char slot = ped->m_nSelectedWepSlot;
    if (slot >= 13)
        return 0;
    const int wt = (int)ped->m_aWeapons[slot].m_eWeaponType;
    if (wt <= 0)
        return 0;
    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    const bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
    if (needsAmmo && ped->m_aWeapons[slot].m_nAmmoTotal == 0)
        return 0;
    return wt;
}

static bool ShouldReplaceHeldWeaponForPed(CPed* ped) {
    if (!g_enabled || !g_weaponReplacementEnabled || !g_weaponReplacementInHands || !ped)
        return false;
    CPlayerPed* player = FindPlayerPed(0);
    if (player && ped == player)
        return true;
    if (!g_renderAllPedsWeapons || !player)
        return false;
    const CVector& pp = player->GetPosition();
    const CVector& p = ped->GetPosition();
    const float dx = p.x - pp.x;
    const float dy = p.y - pp.y;
    const float dz = p.z - pp.z;
    return (dx * dx + dy * dy + dz * dz) <= (g_renderAllPedsRadius * g_renderAllPedsRadius);
}

static bool ShouldTextureHeldWeaponForPed(CPed* ped) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped)
        return false;
    CPlayerPed* player = FindPlayerPed(0);
    if (player && ped == player)
        return true;
    if (!g_renderAllPedsWeapons || !player)
        return false;
    const CVector& pp = player->GetPosition();
    const CVector& p = ped->GetPosition();
    const float dx = p.x - pp.x;
    const float dy = p.y - pp.y;
    const float dz = p.z - pp.z;
    return (dx * dx + dy * dy + dz * dz) <= (g_renderAllPedsRadius * g_renderAllPedsRadius);
}

static void PrepareHeldWeaponTextureBefore(CPed* ped) {
    if (!ShouldTextureHeldWeaponForPed(ped) || !ped->m_pWeaponObject)
        return;
    const int wt = GetPedCurrentWeaponType(ped);
    if (wt <= 0)
        return;
    WeaponTextureAsset* asset = ResolveUsableWeaponTextureAssetForPed(ped, wt, true);
    ApplyWeaponTextureToRwObject(ped->m_pWeaponObject, asset);
}

static void DestroyAllHeldWeaponReplacementInstances() {
    for (auto& kv : g_heldWeaponReplacements) {
        kv.second.captureActive = false;
        kv.second.originalObject = nullptr;
        DestroyRwObjectInstance(kv.second.rwObject);
    }
    g_heldWeaponReplacements.clear();
}

static void PruneHeldWeaponReplacementInstances() {
    if (!CPools::ms_pPedPool) {
        DestroyAllHeldWeaponReplacementInstances();
        return;
    }
    std::unordered_set<int> alive;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
        CPed* ped = CPools::ms_pPedPool->GetAt(i);
        if (ped)
            alive.insert(CPools::GetPedRef(ped));
    }
    for (auto it = g_heldWeaponReplacements.begin(); it != g_heldWeaponReplacements.end();) {
        if (alive.find(it->first) == alive.end()) {
            DestroyRwObjectInstance(it->second.rwObject);
            it = g_heldWeaponReplacements.erase(it);
        } else {
            ++it;
        }
    }
}

static void PrepareHeldWeaponReplacementBefore(CPed* ped) {
    if (!ShouldReplaceHeldWeaponForPed(ped) || !ped->m_pWeaponObject)
        return;
    const int wt = GetPedCurrentWeaponType(ped);
    if (wt <= 0)
        return;
    WeaponReplacementAsset* asset = ResolveUsableWeaponReplacementAssetForPed(ped, wt, true);
    if (!asset)
        return;

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    HeldWeaponReplacementState& state = g_heldWeaponReplacements[pedRef];
    if (!state.rwObject || state.weaponType != wt || state.replacementKey != asset->key) {
        DestroyRwObjectInstance(state.rwObject);
        state.rwObject = CloneWeaponReplacementObject(*asset);
        state.weaponType = wt;
        state.replacementKey = asset->key;
    }
    if (!state.rwObject)
        return;

    if (RwFrame* frame = GetRwObjectFrameForRender(ped->m_pWeaponObject)) {
        std::memcpy(&state.matrix, RwFrameGetMatrix(frame), sizeof(RwMatrix));
    } else if (RwMatrix* bone = GetBoneMatrix(ped, BONE_R_HAND)) {
        std::memcpy(&state.matrix, bone, sizeof(RwMatrix));
    } else {
        return;
    }
    state.originalObject = ped->m_pWeaponObject;
    state.captureActive = true;
    ped->m_pWeaponObject = nullptr;
}

static void RestoreHeldWeaponReplacementAfter(CPed* ped) {
    if (!ped)
        return;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    auto it = g_heldWeaponReplacements.find(pedRef);
    if (it == g_heldWeaponReplacements.end() || !it->second.captureActive)
        return;

    HeldWeaponReplacementState& state = it->second;
    ped->m_pWeaponObject = state.originalObject;
    state.originalObject = nullptr;
    state.captureActive = false;
    if (!state.rwObject)
        return;

    SetRwObjectMatrixForRender(state.rwObject, state.matrix);

    int oldCull = 0, oldZT = 0, oldZW = 0, oldShade = 0, oldFog = 0;
    RwRenderStateGet(rwRENDERSTATECULLMODE,     &oldCull);
    RwRenderStateGet(rwRENDERSTATEZTESTENABLE,  &oldZT);
    RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &oldZW);
    RwRenderStateGet(rwRENDERSTATESHADEMODE,    &oldShade);
    RwRenderStateGet(rwRENDERSTATEFOGENABLE,    &oldFog);

    int v = rwCULLMODECULLBACK; RwRenderStateSet(rwRENDERSTATECULLMODE, reinterpret_cast<void*>(v));
    v = TRUE; RwRenderStateSet(rwRENDERSTATEZTESTENABLE, reinterpret_cast<void*>(v));
    v = TRUE; RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(v));
    v = rwSHADEMODEGOURAUD; RwRenderStateSet(rwRENDERSTATESHADEMODE, reinterpret_cast<void*>(v));
    v = FALSE; RwRenderStateSet(rwRENDERSTATEFOGENABLE, reinterpret_cast<void*>(v));

    CVector lightPos = { state.matrix.pos.x, state.matrix.pos.y, state.matrix.pos.z };
    ApplyAttachmentLightingForPed(ped, lightPos);
    WeaponTextureAsset* textureAsset = ResolveUsableWeaponTextureAssetForPed(ped, state.weaponType, true);
    ApplyWeaponTextureToRwObject(state.rwObject, textureAsset);
    RenderPreparedRwObject(state.rwObject);
    RestoreWeaponTextureOverrides();

    RwRenderStateSet(rwRENDERSTATECULLMODE,     reinterpret_cast<void*>(oldCull));
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE,  reinterpret_cast<void*>(oldZT));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(oldZW));
    RwRenderStateSet(rwRENDERSTATESHADEMODE,    reinterpret_cast<void*>(oldShade));
    RwRenderStateSet(rwRENDERSTATEFOGENABLE,    reinterpret_cast<void*>(oldFog));
}

static void ClearAllWeaponReplacementInstances() {
    ClearAll();
    ClearAllOtherPeds();
    DestroyAllHeldWeaponReplacementInstances();
}

static bool PedHasWeaponType(CPed* ped, int wt) {
    if (!ped || wt <= 0) return false;
    if (g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
    for (int s = 0; s < 13; s++) {
        auto& w = ped->m_aWeapons[s];
        if ((int)w.m_eWeaponType != wt) continue;
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
        bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
        if (needsAmmo && w.m_nAmmoTotal == 0) return false;
        return true;
    }
    return false;
}

static bool ShouldRenderObjectForPedWithParams(CPed* ped, const CustomObjectSkinParams& p) {
    if (!p.enabled || p.boneId == 0) return false;
    if (p.weaponTypes.empty()) return true;

    if (p.weaponRequireAll) {
        for (int wt : p.weaponTypes) {
            if (!PedHasWeaponType(ped, wt)) return false;
        }
        return true;
    }

    for (int wt : p.weaponTypes) {
        if (PedHasWeaponType(ped, wt)) return true;
    }
    return false;
}

static void ApplyObjectWeaponSuppression(CPed* ped, std::vector<char>* suppress) {
    if (!ped || !suppress || (!g_renderCustomObjects && !g_renderStandardObjects)) return;
    const std::string skin = GetPedStdSkinDffName(ped);
    if (skin.empty()) return;
    auto applyParams = [&](const CustomObjectSkinParams& p) {
        if (!p.hideSelectedWeapons) return;
        if (p.weaponTypes.empty()) return;
        if (!ShouldRenderObjectForPedWithParams(ped, p)) return;
        for (int wt : p.weaponTypes) {
            if (wt <= 0 || wt >= (int)suppress->size()) continue;
            (*suppress)[wt] = 1;
        }
    };
    if (g_renderCustomObjects) for (const auto& o : g_customObjects) {
        CustomObjectSkinParams p;
        if (!ResolveObjectSkinParamsCached(o.iniPath, skin, p)) continue;
        applyParams(p);
    }
    if (g_renderStandardObjects) for (const auto& o : g_standardObjects) {
        CustomObjectSkinParams p;
        if (!ResolveStandardObjectSkinParamsCached(o.modelId, o.slot, skin, p)) continue;
        applyParams(p);
    }
}

static bool EnsureCustomInstance(CustomObjectCfg& o) {
    if (!EnsureCustomModelLoaded(o)) return false;
    return o.rwObject != nullptr;
}

static void RenderCustomObject(CPed* ped, CustomObjectCfg& o, const CustomObjectSkinParams& p) {
    if (!o.rwObject) return;
    RwMatrix* bone = GetBoneMatrix(ped, p.boneId);
    if (!bone) return;

    RpAtomic* atomic = nullptr;
    RwFrame* frame = nullptr;
    if (o.rwObject->type == rpATOMIC) {
        atomic = reinterpret_cast<RpAtomic*>(o.rwObject);
        frame = RpAtomicGetFrame(atomic);
    } else if (o.rwObject->type == rpCLUMP) {
        frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(o.rwObject));
    }
    if (!frame) return;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    ApplyOffset(&mtx, p.x, p.y, p.z);
    RotateMatrix(&mtx, p.rx, p.ry, p.rz);
    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    const float sx = p.scale * p.scaleX;
    const float sy = p.scale * p.scaleY;
    const float sz = p.scale * p.scaleZ;
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        RwV3d s = { sx, sy, sz };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    ApplyAttachmentLightingForPed(ped, lightPos);

    if (o.rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(o.rwObject);
        if (!clump) return;
        RpClumpForAllAtomics(clump, PrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        PrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
}

static RenderedStandardObject* EnsureStandardObjectInstance(CPed* ped, const StandardObjectSlotCfg& cfg, const CustomObjectSkinParams& p) {
    if (!ped || cfg.modelId < 0 || cfg.slot <= 0) return nullptr;
    RwMatrix* bone = GetBoneMatrix(ped, p.boneId);
    if (!bone) return nullptr;
    CBaseModelInfo* mi = GetExistingStandardObjectModelInfo(cfg.modelId);
    if (!mi) {
        DestroyStandardObjectInstancesForSlot(cfg.modelId, cfg.slot);
        return nullptr;
    }

    StandardObjectRuntimeKey key;
    key.ped = ped;
    key.modelId = cfg.modelId;
    key.slot = cfg.slot;
    auto existing = g_renderedStandardObjects.find(key);
    if (existing != g_renderedStandardObjects.end() && existing->second.rwObject)
        return &existing->second;

    if (!CStreaming::HasModelLoaded(cfg.modelId) || !mi->m_pRwObject) {
        const DWORD now = GetTickCount();
        DWORD& last = g_standardObjectLastRequestTick[cfg.modelId];
        if (last == 0 || now - last > 1000) {
            last = now;
            CStreaming::RequestModel(cfg.modelId, 0);
            CStreaming::LoadAllRequestedModels(false);
        }
        return nullptr;
    }

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    RwObject* inst = mi->CreateInstance(&mtx);
    if (!inst) return nullptr;
    if (inst->type == rpCLUMP) {
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), InitAttachmentAtomicCB, nullptr);
    } else if (inst->type == rpATOMIC) {
        InitAttachmentAtomicCB(reinterpret_cast<RpAtomic*>(inst), nullptr);
    } else {
        return nullptr;
    }

    RenderedStandardObject rendered;
    rendered.modelId = cfg.modelId;
    rendered.slot = cfg.slot;
    rendered.ped = ped;
    rendered.rwObject = inst;
    rendered.lastSeenFrame = g_standardObjectRenderFrame;
    auto inserted = g_renderedStandardObjects.emplace(key, rendered);
    return &inserted.first->second;
}

static void RenderStandardObject(CPed* ped, const StandardObjectSlotCfg& cfg, const CustomObjectSkinParams& p) {
    RenderedStandardObject* rendered = EnsureStandardObjectInstance(ped, cfg, p);
    if (!rendered || !rendered->rwObject) return;

    RwMatrix* bone = GetBoneMatrix(ped, p.boneId);
    if (!bone) return;

    RpAtomic* atomic = nullptr;
    RwFrame* frame = nullptr;
    if (rendered->rwObject->type == rpATOMIC) {
        atomic = reinterpret_cast<RpAtomic*>(rendered->rwObject);
        frame = RpAtomicGetFrame(atomic);
    } else if (rendered->rwObject->type == rpCLUMP) {
        frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(rendered->rwObject));
    }
    if (!frame) return;
    rendered->lastSeenFrame = g_standardObjectRenderFrame;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    ApplyOffset(&mtx, p.x, p.y, p.z);
    RotateMatrix(&mtx, p.rx, p.ry, p.rz);
    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    const float sx = p.scale * p.scaleX;
    const float sy = p.scale * p.scaleY;
    const float sz = p.scale * p.scaleZ;
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        RwV3d s = { sx, sy, sz };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    ApplyAttachmentLightingForPed(ped, lightPos);

    if (rendered->rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(rendered->rwObject);
        if (!clump) return;
        RpClumpForAllAtomics(clump, PrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        PrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
}

static CustomSkinCfg* GetSelectedSkin() {
    if (g_customSkins.empty()) return nullptr;
    if (!g_skinSelectedName.empty()) {
        const std::string selectedLower = ToLowerAscii(g_skinSelectedName);
        if (g_selectedSkinCacheIdx >= 0 &&
            g_selectedSkinCacheIdx < (int)g_customSkins.size() &&
            g_selectedSkinCacheNameLower == selectedLower &&
            ToLowerAscii(g_customSkins[(size_t)g_selectedSkinCacheIdx].name) == selectedLower) {
            return &g_customSkins[(size_t)g_selectedSkinCacheIdx];
        }

        for (int i = 0; i < (int)g_customSkins.size(); ++i) {
            if (ToLowerAscii(g_customSkins[(size_t)i].name) == selectedLower) {
                g_selectedSkinCacheIdx = i;
                g_selectedSkinCacheNameLower = selectedLower;
                return &g_customSkins[(size_t)i];
            }
        }
    }
    if (g_uiSkinIdx < 0 || g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
    g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
    g_selectedSkinCacheIdx = g_uiSkinIdx;
    g_selectedSkinCacheNameLower = ToLowerAscii(g_skinSelectedName);
    return &g_customSkins[g_uiSkinIdx];
}

static void CopySkinHierarchyPose(CPed* player, RpClump* skinClump) {
    RpHAnimHierarchy* src = GetAnimHierarchyFromSkinClump(player->m_pRwClump);
    RpHAnimHierarchy* dst = GetAnimHierarchyFromSkinClump(skinClump);
    if (!src || !dst || !src->pMatrixArray || !dst->pMatrixArray) return;
    for (int i = 0; i < src->numNodes; i++) {
        int nodeId = src->pNodeInfo ? src->pNodeInfo[i].nodeID : -1;
        if (nodeId < 0) continue;
        int di = RpHAnimIDGetIndex(dst, nodeId);
        if (di < 0 || di >= dst->numNodes) continue;
        dst->pMatrixArray[di] = src->pMatrixArray[i];
    }
    RpHAnimHierarchyUpdateMatrices(dst);

    // Some skin clumps render from frame matrices directly; mirror frame pose by node ID too.
    if (src->pNodeInfo && dst->pNodeInfo) {
        for (int i = 0; i < src->numNodes; i++) {
            const int nodeId = src->pNodeInfo[i].nodeID;
            const int di = RpHAnimIDGetIndex(dst, nodeId);
            if (di < 0 || di >= dst->numNodes) continue;
            RwFrame* sf = src->pNodeInfo[i].pFrame;
            RwFrame* df = dst->pNodeInfo[di].pFrame;
            if (!sf || !df) continue;
            std::memcpy(RwFrameGetMatrix(df), RwFrameGetMatrix(sf), sizeof(RwMatrix));
            RwMatrixUpdate(RwFrameGetMatrix(df));
        }
    }
}

static RpAtomic* BindSkinHierarchyCB(RpAtomic* a, void* data) {
    if (!a || !data) return a;
    if (RpSkinAtomicGetSkin(a)) {
        RpSkinAtomicSetHAnimHierarchy(a, reinterpret_cast<RpHAnimHierarchy*>(data));
        RpSkinAtomicSetType(a, rpSKINTYPEGENERIC);
        g_skinBindCount++;
    }
    return a;
}

static bool SkinMatchesNickname(const CustomSkinCfg& s, const std::string& nickLower) {
    if (!s.bindToNick || s.nicknames.empty()) return false;
    for (const auto& n : s.nicknames) if (n == nickLower) return true;
    return false;
}

static bool StandardSkinMatchesNickname(const StandardSkinCfg& s, const std::string& nickLower) {
    if (!s.bindToNick || s.nicknames.empty()) return false;
    for (const auto& n : s.nicknames) if (n == nickLower) return true;
    return false;
}

static CustomSkinCfg* FindNickSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    EnsureCustomSkinNickLookup();
    auto it = g_customSkinNickLookup.find(nickLower);
    if (it == g_customSkinNickLookup.end()) return nullptr;
    const int idx = it->second;
    if (idx < 0 || idx >= (int)g_customSkins.size()) return nullptr;
    if (!SkinMatchesNickname(g_customSkins[(size_t)idx], nickLower)) return nullptr;
    return &g_customSkins[(size_t)idx];
}

static StandardSkinCfg* FindNickStandardSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    EnsureStandardSkinNickLookup();
    auto it = g_standardSkinNickLookup.find(nickLower);
    if (it == g_standardSkinNickLookup.end()) return nullptr;
    StandardSkinCfg* skin = OrcGetStandardSkinCfgByModelId(it->second, false);
    if (!skin || !StandardSkinMatchesNickname(*skin, nickLower)) return nullptr;
    return skin;
}

static StandardSkinCfg* GetSelectedStandardSkin() {
    if (g_standardSkinSelectedModelId >= 0) {
        if (StandardSkinCfg* skin = OrcGetStandardSkinCfgByModelId(g_standardSkinSelectedModelId, true))
            return skin;
    }
    std::vector<std::pair<std::string, int>> skins;
    OrcCollectPedSkins(skins);
    if (skins.empty()) return nullptr;
    g_standardSkinSelectedModelId = skins.front().second;
    return OrcGetStandardSkinCfgByModelId(g_standardSkinSelectedModelId, true);
}

struct ResolvedPedSkin {
    CustomSkinCfg* custom = nullptr;
    StandardSkinCfg* standard = nullptr;
    bool isLocalPed = false;
};

static ResolvedPedSkin ResolveSkinForPed(CPed* ped, CPlayerPed* localPlayer) {
    ResolvedPedSkin result;
    if (!ped || (!g_skinModeEnabled && !g_skinRandomFromPools)) return result;
    CustomSkinCfg* selectedCustom = (g_skinModeEnabled && g_skinSelectedSource == SKIN_SELECTED_CUSTOM) ? GetSelectedSkin() : nullptr;
    StandardSkinCfg* selectedStandard = (g_skinModeEnabled && g_skinSelectedSource == SKIN_SELECTED_STANDARD) ? GetSelectedStandardSkin() : nullptr;
    const bool isLocalByPtr = (localPlayer && ped == localPlayer);

    if (g_skinNickMode && samp_bridge::IsSampBuildKnown()) {
        char nick[32] = {};
        bool isLocalBySamp = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocalBySamp)) {
            const bool isLocal = isLocalBySamp || isLocalByPtr;
            result.isLocalPed = isLocal;
            CustomSkinCfg* nickSkin = FindNickSkin(ToLowerAscii(nick));
            // Ник всегда выше «выбранного скина» и рандом-пулов.
            if (nickSkin) {
                result.custom = nickSkin;
                return result;
            }
            if (StandardSkinCfg* standardNickSkin = FindNickStandardSkin(ToLowerAscii(nick))) {
                result.standard = standardNickSkin;
                return result;
            }
            if (isLocal && g_skinModeEnabled && g_skinLocalPreferSelected) {
                result.custom = selectedCustom;
                result.standard = selectedStandard;
                return result;
            }
        }
    }

    // «Always use selected» для локального игрока должно работать и при выключенном nick binding,
    // иначе весь блок выше пропускается и выбранный в Skins скин никогда не применяется.
    if (isLocalByPtr && g_skinLocalPreferSelected) {
        result.isLocalPed = true;
        result.custom = selectedCustom;
        result.standard = selectedStandard;
        return result;
    }

    if (isLocalByPtr) result.isLocalPed = true;
    if (CustomSkinCfg* randomSkin = ResolveRandomSkinForPed(ped)) {
        result.custom = randomSkin;
        result.standard = nullptr;
    }
    return result;
}

static void RenderSkinOnPed(CPed* ped, CustomSkinCfg* sel, bool isLocalPed) {
    if (!ped || !ped->m_pRwClump || !sel) return;
    RpClump* pedClump = ped->m_pRwClump;
    if (!EnsureCustomSkinLoaded(*sel)) return;
    if (!sel->rwObject || sel->rwObject->type != rpCLUMP) return;
    RpClump* clump = reinterpret_cast<RpClump*>(sel->rwObject);
    if (!clump) return;
    RwFrame* srcFrame = RpClumpGetFrame(pedClump);
    RwFrame* dstFrame = RpClumpGetFrame(clump);
    if (!srcFrame || !dstFrame) return;
    std::memcpy(RwFrameGetMatrix(dstFrame), RwFrameGetMatrix(srcFrame), sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(dstFrame));
    RpHAnimHierarchy* srcH = GetAnimHierarchyFromSkinClump(pedClump);
    g_skinBindCount = 0;
    if (srcH && clump) RpClumpForAllAtomics(clump, BindSkinHierarchyCB, srcH);
    const bool canAnimate = (srcH && g_skinBindCount > 0);
    if (isLocalPed) g_skinCanAnimate = canAnimate;
    if (canAnimate) {
        if (ped->m_pRwClump != pedClump) return;
        CopySkinHierarchyPose(ped, clump);
    }
    RwFrameUpdateObjects(dstFrame);
    // Штатная цепочка освещения педа (SEH) + ApplyAttachment colourScale=1.0, без PrepAtomicCB.
    const bool lit = OrcTryPedSetupLighting(ped);
    static int s_setupLogLeft = 8;
    if (g_orcLogLevel >= OrcLogLevel::Info && s_setupLogLeft > 0) {
        OrcLogInfo("SetupLighting skin=%s -> %d ped=%p", sel->name.c_str(), lit ? 1 : 0, ped);
        s_setupLogLeft--;
    }
    CVector boundCentre{};
    ped->GetBoundCentre(boundCentre);
    static int s_fallbackLogLeft = 8;
    if (g_orcLogLevel >= OrcLogLevel::Info && !lit && s_fallbackLogLeft > 0) {
        OrcLogInfo("skin: SetupLighting false, ApplyAttachment only name=%s", sel->name.c_str());
        s_fallbackLogLeft--;
    }
    ApplyAttachmentLightingForPed(ped, boundCentre, 1.0f);
    const char* remapKey = sel->remapKey.empty() ? sel->name.c_str() : sel->remapKey.c_str();
    const char* remapFallback = sel->remapFallbackKey.empty() ? nullptr : sel->remapFallbackKey.c_str();
    OrcTextureRemapApplyToClumpBefore(ped, clump, remapKey, remapFallback, sel->txdSlot);
    OrcTryRpClumpRender(clump);
    OrcTextureRemapRestoreAfter();
    if (lit)
        OrcTryPedRemoveLighting(ped);
}

static void RenderStandardSkinOnPed(CPed* ped, StandardSkinCfg* sel, bool isLocalPed) {
    if (!ped || !ped->m_pRwClump || !sel) return;
    RpClump* pedClump = ped->m_pRwClump;
    if (!EnsureStandardSkinLoaded(*sel)) return;
    if (!sel->rwObject || sel->rwObject->type != rpCLUMP) return;
    RpClump* clump = reinterpret_cast<RpClump*>(sel->rwObject);
    if (!clump) return;
    RwFrame* srcFrame = RpClumpGetFrame(pedClump);
    RwFrame* dstFrame = RpClumpGetFrame(clump);
    if (!srcFrame || !dstFrame) return;
    std::memcpy(RwFrameGetMatrix(dstFrame), RwFrameGetMatrix(srcFrame), sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(dstFrame));
    RpHAnimHierarchy* srcH = GetAnimHierarchyFromSkinClump(pedClump);
    g_skinBindCount = 0;
    if (srcH && clump) RpClumpForAllAtomics(clump, BindSkinHierarchyCB, srcH);
    const bool canAnimate = (srcH && g_skinBindCount > 0);
    if (isLocalPed) g_skinCanAnimate = canAnimate;
    if (canAnimate) {
        if (ped->m_pRwClump != pedClump) return;
        CopySkinHierarchyPose(ped, clump);
    }
    RwFrameUpdateObjects(dstFrame);
    const bool lit = OrcTryPedSetupLighting(ped);
    static int s_setupLogLeft = 8;
    if (g_orcLogLevel >= OrcLogLevel::Info && s_setupLogLeft > 0) {
        OrcLogInfo("SetupLighting standard skin=%s[%d] -> %d ped=%p",
            sel->dffName.c_str(), sel->modelId, lit ? 1 : 0, ped);
        s_setupLogLeft--;
    }
    CVector boundCentre{};
    ped->GetBoundCentre(boundCentre);
    ApplyAttachmentLightingForPed(ped, boundCentre, 1.0f);
    int txdIndex = -1;
    if (CBaseModelInfo* mi = CModelInfo::GetModelInfo(sel->modelId))
        txdIndex = mi->m_nTxdIndex;
    OrcTextureRemapApplyToClumpBefore(ped, clump, sel->dffName.c_str(), nullptr, txdIndex);
    OrcTryRpClumpRender(clump);
    OrcTextureRemapRestoreAfter();
    if (lit)
        OrcTryPedRemoveLighting(ped);
}

static void SyncPedWeapons(CPed* ped, RenderedWeapon* arr, const std::vector<char>* suppress = nullptr) {
    if (!ped) return;
    unsigned char curSlot = ped->m_nSelectedWepSlot;
    int curType = 0;
    if (curSlot < 13) curType = (int)ped->m_aWeapons[curSlot].m_eWeaponType;
    if (g_cfg.empty()) return;
    const int maxWt = (int)g_cfg.size();
    std::vector<char> want(maxWt, 0);
    std::vector<char> want2(maxWt, 0);
    for (int s = 0; s < 13; s++) {
        auto& w = ped->m_aWeapons[s];
        int wt = (int)w.m_eWeaponType;
        if (wt <= 0 || wt >= maxWt) continue;
        if (suppress && wt < (int)suppress->size() && (*suppress)[wt]) continue;
        if (wt == curType) continue;
        const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
        if (!wc.enabled || wc.boneId == 0) continue;
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
        bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
        if (needsAmmo && w.m_nAmmoTotal == 0) continue;
        want[wt] = true;

        CWeaponInfo* twinInfo = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 2);
        if (!twinInfo) twinInfo = wi;
        if (g_considerWeaponSkills && twinInfo && twinInfo->m_nFlags.bTwinPistol) {
            const char skill = ped->GetWeaponSkill(static_cast<eWeaponType>(wt));
            if (skill == WEAPSKILL_PRO) {
                const WeaponCfg& wc2 = GetWeaponCfg2ForPed(ped, wt);
                if (wc2.enabled && wc2.boneId != 0) want2[wt] = true;
            }
        }
    }
    for (int i = 0; i < kMax; i++) {
        if (!arr[i].active) continue;
        int wt = arr[i].weaponType;
        bool keep = (wt >= 0 && wt < maxWt) && (arr[i].secondary ? want2[wt] : want[wt]);
        if (keep) {
            const std::string desiredReplacementKey =
                (g_weaponReplacementEnabled && g_weaponReplacementOnBody)
                ? ResolveUsableWeaponReplacementKeyForPed(ped, wt, true)
                : std::string{};
            if (desiredReplacementKey != arr[i].replacementKey)
                keep = false;
        }
        if (!keep) DestroyRendered(arr[i]);
    }
    for (int wt = 1; wt < maxWt; wt++) if (want[wt])  CreateWeaponInstance(arr, wt, false, 0, ped);
    for (int wt = 1; wt < maxWt; wt++) if (want2[wt]) CreateWeaponInstance(arr, wt, true,  0, ped);
}

static int RenderPedWeapons(CPed* ped, RenderedWeapon* arr) {
    int active = 0;
    for (int i = 0; i < kMax; i++) {
        if (!arr[i].active) continue;
        active++;
        RenderOneWeapon(ped, arr[i]);
    }
    return active;
}

static void RenderSkinsForPeds(CPlayerPed* localPlayer) {
    if (!localPlayer || (!g_skinModeEnabled && !g_skinRandomFromPools)) return;
    g_skinCanAnimate = false;
    if (g_skinRandomFromPools)
        PrunePedRandomSkinMap();
    bool localDone = false;
    if (!CPools::ms_pPedPool) return;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; i++) {
        CPed* ped = CPools::ms_pPedPool->GetAt(i);
        if (!ped || !ped->m_pRwClump) continue;
        ResolvedPedSkin skin = ResolveSkinForPed(ped, localPlayer);
        if (!skin.custom && !skin.standard) continue;
        if (skin.custom)
            RenderSkinOnPed(ped, skin.custom, skin.isLocalPed);
        else
            RenderStandardSkinOnPed(ped, skin.standard, skin.isLocalPed);
        if (skin.isLocalPed) localDone = true;
    }
    if (!localDone) {
        ResolvedPedSkin skin = ResolveSkinForPed(localPlayer, localPlayer);
        if (skin.custom)
            RenderSkinOnPed(localPlayer, skin.custom, true);
        else if (skin.standard)
            RenderStandardSkinOnPed(localPlayer, skin.standard, true);
    }
}

static void SyncAndRender() {
    if (!g_enabled) {
        ClearAll();
        ClearAllOtherPeds();
        DestroyAllHeldWeaponReplacementInstances();
        RestoreWeaponTextureOverrides();
        DestroyStandardSkinPreview();
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
        DestroyAllStandardObjectInstances();
        for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
        for (auto& s : g_standardSkins) DestroyStandardSkinInstance(s);
        DestroyAllRandomPoolSkins();
        OrcTextureRemapClearRuntimeState();
        return;
    }
    CPlayerPed* player = FindPlayerPed(0);
    if (!player) {
        ClearAll();
        ClearAllOtherPeds();
        DestroyAllHeldWeaponReplacementInstances();
        RestoreWeaponTextureOverrides();
        DestroyStandardSkinPreview();
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
        DestroyAllStandardObjectInstances();
        for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
        for (auto& s : g_standardSkins) DestroyStandardSkinInstance(s);
        DestroyAllRandomPoolSkins();
        OrcTextureRemapClearRuntimeState();
        return;
    }
    if (!g_weaponReplacementEnabled || !g_weaponReplacementInHands)
        DestroyAllHeldWeaponReplacementInstances();
    else
        PruneHeldWeaponReplacementInstances();

    std::vector<char> suppress;
    suppress.assign(g_cfg.size(), 0);
    ApplyObjectWeaponSuppression(player, &suppress);
    SyncPedWeapons(player, g_rendered, &suppress);
    std::vector<char> suppressPed;
    suppressPed.assign(g_cfg.size(), 0);
    std::vector<char> objectUsed;
    objectUsed.assign(g_customObjects.size(), 0);
    ++g_standardObjectRenderFrame;
    if (g_standardObjectRenderFrame == 0)
        ++g_standardObjectRenderFrame;
    int active = 0;
    for (int i = 0; i < kMax; i++) if (g_rendered[i].active) active++;
    const std::string plSkin = GetPedStdSkinDffName(player);
    for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
        auto& o = g_customObjects[oi];
        if (!g_renderCustomObjects) {
            DestroyCustomObjectInstance(o);
            continue;
        }
        CustomObjectSkinParams op;
        if (plSkin.empty() || !ResolveObjectSkinParamsCached(o.iniPath, plSkin, op)) continue;
        if (ShouldRenderObjectForPedWithParams(player, op) && EnsureCustomInstance(o)) {
            active++;
            objectUsed[oi] = 1;
        }
    }
    if (g_skinModeEnabled) {
        if (g_skinSelectedSource == SKIN_SELECTED_STANDARD) {
            if (GetSelectedStandardSkin() != nullptr) active++;
        } else if (GetSelectedSkin() != nullptr) {
            active++;
        }
    }
    if (g_skinRandomFromPools && g_skinRandomPoolVariants > 0)
        active++;
    if (g_renderStandardObjects && !g_standardObjects.empty())
        active++;
    if (g_skinNickMode && samp_bridge::IsSampBuildKnown() && (!g_customSkins.empty() || !g_standardSkins.empty()))
        active++;
    if ((g_renderAllPedsWeapons || g_renderAllPedsObjects) && CPools::ms_pPedPool) active++;
    if (!active) {
        if (!g_renderStandardObjects || g_standardObjects.empty())
            DestroyAllStandardObjectInstances();
        return;
    }

    int oldCull, oldZT, oldZW, oldShade, oldFog;
    RwRenderStateGet(rwRENDERSTATECULLMODE,     &oldCull);
    RwRenderStateGet(rwRENDERSTATEZTESTENABLE,  &oldZT);
    RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &oldZW);
    RwRenderStateGet(rwRENDERSTATESHADEMODE,    &oldShade);
    RwRenderStateGet(rwRENDERSTATEFOGENABLE,    &oldFog);

    int v;
    v = rwCULLMODECULLBACK;  RwRenderStateSet(rwRENDERSTATECULLMODE,     reinterpret_cast<void*>(v));
    v = TRUE;                RwRenderStateSet(rwRENDERSTATEZTESTENABLE,  reinterpret_cast<void*>(v));
    v = TRUE;                RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(v));
    v = rwSHADEMODEGOURAUD;  RwRenderStateSet(rwRENDERSTATESHADEMODE,    reinterpret_cast<void*>(v));
    v = FALSE;               RwRenderStateSet(rwRENDERSTATEFOGENABLE,    reinterpret_cast<void*>(v));

    RenderPedWeapons(player, g_rendered);
    if (g_renderCustomObjects) {
        for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
            auto& o = g_customObjects[oi];
            CustomObjectSkinParams op;
            if (plSkin.empty() || !ResolveObjectSkinParamsCached(o.iniPath, plSkin, op)) {
                continue;
            }
            if (!ShouldRenderObjectForPedWithParams(player, op)) {
                continue;
            }
            if (!EnsureCustomInstance(o)) continue;
            RenderCustomObject(player, o, op);
            objectUsed[oi] = 1;
        }
    }
    if (g_renderStandardObjects) {
        for (const auto& o : g_standardObjects) {
            CustomObjectSkinParams op;
            if (plSkin.empty() || !ResolveStandardObjectSkinParamsCached(o.modelId, o.slot, plSkin, op))
                continue;
            if (!ShouldRenderObjectForPedWithParams(player, op))
                continue;
            RenderStandardObject(player, o, op);
        }
    }
    RenderSkinsForPeds(player);
    if ((g_renderAllPedsWeapons || g_renderAllPedsObjects) && CPools::ms_pPedPool) {
        if (!g_renderAllPedsWeapons) {
            ClearAllOtherPeds();
        }
        std::unordered_set<int> seen;
        const CVector& pp = player->GetPosition();
        const float r2 = g_renderAllPedsRadius * g_renderAllPedsRadius;
        for (int i = 0; i < CPools::ms_pPedPool->m_nSize; i++) {
            CPed* ped = CPools::ms_pPedPool->GetAt(i);
            if (!ped || ped == player || !ped->m_pRwClump) continue;
            const CVector& p = ped->GetPosition();
            float dx = p.x - pp.x, dy = p.y - pp.y, dz = p.z - pp.z;
            if ((dx * dx + dy * dy + dz * dz) > r2) continue;
            int h = CPools::GetPedRef(ped);
            if (g_renderAllPedsWeapons) {
                seen.insert(h);
                auto& cache = g_otherPedsRendered[h];
                std::fill(suppressPed.begin(), suppressPed.end(), 0);
                ApplyObjectWeaponSuppression(ped, &suppressPed);
                SyncPedWeapons(ped, cache.data(), &suppressPed);
                RenderPedWeapons(ped, cache.data());
            }
            if (g_renderAllPedsObjects && g_renderCustomObjects) {
                const std::string pedSkin = GetPedStdSkinDffName(ped);
                if (!pedSkin.empty()) {
                    for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
                        auto& o = g_customObjects[oi];
                        CustomObjectSkinParams op;
                        if (!ResolveObjectSkinParamsCached(o.iniPath, pedSkin, op))
                            continue;
                        if (!ShouldRenderObjectForPedWithParams(ped, op))
                            continue;
                        if (!EnsureCustomInstance(o))
                            continue;
                        RenderCustomObject(ped, o, op);
                        objectUsed[oi] = 1;
                    }
                }
            }
            if (g_renderAllPedsObjects && g_renderStandardObjects) {
                const std::string pedSkin = GetPedStdSkinDffName(ped);
                if (!pedSkin.empty()) {
                    for (const auto& o : g_standardObjects) {
                        CustomObjectSkinParams op;
                        if (!ResolveStandardObjectSkinParamsCached(o.modelId, o.slot, pedSkin, op))
                            continue;
                        if (!ShouldRenderObjectForPedWithParams(ped, op))
                            continue;
                        RenderStandardObject(ped, o, op);
                    }
                }
            }
        }
        if (g_renderAllPedsWeapons) {
            for (auto it = g_otherPedsRendered.begin(); it != g_otherPedsRendered.end();) {
                if (seen.find(it->first) == seen.end()) {
                    for (int i = 0; i < kMax; i++) DestroyRendered(it->second[i]);
                    it = g_otherPedsRendered.erase(it);
                } else {
                    ++it;
                }
            }
        }
    } else {
        ClearAllOtherPeds();
    }
    if (g_renderCustomObjects) {
        for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
            if (!objectUsed[oi]) DestroyCustomObjectInstance(g_customObjects[oi]);
        }
    } else {
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
    }
    if (g_renderStandardObjects) {
        for (auto it = g_renderedStandardObjects.begin(); it != g_renderedStandardObjects.end();) {
            if (it->second.lastSeenFrame != g_standardObjectRenderFrame) {
                DestroyRenderedStandardObject(it->second);
                it = g_renderedStandardObjects.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        DestroyAllStandardObjectInstances();
    }

    RwRenderStateSet(rwRENDERSTATECULLMODE,     reinterpret_cast<void*>(oldCull));
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE,  reinterpret_cast<void*>(oldZT));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(oldZW));
    RwRenderStateSet(rwRENDERSTATESHADEMODE,    reinterpret_cast<void*>(oldShade));
    RwRenderStateSet(rwRENDERSTATEFOGENABLE,    reinterpret_cast<void*>(oldFog));
}

static CPed* g_hiddenPed = nullptr;
static RpClump* g_hiddenClump = nullptr;
static bool g_hideSnapshotValid = false;

static void OnInitRw() {}
static void OnDrawingEvent();
static void OnPedRenderBefore(CPed* ped);
static void OnPedRenderAfter(CPed* ped);
static void OnD3dLost() {
    DestroyStandardSkinPreview();
    RestoreWeaponTextureOverrides();
    OrcTextureRemapRestoreAfter();
    overlay::OnResetBefore();
}
static void OnD3dReset() {
    DestroyStandardSkinPreview();
    RestoreWeaponTextureOverrides();
    OrcTextureRemapRestoreAfter();
    overlay::OnResetBefore();
    overlay::OnResetAfter();
}
static void OnShutdownRw();

static void OnDrawingEvent() {
    static bool inited = false;
    if (!inited) {
        inited = true;
        __try {
            SetupDefaults();
            SaveDefaultConfig();
            LoadConfig();
            OrcLogInfo("session start: skin path SetupLighting+ApplyAttachment+RemoveLighting");
            OrcLogInfo("paths logfile=%s inifile=%s", OrcLogGetPath(), g_iniPath);
            DiscoverCustomObjectsAndEnsureIni();
            LoadStandardObjectsFromIni();
            DiscoverCustomSkins();
            LoadStandardSkinsFromIni();
            DiscoverWeaponReplacements();
            DiscoverWeaponTextures();
            overlay::SetDrawCallback(&OrcUiDraw);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogError("OnDrawingEvent first-frame init: SEH ex=0x%08X, plugin disabled",
                GetExceptionCode());
            g_enabled = false;
            overlay::SetOpen(false);
        }
    }
    samp_bridge::Poll(g_toggleCommand.c_str(), &ToggleOverlayFromSamp);
    RefreshActivationRouting();
    overlay::Init();  // no-op after first time
    ApplyPendingLocalPlayerModel();
    __try { SyncAndRender(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SyncAndRender: SEH ex=0x%08X", GetExceptionCode());
    }
    overlay::DrawFrame();
}

static void OnPedRenderBefore(CPed* ped) {
    OrcTextureRemapApplyBefore(ped);
    PrepareHeldWeaponReplacementBefore(ped);
    PrepareHeldWeaponTextureBefore(ped);
    if ((!g_skinModeEnabled && !g_skinRandomFromPools) || !g_skinHideBasePed) return;
    CPlayerPed* player = FindPlayerPed(0);
    if (!ped || !ped->m_pRwClump) return;
    ResolvedPedSkin skin = ResolveSkinForPed(ped, player);
    if (!skin.custom && !skin.standard) return;
    if (skin.custom) {
        if (!EnsureCustomSkinLoaded(*skin.custom)) return;
    } else {
        if (!EnsureStandardSkinLoaded(*skin.standard)) return;
    }
    g_hiddenPed = ped;
    g_hiddenClump = ped->m_pRwClump;
    __try {
        g_hideSnapshotValid = true;
        CVisibilityPlugins::SetClumpAlpha(g_hiddenClump, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OnPedRenderBefore: SetClumpAlpha SEH ex=0x%08X", GetExceptionCode());
        g_hideSnapshotValid = false;
        g_hiddenPed = nullptr;
        g_hiddenClump = nullptr;
    }
}

static void OnPedRenderAfter(CPed* ped) {
    OrcTextureRemapRestoreAfter();
    RestoreHeldWeaponReplacementAfter(ped);
    RestoreWeaponTextureOverrides();
    // Always try to finish previously captured hide snapshot, even if toggles changed.
    if (!g_hideSnapshotValid) return;
    if (!ped || ped != g_hiddenPed) return;

    __try {
        if (g_hiddenClump) CVisibilityPlugins::SetClumpAlpha(g_hiddenClump, 255);
        if (ped->m_pRwClump && ped->m_pRwClump != g_hiddenClump)
            CVisibilityPlugins::SetClumpAlpha(ped->m_pRwClump, 255);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // nothing
    }
    g_hideSnapshotValid = false;
    g_hiddenPed = nullptr;
    g_hiddenClump = nullptr;
}

static void OnShutdownRw() {
    OrcLogInfo("shutdownRw: releasing hooks and instances");
    overlay::Shutdown();
    samp_bridge::Shutdown();
    for (int i = 0; i < kMax; i++) g_rendered[i] = {};
    ClearAllOtherPeds();
    DestroyAllHeldWeaponReplacementInstances();
    DestroyWeaponReplacementAssets();
    DestroyWeaponTextureAssets();
    DestroyStandardSkinPreview();
    g_hideSnapshotValid = false;
    g_hiddenPed = nullptr;
    g_hiddenClump = nullptr;
    for (auto& o : g_customObjects) {
        DestroyCustomObjectInstance(o);
        o.txdSlot = -1;
    }
    DestroyAllStandardObjectInstances();
    for (auto& s : g_customSkins) {
        DestroyCustomSkinInstance(s);
        s.txdSlot = -1;
    }
    for (auto& s : g_standardSkins)
        DestroyStandardSkinInstance(s);
    DestroyAllRandomPoolSkins();
    OrcTextureRemapClearRuntimeState();
    // CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB @ 0x5D95B0 -> ret.
    DWORD oldProt;
    BYTE* p = reinterpret_cast<BYTE*>(0x5D95B0);
    if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
        *p = 0xC3;
        DWORD tmp; VirtualProtect(p, 1, oldProt, &tmp);
    }
}

// ----------------------------------------------------------------------------
// Plugin entry
// ----------------------------------------------------------------------------
class OrcOutFit {
public:
    OrcOutFit() {
        // LoadConfig откладываем до первого кадра: к этому моменту DllMain уже
        // проставит g_iniPath через LogInit().
        Events::initRwEvent.after += &OnInitRw;
        Events::drawingEvent += &OnDrawingEvent;
        g_standardSkinPreviewRenderEvent += &OnStandardSkinPreviewRenderEvent;
        Events::processScriptsEvent += &OrcTextureRemapOnProcessScripts;
        Events::pedSetModelEvent += &OrcTextureRemapOnPedSetModel;
        Events::pedRenderEvent.before += &OnPedRenderBefore;
        Events::pedRenderEvent.after += &OnPedRenderAfter;
        Events::d3dLostEvent += &OnD3dLost;
        Events::d3dResetEvent += &OnD3dReset;
        // При shutdownRwEvent сцена уже частично разобрана, RpClumpDestroy на
        // клонированных материалах падает в CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB
        // (env map plugin data ссылается на уже освобождённую текстуру).
        // Патчим сам деструктор на RET — безвредно т.к. процесс всё равно завершается.
        Events::shutdownRwEvent += &OnShutdownRw;
    }
} g_plugin;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        LogInit();
        OrcLogInfo("DllMain PROCESS_ATTACH ini=%s", g_iniPath);
        // Hook before first drawing frame: `LoadWeaponObject` runs during boot weapon.dat load.
        EnsureWeaponDatHookInstalled();
        EnsurePedDatHookInstalled();
        OrcTextureRemapInstallHooks();
    }
    return TRUE;
}
