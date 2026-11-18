#include "orc_texture_remap.h"

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CHandObject.h"
#include "CKeyGen.h"
#include "CPedModelInfo.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "eModelInfoType.h"
#include "CPools.h"
#include "CTxdStore.h"
#include "RenderWare.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <utility>

#include "orc_types.h"
#include "orc_app.h"
#include "orc_ini.h"
#include "orc_ini_cache.h"
#include "orc_log.h"
#include "orc_random_pick.h"
#include "orc_skin_native.h"
#include "samp_bridge.h"
#include "external/MinHook/include/MinHook.h"

using namespace plugin;

static std::string TextureRemapToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

static std::string TextureRemapFileStemForDff(const char* dffName, int modelId) {
    if (dffName && dffName[0])
        return std::string(dffName);

    char fallback[32] = {};
    _snprintf_s(fallback, _TRUNCATE, "id%d", modelId);
    return std::string(fallback);
}

static std::string TextureRemapDffKey(const char* dffName, int modelId) {
    return TextureRemapToLowerAscii(TextureRemapFileStemForDff(dffName, modelId));
}

static std::string TextureRemapIniPathForDff(const char* dffName, int modelId) {
    return std::string(g_gameTextureDir) + "\\" + TextureRemapFileStemForDff(dffName, modelId) + ".ini";
}

// ----------------------------------------------------------------------------
// Texture remaps (PedFuncs-style *_remap support for standard ped TXDs)
// ----------------------------------------------------------------------------
static constexpr int kTextureRemapLimit = 8;
static constexpr int kTextureRemapUnbound = -2;

struct TextureRemapAutoNickCandidate {
    RwTexture* texture = nullptr;
    std::string textureName;
    std::string nickKeyLower;
};

struct TextureRemapSlotState {
    std::string originalName;
    std::vector<RwTexture*> remaps;
    std::vector<std::string> remapNames;
    std::vector<TextureRemapAutoNickCandidate> autoNickCandidates;
    int selected = -1;
    int nickSelected = kTextureRemapUnbound;
    int autoNickSelected = -1;
};

struct TextureRemapBindingSlot {
    std::string originalName;
    std::string remapName;
};

struct TextureRemapNickBinding {
    int id = -1;
    bool enabled = true;
    std::string nickListCsv;
    std::vector<std::string> nicknames;
    std::vector<TextureRemapBindingSlot> slots;
};

static bool TextureRemapNickMatches(const TextureRemapNickBinding& binding, const std::string& nickLower) {
    if (!binding.enabled || nickLower.empty())
        return false;
    for (const auto& nick : binding.nicknames) {
        if (nick == nickLower)
            return true;
    }
    return false;
}

