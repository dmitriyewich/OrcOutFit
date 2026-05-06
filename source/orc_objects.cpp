// OrcOutFit: custom/standard attached objects (discovery, per-skin INI, instances, render).

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CVector.h"
#include "CWeaponInfo.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "eModelInfoType.h"
#include "eWeaponType.h"
#include "CPools.h"
#include "CTxdStore.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_ini.h"
#include "orc_ini_cache.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_render.h"
#include "orc_types.h"

using namespace plugin;

static std::string StandardObjectsIniPath();

static std::string TrimAscii(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

static void AddIniValue(std::vector<OrcIniValue>& values, const char* section, const char* key, const char* value) {
    OrcIniValue v;
    v.section = section ? section : "";
    v.key = key ? key : "";
    v.value = value ? value : "";
    values.push_back(std::move(v));
}

static void AddIniInt(std::vector<OrcIniValue>& values, const char* section, const char* key, int value) {
    char buf[32];
    _snprintf_s(buf, _TRUNCATE, "%d", value);
    AddIniValue(values, section, key, buf);
}

static void AddIniFloat(std::vector<OrcIniValue>& values, const char* section, const char* key, float value, const char* fmt) {
    char buf[32];
    _snprintf_s(buf, _TRUNCATE, fmt, value);
    AddIniValue(values, section, key, buf);
}

static void DestroyCustomObjectInstance(CustomObjectCfg& o);
static void DestroyAllStandardObjectInstances();
static void DestroyStandardObjectInstancesForSlot(int modelId, int slot);
// ----------------------------------------------------------------------------
// Custom objects discovery (game folder) + per-object INI
// ----------------------------------------------------------------------------
std::vector<CustomObjectCfg> g_customObjects;
std::vector<StandardObjectSlotCfg> g_standardObjects;

std::vector<std::string> ParseNickCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::string token;
    auto flush = [&]() {
        std::string t = TrimAscii(token);
        token.clear();
        if (!t.empty()) out.push_back(OrcToLowerAscii(t));
    };
    for (size_t i = 0; i < csv.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(csv[i]);
        if (c == ',') {
            flush();
            continue;
        }
        token += static_cast<char>(c);
    }
    flush();
    return out;
}

static void CreateObjectIniStubIfMissing(const std::string& iniPath, const std::string& baseName) {
    if (OrcFileExistsA(iniPath.c_str())) return;
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "; OrcOutFit object \"%s\"\n"
        "; Add one section per standard ped skin (ped.dat DFF name), e.g.:\n"
        "; [Skin.wmyclot]\n"
        "; Enabled=1\n"
        "; Bone=%d\n"
        "; OffsetX=0.000\n"
        "; ...\n",
        baseName.c_str(), BONE_R_THIGH);
    fclose(f);
}

static std::vector<int> ParseWeaponTypesCsv(const char* csv) {
    // Parses only commas/whitespace as separators.
    std::vector<int> out;
    if (!csv || !csv[0]) return out;

    const char* p = csv;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') ++p;
        if (!*p) break;
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p) { ++p; continue; }
        if (v > 0 && v < (long)g_cfg.size()) out.push_back((int)v);
        p = end;
    }
    // De-dup (small list; keep it simple).
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static void WeaponTypesToCsv(const std::vector<int>& types, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    bool first = true;
    for (int wt : types) {
        char tmp[16];
        _snprintf_s(tmp, _TRUNCATE, "%d", wt);
        if (!first) strncat_s(out, outSize, ",", _TRUNCATE);
        strncat_s(out, outSize, tmp, _TRUNCATE);
        first = false;
    }
}

static std::string ObjectSkinIniSection(const char* skinDffName) {
    return std::string("Skin.") + (skinDffName ? skinDffName : "");
}

static bool ObjectSkinIniSectionHasData(const OrcIniDocument& doc, const char* sec) {
    if (!sec || !doc.IsLoaded()) return false;
    if (!doc.GetString(sec, "Bone", "").empty()) return true;
    return !doc.GetString(sec, "Enabled", "").empty();
}

static bool ObjectSkinIniSectionExists(const char* iniPath, const char* sec) {
    if (!iniPath || !sec) return false;
    const OrcIniDocument* doc = OrcIniCacheGet(iniPath);
    if (!doc || !doc->IsLoaded()) return false;
    return ObjectSkinIniSectionHasData(*doc, sec);
}

