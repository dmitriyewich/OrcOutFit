// OrcOutFit — custom / standard / random skin discovery, INI, overlay rendering, UI preview.

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CVector.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "eModelInfoType.h"
#include "CPools.h"
#include "CVisibilityPlugins.h"
#include "CTxdStore.h"
#include "CScene.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"
#include "game_sa/rw/rpskin.h"
#include "extensions/ScriptCommands.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_ini.h"
#include "orc_ini_cache.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_render.h"
#include "orc_texture_remap.h"
#include "orc_types.h"
#include "samp_bridge.h"

using namespace plugin;

static std::string StandardSkinsIniPath();

std::vector<CustomSkinCfg> g_customSkins;
std::vector<StandardSkinCfg> g_standardSkins;

static bool g_customSkinLookupDirty = true;
static std::unordered_map<std::string, int> g_customSkinNickLookup;
static int g_selectedSkinCacheIdx = -1;
static std::string g_selectedSkinCacheNameLower;

static bool g_skinCanAnimate = false;
static int g_skinBindCount = 0;

static CPed* g_hiddenPed = nullptr;
static RpClump* g_hiddenClump = nullptr;
static bool g_hideSnapshotValid = false;

void InvalidateCustomSkinLookupCache() {
    for (const auto& s : g_customSkins) {
        if (!s.iniPath.empty())
            OrcIniCacheInvalidatePath(s.iniPath.c_str());
    }
    OrcIniCacheInvalidatePath(StandardSkinsIniPath().c_str());
    g_customSkinLookupDirty = true;
    g_customSkinNickLookup.clear();
    g_selectedSkinCacheIdx = -1;
    g_selectedSkinCacheNameLower.clear();
}