static bool TextureRemapIsHexDigit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static std::string TextureRemapNormalizeNickForMatch(const char* nick) {
    std::string out;
    if (!nick)
        return out;

    const std::string src(nick);
    for (size_t i = 0; i < src.size();) {
        if (src[i] == '{') {
            const size_t close = src.find('}', i + 1);
            const size_t len = (close == std::string::npos) ? 0 : (close - i - 1);
            if (close != std::string::npos && (len == 6 || len == 8)) {
                bool allHex = true;
                for (size_t j = i + 1; j < close; ++j) {
                    if (!TextureRemapIsHexDigit(src[j])) {
                        allHex = false;
                        break;
                    }
                }
                if (allHex) {
                    i = close + 1;
                    continue;
                }
            }
        }

        char c = src[i++];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

struct TextureRemapGeometryRef {
    const RpAtomic* atomic;
    const RpGeometry* geometry;
    RpMaterial** materials;
    int materialCount;
};

struct TextureRemapMaterialBinding {
    RpMaterial** entry;
    RpMaterial* material;
    RwTexture* original;
    int slot;
};

struct PedTextureRemapState {
    int modelId = -1;
    int txdIndex = -1;
    std::string dffName;
    std::string fallbackDffName;
    std::string sequentialOwnerKey;
    RpClump* clump = nullptr;
    RwTexDictionary* dictionary = nullptr;
    unsigned int nickBindingRevision = 0;
    unsigned int nickMode = 0;
    DWORD nickCheckedAt = 0;
    bool nickPolled = false;
    char nick[32] = {};
    std::string autoNickLogKey;
    int slotCount = 0;
    int totalRemapTextures = 0;
    int totalAutoNickTextures = 0;
    bool assetScanFailed = false;
    std::vector<TextureRemapGeometryRef> geometries;
    std::vector<TextureRemapMaterialBinding> materials;
    std::array<TextureRemapSlotState, kTextureRemapLimit> slots;
};

struct PedTextureRemapCache {
    PedTextureRemapState base;
    std::unordered_map<int, PedTextureRemapState> replacements;
};

struct TextureRemapRestoreEntry {
    RpMaterial* material;
    RwTexture* original;
    RwTexture* replacement;
};

using FindTxdTextureFn = RwTexture*(__cdecl*)(const char*);
using HandRenderFn = void(__thiscall*)(CHandObject*);

struct AdditionalSkinTxd {
    const char* name;
    int slot = -1;
};

static FindTxdTextureFn g_findTxdTextureOrig = nullptr;
static HandRenderFn g_handRenderOrig = nullptr;
static AdditionalSkinTxd g_sharedSkinTxds[] = {{"peds1"}, {"peds2"}, {"peds3"}, {"peds4"}, {"peds5"}};
static AdditionalSkinTxd g_gangHandsTxd{"ganghands"};
static bool g_hasSharedSkinTxds = false;
static unsigned int g_textureRemapNickBindingRevision = 1;
static constexpr uint64_t kTextureRemapPointerKey = uint64_t{1} << 32;
static std::unordered_map<uint64_t, PedTextureRemapCache> g_pedTextureRemaps;
static std::unordered_map<std::string, OrcSequentialPickState> g_textureRemapSequentialStates;
static std::unordered_map<std::string, int> g_textureRemapSequentialAssignments;
static std::unordered_map<std::string, OrcRandomNoRepeatPickState> g_textureRemapNoRepeatStates;
static std::unordered_map<std::string, int> g_textureRemapNoRepeatAssignments;
static std::vector<TextureRemapRestoreEntry> g_textureRemapRestoreEntries;
static std::vector<size_t> g_textureRemapRestoreScopes;
static std::unordered_map<std::string, std::vector<TextureRemapNickBinding>> g_textureRemapNickBindingsByDff;

static TxdDef* GetTxdDefByIndex(int txdIndex) {
    auto* pool = CTxdStore::ms_pTxdPool;
    return pool && txdIndex >= 0 && txdIndex < pool->m_nSize ? pool->GetAt(txdIndex) : nullptr;
}

static RwTexDictionary* GetTxdDictionaryByIndex(int txdIndex) {
    const TxdDef* txd = GetTxdDefByIndex(txdIndex);
    return txd ? txd->m_pRwDictionary : nullptr;
}

static void ReleaseAdditionalSkinTxd(AdditionalSkinTxd& asset) {
    const int slot = std::exchange(asset.slot, -1);
    const TxdDef* txd = GetTxdDefByIndex(slot);
    if (!txd || txd->m_hash != CKeyGen::GetUppercaseKey(asset.name) || !txd->m_wRefsCount)
        return;
    __try {
        CTxdStore::RemoveRef(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap: release %s.txd SEH ex=0x%08X slot=%d", asset.name, GetExceptionCode(), slot);
    }
}

static bool RequestAdditionalSkinTxd(AdditionalSkinTxd& asset) {
    const int slot = CTxdStore::FindTxdSlot(asset.name);
    if (asset.slot != slot)
        ReleaseAdditionalSkinTxd(asset);
    const TxdDef* txd = GetTxdDefByIndex(slot);
    if (!txd)
        return false;

    if (asset.slot < 0) {
        CTxdStore::AddRef(slot);
        asset.slot = slot;
        OrcLogInfo("texture remap: found additional %s.txd slot=%d", asset.name, slot);
    }
    if (txd->m_pRwDictionary)
        return false;

    CStreaming::RequestTxdModel(slot, GAME_REQUIRED | KEEP_IN_MEMORY);
    return true;
}

static void ReleaseAdditionalTextureRemapTxds() {
    g_hasSharedSkinTxds = false;
    for (auto& asset : g_sharedSkinTxds)
        ReleaseAdditionalSkinTxd(asset);
    ReleaseAdditionalSkinTxd(g_gangHandsTxd);
}

void OrcTextureRemapRefreshAdditionalTxds() {
    if (!CTxdStore::ms_pTxdPool || OrcIsRuntimeShuttingDown())
        return;

    const int previousHandsSlot = g_gangHandsTxd.slot;
    RwTexDictionary* previousHands = GetTxdDictionaryByIndex(previousHandsSlot);
    bool requested = RequestAdditionalSkinTxd(g_gangHandsTxd);
    g_hasSharedSkinTxds = false;
    for (auto& asset : g_sharedSkinTxds) {
        requested |= RequestAdditionalSkinTxd(asset);
        g_hasSharedSkinTxds |= asset.slot >= 0;
    }

    // Never start streaming recursively from a RenderWare texture lookup.
    if (requested)
        CStreaming::LoadAllRequestedModels(false);

    for (const auto& asset : g_sharedSkinTxds) {
        if (asset.slot >= 0 && !GetTxdDictionaryByIndex(asset.slot))
            OrcLogError("texture remap: unable to load %s.txd slot=%d", asset.name, asset.slot);
    }
    if (g_gangHandsTxd.slot >= 0) {
        RwTexDictionary* hands = GetTxdDictionaryByIndex(g_gangHandsTxd.slot);
        if (!hands)
            OrcLogError("texture remap: unable to load ganghands.txd slot=%d", g_gangHandsTxd.slot);
        else if (g_gangHandsTxd.slot != previousHandsSlot || hands != previousHands)
            OrcLogInfo("texture remap: ganghands.txd ready slot=%d", g_gangHandsTxd.slot);
    }
}

static RwTexture* __cdecl FindTxdTextureWithSharedSkins(const char* name) {
    // The game's CTxdStore callback checks the current dictionary and its parents first.
    if (!name || !name[0])
        return nullptr;
    RwTexture* original = g_findTxdTextureOrig(name);
    if (original || !g_hasSharedSkinTxds)
        return original;

    for (const auto& asset : g_sharedSkinTxds) {
        if (RwTexDictionary* dict = GetTxdDictionaryByIndex(asset.slot)) {
            if (RwTexture* texture = RwTexDictionaryFindNamedTexture(dict, name))
                return texture;
        }
    }
    return nullptr;
}

static RwTexture* ResolveGangHandTexture(CPed* ped, RwTexDictionary* hands) {
    if (!ped || ped->m_nModelIndex < 0)
        return nullptr;
    CBaseModelInfo* mi = CModelInfo::GetModelInfo(ped->m_nModelIndex);
    if (!mi || mi->GetModelType() != MODEL_INFO_PED)
        return nullptr;

    OrcNativeSkinActiveInfo skin;
    const bool hasReplacement = OrcSkinsGetOverlayTextureRemapInfo(ped, skin) || OrcSkinNativeGetActiveInfo(ped, skin);
    if (hasReplacement) {
        if (RwTexDictionary* dict = GetTxdDictionaryByIndex(skin.txdSlot)) {
            if (skin.dffName && skin.dffName[0]) {
                if (RwTexture* texture = RwTexDictionaryFindHashNamedTexture(dict, CKeyGen::GetUppercaseKey(skin.dffName)))
                    return texture;
            }
            if (RwTexture* texture = RwTexDictionaryFindHashNamedTexture(dict, mi->m_nKey))
                return texture;
        }
    }

    if (!hands)
        return nullptr;

    if (!hasReplacement) {
        RwTexDictionary* dict = GetTxdDictionaryByIndex(mi->m_nTxdIndex);
        if (dict && RwTexDictionaryFindHashNamedTexture(dict, mi->m_nKey))
            return nullptr;  // Keep the exact texture chosen by the game's constructor.

        // PedFuncs and the standalone fix own the vanilla fallback if they patched it.
        constexpr unsigned char stockFallback[] = {0x57, 0xE8, 0xC1, 0x59, 0x19, 0, 0x83, 0xC4, 4};
        if (std::memcmp(reinterpret_cast<const void*>(0x59EF79), stockFallback, sizeof(stockFallback)) != 0)
            return nullptr;
    }

    const unsigned int type = static_cast<CPedModelInfo*>(mi)->m_nPedType;
    const bool black = type == PED_TYPE_GANG1 || type == PED_TYPE_GANG2;
    const char* name = black ? "hands_black" : "hands_white";
    RwTexture* texture = RwTexDictionaryFindNamedTexture(hands, name);
    static bool logged[2] = {};
    if (texture && !logged[black]) {
        logged[black] = true;
        OrcLogInfo("texture remap: gang hand fallback %s model=%d", name, ped->m_nModelIndex);
    }
    return texture;
}

static void RestoreGangHandMaterial(RpMaterial* material, RwTexture* replacement, RwTexture* previous) {
    __try {
        if (material->texture == replacement)
            RpMaterialSetTexture(material, previous);
    } __finally {
        if (previous)
            RwTextureDestroy(previous);
    }
}

static void __fastcall RenderHandWithSkinTexture(CHandObject* hand, void*) {
    RwTexture* texture = nullptr;
    RpMaterial* material = nullptr;
    RwTexture* previous = nullptr;
    __try {
        if (hand && hand->m_pRwClump) {
            texture = ResolveGangHandTexture(hand->m_pPed, GetTxdDictionaryByIndex(g_gangHandsTxd.slot));
            if (texture && texture != hand->m_pTexture) {
                const RpAtomic* atomic = GetFirstAtomic(hand->m_pRwClump);
                const RpGeometry* geometry = atomic ? atomic->geometry : nullptr;
                if (geometry && geometry->matList.numMaterials > 0 && geometry->matList.materials) {
                    RpMaterial* first = geometry->matList.materials[0];
                    if (first) {
                        previous = first->texture;
                        if (previous)
                            RwTextureAddRef(previous);
                        material = first;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            OrcLogError("texture remap: gang hand resolution SEH ex=0x%08X", GetExceptionCode());
        }
    }
    if (!material) {
        g_handRenderOrig(hand);
        return;
    }

    // CHandObject::Render replaces the texture on the first material of its shared geometry.
    RwTexture* original = hand->m_pTexture;
    hand->m_pTexture = texture;
    __try {
        g_handRenderOrig(hand);
    } __finally {
        hand->m_pTexture = original;
        RestoreGangHandMaterial(material, texture, previous);
    }
}

static void InstallTextureRemapHook(uintptr_t address, void* hook, void** original, const char* label) {
    void* target = reinterpret_cast<void*>(address);
    MH_STATUS st = MH_CreateHook(target, hook, original);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK)
        OrcLogError("texture remap: %s hook -> %s", label, MH_StatusToString(st));
}

void OrcTextureRemapInstallHooks() {
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("texture remap hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }

    InstallTextureRemapHook(0x731720, reinterpret_cast<void*>(&FindTxdTextureWithSharedSkins),
        reinterpret_cast<void**>(&g_findTxdTextureOrig), "CTxdStore::FindTextureCB");
    InstallTextureRemapHook(0x59EE80, reinterpret_cast<void*>(&RenderHandWithSkinTexture),
        reinterpret_cast<void**>(&g_handRenderOrig), "CHandObject::Render");
}

static int RandomInclusive(int lo, int hi) {
    if (hi < lo) return lo;
    return lo + (rand() % (hi - lo + 1));
}

static bool SlotHasRemapVariant(const TextureRemapSlotState& slot, RwTexture* texture, const std::string& name) {
    for (size_t i = 0; i < slot.remaps.size(); ++i) {
        if (slot.remaps[i] == texture)
            return true;
        if (_stricmp(slot.remapNames[i].c_str(), name.c_str()) == 0)
            return true;
    }
    return false;
}

static bool SlotHasAutoNickCandidate(const TextureRemapSlotState& slot, RwTexture* texture, const std::string& name) {
    for (const auto& candidate : slot.autoNickCandidates) {
        if (candidate.texture == texture)
            return true;
        if (_stricmp(candidate.textureName.c_str(), name.c_str()) == 0)
            return true;
    }
    return false;
}

static void ClearTextureRemapAutoNickSlotSelections(PedTextureRemapState& state) {
    for (int i = 0; i < state.slotCount; ++i)
        state.slots[(size_t)i].autoNickSelected = -1;
}

static void ClearTextureRemapAutoNickSelections(PedTextureRemapState& state) {
    ClearTextureRemapAutoNickSlotSelections(state);
    state.autoNickLogKey.clear();
}

static bool SetRealRemapSelection(TextureRemapSlotState& slot, int requested) {
    if (requested == -1) {
        slot.selected = -1;
        return true;
    }
    if (requested >= 0 && requested < (int)slot.remaps.size()) {
        slot.selected = requested;
        return true;
    }

    slot.selected = -1;
    return false;
}

static int ClampTextureRemapRandomMode(int mode) {
    if (mode == TEXTURE_REMAP_RANDOM_PER_TEXTURE)
        return TEXTURE_REMAP_RANDOM_PER_TEXTURE;
    return TEXTURE_REMAP_RANDOM_LINKED_VARIANT;
}

static std::string TextureRemapSequentialOwnerKey(CPed* ped) {
    const int ref = OrcSafeGetPedRef(ped);
    if (ref > 0)
        return "r:" + std::to_string(ref);
    return "p:" + std::to_string(reinterpret_cast<uintptr_t>(ped));
}

static std::string TextureRemapSequentialBaseKey(const PedTextureRemapState& state) {
    std::string key;
    key.reserve(96);
    key += TextureRemapToLowerAscii(state.dffName.empty() ? std::string("?") : state.dffName);
    key += "|";
    key += TextureRemapToLowerAscii(state.fallbackDffName.empty() ? std::string("-") : state.fallbackDffName);
    key += "|m:";
    key += std::to_string(state.modelId);
    key += "|t:";
    key += std::to_string(state.txdIndex);
    return key;
}

static std::string TextureRemapSequentialLinkedPoolKey(const PedTextureRemapState& state) {
    return std::string("linked|") + TextureRemapSequentialBaseKey(state);
}

static std::string TextureRemapSequentialSlotPoolKey(
    const PedTextureRemapState& state,
    const TextureRemapSlotState& slot) {
    return std::string("slot|") + TextureRemapSequentialBaseKey(state) + "|" +
        TextureRemapToLowerAscii(slot.originalName);
}

static std::string TextureRemapSequentialChoiceKey(const PedTextureRemapState& state, const std::string& poolKey) {
    return state.sequentialOwnerKey + "|" + poolKey;
}

static std::vector<int> TextureRemapIndexPool(int total) {
    std::vector<int> pool;
    if (total <= 0)
        return pool;
    pool.reserve((size_t)total);
    for (int i = 0; i < total; ++i)
        pool.push_back(i);
    return pool;
}

static bool TextureRemapPickModeTracksActiveChoices() {
    return g_skinTextureRemapPickMode == ORC_RANDOM_PICK_SEQUENTIAL ||
        g_skinTextureRemapPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT;
}

static void ReleaseTextureRemapStateAssignments(const PedTextureRemapState& state) {
    if (state.sequentialOwnerKey.empty() ||
        (g_textureRemapSequentialAssignments.empty() && g_textureRemapNoRepeatAssignments.empty()))
        return;
    const auto release = [&state](const std::string& poolKey) {
        const std::string choiceKey = TextureRemapSequentialChoiceKey(state, poolKey);
        const auto sequential = g_textureRemapSequentialStates.find(poolKey);
        if (sequential != g_textureRemapSequentialStates.end())
            OrcSequentialEraseChoice(sequential->second, g_textureRemapSequentialAssignments, choiceKey);
        const auto noRepeat = g_textureRemapNoRepeatStates.find(poolKey);
        if (noRepeat != g_textureRemapNoRepeatStates.end())
            OrcRandomNoRepeatEraseChoice(noRepeat->second, g_textureRemapNoRepeatAssignments, choiceKey);
    };
    release(TextureRemapSequentialLinkedPoolKey(state));
    for (int i = 0; i < state.slotCount; ++i)
        release(TextureRemapSequentialSlotPoolKey(state, state.slots[(size_t)i]));
}

static bool TextureRemapPedPointerAlive(uintptr_t pedPtr) {
    auto* pool = CPools::ms_pPedPool;
    if (!pedPtr || !pool)
        return true;
    __try {
        const uintptr_t first = reinterpret_cast<uintptr_t>(pool->m_pObjects);
        const unsigned int stride = pool->GetObjectSize();
        const uintptr_t offset = pedPtr - first;
        return pedPtr >= first && offset % stride == 0 && offset / stride < static_cast<unsigned int>(pool->m_nSize) &&
            pool->GetAt(static_cast<int>(offset / stride)) == reinterpret_cast<CPed*>(pedPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

static bool TextureRemapOwnerIsDead(const char* ownerKey) {
    if (ownerKey[0] == 'r' && ownerKey[1] == ':') {
        const int ref = std::atoi(ownerKey + 2);
        return ref > 0 && !OrcSafeGetPed(ref);
    }
    if (ownerKey[0] == 'p' && ownerKey[1] == ':') {
        const uintptr_t ptr = static_cast<uintptr_t>(_strtoui64(ownerKey + 2, nullptr, 10));
        return ptr != 0 && !TextureRemapPedPointerAlive(ptr);
    }
    return false;
}

static void PruneTextureRemapStateMaps() {
    for (auto it = g_pedTextureRemaps.begin(); it != g_pedTextureRemaps.end();) {
        const uint64_t key = it->first;
        const bool alive = (key & kTextureRemapPointerKey)
            ? TextureRemapPedPointerAlive(static_cast<uint32_t>(key))
            : OrcSafeGetPed(static_cast<int>(key)) != nullptr;
        if (!alive) {
            ReleaseTextureRemapStateAssignments(it->second.base);
            for (const auto& replacement : it->second.replacements)
                ReleaseTextureRemapStateAssignments(replacement.second);
            it = g_pedTextureRemaps.erase(it);
        } else {
            ++it;
        }
    }
}

static void PruneTextureRemapPoolChoices(
    std::unordered_set<std::string>& activeKeys,
    std::unordered_map<std::string, int>& assignments,
    OrcRandomNoRepeatPickState* noRepeat = nullptr) {
    for (auto it = activeKeys.begin(); it != activeKeys.end();) {
        if (!TextureRemapOwnerIsDead(it->c_str())) {
            ++it;
            continue;
        }
        const auto assignment = assignments.find(*it);
        if (assignment != assignments.end()) {
            if (noRepeat)
                OrcRandomNoRepeatInsertIntoBag(*noRepeat, assignment->second);
            assignments.erase(assignment);
        }
        it = activeKeys.erase(it);
    }
}

static int PickTrackedTextureRemapIndex(PedTextureRemapState& state, const std::string& poolKey, int total) {
    const std::vector<int> pool = TextureRemapIndexPool(total);
    if (pool.empty() || state.sequentialOwnerKey.empty())
        return -1;
    const std::string choiceKey = TextureRemapSequentialChoiceKey(state, poolKey);
    if (g_skinTextureRemapPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        auto& picks = g_textureRemapNoRepeatStates[poolKey];
        PruneTextureRemapPoolChoices(picks.activeChoiceKeys, g_textureRemapNoRepeatAssignments, &picks);
        return OrcRandomNoRepeatPickSticky(
            picks,
            g_textureRemapNoRepeatAssignments,
            choiceKey,
            pool,
            -1);
    }
    auto& picks = g_textureRemapSequentialStates[poolKey];
    PruneTextureRemapPoolChoices(picks.activeChoiceKeys, g_textureRemapSequentialAssignments);
    return OrcSequentialPickSticky(
        picks,
        g_textureRemapSequentialAssignments,
        choiceKey,
        pool,
        -1);
}

static void SelectRandomTextureRemapsPerTexture(PedTextureRemapState& state) {
    const bool trackedPick = TextureRemapPickModeTracksActiveChoices() &&
        !state.sequentialOwnerKey.empty();
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        const int total = (int)slot.remaps.size();
        if (total <= 0) {
            slot.selected = -1;
            continue;
        }
        if (trackedPick) {
            const int picked = PickTrackedTextureRemapIndex(state, TextureRemapSequentialSlotPoolKey(state, slot), total);
            slot.selected = (picked >= 0 && picked < total) ? picked : RandomInclusive(0, total - 1);
        } else {
            slot.selected = RandomInclusive(0, total - 1);
        }
    }
}

static void SelectRandomTextureRemapsLinkedVariant(PedTextureRemapState& state) {
    int maxVariants = 0;
    for (int i = 0; i < state.slotCount; ++i) {
        const TextureRemapSlotState& slot = state.slots[(size_t)i];
        maxVariants = std::max(maxVariants, (int)slot.remaps.size());
    }

    if (maxVariants <= 0) {
        for (int i = 0; i < state.slotCount; ++i)
            state.slots[(size_t)i].selected = -1;
        return;
    }

    const bool trackedPick = TextureRemapPickModeTracksActiveChoices() &&
        !state.sequentialOwnerKey.empty();
    int linkedVariant = -1;
    if (trackedPick)
        linkedVariant = PickTrackedTextureRemapIndex(state, TextureRemapSequentialLinkedPoolKey(state), maxVariants);
    if (linkedVariant < 0 || linkedVariant >= maxVariants)
        linkedVariant = RandomInclusive(0, maxVariants - 1);
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        const int total = (int)slot.remaps.size();
        if (total <= 0) {
            slot.selected = -1;
        } else if (linkedVariant < total) {
            slot.selected = linkedVariant;
        } else if (trackedPick) {
            const int picked = PickTrackedTextureRemapIndex(state, TextureRemapSequentialSlotPoolKey(state, slot), total);
            slot.selected = (picked >= 0 && picked < total) ? picked : RandomInclusive(0, total - 1);
        } else {
            slot.selected = RandomInclusive(0, total - 1);
        }
    }
}

static void SelectRandomTextureRemaps(PedTextureRemapState& state) {
    if (ClampTextureRemapRandomMode(g_skinTextureRemapRandomMode) == TEXTURE_REMAP_RANDOM_LINKED_VARIANT)
        SelectRandomTextureRemapsLinkedVariant(state);
    else
        SelectRandomTextureRemapsPerTexture(state);
}

static void LoadTextureRemapNickBindingsForDff(const char* dffName, int modelId) {
    const std::string key = TextureRemapDffKey(dffName, modelId);
    if (g_textureRemapNickBindingsByDff.find(key) != g_textureRemapNickBindingsByDff.end())
        return;

    std::vector<TextureRemapNickBinding> bindings;
    const std::string path = TextureRemapIniPathForDff(dffName, modelId);
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        const OrcIniDocument* docPtr = OrcIniCacheGet(path.c_str());
        if (docPtr && docPtr->IsLoaded()) {
            const OrcIniDocument& doc = *docPtr;
            std::vector<std::string> sections;
            doc.GetAllSectionNames(sections);

            for (const std::string& secStr : sections) {
                const char* sec = secStr.c_str();
                if (_strnicmp(sec, "Binding.", 8) != 0)
                    continue;

                TextureRemapNickBinding binding;
                binding.id = atoi(sec + 8);
                binding.enabled = doc.GetInt(sec, "Enabled", 1) != 0;
                binding.nickListCsv = doc.GetString(sec, "Nicks", "");
                binding.nicknames = ParseNickCsv(binding.nickListCsv);

                const int slotCount = doc.GetInt(sec, "SlotCount", 0);
                for (int i = 0; i < slotCount && i < kTextureRemapLimit; ++i) {
                    char keyOriginal[32], keyRemap[32];
                    _snprintf_s(keyOriginal, _TRUNCATE, "Slot%dOriginal", i);
                    _snprintf_s(keyRemap, _TRUNCATE, "Slot%dRemap", i);

                    const std::string originalName = doc.GetString(sec, keyOriginal, "");
                    const std::string remapName = doc.GetString(sec, keyRemap, "");
                    if (originalName.empty())
                        continue;

                    TextureRemapBindingSlot slot;
                    slot.originalName = originalName;
                    slot.remapName = remapName;
                    binding.slots.push_back(std::move(slot));
                }

                if (!binding.nicknames.empty() && !binding.slots.empty())
                    bindings.push_back(std::move(binding));
            }

            std::sort(bindings.begin(), bindings.end(), [](const TextureRemapNickBinding& a, const TextureRemapNickBinding& b) {
                return a.id < b.id;
            });
        }
    }

    g_textureRemapNickBindingsByDff.insert_or_assign(key, std::move(bindings));
}

static bool ApplyTextureRemapNickBindingToState(PedTextureRemapState& state, const TextureRemapNickBinding& binding) {
    bool any = false;
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        slot.nickSelected = kTextureRemapUnbound;
        for (const auto& saved : binding.slots) {
            if (_stricmp(saved.originalName.c_str(), slot.originalName.c_str()) != 0)
                continue;

            if (saved.remapName.empty()) {
                slot.nickSelected = -1;
            } else {
                for (int r = 0; r < (int)slot.remapNames.size(); ++r) {
                    if (_stricmp(saved.remapName.c_str(), slot.remapNames[(size_t)r].c_str()) == 0) {
                        slot.nickSelected = r;
                        break;
                    }
                }
            }
            any |= slot.nickSelected != kTextureRemapUnbound;
            break;
        }
    }
    return any;
}

static const char* TextureRemapPrimaryDff(const PedTextureRemapState& state) {
    if (!state.dffName.empty())
        return state.dffName.c_str();
    return OrcTryGetPedModelNameById(state.modelId);
}

static bool TryApplyTextureRemapManualNickBindingForDff(
    PedTextureRemapState& state,
    const char* dff,
    const std::string& nickLower
) {
    if (!dff || !dff[0])
        return false;

    const std::string key = TextureRemapDffKey(dff, state.modelId);
    LoadTextureRemapNickBindingsForDff(dff, state.modelId);
    auto it = g_textureRemapNickBindingsByDff.find(key);
    if (it == g_textureRemapNickBindingsByDff.end())
        return false;

    const std::vector<TextureRemapNickBinding>& bindings = it->second;
    for (auto rit = bindings.rbegin(); rit != bindings.rend(); ++rit) {
        if (!TextureRemapNickMatches(*rit, nickLower))
            continue;

        if (ApplyTextureRemapNickBindingToState(state, *rit))
            return true;
    }
    return false;
}

static bool TryApplyTextureRemapManualNickBinding(
    PedTextureRemapState& state,
    const std::string& nickLower
) {
    if (!g_skinTextureRemapNickMode)
        return false;

    if (TryApplyTextureRemapManualNickBindingForDff(state, TextureRemapPrimaryDff(state), nickLower))
        return true;
    if (!state.fallbackDffName.empty())
        return TryApplyTextureRemapManualNickBindingForDff(state, state.fallbackDffName.c_str(), nickLower);
    return false;
}

static bool ApplyTextureRemapAutoNickBinding(PedTextureRemapState& state, const char* nick, const std::string& nickLower) {
    ClearTextureRemapAutoNickSlotSelections(state);
    if (!g_skinTextureRemapAutoNickMode || nickLower.empty()) {
        state.autoNickLogKey.clear();
        return false;
    }

    int appliedSlots = 0;
    std::string logKey = nickLower;
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        int best = -1;
        size_t bestLen = 0;
        for (int c = 0; c < (int)slot.autoNickCandidates.size(); ++c) {
            const TextureRemapAutoNickCandidate& candidate = slot.autoNickCandidates[(size_t)c];
            if (!candidate.texture || candidate.nickKeyLower.empty())
                continue;
            if (nickLower.find(candidate.nickKeyLower) == std::string::npos)
                continue;
            if (candidate.nickKeyLower.size() > bestLen) {
                best = c;
                bestLen = candidate.nickKeyLower.size();
            }
        }

        if (best >= 0) {
            slot.autoNickSelected = best;
            appliedSlots++;
            logKey += "|";
            logKey += slot.originalName;
            logKey += "=";
            logKey += slot.autoNickCandidates[(size_t)best].textureName;
        }
    }

    if (appliedSlots <= 0) {
        state.autoNickLogKey.clear();
        return false;
    }

    if (state.autoNickLogKey != logKey) {
        const char* dff = TextureRemapPrimaryDff(state);
        OrcLogInfo("texture remap auto nick: model=%d dff=%s nick=%s slots=%d",
                   state.modelId, dff ? dff : "?", nick ? nick : "", appliedSlots);
        state.autoNickLogKey = logKey;
    }
    return true;
}

static void ApplyTextureRemapNickBinding(CPed* ped, PedTextureRemapState& state) {
    const unsigned int mode = (g_skinTextureRemapNickMode ? 1u : 0u) |
        (g_skinTextureRemapAutoNickMode && state.totalAutoNickTextures > 0 ? 2u : 0u);
    char nick[32] = {};
    if (mode && samp_bridge::IsSampBuildKnown()) {
        constexpr DWORD kNickRefreshMs = 250;
        const DWORD now = GetTickCount();
        if (!state.nickPolled || state.nickMode != mode || now - state.nickCheckedAt >= kNickRefreshMs) {
            state.nickCheckedAt = now;
            state.nickPolled = true;
            if (!samp_bridge::GetPedNickname(ped, nick, sizeof(nick), nullptr))
                nick[0] = '\0';
        } else {
            std::memcpy(nick, state.nick, sizeof(nick));
        }
    } else {
        state.nickPolled = false;
    }
    if (state.nickMode == mode && state.nickBindingRevision == g_textureRemapNickBindingRevision &&
        std::strcmp(state.nick, nick) == 0) {
        return;
    }

    state.nickMode = mode;
    state.nickBindingRevision = g_textureRemapNickBindingRevision;
    std::memcpy(state.nick, nick, sizeof(nick));
    for (int i = 0; i < state.slotCount; ++i)
        state.slots[(size_t)i].nickSelected = kTextureRemapUnbound;

    if (nick[0]) {
        if (TryApplyTextureRemapManualNickBinding(state, TextureRemapToLowerAscii(nick))) {
            ClearTextureRemapAutoNickSelections(state);
            return;
        }
        if (mode & 2u) {
            ApplyTextureRemapAutoNickBinding(state, nick, TextureRemapNormalizeNickForMatch(nick));
            return;
        }
    }
    ClearTextureRemapAutoNickSelections(state);
}

struct TextureRemapTextureRef {
    RwTexture* texture = nullptr;
    std::string name;
    std::string lowerName;
};

struct TextureRemapScannedMaterial {
    RpMaterial** entry;
    RpMaterial* material;
    RwTexture* original;
    size_t originalIndex;
};

struct TextureRemapMaterialScan {
    std::vector<TextureRemapTextureRef> originals;
    std::vector<TextureRemapScannedMaterial> materials;
    std::vector<TextureRemapGeometryRef>& geometries;
};

static int GetOrAddTextureRemapSlot(PedTextureRemapState& state, const TextureRemapTextureRef& original) {
    for (int i = 0; i < state.slotCount; ++i) {
        if (_stricmp(state.slots[(size_t)i].originalName.c_str(), original.name.c_str()) == 0)
            return i;
    }
    if (state.slotCount >= kTextureRemapLimit)
        return -1;

    const int idx = state.slotCount++;
    TextureRemapSlotState& slot = state.slots[(size_t)idx];
    slot.originalName = original.name;
    return idx;
}

static RwTexture* TextureRemapCollectTextureCB(RwTexture* texture, void* data) {
    if (texture && texture->name[0]) {
        auto& textures = *static_cast<std::vector<TextureRemapTextureRef>*>(data);
        std::string name(texture->name, strnlen_s(texture->name, sizeof(texture->name)));
        textures.push_back({texture, name, TextureRemapToLowerAscii(name)});
    }
    return texture;
}

static void CollectTextureRemapVariants(
    PedTextureRemapState& state,
    const std::vector<TextureRemapTextureRef>& textures,
    const std::vector<TextureRemapTextureRef>& originals
) {
    for (const auto& texture : textures) {
        const size_t remapPos = texture.lowerName.find("_remap");
        if (remapPos == std::string::npos || remapPos == 0)
            continue;

        const auto original = std::find_if(originals.begin(), originals.end(), [&](const auto& candidate) {
            return candidate.lowerName.size() == remapPos &&
                texture.lowerName.compare(0, remapPos, candidate.lowerName) == 0;
        });
        if (original == originals.end())
            continue;

        const int slotIdx = GetOrAddTextureRemapSlot(state, *original);
        if (slotIdx < 0)
            continue;

        TextureRemapSlotState& slot = state.slots[(size_t)slotIdx];
        if (SlotHasRemapVariant(slot, texture.texture, texture.name))
            continue;

        slot.remaps.push_back(texture.texture);
        slot.remapNames.push_back(texture.name);
        state.totalRemapTextures++;
    }
}

static void SortTextureRemapVariants(PedTextureRemapState& state) {
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        std::vector<size_t> order;
        order.reserve(slot.remapNames.size());
        for (size_t idx = 0; idx < slot.remapNames.size(); ++idx)
            order.push_back(idx);
        std::sort(order.begin(), order.end(), [&slot](size_t a, size_t b) {
            return _stricmp(slot.remapNames[a].c_str(), slot.remapNames[b].c_str()) < 0;
        });

        std::vector<RwTexture*> sortedRemaps;
        std::vector<std::string> sortedNames;
        sortedRemaps.reserve(order.size());
        sortedNames.reserve(order.size());
        for (size_t idx : order) {
            sortedRemaps.push_back(slot.remaps[idx]);
            sortedNames.push_back(slot.remapNames[idx]);
        }
        slot.remaps = std::move(sortedRemaps);
        slot.remapNames = std::move(sortedNames);
    }
}

static RpAtomic* TextureRemapCollectOriginalAtomicCB(RpAtomic* atomic, void* data) {
    auto& scan = *static_cast<TextureRemapMaterialScan*>(data);
    const RpGeometry* geometry = atomic->geometry;
    RpMaterial** materials = geometry ? geometry->matList.materials : nullptr;
    const int count = geometry ? geometry->matList.numMaterials : 0;
    scan.geometries.push_back({atomic, geometry, materials, count});
    if (!materials)
        return atomic;

    for (int i = 0; i < count; ++i) {
        RpMaterial* material = materials[i];
        RwTexture* texture = material ? material->texture : nullptr;
        if (!texture || !texture->name[0])
            continue;
        const auto duplicate = std::find_if(scan.materials.begin(), scan.materials.end(),
            [material](const auto& existing) { return existing.material == material; });
        if (duplicate != scan.materials.end())
            continue;

        std::string name(texture->name, strnlen_s(texture->name, sizeof(texture->name)));
        std::string lowerName = TextureRemapToLowerAscii(name);
        const auto original = std::find_if(scan.originals.begin(), scan.originals.end(),
            [&lowerName](const auto& existing) { return existing.lowerName == lowerName; });
        const size_t originalIndex = static_cast<size_t>(original - scan.originals.begin());
        if (original == scan.originals.end())
            scan.originals.push_back({texture, std::move(name), std::move(lowerName)});
        scan.materials.push_back({materials + i, material, texture, originalIndex});
    }
    return atomic;
}

static bool ScanTextureRemapAssets(
    RpClump* clump,
    RwTexDictionary* dict,
    std::vector<TextureRemapTextureRef>& textures,
    TextureRemapMaterialScan& scan
) {
    if (!clump || !dict)
        return false;
    __try {
        RwTexDictionaryForAllTextures(dict, TextureRemapCollectTextureCB, &textures);
        RpClumpForAllAtomics(clump, TextureRemapCollectOriginalAtomicCB, &scan);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap asset scan: SEH ex=0x%08X clump=%p txd=%p", GetExceptionCode(), clump, dict);
        return false;
    }
}

static void CollectAutoNickTextureCandidates(
    PedTextureRemapState& state,
    const std::vector<TextureRemapTextureRef>& textures,
    const std::vector<TextureRemapTextureRef>& originals
) {
    for (const auto& texture : textures) {
        if (!texture.texture || texture.name.empty())
            continue;
        if (texture.lowerName.find("_remap") != std::string::npos)
            continue;

        const TextureRemapTextureRef* bestOriginal = nullptr;
        size_t bestLen = 0;
        bool isOriginal = false;
        for (const auto& original : originals) {
            if (original.lowerName == texture.lowerName) {
                isOriginal = true;
                break;
            }
            const size_t len = original.lowerName.size();
            if (len == 0 || texture.lowerName.size() <= len + 1)
                continue;
            if (texture.lowerName.compare(0, len, original.lowerName) != 0)
                continue;
            if (texture.lowerName[len] != '_')
                continue;
            if (len > bestLen) {
                bestOriginal = &original;
                bestLen = len;
            }
        }

        if (isOriginal || !bestOriginal)
            continue;

        const std::string nickKey = texture.name.substr(bestLen + 1);
        if (nickKey.empty())
            continue;

        const int slotIdx = GetOrAddTextureRemapSlot(state, *bestOriginal);
        if (slotIdx < 0)
            continue;

        TextureRemapSlotState& slot = state.slots[(size_t)slotIdx];
        if (SlotHasAutoNickCandidate(slot, texture.texture, texture.name))
            continue;

        TextureRemapAutoNickCandidate candidate;
        candidate.texture = texture.texture;
        candidate.textureName = texture.name;
        candidate.nickKeyLower = TextureRemapToLowerAscii(nickKey);
        slot.autoNickCandidates.push_back(std::move(candidate));
        state.totalAutoNickTextures++;
    }
}

static void ScanTextureRemaps(PedTextureRemapState& state) {
    std::vector<TextureRemapTextureRef> textures;
    TextureRemapMaterialScan scan{{}, {}, state.geometries};
    if (!ScanTextureRemapAssets(state.clump, state.dictionary, textures, scan)) {
        state.assetScanFailed = true;
        return;
    }

    CollectTextureRemapVariants(state, textures, scan.originals);
    CollectAutoNickTextureCandidates(state, textures, scan.originals);
    if (state.totalRemapTextures > 0 || state.totalAutoNickTextures > 0) {
        for (const TextureRemapScannedMaterial& material : scan.materials) {
            const std::string& name = scan.originals[material.originalIndex].name;
            for (int slot = 0; slot < state.slotCount; ++slot) {
                if (_stricmp(state.slots[(size_t)slot].originalName.c_str(), name.c_str()) == 0) {
                    state.materials.push_back({material.entry, material.material, material.original, slot});
                    break;
                }
            }
        }
        SortTextureRemapVariants(state);
        SelectRandomTextureRemaps(state);
        OrcLogInfo("texture remap scan: dff=%s model=%d txd=%d slots=%d variants=%d autoNick=%d",
                   state.dffName.c_str(), state.modelId, state.txdIndex, state.slotCount,
                   state.totalRemapTextures, state.totalAutoNickTextures);
    }
}

static bool TextureRemapTopologyMatches(const PedTextureRemapState& state) {
    if (state.assetScanFailed)
        return true;
    __try {
        // Clones normally keep their geometry. Clothing rebuilds and other mods can replace it in-place.
        auto* link = rwLinkListGetFirstLLLink(&state.clump->atomicList);
        const auto* end = rwLinkListGetTerminator(&state.clump->atomicList);
        for (const auto& cached : state.geometries) {
            if (link == end)
                return false;
            auto* atomic = rwLLLinkGetData(link, RpAtomic, inClumpLink);
            if (atomic != cached.atomic || atomic->geometry != cached.geometry)
                return false;
            const RpGeometry* geometry = cached.geometry;
            if (geometry && (geometry->matList.materials != cached.materials ||
                geometry->matList.numMaterials != cached.materialCount))
                return false;
            link = link->next;
        }
        if (link != end)
            return false;
        for (const auto& binding : state.materials) {
            if (*binding.entry != binding.material)
                return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint64_t TextureRemapPedKey(CPed* ped) {
    const int ref = OrcSafeGetPedRef(ped);
    if (ref > 0)
        return static_cast<unsigned int>(ref);
    if (!ped)
        return 0;
    return kTextureRemapPointerKey | reinterpret_cast<uintptr_t>(ped);
}

static PedTextureRemapState* EnsureTextureRemapState(
    CPed* ped,
    RpClump* clump,
    const char* dffName,
    const char* fallbackDffName,
    int txdIndex,
    bool isReplacement
) {
    if (!ped || !clump)
        return nullptr;
    const uint64_t pedKey = TextureRemapPedKey(ped);
    if (!pedKey)
        return nullptr;

    const int modelId = ped->m_nModelIndex;
    RwTexDictionary* dict = GetTxdDictionaryByIndex(txdIndex);
    PedTextureRemapCache& cache = g_pedTextureRemaps[pedKey];
    PedTextureRemapState& state = isReplacement ? cache.replacements[txdIndex] : cache.base;
    if (state.clump == clump && state.dictionary == dict && state.modelId == modelId &&
        state.txdIndex == txdIndex && state.dffName == (dffName ? dffName : "") &&
        state.fallbackDffName == (fallbackDffName ? fallbackDffName : "") &&
        TextureRemapTopologyMatches(state)) {
        return &state;
    }

    ReleaseTextureRemapStateAssignments(state);
    state = PedTextureRemapState{};
    state.modelId = modelId;
    state.txdIndex = txdIndex;
    state.clump = clump;
    state.dictionary = dict;
    if (dffName)
        state.dffName = dffName;
    if (fallbackDffName)
        state.fallbackDffName = fallbackDffName;
    state.sequentialOwnerKey = TextureRemapSequentialOwnerKey(ped);
    ScanTextureRemaps(state);
    return &state;
}

static PedTextureRemapState* EnsurePedTextureRemapState(CPed* ped) {
    if (!ped)
        return nullptr;
    const int modelId = ped->m_nModelIndex;
    CBaseModelInfo* mi = modelId >= 0 ? CModelInfo::GetModelInfo(modelId) : nullptr;
    const int txdIndex = mi && mi->GetModelType() == MODEL_INFO_PED ? mi->m_nTxdIndex : -1;
    return EnsureTextureRemapState(ped, ped->m_pRwClump, OrcTryGetPedModelNameById(modelId), nullptr, txdIndex, false);
}

static PedTextureRemapState* EnsureActiveTextureRemapState(CPed* ped) {
    OrcNativeSkinActiveInfo info;
    if (OrcSkinNativeGetActiveInfo(ped, info)) {
        return EnsureTextureRemapState(ped, info.clump, info.dffName, info.fallbackDffName, info.txdSlot, true);
    }
    return EnsurePedTextureRemapState(ped);
}

static PedTextureRemapState* EnsureVisibleTextureRemapState(CPed* ped) {
    OrcNativeSkinActiveInfo info;
    if (OrcSkinsGetOverlayTextureRemapInfo(ped, info)) {
        return EnsureTextureRemapState(ped, info.clump, info.dffName, info.fallbackDffName, info.txdSlot, true);
    }
    return EnsureActiveTextureRemapState(ped);
}

static PedTextureRemapState* EnsureLocalTextureRemapState() {
    return EnsureVisibleTextureRemapState(FindPlayerPed(0));
}

static void FillTextureRemapPedInfo(const PedTextureRemapState& state, TextureRemapPedInfo& out) {
    out = TextureRemapPedInfo{};
    out.modelId = state.modelId;
    out.txdIndex = state.txdIndex;
    out.totalRemapTextures = state.totalRemapTextures;
    if (!state.dffName.empty())
        out.dffName = state.dffName;
    else if (const char* dff = OrcTryGetPedModelNameById(state.modelId))
        out.dffName = dff;
    for (int i = 0; i < state.slotCount; ++i) {
        const TextureRemapSlotState& src = state.slots[(size_t)i];
        TextureRemapSlotInfo dst;
        dst.originalName = src.originalName;
        dst.remapNames = src.remapNames;
        dst.selected = src.selected;
        out.slots.push_back(std::move(dst));
    }
}

void OrcCollectPedTextureRemapStats(std::vector<TextureRemapModelStats>& out) {
    out.clear();
    out.reserve(g_pedTextureRemaps.size());
    const auto append = [&out](const PedTextureRemapState& state) {
        if (state.totalRemapTextures > 0)
            out.push_back({state.dffName, state.modelId, state.slotCount, state.totalRemapTextures});
    };
    for (const auto& ped : g_pedTextureRemaps) {
        append(ped.second.base);
        for (const auto& replacement : ped.second.replacements)
            append(replacement.second);
    }
    const auto modelLess = [](const TextureRemapModelStats& a, const TextureRemapModelStats& b) {
        if (a.modelId != b.modelId)
            return a.modelId < b.modelId;
        return _stricmp(a.dffName.c_str(), b.dffName.c_str()) < 0;
    };
    std::sort(out.begin(), out.end(), modelLess);
    out.erase(std::unique(out.begin(), out.end(), [&modelLess](const auto& a, const auto& b) {
        return !modelLess(a, b) && !modelLess(b, a);
    }), out.end());
}

bool OrcGetLocalPedTextureRemaps(TextureRemapPedInfo& out) {
    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state) {
        out = TextureRemapPedInfo{};
        return false;
    }
    FillTextureRemapPedInfo(*state, out);
    return true;
}

bool OrcSetLocalPedTextureRemap(int slot, int remap) {
    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state || slot < 0 || slot >= state->slotCount)
        return false;
    TextureRemapSlotState& s = state->slots[(size_t)slot];
    if (TextureRemapPickModeTracksActiveChoices())
        ReleaseTextureRemapStateAssignments(*state);
    return SetRealRemapSelection(s, remap);
}

bool OrcRandomizeLocalPedTextureRemaps() {
    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state || state->totalRemapTextures <= 0)
        return false;
    if (TextureRemapPickModeTracksActiveChoices())
        ReleaseTextureRemapStateAssignments(*state);
    SelectRandomTextureRemaps(*state);
    return true;
}

bool OrcSetAllLocalPedTextureRemaps(int remap) {
    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state)
        return false;
    if (TextureRemapPickModeTracksActiveChoices())
        ReleaseTextureRemapStateAssignments(*state);
    bool ok = true;
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& s = state->slots[(size_t)i];
        if (!SetRealRemapSelection(s, remap))
            ok = false;
    }
    return ok;
}

void OrcReloadTextureRemapNickBindings() {
    for (const auto& [dff, bindings] : g_textureRemapNickBindingsByDff) {
        const std::string path = TextureRemapIniPathForDff(dff.c_str(), -1);
        OrcIniCacheInvalidatePath(path.c_str());
    }
    g_textureRemapNickBindingsByDff.clear();
    ++g_textureRemapNickBindingRevision;
}

void OrcCollectLocalPedTextureRemapNickBindings(std::vector<TextureRemapNickBindingInfo>& out) {
    out.clear();
    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state)
        return;

    const char* dff = TextureRemapPrimaryDff(*state);
    const std::string key = TextureRemapDffKey(dff, state->modelId);
    LoadTextureRemapNickBindingsForDff(dff, state->modelId);

    auto it = g_textureRemapNickBindingsByDff.find(key);
    if (it == g_textureRemapNickBindingsByDff.end())
        return;

    for (const auto& binding : it->second) {
        TextureRemapNickBindingInfo info;
        info.id = binding.id;
        info.enabled = binding.enabled;
        info.nickListCsv = binding.nickListCsv;
        info.slotCount = (int)binding.slots.size();
        out.push_back(std::move(info));
    }
}

