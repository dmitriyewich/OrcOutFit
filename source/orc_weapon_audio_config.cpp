// Attenuation defaults, [WeaponAudio] INI, optional <stem>.audio sidecar.

#include "orc_weapon_audio_config.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "plugin.h"

#include "CGame.h"
#include "CTimer.h"

#include "orc_app.h"
#include "orc_ini.h"
#include "orc_ini_document.h"
#include "orc_path.h"
#include "orc_weapon_audio_internal.h"

static const char* kWeaponAudioSection = "WeaponAudio";

static const char* kClassKey[kOrcWeaponSoundClassCount] = {"Shoot", "After", "Reload", "ReloadOne", "ReloadTwo", "Distant",
    "LowAmmo", "Dryfire", "Melee", "Loop"};

static OrcWeaponAudioAttenuation g_builtin[kOrcWeaponSoundClassCount];
static OrcWeaponAudioAttenuation g_iniOverride[kOrcWeaponSoundClassCount];
static bool g_iniOverrideValid[kOrcWeaponSoundClassCount];
static bool g_builtinInited = false;

struct OrcStemAttPack {
    OrcWeaponAudioAttenuation per[kOrcWeaponSoundClassCount]{};
    bool valid[kOrcWeaponSoundClassCount]{};
};

static std::unordered_map<std::string, OrcStemAttPack> g_stemAtt;

static void OrcEnsureBuiltinDefaults() {
    if (g_builtinInited)
        return;
    g_builtinInited = true;
    auto set = [](int i, float ref, float maxd, float roll, float air) {
        g_builtin[i].refDist = ref;
        g_builtin[i].maxDist = maxd;
        g_builtin[i].rolloffFactor = roll;
        g_builtin[i].airAbsorption = air;
    };
    using O = OrcWeaponSoundClass;
    set((int)O::Shoot, 1.5f, 10000.0f, 1.0f, 2.0f);
    set((int)O::After, 2.3f, 10000.0f, 6.0f, 2.0f);
    set((int)O::Reload, 2.3f, 10000.0f, 6.0f, 2.0f);
    set((int)O::ReloadOne, 2.3f, 10000.0f, 6.0f, 2.0f);
    set((int)O::ReloadTwo, 2.3f, 10000.0f, 6.0f, 2.0f);
    set((int)O::Distant, 2.0f, 500.0f, 1.0f, 2.0f);
    set((int)O::LowAmmo, 4.5f, 10000.0f, 1.0f, 2.0f);
    set((int)O::Dryfire, 1.5f, 10000.0f, 1.0f, 2.0f);
    set((int)O::Melee, 2.0f, 3000.0f, 3.0f, 1.0f);
    set((int)O::Loop, 2.0f, 200.0f, 1.0f, 1.0f);
}

static bool OrcMergeClassAttFromIni(const OrcIniDocument& doc, OrcWeaponAudioAttenuation& io, const char* classKey) {
    bool any = false;
    char buf[64];
    _snprintf_s(buf, _TRUNCATE, "%s.RefDist", classKey);
    if (doc.KeyExists(kWeaponAudioSection, buf)) {
        io.refDist = static_cast<float>(atof(doc.GetString(kWeaponAudioSection, buf, "0").c_str()));
        any = true;
    }
    _snprintf_s(buf, _TRUNCATE, "%s.MaxDist", classKey);
    if (doc.KeyExists(kWeaponAudioSection, buf)) {
        io.maxDist = static_cast<float>(atof(doc.GetString(kWeaponAudioSection, buf, "0").c_str()));
        any = true;
    }
    _snprintf_s(buf, _TRUNCATE, "%s.Rolloff", classKey);
    if (doc.KeyExists(kWeaponAudioSection, buf)) {
        io.rolloffFactor = static_cast<float>(atof(doc.GetString(kWeaponAudioSection, buf, "0").c_str()));
        any = true;
    }
    _snprintf_s(buf, _TRUNCATE, "%s.AirAbsorption", classKey);
    if (doc.KeyExists(kWeaponAudioSection, buf)) {
        io.airAbsorption = static_cast<float>(atof(doc.GetString(kWeaponAudioSection, buf, "0").c_str()));
        any = true;
    }
    return any;
}

void OrcWeaponAudioConfigApplyFromMainIni(const OrcIniDocument& ini) {
    OrcEnsureBuiltinDefaults();
    for (int i = 0; i < kOrcWeaponSoundClassCount; ++i) {
        OrcWeaponAudioAttenuation a = g_builtin[i];
        const bool any = OrcMergeClassAttFromIni(ini, a, kClassKey[i]);
        g_iniOverrideValid[i] = any;
        if (any)
            g_iniOverride[i] = a;
    }

    g_weaponAudioEfxReverb = ini.GetInt(kWeaponAudioSection, "EfxReverb", 1) != 0;
    g_weaponAudioEfxInteriorOnly = ini.GetInt(kWeaponAudioSection, "EfxReverbInteriorOnly", 1) != 0;

    g_stemAtt.clear();
}

void OrcWeaponAudioConfigClearStemOverrides() {
    g_stemAtt.clear();
}

static void OrcPushIniFloat(std::vector<OrcIniValue>& values, const char* key, float v, const char* fmt) {
    char buf[48];
    _snprintf_s(buf, _TRUNCATE, fmt, v);
    OrcIniValue iv;
    iv.section = kWeaponAudioSection;
    iv.key = key;
    iv.value = buf;
    values.push_back(std::move(iv));
}

