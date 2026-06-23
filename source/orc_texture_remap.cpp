#include "orc_texture_remap.h"

#include "plugin.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "eModelInfoType.h"
#include "CPools.h"
#include "CTxdStore.h"
#include "CCutsceneMgr.h"
#include "CTimer.h"
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
#include <cstdlib>
#include <utility>

#include "orc_types.h"
#include "orc_app.h"
#include "orc_ini.h"
#include "orc_ini_cache.h"
#include "orc_log.h"
#include "orc_random_pick.h"
#include "orc_skin_native.h"
#include "orc_weapon_assets.h"
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

struct TextureRemapAutoNickCandidate {
    RwTexture* texture = nullptr;
    std::string textureName;
    std::string nickKey;
    std::string nickKeyLower;
};

struct TextureRemapSlotState {
    RwTexture* original = nullptr;
    std::string originalName;
    std::vector<RwTexture*> remaps;
    std::vector<std::string> remapNames;
    std::vector<TextureRemapAutoNickCandidate> autoNickCandidates;
    int selected = -1;
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

struct PedTextureRemapState {
    int modelId = -1;
    int txdIndex = -1;
    std::string dffName;
    std::string fallbackDffName;
    std::string sequentialOwnerKey;
    bool scanned = false;
    bool nickBindingApplied = false;
    int nickBindingId = -1;
    bool autoNickApplied = false;
    int autoNickSlotCount = 0;
    std::string autoNickLogKey;
    int slotCount = 0;
    int totalRemapTextures = 0;
    int totalAutoNickTextures = 0;
    std::array<TextureRemapSlotState, kTextureRemapLimit> slots;
};

struct TextureRemapRestoreEntry {
    RpMaterial* material = nullptr;
    RwTexture* texture = nullptr;
};

using AssignRemapTxdFn = void(__cdecl*)(const char*, uint16_t);
using FindNamedTextureFn = RwTexture*(__cdecl*)(RwTexDictionary*, const char*);

static AssignRemapTxdFn g_AssignRemapTxd_Orig = nullptr;
static FindNamedTextureFn g_RwTexDictionaryFindNamedTexture_Orig = nullptr;
static bool g_textureRemapHooksInstalled = false;
static bool g_textureRemapTxdsNotLoadedYet = true;
static bool g_textureRemapAnyAdditionalPedsTxd = false;
static int g_textureRemapPedsTxdIndex[4] = {};
static bool g_textureRemapPedsTxdRefAdded[4] = {};
static RwTexDictionary* g_textureRemapPedsTxdDict[4] = {};
static int g_textureRemapGangHandsTxdIndex = 0;
static bool g_textureRemapGangHandsTxdRefAdded = false;
static RwTexDictionary* g_textureRemapGangHandsDict = nullptr;
static unsigned int g_textureRemapCutsceneLastTime = 0;
static std::unordered_map<int, PedTextureRemapState> g_pedTextureRemaps;
static std::unordered_map<std::string, PedTextureRemapState> g_clumpTextureRemaps;
static std::unordered_map<std::string, OrcSequentialPickState> g_textureRemapSequentialStates;
static std::unordered_map<std::string, int> g_textureRemapSequentialAssignments;
static std::unordered_map<std::string, OrcRandomNoRepeatPickState> g_textureRemapNoRepeatStates;
static std::unordered_map<std::string, int> g_textureRemapNoRepeatAssignments;
static std::vector<TextureRemapRestoreEntry> g_textureRemapRestoreEntries;
static std::unordered_map<std::string, std::vector<TextureRemapNickBinding>> g_textureRemapNickBindingsByDff;

static uintptr_t ResolveRelativeCallTarget(uintptr_t callSite) {
    const BYTE* p = reinterpret_cast<const BYTE*>(callSite);
    if (!p || *p != 0xE8) return 0;
    const int32_t rel = *reinterpret_cast<const int32_t*>(p + 1);
    return callSite + 5 + rel;
}

static RwTexDictionary* GetTxdDictionaryByIndex(int txdIndex) {
    if (txdIndex < 0 || !CTxdStore::ms_pTxdPool || !CTxdStore::ms_pTxdPool->m_pObjects)
        return nullptr;
    if (txdIndex >= CTxdStore::ms_pTxdPool->m_nSize)
        return nullptr;
    return CTxdStore::ms_pTxdPool->m_pObjects[txdIndex].m_pRwDictionary;
}

static bool IsValidTxdSlotIndex(int txdIndex) {
    return txdIndex >= 0 &&
        CTxdStore::ms_pTxdPool &&
        CTxdStore::ms_pTxdPool->m_pObjects &&
        txdIndex < CTxdStore::ms_pTxdPool->m_nSize;
}

static void ReleaseTextureRemapTxdRef(int& txdIndex, bool& refAdded, RwTexDictionary*& dict, const char* label) {
    if (!refAdded) {
        txdIndex = 0;
        dict = nullptr;
        return;
    }

    const int slot = txdIndex;
    refAdded = false;
    txdIndex = 0;
    dict = nullptr;

    if (!IsValidTxdSlotIndex(slot))
        return;

    __try {
        CTxdStore::RemoveRef(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap: %s TXD RemoveRef SEH ex=0x%08X slot=%d",
            label ? label : "?",
            GetExceptionCode(),
            slot);
    }
}

static bool AddTextureRemapTxdRef(int& txdIndex, bool& refAdded, RwTexDictionary*& dict, int slot, const char* label) {
    if (slot <= 0)
        return false;
    if (refAdded && txdIndex == slot)
        return true;

    ReleaseTextureRemapTxdRef(txdIndex, refAdded, dict, label);

    if (!IsValidTxdSlotIndex(slot)) {
        OrcLogError("texture remap: %s TXD slot invalid slot=%d", label ? label : "?", slot);
        return false;
    }

    __try {
        CTxdStore::AddRef(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap: %s TXD AddRef SEH ex=0x%08X slot=%d",
            label ? label : "?",
            GetExceptionCode(),
            slot);
        return false;
    }

    txdIndex = slot;
    refAdded = true;
    dict = nullptr;
    g_textureRemapTxdsNotLoadedYet = true;
    return true;
}

static void ReleaseAdditionalTextureRemapTxds() {
    for (int i = 0; i < 4; ++i)
        ReleaseTextureRemapTxdRef(g_textureRemapPedsTxdIndex[i],
            g_textureRemapPedsTxdRefAdded[i],
            g_textureRemapPedsTxdDict[i],
            "peds");

    ReleaseTextureRemapTxdRef(g_textureRemapGangHandsTxdIndex,
        g_textureRemapGangHandsTxdRefAdded,
        g_textureRemapGangHandsDict,
        "ganghands");

    g_textureRemapAnyAdditionalPedsTxd = false;
    g_textureRemapTxdsNotLoadedYet = true;
}

static RwTexture* FindTextureInDictOnly(RwTexDictionary* dict, const char* name) {
    if (!dict || !name || !name[0]) return nullptr;
    if (g_RwTexDictionaryFindNamedTexture_Orig)
        return g_RwTexDictionaryFindNamedTexture_Orig(dict, name);
    return RwTexDictionaryFindNamedTexture(dict, name);
}

static void LoadAdditionalTextureRemapTxds() {
    g_textureRemapTxdsNotLoadedYet = false;
    bool anyRequest = false;

    if (g_textureRemapGangHandsTxdIndex) {
        CStreaming::RequestTxdModel(g_textureRemapGangHandsTxdIndex, GAME_REQUIRED | KEEP_IN_MEMORY);
        anyRequest = true;
    }

    if (g_textureRemapAnyAdditionalPedsTxd) {
        for (int i = 0; i < 4; ++i) {
            if (!g_textureRemapPedsTxdIndex[i]) continue;
            CStreaming::RequestTxdModel(g_textureRemapPedsTxdIndex[i], GAME_REQUIRED | KEEP_IN_MEMORY);
            anyRequest = true;
        }
    }

    if (anyRequest)
        CStreaming::LoadAllRequestedModels(false);

    if (g_textureRemapGangHandsTxdIndex)
        g_textureRemapGangHandsDict = GetTxdDictionaryByIndex(g_textureRemapGangHandsTxdIndex);

    for (int i = 0; i < 4; ++i) {
        if (g_textureRemapPedsTxdIndex[i])
            g_textureRemapPedsTxdDict[i] = GetTxdDictionaryByIndex(g_textureRemapPedsTxdIndex[i]);
    }
}

static void __cdecl CustomAssignRemapTxd(const char* txdName, uint16_t txdId) {
    if (txdName && txdName[0]) {
        const size_t len = strlen(txdName);
        if (_strnicmp(txdName, "peds", 4) == 0 && len > 1 && std::isdigit(static_cast<unsigned char>(txdName[len - 1]))) {
            const int arrayIndex = txdName[len - 1] - '1';
            if (arrayIndex >= 0 && arrayIndex < 4) {
                if (AddTextureRemapTxdRef(g_textureRemapPedsTxdIndex[arrayIndex],
                        g_textureRemapPedsTxdRefAdded[arrayIndex],
                        g_textureRemapPedsTxdDict[arrayIndex],
                        txdId,
                        txdName)) {
                    g_textureRemapAnyAdditionalPedsTxd = true;
                    OrcLogInfo("texture remap: found additional %s.txd slot=%u", txdName, (unsigned)txdId);
                }
            }
        } else if (g_textureRemapGangHandsTxdIndex == 0 && _strnicmp(txdName, "ganghands", 9) == 0) {
            AddTextureRemapTxdRef(g_textureRemapGangHandsTxdIndex,
                g_textureRemapGangHandsTxdRefAdded,
                g_textureRemapGangHandsDict,
                txdId,
                txdName);
        }
    }
    if (g_AssignRemapTxd_Orig)
        g_AssignRemapTxd_Orig(txdName, txdId);
}

static RwTexture* __cdecl CustomRwTexDictionaryFindNamedTexture(RwTexDictionary* dict, const char* name) {
    RwTexture* texture = FindTextureInDictOnly(dict, name);
    if (RwTexture* hudIcon = OrcWeaponHudTryRwTexDictionaryFindOverride(dict, name, texture))
        return hudIcon;
    if (texture)
        return texture;

    if (g_textureRemapTxdsNotLoadedYet && g_textureRemapAnyAdditionalPedsTxd)
        LoadAdditionalTextureRemapTxds();

    if (g_textureRemapAnyAdditionalPedsTxd) {
        for (int i = 0; i < 4; ++i) {
            if (!g_textureRemapPedsTxdDict[i]) continue;
            texture = FindTextureInDictOnly(g_textureRemapPedsTxdDict[i], name);
            if (texture) return texture;
        }
    }

    return nullptr;
}

void OrcTextureRemapInstallHooks() {
    if (g_textureRemapHooksInstalled) return;
    g_textureRemapHooksInstalled = true;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("texture remap hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }

    const uintptr_t assignTarget = ResolveRelativeCallTarget(0x5B62C2);
    if (assignTarget) {
        st = MH_CreateHook(reinterpret_cast<void*>(assignTarget),
                           reinterpret_cast<void*>(&CustomAssignRemapTxd),
                           reinterpret_cast<void**>(&g_AssignRemapTxd_Orig));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            st = MH_EnableHook(reinterpret_cast<void*>(assignTarget));
            if (st != MH_OK && st != MH_ERROR_ENABLED)
                OrcLogError("texture remap AssignRemapTxd hook enable -> %s", MH_StatusToString(st));
        } else {
            OrcLogError("texture remap AssignRemapTxd hook create -> %s", MH_StatusToString(st));
        }
    } else {
        OrcLogError("texture remap: cannot resolve AssignRemapTxd call target");
    }

    const uintptr_t findTarget = ResolveRelativeCallTarget(0x4C7533);
    if (findTarget) {
        st = MH_CreateHook(reinterpret_cast<void*>(findTarget),
                           reinterpret_cast<void*>(&CustomRwTexDictionaryFindNamedTexture),
                           reinterpret_cast<void**>(&g_RwTexDictionaryFindNamedTexture_Orig));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            st = MH_EnableHook(reinterpret_cast<void*>(findTarget));
            if (st != MH_OK && st != MH_ERROR_ENABLED)
                OrcLogError("texture remap RwTexDictionaryFindNamedTexture hook enable -> %s", MH_StatusToString(st));
        } else {
            OrcLogError("texture remap RwTexDictionaryFindNamedTexture hook create -> %s", MH_StatusToString(st));
        }
    } else {
        OrcLogError("texture remap: cannot resolve RwTexDictionaryFindNamedTexture call target");
    }
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
    state.autoNickApplied = false;
    state.autoNickSlotCount = 0;
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

static void EraseTextureRemapSequentialChoiceFromStates(const std::string& choiceKey) {
    for (auto& kv : g_textureRemapSequentialStates)
        kv.second.activeChoiceKeys.erase(choiceKey);
}

static void ReleaseTextureRemapNoRepeatChoiceFromStates(
    const std::string& choiceKey,
    const std::unordered_map<std::string, int>& assignments,
    bool recycle) {
    for (auto& kv : g_textureRemapNoRepeatStates)
        OrcRandomNoRepeatReleaseChoice(kv.second, assignments, choiceKey, recycle);
}

static void PruneTextureRemapAssignments(
    std::unordered_map<std::string, int>& assignments,
    bool noRepeatAssignments) {
    for (auto it = assignments.begin(); it != assignments.end();) {
        bool erase = false;
        if (it->first.rfind("r:", 0) == 0) {
            const size_t sep = it->first.find('|');
            const int ref = (sep == std::string::npos) ? 0 : std::atoi(it->first.substr(2, sep - 2).c_str());
            erase = ref > 0 && !OrcSafeGetPed(ref);
        }

        if (erase) {
            if (noRepeatAssignments)
                ReleaseTextureRemapNoRepeatChoiceFromStates(it->first, assignments, true);
            else
                EraseTextureRemapSequentialChoiceFromStates(it->first);
            it = assignments.erase(it);
        } else {
            ++it;
        }
    }
}

static void PruneTextureRemapTrackedAssignments() {
    PruneTextureRemapAssignments(g_textureRemapSequentialAssignments, false);
    PruneTextureRemapAssignments(g_textureRemapNoRepeatAssignments, true);
}

static void ReleaseTextureRemapTrackedAssignmentsForOwner(const std::string& ownerKey) {
    if (ownerKey.empty())
        return;
    const std::string prefix = ownerKey + "|";
    for (auto it = g_textureRemapSequentialAssignments.begin(); it != g_textureRemapSequentialAssignments.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            EraseTextureRemapSequentialChoiceFromStates(it->first);
            it = g_textureRemapSequentialAssignments.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_textureRemapNoRepeatAssignments.begin(); it != g_textureRemapNoRepeatAssignments.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            ReleaseTextureRemapNoRepeatChoiceFromStates(it->first, g_textureRemapNoRepeatAssignments, true);
            it = g_textureRemapNoRepeatAssignments.erase(it);
        } else {
            ++it;
        }
    }
}