bool LoadObjectSkinParamsFromIni(const char* iniPath, const char* skinDffName, CustomObjectSkinParams& out) {
    if (!iniPath || !iniPath[0] || !skinDffName || !skinDffName[0]) return false;
    const OrcIniDocument* pdoc = OrcIniCacheGet(iniPath);
    if (!pdoc || !pdoc->IsLoaded()) return false;
    const OrcIniDocument& doc = *pdoc;
    const std::string sec = ObjectSkinIniSection(skinDffName);
    if (!ObjectSkinIniSectionHasData(doc, sec.c_str())) return false;

    auto F = [&](const char* key, float def) -> float {
        const std::string s = doc.GetString(sec.c_str(), key, "");
        if (s.empty()) return def;
        return static_cast<float>(atof(s.c_str()));
    };
    out.enabled = doc.GetInt(sec.c_str(), "Enabled", out.enabled ? 1 : 0) != 0;
    out.boneId = doc.GetInt(sec.c_str(), "Bone", out.boneId);
    out.x = F("OffsetX", out.x);
    out.y = F("OffsetY", out.y);
    out.z = F("OffsetZ", out.z);
    out.rx = F("RotationX", out.rx / D2R) * D2R;
    out.ry = F("RotationY", out.ry / D2R) * D2R;
    out.rz = F("RotationZ", out.rz / D2R) * D2R;
    out.scale = F("Scale", out.scale);
    out.scaleX = F("ScaleX", out.scaleX);
    out.scaleY = F("ScaleY", out.scaleY);
    out.scaleZ = F("ScaleZ", out.scaleZ);

    const std::string wcsv = doc.GetString(sec.c_str(), "Weapons", "");
    out.weaponTypes = ParseWeaponTypesCsv(wcsv.c_str());
    const std::string mode = doc.GetString(sec.c_str(), "WeaponsMode", "any");
    out.weaponRequireAll = (OrcToLowerAscii(mode) == "all");
    out.hideSelectedWeapons = doc.GetInt(sec.c_str(), "HideWeapons", out.hideSelectedWeapons ? 1 : 0) != 0;
    return true;
}

struct ObjectSkinParamCacheEntry {
    bool found = false;
    CustomObjectSkinParams params{};
};

static std::unordered_map<std::string, ObjectSkinParamCacheEntry> g_objectSkinParamCache;
bool g_livePreviewObjectActive = false;
std::string g_livePreviewObjectIniPath;
std::string g_livePreviewObjectSkinDff;
CustomObjectSkinParams g_livePreviewObjectParams{};

void InvalidateObjectSkinParamCache() {
    for (const auto& o : g_customObjects) {
        if (!o.iniPath.empty())
            OrcIniCacheInvalidatePath(o.iniPath.c_str());
    }
    OrcIniCacheInvalidatePath(StandardObjectsIniPath().c_str());
    g_objectSkinParamCache.clear();
}

