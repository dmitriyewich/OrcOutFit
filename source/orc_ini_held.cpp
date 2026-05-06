#include "orc_ini_held.h"

#include <cstdlib>
#include <string>

bool OrcIniSectionHasAnyHeldKey(const OrcIniDocument& doc, const char* section) {
    if (!doc.IsLoaded() || !section || !section[0])
        return false;
    static const char* keys[] = {
        "HeldEnabled", "HeldOffsetX", "HeldOffsetY", "HeldOffsetZ",
        "HeldRotationX", "HeldRotationY", "HeldRotationZ", "HeldScale"
    };
    for (const char* k : keys) {
        if (!doc.GetString(section, k, "").empty())
            return true;
    }
    return false;
}

bool OrcIniSectionHasHeldTweakKey(const OrcIniDocument& doc, const char* section) {
    if (!doc.IsLoaded() || !section || !section[0])
        return false;
    static const char* keys[] = {
        "HeldOffsetX", "HeldOffsetY", "HeldOffsetZ",
        "HeldRotationX", "HeldRotationY", "HeldRotationZ", "HeldScale"
    };
    for (const char* k : keys) {
        if (!doc.GetString(section, k, "").empty())
            return true;
    }
    return false;
}

void OrcReadHeldWeaponSectionFromIni(HeldWeaponPoseCfg& h, const OrcIniDocument& doc, const char* section) {
    if (!section || !section[0])
        return;
    if (!OrcIniSectionHasAnyHeldKey(doc, section))
        return;

    const std::string heldEn = doc.GetString(section, "HeldEnabled", "");
    if (!heldEn.empty())
        h.enabled = doc.GetInt(section, "HeldEnabled", 0) != 0;
    else
        h.enabled = OrcIniSectionHasHeldTweakKey(doc, section);

    auto F = [&](const char* key, float def) -> float {
        const std::string s = doc.GetString(section, key, "");
        if (s.empty())
            return def;
        return static_cast<float>(std::atof(s.c_str()));
    };

    h.x = F("HeldOffsetX", h.x);
    h.y = F("HeldOffsetY", h.y);
    h.z = F("HeldOffsetZ", h.z);
    const float rxDeg = F("HeldRotationX", h.rx / D2R);
    const float ryDeg = F("HeldRotationY", h.ry / D2R);
    const float rzDeg = F("HeldRotationZ", h.rz / D2R);
    h.rx = rxDeg * D2R;
    h.ry = ryDeg * D2R;
    h.rz = rzDeg * D2R;
    h.scale = F("HeldScale", h.scale);
}
