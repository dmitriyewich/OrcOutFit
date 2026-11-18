// Attenuation defaults, [WeaponAudio] INI, optional <stem>.audio sidecar.

#include "orc_weapon_audio_config.h"

#include "plugin.h"

#include "CTimer.h"

#include "orc_app.h"
#include "orc_ini.h"
#include "orc_ini_document.h"
#include "orc_path.h"
#include "orc_weapon_audio_internal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace {
constexpr char kWeaponAudioSection[] = "WeaponAudio";

struct OrcAudioClassDefaults {
    const char* key;
    OrcWeaponAudioAttenuation att;
};

constexpr OrcAudioClassDefaults kClassDefaults[kOrcWeaponSoundClassCount] = {
    {"Shoot",          {250.0f, 1.5f, 1.0f, 2.0f}},
    {"After",          {180.0f, 2.3f, 6.0f, 2.0f}},
    {"Reload",         { 45.0f, 2.3f, 6.0f, 2.0f}},
    {"ReloadOne",      { 45.0f, 2.3f, 6.0f, 2.0f}},
    {"ReloadTwo",      { 45.0f, 2.3f, 6.0f, 2.0f}},
    {"Distant",        {500.0f, 2.0f, 1.0f, 2.0f}},
    {"LowAmmo",        { 80.0f, 4.5f, 1.0f, 2.0f}},
    {"Dryfire",        { 35.0f, 1.5f, 1.0f, 2.0f}},
    {"Melee",          { 35.0f, 2.0f, 3.0f, 1.0f}},
    {"Loop",           {200.0f, 2.0f, 1.0f, 1.0f}},
    {"MinigunSpin",    {160.0f, 3.0f, 1.5f, 0.8f}},
    {"MinigunSpinEnd", {160.0f, 3.0f, 1.5f, 0.8f}},
    {"ChainsawStop",   { 80.0f, 2.0f, 1.5f, 1.0f}},
};

struct OrcAudioClassSettings {
    OrcWeaponAudioAttenuation att;
    std::optional<float> pitch;
};

using OrcAudioClassSettingsArray = std::array<OrcAudioClassSettings, kOrcWeaponSoundClassCount>;

struct OrcAudioStemSettings {
    OrcAudioClassSettingsArray classes;
    std::optional<bool> efxReverb;
    std::optional<bool> efxInteriorOnly;
};

constexpr OrcAudioClassSettingsArray OrcBuiltinClasses() {
    OrcAudioClassSettingsArray classes{};
    for (size_t i = 0; i < classes.size(); ++i)
        classes[i].att = kClassDefaults[i].att;
    return classes;
}

OrcAudioClassSettingsArray g_audioClasses = OrcBuiltinClasses();
std::unordered_map<std::string, OrcAudioStemSettings> g_stemSettings;

std::optional<float> OrcReadAudioNumber(const OrcIniDocument& doc, const char* key) {
    const std::string value = doc.GetString(kWeaponAudioSection, key, "");
    const char* begin = value.data();
    const char* end = begin + value.size();
    while (begin != end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    if (begin != end && *begin == '+')
        ++begin;

    float number = 0.0f;
    auto parsed = std::from_chars(begin, end, number);
    if (parsed.ec != std::errc{} || !std::isfinite(number))
        return std::nullopt;

    while (parsed.ptr != end && (*parsed.ptr == ' ' || *parsed.ptr == '\t'))
        ++parsed.ptr;
    if (parsed.ptr != end && *parsed.ptr != ';' && *parsed.ptr != '#')
        return std::nullopt;
    return number;
}

std::optional<bool> OrcReadAudioFlag(const OrcIniDocument& doc, const char* key) {
    if (const auto value = OrcReadAudioNumber(doc, key))
        return *value != 0.0f;
    return std::nullopt;
}

void OrcMergeAudioClasses(const OrcIniDocument& doc, OrcAudioClassSettingsArray& classes) {
    constexpr float kMinPositive = std::numeric_limits<float>::min();
    constexpr float kMaxFloat = std::numeric_limits<float>::max();
    for (size_t i = 0; i < classes.size(); ++i) {
        const std::string prefix = std::string(kClassDefaults[i].key) + ".";
        auto apply = [&](const char* field, float& target, float min, float max) {
            const auto value = OrcReadAudioNumber(doc, (prefix + field).c_str());
            if (value && *value >= min && *value <= max)
                target = *value;
        };

        auto& cls = classes[i];
        apply("RefDist", cls.att.refDist, kMinPositive, kMaxFloat);
        apply("MaxDist", cls.att.maxDist, 0.0f, kMaxFloat);
        apply("Rolloff", cls.att.rolloffFactor, 0.0f, kMaxFloat);
        apply("AirAbsorption", cls.att.airAbsorption, 0.0f, 10.0f);
        if (const auto pitch = OrcReadAudioNumber(doc, (prefix + "Pitch").c_str()))
            cls.pitch = std::clamp(*pitch, 0.01f, 4.0f);
    }
}

const OrcAudioStemSettings& OrcGetStemSettings(const OrcWeaponAudioStemContext& ctx) {
    auto [it, inserted] = g_stemSettings.try_emplace(ctx.asset->dffPath);
    auto& settings = it->second;
    if (inserted) {
        settings.classes = g_audioClasses;
        const std::string path = OrcJoinPath(ctx.dir, ctx.stem + ".audio");
        OrcIniDocument doc;
        if (doc.LoadFromFile(path.c_str())) {
            OrcMergeAudioClasses(doc, settings.classes);
            settings.efxReverb = OrcReadAudioFlag(doc, "EfxReverb");
            settings.efxInteriorOnly = OrcReadAudioFlag(doc, "EfxReverbInteriorOnly");
        }
    }
    return settings;
}

void OrcPushIniFloat(std::vector<OrcIniValue>& values, const std::string& key, float value) {
    char buf[48];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value);
    if (result.ec == std::errc{})
        values.push_back({kWeaponAudioSection, key, std::string(buf, result.ptr)});
}
} // namespace