bool OrcSaveLocalPedTextureRemapNickBinding(const char* nickCsv) {
    std::vector<std::string> nicknames = ParseNickCsv(nickCsv ? nickCsv : "");
    if (nicknames.empty())
        return false;

    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state || state->slotCount <= 0)
        return false;

    const char* dff = TextureRemapPrimaryDff(*state);
    const std::string path = TextureRemapIniPathForDff(dff, state->modelId);

    const OrcIniDocument* iniDoc = OrcIniCacheGet(path.c_str());
    const int nextId = (iniDoc && iniDoc->IsLoaded()) ? iniDoc->GetInt("Main", "NextBindingId", 0) : 0;
    const int id = nextId;
    char nextBuf[32] = {};
    _snprintf_s(nextBuf, _TRUNCATE, "%d", nextId + 1);

    char section[32] = {};
    _snprintf_s(section, _TRUNCATE, "Binding.%d", id);
    std::vector<OrcIniValue> values;
    values.push_back({ "Main", "NextBindingId", nextBuf });
    values.push_back({ section, "Enabled", "1" });
    values.push_back({ section, "Nicks", nickCsv ? nickCsv : "" });

    char countBuf[32] = {};
    _snprintf_s(countBuf, _TRUNCATE, "%d", state->slotCount);
    values.push_back({ section, "SlotCount", countBuf });

    for (int i = 0; i < state->slotCount; ++i) {
        const TextureRemapSlotState& slot = state->slots[(size_t)i];
        char keyOriginal[32], keyRemap[32];
        _snprintf_s(keyOriginal, _TRUNCATE, "Slot%dOriginal", i);
        _snprintf_s(keyRemap, _TRUNCATE, "Slot%dRemap", i);
        values.push_back({ section, keyOriginal, slot.originalName });

        const char* remapName = "";
        if (slot.selected >= 0 && slot.selected < (int)slot.remapNames.size())
            remapName = slot.remapNames[(size_t)slot.selected].c_str();
        values.push_back({ section, keyRemap, remapName });
    }

    if (!OrcIniWriteValues(path.c_str(), "; OrcOutFit texture remap bindings.\n\n", values))
        return false;

    OrcIniCacheInvalidatePath(path.c_str());
    const std::string key = TextureRemapDffKey(dff, state->modelId);
    g_textureRemapNickBindingsByDff.erase(key);
    ++g_textureRemapNickBindingRevision;
    return true;
}

