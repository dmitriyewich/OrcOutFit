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
#include "CPools.h"
#include "CVisibilityPlugins.h"
#include "CPointLights.h"
#include "CTxdStore.h"
#include "CKeyGen.h"
#include "RenderWare.h"
#include "eWeaponType.h"
#include "game_sa/rw/rphanim.h"
#include "game_sa/rw/rpskin.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>

#include "overlay.h"
#include "samp_bridge.h"
#include "orc_types.h"
#include "orc_app.h"
#include "orc_ui.h"
#include "external/MinHook/include/MinHook.h"

using namespace plugin;

static HMODULE g_module = nullptr;
char    g_iniPath[MAX_PATH] = {};
char    g_gameObjDir[MAX_PATH] = {};
static char    g_gameObjOtherDir[MAX_PATH] = {};
char    g_gameSkinDir[MAX_PATH] = {};

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
    // Relative to plugin location (modloader-friendly):
    // <asi-dir>\OrcOutFit\object and <asi-dir>\OrcOutFit\SKINS
    _snprintf_s(g_gameObjDir, _TRUNCATE, "%s\\OrcOutFit\\object", moduleDir);
    _snprintf_s(g_gameObjOtherDir, _TRUNCATE, "%s\\OrcOutFit\\object\\other", moduleDir);
    _snprintf_s(g_gameSkinDir, _TRUNCATE, "%s\\OrcOutFit\\SKINS", moduleDir);

    std::srand(static_cast<unsigned>(GetTickCount()));
}

// ----------------------------------------------------------------------------
// Config: per-weapon attachment (типы и кости: orc_types.h)
// ----------------------------------------------------------------------------
static RpAtomic* InitAtomicCB(RpAtomic* a, void*);
bool g_enabled = true;
bool g_renderAllPedsWeapons = false;
float g_renderAllPedsRadius = 80.0f;
int  g_activationVk = VK_F7;
bool g_sampAllowActivationKey = false;
std::string g_toggleCommand = "/orcoutfit";
bool g_considerWeaponSkills = true;
std::vector<WeaponCfg> g_cfg;
std::vector<WeaponCfg> g_cfg2; // secondary dual-wield placement
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

    if (MH_Initialize() != MH_OK) return;
    if (MH_CreateHook(reinterpret_cast<void*>(0x5B3FB0),
                      reinterpret_cast<void*>(&LoadWeaponObject_Detour),
                      reinterpret_cast<void**>(&g_LoadWeaponObject_Orig)) != MH_OK) return;
    (void)MH_EnableHook(reinterpret_cast<void*>(0x5B3FB0));
}

static void EnsurePedDatHookInstalled() {
    if (g_pedDatHookInstalled) return;
    g_pedDatHookInstalled = true;
    g_pedModelNameById.clear();
    g_pedModelNameById.resize(1000); // grows on demand
    g_pedDatTxdById.clear();
    g_pedDatTxdById.resize(1000); // grows on demand

    // MinHook can be already initialized by other hooks.
    (void)MH_Initialize();
    if (MH_CreateHook(reinterpret_cast<void*>(0x5B7420),
                      reinterpret_cast<void*>(&LoadPedObject_Detour),
                      reinterpret_cast<void**>(&g_LoadPedObject_Orig)) != MH_OK) return;
    (void)MH_EnableHook(reinterpret_cast<void*>(0x5B7420));
}

static const char* TryGetPedModelNameById(int modelId) {
    if (modelId <= 0) return nullptr;
    if (modelId >= (int)g_pedModelNameById.size()) return nullptr;
    if (g_pedModelNameById[modelId].empty()) return nullptr;
    return g_pedModelNameById[modelId].c_str();
}

static std::string LowerAsciiLocal(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static bool TryParseModelIdFolder(const std::string& folder, int* outId) {
    if (!outId) return false;
    if (folder.empty()) return false;
    // Accept "217" or "id217"
    const std::string low = LowerAsciiLocal(folder);
    const char* p = low.c_str();
    if (low.rfind("id", 0) == 0) p += 2;
    if (!*p) return false;
    for (const char* t = p; *t; t++) if (*t < '0' || *t > '9') return false;
    const int v = atoi(p);
    if (v <= 0) return false;
    *outId = v;
    return true;
}

static CBaseModelInfo* ResolvePedModelInfoFromFolderName(const std::string& folderName, int* outModelId) {
    if (outModelId) *outModelId = -1;
    int modelId = -1;

    // 1) Vanilla path: model name is known by CModelInfo.
    CBaseModelInfo* mi = CModelInfo::GetModelInfo(folderName.c_str(), &modelId);
    if (mi && modelId >= 0) {
        if (outModelId) *outModelId = modelId;
        return mi;
    }

    // 2) Server/mod path: folder is model name known only from ped.dat load.
    const std::string want = LowerAsciiLocal(folderName);
    for (int id = 0; id < (int)g_pedModelNameById.size(); id++) {
        if (g_pedModelNameById[id].empty()) continue;
        if (LowerAsciiLocal(g_pedModelNameById[id]) == want) {
            mi = CModelInfo::GetModelInfo(id);
            if (mi) {
                if (outModelId) *outModelId = id;
                return mi;
            }
        }
    }

    // 3) Fallback: folder is id### / digits.
    int parsedId = -1;
    if (TryParseModelIdFolder(folderName, &parsedId)) {
        mi = CModelInfo::GetModelInfo(parsedId);
        if (mi) {
            if (outModelId) *outModelId = parsedId;
            return mi;
        }
    }

    return nullptr;
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    }
}
bool g_skinModeEnabled = false;
bool g_skinHideBasePed = true;
bool g_skinNickMode = true;
bool g_skinLocalPreferSelected = false;
bool g_skinRandomFromPools = false;
int g_skinRandomPoolModels = 0;
int g_skinRandomPoolVariants = 0;
std::string g_skinSelectedName;
static bool g_skinCanAnimate = false;
static int  g_skinBindCount = 0;

std::unordered_map<unsigned int, SkinOtherOverrides> g_otherByModelKey;
void DiscoverOtherOverridesAndObjects();

static void DestroyCustomObjectInstance(CustomObjectCfg& o);
static CustomSkinCfg* GetSelectedSkin();

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

