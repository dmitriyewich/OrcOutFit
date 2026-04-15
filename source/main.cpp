// OrcOutFit — рисует оружие/объекты/скины на локальном игроке.
// Использует plugin-sdk и логику BaseModelRender.

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CWeaponInfo.h"
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

using namespace plugin;

// ----------------------------------------------------------------------------
// Debug log
// ----------------------------------------------------------------------------
static HMODULE g_module = nullptr;
static char    g_logPath[MAX_PATH] = {};
char    g_iniPath[MAX_PATH] = {};
char    g_gameObjDir[MAX_PATH] = {};
char    g_gameSkinDir[MAX_PATH] = {};
static char    g_weaponSettingsDir[MAX_PATH] = {};

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
    _snprintf_s(g_logPath, _TRUNCATE, "%s.log", modPath);
    _snprintf_s(g_iniPath, _TRUNCATE, "%s.ini", modPath);
    // Relative to plugin location (modloader-friendly):
    // <asi-dir>\OrcOutFit\object and <asi-dir>\OrcOutFit\SKINS
    _snprintf_s(g_gameObjDir, _TRUNCATE, "%s\\OrcOutFit\\object", moduleDir);
    _snprintf_s(g_gameSkinDir, _TRUNCATE, "%s\\OrcOutFit\\SKINS", moduleDir);
    _snprintf_s(g_weaponSettingsDir, _TRUNCATE, "%s\\OrcOutFit\\weaponsetting", moduleDir);

    FILE* f = fopen(g_logPath, "w");
    if (f) { fputs("OrcOutFit debug log\n", f); fclose(f); }
    std::srand(static_cast<unsigned>(GetTickCount()));
}

static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
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
WeaponCfg g_cfg[64] = {};
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
std::unordered_map<unsigned int, std::array<WeaponCfg, 64>> g_weaponCfgByModelKey;
static void LoadWeaponSettingOverrides();

// Секция INI → индекс оружия. Дефолтные расположения в стиле тактической выкладки.
// Оси кости (наблюдение): X = вдоль "right", Y = вдоль "up" (spine) / "at" (бедро), Z = "at/up".
// Параметры подбирались так, чтобы длинные стволы шли по диагонали за спиной,
// пистолеты висели в бедренных кобурах, SMG — под левой рукой у пояса.
static void Set(int wt, const char* name, int bone,
                float x, float y, float z,
                float rxDeg = 0, float ryDeg = 0, float rzDeg = 0,
                float scale = 1.0f) {
    auto& c = g_cfg[wt];
    c.enabled = true;
    c.name    = name;
    c.boneId  = bone;
    c.x = x; c.y = y; c.z = z;
    c.rx = rxDeg * D2R; c.ry = ryDeg * D2R; c.rz = rzDeg * D2R;
    c.scale = scale;
}

