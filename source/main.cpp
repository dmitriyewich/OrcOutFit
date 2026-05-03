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
#include "orc_render.h"
#include "orc_weapons.h"
#include "orc_path.h"
#include "orc_attach.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_runtime.h"
#include "external/MinHook/include/MinHook.h"

using namespace plugin;

static HMODULE g_module = nullptr;
char    g_iniPath[MAX_PATH] = {};
char    g_gameObjDir[MAX_PATH] = {};
char    g_gameWeaponsDir[MAX_PATH] = {};
char    g_gameSkinDir[MAX_PATH] = {};
char    g_gameTextureDir[MAX_PATH] = {};
char    g_gameWeaponGunsDir[MAX_PATH] = {};
char    g_gameWeaponGunsNickDir[MAX_PATH] = {};

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
    _snprintf_s(g_gameSkinDir, _TRUNCATE, "%s\\OrcOutFit\\Skins", moduleDir);
    _snprintf_s(g_gameTextureDir, _TRUNCATE, "%s\\OrcOutFit\\Skins\\Textures", moduleDir);

    std::srand(static_cast<unsigned>(GetTickCount()));
}

// ----------------------------------------------------------------------------
// Config: per-weapon attachment (типы и кости: orc_types.h)
// ----------------------------------------------------------------------------
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
bool g_weaponReplacementRandomIncludeVanilla = false;
bool g_weaponTexturesEnabled = true;
bool g_weaponTextureNickMode = true;
bool g_weaponTextureRandomMode = true;
bool g_weaponTextureStandardRemap = true;
bool g_weaponHudIconFromGunsTxd = true;
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

static bool g_pedDatHookInstalled = false;
static int(__cdecl* g_LoadPedObject_Orig)(const char* line) = nullptr;
std::vector<std::string> g_pedModelNameById; // modelId -> name
static std::vector<std::string> g_pedDatTxdById; // modelId -> txd