void SaveObjectSkinParamsToIni(const char* iniPath, const char* skinDffName, const CustomObjectSkinParams& p) {
    if (!iniPath || !iniPath[0] || !skinDffName || !skinDffName[0]) return;
    const std::string sec = ObjectSkinIniSection(skinDffName);
    std::vector<OrcIniValue> values;
    AddIniInt(values, sec.c_str(), "Enabled", p.enabled ? 1 : 0);
    AddIniInt(values, sec.c_str(), "Bone", p.boneId);
    AddIniFloat(values, sec.c_str(), "OffsetX", p.x, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetY", p.y, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetZ", p.z, "%.3f");
    AddIniFloat(values, sec.c_str(), "RotationX", p.rx / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationY", p.ry / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationZ", p.rz / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "Scale", p.scale, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleX", p.scaleX, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleY", p.scaleY, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleZ", p.scaleZ, "%.3f");
    char wcsv[256];
    WeaponTypesToCsv(p.weaponTypes, wcsv, sizeof(wcsv));
    AddIniValue(values, sec.c_str(), "Weapons", wcsv);
    AddIniValue(values, sec.c_str(), "WeaponsMode", p.weaponRequireAll ? "all" : "any");
    AddIniInt(values, sec.c_str(), "HideWeapons", p.hideSelectedWeapons ? 1 : 0);
    if (!OrcIniWriteValues(iniPath, "; OrcOutFit custom object config.\n\n", values))
        OrcLogError("SaveObjectSkinParamsToIni: cannot write %s", iniPath);
    InvalidateObjectSkinParamCache();
}

static bool ResolveObjectSkinParamsCached(const std::string& iniPath, const std::string& skinDff, CustomObjectSkinParams& out) {
    if (iniPath.empty() || skinDff.empty()) return false;
    if (g_livePreviewObjectActive &&
        _stricmp(g_livePreviewObjectIniPath.c_str(), iniPath.c_str()) == 0 &&
        _stricmp(g_livePreviewObjectSkinDff.c_str(), skinDff.c_str()) == 0) {
        out = g_livePreviewObjectParams;
        return true;
    }
    const std::string key = OrcToLowerAscii(iniPath) + "\x1e" + OrcToLowerAscii(skinDff);
    auto it = g_objectSkinParamCache.find(key);
    if (it != g_objectSkinParamCache.end()) {
        if (!it->second.found) return false;
        out = it->second.params;
        return true;
    }

    ObjectSkinParamCacheEntry entry{};
    if (!LoadObjectSkinParamsFromIni(iniPath.c_str(), skinDff.c_str(), entry.params)) {
        g_objectSkinParamCache[key] = entry;
        return false;
    }

    entry.found = true;
    out = entry.params;
    g_objectSkinParamCache[key] = entry;
    return true;
}

// Как ResolveWeaponsPresetIniForPed: сначала DFF из LoadPedObject, иначе — имя из GetWeaponSkinIniLookupName
// (при SkinLocalPreferSelected raw=TRUTH и sel=orc секции часто только под TRUTH).
static bool ResolveObjectSkinParamsForPed(const std::string& iniPath, CPed* ped, CustomObjectSkinParams& out) {
    if (!ped || iniPath.empty()) return false;
    const std::string raw = GetPedStdSkinDffName(ped);
    const std::string sel = GetWeaponSkinIniLookupName(ped);
    if (!raw.empty() && ResolveObjectSkinParamsCached(iniPath, raw, out)) return true;
    if (!sel.empty() && _stricmp(raw.c_str(), sel.c_str()) != 0 && ResolveObjectSkinParamsCached(iniPath, sel, out))
        return true;
    return false;
}

static std::vector<std::string> ParseCsvTokens(const char* csv) {
    std::vector<std::string> out;
    if (!csv || !csv[0]) return out;
    std::string token;
    auto flush = [&]() {
        std::string t = TrimAscii(token);
        token.clear();
        if (!t.empty()) out.push_back(t);
    };
    for (const char* p = csv; *p; ++p) {
        if (*p == ',' || *p == ';' || *p == '\r' || *p == '\n') {
            flush();
        } else {
            token += *p;
        }
    }
    flush();
    return out;
}

static std::string StandardObjectsIniPath() {
    return OrcJoinPath(std::string(g_gameObjDir), "StandardObjects.ini");
}

static std::string StandardObjectSlotKey(int modelId, int slot) {
    char buf[40];
    _snprintf_s(buf, _TRUNCATE, "%d#%d", modelId, slot);
    return std::string(buf);
}

static bool ParseStandardObjectSlotKey(const std::string& token, int& modelId, int& slot) {
    modelId = -1;
    slot = 1;
    const std::string t = TrimAscii(token);
    if (t.empty()) return false;
    const char* p = t.c_str();
    char* end = nullptr;
    const long id = strtol(p, &end, 10);
    if (end == p || id < 0 || id > 30000) return false;
    modelId = (int)id;
    if (*end == '#') {
        char* slotEnd = nullptr;
        const long parsedSlot = strtol(end + 1, &slotEnd, 10);
        if (slotEnd == end + 1 || *slotEnd != '\0' || parsedSlot <= 0 || parsedSlot >= 1000)
            return false;
        slot = (int)parsedSlot;
    } else if (*end != '\0') {
        return false;
    }
    return true;
}

static std::string StandardObjectSkinIniSection(int modelId, int slot, const char* skinDffName) {
    return std::string("Object.") + StandardObjectSlotKey(modelId, slot) + ".Skin." + (skinDffName ? skinDffName : "");
}

static CBaseModelInfo* GetExistingStandardModelInfo(int modelId) {
    if (modelId < 0 || modelId >= CModelInfo::ms_modelInfoCount) return nullptr;
    if (!CModelInfo::ms_modelInfoPtrs) return nullptr;
    CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
    if (!mi) return nullptr;
    if (mi->m_pRwObject) return mi;
    if (!CStreaming::ms_aInfoForModel) return nullptr;
    __try {
        const CStreamingInfo& info = CStreaming::ms_aInfoForModel[modelId];
        if (info.m_nCdSize > 0 || info.m_nLoadState == LOADSTATE_LOADED)
            return mi;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("GetExistingStandardModelInfo: streaming info SEH ex=0x%08X model=%d", GetExceptionCode(), modelId);
    }
    return nullptr;
}

static CBaseModelInfo* GetExistingStandardObjectModelInfo(int modelId) {
    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    if (!mi) return nullptr;
    const eModelInfoType type = mi->GetModelType();
    if (type == MODEL_INFO_ATOMIC ||
        type == MODEL_INFO_TIME ||
        type == MODEL_INFO_WEAPON ||
        type == MODEL_INFO_CLUMP ||
        type == MODEL_INFO_LOD) {
        return mi;
    }
    return nullptr;
}

bool IsValidStandardObjectModel(int modelId) {
    return GetExistingStandardObjectModelInfo(modelId) != nullptr;
}

void SaveStandardObjectListToIni() {
    const std::string path = StandardObjectsIniPath();
    std::string entries;
    for (const auto& o : g_standardObjects) {
        if (!entries.empty()) entries += ",";
        entries += StandardObjectSlotKey(o.modelId, o.slot);
    }
    std::vector<OrcIniValue> values;
    AddIniValue(values, "Objects", "Entries", entries.c_str());
    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit standard game object config.\n\n", values))
        OrcLogError("SaveStandardObjectListToIni: cannot write %s", path.c_str());
}

void LoadStandardObjectsFromIni() {
    g_standardObjects.clear();
    InvalidateStandardObjectSkinParamCache();
    const std::string path = StandardObjectsIniPath();
    if (!OrcFileExistsA(path.c_str())) return;

    const OrcIniDocument* doc = OrcIniCacheGet(path.c_str());
    if (!doc || !doc->IsLoaded()) return;
    const std::string entriesStr = doc->GetString("Objects", "Entries", "");
    std::unordered_set<std::string> seen;
    for (const std::string& token : ParseCsvTokens(entriesStr.c_str())) {
        int modelId = -1;
        int slot = 1;
        if (!ParseStandardObjectSlotKey(token, modelId, slot)) continue;
        if (!IsValidStandardObjectModel(modelId)) continue;
        const std::string key = StandardObjectSlotKey(modelId, slot);
        if (!seen.insert(key).second) continue;
        StandardObjectSlotCfg cfg;
        cfg.modelId = modelId;
        cfg.slot = slot;
        g_standardObjects.push_back(cfg);
    }
    OrcLogInfo("LoadStandardObjectsFromIni: %zu entries from %s", g_standardObjects.size(), path.c_str());
}

bool AddStandardObjectSlot(int modelId) {
    if (!IsValidStandardObjectModel(modelId))
        return false;
    int nextSlot = 1;
    for (const auto& o : g_standardObjects) {
        if (o.modelId == modelId && o.slot >= nextSlot)
            nextSlot = o.slot + 1;
    }
    StandardObjectSlotCfg cfg;
    cfg.modelId = modelId;
    cfg.slot = nextSlot;
    g_standardObjects.push_back(cfg);
    SaveStandardObjectListToIni();
    return true;
}

void RemoveStandardObjectSlot(size_t index) {
    if (index >= g_standardObjects.size()) return;
    const int modelId = g_standardObjects[index].modelId;
    const int slot = g_standardObjects[index].slot;
    g_standardObjects.erase(g_standardObjects.begin() + static_cast<long>(index));
    DestroyStandardObjectInstancesForSlot(modelId, slot);
    SaveStandardObjectListToIni();
}

static bool StandardObjectSkinIniSectionExists(int modelId, int slot, const char* skinDffName) {
    const std::string path = StandardObjectsIniPath();
    const std::string sec = StandardObjectSkinIniSection(modelId, slot, skinDffName);
    return ObjectSkinIniSectionExists(path.c_str(), sec.c_str());
}

bool LoadStandardObjectSkinParamsFromIni(int modelId, int slot, const char* skinDffName, CustomObjectSkinParams& out) {
    if (modelId < 0 || slot <= 0 || !skinDffName || !skinDffName[0]) return false;
    if (!IsValidStandardObjectModel(modelId)) return false;
    const std::string path = StandardObjectsIniPath();
    if (!StandardObjectSkinIniSectionExists(modelId, slot, skinDffName)) return false;
    const std::string sec = StandardObjectSkinIniSection(modelId, slot, skinDffName);

    const OrcIniDocument* pdoc = OrcIniCacheGet(path.c_str());
    if (!pdoc || !pdoc->IsLoaded()) return false;
    const OrcIniDocument& doc = *pdoc;

    auto F = [&](const char* key, float def) -> float {
        const std::string s = doc.GetString(sec.c_str(), key, "");
        if (s.empty()) return def;
        return static_cast<float>(atof(s.c_str()));
    };
    out.enabled = doc.GetInt(sec.c_str(), "Enabled", out.enabled ? 1 : 0) != 0;
    out.boneId = doc.GetInt(sec.c_str(), "Bone", out.boneId);
    out.x = F("OffsetX", out.x);
    out.y = F("OffsetY", out.y);
    out.z = F("OffsetZ", out.z);
    out.rx = F("RotationX", out.rx / D2R) * D2R;
    out.ry = F("RotationY", out.ry / D2R) * D2R;
    out.rz = F("RotationZ", out.rz / D2R) * D2R;
    out.scale = F("Scale", out.scale);
    out.scaleX = F("ScaleX", out.scaleX);
    out.scaleY = F("ScaleY", out.scaleY);
    out.scaleZ = F("ScaleZ", out.scaleZ);

    const std::string wcsv = doc.GetString(sec.c_str(), "Weapons", "");
    out.weaponTypes = ParseWeaponTypesCsv(wcsv.c_str());
    const std::string mode = doc.GetString(sec.c_str(), "WeaponsMode", "any");
    out.weaponRequireAll = (OrcToLowerAscii(mode) == "all");
    out.hideSelectedWeapons = doc.GetInt(sec.c_str(), "HideWeapons", out.hideSelectedWeapons ? 1 : 0) != 0;
    return true;
}

struct StandardObjectSkinParamCacheEntry {
    bool found = false;
    CustomObjectSkinParams params{};
};

static std::unordered_map<std::string, StandardObjectSkinParamCacheEntry> g_standardObjectSkinParamCache;
bool g_livePreviewStandardObjectActive = false;
int g_livePreviewStandardObjectModelId = -1;
int g_livePreviewStandardObjectSlot = -1;
std::string g_livePreviewStandardObjectSkinDff;
CustomObjectSkinParams g_livePreviewStandardObjectParams{};

void InvalidateStandardObjectSkinParamCache() {
    OrcIniCacheInvalidatePath(StandardObjectsIniPath().c_str());
    g_standardObjectSkinParamCache.clear();
}

static bool ResolveStandardObjectSkinParamsCached(int modelId, int slot, const std::string& skinDff, CustomObjectSkinParams& out) {
    if (modelId < 0 || slot <= 0 || skinDff.empty()) return false;
    if (!IsValidStandardObjectModel(modelId)) return false;
    if (g_livePreviewStandardObjectActive &&
        g_livePreviewStandardObjectModelId == modelId &&
        g_livePreviewStandardObjectSlot == slot &&
        _stricmp(g_livePreviewStandardObjectSkinDff.c_str(), skinDff.c_str()) == 0) {
        out = g_livePreviewStandardObjectParams;
        return true;
    }

    const std::string key = StandardObjectSlotKey(modelId, slot) + "\x1e" + OrcToLowerAscii(skinDff);
    auto it = g_standardObjectSkinParamCache.find(key);
    if (it != g_standardObjectSkinParamCache.end()) {
        if (!it->second.found) return false;
        out = it->second.params;
        return true;
    }

    StandardObjectSkinParamCacheEntry entry{};
    if (!LoadStandardObjectSkinParamsFromIni(modelId, slot, skinDff.c_str(), entry.params)) {
        g_standardObjectSkinParamCache[key] = entry;
        return false;
    }

    entry.found = true;
    out = entry.params;
    g_standardObjectSkinParamCache[key] = entry;
    return true;
}

static bool ResolveStandardObjectSkinParamsForPed(int modelId, int slot, CPed* ped, CustomObjectSkinParams& out) {
    if (!ped || modelId < 0 || slot <= 0) return false;
    const std::string raw = GetPedStdSkinDffName(ped);
    const std::string sel = GetWeaponSkinIniLookupName(ped);
    if (!raw.empty() && ResolveStandardObjectSkinParamsCached(modelId, slot, raw, out)) return true;
    if (!sel.empty() && _stricmp(raw.c_str(), sel.c_str()) != 0 &&
        ResolveStandardObjectSkinParamsCached(modelId, slot, sel, out))
        return true;
    return false;
}

void SaveStandardObjectSkinParamsToIni(int modelId, int slot, const char* skinDffName, const CustomObjectSkinParams& p) {
    if (modelId < 0 || slot <= 0 || !skinDffName || !skinDffName[0]) return;
    if (!IsValidStandardObjectModel(modelId)) return;
    const std::string path = StandardObjectsIniPath();
    const std::string sec = StandardObjectSkinIniSection(modelId, slot, skinDffName);
    std::vector<OrcIniValue> values;
    AddIniInt(values, sec.c_str(), "Enabled", p.enabled ? 1 : 0);
    AddIniInt(values, sec.c_str(), "Bone", p.boneId);
    AddIniFloat(values, sec.c_str(), "OffsetX", p.x, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetY", p.y, "%.3f");
    AddIniFloat(values, sec.c_str(), "OffsetZ", p.z, "%.3f");
    AddIniFloat(values, sec.c_str(), "RotationX", p.rx / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationY", p.ry / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "RotationZ", p.rz / D2R, "%.2f");
    AddIniFloat(values, sec.c_str(), "Scale", p.scale, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleX", p.scaleX, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleY", p.scaleY, "%.3f");
    AddIniFloat(values, sec.c_str(), "ScaleZ", p.scaleZ, "%.3f");
    char wcsv[256];
    WeaponTypesToCsv(p.weaponTypes, wcsv, sizeof(wcsv));
    AddIniValue(values, sec.c_str(), "Weapons", wcsv);
    AddIniValue(values, sec.c_str(), "WeaponsMode", p.weaponRequireAll ? "all" : "any");
    AddIniInt(values, sec.c_str(), "HideWeapons", p.hideSelectedWeapons ? 1 : 0);
    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit standard game object config.\n\n", values))
        OrcLogError("SaveStandardObjectSkinParamsToIni: cannot write %s", path.c_str());
    InvalidateStandardObjectSkinParamCache();
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

static bool EnsureCustomModelLoaded(CustomObjectCfg& o) {
    if (o.rwObject) return true;
    if (!OrcFileExistsA(o.dffPath.c_str())) {
        static std::unordered_set<std::string> s_once;
        if (s_once.insert(o.name).second)
            OrcLogError("object \"%s\": DFF missing %s", o.name.c_str(), o.dffPath.c_str());
        return false;
    }
    if (o.txdPath.empty() || !OrcFileExistsA(o.txdPath.c_str())) {
        if (!o.txdMissingLogged) {
            o.txdMissingLogged = true;
            OrcLogError("object \"%s\": TXD missing or invalid path", o.name.c_str());
        }
        return false;
    }

    int txdSlot = CTxdStore::FindTxdSlot(o.name.c_str());
    if (txdSlot == -1) txdSlot = CTxdStore::AddTxdSlot(o.name.c_str());
    if (txdSlot == -1) {
        OrcLogError("object \"%s\": CTxdStore::AddTxdSlot failed", o.name.c_str());
        return false;
    }
    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)o.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        if (!o.txdMissingLogged) {
            o.txdMissingLogged = true;
            OrcLogError("object \"%s\": LoadTxd failed", o.name.c_str());
        }
        return false;
    }
    o.txdSlot = txdSlot;
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);

    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)o.dffPath.c_str());
    if (!stream) {
        CTxdStore::PopCurrentTxd();
        OrcLogError("object \"%s\": RwStreamOpen DFF failed", o.name.c_str());
        return false;
    }

    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* readClump = RpClumpStreamRead(stream);
        if (readClump) {
            o.rwObject = reinterpret_cast<RwObject*>(readClump);
            RpClumpForAllAtomics(readClump, OrcInitAttachmentAtomicCB, nullptr);
            ok = true;
            OrcLogInfo("object \"%s\": clump loaded", o.name.c_str());
        }
    } else
        OrcLogError("object \"%s\": DFF has no CLUMP chunk", o.name.c_str());
    RwStreamClose(stream, nullptr);
    CTxdStore::PopCurrentTxd();
    return ok;
}

