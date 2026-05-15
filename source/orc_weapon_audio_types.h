#pragma once

#include <cstdint>

enum class OrcWeaponSpatial : uint8_t {
    ListenerRelative,
    WorldAtPed,
};

enum class OrcWeaponSoundClass : uint8_t {
    Shoot = 0,
    After,
    Reload,
    ReloadOne,
    ReloadTwo,
    Distant,
    LowAmmo,
    Dryfire,
    Melee,
    Loop,
};

inline constexpr int kOrcWeaponSoundClassCount = 10;

struct OrcWeaponAudioAttenuation {
    float maxDist = 80.0f;
    float refDist = 1.5f;
    float rolloffFactor = 1.0f;
    float airAbsorption = 2.0f;
};

struct OrcWeaponAudioPlayParams {
    float gain = 1.0f;
    float pitch = 1.0f;
    OrcWeaponSpatial spatial = OrcWeaponSpatial::ListenerRelative;
    OrcWeaponSoundClass soundClass = OrcWeaponSoundClass::Shoot;
    OrcWeaponAudioAttenuation att{};
    bool useEfxReverb = false;
};