static std::string ToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::string TrimAscii(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
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

static void CreateDefaultSkinIniIfMissing(const std::string& iniPath, const std::string& baseName) {
    if (OrcFileExistsA(iniPath.c_str())) return;
    FILE* f = fopen(iniPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
            "; OrcOutFit custom skin config for %s\n"
            "; Nicks: one per line and/or comma-separated (case-insensitive).\n\n"
            "[NickBinding]\n"
            "Enabled=0\n"
            "Nicks=\n",
            baseName.c_str());
    fclose(f);
}

struct SkinRandomPool {
    int modelId = -1;
    std::string folderName;
    std::vector<CustomSkinCfg> variants;
    std::vector<int> shuffleBag;
};

static std::vector<SkinRandomPool> g_skinRandomPools;
struct PedRandomSkinState {
    int modelId = -1;
    int variant = -1;
};
static std::unordered_map<CPed*, PedRandomSkinState> g_pedRandomSkinIdx;

static void DestroyCustomSkinInstance(CustomSkinCfg& s);
static void DestroyStandardSkinInstance(StandardSkinCfg& s);
static void DestroyAllRandomPoolSkins();

static void PrunePedRandomSkinMap() {
    std::unordered_set<CPed*> alive;
    if (CPools::ms_pPedPool) {
        for (int i = 0; i < CPools::ms_pPedPool->m_nSize; i++) {
            CPed* p = CPools::ms_pPedPool->GetAt(i);
            if (p) alive.insert(p);
        }
    }
    for (auto it = g_pedRandomSkinIdx.begin(); it != g_pedRandomSkinIdx.end();) {
        if (alive.find(it->first) == alive.end())
            it = g_pedRandomSkinIdx.erase(it);
        else
            ++it;
    }
}

static SkinRandomPool* FindRandomPoolForModelId(int modelId) {
    if (modelId < 0) return nullptr;
    for (auto& p : g_skinRandomPools) {
        if (p.modelId == modelId && !p.variants.empty())
            return &p;
    }
    return nullptr;
}

static int FindPedModelIdByDffName(const std::string& dffName) {
    if (dffName.empty()) return -1;
    const std::string want = ToLowerAscii(dffName);
    for (int id = 0; id < (int)g_pedModelNameById.size(); ++id) {
        if (g_pedModelNameById[id].empty()) continue;
        if (ToLowerAscii(g_pedModelNameById[id]) == want)
            return id;
    }
    return -1;
}

static int PopRandomPoolVariant(SkinRandomPool& pool) {
    const int n = (int)pool.variants.size();
    if (n <= 0) return -1;
    if (pool.shuffleBag.empty()) {
        pool.shuffleBag.reserve((size_t)n);
        for (int i = 0; i < n; ++i)
            pool.shuffleBag.push_back(i);
        for (int i = n - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(pool.shuffleBag[(size_t)i], pool.shuffleBag[(size_t)j]);
        }
    }
    const int pick = pool.shuffleBag.back();
    pool.shuffleBag.pop_back();
    return pick;
}

static CustomSkinCfg* ResolveRandomSkinForPed(CPed* ped) {
    if (!g_skinRandomFromPools || !ped)
        return nullptr;
    SkinRandomPool* pool = FindRandomPoolForModelId(ped->m_nModelIndex);
    if (!pool)
        return nullptr;
    const int n = (int)pool->variants.size();
    if (n <= 0)
        return nullptr;
    auto it = g_pedRandomSkinIdx.find(ped);
    if (it == g_pedRandomSkinIdx.end() || it->second.modelId != (int)ped->m_nModelIndex ||
        it->second.variant < 0 || it->second.variant >= n) {
        const int pick = PopRandomPoolVariant(*pool);
        if (pick < 0) return nullptr;
        g_pedRandomSkinIdx[ped] = PedRandomSkinState{ (int)ped->m_nModelIndex, pick };
        it = g_pedRandomSkinIdx.find(ped);
    }
    return &pool->variants[(size_t)it->second.variant];
}

static void EnsureCustomSkinNickLookup() {
    if (!g_customSkinLookupDirty) return;
    g_customSkinNickLookup.clear();
    for (int i = 0; i < (int)g_customSkins.size(); ++i) {
        const CustomSkinCfg& s = g_customSkins[(size_t)i];
        if (!s.bindToNick || s.nicknames.empty()) continue;
        for (const auto& nick : s.nicknames) {
            if (!nick.empty())
                g_customSkinNickLookup.emplace(nick, i);
        }
    }
    g_customSkinLookupDirty = false;
}

static void DestroyCustomSkinInstance(CustomSkinCfg& s) {
    if (!s.rwObject) return;
    if (s.rwObject->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(s.rwObject));
    } else if (s.rwObject->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(s.rwObject);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    s.rwObject = nullptr;
}

static void DestroyStandardSkinInstance(StandardSkinCfg& s) {
    if (!s.rwObject) return;
    if (s.rwObject->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(s.rwObject));
    } else if (s.rwObject->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(s.rwObject);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    s.rwObject = nullptr;
}

static void DestroyAllRandomPoolSkins() {
    for (auto& pool : g_skinRandomPools)
        for (auto& v : pool.variants)
            DestroyCustomSkinInstance(v);
    g_skinRandomPools.clear();
    g_pedRandomSkinIdx.clear();
    g_skinRandomPoolModels = 0;
    g_skinRandomPoolVariants = 0;
}

static bool EnsureCustomSkinLoaded(CustomSkinCfg& s) {
    if (s.rwObject) return true;
    if (!OrcFileExistsA(s.dffPath.c_str())) {
        static std::unordered_set<std::string> s_once;
        if (s_once.insert(s.name).second)
            OrcLogError("skin \"%s\": DFF missing %s", s.name.c_str(), s.dffPath.c_str());
        return false;
    }
    if (s.txdPath.empty() || !OrcFileExistsA(s.txdPath.c_str())) {
        if (!s.txdMissingLogged) {
            s.txdMissingLogged = true;
            OrcLogError("skin \"%s\": TXD missing or invalid path", s.name.c_str());
        }
        return false;
    }

    int txdSlot = CTxdStore::FindTxdSlot(s.name.c_str());
    if (txdSlot == -1) txdSlot = CTxdStore::AddTxdSlot(s.name.c_str());
    if (txdSlot == -1) {
        OrcLogError("skin \"%s\": CTxdStore::AddTxdSlot failed", s.name.c_str());
        return false;
    }
    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)s.txdPath.c_str());
    if (txdStream) {
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        OrcLogError("skin \"%s\": LoadTxd failed", s.name.c_str());
        return false;
    }
    s.txdSlot = txdSlot;

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)s.dffPath.c_str());
    if (!stream) {
        CTxdStore::PopCurrentTxd();
        OrcLogError("skin \"%s\": RwStreamOpen DFF failed", s.name.c_str());
        return false;
    }
    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* c = RpClumpStreamRead(stream);
        if (c) {
            s.rwObject = reinterpret_cast<RwObject*>(c);
            RpClumpForAllAtomics(c, OrcInitAtomicCB, nullptr);
            ok = true;
            OrcLogInfo("skin \"%s\": clump loaded", s.name.c_str());
        }
    } else
        OrcLogError("skin \"%s\": DFF has no CLUMP chunk", s.name.c_str());
    RwStreamClose(stream, nullptr);
    CTxdStore::PopCurrentTxd();
    return ok;
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

bool OrcIsValidStandardSkinModel(int modelId) {
    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    return mi && mi->GetModelType() == MODEL_INFO_PED;
}

static bool EnsureStandardSkinLoaded(StandardSkinCfg& s) {
    if (s.rwObject && !OrcIsValidStandardSkinModel(s.modelId)) {
        DestroyStandardSkinInstance(s);
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: model no longer exists", s.dffName.c_str(), s.modelId);
        }
        return false;
    }
    if (s.rwObject) return true;
    if (!OrcIsValidStandardSkinModel(s.modelId)) {
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: invalid ped model", s.dffName.c_str(), s.modelId);
        }
        return false;
    }

    if (!CStreaming::HasModelLoaded(s.modelId)) {
        CStreaming::RequestModel(s.modelId, 0);
        for (int i = 0; i < 16 && !CStreaming::HasModelLoaded(s.modelId); ++i)
            CStreaming::LoadAllRequestedModels(false);
        if (!CStreaming::HasModelLoaded(s.modelId))
            return false;
    }

    CBaseModelInfo* mi = CModelInfo::GetModelInfo(s.modelId);
    if (!mi || !mi->m_pRwObject)
        return false;

    RwObject* inst = mi->CreateInstance();
    if (!inst || inst->type != rpCLUMP) {
        if (inst && inst->type == rpATOMIC) {
            auto* a = reinterpret_cast<RpAtomic*>(inst);
            RwFrame* f = RpAtomicGetFrame(a);
            RpAtomicDestroy(a);
            if (f) RwFrameDestroy(f);
        }
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: CreateInstance failed or non-clump", s.dffName.c_str(), s.modelId);
        }
        return false;
    }

    s.rwObject = inst;
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(inst), OrcInitAtomicCB, nullptr);
    s.loadFailedLogged = false;
    OrcLogInfo("standard skin \"%s\" [%d]: clump loaded", s.dffName.c_str(), s.modelId);
    return true;
}