void DiscoverCustomObjectsAndEnsureIni() {
    for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
    g_customObjects.clear();
    InvalidateObjectSkinParamCache();

    DWORD attr = GetFileAttributesA(g_gameObjDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            OrcLogInfo("Objects folder missing or not a directory: %s", g_gameObjDir);
        }
        return;
    }

    std::string dir = g_gameObjDir;
    std::string mask = OrcJoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    int foundDff = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (OrcLowerExt(fname) != ".dff") continue;

        const std::string base = OrcBaseNameNoExt(fname);
        CustomObjectCfg o;
        o.name = base;
        o.dffPath = OrcJoinPath(dir, base + ".dff");
        o.txdPath = OrcFindBestTxdPath(dir, base);
        o.iniPath = OrcJoinPath(dir, base + ".ini");
        CreateObjectIniStubIfMissing(o.iniPath, base);
        g_customObjects.push_back(o);
        foundDff++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    OrcLogInfo("DiscoverCustomObjects: %d DFF in %s", foundDff, g_gameObjDir);
}


struct RenderedStandardObject {
    int modelId = -1;
    int slot = 1;
    CPed* ped = nullptr;
    RwObject* rwObject = nullptr;
    unsigned int lastSeenFrame = 0;
};

struct StandardObjectRuntimeKey {
    CPed* ped = nullptr;
    int modelId = -1;
    int slot = 1;