// CPed::AddWeaponModel / RemoveWeaponModel — GTA SA 1.0 US (plugin-sdk).
// Held weapon mesh is created here; hooking aligns replacement timing with the engine (SA:MP friendly).
static bool g_pedWeaponModelHooksInstalled = false;
using AddWeaponModel_t = void(__thiscall*)(CPed*, int);
using RemoveWeaponModel_t = void(__thiscall*)(CPed*, int);
static AddWeaponModel_t g_AddWeaponModel_Orig = nullptr;
static RemoveWeaponModel_t g_RemoveWeaponModel_Orig = nullptr;

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
    OrcWeaponsEnsureWeaponDatHookInstalled();
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
    g_weaponReplacementRandomIncludeVanilla =
        GetPrivateProfileIntA("Features", "WeaponReplacementRandomIncludeVanilla", 0, g_iniPath) != 0;
    g_weaponTexturesEnabled = GetPrivateProfileIntA("Features", "WeaponTextures", 1, g_iniPath) != 0;
    g_weaponTextureNickMode = GetPrivateProfileIntA("Features", "WeaponTextureNickMode", 1, g_iniPath) != 0;
    g_weaponTextureRandomMode = GetPrivateProfileIntA("Features", "WeaponTextureRandomMode", 1, g_iniPath) != 0;
    g_weaponTextureStandardRemap = GetPrivateProfileIntA("Features", "WeaponTextureStandardRemap", 1, g_iniPath) != 0;
    g_weaponHudIconFromGunsTxd = GetPrivateProfileIntA("Features", "WeaponHudIconFromGunsTxd", 1, g_iniPath) != 0;
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
    g_skinSelectedSource = (OrcToLowerAscii(selectedSource) == "standard") ? SKIN_SELECTED_STANDARD : SKIN_SELECTED_CUSTOM;
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
    AddIniInt(values, "Features", "WeaponReplacementRandomIncludeVanilla",
        g_weaponReplacementRandomIncludeVanilla ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextures", g_weaponTexturesEnabled ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextureNickMode", g_weaponTextureNickMode ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextureRandomMode", g_weaponTextureRandomMode ? 1 : 0);
    AddIniInt(values, "Features", "WeaponTextureStandardRemap", g_weaponTextureStandardRemap ? 1 : 0);
    AddIniInt(values, "Features", "WeaponHudIconFromGunsTxd", g_weaponHudIconFromGunsTxd ? 1 : 0);
    OrcAppendSkinFeatureIniValues(values);
    AddIniInt(values, "Features", "DebugLogLevel", static_cast<int>(g_orcLogLevel));
    AddIniInt(values, "Features", "DebugLog", (g_orcLogLevel >= OrcLogLevel::Info) ? 1 : 0);
    OrcAppendSkinModeIniValues(values);
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
          "; WeaponReplacementRandomIncludeVanilla=1: vanilla game weapon can be picked in Guns random pools.\n"
          "WeaponReplacementRandomIncludeVanilla=0\n"
          "WeaponTextures=1\n"
          "WeaponTextureNickMode=1\n"
          "WeaponTextureRandomMode=1\n"
          "WeaponTextureStandardRemap=1\n"
          "; HUD weapon icon uses `<weapon>icon` from Orc Guns texture / replacement dictionary when present (local player).\n"
          "WeaponHudIconFromGunsTxd=1\n"
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


static bool LoadWeaponOverridesFromIni(const char* fullIni, std::vector<WeaponCfg>* outCfg) {
    if (!fullIni || !outCfg) return false;
    if (!OrcFileExistsA(fullIni)) return false;
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
    if (!fullIni || !OrcFileExistsA(fullIni)) return any1;
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
    if (!fullPath || !fullPath[0] || !OrcFileExistsA(fullPath)) return;
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

bool ResolveWeaponsIniForSkinDff(const char* skinDffName, char* outPath, size_t outPathChars) {
    if (!skinDffName || !skinDffName[0] || !outPath || outPathChars == 0) return false;
    outPath[0] = 0;
    const std::string want = OrcToLowerAscii(std::string(skinDffName));
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

    std::string mask = OrcJoinPath(std::string(g_gameWeaponsDir), "*.ini");
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
        if (OrcLowerExt(fname) != ".ini") continue;
        if (OrcToLowerAscii(OrcBaseNameNoExt(fname)) == want) {
            foundPath = OrcJoinPath(std::string(g_gameWeaponsDir), fname);
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

const WeaponCfg& GetWeaponCfgForPed(CPed* ped, int wt) {
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
    const std::string key = OrcToLowerAscii(dff);
    EnsureWeaponSkinOverrideLoaded(key, wpath);
    auto it = g_weaponSkinOv1.find(key);
    if (it == g_weaponSkinOv1.end() || wt >= (int)it->second.size()) return g_cfg[wt];
    return it->second[wt];
}

const WeaponCfg& GetWeaponCfg2ForPed(CPed* ped, int wt) {
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
    const std::string key = OrcToLowerAscii(dff);
    EnsureWeaponSkinOverrideLoaded(key, wpath);
    auto it = g_weaponSkinOv2.find(key);
    if (it == g_weaponSkinOv2.end() || wt >= (int)it->second.size()) return g_cfg2[wt];
    return it->second[wt];
}

// Queued from UI; applied at the start of drawingEvent (see ApplyPendingLocalPlayerModel).
static int g_pendingLocalPedModelId = -1;

bool OrcApplyLocalPlayerModelById(int modelId) {
    if (!OrcIsValidStandardSkinModel(modelId)) return false;
    g_pendingLocalPedModelId = modelId;
    return true;
}

int OrcGetLocalPlayerModelId() {
    CPlayerPed* ped = FindPlayerPed(0);
    return ped ? (int)ped->m_nModelIndex : -1;
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
    AppendFormat(out, "WeaponReplacementRandomIncludeVanilla=%d\n",
        g_weaponReplacementRandomIncludeVanilla ? 1 : 0);
    AppendFormat(out, "WeaponTextures=%d\n", g_weaponTexturesEnabled ? 1 : 0);
    AppendFormat(out, "WeaponTextureNickMode=%d\n", g_weaponTextureNickMode ? 1 : 0);
    AppendFormat(out, "WeaponTextureRandomMode=%d\n", g_weaponTextureRandomMode ? 1 : 0);
    AppendFormat(out, "WeaponTextureStandardRemap=%d\n", g_weaponTextureStandardRemap ? 1 : 0);
    AppendFormat(out, "WeaponHudIconFromGunsTxd=%d\n", g_weaponHudIconFromGunsTxd ? 1 : 0);
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


static void ApplyPendingLocalPlayerModel() {
    if (g_pendingLocalPedModelId < 0) return;
    const int modelId = g_pendingLocalPedModelId;
    g_pendingLocalPedModelId = -1;

    CPlayerPed* p = FindPlayerPed(0);
    if (!p) {
        OrcLogError("ApplyPendingLocalPlayerModel: no local player ped");
        return;
    }

    if (!OrcIsValidStandardPedModelForLocalApply(modelId)) {
        OrcLogError("ApplyPendingLocalPlayerModel: model %d is missing or is not MODEL_INFO_PED", modelId);
        return;
    }

    OrcWeaponClearLocalRendered();

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
    OrcObjectsDestroyAllStandardInstances();
    OrcLogInfo("ApplyPendingLocalPlayerModel: set local ped model id=%d", modelId);
}




static void SyncAndRender() {
    if (!g_enabled) {
        OrcWeaponClearLocalRendered();
        OrcWeaponClearOtherPedsRendered();
        OrcRestoreWeaponHeldTextureOverrides();
        OrcDestroyAllHeldWeaponReplacementInstances();
        OrcRestoreWeaponTextureOverrides();
        OrcSkinsReleaseAllInstancesAndPreview();
        OrcObjectsReleaseAllInstances();
        OrcTextureRemapClearRuntimeState();
        return;
    }
    CPlayerPed* player = FindPlayerPed(0);
    if (!player) {
        OrcWeaponClearLocalRendered();
        OrcWeaponClearOtherPedsRendered();
        OrcRestoreWeaponHeldTextureOverrides();
        OrcDestroyAllHeldWeaponReplacementInstances();
        OrcRestoreWeaponTextureOverrides();
        OrcSkinsReleaseAllInstancesAndPreview();
        OrcObjectsReleaseAllInstances();
        OrcTextureRemapClearRuntimeState();
        return;
    }
    if (!g_weaponReplacementEnabled || !g_weaponReplacementInHands)
        OrcDestroyAllHeldWeaponReplacementInstances();
    else
        OrcPruneHeldWeaponReplacementInstances();

    std::vector<char> suppress;
    suppress.assign(g_cfg.size(), 0);
    OrcObjectsApplyWeaponSuppression(player, &suppress);
    OrcSyncPedWeapons(player, g_rendered, &suppress);
    std::vector<char> suppressPed;
    suppressPed.assign(g_cfg.size(), 0);
    std::vector<char> objectUsed;
    objectUsed.assign(g_customObjects.size(), 0);
    OrcObjectsBeginFrame();
    int active = 0;
    for (int i = 0; i < OrcWeaponSlotMax; i++) if (g_rendered[i].active) active++;
    OrcObjectsPrepassLocalPlayer(player, active, objectUsed);
    if (OrcSkinsLocalSelectionAddsActiveWork())
        active++;
    if (g_skinRandomFromPools && g_skinRandomPoolVariants > 0)
        active++;
    if (g_renderStandardObjects && !g_standardObjects.empty())
        active++;
    if (g_skinNickMode && samp_bridge::IsSampBuildKnown() && (!g_customSkins.empty() || !g_standardSkins.empty()))
        active++;
    if ((g_renderAllPedsWeapons || g_renderAllPedsObjects) && CPools::ms_pPedPool) active++;
    if (!active) {
        OrcObjectsWhenSkippingRenderNoActive();
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

    OrcRenderPedWeapons(player, g_rendered);
    OrcObjectsRenderLocalPlayer(player, objectUsed);
    OrcSkinsRenderForPeds(player);
    if ((g_renderAllPedsWeapons || g_renderAllPedsObjects) && CPools::ms_pPedPool) {
        if (!g_renderAllPedsWeapons) {
            OrcWeaponClearOtherPedsRendered();
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
                OrcObjectsApplyWeaponSuppression(ped, &suppressPed);
                OrcSyncPedWeapons(ped, cache.data(), &suppressPed);
                OrcRenderPedWeapons(ped, cache.data());
            }
            OrcObjectsRenderForRemotePed(ped, objectUsed);
        }
        if (g_renderAllPedsWeapons) {
            for (auto it = g_otherPedsRendered.begin(); it != g_otherPedsRendered.end();) {
                if (seen.find(it->first) == seen.end()) {
                    for (int i = 0; i < OrcWeaponSlotMax; i++) OrcDestroyRenderedWeapon(it->second[i]);
                    it = g_otherPedsRendered.erase(it);
                } else {
                    ++it;
                }
            }
        }
    } else {
        OrcWeaponClearOtherPedsRendered();
    }
    OrcObjectsFinalizeFrame(objectUsed);

    RwRenderStateSet(rwRENDERSTATECULLMODE,     reinterpret_cast<void*>(oldCull));
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE,  reinterpret_cast<void*>(oldZT));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(oldZW));
    RwRenderStateSet(rwRENDERSTATESHADEMODE,    reinterpret_cast<void*>(oldShade));
    RwRenderStateSet(rwRENDERSTATEFOGENABLE,    reinterpret_cast<void*>(oldFog));
}

static void OnInitRw() {}
static void OnDrawingEvent();
static void OnPedRenderBefore(CPed* ped);
static void OnPedRenderAfter(CPed* ped);
static void OnD3dLost() {
    OrcSkinsDestroyPreview();
    OrcRestoreWeaponTextureOverrides();
    OrcRestoreWeaponHeldTextureOverrides();
    OrcTextureRemapRestoreAfter();
    overlay::OnResetBefore();
}
static void OnD3dReset() {
    OrcSkinsDestroyPreview();
    OrcRestoreWeaponTextureOverrides();
    OrcRestoreWeaponHeldTextureOverrides();
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
    // `CHud::DrawAmmo` / `DrawWeaponIcon`: push Orc Guns/replacement TXD for `CSprite2d::SetTexture` during HUD draw.
    // for `SetTexture` + refresh intercept cache on `drawingEvent`.
    OrcWeaponHudRefreshSampSpriteInterceptCache();
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
    // Replacement must run first: PrepareHeldWeaponTextureBefore targets `m_pWeaponObject`.
    // If we textured the stock mesh then swapped the slot to the replacement clone,
    // the held model would render without Guns/GunsNick TXD / stock remap.
    OrcPrepareHeldWeaponReplacementBefore(ped);
    OrcPrepareHeldWeaponTextureBefore(ped);
    OrcSkinsOnPedRenderBefore(ped);
}

static void OnPedRenderAfter(CPed* ped) {
    OrcTextureRemapRestoreAfter();
    OrcRestoreHeldWeaponReplacementAfter(ped);
    // Held weapon RwMaterial swaps are deferred to EndScene (see OrcFlushDeferredHeldWeaponSlotRestore): this
    // hook runs before CPed draws the held mesh, so immediate restore would revert textures before GPU draw.
    OrcSkinsOnPedRenderAfter(ped);
}

static void OnShutdownRw() {
    OrcLogInfo("shutdownRw: releasing hooks and instances");
    overlay::Shutdown();
    samp_bridge::Shutdown();
    for (int i = 0; i < OrcWeaponSlotMax; i++) g_rendered[i] = {};
    OrcWeaponClearOtherPedsRendered();
    OrcDestroyAllHeldWeaponReplacementInstances();
    OrcWeaponAssetsShutdown();
    OrcSkinsShutdown();
    OrcObjectsShutdown();
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
        OrcSkinsRegisterPreviewHook();
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
        OrcWeaponsEnsureWeaponDatHookInstalled();
        EnsurePedDatHookInstalled();
        OrcWeaponEnsurePedModelHooksInstalled();
        OrcWeaponHudEnsureDrawWeaponIconHookInstalled();
        OrcTextureRemapInstallHooks();
    }
    return TRUE;
}


