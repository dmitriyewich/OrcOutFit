// WeaponsOutFit — рисует оружие из инвентаря на теле игрока.
// Использует plugin-sdk и логику BaseModelRender.

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "CVisibilityPlugins.h"
#include "CPointLights.h"
#include "RenderWare.h"
#include "eWeaponType.h"
#include "game_sa/rw/rphanim.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>

using namespace plugin;

// ----------------------------------------------------------------------------
// Debug log
// ----------------------------------------------------------------------------
static HMODULE g_module = nullptr;
static char    g_logPath[MAX_PATH] = {};
static char    g_iniPath[MAX_PATH] = {};

static void LogInit() {
    char modPath[MAX_PATH] = {};
    GetModuleFileNameA(g_module, modPath, MAX_PATH);
    char* dot = strrchr(modPath, '.');
    if (dot) *dot = 0;
    _snprintf_s(g_logPath, _TRUNCATE, "%s.log", modPath);
    _snprintf_s(g_iniPath, _TRUNCATE, "%s.ini", modPath);
    FILE* f = fopen(g_logPath, "w");
    if (f) { fputs("WeaponsOutFit debug log\n", f); fclose(f); }
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
// Config: per-weapon attachment
// ----------------------------------------------------------------------------
// Bone NODE IDs
static constexpr int BONE_PELVIS    = 2;
static constexpr int BONE_SPINE1    = 3;   // upper back
static constexpr int BONE_R_CLAVIC  = 21;  // right collarbone
static constexpr int BONE_R_UPARM   = 22;
static constexpr int BONE_L_CLAVIC  = 31;
static constexpr int BONE_L_UPARM   = 32;
static constexpr int BONE_L_THIGH   = 41;
static constexpr int BONE_L_CALF    = 42;
static constexpr int BONE_R_THIGH   = 51;
static constexpr int BONE_R_CALF    = 52;

static constexpr float kPi = 3.14159265358979f;
static constexpr float D2R = kPi / 180.0f;

struct WeaponCfg {
    bool  enabled;
    int   boneId;            // 0 = disabled
    float x, y, z;
    float rx, ry, rz;        // радианы
    float scale;
    const char* name;        // ключ секции в INI
};

static bool g_enabled = true;
static WeaponCfg g_cfg[64] = {};

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

static void ReadSection(WeaponCfg& c, const char* section) {
    char buf[64];
    auto F = [&](const char* key, float def)->float{
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_iniPath);
        if (!buf[0]) return def;
        return (float)atof(buf);
    };
    c.enabled = GetPrivateProfileIntA(section, "Enabled", c.enabled ? 1 : 0, g_iniPath) != 0;
    c.boneId  = GetPrivateProfileIntA(section, "Bone", c.boneId, g_iniPath);
    c.x = F("OffsetX", c.x);
    c.y = F("OffsetY", c.y);
    c.z = F("OffsetZ", c.z);
    c.rx = F("RotationX", c.rx / D2R) * D2R;
    c.ry = F("RotationY", c.ry / D2R) * D2R;
    c.rz = F("RotationZ", c.rz / D2R) * D2R;
    c.scale = F("Scale", c.scale);
}

static void LoadConfig() {
    SetupDefaults();
    g_enabled = GetPrivateProfileIntA("Main", "Enabled", 1, g_iniPath) != 0;
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
}