static void LoadSkinCfgFromIni(CustomSkinCfg& s) {
    if (s.iniPath.empty() || !OrcFileExistsA(s.iniPath.c_str())) {
        s.bindToNick = false;
        s.nickListCsv.clear();
        s.nicknames.clear();
        return;
    }
    const OrcIniDocument* doc = OrcIniCacheGet(s.iniPath.c_str());
    if (!doc || !doc->IsLoaded()) {
        s.bindToNick = false;
        s.nickListCsv.clear();
        s.nicknames.clear();
        return;
    }
    s.bindToNick = doc->GetInt("NickBinding", "Enabled", 0) != 0;
    s.nickListCsv = doc->GetString("NickBinding", "Nicks", "");
    s.nicknames = ParseNickCsv(s.nickListCsv);
}

void SaveSkinCfgToIni(const CustomSkinCfg& s) {
    if (s.iniPath.empty()) return;
    std::string text;
    text.reserve(128 + s.nickListCsv.size());
    text += "; OrcOutFit custom skin config for ";
    text += s.name;
    text += "\n";
    text += "; Nicks: one per line and/or comma-separated (case-insensitive).\n\n";
    text += "[NickBinding]\n";
    text += "Enabled=";
    text += s.bindToNick ? "1\n" : "0\n";
    text += "Nicks=";
    text += s.nickListCsv;
    text += "\n";
    if (!OrcWriteTextFileAtomic(s.iniPath.c_str(), text)) {
        OrcLogError("SaveSkinCfgToIni: cannot write %s", s.iniPath.c_str());
        return;
    }
    InvalidateCustomSkinLookupCache();
}

static bool g_standardSkinLookupDirty = true;
static std::unordered_map<std::string, int> g_standardSkinNickLookup;

void InvalidateStandardSkinLookupCache() {
    OrcIniCacheInvalidatePath(StandardSkinsIniPath().c_str());
    g_standardSkinLookupDirty = true;
    g_standardSkinNickLookup.clear();
}

static std::string StandardSkinsIniPath() {
    return OrcJoinPath(std::string(g_gameSkinDir), "StandardSkins.ini");
}

static std::string StandardSkinIniSection(const char* dffName) {
    return std::string("Skin.") + (dffName ? dffName : "");
}

StandardSkinCfg* OrcGetStandardSkinCfgByModelId(int modelId, bool createIfMissing) {
    for (auto& s : g_standardSkins) {
        if (s.modelId == modelId) {
            if (!OrcIsValidStandardSkinModel(modelId)) {
                DestroyStandardSkinInstance(s);
                return nullptr;
            }
            return &s;
        }
    }
    if (!createIfMissing || !OrcIsValidStandardSkinModel(modelId))
        return nullptr;
    const char* dff = OrcTryGetPedModelNameById(modelId);
    if (!dff || !dff[0])
        return nullptr;
    StandardSkinCfg s;
    s.modelId = modelId;
    s.dffName = dff;
    g_standardSkins.push_back(std::move(s));
    return &g_standardSkins.back();
}

void LoadStandardSkinsFromIni() {
    for (auto& s : g_standardSkins)
        DestroyStandardSkinInstance(s);
    g_standardSkins.clear();
    InvalidateStandardSkinLookupCache();

    const std::string path = StandardSkinsIniPath();
    if (!OrcFileExistsA(path.c_str())) return;

    const OrcIniDocument* pdoc = OrcIniCacheGet(path.c_str());
    if (!pdoc || !pdoc->IsLoaded()) return;
    const OrcIniDocument& doc = *pdoc;

    const std::string entriesStr = doc.GetString("StandardSkins", "Entries", "");
    std::unordered_set<std::string> seen;
    for (const std::string& token : ParseCsvTokens(entriesStr.c_str())) {
        const std::string dff = TrimAscii(token);
        if (dff.empty()) continue;
        const std::string dffLower = ToLowerAscii(dff);
        if (!seen.insert(dffLower).second) continue;

        const std::string sec = StandardSkinIniSection(dff.c_str());
        const int modelId = doc.GetInt(sec.c_str(), "ModelId", -1);
        if (!OrcIsValidStandardSkinModel(modelId)) continue;

        StandardSkinCfg cfg;
        cfg.modelId = modelId;
        cfg.dffName = dff;
        cfg.bindToNick = doc.GetInt(sec.c_str(), "Enabled", 0) != 0;
        cfg.nickListCsv = doc.GetString(sec.c_str(), "Nicks", "");
        cfg.nicknames = ParseNickCsv(cfg.nickListCsv);
        g_standardSkins.push_back(std::move(cfg));
    }
    OrcLogInfo("LoadStandardSkinsFromIni: %zu entries from %s", g_standardSkins.size(), path.c_str());
}

