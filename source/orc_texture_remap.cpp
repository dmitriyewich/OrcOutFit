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
#include "orc_log.h"
#include "samp_bridge.h"
#include "external/MinHook/include/MinHook.h"

using namespace plugin;

static std::string TextureRemapToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

static void TextureRemapEnsureDirectoryExists(const char* fullDirPath) {
    if (!fullDirPath || !fullDirPath[0]) return;
    std::string path = fullDirPath;
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '\\') continue;
        std::string partial = path.substr(0, i);
        if (partial.size() == 2 && partial[1] == ':') continue;
        CreateDirectoryA(partial.c_str(), nullptr);
    }
    CreateDirectoryA(path.c_str(), nullptr);
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

struct TextureRemapSlotState {
    RwTexture* original = nullptr;
    std::string originalName;
    std::vector<RwTexture*> remaps;
    std::vector<std::string> remapNames;
    int selected = -1;
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

struct PedTextureRemapState {
    int modelId = -1;
    int txdIndex = -1;
    bool scanned = false;
    bool nickBindingApplied = false;
    int nickBindingId = -1;
    int slotCount = 0;
    int totalRemapTextures = 0;
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
static RwTexDictionary* g_textureRemapPedsTxdDict[4] = {};
static int g_textureRemapGangHandsTxdIndex = 0;
static RwTexDictionary* g_textureRemapGangHandsDict = nullptr;
static unsigned int g_textureRemapCutsceneLastTime = 0;
static std::unordered_map<int, PedTextureRemapState> g_pedTextureRemaps;
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
                g_textureRemapPedsTxdIndex[arrayIndex] = txdId;
                CTxdStore::AddRef(txdId);
                g_textureRemapAnyAdditionalPedsTxd = true;
                OrcLogInfo("texture remap: found additional %s.txd slot=%u", txdName, (unsigned)txdId);
            }
        } else if (g_textureRemapGangHandsTxdIndex == 0 && _strnicmp(txdName, "ganghands", 9) == 0) {
            g_textureRemapGangHandsTxdIndex = txdId;
            CTxdStore::AddRef(txdId);
        }
    }
    if (g_AssignRemapTxd_Orig)
        g_AssignRemapTxd_Orig(txdName, txdId);
}