// 10 слотов SA → распределение по разным костям.
// Slot 1 melee / Slot 2 pistols / Slot 3 shotguns / Slot 4 SMG /
// Slot 5 assault / Slot 6 rifles / Slot 7 heavy / Slot 8 thrown и т.д.
static void SetupDefaults() {
    for (int i = 0; i < 64; i++) { g_cfg[i] = {}; g_cfg[i].scale = 1.0f; }

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
    g_sampAllowActivationKey = GetPrivateProfileIntA("Main", "SampAllowActivationKey", 0, g_iniPath) != 0;
    char keyBuf[32] = {};
    GetPrivateProfileStringA("Main", "ActivationKey", "F7", keyBuf, sizeof(keyBuf), g_iniPath);
    g_activationVk = ParseActivationVk(keyBuf);
    char cmdBuf[64] = {};
    GetPrivateProfileStringA("Main", "Command", "/orcoutfit", cmdBuf, sizeof(cmdBuf), g_iniPath);
    g_toggleCommand = cmdBuf;
    NormalizeCommand();
    if (g_renderAllPedsRadius < 5.0f) g_renderAllPedsRadius = 5.0f;
    for (int i = 0; i < 64; i++) {
        auto& c = g_cfg[i];
        if (c.name) ReadSection(c, c.name);
        // Fallback для кастомного оружия: секция [WeaponNN].
        char sec[16];
        _snprintf_s(sec, _TRUNCATE, "Weapon%d", i);
        if (GetPrivateProfileIntA(sec, "Bone", 0, g_iniPath) != 0) {
            if (!c.name) { c.scale = 1.0f; c.enabled = true; }
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
    LoadWeaponSettingOverrides();
    RefreshActivationRouting();
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
          "ActivationKey=F7\n"
          "SampAllowActivationKey=0\n"
          "Command=/orcoutfit\n\n", f);
    for (int i = 0; i < 64; i++) {
        const auto& c = g_cfg[i];
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
        if (c == '\n' || c == '\r') {
            flush();
            if (c == '\r' && i + 1 < csv.size() && csv[i + 1] == '\n') ++i;
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
        "Scale=1.000\n",
        baseName.c_str(), BONE_R_THIGH);
    fclose(f);
}

static void CreateDefaultSkinIniIfMissing(const std::string& iniPath, const std::string& baseName) {
    if (FileExistsA(iniPath.c_str())) return;
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) {
        Log("CreateDefaultSkinIniIfMissing: cannot create %s", iniPath.c_str());
        return;
    }
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
        if (!o.txdMissingLogged) {
            o.txdMissingLogged = true;
            Log("custom txd missing for '%s' (dff=%s)", o.name.c_str(), o.dffPath.c_str());
        }
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
        if (!o.txdMissingLogged) {
            o.txdMissingLogged = true;
            Log("txd load failed for '%s': %s", o.name.c_str(), o.txdPath.c_str());
        }
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
        if (!s.txdMissingLogged) {
            s.txdMissingLogged = true;
            Log("skin txd missing for '%s' (dff=%s)", s.name.c_str(), s.dffPath.c_str());
        }
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
    if (s.iniPath.empty()) {
        Log("SaveSkinCfgToIni: empty path (skin '%s')", s.name.c_str());
        return;
    }
    FILE* f = fopen(s.iniPath.c_str(), "w");
    if (!f) {
        Log("SaveSkinCfgToIni: fopen failed '%s'", s.iniPath.c_str());
        return;
    }
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
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        Log("custom objects dir missing: %s", g_gameObjDir);
        return;
    }

    std::string dir = g_gameObjDir;
    std::string mask = JoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("custom objects scan failed: %s", dir.c_str());
        return;
    }

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
        if (o.txdPath.empty()) {
            Log("warning: no matching txd for %s.dff in %s", base.c_str(), dir.c_str());
        }
        g_customObjects.push_back(o);
        foundDff++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    Log("custom objects discovered: %d (dir=%s)", foundDff, dir.c_str());
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
        CBaseModelInfo* mi = CModelInfo::GetModelInfo(folderName.c_str(), &modelId);
        if (!mi || modelId < 0) {
            Log("random skin pool: unknown model name '%s' (skipped)", folderName.c_str());
            continue;
        }
        if (mi->GetModelType() != MODEL_INFO_PED) {
            Log("random skin pool: '%s' is not MODEL_INFO_PED (skipped)", folderName.c_str());
            continue;
        }
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

        if (pool.variants.empty()) {
            Log("random skin pool: '%s' (model %d) has no .dff", folderName.c_str(), modelId);
            continue;
        }
        g_skinRandomPoolVariants += (int)pool.variants.size();
        g_skinRandomPoolModels++;
        g_skinRandomPools.push_back(std::move(pool));
        Log("random skin pool: %s -> model id %d, %d variant(s)", folderName.c_str(), modelId,
            (int)g_skinRandomPools.back().variants.size());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void DiscoverCustomSkins() {
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    g_customSkins.clear();
    DestroyAllRandomPoolSkins();
    DWORD attr = GetFileAttributesA(g_gameSkinDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        Log("skins dir missing: %s", g_gameSkinDir);
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
        CreateDefaultSkinIniIfMissing(s.iniPath, base);
        LoadSkinCfgFromIni(s);
        g_customSkins.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    DiscoverRandomSkinPools(dir);
    Log("custom skins discovered: %d (dir=%s), random pools: %d model(s), %d variant(s)", (int)g_customSkins.size(),
        g_gameSkinDir, g_skinRandomPoolModels, g_skinRandomPoolVariants);
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
}

void SaveSkinModeIni() {
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE* t = fopen(g_iniPath, "w");
        if (!t) {
            Log("SaveSkinModeIni: cannot create %s", g_iniPath);
            return;
        }
        fclose(t);
    }
    char buf[64];
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinModeEnabled ? 1 : 0);
    if (!WritePrivateProfileStringA("SkinMode", "Enabled", buf, g_iniPath))
        Log("SaveSkinModeIni: write Enabled failed (le=%lu)", GetLastError());
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinHideBasePed ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "HideBasePed", buf, g_iniPath);
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinNickMode ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "NickMode", buf, g_iniPath);
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinLocalPreferSelected ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "LocalPreferSelected", buf, g_iniPath);
    if (!WritePrivateProfileStringA("SkinMode", "Selected", g_skinSelectedName.c_str(), g_iniPath))
        Log("SaveSkinModeIni: write Selected failed (le=%lu)", GetLastError());
    _snprintf_s(buf, _TRUNCATE, "%d", g_skinRandomFromPools ? 1 : 0);
    WritePrivateProfileStringA("SkinMode", "RandomFromPools", buf, g_iniPath);
}