static void EnsureStandardSkinNickLookup() {
    if (!g_standardSkinLookupDirty) return;
    g_standardSkinNickLookup.clear();
    for (const auto& s : g_standardSkins) {
        if (!OrcIsValidStandardSkinModel(s.modelId)) continue;
        if (!s.bindToNick || s.nicknames.empty()) continue;
        for (const auto& nick : s.nicknames) {
            if (!nick.empty())
                g_standardSkinNickLookup.emplace(nick, s.modelId);
        }
    }
    g_standardSkinLookupDirty = false;
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

void SaveStandardSkinCfgToIni(const StandardSkinCfg& s) {
    if (s.modelId < 0 || s.dffName.empty()) return;
    if (!OrcIsValidStandardSkinModel(s.modelId)) return;
    const std::string path = StandardSkinsIniPath();

    std::string entriesBuf;
    if (const OrcIniDocument* doc = OrcIniCacheGet(path.c_str()); doc && doc->IsLoaded())
        entriesBuf = doc->GetString("StandardSkins", "Entries", "");
    std::vector<std::string> entries = ParseCsvTokens(entriesBuf.c_str());
    const std::string wantLower = ToLowerAscii(s.dffName);
    bool found = false;
    for (std::string& entry : entries) {
        if (ToLowerAscii(TrimAscii(entry)) == wantLower) {
            entry = s.dffName;
            found = true;
            break;
        }
    }
    if (!found)
        entries.push_back(s.dffName);

    std::string entriesCsv;
    for (const std::string& entry : entries) {
        const std::string trimmed = TrimAscii(entry);
        if (trimmed.empty()) continue;
        if (!entriesCsv.empty()) entriesCsv += ",";
        entriesCsv += trimmed;
    }

    const std::string sec = StandardSkinIniSection(s.dffName.c_str());
    std::vector<OrcIniValue> values;
    AddIniValue(values, "StandardSkins", "Entries", entriesCsv.c_str());
    AddIniInt(values, sec.c_str(), "ModelId", s.modelId);
    AddIniInt(values, sec.c_str(), "Enabled", s.bindToNick ? 1 : 0);
    AddIniValue(values, sec.c_str(), "Nicks", s.nickListCsv.c_str());
    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit standard game skin config.\n\n", values))
        OrcLogError("SaveStandardSkinCfgToIni: cannot write %s", path.c_str());
    InvalidateStandardSkinLookupCache();
}

void OrcAppendSkinFeatureIniValues(std::vector<OrcIniValue>& values) {
    AddIniInt(values, "Features", "SkinMode", g_skinModeEnabled ? 1 : 0);
    AddIniInt(values, "Features", "SkinHideBasePed", g_skinHideBasePed ? 1 : 0);
    AddIniInt(values, "Features", "SkinNickMode", g_skinNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinLocalPreferSelected", g_skinLocalPreferSelected ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemap", g_skinTextureRemapEnabled ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapNickMode", g_skinTextureRemapNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapAutoNickMode", g_skinTextureRemapAutoNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapRandomMode", g_skinTextureRemapRandomMode);
}

void OrcAppendSkinModeIniValues(std::vector<OrcIniValue>& values) {
    AddIniValue(values, "SkinMode", "Selected", g_skinSelectedName.c_str());
    AddIniValue(values, "SkinMode", "SelectedSource", g_skinSelectedSource == SKIN_SELECTED_STANDARD ? "standard" : "custom");
    AddIniInt(values, "SkinMode", "StandardSelected", g_standardSkinSelectedModelId);
    AddIniInt(values, "SkinMode", "RandomFromPools", g_skinRandomFromPools ? 1 : 0);
}

void SaveSkinModeIni() {
    std::vector<OrcIniValue> values;
    OrcAppendSkinFeatureIniValues(values);
    OrcAppendSkinModeIniValues(values);
    if (!OrcIniWriteValues(g_iniPath, "; OrcOutFit configuration.\n\n", values))
        OrcLogError("SaveSkinModeIni: cannot write %s", g_iniPath);
}

static void DiscoverRandomSkinPools() {
    DestroyAllRandomPoolSkins();
    const std::string randomDir = OrcJoinPath(std::string(g_gameSkinDir), "Random");
    DWORD attr = GetFileAttributesA(randomDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;

    std::string mask = OrcJoinPath(randomDir, "*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string folder = fd.cFileName;
        if (folder == "." || folder == "..") continue;

        const int modelId = FindPedModelIdByDffName(folder);
        if (!OrcIsValidStandardSkinModel(modelId)) continue;

        const std::string poolDir = OrcJoinPath(randomDir, folder);
        std::string fileMask = OrcJoinPath(poolDir, "*.*");
        WIN32_FIND_DATAA fileData{};
        HANDLE hf = FindFirstFileA(fileMask.c_str(), &fileData);
        if (hf == INVALID_HANDLE_VALUE) continue;

        SkinRandomPool pool;
        pool.modelId = modelId;
        pool.folderName = folder;

        do {
            if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string fname = fileData.cFileName;
            if (OrcLowerExt(fname) != ".dff") continue;
            const std::string base = OrcBaseNameNoExt(fname);
            CustomSkinCfg s;
            s.name = folder + "/" + base;
            s.dffPath = OrcJoinPath(poolDir, fname);
            s.txdPath = OrcFindBestTxdPath(poolDir, base);
            s.iniPath.clear();
            s.remapKey = base;
            s.remapFallbackKey = folder;
            pool.variants.push_back(std::move(s));
        } while (FindNextFileA(hf, &fileData));
        FindClose(hf);

        if (!pool.variants.empty())
            g_skinRandomPools.push_back(std::move(pool));
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    g_skinRandomPoolModels = (int)g_skinRandomPools.size();
    g_skinRandomPoolVariants = 0;
    for (const auto& pool : g_skinRandomPools)
        g_skinRandomPoolVariants += (int)pool.variants.size();
    OrcLogInfo("DiscoverRandomSkinPools: %d pools, %d variants in %s",
               g_skinRandomPoolModels, g_skinRandomPoolVariants, randomDir.c_str());
}

void DiscoverCustomSkins() {
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    g_customSkins.clear();
    InvalidateCustomSkinLookupCache();
    DestroyAllRandomPoolSkins();
    DWORD attr = GetFileAttributesA(g_gameSkinDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            OrcLogInfo("Skins folder missing or not a directory: %s", g_gameSkinDir);
        }
        return;
    }
    std::string dir = g_gameSkinDir;
    std::string mask = OrcJoinPath(dir, "*.*");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fname = fd.cFileName;
        if (OrcLowerExt(fname) != ".dff") continue;
        const std::string base = OrcBaseNameNoExt(fname);
        CustomSkinCfg s;
        s.name = base;
        s.dffPath = OrcJoinPath(dir, base + ".dff");
        s.txdPath = OrcFindBestTxdPath(dir, base);
        s.iniPath = OrcJoinPath(dir, base + ".ini");
        s.remapKey = base;
        CreateDefaultSkinIniIfMissing(s.iniPath, base);
        LoadSkinCfgFromIni(s);
        g_customSkins.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    OrcLogInfo("DiscoverCustomSkins: %zu skins in %s", g_customSkins.size(), g_gameSkinDir);
    DiscoverRandomSkinPools();
    if (!g_skinSelectedName.empty()) {
        for (int i = 0; i < (int)g_customSkins.size(); i++) {
            if (ToLowerAscii(g_customSkins[i].name) == ToLowerAscii(g_skinSelectedName)) {
                g_uiSkinIdx = i;
                break;
            }
        }
    }
    if (g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
    g_uiSkinEditIdx = -1;
}

void OrcCollectRandomSkinPools(std::vector<SkinRandomPoolInfo>& out) {
    out.clear();
    for (const auto& pool : g_skinRandomPools) {
        SkinRandomPoolInfo info;
        info.dffName = pool.folderName;
        info.modelId = pool.modelId;
        info.variants = (int)pool.variants.size();
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SkinRandomPoolInfo& a, const SkinRandomPoolInfo& b) {
        return a.modelId < b.modelId;
    });
}

void OrcCollectPedSkins(std::vector<std::pair<std::string, int>>& out) {
    out.clear();
    for (int id = 0; id < (int)g_pedModelNameById.size(); id++) {
        if (g_pedModelNameById[id].empty()) continue;
        if (!OrcIsValidStandardSkinModel(id)) continue;
        out.push_back({ g_pedModelNameById[id], id });
    }
    std::sort(out.begin(), out.end(), [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
        return a.second < b.second;
    });
}

static void RenderSkinOnPed(CPed* ped, CustomSkinCfg* sel, bool isLocalPed);
static void RenderStandardSkinOnPed(CPed* ped, StandardSkinCfg* sel, bool isLocalPed);
// Skin preview tab/renderer removed.
void OrcSkinsDestroyPreview() {}

static CustomSkinCfg* GetSelectedSkin() {
    if (g_customSkins.empty()) return nullptr;
    if (!g_skinSelectedName.empty()) {
        const std::string selectedLower = ToLowerAscii(g_skinSelectedName);
        if (g_selectedSkinCacheIdx >= 0 &&
            g_selectedSkinCacheIdx < (int)g_customSkins.size() &&
            g_selectedSkinCacheNameLower == selectedLower &&
            ToLowerAscii(g_customSkins[(size_t)g_selectedSkinCacheIdx].name) == selectedLower) {
            return &g_customSkins[(size_t)g_selectedSkinCacheIdx];
        }

        for (int i = 0; i < (int)g_customSkins.size(); ++i) {
            if (ToLowerAscii(g_customSkins[(size_t)i].name) == selectedLower) {
                g_selectedSkinCacheIdx = i;
                g_selectedSkinCacheNameLower = selectedLower;
                return &g_customSkins[(size_t)i];
            }
        }
    }
    if (g_uiSkinIdx < 0 || g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
    g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
    g_selectedSkinCacheIdx = g_uiSkinIdx;
    g_selectedSkinCacheNameLower = ToLowerAscii(g_skinSelectedName);
    return &g_customSkins[g_uiSkinIdx];
}

static void CopySkinHierarchyPose(CPed* player, RpClump* skinClump) {
    RpHAnimHierarchy* src = GetAnimHierarchyFromSkinClump(player->m_pRwClump);
    RpHAnimHierarchy* dst = GetAnimHierarchyFromSkinClump(skinClump);
    if (!src || !dst || !src->pMatrixArray || !dst->pMatrixArray) return;
    for (int i = 0; i < src->numNodes; i++) {
        int nodeId = src->pNodeInfo ? src->pNodeInfo[i].nodeID : -1;
        if (nodeId < 0) continue;
        int di = RpHAnimIDGetIndex(dst, nodeId);
        if (di < 0 || di >= dst->numNodes) continue;
        dst->pMatrixArray[di] = src->pMatrixArray[i];
    }
    RpHAnimHierarchyUpdateMatrices(dst);

    if (src->pNodeInfo && dst->pNodeInfo) {
        for (int i = 0; i < src->numNodes; i++) {
            const int nodeId = src->pNodeInfo[i].nodeID;
            const int di = RpHAnimIDGetIndex(dst, nodeId);
            if (di < 0 || di >= dst->numNodes) continue;
            RwFrame* sf = src->pNodeInfo[i].pFrame;
            RwFrame* df = dst->pNodeInfo[di].pFrame;
            if (!sf || !df) continue;
            std::memcpy(RwFrameGetMatrix(df), RwFrameGetMatrix(sf), sizeof(RwMatrix));
            RwMatrixUpdate(RwFrameGetMatrix(df));
        }
    }
}

static RpAtomic* BindSkinHierarchyCB(RpAtomic* a, void* data) {
    if (!a || !data) return a;
    if (RpSkinAtomicGetSkin(a)) {
        RpSkinAtomicSetHAnimHierarchy(a, reinterpret_cast<RpHAnimHierarchy*>(data));
        RpSkinAtomicSetType(a, rpSKINTYPEGENERIC);
        g_skinBindCount++;
    }
    return a;
}

static bool SkinMatchesNickname(const CustomSkinCfg& s, const std::string& nickLower) {
    if (!s.bindToNick || s.nicknames.empty()) return false;
    for (const auto& n : s.nicknames)
        if (n == nickLower) return true;
    return false;
}

static bool StandardSkinMatchesNickname(const StandardSkinCfg& s, const std::string& nickLower) {
    if (!s.bindToNick || s.nicknames.empty()) return false;
    for (const auto& n : s.nicknames)
        if (n == nickLower) return true;
    return false;
}

static CustomSkinCfg* FindNickSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    EnsureCustomSkinNickLookup();
    auto it = g_customSkinNickLookup.find(nickLower);
    if (it == g_customSkinNickLookup.end()) return nullptr;
    const int idx = it->second;
    if (idx < 0 || idx >= (int)g_customSkins.size()) return nullptr;
    if (!SkinMatchesNickname(g_customSkins[(size_t)idx], nickLower)) return nullptr;
    return &g_customSkins[(size_t)idx];
}

static StandardSkinCfg* FindNickStandardSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    EnsureStandardSkinNickLookup();
    auto it = g_standardSkinNickLookup.find(nickLower);
    if (it == g_standardSkinNickLookup.end()) return nullptr;
    StandardSkinCfg* skin = OrcGetStandardSkinCfgByModelId(it->second, false);
    if (!skin || !StandardSkinMatchesNickname(*skin, nickLower)) return nullptr;
    return skin;
}

static StandardSkinCfg* GetSelectedStandardSkin() {
    if (g_standardSkinSelectedModelId >= 0) {
        if (StandardSkinCfg* skin = OrcGetStandardSkinCfgByModelId(g_standardSkinSelectedModelId, true))
            return skin;
    }
    std::vector<std::pair<std::string, int>> skins;
    OrcCollectPedSkins(skins);
    if (skins.empty()) return nullptr;
    g_standardSkinSelectedModelId = skins.front().second;
    return OrcGetStandardSkinCfgByModelId(g_standardSkinSelectedModelId, true);
}

struct ResolvedPedSkin {
    CustomSkinCfg* custom = nullptr;
    StandardSkinCfg* standard = nullptr;
    bool isLocalPed = false;
};

static ResolvedPedSkin ResolveSkinForPed(CPed* ped, CPlayerPed* localPlayer) {
    ResolvedPedSkin result;
    if (!ped || (!g_skinModeEnabled && !g_skinRandomFromPools)) return result;
    CustomSkinCfg* selectedCustom = (g_skinModeEnabled && g_skinSelectedSource == SKIN_SELECTED_CUSTOM) ? GetSelectedSkin() : nullptr;
    StandardSkinCfg* selectedStandard = (g_skinModeEnabled && g_skinSelectedSource == SKIN_SELECTED_STANDARD) ? GetSelectedStandardSkin() : nullptr;
    const bool isLocalByPtr = (localPlayer && ped == localPlayer);

    if (g_skinNickMode && samp_bridge::IsSampBuildKnown()) {
        char nick[32] = {};
        bool isLocalBySamp = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocalBySamp)) {
            const bool isLocal = isLocalBySamp || isLocalByPtr;
            result.isLocalPed = isLocal;
            CustomSkinCfg* nickSkin = FindNickSkin(ToLowerAscii(nick));
            if (nickSkin) {
                result.custom = nickSkin;
                return result;
            }
            if (StandardSkinCfg* standardNickSkin = FindNickStandardSkin(ToLowerAscii(nick))) {
                result.standard = standardNickSkin;
                return result;
            }
            if (isLocal && g_skinModeEnabled && g_skinLocalPreferSelected) {
                result.custom = selectedCustom;
                result.standard = selectedStandard;
                return result;
            }
        }
    }

    if (isLocalByPtr && g_skinLocalPreferSelected) {
        result.isLocalPed = true;
        result.custom = selectedCustom;
        result.standard = selectedStandard;
        return result;
    }

    if (isLocalByPtr) result.isLocalPed = true;
    if (CustomSkinCfg* randomSkin = ResolveRandomSkinForPed(ped)) {
        result.custom = randomSkin;
        result.standard = nullptr;
    }
    return result;
}