static RwTexture* __cdecl CustomRwTexDictionaryFindNamedTexture(RwTexDictionary* dict, const char* name) {
    RwTexture* texture = FindTextureInDictOnly(dict, name);
    if (texture) return texture;

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

static void SelectRandomTextureRemapsPerTexture(PedTextureRemapState& state) {
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        const int total = (int)slot.remaps.size();
        if (total <= 0) {
            slot.selected = -1;
            continue;
        }
        slot.selected = RandomInclusive(0, total - 1);
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

    const int linkedVariant = RandomInclusive(0, maxVariants - 1);
    for (int i = 0; i < state.slotCount; ++i) {
        TextureRemapSlotState& slot = state.slots[(size_t)i];
        const int total = (int)slot.remaps.size();
        if (total <= 0) {
            slot.selected = -1;
        } else if (linkedVariant < total) {
            slot.selected = linkedVariant;
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
}

static void LoadTextureRemapNickBindingsForDff(const char* dffName, int modelId) {
    const std::string key = TextureRemapDffKey(dffName, modelId);
    if (g_textureRemapNickBindingsByDff.find(key) != g_textureRemapNickBindingsByDff.end())
        return;

    std::vector<TextureRemapNickBinding> bindings;
    const std::string path = TextureRemapIniPathForDff(dffName, modelId);
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::vector<char> names(8192, '\0');
        DWORD chars = GetPrivateProfileSectionNamesA(names.data(), (DWORD)names.size(), path.c_str());
        while (chars == names.size() - 2) {
            names.assign(names.size() * 2, '\0');
            chars = GetPrivateProfileSectionNamesA(names.data(), (DWORD)names.size(), path.c_str());
        }

        for (const char* sec = names.data(); sec && *sec; sec += strlen(sec) + 1) {
            if (_strnicmp(sec, "Binding.", 8) != 0)
                continue;

            TextureRemapNickBinding binding;
            binding.id = atoi(sec + 8);
            binding.enabled = GetPrivateProfileIntA(sec, "Enabled", 1, path.c_str()) != 0;
            char nicks[512] = {};
            GetPrivateProfileStringA(sec, "Nicks", "", nicks, sizeof(nicks), path.c_str());
            binding.nickListCsv = nicks;
            binding.nicknames = ParseNickCsv(binding.nickListCsv);

            const int slotCount = GetPrivateProfileIntA(sec, "SlotCount", 0, path.c_str());
            for (int i = 0; i < slotCount && i < kTextureRemapLimit; ++i) {
                char keyOriginal[32], keyRemap[32];
                _snprintf_s(keyOriginal, _TRUNCATE, "Slot%dOriginal", i);
                _snprintf_s(keyRemap, _TRUNCATE, "Slot%dRemap", i);

                char originalName[64] = {};
                char remapName[64] = {};
                GetPrivateProfileStringA(sec, keyOriginal, "", originalName, sizeof(originalName), path.c_str());
                GetPrivateProfileStringA(sec, keyRemap, "", remapName, sizeof(remapName), path.c_str());
                if (!originalName[0])
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

static bool ApplyTextureRemapNickBinding(CPed* ped, PedTextureRemapState& state) {
    if (!g_skinTextureRemapNickMode || !samp_bridge::IsSampBuildKnown()) {
        if (state.nickBindingApplied)
            SelectRandomTextureRemaps(state);
        return false;
    }

    char nick[32] = {};
    bool isLocal = false;
    if (!samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
        if (state.nickBindingApplied)
            SelectRandomTextureRemaps(state);
        return false;
    }

    const char* dff = OrcTryGetPedModelNameById(state.modelId);
    const std::string key = TextureRemapDffKey(dff, state.modelId);
    LoadTextureRemapNickBindingsForDff(dff, state.modelId);

    auto it = g_textureRemapNickBindingsByDff.find(key);
    if (it != g_textureRemapNickBindingsByDff.end()) {
        const std::string nickLower = TextureRemapToLowerAscii(nick);
        const std::vector<TextureRemapNickBinding>& bindings = it->second;
        for (auto rit = bindings.rbegin(); rit != bindings.rend(); ++rit) {
            if (!TextureRemapNickMatches(*rit, nickLower))
                continue;
            if (ApplyTextureRemapNickBindingToState(state, *rit)) {
                state.nickBindingApplied = true;
                state.nickBindingId = rit->id;
                return true;
            }
        }
    }

    if (state.nickBindingApplied)
        SelectRandomTextureRemaps(state);
    return false;
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
    slot.selected = -1;
    return idx;
}

struct TextureRemapScanCtx {
    PedTextureRemapState* state = nullptr;
    RwTexDictionary* dict = nullptr;
};

static RwTexture* TextureRemapCollectTextureCB(RwTexture* texture, void* data) {
    if (!texture || !data) return texture;
    TextureRemapScanCtx* ctx = reinterpret_cast<TextureRemapScanCtx*>(data);
    if (!ctx->state || !ctx->dict) return texture;

    const std::string name = texture->name;
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

static bool ScanTextureRemapsForModel(int modelId, PedTextureRemapState& out) {
    out = PedTextureRemapState{};
    out.modelId = modelId;

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
        return false;
    }

    out.scanned = true;
    if (out.totalRemapTextures > 0) {
        SelectRandomTextureRemaps(out);
        OrcLogInfo("texture remap scan: model=%d txd=%d slots=%d variants=%d",
                   modelId, out.txdIndex, out.slotCount, out.totalRemapTextures);
    }
    return true;
}

static int TextureRemapPedKey(CPed* ped) {
    if (!ped) return 0;
    __try {
        return CPools::GetPedRef(ped);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (int)(reinterpret_cast<uintptr_t>(ped) & 0x7fffffff);
    }
}

static PedTextureRemapState* EnsurePedTextureRemapState(CPed* ped, bool forceRescan = false) {
    if (!ped) return nullptr;
    const int key = TextureRemapPedKey(ped);
    if (!key) return nullptr;

    auto it = g_pedTextureRemaps.find(key);
    if (it != g_pedTextureRemaps.end() &&
        !forceRescan &&
        it->second.scanned &&
        it->second.modelId == (int)ped->m_nModelIndex) {
        return &it->second;
    }

    PedTextureRemapState fresh;
    if (!ScanTextureRemapsForModel((int)ped->m_nModelIndex, fresh)) {
        fresh.modelId = (int)ped->m_nModelIndex;
        fresh.scanned = true;
    }
    auto inserted = g_pedTextureRemaps.insert_or_assign(key, std::move(fresh));
    return &inserted.first->second;
}

static void FillTextureRemapPedInfo(const PedTextureRemapState& state, TextureRemapPedInfo& out) {
    out = TextureRemapPedInfo{};
    out.modelId = state.modelId;
    out.txdIndex = state.txdIndex;
    out.totalRemapTextures = state.totalRemapTextures;
    if (const char* dff = OrcTryGetPedModelNameById(state.modelId))
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
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state) {
        out = TextureRemapPedInfo{};
        return false;
    }
    FillTextureRemapPedInfo(*state, out);
    return true;
}

bool OrcSetLocalPedTextureRemap(int slot, int remap) {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state || slot < 0 || slot >= state->slotCount)
        return false;
    TextureRemapSlotState& s = state->slots[(size_t)slot];
    return SetRealRemapSelection(s, remap);
}

bool OrcRandomizeLocalPedTextureRemaps() {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, true);
    if (!state || state->totalRemapTextures <= 0)
        return false;
    SelectRandomTextureRemaps(*state);
    return true;
}

bool OrcSetAllLocalPedTextureRemaps(int remap) {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state)
        return false;
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
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
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
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state || state->slotCount <= 0)
        return false;

    const char* dff = OrcTryGetPedModelNameById(state->modelId);
    const std::string path = TextureRemapIniPathForDff(dff, state->modelId);
    TextureRemapEnsureDirectoryExists(g_gameTextureDir);

    const int nextId = GetPrivateProfileIntA("Main", "NextBindingId", 0, path.c_str());
    const int id = nextId;
    char nextBuf[32] = {};
    _snprintf_s(nextBuf, _TRUNCATE, "%d", nextId + 1);
    WritePrivateProfileStringA("Main", "NextBindingId", nextBuf, path.c_str());

    char section[32] = {};
    _snprintf_s(section, _TRUNCATE, "Binding.%d", id);
    WritePrivateProfileStringA(section, "Enabled", "1", path.c_str());
    WritePrivateProfileStringA(section, "Nicks", nickCsv ? nickCsv : "", path.c_str());

    char countBuf[32] = {};
    _snprintf_s(countBuf, _TRUNCATE, "%d", state->slotCount);
    WritePrivateProfileStringA(section, "SlotCount", countBuf, path.c_str());

    for (int i = 0; i < state->slotCount; ++i) {
        const TextureRemapSlotState& slot = state->slots[(size_t)i];
        char keyOriginal[32], keyRemap[32];
        _snprintf_s(keyOriginal, _TRUNCATE, "Slot%dOriginal", i);
        _snprintf_s(keyRemap, _TRUNCATE, "Slot%dRemap", i);
        WritePrivateProfileStringA(section, keyOriginal, slot.originalName.c_str(), path.c_str());

        const char* remapName = "";
        if (slot.selected >= 0 && slot.selected < (int)slot.remapNames.size())
            remapName = slot.remapNames[(size_t)slot.selected].c_str();
        WritePrivateProfileStringA(section, keyRemap, remapName, path.c_str());
    }

    const std::string key = TextureRemapDffKey(dff, state->modelId);
    g_textureRemapNickBindingsByDff.erase(key);
    return true;
}

bool OrcDeleteLocalPedTextureRemapNickBinding(int bindingId) {
    CPlayerPed* ped = FindPlayerPed(0);
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state || bindingId < 0)
        return false;

    const char* dff = OrcTryGetPedModelNameById(state->modelId);
    const std::string path = TextureRemapIniPathForDff(dff, state->modelId);
    char section[32] = {};
    _snprintf_s(section, _TRUNCATE, "Binding.%d", bindingId);
    if (!WritePrivateProfileStringA(section, nullptr, nullptr, path.c_str()))
        return false;

    const std::string key = TextureRemapDffKey(dff, state->modelId);
    g_textureRemapNickBindingsByDff.erase(key);
    return true;
}

