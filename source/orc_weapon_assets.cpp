#include "plugin.h"

#include "CPed.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CClumpModelInfo.h"
#include "CVisibilityPlugins.h"
#include "CModelInfo.h"
#include "CTxdStore.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "samp_bridge.h"

#include "orc_app.h"
#include "orc_weapon_audio.h"
#include "orc_attach.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_random_pick.h"
#include "orc_render.h"
#include "orc_types.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_gunflash_state.h"
#include "orc_weapon_render_policy.h"
#include "orc_weapon_runtime.h"
#include "orc_weapons.h"

using namespace plugin;

static std::vector<WeaponReplacementAsset> g_weaponReplacementAssets;
static std::unordered_map<std::string, int> g_weaponReplacementByKey;
static std::unordered_map<std::string, int> g_weaponReplacementByNick;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomByWeapon;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBags;
static std::unordered_map<std::string, OrcSequentialPickState> g_weaponReplacementSequentialStates;
static std::unordered_map<std::string, OrcRandomNoRepeatPickState> g_weaponReplacementNoRepeatStates;
static std::unordered_map<std::string, int> g_weaponReplacementRandomChoiceByPed;
static WeaponReplacementStats g_weaponReplacementStats;

static constexpr int kWeaponReplacementVanillaChoice = -1;

static std::string MakeWeaponReplacementKey(const std::string& weaponLower, const std::string& matchLower) {
    return weaponLower + "|" + matchLower;
}

static int PedRefFromRandomChoiceKey(const std::string& choiceKey) {
    const size_t sep = choiceKey.find('|');
    if (sep == std::string::npos || sep == 0)
        return 0;
    return std::atoi(choiceKey.substr(0, sep).c_str());
}

static void EraseSequentialChoiceKeyFromStates(
    std::unordered_map<std::string, OrcSequentialPickState>& states,
    const std::string& choiceKey) {
    for (auto& kv : states)
        kv.second.activeChoiceKeys.erase(choiceKey);
}

static void ReleaseNoRepeatChoiceKeyFromStates(
    std::unordered_map<std::string, OrcRandomNoRepeatPickState>& states,
    const std::unordered_map<std::string, int>& choices,
    const std::string& choiceKey,
    bool recycle) {
    for (auto& kv : states)
        OrcRandomNoRepeatReleaseChoice(kv.second, choices, choiceKey, recycle);
}

static void PruneRandomChoicesForDeadPeds(
    std::unordered_map<std::string, int>& choices,
    std::unordered_map<std::string, OrcSequentialPickState>& sequentialStates,
    std::unordered_map<std::string, OrcRandomNoRepeatPickState>& noRepeatStates) {
    for (auto it = choices.begin(); it != choices.end();) {
        const int pedRef = PedRefFromRandomChoiceKey(it->first);
        if (pedRef > 0 && !OrcSafeGetPed(pedRef)) {
            EraseSequentialChoiceKeyFromStates(sequentialStates, it->first);
            ReleaseNoRepeatChoiceKeyFromStates(noRepeatStates, choices, it->first, true);
            it = choices.erase(it);
        } else {
            ++it;
        }
    }
}

static void AppendUniqueLower(std::vector<std::string>& out, const std::string& value) {
    if (value.empty())
        return;
    const std::string lower = OrcToLowerAscii(value);
    if (std::find(out.begin(), out.end(), lower) == out.end())
        out.push_back(lower);
}

static std::string_view WeaponModelBaseName(int wt) {
    if (wt > 0 && wt < (int)g_weaponDatIdeName.size() && !g_weaponDatIdeName[(size_t)wt].empty())
        return g_weaponDatIdeName[(size_t)wt];
    if (wt > 0 && wt < (int)g_cfg.size() && g_cfg[(size_t)wt].name && g_cfg[(size_t)wt].name[0])
        return g_cfg[(size_t)wt].name;
    return {};
}

static const std::vector<std::string>& WeaponLookupNamesLower(int wt) {
    struct CacheEntry {
        std::string rawBase;
        std::vector<std::string> names;
        bool initialized = false;
    };
    static std::array<CacheEntry, 256> cache;
    static const std::vector<std::string> empty;
    if (wt <= 0 || (size_t)wt >= cache.size())
        return empty;
    const std::string_view base = WeaponModelBaseName(wt);
    CacheEntry& entry = cache[(size_t)wt];
    if (entry.initialized && entry.rawBase == base)
        return entry.names;
    entry.initialized = true;
    entry.rawBase = base;
    auto& names = entry.names;
    names.clear();
    AppendUniqueLower(names, entry.rawBase);
    switch (wt) {
    case WEAPONTYPE_SHOTGUN:
        AppendUniqueLower(names, "chromegun");
        AppendUniqueLower(names, "shotgun");
        break;
    case WEAPONTYPE_MP5:
        AppendUniqueLower(names, "mp5lng");
        AppendUniqueLower(names, "mp5");
        break;
    default:
        break;
    }
    return names;
}

struct WeaponReplacementStickyChoiceSnapshot {
    bool vanilla = false;
    std::string assetKeyLower;
};

static std::unordered_map<std::string, WeaponReplacementStickyChoiceSnapshot> g_weaponReplacementRerollAvoidChoiceByPed;

static int FindWeaponReplacementAssetIndexByKeyLower(const std::string& keyLower) {
    const auto found = g_weaponReplacementByKey.find(keyLower);
    return found == g_weaponReplacementByKey.end() ? -1 : found->second;
}

static WeaponReplacementAsset* FindWeaponReplacementAssetByKey(const std::string& keyLower) {
    const int index = FindWeaponReplacementAssetIndexByKeyLower(keyLower);
    return index >= 0 ? &g_weaponReplacementAssets[(size_t)index] : nullptr;
}

static std::unordered_map<std::string, WeaponReplacementStickyChoiceSnapshot> SnapshotWeaponReplacementStickyChoices() {
    std::unordered_map<std::string, WeaponReplacementStickyChoiceSnapshot> snapshot;
    snapshot.reserve(g_weaponReplacementRandomChoiceByPed.size());
    for (const auto& kv : g_weaponReplacementRandomChoiceByPed) {
        WeaponReplacementStickyChoiceSnapshot choice{};
        if (kv.second == kWeaponReplacementVanillaChoice) {
            choice.vanilla = true;
            snapshot.emplace(kv.first, std::move(choice));
            continue;
        }
        if (kv.second < 0 || kv.second >= (int)g_weaponReplacementAssets.size())
            continue;
        choice.assetKeyLower = g_weaponReplacementAssets[(size_t)kv.second].key;
        if (!choice.assetKeyLower.empty())
            snapshot.emplace(kv.first, std::move(choice));
    }
    return snapshot;
}

static void RestoreWeaponReplacementStickyChoices(
    const std::unordered_map<std::string, WeaponReplacementStickyChoiceSnapshot>& snapshot) {
    if (snapshot.empty())
        return;
    for (const auto& kv : snapshot) {
        if (kv.second.vanilla) {
            if (g_weaponReplacementRandomIncludeVanilla)
                g_weaponReplacementRandomChoiceByPed[kv.first] = kWeaponReplacementVanillaChoice;
            continue;
        }
        const int assetIndex = FindWeaponReplacementAssetIndexByKeyLower(kv.second.assetKeyLower);
        if (assetIndex >= 0)
            g_weaponReplacementRandomChoiceByPed[kv.first] = assetIndex;
    }
}

static std::string_view WeaponReplacementChoiceSortKey(int choice) {
    if (choice == kWeaponReplacementVanillaChoice)
        return {};
    if (choice < 0 || choice >= (int)g_weaponReplacementAssets.size())
        return "~";
    return g_weaponReplacementAssets[(size_t)choice].key;
}

static void SortWeaponReplacementRandomPools() {
    auto sortPool = [](std::vector<int>& pool) {
        std::sort(pool.begin(), pool.end(), [](int a, int b) {
            return WeaponReplacementChoiceSortKey(a) < WeaponReplacementChoiceSortKey(b);
        });
        pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
    };
    for (auto& kv : g_weaponReplacementRandomBySkin)
        sortPool(kv.second);
    for (auto& kv : g_weaponReplacementRandomByWeapon)
        sortPool(kv.second);
}

static std::string StripSampColorCodes(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        if (value[i] == '{' && i + 8 <= value.size()) {
            size_t j = i + 1;
            int hex = 0;
            while (j < value.size() && hex < 8) {
                const char c = value[j];
                const bool isHex =
                    (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
                if (!isHex) break;
                ++j;
                ++hex;
            }
            if ((hex == 6 || hex == 8) && j < value.size() && value[j] == '}') {
                i = j + 1;
                continue;
            }
        }
        out.push_back(value[i++]);
    }
    return out;
}

std::string OrcGetWeaponModelBaseNameLower(int wt) {
    return OrcToLowerAscii(std::string(WeaponModelBaseName(wt)));
}

static std::vector<std::string> KnownWeaponModelNamesLower() {
    std::vector<std::string> names;
    for (int wt : g_availableWeaponTypes) {
        for (const std::string& name : WeaponLookupNamesLower(wt))
            AppendUniqueLower(names, name);
        if (wt > 0 && wt < (int)g_cfg.size() && g_cfg[(size_t)wt].name && g_cfg[(size_t)wt].name[0])
            AppendUniqueLower(names, g_cfg[(size_t)wt].name);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() > b.size();
        return a < b;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}



static void ClearWeaponHudIconCache();
static void OrcInitializeReplacementGunflashHidden(RpClump* clump);

static RwTexture* DetachReferencedWeaponTexture(RwTexture* texture, void*) {
    // Match CTxdStore::RemoveTxd: external material owners keep a live texture, but no dictionary link.
    if (texture->refCount > 1) {
        RwTextureDestroy(texture);
        RwTexDictionaryRemoveTexture(texture);
    }
    return texture;
}

static void DestroyWeaponTxd(RwTexDictionary*& dict) {
    if (!dict)
        return;
    RwTexDictionaryForAllTextures(dict, DetachReferencedWeaponTexture, nullptr);
    RwTexDictionaryDestroy(dict);
    dict = nullptr;
}

static RwTexDictionary* ReadWeaponTxd(const char* path) {
    // CTxdStore::LoadTxd uses this GTA reader after the dictionary chunk header. It accepts the same
    // native TXD formats without allocating a streaming slot or overwriting a global dictionary.
    using ReadGtaTxd = RwTexDictionary*(__cdecl*)(RwStream*);
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)path);
    if (!stream)
        return nullptr;
    RwTexDictionary* dict = nullptr;
    __try {
        __try {
            if (RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nullptr, nullptr))
                dict = reinterpret_cast<ReadGtaTxd>(0x730FC0)(stream);
        } __finally {
            RwStreamClose(stream, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon TXD: read SEH ex=0x%08X path=%s", GetExceptionCode(), path);
    }
    return dict;
}