static void RenderSkinOnPed(CPed* ped, CustomSkinCfg* sel, bool isLocalPed) {
    if (!ped || !ped->m_pRwClump || !sel) return;
    RpClump* pedClump = ped->m_pRwClump;
    if (!EnsureCustomSkinLoaded(*sel)) return;
    if (!sel->rwObject || sel->rwObject->type != rpCLUMP) return;
    RpClump* clump = reinterpret_cast<RpClump*>(sel->rwObject);
    if (!clump) return;
    RwFrame* srcFrame = RpClumpGetFrame(pedClump);
    RwFrame* dstFrame = RpClumpGetFrame(clump);
    if (!srcFrame || !dstFrame) return;
    std::memcpy(RwFrameGetMatrix(dstFrame), RwFrameGetMatrix(srcFrame), sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(dstFrame));
    RpHAnimHierarchy* srcH = GetAnimHierarchyFromSkinClump(pedClump);
    g_skinBindCount = 0;
    if (srcH && clump) RpClumpForAllAtomics(clump, BindSkinHierarchyCB, srcH);
    const bool canAnimate = (srcH && g_skinBindCount > 0);
    if (isLocalPed) g_skinCanAnimate = canAnimate;
    if (canAnimate) {
        if (ped->m_pRwClump != pedClump) return;
        CopySkinHierarchyPose(ped, clump);
    }
    RwFrameUpdateObjects(dstFrame);
    const bool lit = OrcTryPedSetupLighting(ped);
    static int s_setupLogLeft = 8;
    if (g_orcLogLevel >= OrcLogLevel::Info && s_setupLogLeft > 0) {
        OrcLogInfo("SetupLighting skin=%s -> %d ped=%p", sel->name.c_str(), lit ? 1 : 0, ped);
        s_setupLogLeft--;
    }
    CVector boundCentre{};
    ped->GetBoundCentre(boundCentre);
    static int s_fallbackLogLeft = 8;
    if (g_orcLogLevel >= OrcLogLevel::Info && !lit && s_fallbackLogLeft > 0) {
        OrcLogInfo("skin: SetupLighting false, ApplyAttachment only name=%s", sel->name.c_str());
        s_fallbackLogLeft--;
    }
    OrcApplyAttachmentLightingForPed(ped, boundCentre, 1.0f);
    const char* remapKey = sel->remapKey.empty() ? sel->name.c_str() : sel->remapKey.c_str();
    const char* remapFallback = sel->remapFallbackKey.empty() ? nullptr : sel->remapFallbackKey.c_str();
    OrcTextureRemapApplyToClumpBefore(ped, clump, remapKey, remapFallback, sel->txdSlot);
    OrcTryRpClumpRender(clump);
    OrcTextureRemapRestoreAfter();
    if (lit)
        OrcTryPedRemoveLighting(ped);
}