static void SaveDefaultConfig() {
    if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = fopen(g_iniPath, "w");
    if (!f) return;
    fputs("; WeaponsOutFit configuration.\n"
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
          "; где NN = числовой eWeaponType (например [Weapon50]).\n\n", f);
    fputs("[Main]\nEnabled=1\n\n", f);
    WeaponCfg tmp[64] = {};
    for (int i = 0; i < 64; i++) { tmp[i] = g_cfg[i]; }
    // reset to defaults for dump (in case LoadConfig mutated nothing since INI absent)
    for (int i = 0; i < 64; i++) {
        const auto& c = tmp[i];
        if (!c.name) continue;
        fprintf(f,
            "[%s]\n"
            "Enabled=%d\n"
            "Bone=%d\n"
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

static int FindSlotByType(int wt) {
    for (int i = 0; i < kMax; i++)
        if (g_rendered[i].active && g_rendered[i].weaponType == wt) return i;
    return -1;
}
static int FindFree() {
    for (int i = 0; i < kMax; i++) if (!g_rendered[i].active) return i;
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
static RpMaterial* GeomMatWhiteCB(RpMaterial* mat, void* /*data*/) {
    mat->color = { 255, 255, 255, 255 };
    return mat;
}

static RpAtomic* PrepareAtomicCB(RpAtomic* atomic, void* /*data*/) {
    if (atomic->geometry) {
        atomic->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        RpGeometryForAllMaterials(atomic->geometry, GeomMatWhiteCB, nullptr);
    }
    return atomic;
}

static RpAtomic* ResetAtomicCB(RpAtomic* atomic, void* /*data*/) {
    // Nullptr => AtomicDefaultRenderCallBack (штатный RW callback SA).
    CVisibilityPlugins::SetAtomicRenderCallback(atomic, nullptr);
    return atomic;
}

// ----------------------------------------------------------------------------
// Apply matrix
// ----------------------------------------------------------------------------
static void ApplyOffset(RwMatrix* m, float ox, float oy, float oz) {
    m->pos.x += m->right.x * ox + m->up.x * oy + m->at.x * oz;
    m->pos.y += m->right.y * ox + m->up.y * oy + m->at.y * oz;
    m->pos.z += m->right.z * ox + m->up.z * oy + m->at.z * oz;
}

static void RotateMatrix(RwMatrix* m, float rx, float ry, float rz) {
    if (rx == 0 && ry == 0 && rz == 0) return;
    RwV3d pos = m->pos;
    RwV3d ax = { 1,0,0 }, ay = { 0,1,0 }, az = { 0,0,1 };
    RwMatrixRotate(m, &az, rz, rwCOMBINEPRECONCAT);
    RwMatrixRotate(m, &ay, ry, rwCOMBINEPRECONCAT);
    RwMatrixRotate(m, &ax, rx, rwCOMBINEPRECONCAT);
    m->pos = pos;
}

// ----------------------------------------------------------------------------
// Create / render
// ----------------------------------------------------------------------------
static bool CreateWeaponInstance(int wt, int slot, CPed* ped) {
    if (wt <= 0 || wt >= 64) return false;
    const WeaponCfg& wc = g_cfg[wt];
    if (!wc.enabled || wc.boneId == 0) return false;
    if (FindSlotByType(wt) >= 0) return true;

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

    // Сброс render-callback на дефолтный RW — как в BaseModelRender::SetRelatedModelInfoCB.
    if (inst->type == rpCLUMP) {
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), ResetAtomicCB, nullptr);
    } else if (inst->type == rpATOMIC) {
        ResetAtomicCB(reinterpret_cast<RpAtomic*>(inst), nullptr);
    }

    int fi = FindFree();
    if (fi < 0) {
        if (inst->type == rpCLUMP) RpClumpDestroy(reinterpret_cast<RpClump*>(inst));
        return false;
    }
    g_rendered[fi] = { true, wt, mid, 0, inst };

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

    const WeaponCfg& wc = g_cfg[r.weaponType];
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
        RpClumpForAllAtomics(clump, PrepareAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        if (atomic->geometry) {
            atomic->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
            RpGeometryForAllMaterials(atomic->geometry, GeomMatWhiteCB, nullptr);
        }
        atomic->renderCallBack(atomic);
    }
}

static void SyncAndRender() {
    if (!g_enabled) { ClearAll(); return; }
    CPlayerPed* player = FindPlayerPed(0);
    if (!player) { ClearAll(); return; }

    unsigned char curSlot = player->m_nSelectedWepSlot;
    int curType = 0;
    if (curSlot < 13) curType = (int)player->m_aWeapons[curSlot].m_eWeaponType;

    // Собираем набор типов в инвентаре (не текущего).
    bool want[64] = {};
    for (int s = 0; s < 13; s++) {
        int wt = (int)player->m_aWeapons[s].m_eWeaponType;
        if (wt <= 0 || wt >= 64) continue;
        if (wt == curType) continue;
        if (!g_cfg[wt].enabled || g_cfg[wt].boneId == 0) continue;
        want[wt] = true;
    }

    // Удаляем ненужное.
    for (int i = 0; i < kMax; i++) {
        if (!g_rendered[i].active) continue;
        int wt = g_rendered[i].weaponType;
        if (wt < 0 || wt >= 64 || !want[wt])
            DestroyRendered(g_rendered[i]);
    }

    // Создаём недостающее.
    for (int wt = 1; wt < 64; wt++)
        if (want[wt]) CreateWeaponInstance(wt, 0, player);

    // Рендер.
    int active = 0;
    for (int i = 0; i < kMax; i++) if (g_rendered[i].active) active++;
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

    for (int i = 0; i < kMax; i++) {
        if (!g_rendered[i].active) continue;
        RenderOneWeapon(player, g_rendered[i]);
    }

    RwRenderStateSet(rwRENDERSTATECULLMODE,     reinterpret_cast<void*>(oldCull));
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE,  reinterpret_cast<void*>(oldZT));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(oldZW));
    RwRenderStateSet(rwRENDERSTATESHADEMODE,    reinterpret_cast<void*>(oldShade));
    RwRenderStateSet(rwRENDERSTATEFOGENABLE,    reinterpret_cast<void*>(oldFog));
}

// ----------------------------------------------------------------------------
// Plugin entry
// ----------------------------------------------------------------------------
class WeaponsOutFit {
public:
    WeaponsOutFit() {
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
                Log("Plugin init. Enabled=%d", (int)g_enabled);
            }
            __try { SyncAndRender(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                static bool once = false;
                if (!once) { once = true; Log("EXCEPTION in SyncAndRender"); }
            }
        };
        // При shutdownRwEvent сцена уже частично разобрана, RpClumpDestroy на
        // клонированных материалах падает в CCustomCarEnvMapPipeline dtor.
        // Просто помечаем слоты свободными — RW сам очистит память процесса.
        Events::shutdownRwEvent += [] {
            for (int i = 0; i < kMax; i++) g_rendered[i] = {};
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