void OrcWeaponAudioConfigApplyFromMainIni(const OrcIniDocument& ini) {
    g_audioClasses = OrcBuiltinClasses();
    OrcMergeAudioClasses(ini, g_audioClasses);
    g_weaponAudioEfxReverb = OrcReadAudioFlag(ini, "EfxReverb").value_or(true);
    g_weaponAudioEfxInteriorOnly = OrcReadAudioFlag(ini, "EfxReverbInteriorOnly").value_or(true);
    OrcWeaponAudioConfigClearStemOverrides();
}

void OrcWeaponAudioConfigClearStemOverrides() {
    g_stemSettings.clear();
}

void OrcWeaponAudioConfigAppendMainIniValues(std::vector<OrcIniValue>& values) {
    for (size_t i = 0; i < g_audioClasses.size(); ++i) {
        const std::string prefix = std::string(kClassDefaults[i].key) + ".";
        const auto& cls = g_audioClasses[i];
        OrcPushIniFloat(values, prefix + "RefDist", cls.att.refDist);
        OrcPushIniFloat(values, prefix + "MaxDist", cls.att.maxDist);
        OrcPushIniFloat(values, prefix + "Rolloff", cls.att.rolloffFactor);
        OrcPushIniFloat(values, prefix + "AirAbsorption", cls.att.airAbsorption);
        if (cls.pitch)
            OrcPushIniFloat(values, prefix + "Pitch", *cls.pitch);
    }
    values.push_back({kWeaponAudioSection, "EfxReverb", g_weaponAudioEfxReverb ? "1" : "0"});
    values.push_back({kWeaponAudioSection, "EfxReverbInteriorOnly", g_weaponAudioEfxInteriorOnly ? "1" : "0"});
}

OrcWeaponSoundClass OrcWeaponInferSoundClassFromSuffix(const char* sfxSuffix) {
    if (!sfxSuffix || !sfxSuffix[0])
        return OrcWeaponSoundClass::Shoot;
    const char* s = sfxSuffix;
    if (*s == '_')
        ++s;

    if (strncmp(s, "distant", 7) == 0)
        return OrcWeaponSoundClass::Distant;
    if (strncmp(s, "reload_one", 10) == 0)
        return OrcWeaponSoundClass::ReloadOne;
    if (strncmp(s, "reload_two", 10) == 0)
        return OrcWeaponSoundClass::ReloadTwo;
    if (strncmp(s, "reload", 6) == 0)
        return OrcWeaponSoundClass::Reload;
    if (strncmp(s, "low_ammo", 8) == 0)
        return OrcWeaponSoundClass::LowAmmo;
    if (strncmp(s, "dryfire", 7) == 0)
        return OrcWeaponSoundClass::Dryfire;
    if (strncmp(s, "martial_punch", 13) == 0 || strncmp(s, "martial_kick", 12) == 0 || strncmp(s, "stomp", 5) == 0 ||
        strncmp(s, "swing", 5) == 0 || strncmp(s, "hitmetal", 8) == 0 || strncmp(s, "hitwood", 7) == 0 ||
        strncmp(s, "hit", 3) == 0)
        return OrcWeaponSoundClass::Melee;
    if (strncmp(s, "after", 5) == 0)
        return OrcWeaponSoundClass::After;
    if (strncmp(s, "shoot", 5) == 0)
        return OrcWeaponSoundClass::Shoot;

    if (strncmp(s, "flamethrower_start", 18) == 0)
        return OrcWeaponSoundClass::Shoot;

    if (strncmp(s, "minigun_barrelspinend", 21) == 0)
        return OrcWeaponSoundClass::MinigunSpinEnd;
    if (strncmp(s, "minigun_barrelspinloop", 22) == 0)
        return OrcWeaponSoundClass::MinigunSpin;

    if (strncmp(s, "chainsaw_stop", 13) == 0)
        return OrcWeaponSoundClass::ChainsawStop;

    if (strncmp(s, "flamethrower_", 13) == 0 || strncmp(s, "minigun_", 8) == 0 || strncmp(s, "chainsaw_", 9) == 0 ||
        strncmp(s, "spraycan_", 9) == 0 || strncmp(s, "extinguisher_", 13) == 0)
        return OrcWeaponSoundClass::Loop;

    return OrcWeaponSoundClass::Shoot;
}

OrcWeaponAudioPlayParams OrcWeaponAudioBuildPlayParams(const OrcWeaponAudioStemContext* ctx, float gainScale,
    OrcWeaponSpatial spatial, OrcWeaponSoundClass cls) {
    const OrcAudioStemSettings* stem = ctx && ctx->asset ? &OrcGetStemSettings(*ctx) : nullptr;
    const size_t index = static_cast<size_t>(cls);
    const auto& classes = stem ? stem->classes : g_audioClasses;
    const auto& settings = classes[index < classes.size() ? index : 0];

    OrcWeaponAudioPlayParams params{};
    params.gain = std::isfinite(gainScale) ? std::max(0.0f, gainScale) : 0.0f;
    params.spatial = spatial;
    params.soundClass = cls;
    const float timeScale = CTimer::ms_fTimeScale;
    params.pitch = settings.pitch.value_or(1.0f) * (std::isfinite(timeScale) ? std::clamp(timeScale, 0.01f, 4.0f) : 1.0f);
    params.att = settings.att;

    if (stem) {
        params.efxReverbEnabled = stem->efxReverb;
        params.efxReverbInteriorOnly = stem->efxInteriorOnly;
    }
    return params;
}