static void RenderStandardSkinOnPed(CPed* ped, StandardSkinCfg* sel, bool isLocalPed) {
    if (!ped || !ped->m_pRwClump || !sel) return;
    RpClump* pedClump = ped->m_pRwClump;
    if (!EnsureStandardSkinLoaded(*sel)) return;
    if (!sel->rwObject || sel->rwObject->type != rpCLUMP) return;
    RpClump* clump = reinterpret_cast<RpClump*>(sel->rwObject);
    if (!clump) return;
    RwFrame* srcFrame = RpClumpGetFrame(pedClump);
    RwFrame* dstFrame = RpClumpGetFrame(clump);
    if (!srcFrame || !dstFrame) return;
    std::memcpy(RwFrameGetMatrix(dstFrame), RwFrameGetMatrix(srcFrame), sizeof(RwMatrix));
    RwMatrixUpdate(RwFrameGetMatrix(dstFrame));
    RpHAnimHierarchy* srcH = GetAnimHierarchyFromSkinClump(pedClump);
    g_skinBindCount = 0;
    if (srcH && clump) RpClumpForAllAtomics(clump, BindSkinHierarchyCB, srcH);
    const bool canAnimate = (srcH && g_skinBindCount > 0);
    if (isLocalPed) g_skinCanAnimate = canAnimate;
    if (canAnimate) {
        if (ped->m_pRwClump != pedClump) return;
        CopySkinHierarchyPose(ped, clump);
    }
    RwFrameUpdateObjects(dstFrame);
    const bool lit = OrcTryPedSetupLighting(ped);
    static int s_setupLogLeft = 8;
    if (g_orcLogLevel >= OrcLogLevel::Info && s_setupLogLeft > 0) {
        OrcLogInfo("SetupLighting standard skin=%s[%d] -> %d ped=%p",
                   sel->dffName.c_str(), sel->modelId, lit ? 1 : 0, ped);
        s_setupLogLeft--;
    }
    CVector boundCentre{};
    ped->GetBoundCentre(boundCentre);
    OrcApplyAttachmentLightingForPed(ped, boundCentre, 1.0f);
    int txdIndex = -1;
    if (CBaseModelInfo* mi = CModelInfo::GetModelInfo(sel->modelId))
        txdIndex = mi->m_nTxdIndex;
    OrcTextureRemapApplyToClumpBefore(ped, clump, sel->dffName.c_str(), nullptr, txdIndex);
    OrcTryRpClumpRender(clump);
    OrcTextureRemapRestoreAfter();
    if (lit)
        OrcTryPedRemoveLighting(ped);
}