void OrcWeaponAudioConfigAppendMainIniValues(std::vector<OrcIniValue>& values) {
    OrcEnsureBuiltinDefaults();
    char kbuf[80];
    for (int i = 0; i < kOrcWeaponSoundClassCount; ++i) {
        const OrcWeaponAudioAttenuation& a = g_iniOverrideValid[i] ? g_iniOverride[i] : g_builtin[i];
        _snprintf_s(kbuf, _TRUNCATE, "%s.RefDist", kClassKey[i]);
        OrcPushIniFloat(values, kbuf, a.refDist, "%.2f");
        _snprintf_s(kbuf, _TRUNCATE, "%s.MaxDist", kClassKey[i]);
        OrcPushIniFloat(values, kbuf, a.maxDist, "%.0f");
        _snprintf_s(kbuf, _TRUNCATE, "%s.Rolloff", kClassKey[i]);
        OrcPushIniFloat(values, kbuf, a.rolloffFactor, "%.2f");
        _snprintf_s(kbuf, _TRUNCATE, "%s.AirAbsorption", kClassKey[i]);
        OrcPushIniFloat(values, kbuf, a.airAbsorption, "%.2f");
    }
    {
        OrcIniValue iv;
        iv.section = kWeaponAudioSection;
        iv.key = "EfxReverb";
        iv.value = g_weaponAudioEfxReverb ? "1" : "0";
        values.push_back(std::move(iv));
    }
    {
        OrcIniValue iv;
        iv.section = kWeaponAudioSection;
        iv.key = "EfxReverbInteriorOnly";
        iv.value = g_weaponAudioEfxInteriorOnly ? "1" : "0";
        values.push_back(std::move(iv));
    }
}

OrcWeaponAudioAttenuation OrcWeaponAudioConfigBuiltinAttenuation(OrcWeaponSoundClass cls) {
    OrcEnsureBuiltinDefaults();
    const int i = static_cast<int>(cls);
    if (i < 0 || i >= kOrcWeaponSoundClassCount)
        return g_builtin[0];
    return g_builtin[i];
}

static void OrcLoadStemSidecar(const OrcWeaponAudioStemContext& ctx, OrcStemAttPack& pack) {
    OrcEnsureBuiltinDefaults();
    const std::string path = OrcJoinPath(ctx.dir, ctx.stem + ".audio");
    OrcIniDocument doc;
    if (!doc.LoadFromFile(path.c_str()))
        return;
    for (int i = 0; i < kOrcWeaponSoundClassCount; ++i) {
        OrcWeaponAudioAttenuation a = g_iniOverrideValid[i] ? g_iniOverride[i] : g_builtin[i];
        const bool any = OrcMergeClassAttFromIni(doc, a, kClassKey[i]);
        if (any) {
            pack.per[i] = a;
            pack.valid[i] = true;
        }
    }
}

static const OrcStemAttPack& OrcGetStemPack(const OrcWeaponAudioStemContext& ctx) {
    const std::string key = OrcJoinPath(ctx.dir, ctx.stem);
    auto it = g_stemAtt.find(key);
    if (it != g_stemAtt.end())
        return it->second;

    OrcStemAttPack pack{};
    OrcLoadStemSidecar(ctx, pack);
    auto ins = g_stemAtt.emplace(key, std::move(pack));
    return ins.first->second;
}

OrcWeaponAudioAttenuation OrcWeaponAudioConfigResolveAttenuation(const OrcWeaponAudioStemContext* ctx, OrcWeaponSoundClass cls) {
    OrcEnsureBuiltinDefaults();
    const int ci = static_cast<int>(cls);
    OrcWeaponAudioAttenuation a = g_iniOverrideValid[ci] ? g_iniOverride[ci] : g_builtin[ci];
    if (ctx) {
        const OrcStemAttPack& sp = OrcGetStemPack(*ctx);
        if (sp.valid[ci])
            a = sp.per[ci];
    }
    return a;
}

bool OrcWeaponAudioConfigEfxReverbEnabled() {
    return g_weaponAudioEfxReverb;
}

bool OrcWeaponAudioConfigEfxInteriorOnly() {
    return g_weaponAudioEfxInteriorOnly;
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
    if (strncmp(s, "hit", 3) == 0)
        return OrcWeaponSoundClass::Melee;
    if (strncmp(s, "after", 5) == 0)
        return OrcWeaponSoundClass::After;
    if (strncmp(s, "shoot", 5) == 0)
        return OrcWeaponSoundClass::Shoot;

    if (strncmp(s, "flamethrower_start", 18) == 0)
        return OrcWeaponSoundClass::Shoot;

    if (strncmp(s, "flamethrower_", 13) == 0 || strncmp(s, "minigun_", 8) == 0 || strncmp(s, "chainsaw_", 9) == 0 ||
        strncmp(s, "spraycan_", 9) == 0 || strncmp(s, "extinguisher_", 13) == 0)
        return OrcWeaponSoundClass::Loop;

    return OrcWeaponSoundClass::Shoot;
}

OrcWeaponAudioPlayParams OrcWeaponAudioBuildPlayParams(const OrcWeaponAudioStemContext* ctx, float gainScale,
    OrcWeaponSpatial spatial, OrcWeaponSoundClass cls) {
    OrcWeaponAudioPlayParams p{};
    p.gain = gainScale >= 0.0f ? gainScale : 0.0f;
    p.spatial = spatial;
    p.soundClass = cls;
    p.pitch = std::max(0.01f, std::min(4.0f, CTimer::ms_fTimeScale));
    p.att = OrcWeaponAudioConfigResolveAttenuation(ctx, cls);
    const bool interior = CGame::currArea > 0;
    p.useEfxReverb =
        g_weaponAudioEfxReverb && spatial == OrcWeaponSpatial::WorldAtPed && (!g_weaponAudioEfxInteriorOnly || interior);
    return p;
}