static void DestroyWeaponReplacementAssets() {
    ClearWeaponHudIconCache();
    for (auto& asset : g_weaponReplacementAssets) {
        OrcDestroyRwObjectInstance(asset.rwObject);
        DestroyWeaponTxd(asset.txd);
    }
    g_weaponReplacementAssets.clear();
    g_weaponReplacementByKey.clear();
    g_weaponReplacementByNick.clear();
    g_weaponReplacementRandomBySkin.clear();
    g_weaponReplacementRandomByWeapon.clear();
    g_weaponReplacementRandomBags.clear();
    g_weaponReplacementSequentialStates.clear();
    g_weaponReplacementNoRepeatStates.clear();
    g_weaponReplacementRandomChoiceByPed.clear();
    g_weaponReplacementStats = {};
}


static RwObject* ReadWeaponDffChunk(const char* path, RwUInt32 chunk) {
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)path);
    if (!stream)
        return nullptr;
    RwObject* object = nullptr;
    __try {
        if (RwStreamFindChunk(stream, chunk, nullptr, nullptr)) {
            object = chunk == rwID_CLUMP
                ? reinterpret_cast<RwObject*>(RpClumpStreamRead(stream))
                : reinterpret_cast<RwObject*>(RpAtomicStreamRead(stream));
        }
    } __finally {
        RwStreamClose(stream, nullptr);
    }
    return object;
}

static RpAtomic* InitWeaponReplacementAtomic(RpAtomic* atomic, void* data) {
    // The geometry already owns its materials and RpAtomicClone retains the geometry.
    CVisibilityPlugins::SetAtomicRenderCallback(atomic, nullptr);
    if ((RpAtomicGetFlags(atomic) & rpATOMICRENDER) && atomic->geometry && atomic->geometry->numTriangles > 0)
        *static_cast<bool*>(data) = true;
    return atomic;
}

static bool PrepareWeaponReplacementObject(RwObject* object) {
    bool hasMesh = false;
    if (object->type == rpCLUMP) {
        auto* clump = reinterpret_cast<RpClump*>(object);
        if (!RpClumpGetFrame(clump))
            return false;
        OrcInitializeReplacementGunflashHidden(clump);
        RpClumpForAllAtomics(clump, InitWeaponReplacementAtomic, &hasMesh);
    } else {
        auto* atomic = reinterpret_cast<RpAtomic*>(object);
        if (!RpAtomicGetFrame(atomic)) {
            RwFrame* frame = RwFrameCreate();
            if (!frame)
                return false;
            RpAtomicSetFrame(atomic, frame);
        }
        InitWeaponReplacementAtomic(atomic, &hasMesh);
    }
    return hasMesh;
}