void OrcSkinsRenderForPeds(CPlayerPed* localPlayer) {
    if (!localPlayer || (!g_skinModeEnabled && !g_skinRandomFromPools)) return;
    g_skinCanAnimate = false;
    if (g_skinRandomFromPools)
        PrunePedRandomSkinMap();
    bool localDone = false;
    if (!CPools::ms_pPedPool) return;
    for (int i = 0; i < CPools::ms_pPedPool->m_nSize; i++) {
        CPed* ped = CPools::ms_pPedPool->GetAt(i);
        if (!ped || !ped->m_pRwClump) continue;
        ResolvedPedSkin skin = ResolveSkinForPed(ped, localPlayer);
        if (!skin.custom && !skin.standard) continue;
        if (skin.custom)
            RenderSkinOnPed(ped, skin.custom, skin.isLocalPed);
        else
            RenderStandardSkinOnPed(ped, skin.standard, skin.isLocalPed);
        if (skin.isLocalPed) localDone = true;
    }
    if (!localDone) {
        ResolvedPedSkin skin = ResolveSkinForPed(localPlayer, localPlayer);
        if (skin.custom)
            RenderSkinOnPed(localPlayer, skin.custom, true);
        else if (skin.standard)
            RenderStandardSkinOnPed(localPlayer, skin.standard, true);
    }
}