static int ParseActivationVk(const char* text) {
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

static void RefreshActivationRouting() {
    overlay::SetToggleVirtualKey(g_activationVk);
    const bool hotkeyAllowed = !samp_bridge::IsSampBuildKnown() || g_sampAllowActivationKey;
    overlay::SetHotkeyEnabled(hotkeyAllowed);
}

void LoadConfig() {
    SetupDefaults();
    g_enabled = GetPrivateProfileIntA("Main", "Enabled", 1, g_iniPath) != 0;
    g_renderAllPedsWeapons = GetPrivateProfileIntA("Main", "RenderAllPedsWeapons", 0, g_iniPath) != 0;
    g_renderAllPedsRadius = (float)GetPrivateProfileIntA("Main", "RenderAllPedsRadius", 80, g_iniPath);
    g_considerWeaponSkills = GetPrivateProfileIntA("Main", "ConsiderWeaponSkills", 1, g_iniPath) != 0;
    g_sampAllowActivationKey = GetPrivateProfileIntA("Main", "SampAllowActivationKey", 0, g_iniPath) != 0;
    char keyBuf[32] = {};
    GetPrivateProfileStringA("Main", "ActivationKey", "F7", keyBuf, sizeof(keyBuf), g_iniPath);
    g_activationVk = ParseActivationVk(keyBuf);
    char cmdBuf[64] = {};
    GetPrivateProfileStringA("Main", "Command", "/orcoutfit", cmdBuf, sizeof(cmdBuf), g_iniPath);
    g_toggleCommand = cmdBuf;
    NormalizeCommand();
    if (g_renderAllPedsRadius < 5.0f) g_renderAllPedsRadius = 5.0f;
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
    g_skinModeEnabled = GetPrivateProfileIntA("SkinMode", "Enabled", 0, g_iniPath) != 0;
    g_skinHideBasePed = GetPrivateProfileIntA("SkinMode", "HideBasePed", 1, g_iniPath) != 0;
    g_skinNickMode = GetPrivateProfileIntA("SkinMode", "NickMode", 1, g_iniPath) != 0;
    g_skinLocalPreferSelected = GetPrivateProfileIntA("SkinMode", "LocalPreferSelected", 0, g_iniPath) != 0;
    g_skinRandomFromPools = GetPrivateProfileIntA("SkinMode", "RandomFromPools", 0, g_iniPath) != 0;
    char skinName[128] = {};
    GetPrivateProfileStringA("SkinMode", "Selected", "", skinName, sizeof(skinName), g_iniPath);
    g_skinSelectedName = skinName;
    RefreshActivationRouting();
    // Weapon types are discovered in SetupDefaults (InitWeaponTypesAndStorage).
}

static void SaveDefaultConfig() {
    if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = fopen(g_iniPath, "w");
    if (!f) return;
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
          "RenderAllPedsWeapons=0\n"
          "RenderAllPedsRadius=80\n"
          "ConsiderWeaponSkills=1\n"
          "ActivationKey=F7\n"
          "SampAllowActivationKey=0\n"
          "Command=/orcoutfit\n\n", f);
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
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE* t = fopen(g_iniPath, "w");
        if (!t) return;
        fclose(t);
    }
    char buf[16];
    _snprintf_s(buf, _TRUNCATE, "%d", g_considerWeaponSkills ? 1 : 0);
    WritePrivateProfileStringA("Main", "ConsiderWeaponSkills", buf, g_iniPath);
}

// ----------------------------------------------------------------------------
// Custom objects discovery (game folder) + per-object INI
// ----------------------------------------------------------------------------
std::vector<CustomObjectCfg> g_customObjects;
std::vector<CustomSkinCfg> g_customSkins;

struct SkinRandomPool {
    int modelId = -1;
    std::string folderName;
    std::vector<CustomSkinCfg> variants;
};

static std::vector<SkinRandomPool> g_skinRandomPools;
static std::unordered_map<CPed*, int> g_pedRandomSkinIdx;

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
    if (it == g_pedRandomSkinIdx.end()) {
        const int pick = rand() % n;
        g_pedRandomSkinIdx[ped] = pick;
        it = g_pedRandomSkinIdx.find(ped);
    }
    return &pool->variants[it->second];
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