static bool ReadWeaponReplacementObject(WeaponReplacementAsset& asset) {
    RwTexDictionary* previous = RwTexDictionaryGetCurrent();
    __try {
        __try {
            RwTexDictionarySetCurrent(asset.txd);
            asset.rwObject = ReadWeaponDffChunk(asset.dffPath.c_str(), rwID_CLUMP);
            if (!asset.rwObject)
                asset.rwObject = ReadWeaponDffChunk(asset.dffPath.c_str(), rwID_ATOMIC);
            return asset.rwObject && PrepareWeaponReplacementObject(asset.rwObject);
        } __finally {
            RwTexDictionarySetCurrent(previous);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon DFF: read SEH ex=0x%08X path=%s", GetExceptionCode(), asset.dffPath.c_str());
        return false;
    }
}

static bool EnsureWeaponReplacementAssetLoaded(WeaponReplacementAsset& asset) {
    if (asset.rwObject)
        return true;
    if (asset.loadAttempted)
        return false;
    asset.loadAttempted = true;

    if (!OrcFileExistsA(asset.dffPath.c_str())) {
        OrcLogError("weapon replacement \"%s\": DFF missing %s", asset.displayName.c_str(), asset.dffPath.c_str());
        return false;
    }
    asset.txd = asset.txdPath.empty() ? nullptr : ReadWeaponTxd(asset.txdPath.c_str());
    if (!asset.txd) {
        OrcLogError("weapon replacement \"%s\": TXD unreadable path=%s", asset.displayName.c_str(), asset.txdPath.c_str());
        return false;
    }
    if (!ReadWeaponReplacementObject(asset)) {
        OrcLogError("weapon replacement \"%s\": DFF has no usable mesh", asset.displayName.c_str());
        OrcDestroyRwObjectInstance(asset.rwObject);
        DestroyWeaponTxd(asset.txd);
        return false;
    }

    OrcLogInfo("weapon replacement \"%s\": loaded", asset.displayName.c_str());
    return true;
}

static bool OrcWeaponFrameIsDescendantOf(RwFrame* frame, RwFrame* ancestor) {
    constexpr int kMaxAncestors = 64;
    for (int i = 0; frame && i < kMaxAncestors; ++i, frame = OrcGetRwFrameParentSafe(frame)) {
        if (frame == ancestor)
            return true;
    }
    return false;
}

struct OrcInitialGunflashVisibilityCtx {
    RwFrame* gunflashRoot = nullptr;
};

static RpAtomic* OrcInitializeGunflashAtomicHiddenCb(RpAtomic* atomic, void* data) {
    auto* ctx = reinterpret_cast<OrcInitialGunflashVisibilityCtx*>(data);
    if (!atomic || !ctx || !ctx->gunflashRoot)
        return atomic;
    RwFrame* frame = RpAtomicGetFrame(atomic);
    if (!frame || !OrcWeaponFrameIsDescendantOf(frame, ctx->gunflashRoot))
        return atomic;
    const RwUInt32 flags = RpAtomicGetFlags(atomic);
    const RwUInt32 hiddenFlags =
        static_cast<RwUInt32>(OrcInitialGunflashAtomicFlags(static_cast<std::uint32_t>(flags)));
    if (hiddenFlags != flags)
        RpAtomicSetFlags(atomic, hiddenFlags);
    return atomic;
}

static void OrcInitializeReplacementGunflashHidden(RpClump* clump) {
    if (!clump)
        return;
    OrcInitialGunflashVisibilityCtx ctx{};
    ctx.gunflashRoot = CClumpModelInfo::GetFrameFromName(clump, "gunflash");
    if (ctx.gunflashRoot)
        RpClumpForAllAtomics(clump, OrcInitializeGunflashAtomicHiddenCb, &ctx);
}

RwObject* OrcCloneWeaponReplacementObject(WeaponReplacementAsset& asset) {
    if (!EnsureWeaponReplacementAssetLoaded(asset) || !asset.rwObject)
        return nullptr;
    if (asset.rwObject->type == rpCLUMP) {
        // RpAtomicClone preserves the template's callback and initial gunflash flags.
        return reinterpret_cast<RwObject*>(RpClumpClone(reinterpret_cast<RpClump*>(asset.rwObject)));
    }
    if (asset.rwObject->type == rpATOMIC) {
        RpAtomic* clone = RpAtomicClone(reinterpret_cast<RpAtomic*>(asset.rwObject));
        if (!clone) return nullptr;
        RwFrame* frame = RwFrameCreate();
        if (!frame) {
            RpAtomicDestroy(clone);
            return nullptr;
        }
        RpAtomicSetFrame(clone, frame);
        return reinterpret_cast<RwObject*>(clone);
    }
    return nullptr;
}

static void AddWeaponReplacementAsset(const std::string& key,
                                      const std::string& weaponLower,
                                      const std::string& matchLower,
                                      const std::string& displayName,
                                      const std::string& dffPath,
                                      const std::string& txdPath,
                                      std::unordered_map<std::string, int>* directMap,
                                      std::unordered_map<std::string, std::vector<int>>* randomBySkinMap,
                                      std::unordered_map<std::string, std::vector<int>>* randomByWeaponMap) {
    WeaponReplacementAsset asset;
    asset.key = key;
    asset.weaponNameLower = weaponLower;
    asset.matchNameLower = matchLower;
    asset.displayName = displayName;
    asset.dffPath = dffPath;
    asset.txdPath = txdPath;
    const int index = (int)g_weaponReplacementAssets.size();
    g_weaponReplacementAssets.push_back(std::move(asset));
    g_weaponReplacementByKey.emplace(key, index);

    const std::string mapKey = MakeWeaponReplacementKey(weaponLower, matchLower);
    if (directMap) {
        if (directMap->find(mapKey) == directMap->end())
            (*directMap)[mapKey] = index;
    }
    if (randomBySkinMap)
        (*randomBySkinMap)[mapKey].push_back(index);
    if (randomByWeaponMap)
        (*randomByWeaponMap)[weaponLower].push_back(index);
}

void DiscoverWeaponReplacements(bool rerollStickyChoices) {
    const auto stickyChoices = SnapshotWeaponReplacementStickyChoices();
    OrcInvalidateHeldWeaponDrawPreparation();
    OrcClearAllWeaponReplacementInstances();
    DestroyWeaponReplacementAssets();

    const std::string gunsDir = g_gameWeaponGunsDir;
    DWORD gunsAttr = GetFileAttributesA(gunsDir.c_str());
    if (gunsAttr != INVALID_FILE_ATTRIBUTES && (gunsAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string weaponMask = OrcJoinPath(gunsDir, "*");
        WIN32_FIND_DATAA weaponData{};
        HANDLE hw = FindFirstFileA(weaponMask.c_str(), &weaponData);
        if (hw != INVALID_HANDLE_VALUE) {
            do {
                if (!(weaponData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::string weaponFolder = weaponData.cFileName;
                if (weaponFolder == "." || weaponFolder == "..") continue;
                const std::string weaponLower = OrcToLowerAscii(weaponFolder);
                const std::string weaponDir = OrcJoinPath(gunsDir, weaponFolder);

                std::string fileMask = OrcJoinPath(weaponDir, "*.*");
                WIN32_FIND_DATAA fileData{};
                HANDLE hf = FindFirstFileA(fileMask.c_str(), &fileData);
                if (hf != INVALID_HANDLE_VALUE) {
                    do {
                        if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        const std::string fname = fileData.cFileName;
                        if (OrcLowerExt(fname) != ".dff") continue;
                        const std::string base = OrcBaseNameNoExt(fname);
                        const std::string baseLower = OrcToLowerAscii(base);
                        AddWeaponReplacementAsset(
                            "wprand:" + weaponLower + ":" + baseLower,
                            weaponLower,
                            baseLower,
                            weaponFolder + "/" + base,
                            OrcJoinPath(weaponDir, fname),
                            OrcFindBestTxdPath(weaponDir, base),
                            nullptr,
                            nullptr,
                            &g_weaponReplacementRandomByWeapon);
                    } while (FindNextFileA(hf, &fileData));
                    FindClose(hf);
                }

                std::string skinDirMask = OrcJoinPath(weaponDir, "*");
                WIN32_FIND_DATAA skinDirData{};
                HANDLE hs = FindFirstFileA(skinDirMask.c_str(), &skinDirData);
                if (hs != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(skinDirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        const std::string skinFolder = skinDirData.cFileName;
                        if (skinFolder == "." || skinFolder == "..") continue;
                        const std::string skinLower = OrcToLowerAscii(skinFolder);
                        const std::string skinDir = OrcJoinPath(weaponDir, skinFolder);
                        std::string variantMask = OrcJoinPath(skinDir, "*.*");
                        WIN32_FIND_DATAA variantData{};
                        HANDLE hv = FindFirstFileA(variantMask.c_str(), &variantData);
                        if (hv == INVALID_HANDLE_VALUE) continue;
                        do {
                            if (variantData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                            const std::string fname = variantData.cFileName;
                            if (OrcLowerExt(fname) != ".dff") continue;
                            const std::string base = OrcBaseNameNoExt(fname);
                            AddWeaponReplacementAsset(
                                "skinrandom:" + weaponLower + ":" + skinLower + ":" + OrcToLowerAscii(base),
                                weaponLower,
                                skinLower,
                                weaponFolder + "/" + skinFolder + "/" + base,
                                OrcJoinPath(skinDir, fname),
                                OrcFindBestTxdPath(skinDir, base),
                                nullptr,
                                &g_weaponReplacementRandomBySkin,
                                nullptr);
                        } while (FindNextFileA(hv, &variantData));
                        FindClose(hv);
                    } while (FindNextFileA(hs, &skinDirData));
                    FindClose(hs);
                }
            } while (FindNextFileA(hw, &weaponData));
            FindClose(hw);
        }
    }

    const std::vector<std::string> knownWeapons = KnownWeaponModelNamesLower();
    const std::string nickDir = g_gameWeaponGunsNickDir;
    DWORD nickAttr = GetFileAttributesA(nickDir.c_str());
    if (nickAttr != INVALID_FILE_ATTRIBUTES && (nickAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string nickMask = OrcJoinPath(nickDir, "*.*");
        WIN32_FIND_DATAA nickData{};
        HANDLE hn = FindFirstFileA(nickMask.c_str(), &nickData);
        if (hn != INVALID_HANDLE_VALUE) {
            do {
                if (nickData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string fname = nickData.cFileName;
                if (OrcLowerExt(fname) != ".dff") continue;
                const std::string base = OrcBaseNameNoExt(fname);
                const std::string baseLower = OrcToLowerAscii(base);
                std::string weaponLower;
                std::string nickLower;
                for (const std::string& known : knownWeapons) {
                    const std::string prefix = known + "_";
                    if (baseLower.rfind(prefix, 0) == 0 && baseLower.size() > prefix.size()) {
                        weaponLower = known;
                        nickLower = baseLower.substr(prefix.size());
                        break;
                    }
                }
                if (weaponLower.empty()) {
                    const size_t underscore = baseLower.find('_');
                    if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= baseLower.size())
                        continue;
                    weaponLower = baseLower.substr(0, underscore);
                    nickLower = baseLower.substr(underscore + 1);
                }
                AddWeaponReplacementAsset(
                    "nick:" + weaponLower + ":" + nickLower,
                    weaponLower,
                    nickLower,
                    base,
                    OrcJoinPath(nickDir, fname),
                    OrcFindBestTxdPath(nickDir, base),
                    &g_weaponReplacementByNick,
                    nullptr,
                    nullptr);
            } while (FindNextFileA(hn, &nickData));
            FindClose(hn);
        }
    }

    SortWeaponReplacementRandomPools();

    g_weaponReplacementStats.randomWeaponWeapons = 0;
    for (const auto& kv : g_weaponReplacementRandomByWeapon)
        g_weaponReplacementStats.randomWeaponWeapons += (int)kv.second.size();
    g_weaponReplacementStats.randomSkinWeapons = 0;
    for (const auto& kv : g_weaponReplacementRandomBySkin)
        g_weaponReplacementStats.randomSkinWeapons += (int)kv.second.size();
    g_weaponReplacementStats.nickWeapons = (int)g_weaponReplacementByNick.size();
    if (rerollStickyChoices)
        g_weaponReplacementRerollAvoidChoiceByPed = stickyChoices;
    else {
        g_weaponReplacementRerollAvoidChoiceByPed.clear();
        RestoreWeaponReplacementStickyChoices(stickyChoices);
    }
    OrcLogInfo("DiscoverWeaponReplacements: weaponRandom=%d skinRandom=%d nick=%d reroll=%d",
        g_weaponReplacementStats.randomWeaponWeapons,
        g_weaponReplacementStats.randomSkinWeapons,
        g_weaponReplacementStats.nickWeapons,
        rerollStickyChoices ? 1 : 0);
    OrcWeaponAudioInvalidateCaches();
}

WeaponReplacementStats OrcGetWeaponReplacementStats() {
    return g_weaponReplacementStats;
}

struct WeaponTextureRestoreEntry {
    RpMaterial* material = nullptr;
    RwTexture* texture = nullptr;
};

static std::vector<WeaponTextureAsset> g_weaponTextureAssets;
static std::unordered_map<std::string, int> g_weaponTextureByNick;
static std::unordered_map<std::string, int> g_weaponTextureBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponTextureRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponTextureRandomBags;
static std::unordered_map<std::string, OrcSequentialPickState> g_weaponTextureSequentialStates;
static std::unordered_map<std::string, OrcRandomNoRepeatPickState> g_weaponTextureNoRepeatStates;
static std::unordered_map<std::string, int> g_weaponTextureRandomChoiceByPed;
static std::vector<WeaponTextureRestoreEntry> g_weaponTextureRestoreEntries;
static WeaponTextureStats g_weaponTextureStats;

static void WeaponTextureRecordMaterialRestore(RpMaterial* material, RwTexture* oldTex) {
    if (material)
        g_weaponTextureRestoreEntries.push_back({ material, oldTex });
}

static RwTexDictionary* GetTxdDictionaryByIndexMain(int txdIndex) {
    auto* pool = CTxdStore::ms_pTxdPool;
    if (!pool || !pool->m_pObjects || !pool->m_byteMap || txdIndex < 0 || txdIndex >= pool->m_nSize)
        return nullptr;
    const TxdDef* slot = pool->GetAt(txdIndex);
    return slot ? slot->m_pRwDictionary : nullptr;
}

static constexpr size_t kWeaponStockRemapMaxSlots = 8;

struct WeaponStockRemapSlot {
    std::string baseTexName;
    std::vector<RwTexture*> remaps;
};

struct WeaponStockRemapCatalog {
    int txdIndex = -1;
    RwTexDictionary* dictionary = nullptr;
    std::vector<WeaponStockRemapSlot> slots;
    std::unordered_map<std::uint64_t, std::vector<int>> selections;
    DWORD lastPrune = 0;
};

static std::unordered_map<int, WeaponStockRemapCatalog> g_weaponStockRemapCatalogByWt;

// Remap variants (*_suffix_remap…) inside Orc loaded Guns/GunsNick TXDs (distinct from game's model TXD).
static std::unordered_map<std::string, WeaponStockRemapCatalog> g_weaponTextureAssetRemapCatalogByKey;

static void ReleaseStockRemapCatalog(const WeaponStockRemapCatalog& catalog) {
    if (catalog.slots.empty())
        return;
    __try {
        if (GetTxdDictionaryByIndexMain(catalog.txdIndex) == catalog.dictionary &&
            CTxdStore::GetNumRefs(catalog.txdIndex) != 0) {
            CTxdStore::RemoveRefWithoutDelete(catalog.txdIndex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon stock remap: TXD release SEH ex=0x%08X slot=%d", GetExceptionCode(), catalog.txdIndex);
    }
}

static void ClearWeaponStockRemapRuntime() {
    for (const auto& entry : g_weaponStockRemapCatalogByWt)
        ReleaseStockRemapCatalog(entry.second);
    g_weaponStockRemapCatalogByWt.clear();
    g_weaponTextureAssetRemapCatalogByKey.clear();
}

static int WeaponStockRemapFindSlotIndex(WeaponStockRemapCatalog& cat, const std::string& originalBaseName) {
    for (int i = 0; i < (int)cat.slots.size(); ++i) {
        if (_stricmp(cat.slots[(size_t)i].baseTexName.c_str(), originalBaseName.c_str()) == 0)
            return i;
    }
    return -1;
}

static bool WeaponStockRemapHasRemap(const WeaponStockRemapSlot& slot, RwTexture* tex) {
    for (RwTexture* r : slot.remaps) {
        if (r == tex)
            return true;
    }
    return false;
}

static int WeaponStockRemapGetOrAddSlot(WeaponStockRemapCatalog& cat, const std::string& originalBaseNameFromRemapSuffix) {
    const int existing = WeaponStockRemapFindSlotIndex(cat, originalBaseNameFromRemapSuffix);
    if (existing >= 0)
        return existing;
    if (cat.slots.size() >= kWeaponStockRemapMaxSlots)
        return -1;
    WeaponStockRemapSlot slot;
    slot.baseTexName = originalBaseNameFromRemapSuffix;
    cat.slots.push_back(std::move(slot));
    return (int)cat.slots.size() - 1;
}

static RwTexture* WeaponStockRemapCollectTextureCB(RwTexture* texture, void* data) {
    if (!texture || !texture->raster || !texture->name[0] || !data)
        return texture;
    const size_t len = strnlen_s(texture->name, sizeof(texture->name));
    if (len == sizeof(texture->name))
        return texture;

    auto& catalog = *static_cast<WeaponStockRemapCatalog*>(data);
    const std::string name(texture->name, len);
    const std::string lower = OrcToLowerAscii(name);
    const size_t remapPos = lower.find("_remap");
    if (remapPos == std::string::npos || remapPos == 0)
        return texture;

    const std::string originalName = name.substr(0, remapPos);
    const int slotIdx = WeaponStockRemapGetOrAddSlot(catalog, originalName);
    if (slotIdx < 0)
        return texture;

    WeaponStockRemapSlot& slot = catalog.slots[(size_t)slotIdx];
    if (WeaponStockRemapHasRemap(slot, texture))
        return texture;
    slot.remaps.push_back(texture);
    return texture;
}

static void WeaponRemapPruneEmptyRemapSlots(WeaponStockRemapCatalog& out) {
    for (auto it = out.slots.begin(); it != out.slots.end();) {
        if (it->remaps.empty())
            it = out.slots.erase(it);
        else
            ++it;
    }
}

static void WeaponRemapScanDictIntoCatalog(WeaponStockRemapCatalog& out, const char* label) {
    __try {
        RwTexDictionaryForAllTextures(out.dictionary, WeaponStockRemapCollectTextureCB, &out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon remap scan: SEH ex=0x%08X asset=%s txd=%d", GetExceptionCode(), label, out.txdIndex);
        out.slots.clear();
    }
    WeaponRemapPruneEmptyRemapSlots(out);
}

static WeaponStockRemapCatalog* OrcGetWeaponStockRemapCatalog(int wt) {
    if (wt <= 0 || wt >= (int)g_weaponModelId.size() || g_weaponModelId[(size_t)wt] <= 0)
        return nullptr;
    const CBaseModelInfo* model = CModelInfo::GetModelInfo(g_weaponModelId[(size_t)wt]);
    if (!model)
        return nullptr;
    const int txdIndex = model->m_nTxdIndex;
    RwTexDictionary* const dictionary = GetTxdDictionaryByIndexMain(txdIndex);
    auto it = g_weaponStockRemapCatalogByWt.find(wt);
    if (it != g_weaponStockRemapCatalogByWt.end()) {
        if (it->second.txdIndex == txdIndex && it->second.dictionary == dictionary)
            return &it->second;
        ReleaseStockRemapCatalog(it->second);
        g_weaponStockRemapCatalogByWt.erase(it);
    }
    if (!dictionary)
        return nullptr;

    WeaponStockRemapCatalog cat;
    cat.txdIndex = txdIndex;
    cat.dictionary = dictionary;
    WeaponRemapScanDictIntoCatalog(cat, "stock");
    const auto ins = g_weaponStockRemapCatalogByWt.emplace(wt, std::move(cat));
    // The catalog borrows remap textures. Keep their streaming dictionary live until invalidation.
    if (!ins.first->second.slots.empty())
        CTxdStore::AddRef(txdIndex);
    return &ins.first->second;
}

static const std::vector<int>& WeaponRemapSelectionsForCatalog(WeaponStockRemapCatalog& cat, int pedRef, int wt) {
    const auto key = (static_cast<std::uint64_t>(pedRef) << 32) | static_cast<std::uint32_t>(wt);
    const auto found = cat.selections.find(key);
    if (found != cat.selections.end())
        return found->second;

    const DWORD now = GetTickCount();
    if (now - cat.lastPrune >= 5000u) {
        cat.lastPrune = now;
        for (auto it = cat.selections.begin(); it != cat.selections.end();) {
            if (!OrcSafeGetPed(static_cast<int>(it->first >> 32)))
                it = cat.selections.erase(it);
            else
                ++it;
        }
    }

    std::vector<int> sel;
    sel.reserve(cat.slots.size());
    for (const auto& slot : cat.slots)
        sel.push_back(rand() % (int)slot.remaps.size());
    return cat.selections.emplace(key, std::move(sel)).first->second;
}

struct WeaponStockRemapApplyCtx {
    const WeaponStockRemapCatalog* catalog = nullptr;
    const std::vector<int>* selections = nullptr;
};

static RpMaterial* WeaponStockRemapApplyMaterialCB(RpMaterial* material, void* data) {
    if (!material || !material->texture || !data)
        return material;
    WeaponStockRemapApplyCtx* ctx = reinterpret_cast<WeaponStockRemapApplyCtx*>(data);
    if (!ctx->catalog || !ctx->selections)
        return material;
    const char* mName = material->texture->name;
    if (!mName || !mName[0])
        return material;
    for (int i = 0; i < (int)ctx->catalog->slots.size(); ++i) {
        const WeaponStockRemapSlot& slot = ctx->catalog->slots[(size_t)i];
        if (_stricmp(mName, slot.baseTexName.c_str()) != 0)
            continue;
        const int varIdx = (i < (int)ctx->selections->size()) ? (*ctx->selections)[(size_t)i] : -1;
        if (varIdx < 0 || varIdx >= (int)slot.remaps.size())
            break;
        RwTexture* replacement = slot.remaps[(size_t)varIdx];
        if (!replacement || replacement == material->texture)
            break;
        WeaponTextureRecordMaterialRestore(material, material->texture);
        material->texture = replacement;
        break;
    }
    return material;
}

static RpAtomic* WeaponStockRemapApplyAtomicCB(RpAtomic* atomic, void* data) {
    if (!atomic || !atomic->geometry)
        return atomic;
    RpGeometryForAllMaterials(atomic->geometry, WeaponStockRemapApplyMaterialCB, data);
    return atomic;
}

static void OrcApplyWeaponRemapCatalogToRwObject(CPed* ped,
                                                 int wt,
                                                 RwObject* object,
                                                 WeaponStockRemapCatalog* cat) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped || !object || !cat || cat->slots.empty())
        return;

    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;

    WeaponStockRemapApplyCtx ctx;
    ctx.catalog = cat;
    ctx.selections = &WeaponRemapSelectionsForCatalog(*cat, pedRef, wt);
    __try {
        if (object->type == rpCLUMP) {
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(object), WeaponStockRemapApplyAtomicCB, &ctx);
        } else if (object->type == rpATOMIC) {
            WeaponStockRemapApplyAtomicCB(reinterpret_cast<RpAtomic*>(object), &ctx);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon remap apply: SEH ex=0x%08X", GetExceptionCode());
    }
}

static void OrcApplyWeaponStockRemapToObject(CPed* ped, int wt, RwObject* object) {
    if (!g_enabled || !g_weaponTexturesEnabled || !g_weaponTextureStandardRemap || wt <= 0)
        return;
    OrcApplyWeaponRemapCatalogToRwObject(ped, wt, object, OrcGetWeaponStockRemapCatalog(wt));
}

static bool EnsureWeaponTextureAssetLoaded(WeaponTextureAsset& asset);

/// Cached *_remap catalogue for an Orc Guns/GunsNick weapon texture TXD.
static WeaponStockRemapCatalog* OrcGetCachedWeaponTexAssetRemapCatalog(WeaponTextureAsset& asset) {
    if (!EnsureWeaponTextureAssetLoaded(asset))
        return nullptr;
    auto it = g_weaponTextureAssetRemapCatalogByKey.find(asset.key);
    if (it != g_weaponTextureAssetRemapCatalogByKey.end())
        return &it->second;

    WeaponStockRemapCatalog built{};
    built.dictionary = asset.txd;
    WeaponRemapScanDictIntoCatalog(built, asset.displayName.c_str());
    if (!built.slots.empty()) {
        OrcLogInfo("weapon texture \"%s\": Guns/GunsNick TXD has %zu remap slot(s)",
            asset.displayName.c_str(), built.slots.size());
    }
    const auto ins = g_weaponTextureAssetRemapCatalogByKey.emplace(asset.key, std::move(built));
    return &ins.first->second;
}

static void OrcApplyWeaponTextureAssetTxdRemapVariants(CPed* ped,
                                                       int wt,
                                                       RwObject* object,
                                                       WeaponTextureAsset* asset) {
    if (!g_enabled || !g_weaponTexturesEnabled || !asset || wt <= 0 || !ped || !object)
        return;
    OrcApplyWeaponRemapCatalogToRwObject(ped, wt, object, OrcGetCachedWeaponTexAssetRemapCatalog(*asset));
}

std::size_t OrcWeaponTextureRestorePoint() {
    return g_weaponTextureRestoreEntries.size();
}

void OrcRestoreWeaponTextureOverrides(std::size_t restorePoint) {
    while (g_weaponTextureRestoreEntries.size() > restorePoint) {
        const WeaponTextureRestoreEntry entry = g_weaponTextureRestoreEntries.back();
        g_weaponTextureRestoreEntries.pop_back();
        __try {
            entry.material->texture = entry.texture;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

static void DestroyWeaponTextureAssets() {
    ClearWeaponHudIconCache();
    OrcRestoreWeaponTextureOverrides();
    g_weaponTextureAssetRemapCatalogByKey.clear();
    for (auto& asset : g_weaponTextureAssets)
        DestroyWeaponTxd(asset.txd);
    g_weaponTextureAssets.clear();
    g_weaponTextureByNick.clear();
    g_weaponTextureBySkin.clear();
    g_weaponTextureRandomBySkin.clear();
    g_weaponTextureRandomBags.clear();
    g_weaponTextureSequentialStates.clear();
    g_weaponTextureNoRepeatStates.clear();
    g_weaponTextureRandomChoiceByPed.clear();
    g_weaponTextureStats = {};
}

static bool EnsureWeaponTextureAssetLoaded(WeaponTextureAsset& asset) {
    if (asset.loadAttempted)
        return asset.txd != nullptr;
    asset.loadAttempted = true;
    asset.txd = asset.txdPath.empty() ? nullptr : ReadWeaponTxd(asset.txdPath.c_str());
    if (!asset.txd) {
        OrcLogError("weapon texture \"%s\": TXD unreadable path=%s", asset.displayName.c_str(), asset.txdPath.c_str());
        return false;
    }
    OrcLogInfo("weapon texture \"%s\": loaded", asset.displayName.c_str());
    return true;
}

static void AddWeaponTextureAsset(const std::string& key,
                                  const std::string& weaponLower,
                                  const std::string& matchLower,
                                  const std::string& displayName,
                                  const std::string& txdPath,
                                  std::unordered_map<std::string, int>* directMap,
                                  std::unordered_map<std::string, std::vector<int>>* randomMap) {
    WeaponTextureAsset asset;
    asset.key = key;
    asset.weaponNameLower = weaponLower;
    asset.matchNameLower = matchLower;
    asset.displayName = displayName;
    asset.txdPath = txdPath;
    const int index = (int)g_weaponTextureAssets.size();
    g_weaponTextureAssets.push_back(std::move(asset));

    const std::string mapKey = MakeWeaponReplacementKey(weaponLower, matchLower);
    if (directMap && directMap->find(mapKey) == directMap->end())
        (*directMap)[mapKey] = index;
    if (randomMap)
        (*randomMap)[mapKey].push_back(index);
}

static std::string WeaponTextureChoiceSortKey(int choice) {
    if (choice < 0 || choice >= (int)g_weaponTextureAssets.size())
        return "~";
    return OrcToLowerAscii(g_weaponTextureAssets[(size_t)choice].key);
}

static void SortWeaponTextureRandomPools() {
    for (auto& kv : g_weaponTextureRandomBySkin) {
        std::vector<int>& pool = kv.second;
        std::sort(pool.begin(), pool.end(), [](int a, int b) {
            return WeaponTextureChoiceSortKey(a) < WeaponTextureChoiceSortKey(b);
        });
        pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
    }
}

void DiscoverWeaponTextures() {
    OrcInvalidateHeldWeaponDrawPreparation();
    ClearWeaponStockRemapRuntime();
    DestroyWeaponTextureAssets();
    const std::string gunsDir = g_gameWeaponGunsDir;
    const DWORD gunsAttr = GetFileAttributesA(gunsDir.c_str());
    const bool gunsOk = gunsAttr != INVALID_FILE_ATTRIBUTES && (gunsAttr & FILE_ATTRIBUTE_DIRECTORY);

    std::string weaponMask = OrcJoinPath(gunsDir, "*");
    WIN32_FIND_DATAA weaponData{};
    HANDLE hw = gunsOk ? FindFirstFileA(weaponMask.c_str(), &weaponData) : INVALID_HANDLE_VALUE;
    if (hw != INVALID_HANDLE_VALUE) {
        do {
            if (!(weaponData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::string weaponFolder = weaponData.cFileName;
            if (weaponFolder == "." || weaponFolder == "..") continue;
            const std::string weaponLower = OrcToLowerAscii(weaponFolder);
            const std::string weaponDir = OrcJoinPath(gunsDir, weaponFolder);

            std::string fileMask = OrcJoinPath(weaponDir, "*.*");
            WIN32_FIND_DATAA fileData{};
            HANDLE hf = FindFirstFileA(fileMask.c_str(), &fileData);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::string fname = fileData.cFileName;
                    if (OrcLowerExt(fname) != ".txd") continue;
                    const std::string base = OrcBaseNameNoExt(fname);
                    const std::string skinLower = OrcToLowerAscii(base);
                    AddWeaponTextureAsset(
                        "skin:" + weaponLower + ":" + skinLower,
                        weaponLower,
                        skinLower,
                        weaponFolder + "/" + base,
                        OrcJoinPath(weaponDir, fname),
                        &g_weaponTextureBySkin,
                        nullptr);
                } while (FindNextFileA(hf, &fileData));
                FindClose(hf);
            }

            std::string skinDirMask = OrcJoinPath(weaponDir, "*");
            WIN32_FIND_DATAA skinDirData{};
            HANDLE hs = FindFirstFileA(skinDirMask.c_str(), &skinDirData);
            if (hs != INVALID_HANDLE_VALUE) {
                do {
                    if (!(skinDirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    const std::string skinFolder = skinDirData.cFileName;
                    if (skinFolder == "." || skinFolder == "..") continue;
                    const std::string skinLower = OrcToLowerAscii(skinFolder);
                    const std::string skinDir = OrcJoinPath(weaponDir, skinFolder);
                    std::string variantMask = OrcJoinPath(skinDir, "*.*");
                    WIN32_FIND_DATAA variantData{};
                    HANDLE hv = FindFirstFileA(variantMask.c_str(), &variantData);
                    if (hv == INVALID_HANDLE_VALUE) continue;
                    do {
                        if (variantData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        const std::string fname = variantData.cFileName;
                        if (OrcLowerExt(fname) != ".txd") continue;
                        const std::string base = OrcBaseNameNoExt(fname);
                        AddWeaponTextureAsset(
                            "skinrandom:" + weaponLower + ":" + skinLower + ":" + OrcToLowerAscii(base),
                            weaponLower,
                            skinLower,
                            weaponFolder + "/" + skinFolder + "/" + base,
                            OrcJoinPath(skinDir, fname),
                            nullptr,
                            &g_weaponTextureRandomBySkin);
                    } while (FindNextFileA(hv, &variantData));
                    FindClose(hv);
                } while (FindNextFileA(hs, &skinDirData));
                FindClose(hs);
            }
        } while (FindNextFileA(hw, &weaponData));
        FindClose(hw);
    }

    const std::vector<std::string> knownWeapons = KnownWeaponModelNamesLower();
    const std::string nickDir = g_gameWeaponGunsNickDir;
    DWORD nickAttr = GetFileAttributesA(nickDir.c_str());
    if (nickAttr != INVALID_FILE_ATTRIBUTES && (nickAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string nickMask = OrcJoinPath(nickDir, "*.*");
        WIN32_FIND_DATAA nickData{};
        HANDLE hn = FindFirstFileA(nickMask.c_str(), &nickData);
        if (hn != INVALID_HANDLE_VALUE) {
            do {
                if (nickData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string fname = nickData.cFileName;
                if (OrcLowerExt(fname) != ".txd") continue;
                const std::string base = OrcBaseNameNoExt(fname);
                const std::string baseLower = OrcToLowerAscii(base);
                std::string weaponLower;
                std::string nickLower;
                for (const std::string& known : knownWeapons) {
                    const std::string prefix = known + "_";
                    if (baseLower.rfind(prefix, 0) == 0 && baseLower.size() > prefix.size()) {
                        weaponLower = known;
                        nickLower = baseLower.substr(prefix.size());
                        break;
                    }
                }
                if (weaponLower.empty()) {
                    const size_t underscore = baseLower.find('_');
                    if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= baseLower.size())
                        continue;
                    weaponLower = baseLower.substr(0, underscore);
                    nickLower = baseLower.substr(underscore + 1);
                }
                AddWeaponTextureAsset(
                    "nick:" + weaponLower + ":" + nickLower,
                    weaponLower,
                    nickLower,
                    base,
                    OrcJoinPath(nickDir, fname),
                    &g_weaponTextureByNick,
                    nullptr);
            } while (FindNextFileA(hn, &nickData));
            FindClose(hn);
        }
    }

    SortWeaponTextureRandomPools();

    g_weaponTextureStats.indexedTxdFiles = (int)g_weaponTextureAssets.size();
    g_weaponTextureStats.uniqueSkinTextures = (int)g_weaponTextureBySkin.size();
    g_weaponTextureStats.randomSkinTextures = 0;
    for (const auto& kv : g_weaponTextureRandomBySkin)
        g_weaponTextureStats.randomSkinTextures += (int)kv.second.size();
    g_weaponTextureStats.nickTextures = (int)g_weaponTextureByNick.size();
    OrcLogInfo("DiscoverWeaponTextures (Guns/GunsNick): txdIndexed=%d skin=%d random=%d nick=%d (*_remap per TXD at load)",
        g_weaponTextureStats.indexedTxdFiles,
        g_weaponTextureStats.uniqueSkinTextures,
        g_weaponTextureStats.randomSkinTextures,
        g_weaponTextureStats.nickTextures);
}

WeaponTextureStats OrcGetWeaponTextureStats() {
    return g_weaponTextureStats;
}

static int PopWeaponTextureRandomChoice(const std::string& mapKey, const std::vector<int>& pool) {
    std::vector<int>& bag = g_weaponTextureRandomBags[mapKey];
    if (bag.empty()) {
        bag = pool;
        OrcRandomNoRepeatShuffle(bag);
    }
    const int assetIndex = bag.back();
    bag.pop_back();
    return assetIndex;
}

static int PickStickyWeaponTextureChoice(CPed* ped, const std::string& mapKey, const std::vector<int>& pool) {
    const int pedRef = OrcSafeGetPedRef(ped);
    if (pedRef <= 0)
        return PopWeaponTextureRandomChoice(mapKey, pool);

    const std::string choiceKey = std::to_string(pedRef) + "|" + mapKey;
    auto chosen = g_weaponTextureRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponTextureRandomChoiceByPed.end()) {
        const int assetIndex = chosen->second;
        if (assetIndex >= 0 && assetIndex < (int)g_weaponTextureAssets.size()) {
            if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL)
                g_weaponTextureSequentialStates[mapKey].activeChoiceKeys.insert(choiceKey);
            else if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT)
                g_weaponTextureNoRepeatStates[mapKey].activeChoiceKeys.insert(choiceKey);
            return assetIndex;
        }
    }
    PruneRandomChoicesForDeadPeds(
        g_weaponTextureRandomChoiceByPed,
        g_weaponTextureSequentialStates,
        g_weaponTextureNoRepeatStates);
    if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL) {
        return OrcSequentialPickSticky(
            g_weaponTextureSequentialStates[mapKey],
            g_weaponTextureRandomChoiceByPed,
            choiceKey,
            pool,
            -1);
    }
    if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        return OrcRandomNoRepeatPickSticky(
            g_weaponTextureNoRepeatStates[mapKey],
            g_weaponTextureRandomChoiceByPed,
            choiceKey,
            pool,
            -1);
    }
    const int assetIndex = PopWeaponTextureRandomChoice(mapKey, pool);
    g_weaponTextureRandomChoiceByPed[choiceKey] = assetIndex;
    return assetIndex;
}

static void ForgetFailedWeaponChoice(int failedIndex,
    std::vector<int>& sourcePool,
    const std::string& poolKey,
    std::unordered_map<std::string, std::vector<int>>& bags,
    std::unordered_map<std::string, int>& choices,
    std::unordered_map<std::string, OrcSequentialPickState>& sequentialStates,
    std::unordered_map<std::string, OrcRandomNoRepeatPickState>& noRepeatStates);

static WeaponTextureAsset* PickUsableWeaponTexture(CPed* ped, const std::string& poolKey, std::vector<int>& pool) {
    while (!pool.empty()) {
        const int pick = PickStickyWeaponTextureChoice(ped, poolKey, pool);
        if (pick < 0 || pick >= (int)g_weaponTextureAssets.size())
            return nullptr;
        WeaponTextureAsset& asset = g_weaponTextureAssets[(size_t)pick];
        if (EnsureWeaponTextureAssetLoaded(asset))
            return &asset;
        ForgetFailedWeaponChoice(pick, pool, poolKey, g_weaponTextureRandomBags, g_weaponTextureRandomChoiceByPed,
            g_weaponTextureSequentialStates, g_weaponTextureNoRepeatStates);
    }
    return nullptr;
}

static WeaponTextureAsset* FindUsableWeaponTexture(const std::unordered_map<std::string, int>& index,
    const std::string& key) {
    const auto found = index.find(key);
    if (found == index.end())
        return nullptr;
    WeaponTextureAsset& asset = g_weaponTextureAssets[(size_t)found->second];
    return EnsureWeaponTextureAssetLoaded(asset) ? &asset : nullptr;
}

WeaponTextureAsset* OrcResolveUsableWeaponTextureAssetForPed(CPed* ped,
    int wt,
    bool allowRandom,
    const std::string* replacementKeyHint) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped || wt <= 0)
        return nullptr;
    const auto& weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return nullptr;

    if (g_weaponTextureNickMode && !g_weaponTextureByNick.empty() && samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = OrcToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                for (const std::string& weaponName : weaponNames) {
                    if (WeaponTextureAsset* asset = FindUsableWeaponTexture(g_weaponTextureByNick,
                            MakeWeaponReplacementKey(weaponName, nickLower)))
                        return asset;
                }
            }
        }
    }

    const std::string* wprandKeyPtr = replacementKeyHint;
    std::string resolvedReplKeyBuf;
    if (!replacementKeyHint && g_weaponReplacementEnabled) {
        resolvedReplKeyBuf = OrcResolveUsableWeaponReplacementKeyForPed(ped, wt, allowRandom);
        if (!resolvedReplKeyBuf.empty())
            wprandKeyPtr = &resolvedReplKeyBuf;
    }
    if (wprandKeyPtr && wprandKeyPtr->compare(0, 7, "wprand:") == 0) {
        std::string variant = wprandKeyPtr->substr(7);
        const size_t separator = variant.find(':');
        if (separator != std::string::npos &&
            std::find(weaponNames.begin(), weaponNames.end(), variant.substr(0, separator)) != weaponNames.end()) {
            // The key already contains the DFF stem, which can itself contain dots.
            variant[separator] = '|';
            if (WeaponTextureAsset* asset = FindUsableWeaponTexture(g_weaponTextureBySkin, variant))
                return asset;
        }
    }

    const std::string skinLowerRaw = OrcToLowerAscii(GetPedStdSkinDffName(ped));

    if (!skinLowerRaw.empty()) {
        for (const std::string& weaponName : weaponNames) {
            if (WeaponTextureAsset* asset = FindUsableWeaponTexture(g_weaponTextureBySkin,
                    MakeWeaponReplacementKey(weaponName, skinLowerRaw)))
                return asset;
        }
        if (allowRandom && g_weaponTextureRandomMode) {
            for (const std::string& weaponName : weaponNames) {
                const std::string skinKey = MakeWeaponReplacementKey(weaponName, skinLowerRaw);
                const auto found = g_weaponTextureRandomBySkin.find(skinKey);
                if (found != g_weaponTextureRandomBySkin.end()) {
                    if (WeaponTextureAsset* asset = PickUsableWeaponTexture(ped, skinKey, found->second))
                        return asset;
                }
            }
        }
    }

    // `Weapons\Guns\<weapon>\<weapon>.txd` is indexed as match key `<weapon>|<weapon>` (same string twice).
    // Used when ped skin has no `<weapon>\<dff>.txd` entry (many packs ship one bundle as desert_eagle\desert_eagle.txd).
    for (const std::string& weaponName : weaponNames) {
        if (WeaponTextureAsset* asset = FindUsableWeaponTexture(g_weaponTextureBySkin,
                MakeWeaponReplacementKey(weaponName, weaponName)))
            return asset;
    }
    return nullptr;
}

namespace {
struct WeaponHudIconCacheEntry {
    const void* asset = nullptr;
    const std::string* replacementKey = nullptr;
    RwTexDictionary* dict = nullptr;
    unsigned int stockHash = 0;
    int weaponType = 0;
    RwTexture* texture = nullptr;
};

WeaponHudIconCacheEntry g_textureHudIcon;
WeaponHudIconCacheEntry g_replacementHudIcon;

struct HudIconCandidates {
    std::string vanillaIcon;
    std::string folderIcon;
    std::string matchIcon;
    std::string variantIcon;
    std::string_view replacementBasename;
    std::string_view folder;
    std::string_view match;
    RwTexture* best = nullptr;
    int bestScore = -1;
    size_t bestNameLength = 0;
    RwTexture* lone = nullptr;
    unsigned int count = 0;
};

std::string_view HudReplacementBasename(std::string_view key) {
    if (key.compare(0, 7, "wprand:") != 0 && key.compare(0, 11, "skinrandom:") != 0)
        return {};
    const size_t separator = key.rfind(':');
    return separator > 6 ? key.substr(separator + 1) : std::string_view{};
}

RwTexture* CollectHudIconCandidate(RwTexture* texture, void* data) {
    if (!texture || !texture->raster)
        return texture;
    const size_t length = strnlen_s(texture->name, sizeof(texture->name));
    if (length < 4 || length == sizeof(texture->name) || _stricmp(texture->name + length - 4, "icon") != 0 ||
        _stricmp(texture->name, "skipicon") == 0) {
        return texture;
    }

    auto& candidates = *static_cast<HudIconCandidates*>(data);
    const std::string name = OrcToLowerAscii(std::string(texture->name, length));
    candidates.lone = texture;
    ++candidates.count;

    int score = -1;
    if (!candidates.vanillaIcon.empty() && name == candidates.vanillaIcon)
        score = 7;
    else if (!candidates.folderIcon.empty() && name == candidates.folderIcon)
        score = 6;
    else if (!candidates.variantIcon.empty() && name == candidates.variantIcon)
        score = 5;
    else if (!candidates.matchIcon.empty() && name == candidates.matchIcon)
        score = 4;
    else if (!candidates.replacementBasename.empty() && name.find(candidates.replacementBasename) != std::string::npos)
        score = 3;
    else if (!candidates.match.empty() && candidates.match != candidates.folder && name.find(candidates.match) != std::string::npos)
        score = 2;
    else if (!candidates.folder.empty() && name.find(candidates.folder) != std::string::npos)
        score = 1;

    if (score >= 0 && (score > candidates.bestScore ||
        (score == candidates.bestScore && length > candidates.bestNameLength))) {
        candidates.best = texture;
        candidates.bestScore = score;
        candidates.bestNameLength = length;
    }
    return texture;
}

template <typename Asset>
RwTexture* FindHudIconForAsset(WeaponHudIconCacheEntry& cache, const Asset& asset,
    int weaponType, unsigned int stockHash, const std::string& replacementKey) {
    // Rescans invalidate the borrowed pointers before destroying the private dictionaries.
    RwTexDictionary* dict = asset.txd;
    if (cache.asset == &asset && cache.dict == dict && cache.stockHash == stockHash &&
        cache.weaponType == weaponType && cache.replacementKey == &replacementKey) {
        return cache.texture;
    }

    cache = { &asset, &replacementKey, dict, stockHash, weaponType, nullptr };
    if (!dict)
        return nullptr;

    RwTexture* texture = RwTexDictionaryFindHashNamedTexture(dict, stockHash);
    if (!texture || !texture->raster) {
        const std::string vanilla = OrcGetWeaponModelBaseNameLower(weaponType);
        const std::string_view variant = HudReplacementBasename(replacementKey);
        HudIconCandidates candidates{
            vanilla.empty() ? std::string{} : vanilla + "icon",
            asset.weaponNameLower.empty() ? std::string{} : asset.weaponNameLower + "icon",
            asset.matchNameLower.empty() ? std::string{} : asset.matchNameLower + "icon",
            variant.empty() ? std::string{} : std::string(variant) + "icon",
            variant, asset.weaponNameLower, asset.matchNameLower
        };
        RwTexDictionaryForAllTextures(dict, &CollectHudIconCandidate, &candidates);
        texture = candidates.best;
        if (!texture && candidates.count == 1)
            texture = candidates.lone;
    }

    cache.texture = texture;
    OrcLogInfoThrottled(430, 4000u, "hud icon: wt=%d hash=%08X asset=%s texture=%s", weaponType, stockHash,
        asset.displayName.c_str(), texture ? texture->name : "-");
    return texture;
}
}  // namespace

static void ClearWeaponHudIconCache() {
    g_textureHudIcon = {};
    g_replacementHudIcon = {};
}

RwTexture* OrcWeaponHudResolveIconTexture(CPed* ped, int weaponType, unsigned int stockHash) {
    if (!ped || weaponType <= 0)
        return nullptr;

    WeaponReplacementAsset* replacement = g_weaponReplacementEnabled
        ? OrcResolveUsableWeaponReplacementAssetForPed(ped, weaponType, true)
        : nullptr;
    if (g_weaponTexturesEnabled) {
        static const std::string stockKey;
        const std::string& hint = replacement ? replacement->key : stockKey;
        if (WeaponTextureAsset* asset = OrcResolveUsableWeaponTextureAssetForPed(ped, weaponType, true, &hint)) {
            if (RwTexture* texture = FindHudIconForAsset(g_textureHudIcon, *asset, weaponType, stockHash,
                    replacement ? replacement->key : asset->key)) {
                return texture;
            }
        }
    }
    return replacement
        ? FindHudIconForAsset(g_replacementHudIcon, *replacement, weaponType, stockHash, replacement->key)
        : nullptr;
}

struct WeaponTextureApplyCtx {
    RwTexDictionary* dict = nullptr;
};

static RpMaterial* WeaponTextureApplyMaterialCB(RpMaterial* material, void* data) {
    if (!material || !material->texture || !data)
        return material;
    WeaponTextureApplyCtx* ctx = reinterpret_cast<WeaponTextureApplyCtx*>(data);
    if (!ctx->dict)
        return material;
    RwTexture* replacement = RwTexDictionaryFindNamedTexture(ctx->dict, material->texture->name);
    if (!replacement || !replacement->raster || replacement == material->texture)
        return material;
    WeaponTextureRecordMaterialRestore(material, material->texture);
    material->texture = replacement;
    return material;
}

static RpAtomic* WeaponTextureApplyAtomicCB(RpAtomic* atomic, void* data) {
    if (!atomic || !atomic->geometry)
        return atomic;
    RpGeometryForAllMaterials(atomic->geometry, WeaponTextureApplyMaterialCB, data);
    return atomic;
}

static void OrcApplyWeaponTextureToRwObject(RwObject* object, WeaponTextureAsset* asset) {
    if (!object || !asset || !EnsureWeaponTextureAssetLoaded(*asset))
        return;
    WeaponTextureApplyCtx ctx;
    ctx.dict = asset->txd;
    __try {
        if (object->type == rpCLUMP) {
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(object), WeaponTextureApplyAtomicCB, &ctx);
        } else if (object->type == rpATOMIC) {
            WeaponTextureApplyAtomicCB(reinterpret_cast<RpAtomic*>(object), &ctx);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon texture apply: SEH ex=0x%08X asset=%s", GetExceptionCode(), asset->displayName.c_str());
    }
}

void OrcApplyWeaponTexturesCombined(CPed* ped,
    int wt,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement) {
    if (!g_enabled || !g_weaponTexturesEnabled || !object)
        return;
    if (!weaponMeshIsReplacement)
        OrcApplyWeaponStockRemapToObject(ped, wt, object);
    if (customAsset && weaponMeshIsReplacement)
        OrcApplyWeaponTextureAssetTxdRemapVariants(ped, wt, object, customAsset);
    OrcApplyWeaponTextureToRwObject(object, customAsset);
}

static int PopWeaponReplacementRandomChoice(const std::string& bagPoolKey, const std::vector<int>& sourcePool,
    int avoidChoice = -2) {
    if (sourcePool.empty())
        return -2;
    std::vector<int>& bag = g_weaponReplacementRandomBags[bagPoolKey];
    auto refill = [&]() {
        bag = sourcePool;
        if (g_weaponReplacementRandomIncludeVanilla)
            bag.push_back(kWeaponReplacementVanillaChoice);
        OrcRandomNoRepeatShuffle(bag);
    };
    if (bag.empty())
        refill();
    if (!g_weaponReplacementRandomIncludeVanilla && bag.back() == kWeaponReplacementVanillaChoice) {
        bag.pop_back();
        if (bag.empty())
            refill();
    }
    if (bag.back() == avoidChoice) {
        if (bag.size() == 1 && sourcePool.size() + (g_weaponReplacementRandomIncludeVanilla ? 1u : 0u) > 1)
            refill();
        auto it = std::find_if(bag.rbegin(), bag.rend(), [avoidChoice](int value) {
            return value != avoidChoice && (g_weaponReplacementRandomIncludeVanilla || value != kWeaponReplacementVanillaChoice);
        });
        if (it != bag.rend()) {
            const int picked = *it;
            bag.erase(std::prev(it.base()));
            return picked;
        }
    }
    const int v = bag.back();
    bag.pop_back();
    return v;
}

static std::vector<int> BuildWeaponReplacementSequentialPool(const std::vector<int>& sourcePool) {
    std::vector<int> pool = sourcePool;
    if (g_weaponReplacementRandomIncludeVanilla &&
        std::find(pool.begin(), pool.end(), kWeaponReplacementVanillaChoice) == pool.end()) {
        pool.insert(pool.begin(), kWeaponReplacementVanillaChoice);
    }
    return pool;
}

static int PickStickyWeaponReplacementChoice(CPed* ped,
    const std::string& stickySuffix,
    const std::string& bagPoolKey,
    const std::vector<int>& sourcePool) {
    if (!ped || sourcePool.empty())
        return -2;
    const int pedRef = OrcSafeGetPedRef(ped);
    if (pedRef <= 0)
        return PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool);
    const std::string choiceKey = std::to_string(pedRef) + "|" + stickySuffix;
    auto chosen = g_weaponReplacementRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponReplacementRandomChoiceByPed.end()) {
        if (chosen->second != kWeaponReplacementVanillaChoice || g_weaponReplacementRandomIncludeVanilla) {
            if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL)
                g_weaponReplacementSequentialStates[bagPoolKey].activeChoiceKeys.insert(choiceKey);
            else if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT)
                g_weaponReplacementNoRepeatStates[bagPoolKey].activeChoiceKeys.insert(choiceKey);
            return chosen->second;
        }
        EraseSequentialChoiceKeyFromStates(g_weaponReplacementSequentialStates, choiceKey);
        ReleaseNoRepeatChoiceKeyFromStates(g_weaponReplacementNoRepeatStates, g_weaponReplacementRandomChoiceByPed,
            choiceKey, false);
        g_weaponReplacementRandomChoiceByPed.erase(chosen);
    }
    PruneRandomChoicesForDeadPeds(
        g_weaponReplacementRandomChoiceByPed,
        g_weaponReplacementSequentialStates,
        g_weaponReplacementNoRepeatStates);

    std::optional<int> avoidChoice;
    const auto avoidIt = g_weaponReplacementRerollAvoidChoiceByPed.find(choiceKey);
    if (avoidIt != g_weaponReplacementRerollAvoidChoiceByPed.end()) {
        if (avoidIt->second.vanilla)
            avoidChoice = kWeaponReplacementVanillaChoice;
        else if (const int index = FindWeaponReplacementAssetIndexByKeyLower(avoidIt->second.assetKeyLower); index >= 0)
            avoidChoice = index;
    }
    if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL) {
        const std::vector<int> sequentialPool = BuildWeaponReplacementSequentialPool(sourcePool);
        return OrcSequentialPickSticky(
            g_weaponReplacementSequentialStates[bagPoolKey],
            g_weaponReplacementRandomChoiceByPed,
            choiceKey,
            sequentialPool,
            -2,
            avoidChoice);
    }
    if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        const std::vector<int> noRepeatPool = BuildWeaponReplacementSequentialPool(sourcePool);
        return OrcRandomNoRepeatPickSticky(
            g_weaponReplacementNoRepeatStates[bagPoolKey],
            g_weaponReplacementRandomChoiceByPed,
            choiceKey,
            noRepeatPool,
            -2,
            avoidChoice);
    }
    const int picked = PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool, avoidChoice.value_or(-2));
    if (picked != -2)
        g_weaponReplacementRandomChoiceByPed[choiceKey] = picked;
    return picked;
}