    bool operator==(const StandardObjectRuntimeKey& other) const {
        return ped == other.ped && modelId == other.modelId && slot == other.slot;
    }
};

struct StandardObjectRuntimeKeyHash {
    size_t operator()(const StandardObjectRuntimeKey& key) const {
        size_t h = reinterpret_cast<size_t>(key.ped) >> 4;
        h ^= static_cast<size_t>(key.modelId) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(key.slot) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<StandardObjectRuntimeKey, RenderedStandardObject, StandardObjectRuntimeKeyHash> g_renderedStandardObjects;
static std::unordered_map<int, DWORD> g_standardObjectLastRequestTick;
static unsigned int g_standardObjectRenderFrame = 0;

static void DestroyRenderedStandardObject(RenderedStandardObject& r) {
    if (!r.rwObject) {
        r = {};
        return;
    }
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

static void DestroyAllStandardObjectInstances() {
    for (auto& kv : g_renderedStandardObjects)
        DestroyRenderedStandardObject(kv.second);
    g_renderedStandardObjects.clear();
}

static void DestroyStandardObjectInstancesForSlot(int modelId, int slot) {
    for (auto it = g_renderedStandardObjects.begin(); it != g_renderedStandardObjects.end();) {
        if (it->second.modelId == modelId && it->second.slot == slot) {
            DestroyRenderedStandardObject(it->second);
            it = g_renderedStandardObjects.erase(it);
        } else {
            ++it;
        }
    }
}

static bool PedHasWeaponType(CPed* ped, int wt) {
    if (!ped || wt <= 0) return false;
    if (g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
    for (int s = 0; s < 13; s++) {
        auto& w = ped->m_aWeapons[s];
        if ((int)w.m_eWeaponType != wt) continue;
        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
        bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
        if (needsAmmo && w.m_nAmmoTotal == 0) return false;
        return true;
    }
    return false;
}

static bool ShouldRenderObjectForPedWithParams(CPed* ped, const CustomObjectSkinParams& p) {
    if (!p.enabled || p.boneId == 0) return false;
    if (p.weaponTypes.empty()) return true;

    if (p.weaponRequireAll) {
        for (int wt : p.weaponTypes) {
            if (!PedHasWeaponType(ped, wt)) return false;
        }
        return true;
    }

    for (int wt : p.weaponTypes) {
        if (PedHasWeaponType(ped, wt)) return true;
    }
    return false;
}

static void ApplyObjectWeaponSuppression(CPed* ped, std::vector<char>* suppress) {
    if (!ped || !suppress || (!g_renderCustomObjects && !g_renderStandardObjects)) return;
    auto applyParams = [&](const CustomObjectSkinParams& p) {
        if (!p.hideSelectedWeapons) return;
        if (p.weaponTypes.empty()) return;
        if (!ShouldRenderObjectForPedWithParams(ped, p)) return;
        for (int wt : p.weaponTypes) {
            if (wt <= 0 || wt >= (int)suppress->size()) continue;
            (*suppress)[wt] = 1;
        }
    };
    if (g_renderCustomObjects) for (const auto& o : g_customObjects) {
        CustomObjectSkinParams p;
        if (!ResolveObjectSkinParamsForPed(o.iniPath, ped, p)) continue;
        applyParams(p);
    }
    if (g_renderStandardObjects) for (const auto& o : g_standardObjects) {
        CustomObjectSkinParams p;
        if (!ResolveStandardObjectSkinParamsForPed(o.modelId, o.slot, ped, p)) continue;
        applyParams(p);
    }
}

static bool EnsureCustomInstance(CustomObjectCfg& o) {
    if (!EnsureCustomModelLoaded(o)) return false;
    return o.rwObject != nullptr;
}

static void RenderCustomObject(CPed* ped, CustomObjectCfg& o, const CustomObjectSkinParams& p) {
    if (!o.rwObject) return;
    RwMatrix* bone = OrcGetBoneMatrix(ped, p.boneId);
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
    OrcApplyAttachmentOffset(&mtx, p.x, p.y, p.z);
    OrcRotateAttachmentMatrix(&mtx, p.rx, p.ry, p.rz);
    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    const float sx = p.scale * p.scaleX;
    const float sy = p.scale * p.scaleY;
    const float sz = p.scale * p.scaleZ;
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        RwV3d s = { sx, sy, sz };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    OrcApplyAttachmentLightingForPed(ped, lightPos);

    if (o.rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(o.rwObject);
        if (!clump) return;
        RpClumpForAllAtomics(clump, OrcPrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        OrcPrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
}

static RenderedStandardObject* EnsureStandardObjectInstance(CPed* ped, const StandardObjectSlotCfg& cfg, const CustomObjectSkinParams& p) {
    if (!ped || cfg.modelId < 0 || cfg.slot <= 0) return nullptr;
    RwMatrix* bone = OrcGetBoneMatrix(ped, p.boneId);
    if (!bone) return nullptr;
    CBaseModelInfo* mi = GetExistingStandardObjectModelInfo(cfg.modelId);
    if (!mi) {
        DestroyStandardObjectInstancesForSlot(cfg.modelId, cfg.slot);
        return nullptr;
    }

    StandardObjectRuntimeKey key;
    key.ped = ped;
    key.modelId = cfg.modelId;
    key.slot = cfg.slot;
    auto existing = g_renderedStandardObjects.find(key);
    if (existing != g_renderedStandardObjects.end() && existing->second.rwObject)
        return &existing->second;

    if (!CStreaming::HasModelLoaded(cfg.modelId) || !mi->m_pRwObject) {
        const DWORD now = GetTickCount();
        DWORD& last = g_standardObjectLastRequestTick[cfg.modelId];
        if (last == 0 || now - last > 1000) {
            last = now;
            CStreaming::RequestModel(cfg.modelId, 0);
            CStreaming::LoadAllRequestedModels(false);
        }
        return nullptr;
    }

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    RwObject* inst = mi->CreateInstance(&mtx);
    if (!inst) return nullptr;
    if (inst->type == rpCLUMP) {
        RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), OrcInitAttachmentAtomicCB, nullptr);
    } else if (inst->type == rpATOMIC) {
        OrcInitAttachmentAtomicCB(reinterpret_cast<RpAtomic*>(inst), nullptr);
    } else {
        return nullptr;
    }

    RenderedStandardObject rendered;
    rendered.modelId = cfg.modelId;
    rendered.slot = cfg.slot;
    rendered.ped = ped;
    rendered.rwObject = inst;
    rendered.lastSeenFrame = g_standardObjectRenderFrame;
    auto inserted = g_renderedStandardObjects.emplace(key, rendered);
    return &inserted.first->second;
}

static void RenderStandardObject(CPed* ped, const StandardObjectSlotCfg& cfg, const CustomObjectSkinParams& p) {
    RenderedStandardObject* rendered = EnsureStandardObjectInstance(ped, cfg, p);
    if (!rendered || !rendered->rwObject) return;

    RwMatrix* bone = OrcGetBoneMatrix(ped, p.boneId);
    if (!bone) return;

    RpAtomic* atomic = nullptr;
    RwFrame* frame = nullptr;
    if (rendered->rwObject->type == rpATOMIC) {
        atomic = reinterpret_cast<RpAtomic*>(rendered->rwObject);
        frame = RpAtomicGetFrame(atomic);
    } else if (rendered->rwObject->type == rpCLUMP) {
        frame = RpClumpGetFrame(reinterpret_cast<RpClump*>(rendered->rwObject));
    }
    if (!frame) return;
    rendered->lastSeenFrame = g_standardObjectRenderFrame;

    RwMatrix mtx{};
    std::memcpy(&mtx, bone, sizeof(RwMatrix));
    OrcApplyAttachmentOffset(&mtx, p.x, p.y, p.z);
    OrcRotateAttachmentMatrix(&mtx, p.rx, p.ry, p.rz);
    std::memcpy(RwFrameGetMatrix(frame), &mtx, sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(frame));
    const float sx = p.scale * p.scaleX;
    const float sy = p.scale * p.scaleY;
    const float sz = p.scale * p.scaleZ;
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        RwV3d s = { sx, sy, sz };
        RwMatrixScale(RwFrameGetMatrix(frame), &s, rwCOMBINEPRECONCAT);
    }
    RwFrameUpdateObjects(frame);

    CVector lightPos = { bone->pos.x, bone->pos.y, bone->pos.z };
    OrcApplyAttachmentLightingForPed(ped, lightPos);

    if (rendered->rwObject->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(rendered->rwObject);
        if (!clump) return;
        RpClumpForAllAtomics(clump, OrcPrepAtomicCB, nullptr);
        RpClumpRender(clump);
    } else {
        OrcPrepAtomicCB(atomic, nullptr);
        atomic->renderCallBack(atomic);
    }
}

bool OrcIsValidStandardPedModelForLocalApply(int modelId) {
    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    return mi != nullptr && mi->GetModelType() == MODEL_INFO_PED;
}

void OrcObjectsBeginFrame() {
    ++g_standardObjectRenderFrame;
    if (g_standardObjectRenderFrame == 0)
        ++g_standardObjectRenderFrame;
}

void OrcObjectsReleaseAllInstances() {
    for (auto& o : g_customObjects)
        DestroyCustomObjectInstance(o);
    DestroyAllStandardObjectInstances();
}

void OrcObjectsApplyWeaponSuppression(CPed* ped, std::vector<char>* suppress) {
    ApplyObjectWeaponSuppression(ped, suppress);
}

void OrcObjectsPrepassLocalPlayer(CPlayerPed* player, int& active, std::vector<char>& objectUsed) {
    for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
        auto& o = g_customObjects[oi];
        if (!g_renderCustomObjects) {
            DestroyCustomObjectInstance(o);
            continue;
        }
        CustomObjectSkinParams op;
        if (!ResolveObjectSkinParamsForPed(o.iniPath, player, op)) continue;
        if (ShouldRenderObjectForPedWithParams(player, op) && EnsureCustomInstance(o)) {
            active++;
            objectUsed[oi] = 1;
        }
    }
}

void OrcObjectsRenderLocalPlayer(CPlayerPed* player, std::vector<char>& objectUsed) {
    if (g_renderCustomObjects) {
        for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
            auto& o = g_customObjects[oi];
            CustomObjectSkinParams op;
            if (!ResolveObjectSkinParamsForPed(o.iniPath, player, op)) {
                continue;
            }
            if (!ShouldRenderObjectForPedWithParams(player, op)) {
                continue;
            }
            if (!EnsureCustomInstance(o)) continue;
            RenderCustomObject(player, o, op);
            objectUsed[oi] = 1;
        }
    }
    if (g_renderStandardObjects) {
        for (const auto& o : g_standardObjects) {
            CustomObjectSkinParams op;
            if (!ResolveStandardObjectSkinParamsForPed(o.modelId, o.slot, player, op))
                continue;
            if (!ShouldRenderObjectForPedWithParams(player, op))
                continue;
            RenderStandardObject(player, o, op);
        }
    }
}

void OrcObjectsRenderForRemotePed(CPed* ped, std::vector<char>& objectUsed) {
    if (!ped) return;
    if (g_renderAllPedsObjects && g_renderCustomObjects) {
        for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
            auto& o = g_customObjects[oi];
            CustomObjectSkinParams op;
            if (!ResolveObjectSkinParamsForPed(o.iniPath, ped, op))
                continue;
            if (!ShouldRenderObjectForPedWithParams(ped, op))
                continue;
            if (!EnsureCustomInstance(o))
                continue;
            RenderCustomObject(ped, o, op);
            objectUsed[oi] = 1;
        }
    }
    if (g_renderAllPedsObjects && g_renderStandardObjects) {
        for (const auto& o : g_standardObjects) {
            CustomObjectSkinParams op;
            if (!ResolveStandardObjectSkinParamsForPed(o.modelId, o.slot, ped, op))
                continue;
            if (!ShouldRenderObjectForPedWithParams(ped, op))
                continue;
            RenderStandardObject(ped, o, op);
        }
    }
}