static void CreateDefaultObjectIniIfMissing(const std::string& iniPath, const std::string& baseName) {
    if (FileExistsA(iniPath.c_str())) return;
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "; OrcOutFit custom object config for %s\n"
        "; Generated automatically from object folder scan.\n\n"
        "[Main]\n"
        "Enabled=1\n"
        "Bone=%d\n"
        "OffsetX=0.000\nOffsetY=0.000\nOffsetZ=0.000\n"
        "RotationX=0.0\nRotationY=0.0\nRotationZ=0.0\n"
        "Scale=1.000\n"
        "ScaleX=1.000\nScaleY=1.000\nScaleZ=1.000\n"
        "; Optional: render only when ped has weapon(s).\n"
        "; Weapons is a comma-separated list of weapon IDs (eWeaponType), e.g. 22,23.\n"
        "; If empty, object renders always.\n"
        "Weapons=\n"
        "; WeaponsMode: any|all\n"
        "WeaponsMode=any\n"
        "; If enabled, hide selected weapon(s) on body when object renders.\n"
        "HideWeapons=0\n",
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

static void LoadObjectCfgFromIni(CustomObjectCfg& o) {
    char buf[64];
    auto F = [&](const char* key, float def)->float{
        GetPrivateProfileStringA("Main", key, "", buf, sizeof(buf), o.iniPath.c_str());
        if (!buf[0]) return def;
        return (float)atof(buf);
    };
    o.enabled = GetPrivateProfileIntA("Main", "Enabled", o.enabled ? 1 : 0, o.iniPath.c_str()) != 0;
    o.boneId  = GetPrivateProfileIntA("Main", "Bone", o.boneId, o.iniPath.c_str());
    o.x = F("OffsetX", o.x);
    o.y = F("OffsetY", o.y);
    o.z = F("OffsetZ", o.z);
    o.rx = F("RotationX", o.rx / D2R) * D2R;
    o.ry = F("RotationY", o.ry / D2R) * D2R;
    o.rz = F("RotationZ", o.rz / D2R) * D2R;
    o.scale = F("Scale", o.scale);
    o.scaleX = F("ScaleX", o.scaleX);
    o.scaleY = F("ScaleY", o.scaleY);
    o.scaleZ = F("ScaleZ", o.scaleZ);

    char wcsv[256] = {};
    GetPrivateProfileStringA("Main", "Weapons", "", wcsv, sizeof(wcsv), o.iniPath.c_str());
    o.weaponTypes = ParseWeaponTypesCsv(wcsv);
    char mode[16] = {};
    GetPrivateProfileStringA("Main", "WeaponsMode", "any", mode, sizeof(mode), o.iniPath.c_str());
    o.weaponRequireAll = (ToLowerAscii(mode) == "all");
    o.hideSelectedWeapons = GetPrivateProfileIntA("Main", "HideWeapons", o.hideSelectedWeapons ? 1 : 0, o.iniPath.c_str()) != 0;
}

static void DestroySkinOtherOverrides(SkinOtherOverrides& so) {
    for (auto& o : so.objects) DestroyCustomObjectInstance(o);
    so.objects.clear();
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
    if (!FileExistsA(o.dffPath.c_str())) return false;
    if (o.txdPath.empty() || !FileExistsA(o.txdPath.c_str())) {
        if (!o.txdMissingLogged) o.txdMissingLogged = true;
        return false;
    }

    int txdSlot = CTxdStore::FindTxdSlot(o.name.c_str());
    if (txdSlot == -1) txdSlot = CTxdStore::AddTxdSlot(o.name.c_str());
    if (txdSlot == -1) return false;
    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)o.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        if (!o.txdMissingLogged) o.txdMissingLogged = true;
        return false;
    }
    o.txdSlot = txdSlot;
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);

    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)o.dffPath.c_str());
    if (!stream) { CTxdStore::PopCurrentTxd(); return false; }

    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* readClump = RpClumpStreamRead(stream);
        if (readClump) {
            o.rwObject = reinterpret_cast<RwObject*>(readClump);
            RpClumpForAllAtomics(readClump, InitAtomicCB, nullptr);
            ok = true;
        }
    }
    RwStreamClose(stream, nullptr);
    CTxdStore::PopCurrentTxd();
    return ok;
}

static bool EnsureCustomSkinLoaded(CustomSkinCfg& s) {
    if (s.rwObject) return true;
    if (!FileExistsA(s.dffPath.c_str())) return false;
    if (s.txdPath.empty() || !FileExistsA(s.txdPath.c_str())) {
        if (!s.txdMissingLogged) s.txdMissingLogged = true;
        return false;
    }

    int txdSlot = CTxdStore::FindTxdSlot(s.name.c_str());
    if (txdSlot == -1) txdSlot = CTxdStore::AddTxdSlot(s.name.c_str());
    if (txdSlot == -1) return false;
    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)s.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) return false;
    s.txdSlot = txdSlot;

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)s.dffPath.c_str());
    if (!stream) { CTxdStore::PopCurrentTxd(); return false; }
    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* c = RpClumpStreamRead(stream);
        if (c) {
            s.rwObject = reinterpret_cast<RwObject*>(c);
            RpClumpForAllAtomics(c, InitAtomicCB, nullptr);
            ok = true;
        }
    }
    RwStreamClose(stream, nullptr);
    CTxdStore::PopCurrentTxd();
    return ok;
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
    FILE* f = fopen(s.iniPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "; OrcOutFit custom skin config for %s\n"
        "; Nicks: one per line and/or comma-separated (case-insensitive).\n\n"
        "[NickBinding]\n"
        "Enabled=%d\n"
        "Nicks=",
        s.name.c_str(), s.bindToNick ? 1 : 0);
    fputs(s.nickListCsv.c_str(), f);
    fputc('\n', f);
    fclose(f);
}

