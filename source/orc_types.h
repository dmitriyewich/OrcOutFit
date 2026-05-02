#pragma once

#include <string>
#include <array>
#include <cstdint>
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
    float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;

    // Weapon filter:
    // - if weaponTypes.empty() -> render always
    // - otherwise render only when condition is met
    std::vector<int> weaponTypes;
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

struct StandardObjectSlotCfg {
    int modelId = -1;
    int slot = 1;
};

struct StandardSkinCfg {
    int modelId = -1;
    std::string dffName;
    bool bindToNick = false;
    std::string nickListCsv;
    std::vector<std::string> nicknames;
    RwObject* rwObject = nullptr;
    bool loadFailedLogged = false;
};

enum SkinSelectedSource {
    SKIN_SELECTED_CUSTOM = 0,
    SKIN_SELECTED_STANDARD = 1,
};

struct TextureRemapSlotInfo {
    std::string originalName;
    std::vector<std::string> remapNames;
    int selected = -1; // -1 = original texture, 0..N-1 = *_remap variant
};

struct TextureRemapPedInfo {
    std::string dffName;
    int modelId = -1;
    int txdIndex = -1;
    int totalRemapTextures = 0;
    std::vector<TextureRemapSlotInfo> slots;
};

struct TextureRemapNickBindingInfo {
    int id = -1;
    bool enabled = true;
    std::string nickListCsv;
    int slotCount = 0;
};

enum TextureRemapRandomMode {
    TEXTURE_REMAP_RANDOM_PER_TEXTURE = 0,
    TEXTURE_REMAP_RANDOM_LINKED_VARIANT = 1,
};

// Per standard ped skin (ped.dat DFF name): stored under [Skin.<name>] in `Objects\\<obj>.ini`.
struct CustomObjectSkinParams {
    bool enabled = true;
    int boneId = BONE_R_THIGH;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    float scale = 1.0f;
    float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
    std::vector<int> weaponTypes;
    bool weaponRequireAll = false;
    bool hideSelectedWeapons = false;
};