static void LoadWeaponSettingOverrides() {
    g_weaponCfgByModelKey.clear();
    DWORD attr = GetFileAttributesA(g_weaponSettingsDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    std::string dir = g_weaponSettingsDir;
    std::string mask = JoinPath(dir, "*.ini");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string iniFile = fd.cFileName;
        if (LowerExt(iniFile) != ".ini") continue;
        const std::string base = BaseNameNoExt(iniFile);
        const std::string fullIni = JoinPath(dir, iniFile);
        unsigned int modelKey = CKeyGen::GetUppercaseKey(base.c_str());
        auto cfgArr = std::array<WeaponCfg, 64>{};
        for (int i = 0; i < 64; i++) cfgArr[i] = g_cfg[i];
        for (int i = 0; i < 64; i++) {
            auto& c = cfgArr[i];
            if (c.name) ReadSectionFromIni(c, c.name, fullIni.c_str());
            char sec[16];
            _snprintf_s(sec, _TRUNCATE, "Weapon%d", i);
            if (GetPrivateProfileIntA(sec, "Bone", 0, fullIni.c_str()) != 0) {
                if (!c.name) { c.scale = 1.0f; c.enabled = true; }
                ReadSectionFromIni(c, sec, fullIni.c_str());
            }
        }
        g_weaponCfgByModelKey[modelKey] = cfgArr;
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    Log("weaponsetting overrides loaded: %d (dir=%s)", count, g_weaponSettingsDir);
}

static const WeaponCfg& GetWeaponCfgForPed(CPed* ped, int wt) {
    if (!ped || wt < 0 || wt >= 64) return g_cfg[0];
    auto* mi = CModelInfo::GetModelInfo(ped->m_nModelIndex);
    if (mi) {
        auto it = g_weaponCfgByModelKey.find(mi->m_nKey);
        if (it != g_weaponCfgByModelKey.end()) return it->second[wt];
    }
    return g_cfg[wt];
}

// ----------------------------------------------------------------------------
// Save single weapon section
// ----------------------------------------------------------------------------
void SaveWeaponSection(int wt) {
    auto& c = g_cfg[wt];
    char sec[32];
    if (c.name) _snprintf_s(sec, _TRUNCATE, "%s", c.name);
    else        _snprintf_s(sec, _TRUNCATE, "Weapon%d", wt);

    char buf[32];
    auto W = [&](const char* key, const char* v) {
        WritePrivateProfileStringA(sec, key, v, g_iniPath);
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

// ----------------------------------------------------------------------------
// Render state
// ----------------------------------------------------------------------------
struct RenderedWeapon {
    bool      active;
    int       weaponType;
    int       modelId;
    int       slot;
    RwObject* rwObject;
};
static constexpr int kMax = 13;
static RenderedWeapon g_rendered[kMax] = {};
using PedWeaponCache = std::array<RenderedWeapon, kMax>;
static std::unordered_map<int, PedWeaponCache> g_otherPedsRendered;

static int FindSlotByType(RenderedWeapon* arr, int wt) {
    for (int i = 0; i < kMax; i++)
        if (arr[i].active && arr[i].weaponType == wt) return i;
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
static bool CreateWeaponInstance(RenderedWeapon* arr, int wt, int slot, CPed* ped) {
    if (wt <= 0 || wt >= 64) return false;
    const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
    if (!wc.enabled || wc.boneId == 0) return false;
    if (FindSlotByType(arr, wt) >= 0) return true;

    CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    if (!info) return false;
    int mid = info->m_nModelId;
    if (mid <= 0) return false;

    auto* mi = CModelInfo::GetModelInfo(mid);
    if (!mi || !mi->m_pRwObject) return false;

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
    arr[fi] = { true, wt, mid, 0, inst };

    static bool logged[64] = {};
    if (!logged[wt]) {
        logged[wt] = true;
        Log("created wt=%d mid=%d bone=%d rwo=%p type=%d", wt, mid, wc.boneId, inst, (int)inst->type);
    }
    (void)slot;
    return true;
}

static void RenderOneWeapon(CPed* ped, RenderedWeapon& r) {
    if (!r.rwObject) return;

    const WeaponCfg& wc = GetWeaponCfgForPed(ped, r.weaponType);
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
    if (o.scale != 1.0f) {
        RwV3d s = { o.scale, o.scale, o.scale };
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
    if (!EnsureCustomSkinLoaded(*sel)) {
        static int logCooldown = 0;
        if ((logCooldown++ % 180) == 0) Log("skin load pending/failed: %s", sel->name.c_str());
        return;
    }
    if (!sel->rwObject || sel->rwObject->type != rpCLUMP) {
        static bool onceBadType = false;
        if (!onceBadType) { onceBadType = true; Log("skin bad rwObject/type for %s", sel->name.c_str()); }
        return;
    }
    RpClump* clump = reinterpret_cast<RpClump*>(sel->rwObject);
    RwFrame* srcFrame = RpClumpGetFrame(pedClump);
    RwFrame* dstFrame = RpClumpGetFrame(clump);
    if (!srcFrame || !dstFrame) {
        static bool onceNoFrame = false;
        if (!onceNoFrame) { onceNoFrame = true; Log("skin frame missing src=%p dst=%p", srcFrame, dstFrame); }
        return;
    }
    static bool loggedSkinInfo = false;
    if (!loggedSkinInfo) {
        loggedSkinInfo = true;
        RpHAnimHierarchy* srcH = GetAnimHierarchyFromSkinClump(pedClump);
        RpHAnimHierarchy* dstH = GetAnimHierarchyFromSkinClump(clump);
        int skinnedAtomics = 0;
        RpClumpForAllAtomics(clump, +[](RpAtomic* a, void* d)->RpAtomic* {
            if (RpSkinAtomicGetSkin(a)) (*reinterpret_cast<int*>(d))++;
            return a;
        }, &skinnedAtomics);
        Log("skin selected=%s srcH=%p dstH=%p srcNodes=%d dstNodes=%d skinAtomics=%d",
            sel->name.c_str(), srcH, dstH, srcH ? srcH->numNodes : -1, dstH ? dstH->numNodes : -1, skinnedAtomics);
    }
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
    } else {
        static bool loggedNoSkinHierarchy = false;
        if (!loggedNoSkinHierarchy) {
            loggedNoSkinHierarchy = true;
            Log("selected skin cannot bind anim hierarchy (srcH=%p bindCount=%d)", srcH, g_skinBindCount);
        }
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

static void SyncPedWeapons(CPed* ped, RenderedWeapon* arr) {
    if (!ped) return;
    unsigned char curSlot = ped->m_nSelectedWepSlot;
    int curType = 0;
    if (curSlot < 13) curType = (int)ped->m_aWeapons[curSlot].m_eWeaponType;
    bool want[64] = {};
    for (int s = 0; s < 13; s++) {
        auto& w = ped->m_aWeapons[s];
        int wt = (int)w.m_eWeaponType;
        if (wt <= 0 || wt >= 64) continue;
        if (wt == curType) continue;
        const WeaponCfg& wc = GetWeaponCfgForPed(ped, wt);
        if (!wc.enabled || wc.boneId == 0) continue;
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
        bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
        if (needsAmmo && w.m_nAmmoTotal == 0) continue;
        want[wt] = true;
    }
    for (int i = 0; i < kMax; i++) {
        if (!arr[i].active) continue;
        int wt = arr[i].weaponType;
        if (wt < 0 || wt >= 64 || !want[wt]) DestroyRendered(arr[i]);
    }
    for (int wt = 1; wt < 64; wt++) if (want[wt]) CreateWeaponInstance(arr, wt, 0, ped);
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
        DestroyAllRandomPoolSkins();
        return;
    }
    CPlayerPed* player = FindPlayerPed(0);
    if (!player) {
        ClearAll();
        ClearAllOtherPeds();
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
        for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
        DestroyAllRandomPoolSkins();
        return;
    }

    SyncPedWeapons(player, g_rendered);
    int active = 0;
    for (int i = 0; i < kMax; i++) if (g_rendered[i].active) active++;
    for (auto& o : g_customObjects) if (EnsureCustomInstance(o, player)) active++;
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
        if (!o.enabled || o.boneId == 0) continue;
        RenderCustomObject(player, o);
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

// ----------------------------------------------------------------------------
// Plugin entry
// ----------------------------------------------------------------------------
class OrcOutFit {
public:
    OrcOutFit() {
        // LoadConfig откладываем до первого кадра: к этому моменту DllMain уже
        // проставит g_iniPath через LogInit().
        Events::initRwEvent.after += [] {
            Log("initRw");
        };
        Events::drawingEvent += [] {
            static bool inited = false;
            if (!inited) {
                inited = true;
                SetupDefaults();
                SaveDefaultConfig();
                LoadConfig();
                DiscoverCustomObjectsAndEnsureIni();
                DiscoverCustomSkins();
                overlay::SetDrawCallback(&OrcUiDraw);
                Log("Plugin init. Enabled=%d", (int)g_enabled);
            }
            samp_bridge::Poll(g_toggleCommand.c_str(), &ToggleOverlayFromSamp);
            RefreshActivationRouting();
            overlay::Init();  // no-op после первого раза
            __try { SyncAndRender(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                static bool once = false;
                if (!once) { once = true; Log("EXCEPTION in SyncAndRender"); }
            }
            overlay::DrawFrame();
        };
        Events::pedRenderEvent.before += [](CPed* ped) {
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
        };
        Events::pedRenderEvent.after += [](CPed* ped) {
            // Always try to finish previously captured hide snapshot, even if toggles changed.
            if (!g_hideSnapshotValid) return;
            if (!ped || ped != g_hiddenPed) return;

            __try {
                if (g_hiddenClump) CVisibilityPlugins::SetClumpAlpha(g_hiddenClump, 255);
                if (ped->m_pRwClump && ped->m_pRwClump != g_hiddenClump) {
                    CVisibilityPlugins::SetClumpAlpha(ped->m_pRwClump, 255);
                    static int mismatchLogTick = 0;
                    if ((mismatchLogTick++ % 120) == 0) {
                        Log("skin hide restore: clump switched old=%p new=%p", g_hiddenClump, ped->m_pRwClump);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // nothing
            }
            g_hideSnapshotValid = false;
            g_hiddenPed = nullptr;
            g_hiddenClump = nullptr;
        };
        Events::d3dResetEvent += [] { overlay::OnResetBefore(); overlay::OnResetAfter(); };
        // При shutdownRwEvent сцена уже частично разобрана, RpClumpDestroy на
        // клонированных материалах падает в CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB
        // (env map plugin data ссылается на уже освобождённую текстуру).
        // Патчим сам деструктор на RET — безвредно т.к. процесс всё равно завершается.
        Events::shutdownRwEvent += [] {
            overlay::Shutdown();
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
            DestroyAllRandomPoolSkins();
            // CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB @ 0x5D95B0 -> ret.
            DWORD oldProt;
            BYTE* p = reinterpret_cast<BYTE*>(0x5D95B0);
            if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                *p = 0xC3;
                DWORD tmp; VirtualProtect(p, 1, oldProt, &tmp);
            }
        };
    }
} g_plugin;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        LogInit();
    }
    return TRUE;
}