bool OrcDeleteLocalPedTextureRemapNickBinding(int bindingId) {
    PedTextureRemapState* state = EnsureLocalTextureRemapState();
    if (!state || bindingId < 0)
        return false;

    const char* dff = TextureRemapPrimaryDff(*state);
    const std::string path = TextureRemapIniPathForDff(dff, state->modelId);
    char section[32] = {};
    _snprintf_s(section, _TRUNCATE, "Binding.%d", bindingId);
    if (!OrcIniDeleteSection(path.c_str(), section))
        return false;

    OrcIniCacheInvalidatePath(path.c_str());
    const std::string key = TextureRemapDffKey(dff, state->modelId);
    g_textureRemapNickBindingsByDff.erase(key);
    ++g_textureRemapNickBindingRevision;
    return true;
}

static void SetTextureRemapScopeTextures(size_t start, bool resume) {
    for (size_t i = g_textureRemapRestoreEntries.size(); i > start; --i) {
        const auto& entry = g_textureRemapRestoreEntries[i - 1];
        __try {
            RwTexture* expected = resume ? entry.original : entry.replacement;
            if (entry.material->texture == expected)
                entry.material->texture = resume ? entry.replacement : entry.original;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

static void TextureRemapBeginDraw() {
    if (!g_textureRemapRestoreScopes.empty())
        SetTextureRemapScopeTextures(g_textureRemapRestoreScopes.back(), false);
    g_textureRemapRestoreScopes.push_back(g_textureRemapRestoreEntries.size());
}

static int GetSelectedRemap(const TextureRemapSlotState& slot) {
    return slot.nickSelected == kTextureRemapUnbound ? slot.selected : slot.nickSelected;
}

static void ApplyTextureRemaps(CPed* ped, RpClump* clump, PedTextureRemapState* state) {
    if (!state || state->clump != clump || state->materials.empty())
        return;
    ApplyTextureRemapNickBinding(ped, *state);

    std::array<RwTexture*, kTextureRemapLimit> replacements{};
    bool anyReplacement = false;
    for (int i = 0; i < state->slotCount; ++i) {
        const TextureRemapSlotState& slot = state->slots[(size_t)i];
        RwTexture* replacement = nullptr;
        if (slot.autoNickSelected >= 0 && slot.autoNickSelected < (int)slot.autoNickCandidates.size()) {
            replacement = slot.autoNickCandidates[(size_t)slot.autoNickSelected].texture;
        } else {
            const int selected = GetSelectedRemap(slot);
            if (selected >= 0 && selected < (int)slot.remaps.size())
                replacement = slot.remaps[(size_t)selected];
        }
        replacements[(size_t)i] = replacement;
        anyReplacement |= replacement != nullptr;
    }
    if (!anyReplacement)
        return;

    __try {
        for (const auto& binding : state->materials) {
            RwTexture* replacement = replacements[(size_t)binding.slot];
            if (!replacement || binding.material->texture != binding.original)
                continue;
            g_textureRemapRestoreEntries.push_back({binding.material, binding.original, replacement});
            binding.material->texture = replacement;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            OrcLogError("texture remap draw: SEH ex=0x%08X ped=%p dff=%s",
                GetExceptionCode(), ped, state->dffName.c_str());
        }
    }
}

void OrcTextureRemapApplyBefore(CPed* ped) {
    TextureRemapBeginDraw();
    if (!g_enabled || !g_skinTextureRemapEnabled || !ped || !ped->m_pRwClump)
        return;
    ApplyTextureRemaps(ped, ped->m_pRwClump, EnsureActiveTextureRemapState(ped));
}

void OrcTextureRemapApplyToClumpBefore(CPed* ped, RpClump* clump, const char* dffName, const char* fallbackDffName, int txdIndex) {
    TextureRemapBeginDraw();
    if (!g_enabled || !g_skinTextureRemapEnabled || !ped || !clump || txdIndex < 0)
        return;
    ApplyTextureRemaps(ped, clump, EnsureTextureRemapState(ped, clump, dffName, fallbackDffName, txdIndex, true));
}

void OrcTextureRemapRestoreAfter() {
    if (g_textureRemapRestoreScopes.empty())
        return;
    const size_t start = g_textureRemapRestoreScopes.back();
    SetTextureRemapScopeTextures(start, false);
    g_textureRemapRestoreEntries.resize(start);
    g_textureRemapRestoreScopes.pop_back();
    if (!g_textureRemapRestoreScopes.empty())
        SetTextureRemapScopeTextures(g_textureRemapRestoreScopes.back(), true);
}

void OrcTextureRemapRestoreAll() {
    if (!g_textureRemapRestoreScopes.empty())
        SetTextureRemapScopeTextures(g_textureRemapRestoreScopes.back(), false);
    g_textureRemapRestoreEntries.clear();
    g_textureRemapRestoreScopes.clear();
}

void OrcTextureRemapInvalidateStates() {
    OrcTextureRemapRestoreAll();
    g_pedTextureRemaps.clear();
    g_textureRemapSequentialStates.clear();
    g_textureRemapSequentialAssignments.clear();
    g_textureRemapNoRepeatStates.clear();
    g_textureRemapNoRepeatAssignments.clear();
}

void OrcTextureRemapForgetOverlayClump(RpClump* clump, int txdSlot) {
    if (!clump)
        return;
    for (auto& ped : g_pedTextureRemaps) {
        auto& replacements = ped.second.replacements;
        const auto asset = replacements.find(txdSlot);
        if (asset != replacements.end() && asset->second.clump == clump) {
            ReleaseTextureRemapStateAssignments(asset->second);
            replacements.erase(asset);
        }
    }
}

void OrcTextureRemapClearRuntimeState() {
    OrcTextureRemapInvalidateStates();
    if (OrcIsRuntimeShuttingDown())
        ReleaseAdditionalTextureRemapTxds();
}

void OrcTextureRemapOnGameProcess() {
    if (g_pedTextureRemaps.empty())
        return;
    static DWORD lastPruneAt = 0;
    const DWORD now = GetTickCount();
    if (now - lastPruneAt < 500u)
        return;
    lastPruneAt = now;
    PruneTextureRemapStateMaps();
}

void OrcTextureRemapOnPedSetModel(CPed* ped, int) {
    const auto cached = g_pedTextureRemaps.find(TextureRemapPedKey(ped));
    if (cached == g_pedTextureRemaps.end())
        return;
    ReleaseTextureRemapStateAssignments(cached->second.base);
    for (const auto& replacement : cached->second.replacements)
        ReleaseTextureRemapStateAssignments(replacement.second);
    g_pedTextureRemaps.erase(cached);
}

extern "C" int32_t __declspec(dllexport) Ext_GetPedRemap(CPed* ped, int index) {
    PedTextureRemapState* state = EnsureVisibleTextureRemapState(ped);
    if (!state || index < 0 || index >= state->slotCount)
        return -1;
    return GetSelectedRemap(state->slots[(size_t)index]);
}

extern "C" void __declspec(dllexport) Ext_SetPedRemap(CPed* ped, int index, int num) {
    PedTextureRemapState* state = EnsureVisibleTextureRemapState(ped);
    if (!state || index < 0 || index >= state->slotCount)
        return;
    TextureRemapSlotState& s = state->slots[(size_t)index];
    SetRealRemapSelection(s, num);
}

extern "C" void __declspec(dllexport) Ext_SetAllPedRemaps(CPed* ped, int num) {
    PedTextureRemapState* state = EnsureVisibleTextureRemapState(ped);
    if (!state)
        return;
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& s = state->slots[(size_t)i];
        SetRealRemapSelection(s, num);
    }
}