static void ForgetFailedWeaponChoice(int failedIndex,
    std::vector<int>& sourcePool,
    const std::string& poolKey,
    std::unordered_map<std::string, std::vector<int>>& bags,
    std::unordered_map<std::string, int>& choices,
    std::unordered_map<std::string, OrcSequentialPickState>& sequentialStates,
    std::unordered_map<std::string, OrcRandomNoRepeatPickState>& noRepeatStates) {
    sourcePool.erase(std::remove(sourcePool.begin(), sourcePool.end(), failedIndex), sourcePool.end());
    const auto bag = bags.find(poolKey);
    if (bag != bags.end())
        bag->second.erase(std::remove(bag->second.begin(), bag->second.end(), failedIndex), bag->second.end());
    for (auto it = choices.begin(); it != choices.end();) {
        if (it->second != failedIndex) {
            ++it;
            continue;
        }
        EraseSequentialChoiceKeyFromStates(sequentialStates, it->first);
        ReleaseNoRepeatChoiceKeyFromStates(noRepeatStates, choices, it->first, false);
        it = choices.erase(it);
    }
}

static WeaponReplacementAsset* PickUsableWeaponReplacement(CPed* ped, const std::string& stickySuffix,
    const std::string& poolKey, std::vector<int>& pool, bool& choseVanilla) {
    while (!pool.empty()) {
        const int pick = PickStickyWeaponReplacementChoice(ped, stickySuffix, poolKey, pool);
        if (pick < kWeaponReplacementVanillaChoice || pick >= (int)g_weaponReplacementAssets.size())
            return nullptr;
        choseVanilla = pick == kWeaponReplacementVanillaChoice;
        WeaponReplacementAsset* asset = choseVanilla ? nullptr : &g_weaponReplacementAssets[(size_t)pick];
        if (choseVanilla || EnsureWeaponReplacementAssetLoaded(*asset)) {
            if (!g_weaponReplacementRerollAvoidChoiceByPed.empty()) {
                const int pedRef = OrcSafeGetPedRef(ped);
                if (pedRef > 0)
                    g_weaponReplacementRerollAvoidChoiceByPed.erase(std::to_string(pedRef) + "|" + stickySuffix);
            }
            return asset;
        }
        ForgetFailedWeaponChoice(pick, pool, poolKey, g_weaponReplacementRandomBags, g_weaponReplacementRandomChoiceByPed,
            g_weaponReplacementSequentialStates, g_weaponReplacementNoRepeatStates);
    }
    return nullptr;
}