static bool TextureRemapPedPointerAlive(uintptr_t pedPtr) {
    if (!pedPtr || !CPools::ms_pPedPool)
        return true;
    CPed* target = reinterpret_cast<CPed*>(pedPtr);
    __try {
        for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
            if (CPools::ms_pPedPool->GetAt(i) == target)
                return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}

static bool TextureRemapOwnerIsDead(const std::string& ownerKey) {
    if (ownerKey.rfind("r:", 0) == 0) {
        const int ref = std::atoi(ownerKey.c_str() + 2);
        return ref > 0 && !OrcSafeGetPed(ref);
    }
    if (ownerKey.rfind("p:", 0) == 0) {
        const uintptr_t ptr = static_cast<uintptr_t>(_strtoui64(ownerKey.c_str() + 2, nullptr, 10));
        return ptr != 0 && !TextureRemapPedPointerAlive(ptr);
    }
    return false;
}

static void PruneTextureRemapStateMaps() {
    if (!TextureRemapPickModeTracksActiveChoices())
        return;

    for (auto it = g_pedTextureRemaps.begin(); it != g_pedTextureRemaps.end();) {
        if (TextureRemapOwnerIsDead(it->second.sequentialOwnerKey)) {
            ReleaseTextureRemapTrackedAssignmentsForOwner(it->second.sequentialOwnerKey);
            it = g_pedTextureRemaps.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = g_clumpTextureRemaps.begin(); it != g_clumpTextureRemaps.end();) {
        if (TextureRemapOwnerIsDead(it->second.sequentialOwnerKey)) {
            ReleaseTextureRemapTrackedAssignmentsForOwner(it->second.sequentialOwnerKey);
            it = g_clumpTextureRemaps.erase(it);
        } else {
            ++it;
        }
    }

    PruneTextureRemapTrackedAssignments();
}

static int PickTrackedTextureRemapIndex(PedTextureRemapState& state, const std::string& poolKey, int total) {
    const std::vector<int> pool = TextureRemapIndexPool(total);
    if (pool.empty() || state.sequentialOwnerKey.empty())
        return -1;
    PruneTextureRemapTrackedAssignments();
    const std::string choiceKey = TextureRemapSequentialChoiceKey(state, poolKey);
    if (g_skinTextureRemapPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        return OrcRandomNoRepeatPickSticky(
            g_textureRemapNoRepeatStates[poolKey],
            g_textureRemapNoRepeatAssignments,
            choiceKey,
            pool,
            -1);
    }
    return OrcSequentialPickSticky(
        g_textureRemapSequentialStates[poolKey],
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
    state.nickBindingApplied = false;
    state.nickBindingId = -1;
    ClearTextureRemapAutoNickSelections(state);
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
        for (const auto& saved : binding.slots) {
            if (_stricmp(saved.originalName.c_str(), slot.originalName.c_str()) != 0)
                continue;

            if (saved.remapName.empty()) {
                slot.selected = -1;
                any = true;
                break;
            }

            bool matched = false;
            for (int r = 0; r < (int)slot.remapNames.size(); ++r) {
                if (_stricmp(saved.remapName.c_str(), slot.remapNames[(size_t)r].c_str()) == 0) {
                    slot.selected = r;
                    any = true;
                    matched = true;
                    break;
                }
            }
            if (!matched)
                slot.selected = -1;
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
    const std::string& nickLower,
    bool& matchedBinding
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

        matchedBinding = true;
        if (state.nickBindingApplied && state.nickBindingId != rit->id)
            SelectRandomTextureRemaps(state);

        if (ApplyTextureRemapNickBindingToState(state, *rit)) {
            state.nickBindingApplied = true;
            state.nickBindingId = rit->id;
            return true;
        }
        return false;
    }
    return false;
}

static bool TryApplyTextureRemapManualNickBinding(
    PedTextureRemapState& state,
    const std::string& nickLower,
    bool& matchedBinding
) {
    matchedBinding = false;
    if (!g_skinTextureRemapNickMode)
        return false;

    if (TryApplyTextureRemapManualNickBindingForDff(state, TextureRemapPrimaryDff(state), nickLower, matchedBinding))
        return true;
    if (!state.fallbackDffName.empty())
        return TryApplyTextureRemapManualNickBindingForDff(state, state.fallbackDffName.c_str(), nickLower, matchedBinding);
    return false;
}

static bool ApplyTextureRemapAutoNickBinding(PedTextureRemapState& state, const char* nick, const std::string& nickLower) {
    ClearTextureRemapAutoNickSlotSelections(state);
    state.autoNickApplied = false;
    state.autoNickSlotCount = 0;
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

    state.autoNickApplied = true;
    state.autoNickSlotCount = appliedSlots;
    if (state.autoNickLogKey != logKey) {
        const char* dff = TextureRemapPrimaryDff(state);
        OrcLogInfo("texture remap auto nick: model=%d dff=%s nick=%s slots=%d",
                   state.modelId, dff ? dff : "?", nick ? nick : "", appliedSlots);
        state.autoNickLogKey = logKey;
    }
    return true;
}

static bool ApplyTextureRemapNickBinding(CPed* ped, PedTextureRemapState& state) {
    if ((!g_skinTextureRemapNickMode && !g_skinTextureRemapAutoNickMode) || !samp_bridge::IsSampBuildKnown()) {
        if (state.nickBindingApplied)
            SelectRandomTextureRemaps(state);
        else
            ClearTextureRemapAutoNickSelections(state);
        return false;
    }

    char nick[32] = {};
    bool isLocal = false;
    if (!samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
        if (state.nickBindingApplied)
            SelectRandomTextureRemaps(state);
        else
            ClearTextureRemapAutoNickSelections(state);
        return false;
    }

    const std::string manualNickLower = TextureRemapToLowerAscii(nick);
    const std::string autoNickLower = TextureRemapNormalizeNickForMatch(nick);
    bool matchedManualBinding = false;
    if (TryApplyTextureRemapManualNickBinding(state, manualNickLower, matchedManualBinding)) {
        ClearTextureRemapAutoNickSelections(state);
        return true;
    }

    if (matchedManualBinding) {
        if (state.nickBindingApplied)
            SelectRandomTextureRemaps(state);
        state.nickBindingApplied = false;
        state.nickBindingId = -1;
        ClearTextureRemapAutoNickSelections(state);
        return false;
    }

    if (state.nickBindingApplied)
        SelectRandomTextureRemaps(state);
    state.nickBindingApplied = false;
    state.nickBindingId = -1;

    return ApplyTextureRemapAutoNickBinding(state, nick, autoNickLower);
}

static int GetOrAddTextureRemapSlot(PedTextureRemapState& state, RwTexDictionary* dict, const std::string& originalName) {
    for (int i = 0; i < state.slotCount; ++i) {
        if (_stricmp(state.slots[(size_t)i].originalName.c_str(), originalName.c_str()) == 0)
            return i;
    }
    if (state.slotCount >= kTextureRemapLimit)
        return -1;

    RwTexture* original = FindTextureInDictOnly(dict, originalName.c_str());
    if (!original)
        return -1;

    const int idx = state.slotCount++;
    TextureRemapSlotState& slot = state.slots[(size_t)idx];
    slot.original = original;
    slot.originalName = originalName;
    slot.remaps.clear();
    slot.remapNames.clear();
    slot.autoNickCandidates.clear();
    slot.selected = -1;
    slot.autoNickSelected = -1;
    return idx;
}

struct TextureRemapTextureRef {
    RwTexture* texture = nullptr;
    std::string name;
    std::string lowerName;
};

struct TextureRemapOriginalName {
    std::string name;
    std::string lowerName;
};

struct TextureRemapScanCtx {
    PedTextureRemapState* state = nullptr;
    RwTexDictionary* dict = nullptr;
    std::vector<TextureRemapTextureRef> textures;
};

static RwTexture* TextureRemapCollectTextureCB(RwTexture* texture, void* data) {
    if (!texture || !data) return texture;
    TextureRemapScanCtx* ctx = reinterpret_cast<TextureRemapScanCtx*>(data);
    if (!ctx->state || !ctx->dict) return texture;

    const std::string name = texture->name;
    if (!name.empty())
        ctx->textures.push_back({ texture, name, TextureRemapToLowerAscii(name) });

    const size_t remapPos = TextureRemapToLowerAscii(name).find("_remap");
    if (remapPos == std::string::npos || remapPos == 0)
        return texture;

    const std::string originalName = name.substr(0, remapPos);
    const int slotIdx = GetOrAddTextureRemapSlot(*ctx->state, ctx->dict, originalName);
    if (slotIdx < 0) {
        static int s_limitLogsLeft = 8;
        if (s_limitLogsLeft > 0) {
            OrcLogError("texture remap: cannot add %s (limit=%d or missing base texture)", name.c_str(), kTextureRemapLimit);
            s_limitLogsLeft--;
        }
        return texture;
    }

    TextureRemapSlotState& slot = ctx->state->slots[(size_t)slotIdx];
    if (SlotHasRemapVariant(slot, texture, name))
        return texture;

    slot.remaps.push_back(texture);
    slot.remapNames.push_back(name);
    ctx->state->totalRemapTextures++;
    return texture;
}

static void SortTextureRemapVariants(PedTextureRemapState& state) {
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        std::vector<size_t> order;
        order.reserve(slot.remapNames.size());
        for (size_t idx = 0; idx < slot.remapNames.size(); ++idx)
            order.push_back(idx);
        std::sort(order.begin(), order.end(), [&slot](size_t a, size_t b) {
            return TextureRemapToLowerAscii(slot.remapNames[a]) < TextureRemapToLowerAscii(slot.remapNames[b]);
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

static bool TextureRemapHasOriginalName(const std::vector<TextureRemapOriginalName>& originals, const std::string& lowerName) {
    for (const auto& original : originals) {
        if (original.lowerName == lowerName)
            return true;
    }
    return false;
}

static void TextureRemapAddOriginalName(std::vector<TextureRemapOriginalName>& originals, const char* name) {
    if (!name || !name[0])
        return;

    TextureRemapOriginalName original;
    original.name = name;
    original.lowerName = TextureRemapToLowerAscii(original.name);
    if (original.lowerName.empty() || TextureRemapHasOriginalName(originals, original.lowerName))
        return;
    originals.push_back(std::move(original));
}

struct TextureRemapOriginalScanCtx {
    std::vector<TextureRemapOriginalName>* originals = nullptr;
};

static RpMaterial* TextureRemapCollectOriginalMaterialCB(RpMaterial* material, void* data) {
    if (!material || !material->texture || !data)
        return material;
    TextureRemapOriginalScanCtx* ctx = reinterpret_cast<TextureRemapOriginalScanCtx*>(data);
    if (!ctx->originals)
        return material;
    TextureRemapAddOriginalName(*ctx->originals, material->texture->name);
    return material;
}

static RpAtomic* TextureRemapCollectOriginalAtomicCB(RpAtomic* atomic, void* data) {
    if (!atomic || !atomic->geometry)
        return atomic;
    RpGeometryForAllMaterials(atomic->geometry, TextureRemapCollectOriginalMaterialCB, data);
    return atomic;
}

static void CollectPedMaterialOriginalNames(CPed* ped, std::vector<TextureRemapOriginalName>& originals) {
    if (!ped || !ped->m_pRwClump)
        return;
    TextureRemapOriginalScanCtx ctx;
    ctx.originals = &originals;
    __try {
        RpClumpForAllAtomics(ped->m_pRwClump, TextureRemapCollectOriginalAtomicCB, &ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap material scan: SEH ex=0x%08X ped=%p", GetExceptionCode(), ped);
    }
}

static void CollectClumpMaterialOriginalNames(RpClump* clump, std::vector<TextureRemapOriginalName>& originals) {
    if (!clump)
        return;
    TextureRemapOriginalScanCtx ctx;
    ctx.originals = &originals;
    __try {
        RpClumpForAllAtomics(clump, TextureRemapCollectOriginalAtomicCB, &ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap clump material scan: SEH ex=0x%08X clump=%p", GetExceptionCode(), clump);
    }
}

static void CollectAutoNickTextureCandidates(
    PedTextureRemapState& state,
    RwTexDictionary* dict,
    const std::vector<TextureRemapTextureRef>& textures,
    const std::vector<TextureRemapOriginalName>& materialOriginals
) {
    std::vector<TextureRemapOriginalName> originals = materialOriginals;
    for (int i = 0; i < state.slotCount; ++i)
        TextureRemapAddOriginalName(originals, state.slots[(size_t)i].originalName.c_str());

    if (originals.empty())
        return;

    for (const auto& texture : textures) {
        if (!texture.texture || texture.name.empty())
            continue;
        if (texture.lowerName.find("_remap") != std::string::npos)
            continue;

        const TextureRemapOriginalName* bestOriginal = nullptr;
        size_t bestLen = 0;
        for (const auto& original : originals) {
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

        if (!bestOriginal)
            continue;

        const std::string nickKey = texture.name.substr(bestLen + 1);
        if (nickKey.empty())
            continue;

        const int slotIdx = GetOrAddTextureRemapSlot(state, dict, bestOriginal->name);
        if (slotIdx < 0)
            continue;

        TextureRemapSlotState& slot = state.slots[(size_t)slotIdx];
        if (SlotHasAutoNickCandidate(slot, texture.texture, texture.name))
            continue;

        TextureRemapAutoNickCandidate candidate;
        candidate.texture = texture.texture;
        candidate.textureName = texture.name;
        candidate.nickKey = nickKey;
        candidate.nickKeyLower = TextureRemapToLowerAscii(nickKey);
        slot.autoNickCandidates.push_back(std::move(candidate));
        state.totalAutoNickTextures++;
    }
}

static bool ScanTextureRemapsForPed(CPed* ped, int modelId, const std::string& ownerKey, PedTextureRemapState& out) {
    out = PedTextureRemapState{};
    out.modelId = modelId;
    out.sequentialOwnerKey = ownerKey;
    const char* dff = OrcTryGetPedModelNameById(modelId);
    if (dff && dff[0])
        out.dffName = dff;

    CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
    if (!mi || mi->GetModelType() != MODEL_INFO_PED)
        return false;

    out.txdIndex = mi->m_nTxdIndex;
    RwTexDictionary* dict = GetTxdDictionaryByIndex(out.txdIndex);
    if (!dict)
        return false;

    TextureRemapScanCtx ctx;
    ctx.state = &out;
    ctx.dict = dict;
    __try {
        RwTexDictionaryForAllTextures(dict, TextureRemapCollectTextureCB, &ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap scan: SEH ex=0x%08X model=%d txd=%d", GetExceptionCode(), modelId, out.txdIndex);
        out = PedTextureRemapState{};
        out.modelId = modelId;
        out.sequentialOwnerKey = ownerKey;
        return false;
    }

    std::vector<TextureRemapOriginalName> materialOriginals;
    CollectPedMaterialOriginalNames(ped, materialOriginals);
    CollectAutoNickTextureCandidates(out, dict, ctx.textures, materialOriginals);

    out.scanned = true;
    if (out.totalRemapTextures > 0 || out.totalAutoNickTextures > 0) {
        SortTextureRemapVariants(out);
        SelectRandomTextureRemaps(out);
        OrcLogInfo("texture remap scan: model=%d txd=%d slots=%d variants=%d autoNick=%d",
                   modelId, out.txdIndex, out.slotCount, out.totalRemapTextures, out.totalAutoNickTextures);
    }
    return true;
}

static bool ScanTextureRemapsForClump(
    CPed* ped,
    RpClump* clump,
    int txdIndex,
    const char* dffName,
    const char* fallbackDffName,
    const std::string& ownerKey,
    PedTextureRemapState& out
) {
    out = PedTextureRemapState{};
    out.modelId = ped ? (int)ped->m_nModelIndex : -1;
    out.txdIndex = txdIndex;
    out.sequentialOwnerKey = ownerKey;
    if (dffName && dffName[0])
        out.dffName = dffName;
    if (fallbackDffName && fallbackDffName[0] && _stricmp(fallbackDffName, out.dffName.c_str()) != 0)
        out.fallbackDffName = fallbackDffName;

    RwTexDictionary* dict = GetTxdDictionaryByIndex(out.txdIndex);
    if (!clump || !dict)
        return false;

    TextureRemapScanCtx ctx;
    ctx.state = &out;
    ctx.dict = dict;
    __try {
        RwTexDictionaryForAllTextures(dict, TextureRemapCollectTextureCB, &ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap clump scan: SEH ex=0x%08X txd=%d dff=%s", GetExceptionCode(), out.txdIndex, out.dffName.c_str());
        out = PedTextureRemapState{};
        out.sequentialOwnerKey = ownerKey;
        return false;
    }

    std::vector<TextureRemapOriginalName> materialOriginals;
    CollectClumpMaterialOriginalNames(clump, materialOriginals);
    CollectAutoNickTextureCandidates(out, dict, ctx.textures, materialOriginals);

    out.scanned = true;
    if (out.totalRemapTextures > 0 || out.totalAutoNickTextures > 0) {
        SortTextureRemapVariants(out);
        SelectRandomTextureRemaps(out);
        OrcLogInfo("texture remap clump scan: dff=%s fallback=%s model=%d txd=%d slots=%d variants=%d autoNick=%d",
                   out.dffName.empty() ? "?" : out.dffName.c_str(),
                   out.fallbackDffName.empty() ? "-" : out.fallbackDffName.c_str(),
                   out.modelId,
                   out.txdIndex,
                   out.slotCount,
                   out.totalRemapTextures,
                   out.totalAutoNickTextures);
    }
    return true;
}

static int TextureRemapPedKey(CPed* ped) {
    const int ref = OrcSafeGetPedRef(ped);
    if (ref > 0)
        return ref;
    if (!ped)
        return 0;
    return (int)(reinterpret_cast<uintptr_t>(ped) & 0x7fffffff);
}

static PedTextureRemapState* EnsurePedTextureRemapState(CPed* ped, bool forceRescan = false) {
    if (!ped) return nullptr;
    PruneTextureRemapStateMaps();
    const int key = TextureRemapPedKey(ped);
    if (!key) return nullptr;

    auto it = g_pedTextureRemaps.find(key);
    if (it != g_pedTextureRemaps.end() &&
        !forceRescan &&
        it->second.scanned &&
        it->second.modelId == (int)ped->m_nModelIndex) {
        return &it->second;
    }

    const std::string ownerKey = TextureRemapSequentialOwnerKey(ped);
    PedTextureRemapState fresh;
    if (!ScanTextureRemapsForPed(ped, (int)ped->m_nModelIndex, ownerKey, fresh)) {
        fresh.modelId = (int)ped->m_nModelIndex;
        fresh.sequentialOwnerKey = ownerKey;
        fresh.scanned = true;
    }
    auto inserted = g_pedTextureRemaps.insert_or_assign(key, std::move(fresh));
    return &inserted.first->second;
}

static PedTextureRemapState* EnsureClumpTextureRemapState(
    CPed* ped,
    RpClump* clump,
    const char* dffName,
    const char* fallbackDffName,
    int txdIndex,
    bool forceRescan = false
) {
    if (!ped || !clump || txdIndex < 0)
        return nullptr;
    PruneTextureRemapStateMaps();
    const int pedKey = TextureRemapPedKey(ped);
    if (!pedKey)
        return nullptr;
    std::string key = std::to_string(pedKey);
    key += "|";
    key += dffName && dffName[0] ? TextureRemapToLowerAscii(dffName) : "?";
    key += "|";
    key += fallbackDffName && fallbackDffName[0] ? TextureRemapToLowerAscii(fallbackDffName) : "-";
    key += "|";
    key += std::to_string(txdIndex);

    auto it = g_clumpTextureRemaps.find(key);
    if (it != g_clumpTextureRemaps.end() && !forceRescan && it->second.scanned)
        return &it->second;

    const std::string ownerKey = TextureRemapSequentialOwnerKey(ped);
    PedTextureRemapState fresh;
    if (!ScanTextureRemapsForClump(ped, clump, txdIndex, dffName, fallbackDffName, ownerKey, fresh)) {
        fresh.modelId = (int)ped->m_nModelIndex;
        fresh.txdIndex = txdIndex;
        fresh.sequentialOwnerKey = ownerKey;
        if (dffName && dffName[0])
            fresh.dffName = dffName;
        if (fallbackDffName && fallbackDffName[0])
            fresh.fallbackDffName = fallbackDffName;
        fresh.scanned = true;
    }
    auto inserted = g_clumpTextureRemaps.insert_or_assign(key, std::move(fresh));
    return &inserted.first->second;
}

static PedTextureRemapState* EnsureActiveTextureRemapState(CPed* ped, bool forceRescan = false) {
    OrcNativeSkinActiveInfo nativeInfo;
    if (OrcSkinNativeGetActiveInfo(ped, nativeInfo)) {
        return EnsureClumpTextureRemapState(
            ped,
            nativeInfo.clump,
            nativeInfo.dffName,
            nativeInfo.fallbackDffName,
            nativeInfo.txdSlot,
            forceRescan);
    }
    return EnsurePedTextureRemapState(ped, forceRescan);
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

void OrcCollectPedTextureRemapStats(std::vector<TextureRemapPedInfo>& out) {
    out.clear();
    for (const auto& kv : g_pedTextureRemaps) {
        const PedTextureRemapState& state = kv.second;
        if (!state.scanned || state.totalRemapTextures <= 0)
            continue;
        TextureRemapPedInfo info;
        FillTextureRemapPedInfo(state, info);
        out.push_back(std::move(info));
    }
    for (const auto& kv : g_clumpTextureRemaps) {
        const PedTextureRemapState& state = kv.second;
        if (!state.scanned || state.totalRemapTextures <= 0)
            continue;
        TextureRemapPedInfo info;
        FillTextureRemapPedInfo(state, info);
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const TextureRemapPedInfo& a, const TextureRemapPedInfo& b) {
        return a.modelId < b.modelId;
    });
}

bool OrcGetLocalPedTextureRemaps(TextureRemapPedInfo& out) {
    CPlayerPed* ped = FindPlayerPed(0);
    if (!ped) {
        out = TextureRemapPedInfo{};
        return false;
    }
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state) {
        out = TextureRemapPedInfo{};
        return false;
    }
    FillTextureRemapPedInfo(*state, out);
    return true;
}

bool OrcSetLocalPedTextureRemap(int slot, int remap) {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state || slot < 0 || slot >= state->slotCount)
        return false;
    TextureRemapSlotState& s = state->slots[(size_t)slot];
    if (TextureRemapPickModeTracksActiveChoices())
        ReleaseTextureRemapTrackedAssignmentsForOwner(state->sequentialOwnerKey);
    return SetRealRemapSelection(s, remap);
}

bool OrcRandomizeLocalPedTextureRemaps() {
    CPlayerPed* ped = FindPlayerPed(0);
    if (TextureRemapPickModeTracksActiveChoices()) {
        if (PedTextureRemapState* oldState = EnsureActiveTextureRemapState(ped, false))
            ReleaseTextureRemapTrackedAssignmentsForOwner(oldState->sequentialOwnerKey);
        PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, true);
        return state && state->totalRemapTextures > 0;
    }

    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, true);
    if (!state || state->totalRemapTextures <= 0)
        return false;
    SelectRandomTextureRemaps(*state);
    return true;
}

bool OrcSetAllLocalPedTextureRemaps(int remap) {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state)
        return false;
    if (TextureRemapPickModeTracksActiveChoices())
        ReleaseTextureRemapTrackedAssignmentsForOwner(state->sequentialOwnerKey);
    bool ok = true;
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& s = state->slots[(size_t)i];
        if (!SetRealRemapSelection(s, remap))
            ok = false;
    }
    return ok;
}

void OrcReloadTextureRemapNickBindings() {
    g_textureRemapNickBindingsByDff.clear();
}

void OrcCollectLocalPedTextureRemapNickBindings(std::vector<TextureRemapNickBindingInfo>& out) {
    out.clear();
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state)
        return;

    const char* dff = OrcTryGetPedModelNameById(state->modelId);
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

    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state || state->slotCount <= 0)
        return false;

    const char* dff = OrcTryGetPedModelNameById(state->modelId);
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
    return true;
}

bool OrcDeleteLocalPedTextureRemapNickBinding(int bindingId) {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state || bindingId < 0)
        return false;

    const char* dff = OrcTryGetPedModelNameById(state->modelId);
    const std::string path = TextureRemapIniPathForDff(dff, state->modelId);
    char section[32] = {};
    _snprintf_s(section, _TRUNCATE, "Binding.%d", bindingId);
    if (!OrcIniDeleteSection(path.c_str(), section))
        return false;

    OrcIniCacheInvalidatePath(path.c_str());
    const std::string key = TextureRemapDffKey(dff, state->modelId);
    g_textureRemapNickBindingsByDff.erase(key);
    return true;
}

static RpMaterial* TextureRemapApplyMaterialCB(RpMaterial* material, void* data) {
    if (!material || !data || !material->texture) return material;
    PedTextureRemapState* state = reinterpret_cast<PedTextureRemapState*>(data);
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& slot = state->slots[(size_t)i];
        if (material->texture != slot.original)
            continue;

        RwTexture* replacement = nullptr;
        if (slot.autoNickSelected >= 0 && slot.autoNickSelected < (int)slot.autoNickCandidates.size()) {
            replacement = slot.autoNickCandidates[(size_t)slot.autoNickSelected].texture;
        } else if (slot.selected >= 0 && slot.selected < (int)slot.remaps.size()) {
            replacement = slot.remaps[(size_t)slot.selected];
        }
        if (!replacement)
            continue;

        g_textureRemapRestoreEntries.push_back({ material, material->texture });
        material->texture = replacement;
        break;
    }
    return material;
}

static RpAtomic* TextureRemapApplyAtomicCB(RpAtomic* atomic, void* data) {
    if (!atomic || !atomic->geometry) return atomic;
    RpGeometryForAllMaterials(atomic->geometry, TextureRemapApplyMaterialCB, data);
    return atomic;
}

void OrcTextureRemapApplyBefore(CPed* ped) {
    if (!g_enabled || !g_skinTextureRemapEnabled || !ped || !ped->m_pRwClump)
        return;
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state || state->slotCount <= 0 || (state->totalRemapTextures <= 0 && state->totalAutoNickTextures <= 0))
        return;
    ApplyTextureRemapNickBinding(ped, *state);
    __try {
        RpClumpForAllAtomics(ped->m_pRwClump, TextureRemapApplyAtomicCB, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap before: SEH ex=0x%08X ped=%p", GetExceptionCode(), ped);
    }
}

void OrcTextureRemapApplyToClumpBefore(CPed* ped, RpClump* clump, const char* dffName, const char* fallbackDffName, int txdIndex) {
    if (!g_enabled || !g_skinTextureRemapEnabled || !ped || !clump || txdIndex < 0)
        return;
    PedTextureRemapState* state = EnsureClumpTextureRemapState(ped, clump, dffName, fallbackDffName, txdIndex, false);
    if (!state || state->slotCount <= 0 || (state->totalRemapTextures <= 0 && state->totalAutoNickTextures <= 0))
        return;
    ApplyTextureRemapNickBinding(ped, *state);
    __try {
        RpClumpForAllAtomics(clump, TextureRemapApplyAtomicCB, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap clump before: SEH ex=0x%08X ped=%p dff=%s", GetExceptionCode(), ped, dffName ? dffName : "?");
    }
}

void OrcTextureRemapRestoreAfter() {
    if (g_textureRemapRestoreEntries.empty())
        return;
    for (auto it = g_textureRemapRestoreEntries.rbegin(); it != g_textureRemapRestoreEntries.rend(); ++it) {
        if (!it->material) continue;
        __try {
            it->material->texture = it->texture;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_textureRemapRestoreEntries.clear();
}

void OrcTextureRemapClearRuntimeState() {
    OrcTextureRemapRestoreAfter();
    g_pedTextureRemaps.clear();
    g_clumpTextureRemaps.clear();
    g_textureRemapSequentialStates.clear();
    g_textureRemapSequentialAssignments.clear();
    g_textureRemapNoRepeatStates.clear();
    g_textureRemapNoRepeatAssignments.clear();
    if (OrcIsRuntimeShuttingDown())
        ReleaseAdditionalTextureRemapTxds();
}

int OrcTextureRemapQueryMaxLinkedVariantsForTxd(int txdIndex) {
    if (!g_enabled || txdIndex < 0)
        return 0;
    RwTexDictionary* dict = GetTxdDictionaryByIndex(txdIndex);
    if (!dict)
        return 0;
    PedTextureRemapState out{};
    out.modelId = -1;
    out.txdIndex = txdIndex;
    TextureRemapScanCtx ctx;
    ctx.state = &out;
    ctx.dict = dict;
    __try {
        RwTexDictionaryForAllTextures(dict, TextureRemapCollectTextureCB, &ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap txd query: SEH ex=0x%08X txd=%d", GetExceptionCode(), txdIndex);
        return 0;
    }
    int maxVariants = 0;
    for (int i = 0; i < out.slotCount; ++i)
        maxVariants = std::max(maxVariants, (int)out.slots[(size_t)i].remaps.size());
    return maxVariants;
}

void OrcTextureRemapOnProcessScripts() {
    if (CCutsceneMgr::ms_running)
        g_textureRemapCutsceneLastTime = CTimer::m_snTimeInMilliseconds;
}

void OrcTextureRemapOnPedSetModel(CPed* ped, int) {
    if (!ped)
        return;
    const int key = TextureRemapPedKey(ped);
    if (key) {
        auto old = g_pedTextureRemaps.find(key);
        if (old != g_pedTextureRemaps.end())
            ReleaseTextureRemapTrackedAssignmentsForOwner(old->second.sequentialOwnerKey);
        g_pedTextureRemaps.erase(key);
    }
    for (auto it = g_clumpTextureRemaps.begin(); it != g_clumpTextureRemaps.end();) {
        const std::string prefix = std::to_string(key) + "|";
        if (key && it->first.rfind(prefix, 0) == 0) {
            ReleaseTextureRemapTrackedAssignmentsForOwner(it->second.sequentialOwnerKey);
            it = g_clumpTextureRemaps.erase(it);
        } else {
            ++it;
        }
    }
    if ((CTimer::m_snTimeInMilliseconds - g_textureRemapCutsceneLastTime) > 3000 && g_skinTextureRemapEnabled)
        EnsurePedTextureRemapState(ped, true);
}

extern "C" int32_t __declspec(dllexport) Ext_GetPedRemap(CPed* ped, int index) {
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state || index < 0 || index >= state->slotCount)
        return -1;
    return state->slots[(size_t)index].selected;
}

extern "C" void __declspec(dllexport) Ext_SetPedRemap(CPed* ped, int index, int num) {
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state || index < 0 || index >= state->slotCount)
        return;
    TextureRemapSlotState& s = state->slots[(size_t)index];
    SetRealRemapSelection(s, num);
}

extern "C" void __declspec(dllexport) Ext_SetAllPedRemaps(CPed* ped, int num) {
    PedTextureRemapState* state = EnsureActiveTextureRemapState(ped, false);
    if (!state)
        return;
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& s = state->slots[(size_t)i];
        SetRealRemapSelection(s, num);
    }
}

