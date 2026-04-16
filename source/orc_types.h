#pragma once

#include <string>
#include <array>
#include <vector>
#include <cstdint>

struct RwObject;

// RpHAnim NODE IDs (same as in main / context.md)
constexpr int BONE_PELVIS = 2;
constexpr int BONE_SPINE1 = 3;
constexpr int BONE_R_CLAVIC = 21;
constexpr int BONE_R_UPARM = 22;
constexpr int BONE_L_CLAVIC = 31;
constexpr int BONE_L_UPARM = 32;
constexpr int BONE_L_THIGH = 41;
constexpr int BONE_L_CALF = 42;
constexpr int BONE_R_THIGH = 51;
constexpr int BONE_R_CALF = 52;

constexpr float kOrcPi = 3.14159265358979f;
constexpr float D2R = kOrcPi / 180.0f;

struct WeaponCfg {
    bool  enabled = false;
    int   boneId = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    float scale = 1.0f;
    const char* name = nullptr;
};

struct CustomObjectCfg {
    std::string name;
    std::string dffPath;
    std::string txdPath;
    std::string iniPath;
    int txdSlot = -1;
    RwObject* rwObject = nullptr;
    bool txdMissingLogged = false;
    bool  enabled = true;
    int   boneId = BONE_R_THIGH;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    float scale = 1.0f;

    // Weapon filter: if mask==0 -> render always. Otherwise render only when condition is met.
    // Bits correspond to eWeaponType (0..63).
    std::uint64_t weaponMask = 0;
    bool weaponRequireAll = false;     // false: any selected weapon present; true: all selected weapons present
    bool hideSelectedWeapons = false;  // when condition is met, hide selected weapon(s) on body
};

struct CustomSkinCfg {
    std::string name;
    std::string dffPath;
    std::string txdPath;
    std::string iniPath;
    bool bindToNick = false;
    std::string nickListCsv;
    std::vector<std::string> nicknames;
    int txdSlot = -1;
    RwObject* rwObject = nullptr;
    bool txdMissingLogged = false;
};

// Overrides for standard ped skin under `OrcOutFit/object/other/<skinName>/`.
// Keyed by standard ped model key (see `CModelInfo::m_nKey`).
struct SkinOtherOverrides {
    std::string skinName;       // folder name under object\\other\\<skinName>
    std::string dirPath;        // .../OrcOutFit/object/other/<skinName>
    std::string weaponsIniPath; // .../OrcOutFit/object/other/<skinName>/weapons.ini

    bool hasWeaponOverrides = false; // whether weapons.ini exists and was loaded
    std::array<WeaponCfg, 64> weaponCfg = {};
    std::array<WeaponCfg, 64> weaponCfg2 = {}; // secondary (dual-wield) weapon placement
    std::vector<CustomObjectCfg> objects; // *.dff discovered in the skin folder
};
