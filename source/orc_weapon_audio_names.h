#pragma once

#include <cstring>

// Maps internal suffix (_shoot) to optional bare EarShot-style filename (shoot).

struct OrcWeaponAudioSuffixAlias {
    const char* suffix;   // includes leading underscore, e.g. "_shoot"
    const char* bareName; // no extension, e.g. "shoot"
};

inline constexpr OrcWeaponAudioSuffixAlias kOrcWeaponAudioSuffixAliases[] = {
    {"_shoot", "shoot"},
    {"_after", "after"},
    {"_reload", "reload"},
    {"_reload_one", "reload_one"},
    {"_reload_two", "reload_two"},
    {"_distant", "distant"},
    {"_low_ammo", "low_ammo"},
    {"_dryfire", "dryfire"},
    {"_hit", "hit"},
    {"_hitmetal", "hitmetal"},
    {"_hitwood", "hitwood"},
    {"_swing", "swing"},
    {"_stomp", "stomp"},
    {"_martial_punch", "martial_punch"},
    {"_martial_kick", "martial_kick"},
    {"_flamethrower_start", "flamethrower_start"},
    {"_flamethrower_fire", "flamethrower_fire"},
    {"_flamethrower_idlegasloop", "flamethrower_idlegasloop"},
    {"_minigun_fireloop", "minigun_fireloop"},
    {"_minigun_barrelspinloop", "minigun_barrelspinloop"},
    {"_minigun_barrelspinend", "minigun_barrelspinend"},
    {"_chainsaw_idle", "chainsaw_idle"},
    {"_chainsaw_active", "chainsaw_active"},
    {"_chainsaw_cuttingflesh", "chainsaw_cuttingflesh"},
    {"_chainsaw_stop", "chainsaw_stop"},
    {"_spraycan_sprayloop", "spraycan_sprayloop"},
    {"_extinguisher_loop", "extinguisher_loop"},
};

inline const char* OrcWeaponAudioBareAliasForSuffix(const char* suffix) {
    if (!suffix || !suffix[0])
        return nullptr;
    for (const OrcWeaponAudioSuffixAlias& e : kOrcWeaponAudioSuffixAliases) {
        if (strcmp(suffix, e.suffix) == 0)
            return e.bareName;
    }
    return nullptr;
}