void OrcObjectsFinalizeFrame(std::vector<char>& objectUsed) {
    if (g_renderCustomObjects) {
        for (size_t oi = 0; oi < g_customObjects.size(); ++oi) {
            if (!objectUsed[oi]) DestroyCustomObjectInstance(g_customObjects[oi]);
        }
    } else {
        for (auto& o : g_customObjects) DestroyCustomObjectInstance(o);
    }
    if (g_renderStandardObjects) {
        for (auto it = g_renderedStandardObjects.begin(); it != g_renderedStandardObjects.end();) {
            if (it->second.lastSeenFrame != g_standardObjectRenderFrame) {
                DestroyRenderedStandardObject(it->second);
                it = g_renderedStandardObjects.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        DestroyAllStandardObjectInstances();
    }
}

void OrcObjectsWhenSkippingRenderNoActive() {
    if (!g_renderStandardObjects || g_standardObjects.empty())
        DestroyAllStandardObjectInstances();
}

void OrcObjectsDestroyAllStandardInstances() {
    DestroyAllStandardObjectInstances();
}

void OrcObjectsShutdown() {
    for (auto& o : g_customObjects) {
        DestroyCustomObjectInstance(o);
        o.txdSlot = -1;
    }
    DestroyAllStandardObjectInstances();
}