WeaponReplacementAsset* OrcResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
    if (!g_weaponReplacementEnabled || !ped || wt <= 0)
        return nullptr;
    if (g_livePreviewHeldBaseForceVanilla && wt == g_livePreviewHeldBaseWeaponType &&
        OrcWeaponUiLivePreviewMatchesPedWeaponsIni(ped)) {
        CPlayerPed* local = FindPlayerPed(0);
        if (local && ped == local)
            return nullptr;
    }
    if (g_livePreviewHeldCustomForceActive && wt == g_livePreviewHeldCustomWeaponType &&
        !g_livePreviewHeldCustomKey.empty() && OrcWeaponUiLivePreviewMatchesPedWeaponsIni(ped)) {
        CPlayerPed* local = FindPlayerPed(0);
        if (local && ped == local) {
            WeaponReplacementAsset* forced = FindWeaponReplacementAssetByKey(g_livePreviewHeldCustomKey);
            if (forced)
                return EnsureWeaponReplacementAssetLoaded(*forced) ? forced : nullptr;
        }
    }
    const auto& weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return nullptr;

    if (!g_weaponReplacementByNick.empty() && samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = OrcToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                for (const std::string& weaponName : weaponNames) {
                    const std::string nickKey = MakeWeaponReplacementKey(weaponName, nickLower);
                    auto nickIt = g_weaponReplacementByNick.find(nickKey);
                    if (nickIt != g_weaponReplacementByNick.end() &&
                        nickIt->second >= 0 && nickIt->second < (int)g_weaponReplacementAssets.size()) {
                        WeaponReplacementAsset& asset = g_weaponReplacementAssets[(size_t)nickIt->second];
                        if (EnsureWeaponReplacementAssetLoaded(asset))
                            return &asset;
                    }
                }
            }
        }
    }

    if (allowRandom) {
        bool choseVanilla = false;
        if (!g_weaponReplacementRandomBySkin.empty()) {
            const std::string skinLower = OrcToLowerAscii(GetPedStdSkinDffName(ped));
            for (const std::string& weaponName : weaponNames) {
                if (skinLower.empty())
                    break;
                const std::string skinKey = MakeWeaponReplacementKey(weaponName, skinLower);
                auto skinIt = g_weaponReplacementRandomBySkin.find(skinKey);
                if (skinIt != g_weaponReplacementRandomBySkin.end()) {
                    if (WeaponReplacementAsset* picked = PickUsableWeaponReplacement(ped, "sr|" + skinKey,
                            skinKey, skinIt->second, choseVanilla))
                        return picked;
                    if (choseVanilla)
                        return nullptr;
                }
            }
        }
        // CJ can have no base skin DFF. The weapon pool only needs the weapon name.
        for (const std::string& weaponName : weaponNames) {
            auto wIt = g_weaponReplacementRandomByWeapon.find(weaponName);
            if (wIt != g_weaponReplacementRandomByWeapon.end()) {
                if (WeaponReplacementAsset* picked = PickUsableWeaponReplacement(ped, "wr|" + weaponName,
                        "w|" + weaponName, wIt->second, choseVanilla))
                    return picked;
                if (choseVanilla)
                    return nullptr;
            }
        }
    }
    return nullptr;
}