static RpMaterial* TextureRemapApplyMaterialCB(RpMaterial* material, void* data) {
    if (!material || !data || !material->texture) return material;
    PedTextureRemapState* state = reinterpret_cast<PedTextureRemapState*>(data);
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& slot = state->slots[(size_t)i];
        if (slot.selected < 0 || slot.selected >= (int)slot.remaps.size())
            continue;
        if (material->texture != slot.original)
            continue;
        g_textureRemapRestoreEntries.push_back({ material, material->texture });
        material->texture = slot.remaps[(size_t)slot.selected];
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
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state || state->slotCount <= 0 || state->totalRemapTextures <= 0)
        return;
    ApplyTextureRemapNickBinding(ped, *state);
    __try {
        RpClumpForAllAtomics(ped->m_pRwClump, TextureRemapApplyAtomicCB, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("texture remap before: SEH ex=0x%08X ped=%p", GetExceptionCode(), ped);
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
}

void OrcTextureRemapOnProcessScripts() {
    if (CCutsceneMgr::ms_running)
        g_textureRemapCutsceneLastTime = CTimer::m_snTimeInMilliseconds;
}

void OrcTextureRemapOnPedSetModel(CPed* ped, int) {
    if (!ped)
        return;
    const int key = TextureRemapPedKey(ped);
    if (key)
        g_pedTextureRemaps.erase(key);
    if ((CTimer::m_snTimeInMilliseconds - g_textureRemapCutsceneLastTime) > 3000 && g_skinTextureRemapEnabled)
        EnsurePedTextureRemapState(ped, true);
}

extern "C" int32_t __declspec(dllexport) Ext_GetPedRemap(CPed* ped, int index) {
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state || index < 0 || index >= state->slotCount)
        return -1;
    return state->slots[(size_t)index].selected;
}

extern "C" void __declspec(dllexport) Ext_SetPedRemap(CPed* ped, int index, int num) {
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state || index < 0 || index >= state->slotCount)
        return;
    TextureRemapSlotState& s = state->slots[(size_t)index];
    SetRealRemapSelection(s, num);
}

extern "C" void __declspec(dllexport) Ext_SetAllPedRemaps(CPed* ped, int num) {
    PedTextureRemapState* state = EnsurePedTextureRemapState(ped, false);
    if (!state)
        return;
    for (int i = 0; i < state->slotCount; ++i) {
        TextureRemapSlotState& s = state->slots[(size_t)i];
        SetRealRemapSelection(s, num);
    }
}