void DiscoverCustomObjectsAndEnsureIni() {
    for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
    g_customObjects.clear();

    DWORD attr = GetFileAttributesA(g_gameObjDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

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
        CreateDefaultObjectIniIfMissing(o.iniPath, base);
        LoadObjectCfgFromIni(o);
        // if (o.txdPath.empty()) — тихий пропуск, без лога
        g_customObjects.push_back(o);
        foundDff++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void DiscoverRandomSkinPools(const std::string& skinRootDir) {
    const std::string rndRoot = JoinPath(skinRootDir, "random");
    DWORD ra = GetFileAttributesA(rndRoot.c_str());
    if (ra == INVALID_FILE_ATTRIBUTES || !(ra & FILE_ATTRIBUTE_DIRECTORY))
        return;

    std::string mask = JoinPath(rndRoot, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        const std::string folderName = fd.cFileName;
        int modelId = -1;
        CBaseModelInfo* mi = ResolvePedModelInfoFromFolderName(folderName, &modelId);
        if (!mi || modelId < 0) continue;
        if (mi->GetModelType() != MODEL_INFO_PED) continue;
        const std::string subDir = JoinPath(rndRoot, folderName);
        SkinRandomPool pool;
        pool.folderName = folderName;
        pool.modelId = modelId;

        std::string dmask = JoinPath(subDir, "*.*");
        WIN32_FIND_DATAA dfd{};
        HANDLE dh = FindFirstFileA(dmask.c_str(), &dfd);
        if (dh == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (dfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::string fname = dfd.cFileName;
            if (LowerExt(fname) != ".dff")
                continue;
            const std::string dffBase = BaseNameNoExt(fname);
            CustomSkinCfg s;
            char uniq[160];
            _snprintf_s(uniq, _TRUNCATE, "rnd_%s_%s", folderName.c_str(), dffBase.c_str());
            s.name = uniq;
            s.dffPath = JoinPath(subDir, dffBase + ".dff");
            s.txdPath = FindBestTxdPath(subDir, dffBase);
            s.iniPath = JoinPath(subDir, dffBase + ".ini");
            CreateDefaultSkinIniIfMissing(s.iniPath, dffBase);
            LoadSkinCfgFromIni(s);
            pool.variants.push_back(std::move(s));
        } while (FindNextFileA(dh, &dfd));
        FindClose(dh);

        if (pool.variants.empty()) continue;
        g_skinRandomPoolVariants += (int)pool.variants.size();
        g_skinRandomPoolModels++;
        g_skinRandomPools.push_back(std::move(pool));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void DiscoverCustomSkins() {
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    g_customSkins.clear();
    DestroyAllRandomPoolSkins();
    DWORD attr = GetFileAttributesA(g_gameSkinDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
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
        CreateDefaultSkinIniIfMissing(s.iniPath, base);
        LoadSkinCfgFromIni(s);
        g_customSkins.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    DiscoverRandomSkinPools(dir);
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

void SaveCustomObjectIni(const CustomObjectCfg& o) {
    auto W = [&](const char* key, const char* v) {
        WritePrivateProfileStringA("Main", key, v, o.iniPath.c_str());
    };
    char buf[64];
    _snprintf_s(buf, _TRUNCATE, "%d", o.enabled ? 1 : 0); W("Enabled", buf);
    _snprintf_s(buf, _TRUNCATE, "%d", o.boneId);          W("Bone", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.x);             W("OffsetX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.y);             W("OffsetY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.z);             W("OffsetZ", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", o.rx / D2R);      W("RotationX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", o.ry / D2R);      W("RotationY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", o.rz / D2R);      W("RotationZ", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.scale);         W("Scale", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.scaleX);        W("ScaleX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.scaleY);        W("ScaleY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", o.scaleZ);        W("ScaleZ", buf);

    char wcsv[256];
    WeaponTypesToCsv(o.weaponTypes, wcsv, sizeof(wcsv));
    W("Weapons", wcsv);
    W("WeaponsMode", o.weaponRequireAll ? "all" : "any");
    _snprintf_s(buf, _TRUNCATE, "%d", o.hideSelectedWeapons ? 1 : 0); W("HideWeapons", buf);
}

void SaveSkinModeIni() {
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE* t = fopen(g_iniPath, "w");
        if (!t) return;
        fclose(t);
    }
    char buf[64];
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinModeEnabled ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "Enabled", buf, g_iniPath);
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinHideBasePed ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "HideBasePed", buf, g_iniPath);
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinNickMode ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "NickMode", buf, g_iniPath);
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinLocalPreferSelected ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "LocalPreferSelected", buf, g_iniPath);
    WritePrivateProfileStringA("SkinMode", "Selected", g_skinSelectedName.c_str(), g_iniPath);
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinRandomFromPools ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "RandomFromPools", buf, g_iniPath);
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

static const WeaponCfg& GetWeaponCfgForPed(CPed* ped, int wt) {
    if (!ped || wt < 0 || wt >= (int)g_cfg.size()) return g_cfg[0];
    CPlayerPed* local = FindPlayerPed(0);
    if (local && ped == local) {
        auto* mi = CModelInfo::GetModelInfo(ped->m_nModelIndex);
        if (mi) {
            auto it = g_otherByModelKey.find(mi->m_nKey);
            if (it != g_otherByModelKey.end() && it->second.hasWeaponOverrides) {
                if (wt >= 0 && wt < (int)it->second.weaponCfg.size())
                    return it->second.weaponCfg[wt];
            }
        }
    }
    return g_cfg[wt];
}

static const WeaponCfg& GetWeaponCfg2ForPed(CPed* ped, int wt) {
    if (!ped || wt < 0 || wt >= (int)g_cfg2.size()) return g_cfg2[0];
    CPlayerPed* local = FindPlayerPed(0);
    if (local && ped == local) {
        auto* mi = CModelInfo::GetModelInfo(ped->m_nModelIndex);
        if (mi) {
            auto it = g_otherByModelKey.find(mi->m_nKey);
            if (it != g_otherByModelKey.end() && it->second.hasWeaponOverrides) {
                if (wt >= 0 && wt < (int)it->second.weaponCfg2.size())
                    return it->second.weaponCfg2[wt];
            }
        }
    }
    return g_cfg2[wt];
}

void DiscoverOtherOverridesAndObjects() {
    for (auto& kv : g_otherByModelKey) DestroySkinOtherOverrides(kv.second);
    g_otherByModelKey.clear();

    DWORD attr = GetFileAttributesA(g_gameObjOtherDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    std::string root = g_gameObjOtherDir;
    std::string mask = JoinPath(root, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    int modelFoldersFound = 0;
    int objFound = 0;
    int weaponIniFound = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        const std::string folderName = fd.cFileName;
        const std::string skinDir = JoinPath(root, folderName);
        SkinOtherOverrides so;
        so.skinName = folderName; // folder name = standard ped model name (or id###)
        so.weaponsIniPath = JoinPath(skinDir, "weapons.ini");

        so.hasWeaponOverrides = LoadWeaponOverridesFromIni2(so.weaponsIniPath.c_str(), &so.weaponCfg, &so.weaponCfg2);
        if (so.hasWeaponOverrides) weaponIniFound++;

        std::string dmask = JoinPath(skinDir, "*.*");
        WIN32_FIND_DATAA dfd{};
        HANDLE dh = FindFirstFileA(dmask.c_str(), &dfd);
        if (dh != INVALID_HANDLE_VALUE) {
            do {
                if (dfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string fname = dfd.cFileName;
                if (LowerExt(fname) != ".dff") continue;
                const std::string base = BaseNameNoExt(fname);
                CustomObjectCfg o;
                o.name = base;
                o.dffPath = JoinPath(skinDir, base + ".dff");
                o.txdPath = FindBestTxdPath(skinDir, base);
                o.iniPath = JoinPath(skinDir, base + ".ini");
                CreateDefaultObjectIniIfMissing(o.iniPath, base);
                LoadObjectCfgFromIni(o);
                        // if (o.txdPath.empty()) — тихий пропуск, без лога
                so.objects.push_back(std::move(o));
                objFound++;
            } while (FindNextFileA(dh, &dfd));
            FindClose(dh);
        }

        unsigned int modelKey = 0;
        // Allow folder names like "217" or "id217" for servers/SP mods that add skins by id.
        int parsedId = -1;
        const char* s = folderName.c_str();
        if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'd' || s[1] == 'D')) s += 2;
        bool allDigits = (*s != 0);
        for (const char* p = s; *p; ++p) if (*p < '0' || *p > '9') { allDigits = false; break; }
        if (allDigits) parsedId = atoi(s);

        if (parsedId >= 0) {
            CBaseModelInfo* mi = CModelInfo::GetModelInfo(parsedId);
            if (mi) modelKey = mi->m_nKey;
        }
        if (!modelKey) {
            int modelId = -1;
            CBaseModelInfo* mi = CModelInfo::GetModelInfo(folderName.c_str(), &modelId);
            if (mi) modelKey = mi->m_nKey;
        }
        if (!modelKey) modelKey = CKeyGen::GetUppercaseKey(folderName.c_str());
        g_otherByModelKey[modelKey] = std::move(so);
        modelFoldersFound++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
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

    char buf[32];
    auto W = [&](const char* key, const char* v) {
        WritePrivateProfileStringA(sec, key, v, g_iniPath);
        if (lstrcmpiA(sec, secNum) != 0) WritePrivateProfileStringA(secNum, key, v, g_iniPath);
    };
    _snprintf_s(buf, _TRUNCATE, "%d", c.enabled ? 1 : 0);     W("Enabled", buf);
    _snprintf_s(buf, _TRUNCATE, "%d", c.boneId);              W("Bone", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.x);                 W("OffsetX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.y);                 W("OffsetY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.z);                 W("OffsetZ", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", c.rx / D2R);          W("RotationX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", c.ry / D2R);          W("RotationY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", c.rz / D2R);          W("RotationZ", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.scale);             W("Scale", buf);
}

void SaveWeaponSection2(int wt) {
    if (wt <= 0 || wt >= (int)g_cfg2.size()) return;
    auto& c = g_cfg2[wt];
    char sec[96];
    if (g_cfg[wt].name) _snprintf_s(sec, _TRUNCATE, "%s2", g_cfg[wt].name);
    else               _snprintf_s(sec, _TRUNCATE, "Weapon%d_2", wt);
    char secNum[32];
    _snprintf_s(secNum, _TRUNCATE, "Weapon%d_2", wt);

    char buf[32];
    auto W = [&](const char* key, const char* v) {
        WritePrivateProfileStringA(sec, key, v, g_iniPath);
        if (lstrcmpiA(sec, secNum) != 0) WritePrivateProfileStringA(secNum, key, v, g_iniPath);
    };
    _snprintf_s(buf, _TRUNCATE, "%d", c.enabled ? 1 : 0);     W("Enabled", buf);
    _snprintf_s(buf, _TRUNCATE, "%d", c.boneId);              W("Bone", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.x);                 W("OffsetX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.y);                 W("OffsetY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.z);                 W("OffsetZ", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", c.rx / D2R);          W("RotationX", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", c.ry / D2R);          W("RotationY", buf);
    _snprintf_s(buf, _TRUNCATE, "%.2f", c.rz / D2R);          W("RotationZ", buf);
    _snprintf_s(buf, _TRUNCATE, "%.3f", c.scale);             W("Scale", buf);
}

SkinOtherOverrides* EnsureOtherOverridesForLocalSkin() {
    CPlayerPed* player = FindPlayerPed(0);
    if (!player) return nullptr;

    CBaseModelInfo* mi = CModelInfo::GetModelInfo(player->m_nModelIndex);
    if (!mi) return nullptr;

    const unsigned int modelKey = mi->m_nKey;
    auto it = g_otherByModelKey.find(modelKey);
    if (it != g_otherByModelKey.end()) return &it->second;

    // Folder name should match standard ped skin name (e.g. wmyclot).
    // If name is unavailable (common in SA:MP), fallback to "id<modelId>".
    std::string skinName;
    CPlayerInfo* pi = player->GetPlayerInfoForThisPlayerPed();
    if (pi && pi->m_szSkinName[0]) skinName = pi->m_szSkinName;

    if (skinName.empty()) {
        // Prefer model name captured from ped.dat load.
        if (const char* nm = TryGetPedModelNameById((int)player->m_nModelIndex)) {
            skinName = nm;
        }
    }

    if (skinName.empty()) {
        char tmp[32];
        _snprintf_s(tmp, _TRUNCATE, "id%d", (int)player->m_nModelIndex);
        skinName = tmp;
    }

    SkinOtherOverrides so;
    so.skinName = skinName;
    const std::string dirPath = std::string(g_gameObjOtherDir) + "\\" + skinName;
    so.weaponsIniPath = dirPath + "\\weapons.ini";

    so.hasWeaponOverrides = true; // we want runtime to use per-skin overrides immediately
    so.weaponCfg = g_cfg;
    so.weaponCfg2 = g_cfg2;

    auto [newIt, ok] = g_otherByModelKey.emplace(modelKey, std::move(so));
    (void)ok;
    return &newIt->second;
}

void SaveOtherSkinWeaponsIni(const SkinOtherOverrides& so) {
    if (so.weaponsIniPath.empty()) return;

    // Create directory on demand (last level: skin folder).
    const size_t pos = so.weaponsIniPath.find_last_of("\\/");
    if (pos != std::string::npos) {
        const std::string dir = so.weaponsIniPath.substr(0, pos);
        if (!dir.empty()) CreateDirectoryA(dir.c_str(), nullptr);
    }

    FILE* f = fopen(so.weaponsIniPath.c_str(), "w");
    if (!f) return;

    fprintf(f,
        "; OrcOutFit weapon overrides for %s\n"
        "; Generated/edited via UI.\n\n",
        so.skinName.c_str());
    fclose(f);

    char sec[32];
    char buf[64];
    for (int wt = 1; wt < (int)so.weaponCfg.size(); wt++) {
        const WeaponCfg& c = so.weaponCfg[wt];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d", wt);

        _snprintf_s(buf, _TRUNCATE, "%d", c.enabled ? 1 : 0);
        WritePrivateProfileStringA(sec, "Enabled", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%d", c.boneId);
        WritePrivateProfileStringA(sec, "Bone", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.x);
        WritePrivateProfileStringA(sec, "OffsetX", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.y);
        WritePrivateProfileStringA(sec, "OffsetY", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.z);
        WritePrivateProfileStringA(sec, "OffsetZ", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.2f", c.rx / D2R);
        WritePrivateProfileStringA(sec, "RotationX", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.2f", c.ry / D2R);
        WritePrivateProfileStringA(sec, "RotationY", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.2f", c.rz / D2R);
        WritePrivateProfileStringA(sec, "RotationZ", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.scale);
        WritePrivateProfileStringA(sec, "Scale", buf, so.weaponsIniPath.c_str());
    }
    for (int wt = 1; wt < (int)so.weaponCfg2.size(); wt++) {
        const WeaponCfg& c = so.weaponCfg2[wt];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d_2", wt);
        _snprintf_s(buf, _TRUNCATE, "%d", c.enabled ? 1 : 0);
        WritePrivateProfileStringA(sec, "Enabled", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%d", c.boneId);
        WritePrivateProfileStringA(sec, "Bone", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.x);
        WritePrivateProfileStringA(sec, "OffsetX", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.y);
        WritePrivateProfileStringA(sec, "OffsetY", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.z);
        WritePrivateProfileStringA(sec, "OffsetZ", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.2f", c.rx / D2R);
        WritePrivateProfileStringA(sec, "RotationX", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.2f", c.ry / D2R);
        WritePrivateProfileStringA(sec, "RotationY", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.2f", c.rz / D2R);
        WritePrivateProfileStringA(sec, "RotationZ", buf, so.weaponsIniPath.c_str());
        _snprintf_s(buf, _TRUNCATE, "%.3f", c.scale);
        WritePrivateProfileStringA(sec, "Scale", buf, so.weaponsIniPath.c_str());
    }
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
    if (!r.rwObject) { r.active = false; return; }
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

static void ClearAll() {
    for (int i = 0; i < kMax; i++) DestroyRendered(g_rendered[i]);
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
    if (!ped) return nullptr;
    // drawingEvent срабатывает уже после CPed::Render, значит RpHAnim обновлён.
    RpHAnimHierarchy* h = GetAnimHierarchyFromSkinClump(ped->m_pRwClump);
    if (!h) return nullptr;
    RwInt32 id = RpHAnimIDGetIndex(h, boneNodeId);
    if (id < 0) return nullptr;
    return &h->pMatrixArray[id];
}

// ----------------------------------------------------------------------------
// Render callbacks (из BaseModelRender::ClumpsForAtomic / GeometryForMaterials)
// ----------------------------------------------------------------------------
// Whitens material color — в паре с rpGEOMETRYMODULATEMATERIALCOLOR даёт
// чистое lighting * 1.0 без родного тинта оружия.
static RpMaterial* WhiteMatCB(RpMaterial* m, void*) {
    m->color = { 255, 255, 255, 255 };
    return m;
}

// Per-frame prep: форсим modulate-флаг и белый цвет материалов перед рендером.
static RpAtomic* PrepAtomicCB(RpAtomic* a, void*) {
    if (a->geometry) {
        a->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        RpGeometryForAllMaterials(a->geometry, WhiteMatCB, nullptr);
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
    CVisibilityPlugins::SetAtomicRenderCallback(a, nullptr);
    if (a->geometry) {
        RpGeometryForAllMaterials(a->geometry,
            +[](RpMaterial* m, void*)->RpMaterial*{ if (m) m->refCount++; return m; },
            nullptr);
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

    RwMatrix* bone = GetBoneMatrix(ped, wc.boneId);
    if (!bone) return false;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));

    RwObject* inst = mi->CreateInstance(&mtx);
    if (!inst) return false;

    // Сброс render-callback на дефолтный RW + leak материалов (см. InitAtomicCB).
    if (inst->type == rpCLUMP) {
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), InitAtomicCB, nullptr);
    } else if (inst->type == rpATOMIC) {
        InitAtomicCB(reinterpret_cast<RpAtomic*>(inst), nullptr);
    }

    int fi = FindFree(arr);
    if (fi < 0) {
        if (inst->type == rpCLUMP) RpClumpDestroy(reinterpret_cast<RpClump*>(inst));
        return false;
    }
    arr[fi] = { true, wt, secondary, mid, 0, inst };

    static std::unordered_set<int> logged;
    if (!secondary && logged.insert(wt).second) {
    }
    (void)slot;
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
    float lightOut = 0.0f;
    float light = CPointLights::GenerateLightsAffectingObject(&lightPos, &lightOut, nullptr) * 0.5f;
    SetLightColoursForPedsCarsAndObjects(light);

    if (r.rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(r.rwObject);
        RpClumpForAllAtomics(clump, PrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        PrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
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

static bool ShouldRenderObjectForPed(CPed* ped, const CustomObjectCfg& o) {
    if (!o.enabled || o.boneId == 0) return false;
    if (o.weaponTypes.empty()) return true; // no filter -> always render

    if (o.weaponRequireAll) {
        for (int wt : o.weaponTypes) {
            if (!PedHasWeaponType(ped, wt)) return false;
        }
        return true;
    }

    for (int wt : o.weaponTypes) {
        if (PedHasWeaponType(ped, wt)) return true;
    }
    return false;
}

static void ApplyObjectWeaponSuppression(CPed* ped, const std::vector<CustomObjectCfg>& objs, std::vector<char>* suppress) {
    if (!ped || !suppress) return;
    for (const auto& o : objs) {
        if (!o.hideSelectedWeapons) continue;
        if (o.weaponTypes.empty()) continue; // nothing selected -> do not suppress anything
        if (!ShouldRenderObjectForPed(ped, o)) continue;
        for (int wt : o.weaponTypes) {
            if (wt <= 0 || wt >= (int)suppress->size()) continue;
            (*suppress)[wt] = 1;
        }
    }
}

static bool EnsureCustomInstance(CustomObjectCfg& o, CPed* ped) {
    if (!o.enabled || o.boneId == 0) {
        DestroyCustomObjectInstance(o);
        return false;
    }
    if (!EnsureCustomModelLoaded(o)) return false;
    (void)ped;
    return o.rwObject != nullptr;
}

static void RenderCustomObject(CPed* ped, CustomObjectCfg& o) {
    if (!o.rwObject) return;
    RwMatrix* bone = GetBoneMatrix(ped, o.boneId);
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
    ApplyOffset(&mtx, o.x, o.y, o.z);
    RotateMatrix(&mtx, o.rx, o.ry, o.rz);
    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    const float sx = o.scale * o.scaleX;
    const float sy = o.scale * o.scaleY;
    const float sz = o.scale * o.scaleZ;
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        RwV3d s = { sx, sy, sz };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    float lightOut = 0.0f;
    float light = CPointLights::GenerateLightsAffectingObject(&lightPos, &lightOut, nullptr) * 0.5f;
    SetLightColoursForPedsCarsAndObjects(light);

    if (o.rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(o.rwObject);
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
        for (auto& s : g_customSkins) {
            if (ToLowerAscii(s.name) == ToLowerAscii(g_skinSelectedName)) return &s;
        }
    }
    if (g_uiSkinIdx < 0 || g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
    g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
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

static CustomSkinCfg* FindNickSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    for (auto& s : g_customSkins) {
        if (SkinMatchesNickname(s, nickLower)) return &s;
    }
    for (auto& pool : g_skinRandomPools) {
        for (auto& s : pool.variants) {
            if (SkinMatchesNickname(s, nickLower)) return &s;
        }
    }
    return nullptr;
}

static CustomSkinCfg* ResolveSkinForPed(CPed* ped, CPlayerPed* localPlayer, bool* isLocalPedOut) {
    if (isLocalPedOut) *isLocalPedOut = false;
    if (!ped || !g_skinModeEnabled) return nullptr;
    CustomSkinCfg* selected = GetSelectedSkin();
    const bool isLocalByPtr = (localPlayer && ped == localPlayer);

    if (g_skinNickMode && samp_bridge::IsSampBuildKnown()) {
        char nick[32] = {};
        bool isLocalBySamp = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocalBySamp)) {
            const bool isLocal = isLocalBySamp || isLocalByPtr;
            if (isLocalPedOut) *isLocalPedOut = isLocal;
            CustomSkinCfg* nickSkin = FindNickSkin(ToLowerAscii(nick));
            // Ник всегда выше «выбранного скина» и рандом-пулов.
            if (nickSkin) return nickSkin;
            if (isLocal) {
                if (g_skinLocalPreferSelected && selected) return selected;
                if (selected) return selected;
            }
        }
    }

    if (g_skinRandomFromPools) {
        if (CustomSkinCfg* rnd = ResolveRandomSkinForPed(ped))
            return rnd;
    }

    if (isLocalByPtr) {
        if (isLocalPedOut) *isLocalPedOut = true;
        return selected;
    }
    return nullptr;
}

static void RenderSkinOnPed(CPed* ped, CustomSkinCfg* sel, bool isLocalPed) {
    if (!ped || !ped->m_pRwClump || !sel) return;
    RpClump* pedClump = ped->m_pRwClump;
    if (!EnsureCustomSkinLoaded(*sel)) return;
    if (!sel->rwObject || sel->rwObject->type != rpCLUMP) return;
    RpClump* clump = reinterpret_cast<RpClump*>(sel->rwObject);
    RwFrame* srcFrame = RpClumpGetFrame(pedClump);
    RwFrame* dstFrame = RpClumpGetFrame(clump);
    if (!srcFrame || !dstFrame) return;
    std::memcpy(RwFrameGetMatrix(dstFrame), RwFrameGetMatrix(srcFrame), sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(dstFrame));
    RpHAnimHierarchy* srcH = GetAnimHierarchyFromSkinClump(pedClump);
    g_skinBindCount = 0;
    if (srcH) RpClumpForAllAtomics(clump, BindSkinHierarchyCB, srcH);
    const bool canAnimate = (srcH && g_skinBindCount > 0);
    if (isLocalPed) g_skinCanAnimate = canAnimate;
    if (canAnimate) {
        if (ped->m_pRwClump != pedClump) return;
        CopySkinHierarchyPose(ped, clump);
    }
    RwFrameUpdateObjects(dstFrame);
    // Explicit lighting for custom skin. Without this, when no weapon/object render
    // runs before, lighting state may stay dark and skin appears black.
    const CVector& p = ped->GetPosition();
    CVector lightPos = { p.x, p.y, p.z };
    float lightOut = 0.0f;
    float light = CPointLights::GenerateLightsAffectingObject(&lightPos, &lightOut, nullptr) * 0.5f;
    SetLightColoursForPedsCarsAndObjects(light);
    RpClumpForAllAtomics(clump, PrepAtomicCB, nullptr);
    RpClumpRender(clump);
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
        const bool keep = (wt >= 0 && wt < maxWt) && (arr[i].secondary ? want2[wt] : want[wt]);
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
    if (!localPlayer || !g_skinModeEnabled) return;
    g_skinCanAnimate = false;
    if (g_skinRandomFromPools)
        PrunePedRandomSkinMap();
    bool localDone = false;
    if (!CPools::ms_pPedPool) return;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; i++) {
        CPed* ped = CPools::ms_pPedPool->GetAt(i);
        if (!ped || !ped->m_pRwClump) continue;
        bool isLocal = false;
        CustomSkinCfg* skin = ResolveSkinForPed(ped, localPlayer, &isLocal);
        if (!skin) continue;
        RenderSkinOnPed(ped, skin, isLocal);
        if (isLocal) localDone = true;
    }
    if (!localDone) {
        bool isLocal = true;
        CustomSkinCfg* skin = ResolveSkinForPed(localPlayer, localPlayer, &isLocal);
        if (skin) RenderSkinOnPed(localPlayer, skin, true);
    }
}

static void SyncAndRender() {
    if (!g_enabled) {
        ClearAll();
        ClearAllOtherPeds();
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
        for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
        for (auto& kv : g_otherByModelKey) DestroySkinOtherOverrides(kv.second);
        DestroyAllRandomPoolSkins();
        return;
    }
    CPlayerPed* player = FindPlayerPed(0);
    if (!player) {
        ClearAll();
        ClearAllOtherPeds();
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
        for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
        for (auto& kv : g_otherByModelKey) DestroySkinOtherOverrides(kv.second);
        DestroyAllRandomPoolSkins();
        return;
    }

    std::vector<char> suppress;
    suppress.assign(g_cfg.size(), 0);
    ApplyObjectWeaponSuppression(player, g_customObjects, &suppress);
    {
        CBaseModelInfo* mi = CModelInfo::GetModelInfo(player->m_nModelIndex);
        if (mi) {
            auto it = g_otherByModelKey.find(mi->m_nKey);
            if (it != g_otherByModelKey.end())
                ApplyObjectWeaponSuppression(player, it->second.objects, &suppress);
        }
    }
    SyncPedWeapons(player, g_rendered, &suppress);
    int active = 0;
    for (int i = 0; i < kMax; i++) if (g_rendered[i].active) active++;
    for (auto& o : g_customObjects) if (ShouldRenderObjectForPed(player, o) && EnsureCustomInstance(o, player)) active++;
    // object\other overrides are also rendered (regardless of Skin mode),
    // so they must be included into the early-out `active` check.
    {
        CBaseModelInfo* mi = CModelInfo::GetModelInfo(player->m_nModelIndex);
        if (mi) {
            auto it = g_otherByModelKey.find(mi->m_nKey);
            if (it != g_otherByModelKey.end()) {
                for (auto& o : it->second.objects) {
                    if (!ShouldRenderObjectForPed(player, o)) continue;
                    if (EnsureCustomInstance(o, player)) active++;
                }
            }
        }
    }
    if (g_skinModeEnabled) {
        bool needSkinPass = GetSelectedSkin() != nullptr;
        if (!needSkinPass && g_skinRandomFromPools) {
            for (const auto& p : g_skinRandomPools) {
                if (!p.variants.empty()) {
                    needSkinPass = true;
                    break;
                }
            }
        }
        if (needSkinPass) active++;
    }
    if (g_skinNickMode && samp_bridge::IsSampBuildKnown()
        && (!g_customSkins.empty() || g_skinRandomPoolVariants > 0))
        active++;
    if (g_renderAllPedsWeapons && CPools::ms_pPedPool) active++;
    if (!active) return;

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
    for (auto& o : g_customObjects) {
        if (!ShouldRenderObjectForPed(player, o)) { DestroyCustomObjectInstance(o); continue; }
        EnsureCustomInstance(o, player);
        RenderCustomObject(player, o);
    }
    {
        auto* mi = CModelInfo::GetModelInfo(player->m_nModelIndex);
        if (mi) {
            auto it = g_otherByModelKey.find(mi->m_nKey);
            if (it != g_otherByModelKey.end()) {
                for (auto& o : it->second.objects) {
                    if (!ShouldRenderObjectForPed(player, o)) { DestroyCustomObjectInstance(o); continue; }
                    EnsureCustomInstance(o, player);
                    RenderCustomObject(player, o);
                }
            }
        }
    }
    RenderSkinsForPeds(player);
    if (g_renderAllPedsWeapons && CPools::ms_pPedPool) {
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
            seen.insert(h);
            auto& cache = g_otherPedsRendered[h];
            SyncPedWeapons(ped, cache.data());
            RenderPedWeapons(ped, cache.data());
        }
        for (auto it = g_otherPedsRendered.begin(); it != g_otherPedsRendered.end();) {
            if (seen.find(it->first) == seen.end()) {
                for (int i = 0; i < kMax; i++) DestroyRendered(it->second[i]);
                it = g_otherPedsRendered.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        ClearAllOtherPeds();
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
static void OnD3dReset() { overlay::OnResetBefore(); overlay::OnResetAfter(); }
static void OnShutdownRw();

static void OnDrawingEvent() {
    static bool inited = false;
    if (!inited) {
        inited = true;
        __try {
            SetupDefaults();
            SaveDefaultConfig();
            LoadConfig();
            DiscoverCustomObjectsAndEnsureIni();
            DiscoverCustomSkins();
            DiscoverOtherOverridesAndObjects();
            overlay::SetDrawCallback(&OrcUiDraw);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Если падаем на самом старте, хотя бы не дать игре умереть “без причин”.
            g_enabled = false;
            overlay::SetOpen(false);
        }
    }
    samp_bridge::Poll(g_toggleCommand.c_str(), &ToggleOverlayFromSamp);
    RefreshActivationRouting();
    overlay::Init();  // no-op after first time
    __try { SyncAndRender(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    overlay::DrawFrame();
}

static void OnPedRenderBefore(CPed* ped) {
    if (!g_skinModeEnabled || !g_skinHideBasePed) return;
    CPlayerPed* player = FindPlayerPed(0);
    if (!ped || !ped->m_pRwClump) return;
    bool isLocal = false;
    CustomSkinCfg* skin = ResolveSkinForPed(ped, player, &isLocal);
    if (!skin) return;
    if (!EnsureCustomSkinLoaded(*skin)) return;
    g_hiddenPed = ped;
    g_hiddenClump = ped->m_pRwClump;
    __try {
        g_hideSnapshotValid = true;
        CVisibilityPlugins::SetClumpAlpha(g_hiddenClump, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hideSnapshotValid = false;
        g_hiddenPed = nullptr;
        g_hiddenClump = nullptr;
    }
}

static void OnPedRenderAfter(CPed* ped) {
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
    overlay::Shutdown();
    samp_bridge::Shutdown();
    for (int i = 0; i < kMax; i++) g_rendered[i] = {};
    ClearAllOtherPeds();
    g_hideSnapshotValid = false;
    g_hiddenPed = nullptr;
    g_hiddenClump = nullptr;
    for (auto& o : g_customObjects) {
        DestroyCustomObjectInstance(o);
        o.txdSlot = -1;
    }
    for (auto& s : g_customSkins) {
        DestroyCustomSkinInstance(s);
        s.txdSlot = -1;
    }
    for (auto& kv : g_otherByModelKey) DestroySkinOtherOverrides(kv.second);
    DestroyAllRandomPoolSkins();
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
        Events::pedRenderEvent.before += &OnPedRenderBefore;
        Events::pedRenderEvent.after += &OnPedRenderAfter;
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
        // Hook before first drawing frame: `LoadWeaponObject` runs during boot weapon.dat load.
        EnsureWeaponDatHookInstalled();
        EnsurePedDatHookInstalled();
    }
    return TRUE;
}