std::string OrcResolveUsableWeaponReplacementKeyForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponReplacementAsset* asset = OrcResolveUsableWeaponReplacementAssetForPed(ped, wt, allowRandom);
    return asset ? asset->key : std::string{};
}

bool OrcPinWeaponReplacementChoiceForPed(CPed* ped, int wt, const std::string& replacementKey) {
    if (!ped || wt <= 0 || replacementKey.empty())
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return false;
    const auto& weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return false;

    const std::string keyLower = OrcToLowerAscii(replacementKey);
    const int assetIndex = FindWeaponReplacementAssetIndexByKeyLower(keyLower);
    if (assetIndex < 0)
        return false;
    const auto& asset = g_weaponReplacementAssets[(size_t)assetIndex];
    if (std::find(weaponNames.begin(), weaponNames.end(), asset.weaponNameLower) == weaponNames.end())
        return false;

    bool pinned = false;
    const std::string pedPrefix = std::to_string(pedRef) + "|";
    const std::string skinRaw = GetPedStdSkinDffName(ped);
    for (const std::string& weaponName : weaponNames) {
        if (!skinRaw.empty()) {
            const std::string skinKey = MakeWeaponReplacementKey(weaponName, OrcToLowerAscii(skinRaw));
            auto itSkinPool = g_weaponReplacementRandomBySkin.find(skinKey);
            if (itSkinPool != g_weaponReplacementRandomBySkin.end() && !itSkinPool->second.empty()) {
                g_weaponReplacementRandomChoiceByPed[pedPrefix + "sr|" + skinKey] = assetIndex;
                pinned = true;
            }
        }
        auto itWPool = g_weaponReplacementRandomByWeapon.find(weaponName);
        if (itWPool != g_weaponReplacementRandomByWeapon.end() && !itWPool->second.empty()) {
            g_weaponReplacementRandomChoiceByPed[pedPrefix + "wr|" + weaponName] = assetIndex;
            pinned = true;
        }
    }
    return pinned;
}

