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
#include <array>
#include <cstdint>
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
#include "orc_random_pick.h"
#include "orc_render.h"
#include "orc_skin_native.h"
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
static std::string g_selectedSkinCacheName;

namespace {
struct OverlaySkeleton {
    RpHAnimHierarchy* hierarchy = nullptr;
    RpHAnimHierarchy alternate{};
    bool useAlternate = false;
    bool drawing = false;
};

std::unordered_map<RpClump*, OverlaySkeleton> g_overlaySkeletons;
OverlaySkeleton* g_drawingOverlaySkeleton = nullptr;
std::unordered_set<CustomSkinCfg*> g_loadedCustomSkins;
bool g_skinTxdPoolFull = false;
std::vector<CPed*> g_overlayRenderPeds;
bool g_overlayRenderHookInstalled = false;
}

void InvalidateCustomSkinLookupCache() {
    g_customSkinLookupDirty = true;
    g_customSkinNickLookup.clear();
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

static constexpr char kCustomSkinIniHeader[] =
    "; OrcOutFit custom skin config.\n"
    "; Nicks: comma-separated (case-insensitive).\n\n";

static void CreateDefaultSkinIniIfMissing(const std::string& iniPath) {
    if (OrcFileExistsA(iniPath.c_str()))
        return;
    const std::vector<OrcIniValue> values = {
        {"NickBinding", "Enabled", "0"},
        {"NickBinding", "Nicks", ""},
    };
    if (!OrcIniWriteValues(iniPath.c_str(), kCustomSkinIniHeader, values))
        OrcLogInfoThrottled(948u, 5000u, "skin: cannot create default INI %s", iniPath.c_str());
}

struct SkinRandomPool {
    std::string folderName;
    std::vector<CustomSkinCfg> variants;
    std::vector<int> choices;
    bool choicesDirty = true;
    bool choicesIncludeVanilla = false;
    std::vector<int> shuffleBag;
    OrcSequentialPickState sequentialState;
    OrcRandomNoRepeatPickState noRepeatState;
    std::unordered_map<std::string, int> trackedAssignments;
};

static constexpr int kSkinRandomVanillaChoice = -1;
static constexpr int kSkinRandomInvalidChoice = -2;

static std::unordered_map<int, SkinRandomPool> g_skinRandomPools;
struct PedRandomSkinState {
    int pedRef;
    int modelId;
    int variant;
};
static std::unordered_map<CPed*, PedRandomSkinState> g_pedRandomSkinIdx;
static bool g_skinRandomStateEnabled = false;
static int g_skinRandomStatePickMode = ORC_RANDOM_PICK_RANDOM;

static const char* SkinRandomPickModeToIni(int mode) {
    if (mode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT)
        return "random_no_repeat";
    return mode == ORC_RANDOM_PICK_SEQUENTIAL ? "sequential" : "random";
}

static SkinRandomPool* FindRandomPoolForModelId(int modelId) {
    const auto it = g_skinRandomPools.find(modelId);
    return it != g_skinRandomPools.end() ? &it->second : nullptr;
}

static bool IsValidSkinRandomChoice(const SkinRandomPool& pool, int choice) {
    if (choice == kSkinRandomVanillaChoice)
        return g_skinRandomIncludeVanilla;
    return choice >= 0 && choice < static_cast<int>(pool.variants.size()) &&
        !pool.variants[static_cast<size_t>(choice)].loadFailed;
}

static CPed* GetRandomPedByRef(int pedRef) {
    auto* pool = CPools::ms_pPedPool;
    // CPool encodes a free slot in the high bit of the reference's low byte.
    if (!pool || pedRef < 0 || (pedRef & 0x80))
        return nullptr;
    __try {
        // The first pool slot may also have generation zero after the counter wraps.
        return pool->GetAtRef(pedRef);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void ReleasePedRandomSkinTrackedChoice(const PedRandomSkinState& state) {
    if (g_skinRandomStatePickMode == ORC_RANDOM_PICK_RANDOM)
        return;
    SkinRandomPool* pool = FindRandomPoolForModelId(state.modelId);
    if (!pool)
        return;
    const std::string key = std::to_string(state.pedRef);
    if (g_skinRandomStatePickMode == ORC_RANDOM_PICK_SEQUENTIAL) {
        OrcSequentialEraseChoice(pool->sequentialState, pool->trackedAssignments, key);
    } else {
        OrcRandomNoRepeatEraseChoice(pool->noRepeatState, pool->trackedAssignments, key,
            IsValidSkinRandomChoice(*pool, state.variant));
    }
}

static void PrunePedRandomSkinMap() {
    for (auto it = g_pedRandomSkinIdx.begin(); it != g_pedRandomSkinIdx.end();) {
        if (GetRandomPedByRef(it->second.pedRef) != it->first) {
            ReleasePedRandomSkinTrackedChoice(it->second);
            it = g_pedRandomSkinIdx.erase(it);
        } else {
            ++it;
        }
    }
}

static void ResetRandomSkinSelections() {
    g_pedRandomSkinIdx.clear();
    for (auto& entry : g_skinRandomPools) {
        SkinRandomPool& pool = entry.second;
        pool.shuffleBag.clear();
        pool.sequentialState = {};
        pool.noRepeatState = {};
        pool.trackedAssignments.clear();
    }
}

static void SyncRandomSkinSelectionSettings() {
    const bool enabled = g_enabled && g_skinRandomFromPools;
    if (g_skinRandomStateEnabled == enabled && g_skinRandomStatePickMode == g_skinRandomPickMode)
        return;
    ResetRandomSkinSelections();
    g_skinRandomStateEnabled = enabled;
    g_skinRandomStatePickMode = g_skinRandomPickMode;
}

void OrcSkinsPruneRandomSelections() {
    SyncRandomSkinSelectionSettings();
    if (g_pedRandomSkinIdx.empty()) return;
    static DWORD lastPrune = 0;
    const DWORD now = GetTickCount();
    if (now - lastPrune >= 500u) {
        lastPrune = now;
        PrunePedRandomSkinMap();
    }
}

static void PrepareSkinRandomPoolChoices(SkinRandomPool& pool) {
    if (!pool.choicesDirty && pool.choicesIncludeVanilla == g_skinRandomIncludeVanilla)
        return;
    pool.choicesDirty = false;
    pool.choicesIncludeVanilla = g_skinRandomIncludeVanilla;
    pool.choices.clear();
    pool.choices.reserve(pool.variants.size() + (g_skinRandomIncludeVanilla ? 1u : 0u));
    if (g_skinRandomIncludeVanilla)
        pool.choices.push_back(kSkinRandomVanillaChoice);
    for (int i = 0; i < static_cast<int>(pool.variants.size()); ++i) {
        if (IsValidSkinRandomChoice(pool, i))
            pool.choices.push_back(i);
    }
    pool.shuffleBag.clear();
}

static int PickSkinRandomVariant(SkinRandomPool& pool, int pedRef) {
    PrepareSkinRandomPoolChoices(pool);
    if (pool.choices.empty())
        return kSkinRandomInvalidChoice;

    if (g_skinRandomStatePickMode == ORC_RANDOM_PICK_SEQUENTIAL) {
        return OrcSequentialPickSticky(pool.sequentialState, pool.trackedAssignments,
            std::to_string(pedRef), pool.choices, kSkinRandomInvalidChoice);
    }
    if (g_skinRandomStatePickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        return OrcRandomNoRepeatPickSticky(pool.noRepeatState, pool.trackedAssignments,
            std::to_string(pedRef), pool.choices, kSkinRandomInvalidChoice);
    }
    if (pool.shuffleBag.empty()) {
        pool.shuffleBag = pool.choices;
        OrcRandomNoRepeatShuffle(pool.shuffleBag);
    }
    const int choice = pool.shuffleBag.back();
    pool.shuffleBag.pop_back();
    return choice;
}

static bool ResolveRandomSkinForPedModel(CPed* ped, int modelId, CustomSkinCfg** outSkin) {
    SyncRandomSkinSelectionSettings();
    if (!g_skinRandomStateEnabled || !ped)
        return false;

    SkinRandomPool* pool = FindRandomPoolForModelId(modelId);
    auto it = g_pedRandomSkinIdx.find(ped);
    if (!pool && it == g_pedRandomSkinIdx.end())
        return false;

    const int pedRef = OrcSafeGetPedRef(ped);
    if (it != g_pedRandomSkinIdx.end() &&
        (it->second.pedRef != pedRef || it->second.modelId != modelId || !pool ||
         !IsValidSkinRandomChoice(*pool, it->second.variant))) {
        if (pool && it->second.modelId == modelId && it->second.variant >= 0 &&
            !IsValidSkinRandomChoice(*pool, it->second.variant)) {
            pool->choicesDirty = true;
        }
        ReleasePedRandomSkinTrackedChoice(it->second);
        g_pedRandomSkinIdx.erase(it);
        it = g_pedRandomSkinIdx.end();
    }
    if (!pool)
        return false;
    if (it == g_pedRandomSkinIdx.end()) {
        if (GetRandomPedByRef(pedRef) != ped)
            return false;
        if (g_skinRandomStatePickMode != ORC_RANDOM_PICK_RANDOM)
            PrunePedRandomSkinMap();
        int pick = PickSkinRandomVariant(*pool, pedRef);
        // A different ped may have discovered a broken asset since this pool was built.
        if (pick != kSkinRandomInvalidChoice && !IsValidSkinRandomChoice(*pool, pick)) {
            ReleasePedRandomSkinTrackedChoice({pedRef, modelId, pick});
            pool->choicesDirty = true;
            pick = PickSkinRandomVariant(*pool, pedRef);
        }
        if (pick == kSkinRandomInvalidChoice)
            return false;
        it = g_pedRandomSkinIdx.emplace(ped, PedRandomSkinState{pedRef, modelId, pick}).first;
    }

    if (it->second.variant != kSkinRandomVanillaChoice)
        *outSkin = &pool->variants[static_cast<size_t>(it->second.variant)];
    return true;
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

void OrcReleaseCustomSkinTxd(int txdSlot) {
    auto* pool = CTxdStore::ms_pTxdPool;
    if (!pool || txdSlot < 0 || txdSlot >= pool->m_nSize)
        return;
    const TxdDef* txd = pool->GetAt(txdSlot);
    if (!txd || !txd->m_wRefsCount)
        return;

    __try {
        if (CTxdStore::RemoveRefWithoutDelete(txdSlot)->m_wRefsCount == 0) {
            CTxdStore::RemoveTxd(txdSlot);
            CTxdStore::RemoveTxdSlot(txdSlot);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("skin: private TXD release SEH ex=0x%08X slot=%d", GetExceptionCode(), txdSlot);
    }
}

static void DestroyCustomSkinInstance(CustomSkinCfg& s) {
    if (s.rwObject) {
        auto* clump = reinterpret_cast<RpClump*>(s.rwObject);
        OrcTextureRemapForgetOverlayClump(clump, s.txdSlot);
        g_overlaySkeletons.erase(clump);
        g_loadedCustomSkins.erase(&s);
    }
    OrcDestroyRwObjectInstance(s.rwObject);
    OrcReleaseCustomSkinTxd(s.txdSlot);
    s.txdSlot = -1;
    s.retryLoadAt = 0;
}

static void DestroyStandardSkinInstance(StandardSkinCfg& s) {
    if (s.rwObject) {
        auto* clump = reinterpret_cast<RpClump*>(s.rwObject);
        OrcTextureRemapForgetOverlayClump(clump, s.loadedModel ? s.loadedModel->m_nTxdIndex : -1);
        g_overlaySkeletons.erase(clump);
    }
    OrcDestroyRwObjectInstance(s.rwObject);
    if (s.loadedModel) {
        s.loadedModel->RemoveRef();
        s.loadedModel = nullptr;
    }
    s.retryLoadAt = 0;
}

void OrcSkinsPruneUnusedAssets() {
    if (g_loadedCustomSkins.empty() && g_standardSkins.empty())
        return;

    const uint32_t now = GetTickCount();
    static uint32_t lastPrune = 0;
    if (!g_skinTxdPoolFull && now - lastPrune < 2000)
        return;
    lastPrune = now;

    constexpr uint32_t kTemplateIdleMs = 30000;
    const uint32_t customIdleMs = g_skinTxdPoolFull ? 1000 : kTemplateIdleMs;
    g_skinTxdPoolFull = false;
    auto* pool = CTxdStore::ms_pTxdPool;
    if (pool) {
        for (auto it = g_loadedCustomSkins.begin(); it != g_loadedCustomSkins.end();) {
            auto* skin = *it++;
            if (now - skin->lastUseAt < customIdleMs)
                continue;
            const auto* txd = pool->GetAt(skin->txdSlot);
            // Native clones own a separate ref to the private dictionary.
            if (txd && txd->m_wRefsCount == 1)
                DestroyCustomSkinInstance(*skin);
        }
    }

    for (auto& skin : g_standardSkins) {
        if (skin.rwObject && now - skin.lastUseAt >= kTemplateIdleMs)
            DestroyStandardSkinInstance(skin);
    }
}

static void ReleaseRandomPoolSkinInstances() {
    for (auto& entry : g_skinRandomPools) {
        for (auto& variant : entry.second.variants)
            DestroyCustomSkinInstance(variant);
    }
    ResetRandomSkinSelections();
}

static void DestroyAllRandomPoolSkins() {
    ReleaseRandomPoolSkinInstances();
    g_skinRandomPools.clear();
    g_skinRandomPoolModels = 0;
    g_skinRandomPoolVariants = 0;
}

static bool LoadSkinDictionary(int txdSlot, const char* path) {
    RwStream* stream = nullptr;
    bool loaded = false;
    __try {
        __try {
            stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, const_cast<char*>(path));
            if (stream)
                loaded = CTxdStore::LoadTxd(txdSlot, stream);
        } __finally {
            if (stream)
                RwStreamClose(stream, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("skin: TXD stream SEH ex=0x%08X path=%s", GetExceptionCode(), path);
    }
    return loaded;
}

static RpClump* ReadSkinClump(int txdSlot, const char* path) {
    RwStream* stream = nullptr;
    RpClump* clump = nullptr;
    RwTexDictionary* previous = RwTexDictionaryGetCurrent();
    __try {
        __try {
            CTxdStore::SetCurrentTxd(txdSlot);
            stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, const_cast<char*>(path));
            if (stream && RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr))
                clump = RpClumpStreamRead(stream);
        } __finally {
            RwTexDictionarySetCurrent(previous);
            if (stream)
                RwStreamClose(stream, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("skin: DFF stream SEH ex=0x%08X path=%s", GetExceptionCode(), path);
    }
    return clump;
}

static bool IsSkinLoadDeferred(uint32_t retryAt, uint32_t now) {
    return retryAt && static_cast<int32_t>(retryAt - now) > 0;
}

bool OrcEnsureCustomSkinLoaded(CustomSkinCfg& s) {
    if (s.rwObject) {
        s.lastUseAt = GetTickCount();
        return true;
    }
    if (s.loadFailed)
        return false;

    const uint32_t now = GetTickCount();
    if (IsSkinLoadDeferred(s.retryLoadAt, now))
        return false;

    const auto fail = [&s](const char* reason, const char* path) {
        s.loadFailed = true;
        OrcLogError("skin \"%s\": %s (%s)", s.name.c_str(), reason, path);
        return false;
    };
    if (s.txdPath.empty())
        return fail("TXD missing", s.dffPath.c_str());

    auto* pool = CTxdStore::ms_pTxdPool;
    if (!pool)
        return false;
    // The stock AddTxdSlot dereferences its allocation when the pool is full.
    if (!pool->GetNoOfFreeSpaces()) {
        g_skinTxdPoolFull = true;
        s.retryLoadAt = now + 1000;
        OrcLogInfoThrottled(945u, 5000u, "skin: TXD pool full; deferred %s", s.name.c_str());
        return false;
    }

    // This slot belongs to the template and to any native clones referencing it.
    const int txdSlot = CTxdStore::AddTxdSlot("orcoutfit_skin");
    if (txdSlot < 0) {
        s.retryLoadAt = now + 1000;
        return false;
    }
    if (!LoadSkinDictionary(txdSlot, s.txdPath.c_str())) {
        CTxdStore::RemoveTxdSlot(txdSlot);
        return fail("LoadTxd failed", s.txdPath.c_str());
    }
    CTxdStore::AddRef(txdSlot);
    s.txdSlot = txdSlot;

    RpClump* clump = ReadSkinClump(txdSlot, s.dffPath.c_str());
    s.rwObject = reinterpret_cast<RwObject*>(clump);
    if (!clump || !RpClumpGetFrame(clump)) {
        DestroyCustomSkinInstance(s);
        return fail("DFF clump load failed", s.dffPath.c_str());
    }
    s.retryLoadAt = 0;
    s.lastUseAt = now;
    g_loadedCustomSkins.insert(&s);
    OrcLogInfo("skin \"%s\": clump loaded", s.name.c_str());
    return true;
}

static bool EnsureStandardSkinLoaded(StandardSkinCfg& s) {
    CBaseModelInfo* model = OrcIsValidStandardSkinModel(s.modelId) ? CModelInfo::GetModelInfo(s.modelId) : nullptr;
    const uint32_t now = GetTickCount();
    if (s.rwObject && model == s.loadedModel) {
        s.lastUseAt = now;
        return true;
    }
    if (s.rwObject)
        DestroyStandardSkinInstance(s);
    if (!model)
        return false;

    if (IsSkinLoadDeferred(s.retryLoadAt, now))
        return false;
    s.retryLoadAt = now + 250;
    if (!CStreaming::HasModelLoaded(s.modelId) || !model->m_pRwObject) {
        CStreaming::RequestModel(s.modelId, 0);
        return false;
    }

    RwObject* instance = model->CreateInstance();
    if (!instance || instance->type != rpCLUMP) {
        OrcDestroyRwObjectInstance(instance);
        if (!s.loadFailedLogged) {
            s.loadFailedLogged = true;
            OrcLogError("standard skin \"%s\" [%d]: CreateInstance failed or non-clump", s.dffName.c_str(), s.modelId);
        }
        return false;
    }

    model->AddRef();
    s.loadedModel = model;
    s.rwObject = instance;
    s.lastUseAt = now;
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
    const std::vector<OrcIniValue> values = {
        {"NickBinding", "Enabled", s.bindToNick ? "1" : "0"},
        {"NickBinding", "Nicks", s.nickListCsv},
    };
    if (!OrcIniWriteValues(s.iniPath.c_str(), kCustomSkinIniHeader, values)) {
        OrcLogError("SaveSkinCfgToIni: cannot write %s", s.iniPath.c_str());
        return;
    }
    OrcIniCacheInvalidatePath(s.iniPath.c_str());
    InvalidateCustomSkinLookupCache();
}

static bool g_standardSkinLookupDirty = true;
static std::unordered_map<std::string, int> g_standardSkinNickLookup;

void InvalidateStandardSkinLookupCache() {
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
    g_uiStdSkinEditModelId = -1;
    for (auto& s : g_standardSkins)
        DestroyStandardSkinInstance(s);
    g_standardSkins.clear();
    InvalidateStandardSkinLookupCache();

    const std::string path = StandardSkinsIniPath();
    OrcIniCacheInvalidatePath(path.c_str());
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
        int modelId = doc.GetInt(sec.c_str(), "ModelId", -1);
        const char* modelDff = OrcTryGetPedModelNameById(modelId);
        if (!modelDff || ToLowerAscii(modelDff) != dffLower || !OrcIsValidStandardSkinModel(modelId)) {
            modelId = -1;
            for (int id = 0; id < static_cast<int>(g_pedModelNameById.size()); ++id) {
                if (ToLowerAscii(g_pedModelNameById[static_cast<size_t>(id)]) == dffLower &&
                    OrcIsValidStandardSkinModel(id)) {
                    modelId = id;
                    break;
                }
            }
            if (modelId < 0) {
                OrcLogInfo("standard skin: configured DFF unavailable: %s", dff.c_str());
                continue;
            }
            modelDff = OrcTryGetPedModelNameById(modelId);
        }

        StandardSkinCfg cfg;
        cfg.modelId = modelId;
        cfg.dffName = modelDff;
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
    else
        OrcIniCacheInvalidatePath(path.c_str());
    InvalidateStandardSkinLookupCache();
}

void OrcAppendSkinFeatureIniValues(std::vector<OrcIniValue>& values) {
    AddIniInt(values, "Features", "SkinMode", g_skinModeEnabled ? 1 : 0);
    AddIniValue(values, "Features", "SkinCustomMode",
        g_skinCustomMode == SKIN_CUSTOM_MODE_OVERLAY ? "overlay" : "native");
    AddIniValue(values, "Features", "SkinNativeFallback",
        g_skinNativeFallback == SKIN_NATIVE_FALLBACK_OVERLAY ? "overlay" : "vanilla");
    AddIniInt(values, "Features", "SkinHideBasePed", g_skinHideBasePed ? 1 : 0);
    AddIniInt(values, "Features", "SkinNickMode", g_skinNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinLocalPreferSelected", g_skinLocalPreferSelected ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemap", g_skinTextureRemapEnabled ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapNickMode", g_skinTextureRemapNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapAutoNickMode", g_skinTextureRemapAutoNickMode ? 1 : 0);
    AddIniInt(values, "Features", "SkinTextureRemapRandomMode", g_skinTextureRemapRandomMode);
    AddIniValue(values, "Features", "SkinTextureRemapPickMode", SkinRandomPickModeToIni(g_skinTextureRemapPickMode));
}

void OrcAppendSkinModeIniValues(std::vector<OrcIniValue>& values) {
    AddIniValue(values, "SkinMode", "Selected", g_skinSelectedName.c_str());
    AddIniValue(values, "SkinMode", "SelectedSource", g_skinSelectedSource == SKIN_SELECTED_STANDARD ? "standard" : "custom");
    AddIniInt(values, "SkinMode", "StandardSelected", g_standardSkinSelectedModelId);
    AddIniInt(values, "SkinMode", "RandomFromPools", g_skinRandomFromPools ? 1 : 0);
    AddIniInt(values, "SkinMode", "SkinRandomIncludeVanilla", g_skinRandomIncludeVanilla ? 1 : 0);
    AddIniValue(values, "SkinMode", "RandomPickMode", SkinRandomPickModeToIni(g_skinRandomPickMode));
}

void SaveSkinModeIni() {
    std::vector<OrcIniValue> values;
    OrcAppendSkinFeatureIniValues(values);
    OrcAppendSkinModeIniValues(values);
    if (!OrcIniWriteValues(g_iniPath, "; OrcOutFit configuration.\n\n", values))
        OrcLogError("SaveSkinModeIni: cannot write %s", g_iniPath);
}

struct SkinFolderFiles {
    std::vector<std::string> dffs;
    std::unordered_map<std::string, std::string> txdsByBasename;
    size_t txdCount = 0;

    std::string FindTxd(const std::string& dir, const std::string& basename) const {
        const auto it = txdsByBasename.find(ToLowerAscii(basename));
        if (it != txdsByBasename.end())
            return OrcJoinPath(dir, it->second);
        if (txdCount == 1)
            return OrcJoinPath(dir, txdsByBasename.begin()->second);
        return {};
    }
};

static SkinFolderFiles ReadSkinFolderFiles(const std::string& dir) {
    SkinFolderFiles files;
    WIN32_FIND_DATAA data{};
    const HANDLE handle = FindFirstFileA(OrcJoinPath(dir, "*").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return files;

    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::string name = data.cFileName;
        const std::string ext = OrcLowerExt(name);
        if (ext == ".dff") {
            files.dffs.push_back(std::move(name));
        } else if (ext == ".txd") {
            std::string key = ToLowerAscii(OrcBaseNameNoExt(name));
            files.txdsByBasename.emplace(std::move(key), std::move(name));
            ++files.txdCount;
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    std::sort(files.dffs.begin(), files.dffs.end(), [](const std::string& a, const std::string& b) {
        constexpr size_t extensionSize = 4; // ".dff"
        const int compared = _strnicmp(a.c_str(), b.c_str(), std::min(a.size(), b.size()) - extensionSize);
        if (compared != 0)
            return compared < 0;
        if (a.size() != b.size())
            return a.size() < b.size();
        return a < b;
    });
    return files;
}

static void DiscoverRandomSkinPools() {
    const std::string randomDir = OrcJoinPath(g_gameSkinDir, "Random");
    WIN32_FIND_DATAA fd{};
    const HANDLE handle = FindFirstFileA(OrcJoinPath(randomDir, "*").c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    std::unordered_map<std::string, int> modelIdsByDff;
    for (int id = 0; id < static_cast<int>(g_pedModelNameById.size()); ++id) {
        const std::string& dff = g_pedModelNameById[static_cast<size_t>(id)];
        if (!dff.empty() && OrcIsValidStandardSkinModel(id))
            modelIdsByDff.emplace(ToLowerAscii(dff), id);
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const std::string folder = fd.cFileName;
        const auto model = modelIdsByDff.find(ToLowerAscii(folder));
        if (model == modelIdsByDff.end())
            continue;

        const std::string poolDir = OrcJoinPath(randomDir, folder);
        const SkinFolderFiles files = ReadSkinFolderFiles(poolDir);
        if (files.dffs.empty())
            continue;

        SkinRandomPool& pool = g_skinRandomPools[model->second];
        pool.folderName = folder;
        pool.variants.reserve(files.dffs.size());
        for (const auto& name : files.dffs) {
            const std::string base = OrcBaseNameNoExt(name);
            CustomSkinCfg skin;
            skin.name = folder + "/" + base;
            skin.dffPath = OrcJoinPath(poolDir, name);
            skin.txdPath = files.FindTxd(poolDir, base);
            skin.remapKey = base;
            skin.remapFallbackKey = folder;
            pool.variants.push_back(std::move(skin));
        }
        g_skinRandomPoolVariants += static_cast<int>(files.dffs.size());
    } while (FindNextFileA(handle, &fd));
    FindClose(handle);

    g_skinRandomPoolModels = static_cast<int>(g_skinRandomPools.size());
    OrcLogInfo("DiscoverRandomSkinPools: %d pools, %d variants in %s",
               g_skinRandomPoolModels, g_skinRandomPoolVariants, randomDir.c_str());
}

void DiscoverCustomSkins() {
    OrcTextureRemapInvalidateStates();
    OrcTextureRemapRefreshAdditionalTxds();
    OrcSkinNativeOnSkinAssetsReleased();
    for (auto& skin : g_customSkins) {
        OrcIniCacheInvalidatePath(skin.iniPath.c_str());
        DestroyCustomSkinInstance(skin);
    }
    g_customSkins.clear();
    g_selectedSkinCacheIdx = -1;
    g_selectedSkinCacheName.clear();
    InvalidateCustomSkinLookupCache();
    DestroyAllRandomPoolSkins();
    g_uiSkinIdx = 0;
    g_uiSkinEditIdx = -1;

    const DWORD attributes = GetFileAttributesA(g_gameSkinDir);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        OrcLogInfo("Skins folder missing or not a directory: %s", g_gameSkinDir);
        return;
    }
    const std::string dir = g_gameSkinDir;
    const SkinFolderFiles files = ReadSkinFolderFiles(dir);
    const std::string selected = ToLowerAscii(g_skinSelectedName);
    g_customSkins.reserve(files.dffs.size());
    for (const auto& name : files.dffs) {
        const std::string base = OrcBaseNameNoExt(name);
        CustomSkinCfg skin;
        skin.name = base;
        skin.dffPath = OrcJoinPath(dir, name);
        skin.txdPath = files.FindTxd(dir, base);
        skin.iniPath = OrcJoinPath(dir, base + ".ini");
        skin.remapKey = base;
        CreateDefaultSkinIniIfMissing(skin.iniPath);
        LoadSkinCfgFromIni(skin);
        if (ToLowerAscii(base) == selected)
            g_uiSkinIdx = static_cast<int>(g_customSkins.size());
        g_customSkins.push_back(std::move(skin));
    }
    OrcLogInfo("DiscoverCustomSkins: %zu skins in %s", g_customSkins.size(), g_gameSkinDir);
    DiscoverRandomSkinPools();
}

void OrcCollectRandomSkinPools(std::vector<SkinRandomPoolInfo>& out) {
    out.clear();
    out.reserve(g_skinRandomPools.size());
    for (const auto& entry : g_skinRandomPools) {
        SkinRandomPoolInfo info;
        info.dffName = entry.second.folderName;
        info.modelId = entry.first;
        info.variants = static_cast<int>(entry.second.variants.size());
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SkinRandomPoolInfo& a, const SkinRandomPoolInfo& b) {
        return a.modelId < b.modelId;
    });
}

// OrcCollectPedSkins вынесена в orc_ped_index.cpp.

// Skin preview tab/renderer removed.

static CustomSkinCfg* GetSelectedSkin() {
    if (g_customSkins.empty())
        return nullptr;
    if (g_skinSelectedName.empty())
        return &g_customSkins.front();

    if (g_selectedSkinCacheName != g_skinSelectedName) {
        g_selectedSkinCacheName = g_skinSelectedName;
        g_selectedSkinCacheIdx = -1;
        const std::string want = ToLowerAscii(g_skinSelectedName);
        for (int i = 0; i < static_cast<int>(g_customSkins.size()); ++i) {
            if (ToLowerAscii(g_customSkins[static_cast<size_t>(i)].name) == want) {
                g_selectedSkinCacheIdx = i;
                break;
            }
        }
    }
    return g_selectedSkinCacheIdx < 0 ? nullptr : &g_customSkins[static_cast<size_t>(g_selectedSkinCacheIdx)];
}


static CustomSkinCfg* FindNickSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    EnsureCustomSkinNickLookup();
    const auto it = g_customSkinNickLookup.find(nickLower);
    if (it == g_customSkinNickLookup.end()) return nullptr;
    return &g_customSkins[static_cast<size_t>(it->second)];
}

static StandardSkinCfg* FindNickStandardSkin(const std::string& nickLower) {
    if (nickLower.empty()) return nullptr;
    EnsureStandardSkinNickLookup();
    const auto it = g_standardSkinNickLookup.find(nickLower);
    return it == g_standardSkinNickLookup.end() ? nullptr : OrcGetStandardSkinCfgByModelId(it->second, false);
}

static int GetSelectedStandardSkinModelId() {
    if (g_standardSkinSelectedModelId >= 0)
        return g_standardSkinSelectedModelId;
    for (int id = 0; id < static_cast<int>(g_pedModelNameById.size()); ++id) {
        if (!g_pedModelNameById[static_cast<size_t>(id)].empty() && OrcIsValidStandardSkinModel(id))
            return id;
    }
    return -1;
}

static StandardSkinCfg* GetSelectedStandardSkin() {
    return OrcGetStandardSkinCfgByModelId(GetSelectedStandardSkinModelId(), true);
}

const char* OrcGetSelectedManualSkinDffName() {
    if (g_skinSelectedSource == SKIN_SELECTED_CUSTOM) {
        const auto* skin = GetSelectedSkin();
        return skin ? skin->name.c_str() : nullptr;
    }
    const int modelId = GetSelectedStandardSkinModelId();
    return OrcIsValidStandardSkinModel(modelId) ? OrcTryGetPedModelNameById(modelId) : nullptr;
}

OrcResolvedPedSkin OrcResolveSkinForPedModel(CPed* ped, CPlayerPed* localPlayer, int baseModelId) {
    OrcResolvedPedSkin result;
    if (!ped || (!g_skinModeEnabled && !g_skinRandomFromPools)) return result;
    result.isLocalPed = ped == localPlayer;

    if (g_skinModeEnabled && g_skinNickMode && samp_bridge::IsSampBuildKnown()) {
        // Avoid a player-pool lookup per ped when there are no nick bindings.
        EnsureCustomSkinNickLookup();
        EnsureStandardSkinNickLookup();
        if (!g_customSkinNickLookup.empty() || !g_standardSkinNickLookup.empty()) {
            char nick[32] = {};
            bool isLocalBySamp = false;
            if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocalBySamp)) {
                result.isLocalPed = result.isLocalPed || isLocalBySamp;
                const std::string nickLower = ToLowerAscii(nick);
                result.custom = FindNickSkin(nickLower);
                if (result.custom)
                    return result;
                result.standard = FindNickStandardSkin(nickLower);
                if (result.standard)
                    return result;
            }
        }
    }

    if (result.isLocalPed && g_skinModeEnabled && g_skinLocalPreferSelected) {
        if (g_skinSelectedSource == SKIN_SELECTED_CUSTOM)
            result.custom = GetSelectedSkin();
        else
            result.standard = GetSelectedStandardSkin();
        if (result.custom || result.standard)
            return result;
    }

    result.fromRandomPool = ResolveRandomSkinForPedModel(ped, baseModelId, &result.custom);
    return result;
}

OrcResolvedPedSkin OrcResolveSkinForPed(CPed* ped, CPlayerPed* localPlayer) {
    return OrcResolveSkinForPedModel(ped, localPlayer, ped ? (int)ped->m_nModelIndex : -1);
}

namespace {
constexpr uintptr_t kEntityClumpRenderCall = 0x53439C;
constexpr uintptr_t kRwClumpRender = 0x749B20;

bool IsOverlayModeEnabled() {
    return g_overlayRenderHookInstalled && !OrcIsRuntimeShuttingDown() && g_enabled &&
        (g_skinModeEnabled || g_skinRandomFromPools) &&
        (g_skinCustomMode == SKIN_CUSTOM_MODE_OVERLAY || g_skinNativeFallback == SKIN_NATIVE_FALLBACK_OVERLAY);
}

RpAtomic* DrawFadedOverlayAtomic(RpAtomic* atomic, int alpha, unsigned char* previousAlphas) {
    RpGeometry& geometry = *atomic->geometry;
    const auto savedFlags = geometry.flags;
    auto& materials = geometry.matList;
    int saved = 0;
    RpAtomic* result = nullptr;
    __try {
        geometry.flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        for (; saved < materials.numMaterials; ++saved) {
            if (auto* material = materials.materials[saved]) {
                previousAlphas[saved] = material->color.alpha;
                if (material->color.alpha > alpha)
                    material->color.alpha = static_cast<RwUInt8>(alpha);
            }
        }
        result = AtomicDefaultRenderCallBack(atomic);
    } __finally {
        while (saved > 0) {
            --saved;
            if (auto* material = materials.materials[saved])
                material->color.alpha = previousAlphas[saved];
        }
        geometry.flags = savedFlags;
    }
    return result;
}

RpAtomic* DrawOverlayAtomic(RpAtomic* atomic) {
    const int alpha = CVisibilityPlugins::GetClumpAlpha(RpAtomicGetClump(atomic));
    if (alpha <= 0)
        return atomic;
    if (alpha >= 255 || atomic->geometry->matList.numMaterials <= 0)
        return AtomicDefaultRenderCallBack(atomic);

    std::array<unsigned char, 64> inlineAlphas;
    const auto count = static_cast<size_t>(atomic->geometry->matList.numMaterials);
    if (count <= inlineAlphas.size())
        return DrawFadedOverlayAtomic(atomic, alpha, inlineAlphas.data());
    std::vector<unsigned char> alphas(count);
    return DrawFadedOverlayAtomic(atomic, alpha, alphas.data());
}

RpAtomic* DrawSkinnedOverlayAtomic(RpAtomic* atomic) {
    auto* skeleton = g_drawingOverlaySkeleton;
    if (!skeleton)
        return DrawOverlayAtomic(atomic);

    // D3D9 caches the palette by hierarchy and atomic frame within each camera update.
    // Consecutive draws of the same template still need a new palette for each ped.
    skeleton->useAlternate = !skeleton->useAlternate;
    auto* drawHierarchy = skeleton->useAlternate ? &skeleton->alternate : skeleton->hierarchy;
    RpSkinAtomicSetHAnimHierarchy(atomic, drawHierarchy);
    __try {
        return DrawOverlayAtomic(atomic);
    } __finally {
        RpSkinAtomicSetHAnimHierarchy(atomic, skeleton->hierarchy);
    }
}

struct OverlaySetup {
    RpHAnimHierarchy* hierarchy;
    bool valid = true;
    bool hasSkinnedAtomic = false;
};

RpAtomic* PrepareOverlayAtomic(RpAtomic* atomic, void* userData) {
    auto& setup = *static_cast<OverlaySetup*>(userData);
    auto* geometry = RpAtomicGetGeometry(atomic);
    if (!geometry) {
        setup.valid = false;
        return atomic;
    }
    if (auto* skin = RpSkinGeometryGetSkin(geometry)) {
        const auto bones = RpSkinGetNumBones(skin);
        if (bones != static_cast<unsigned>(setup.hierarchy->numNodes) ||
            !RpSkinGetSkinToBoneMatrices(skin) || !RpSkinAtomicSetType(atomic, rpSKINTYPEGENERIC)) {
            setup.valid = false;
            return atomic;
        }
        RpSkinAtomicSetHAnimHierarchy(atomic, setup.hierarchy);
        if (RpAtomicGetFlags(atomic) & rpATOMICRENDER)
            setup.hasSkinnedAtomic = true;
        CVisibilityPlugins::SetAtomicRenderCallback(atomic, &DrawSkinnedOverlayAtomic);
    } else {
        CVisibilityPlugins::SetAtomicRenderCallback(atomic, &DrawOverlayAtomic);
    }
    return atomic;
}

bool HasSkinPalette(const RpHAnimHierarchy* hierarchy) {
    return hierarchy && hierarchy->numNodes > 0 && hierarchy->pNodeInfo && hierarchy->pMatrixArray;
}

OverlaySkeleton* GetOverlaySkeleton(RpClump* clump) {
    const auto [entry, inserted] = g_overlaySkeletons.try_emplace(clump);
    auto& skeleton = entry->second;
    if (inserted) {
        auto* hierarchy = GetAnimHierarchyFromClump(clump);
        if (HasSkinPalette(hierarchy)) {
            OverlaySetup setup{hierarchy};
            RpClumpForAllAtomics(clump, PrepareOverlayAtomic, &setup);
            if (setup.valid && setup.hasSkinnedAtomic)
                skeleton.hierarchy = hierarchy;
        }
        if (!skeleton.hierarchy)
            OrcLogError("skin overlay: clump %p lacks a compatible skinned hierarchy", clump);
    }
    return skeleton.hierarchy ? &skeleton : nullptr;
}

bool MatchOverlayPalette(RpHAnimHierarchy* source, RpHAnimHierarchy* target, bool copy) {
    if (!HasSkinPalette(source) || !target)
        return false;
    for (int i = 0; i < target->numNodes; ++i) {
        const int bone = target->pNodeInfo[i].nodeID;
        int sourceIndex = i;
        if (i >= source->numNodes || source->pNodeInfo[i].nodeID != bone)
            sourceIndex = RpHAnimIDGetIndex(source, bone);
        if (sourceIndex < 0 || sourceIndex >= source->numNodes)
            return false;
        if (copy)
            target->pMatrixArray[i] = source->pMatrixArray[sourceIndex];
    }
    if (copy) {
        target->flags = (target->flags & ~rpHANIMHIERARCHYLOCALSPACEMATRICES) |
            (source->flags & rpHANIMHIERARCHYLOCALSPACEMATRICES);
    }
    return true;
}

bool ResolveOverlaySkin(CPed* ped, bool load, OrcNativeSkinActiveInfo& out) {
    OrcNativeSkinActiveInfo active;
    if (OrcSkinNativeGetActiveInfo(ped, active))
        return false;

    const auto skin = OrcResolveSkinForPed(ped, FindPlayerPed(0));
    const bool overlay = g_skinCustomMode == SKIN_CUSTOM_MODE_OVERLAY;
    if (auto* asset = skin.custom; asset && (overlay || OrcSkinNativeShouldOverlayFallback(ped, asset))) {
        if (load && !OrcEnsureCustomSkinLoaded(*asset))
            return false;
        out.clump = reinterpret_cast<RpClump*>(asset->rwObject);
        out.dffName = asset->remapKey.empty() ? asset->name.c_str() : asset->remapKey.c_str();
        out.fallbackDffName = asset->remapFallbackKey.empty() ? nullptr : asset->remapFallbackKey.c_str();
        out.txdSlot = asset->txdSlot;
    } else if (auto* asset = skin.standard; asset && (overlay || OrcSkinNativeShouldOverlayFallback(ped, asset))) {
        if (load && !EnsureStandardSkinLoaded(*asset))
            return false;
        if (!asset->loadedModel)
            return false;
        out.clump = reinterpret_cast<RpClump*>(asset->rwObject);
        out.dffName = asset->dffName.c_str();
        out.txdSlot = asset->loadedModel->m_nTxdIndex;
    }
    return out.clump != nullptr;
}

bool DrawSkinOverlay(CPed* ped, RpClump* sourceClump) {
    OrcNativeSkinActiveInfo overlay;
    if (!ResolveOverlaySkin(ped, true, overlay) || ped->m_pRwClump != sourceClump)
        return false;

    auto* sourceFrame = RpClumpGetFrame(sourceClump);
    auto* targetFrame = RpClumpGetFrame(overlay.clump);
    auto* skeleton = GetOverlaySkeleton(overlay.clump);
    if (skeleton && skeleton->drawing)
        return false;
    if (!sourceFrame || !targetFrame || !skeleton ||
        !MatchOverlayPalette(GetAnimHierarchyFromSkinClump(sourceClump), skeleton->hierarchy, true)) {
        OrcLogInfoThrottled(946u, 5000u, "skin overlay: incompatible pose model=%d dff=%s",
            ped->m_nModelIndex, overlay.dffName);
        return false;
    }

    const int alpha = CVisibilityPlugins::GetClumpAlpha(sourceClump);
    if (alpha <= 0)
        return true;
    auto* previousSkeleton = g_drawingOverlaySkeleton;
    bool lit = false;
    skeleton->drawing = true;
    __try {
        CVisibilityPlugins::SetClumpAlpha(overlay.clump, alpha);
        RwFrameTransform(targetFrame, RwFrameGetMatrix(sourceFrame), rwCOMBINEREPLACE);
        skeleton->alternate = *skeleton->hierarchy;
        lit = OrcTryPedSetupLighting(ped);
        CVector boundCentre{};
        ped->GetBoundCentre(boundCentre);
        OrcApplyAttachmentLightingForPed(ped, boundCentre, 1.0f);
        __try {
            OrcTextureRemapApplyToClumpBefore(ped, overlay.clump, overlay.dffName, overlay.fallbackDffName, overlay.txdSlot);
            g_drawingOverlaySkeleton = skeleton;
            return RpClumpRender(overlay.clump) != nullptr;
        } __finally {
            g_drawingOverlaySkeleton = previousSkeleton;
            OrcTextureRemapRestoreAfter();
        }
    } __finally {
        skeleton->drawing = false;
        if (lit) {
            OrcTryPedRemoveLighting(ped, lit);
            // CEntity::Render returns to CPed::Render, which still draws the held weapon.
            OrcTryPedSetupLighting(ped);
        }
    }
}

RpClump* __cdecl RenderClumpWithSkinOverlay(RpClump* clump) {
    CPed* ped = g_overlayRenderPeds.empty() ? nullptr : g_overlayRenderPeds.back();
    if (ped && ped->m_pRwClump == clump) {
        bool rendered = false;
        __try {
            rendered = DrawSkinOverlay(ped, clump);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcLogInfoThrottled(947u, 5000u, "skin overlay: draw SEH ex=0x%08X", GetExceptionCode());
        }
        if (rendered && g_skinHideBasePed)
            return clump;
    }
    return RpClumpRender(clump);
}

uintptr_t ReadEntityClumpRenderTarget() {
    if (plugin::patch::GetUChar(kEntityClumpRenderCall) != 0xE8)
        return 0;
    return kEntityClumpRenderCall + 5 + plugin::patch::GetInt(kEntityClumpRenderCall + 1);
}
}

void OrcSkinsInstallOverlayRenderHook() {
    if (g_overlayRenderHookInstalled)
        return;
    // CPed::Render's two CEntity::Render calls are enclosed by pedRenderEvent.
    if (ReadEntityClumpRenderTarget() != kRwClumpRender) {
        OrcLogError("skin overlay: unexpected CEntity::Render clump call at 0x%08X",
            static_cast<unsigned>(kEntityClumpRenderCall));
        return;
    }
    plugin::patch::RedirectCall(kEntityClumpRenderCall, reinterpret_cast<void*>(&RenderClumpWithSkinOverlay));
    g_overlayRenderHookInstalled =
        ReadEntityClumpRenderTarget() == reinterpret_cast<uintptr_t>(&RenderClumpWithSkinOverlay);
    if (g_overlayRenderHookInstalled)
        OrcLogInfo("skin overlay: using the game's ped clump render pass");
    else
        OrcLogError("skin overlay: could not attach to CEntity::Render");
}

bool OrcSkinsGetOverlayTextureRemapInfo(CPed* ped, OrcNativeSkinActiveInfo& out) {
    if (!IsOverlayModeEnabled() || !ped || !ped->m_pRwClump)
        return false;
    out = {};
    if (!ResolveOverlaySkin(ped, false, out))
        return false;
    auto* skeleton = GetOverlaySkeleton(out.clump);
    return skeleton && MatchOverlayPalette(GetAnimHierarchyFromSkinClump(ped->m_pRwClump), skeleton->hierarchy, false);
}

void OrcSkinsOnPedRenderBefore(CPed* ped) {
    if (IsOverlayModeEnabled() || !g_overlayRenderPeds.empty())
        g_overlayRenderPeds.push_back(ped);
}

void OrcSkinsOnPedRenderAfter(CPed* ped) {
    if (!g_overlayRenderPeds.empty() && g_overlayRenderPeds.back() == ped)
        g_overlayRenderPeds.pop_back();
}

void OrcSkinsReleaseAllInstancesAndPreview() {
    OrcSkinNativeOnSkinAssetsReleased();
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    for (auto& s : g_standardSkins) DestroyStandardSkinInstance(s);
    ReleaseRandomPoolSkinInstances();
}

void OrcSkinsShutdown() {
    OrcSkinNativeShutdown();
    g_overlayRenderPeds.clear();
    if (g_overlayRenderHookInstalled &&
        ReadEntityClumpRenderTarget() == reinterpret_cast<uintptr_t>(&RenderClumpWithSkinOverlay)) {
        plugin::patch::RedirectCall(kEntityClumpRenderCall, kRwClumpRender);
    }
    g_overlayRenderHookInstalled = false;
    for (auto& s : g_customSkins) DestroyCustomSkinInstance(s);
    for (auto& s : g_standardSkins) DestroyStandardSkinInstance(s);
    DestroyAllRandomPoolSkins();
}