bool OrcSkinsLocalSelectionAddsActiveWork() {
    if (!g_skinModeEnabled) return false;
    if (g_skinSelectedSource == SKIN_SELECTED_STANDARD)
        return GetSelectedStandardSkin() != nullptr;
    return GetSelectedSkin() != nullptr;
}

void OrcSkinsOnPedRenderBefore(CPed* ped) {
    if ((!g_skinModeEnabled && !g_skinRandomFromPools) || !g_skinHideBasePed) return;
    CPlayerPed* player = FindPlayerPed(0);
    if (!ped || !ped->m_pRwClump) return;
    ResolvedPedSkin skin = ResolveSkinForPed(ped, player);
    if (!skin.custom && !skin.standard) return;
    if (skin.custom) {
        if (!EnsureCustomSkinLoaded(*skin.custom)) return;
    } else {
        if (!EnsureStandardSkinLoaded(*skin.standard)) return;
    }
    g_hiddenPed = ped;
    g_hiddenClump = ped->m_pRwClump;
    __try {
        g_hideSnapshotValid = true;
        CVisibilityPlugins::SetClumpAlpha(g_hiddenClump, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("OnPedRenderBefore: SetClumpAlpha SEH ex=0x%08X", GetExceptionCode());
        g_hideSnapshotValid = false;
        g_hiddenPed = nullptr;
        g_hiddenClump = nullptr;
    }
}

void OrcSkinsOnPedRenderAfter(CPed* ped) {
    if (!g_hideSnapshotValid) return;
    if (!ped || ped != g_hiddenPed) return;

    __try {
        if (g_hiddenClump) CVisibilityPlugins::SetClumpAlpha(g_hiddenClump, 255);
        if (ped->m_pRwClump && ped->m_pRwClump != g_hiddenClump)
            CVisibilityPlugins::SetClumpAlpha(ped->m_pRwClump, 255);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_hideSnapshotValid = false;
    g_hiddenPed = nullptr;
    g_hiddenClump = nullptr;
}

void OrcSkinsReleaseAllInstancesAndPreview() {
    OrcSkinsDestroyPreview();
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    for (auto& s : g_standardSkins) DestroyStandardSkinInstance(s);
    DestroyAllRandomPoolSkins();
}

void OrcSkinsShutdown() {
    OrcSkinsDestroyPreview();
    g_hideSnapshotValid = false;
    g_hiddenPed = nullptr;
    g_hiddenClump = nullptr;
    for (auto& s : g_customSkins) {
        DestroyCustomSkinInstance(s);
        s.txdSlot = -1;
    }
    for (auto& s : g_standardSkins)
        DestroyStandardSkinInstance(s);
    DestroyAllRandomPoolSkins();
}