void OrcCollectWeaponReplacementVariantKeys(int wt, std::vector<std::string>& outKeys) {
    outKeys.clear();
    const auto& weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return;
    outKeys.reserve(g_weaponReplacementAssets.size());
    for (const std::string& weaponName : weaponNames) {
        const std::string pref = std::string("wprand:") + weaponName + ":";
        for (const WeaponReplacementAsset& asset : g_weaponReplacementAssets) {
            if (asset.key.rfind(pref, 0) != 0)
                continue;
            outKeys.push_back(asset.key);
        }
    }
    std::sort(outKeys.begin(), outKeys.end());
    outKeys.erase(std::unique(outKeys.begin(), outKeys.end()), outKeys.end());
}


void OrcWeaponAssetsShutdown() {
    OrcRestoreWeaponTextureOverrides();
    ClearWeaponStockRemapRuntime();
    DestroyWeaponReplacementAssets();
    DestroyWeaponTextureAssets();
}

bool OrcWeaponReplacementIsStickyVanillaChoice(CPed* ped, int wt) {
    if (!g_weaponReplacementRandomIncludeVanilla || !ped || wt <= 0)
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return false;
    const auto& weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return false;
    const std::string pedPrefix = std::to_string(pedRef) + "|";
    const std::string skinRaw = GetPedStdSkinDffName(ped);
    for (const std::string& weaponName : weaponNames) {
        if (!skinRaw.empty()) {
            const std::string skinKey = MakeWeaponReplacementKey(weaponName, OrcToLowerAscii(skinRaw));
            auto itSkin = g_weaponReplacementRandomChoiceByPed.find(pedPrefix + "sr|" + skinKey);
            if (itSkin != g_weaponReplacementRandomChoiceByPed.end() &&
                itSkin->second == kWeaponReplacementVanillaChoice)
                return true;
        }
        auto itW = g_weaponReplacementRandomChoiceByPed.find(pedPrefix + "wr|" + weaponName);
        if (itW != g_weaponReplacementRandomChoiceByPed.end() &&
            itW->second == kWeaponReplacementVanillaChoice)
            return true;
    }
    return false;
}

size_t OrcWeaponAssetsDbgReplacementNickKeys() {
    return g_weaponReplacementByNick.size();
}

size_t OrcWeaponAssetsDbgRandomReplacementSkinPools() {
    return g_weaponReplacementRandomBySkin.size();
}

size_t OrcWeaponAssetsDbgRandomReplacementWeaponPools() {
    return g_weaponReplacementRandomByWeapon.size();
}

