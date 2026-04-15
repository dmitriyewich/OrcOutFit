#pragma once

#include <string>
#include <vector>

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
