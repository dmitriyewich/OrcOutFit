#include "plugin.h"

#include "CPed.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CClumpModelInfo.h"
#include "CVisibilityPlugins.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "CTxdStore.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
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

void OrcClearAllWeaponReplacementInstances();
static std::vector<WeaponReplacementAsset> g_weaponReplacementAssets;
static std::unordered_map<std::string, int> g_weaponReplacementByNick;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomByWeapon;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBags;
static std::unordered_map<std::string, OrcSequentialPickState> g_weaponReplacementSequentialStates;
static std::unordered_map<std::string, OrcRandomNoRepeatPickState> g_weaponReplacementNoRepeatStates;
static std::unordered_map<std::string, int> g_weaponReplacementRandomChoiceByPed;
static WeaponReplacementStats g_weaponReplacementStats;

static constexpr int kWeaponReplacementVanillaChoice = -1;

std::string OrcGetWeaponModelBaseNameLower(int wt);

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

static std::vector<std::string> WeaponLookupNamesLower(int wt) {
    std::vector<std::string> names;
    AppendUniqueLower(names, OrcGetWeaponModelBaseNameLower(wt));
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

static WeaponReplacementAsset* FindWeaponReplacementAssetByKey(const std::string& keyLower) {
    if (keyLower.empty())
        return nullptr;
    for (WeaponReplacementAsset& asset : g_weaponReplacementAssets) {
        if (OrcToLowerAscii(asset.key) == keyLower)
            return &asset;
    }
    return nullptr;
}

static int FindWeaponReplacementAssetIndexByKeyLower(const std::string& keyLower) {
    if (keyLower.empty())
        return -1;
    for (int i = 0; i < (int)g_weaponReplacementAssets.size(); ++i) {
        if (OrcToLowerAscii(g_weaponReplacementAssets[(size_t)i].key) == keyLower)
            return i;
    }
    return -1;
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
        choice.assetKeyLower = OrcToLowerAscii(g_weaponReplacementAssets[(size_t)kv.second].key);
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

static bool WeaponReplacementChoiceMatchesSnapshot(int choice, const WeaponReplacementStickyChoiceSnapshot& snapshot) {
    if (choice == kWeaponReplacementVanillaChoice)
        return snapshot.vanilla;
    if (choice < 0 || choice >= (int)g_weaponReplacementAssets.size())
        return false;
    return !snapshot.assetKeyLower.empty() &&
        OrcToLowerAscii(g_weaponReplacementAssets[(size_t)choice].key) == snapshot.assetKeyLower;
}

static std::string WeaponReplacementChoiceSortKey(int choice) {
    if (choice == kWeaponReplacementVanillaChoice)
        return std::string{};
    if (choice < 0 || choice >= (int)g_weaponReplacementAssets.size())
        return "~";
    return OrcToLowerAscii(g_weaponReplacementAssets[(size_t)choice].key);
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
    if (wt > 0 && wt < (int)g_weaponDatIdeName.size() && !g_weaponDatIdeName[(size_t)wt].empty())
        return OrcToLowerAscii(g_weaponDatIdeName[(size_t)wt]);
    if (wt > 0 && wt < (int)g_cfg.size() && g_cfg[(size_t)wt].name && g_cfg[(size_t)wt].name[0])
        return OrcToLowerAscii(g_cfg[(size_t)wt].name);
    return {};
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



static void ClearWeaponHudIconCache(int txdSlot = -1);

static void DestroyWeaponReplacementAssets() {
    ClearWeaponHudIconCache();
    for (auto& asset : g_weaponReplacementAssets) {
        OrcDestroyRwObjectInstance(asset.rwObject);
        asset.txdSlot = -1;
    }
    g_weaponReplacementAssets.clear();
    g_weaponReplacementByNick.clear();
    g_weaponReplacementRandomBySkin.clear();
    g_weaponReplacementRandomByWeapon.clear();
    g_weaponReplacementRandomBags.clear();
    g_weaponReplacementSequentialStates.clear();
    g_weaponReplacementNoRepeatStates.clear();
    g_weaponReplacementRandomChoiceByPed.clear();
    g_weaponReplacementStats = {};
}


static bool EnsureWeaponReplacementAssetLoaded(WeaponReplacementAsset& asset) {
    if (asset.rwObject)
        return true;
    if (asset.loadAttempted)
        return false;
    asset.loadAttempted = true;

    if (!OrcFileExistsA(asset.dffPath.c_str())) {
        if (!asset.loadFailedLogged) {
            asset.loadFailedLogged = true;
            OrcLogError("weapon replacement \"%s\": DFF missing %s", asset.displayName.c_str(), asset.dffPath.c_str());
        }
        return false;
    }
    if (asset.txdPath.empty() || !OrcFileExistsA(asset.txdPath.c_str())) {
        if (!asset.loadFailedLogged) {
            asset.loadFailedLogged = true;
            OrcLogError("weapon replacement \"%s\": TXD missing or invalid path", asset.displayName.c_str());
        }
        return false;
    }

    std::string txdName = "orc_wr_" + asset.key;
    for (char& c : txdName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    int txdSlot = CTxdStore::FindTxdSlot(txdName.c_str());
    if (txdSlot == -1)
        txdSlot = CTxdStore::AddTxdSlot(txdName.c_str());
    if (txdSlot == -1) {
        OrcLogError("weapon replacement \"%s\": CTxdStore::AddTxdSlot failed", asset.displayName.c_str());
        return false;
    }

    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.txdPath.c_str());
    if (txdStream) {
        ClearWeaponHudIconCache(txdSlot);
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        OrcLogError("weapon replacement \"%s\": LoadTxd failed", asset.displayName.c_str());
        return false;
    }
    asset.txdSlot = txdSlot;

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(txdSlot);
    RwStream* stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.dffPath.c_str());
    if (!stream) {
        CTxdStore::PopCurrentTxd();
        OrcLogError("weapon replacement \"%s\": RwStreamOpen DFF failed", asset.displayName.c_str());
        return false;
    }

    bool ok = false;
    if (RwStreamFindChunk(stream, rwID_CLUMP, nullptr, nullptr)) {
        RpClump* c = RpClumpStreamRead(stream);
        if (c) {
            asset.rwObject = reinterpret_cast<RwObject*>(c);
            RpClumpForAllAtomics(c, OrcInitAtomicCB, nullptr);
            ok = true;
        }
    }
    RwStreamClose(stream, nullptr);

    if (!ok) {
        stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.dffPath.c_str());
        if (stream) {
            if (RwStreamFindChunk(stream, rwID_ATOMIC, nullptr, nullptr)) {
                RpAtomic* a = RpAtomicStreamRead(stream);
                if (a) {
                    if (!RpAtomicGetFrame(a)) {
                        RwFrame* frame = RwFrameCreate();
                        if (frame) RpAtomicSetFrame(a, frame);
                    }
                    OrcInitAtomicCB(a, nullptr);
                    asset.rwObject = reinterpret_cast<RwObject*>(a);
                    ok = true;
                }
            }
            RwStreamClose(stream, nullptr);
        }
    }
    CTxdStore::PopCurrentTxd();

    if (!ok) {
        OrcLogError("weapon replacement \"%s\": DFF has no readable clump/atomic", asset.displayName.c_str());
        return false;
    }

    asset.loadFailedLogged = false;
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
        RpClump* clone = RpClumpClone(reinterpret_cast<RpClump*>(asset.rwObject));
        if (!clone) return nullptr;
        RpClumpForAllAtomics(clone, OrcInitAtomicCB, nullptr);
        // A DFF may ship with its gunflash atomic visible. A fresh clone can be rendered before GTA's
        // end-of-batch ClearGunFlash, producing a one-frame muzzle flash while selecting the weapon.
        // Start hidden; vanilla SetGunFlashAlpha enables the same atomic on an actual shot.
        OrcInitializeReplacementGunflashHidden(clone);
        return reinterpret_cast<RwObject*>(clone);
    }
    if (asset.rwObject->type == rpATOMIC) {
        RpAtomic* clone = RpAtomicClone(reinterpret_cast<RpAtomic*>(asset.rwObject));
        if (!clone) return nullptr;
        RwFrame* frame = RwFrameCreate();
        if (frame) RpAtomicSetFrame(clone, frame);
        OrcInitAtomicCB(clone, nullptr);
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
static std::vector<WeaponTextureRestoreEntry> g_weaponTextureHeldRestoreEntries;
static int g_weaponTextureHeldDeferDepth = 0;
static WeaponTextureStats g_weaponTextureStats;

static bool WeaponTextureRestoreUsingHeldDeferBucket() {
    return g_weaponTextureHeldDeferDepth > 0;
}

static void WeaponTextureRecordMaterialRestore(RpMaterial* material, RwTexture* oldTex) {
    if (!material)
        return;
    if (WeaponTextureRestoreUsingHeldDeferBucket())
        g_weaponTextureHeldRestoreEntries.push_back({ material, oldTex });
    else
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
    /// Resolved base texture when present (optional). Materials may reference a different RwTexture* with same name — apply uses baseTexName.
    RwTexture* original = nullptr;
    std::string baseTexName;
    std::vector<RwTexture*> remaps;
};

struct WeaponStockRemapCatalog {
    int wt = -1;
    int txdIndex = -1;
    std::vector<WeaponStockRemapSlot> slots;
};

static std::unordered_map<int, WeaponStockRemapCatalog> g_weaponStockRemapCatalogByWt;
static std::unordered_map<std::string, std::vector<int>> g_weaponStockRemapSelectionByPedWt;

// Remap variants (*_suffix_remap…) inside Orc loaded Guns/GunsNick TXDs (distinct from game's model TXD).
static std::unordered_map<std::string, WeaponStockRemapCatalog> g_weaponTextureAssetRemapCatalogByKey;
static std::unordered_map<std::string, std::vector<int>> g_weaponTextureAssetRemapSelectionByKey;

static void ClearWeaponStockRemapRuntime() {
    g_weaponStockRemapCatalogByWt.clear();
    g_weaponStockRemapSelectionByPedWt.clear();
    g_weaponTextureAssetRemapCatalogByKey.clear();
    g_weaponTextureAssetRemapSelectionByKey.clear();
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

static int WeaponStockRemapGetOrAddSlot(WeaponStockRemapCatalog& cat, RwTexDictionary* dict, const std::string& originalBaseNameFromRemapSuffix) {
    const int existing = WeaponStockRemapFindSlotIndex(cat, originalBaseNameFromRemapSuffix);
    if (existing >= 0)
        return existing;
    if (cat.slots.size() >= kWeaponStockRemapMaxSlots)
        return -1;
    WeaponStockRemapSlot slot;
    slot.baseTexName = originalBaseNameFromRemapSuffix;
    if (dict && !originalBaseNameFromRemapSuffix.empty())
        slot.original = RwTexDictionaryFindNamedTexture(dict, originalBaseNameFromRemapSuffix.c_str());
    cat.slots.push_back(std::move(slot));
    return (int)cat.slots.size() - 1;
}

struct WeaponStockRemapScanCtx {
    WeaponStockRemapCatalog* catalog = nullptr;
    RwTexDictionary* dict = nullptr;
};

static RwTexture* WeaponStockRemapCollectTextureCB(RwTexture* texture, void* data) {
    if (!texture || !texture->name[0] || !data)
        return texture;
    WeaponStockRemapScanCtx* ctx = reinterpret_cast<WeaponStockRemapScanCtx*>(data);
    if (!ctx->catalog || !ctx->dict)
        return texture;

    const std::string name = texture->name;
    const std::string lower = OrcToLowerAscii(name);
    const size_t remapPos = lower.find("_remap");
    if (remapPos == std::string::npos || remapPos == 0)
        return texture;

    const std::string originalName = name.substr(0, remapPos);
    const int slotIdx = WeaponStockRemapGetOrAddSlot(*ctx->catalog, ctx->dict, originalName);
    if (slotIdx < 0)
        return texture;

    WeaponStockRemapSlot& slot = ctx->catalog->slots[(size_t)slotIdx];
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

/// Fills `out` with slots from any TXD (game model or Orc-loaded Guns/GunsNick). Does not clear `out` first.
static bool WeaponRemapScanDictIntoCatalog(RwTexDictionary* dict, WeaponStockRemapCatalog& out) {
    if (!dict)
        return false;
    WeaponStockRemapScanCtx ctx;
    ctx.catalog = &out;
    ctx.dict = dict;
    RwTexDictionaryForAllTextures(dict, WeaponStockRemapCollectTextureCB, &ctx);
    WeaponRemapPruneEmptyRemapSlots(out);
    return true;
}

static bool OrcTryBuildWeaponStockRemapCatalog(int wt, WeaponStockRemapCatalog& out) {
    out = WeaponStockRemapCatalog{};
    out.wt = wt;
    if (wt <= 0)
        return true;

    CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
    if (!wi || wi->m_nModelId <= 0)
        return true;

    CBaseModelInfo* mi = CModelInfo::GetModelInfo(wi->m_nModelId);
    if (!mi)
        return true;

    out.txdIndex = mi->m_nTxdIndex;
    RwTexDictionary* dict = GetTxdDictionaryByIndexMain(out.txdIndex);
    if (!dict)
        return false;

    __try {
        WeaponRemapScanDictIntoCatalog(dict, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon stock remap scan: SEH ex=0x%08X wt=%d txd=%d", GetExceptionCode(), wt, out.txdIndex);
        out.slots.clear();
        return true;
    }
    return true;
}

static const WeaponStockRemapCatalog* OrcGetWeaponStockRemapCatalog(int wt) {
    auto it = g_weaponStockRemapCatalogByWt.find(wt);
    if (it != g_weaponStockRemapCatalogByWt.end())
        return &it->second;

    WeaponStockRemapCatalog cat;
    if (!OrcTryBuildWeaponStockRemapCatalog(wt, cat))
        return nullptr;
    const auto ins = g_weaponStockRemapCatalogByWt.insert({ wt, std::move(cat) });
    return &ins.first->second;
}

static void WeaponRemapEnsureSelectionsForCatalog(CPed* ped,
                                                  const WeaponStockRemapCatalog& cat,
                                                  std::unordered_map<std::string, std::vector<int>>& selectionStore,
                                                  const std::string& selectionKeyTailAfterPedPipe) {
    if (!ped || cat.slots.empty())
        return;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    const std::string key = std::to_string(pedRef) + "|" + selectionKeyTailAfterPedPipe;
    auto it = selectionStore.find(key);
    if (it != selectionStore.end() && it->second.size() == cat.slots.size())
        return;

    std::vector<int> sel;
    sel.reserve(cat.slots.size());
    for (const auto& slot : cat.slots) {
        const int n = (int)slot.remaps.size();
        if (n <= 0)
            sel.push_back(-1);
        else
            sel.push_back(rand() % n);
    }
    selectionStore[key] = std::move(sel);
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
        bool baseMatch =
            slot.baseTexName.empty()
                ? (slot.original != nullptr && material->texture == slot.original)
                : (_stricmp(mName, slot.baseTexName.c_str()) == 0);
        if (!baseMatch)
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
                                                 RwObject* object,
                                                 const WeaponStockRemapCatalog* cat,
                                                 std::unordered_map<std::string, std::vector<int>>& selectionStore,
                                                 const std::string& selectionKeyTailAfterPedPipe) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped || !object || !cat || cat->slots.empty())
        return;

    WeaponRemapEnsureSelectionsForCatalog(ped, *cat, selectionStore, selectionKeyTailAfterPedPipe);
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return;
    const std::string key = std::to_string(pedRef) + "|" + selectionKeyTailAfterPedPipe;
    auto sit = selectionStore.find(key);
    if (sit == selectionStore.end() || sit->second.size() != cat->slots.size())
        return;

    WeaponStockRemapApplyCtx ctx;
    ctx.catalog = cat;
    ctx.selections = &sit->second;
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
    const WeaponStockRemapCatalog* cat = OrcGetWeaponStockRemapCatalog(wt);
    if (!cat || cat->slots.empty())
        return;
    OrcApplyWeaponRemapCatalogToRwObject(ped, object, cat, g_weaponStockRemapSelectionByPedWt, std::to_string(wt));
}

static bool EnsureWeaponTextureAssetLoaded(WeaponTextureAsset& asset);

/// Cached *_remap catalogue for an Orc Guns/GunsNick weapon texture TXD.
static const WeaponStockRemapCatalog* OrcGetCachedWeaponTexAssetRemapCatalog(WeaponTextureAsset& asset) {
    if (!EnsureWeaponTextureAssetLoaded(asset))
        return nullptr;
    RwTexDictionary* dict = GetTxdDictionaryByIndexMain(asset.txdSlot);
    if (!dict)
        return nullptr;
    auto it = g_weaponTextureAssetRemapCatalogByKey.find(asset.key);
    if (it != g_weaponTextureAssetRemapCatalogByKey.end())
        return &it->second;

    WeaponStockRemapCatalog built{};
    built.wt = -1;
    built.txdIndex = asset.txdSlot;
    __try {
        WeaponRemapScanDictIntoCatalog(dict, built);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon Guns TXD remap scan: SEH asset=%s ex=0x%08X", asset.displayName.c_str(),
            GetExceptionCode());
        built.slots.clear();
    }
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
    const WeaponStockRemapCatalog* cat = OrcGetCachedWeaponTexAssetRemapCatalog(*asset);
    if (!cat || cat->slots.empty())
        return;
    const std::string tail = std::to_string(wt) + "|" + asset->key;
    OrcApplyWeaponRemapCatalogToRwObject(ped, object, cat, g_weaponTextureAssetRemapSelectionByKey, tail);
}

void OrcRestoreWeaponTextureOverrides() {
    if (g_weaponTextureRestoreEntries.empty())
        return;
    for (auto it = g_weaponTextureRestoreEntries.rbegin(); it != g_weaponTextureRestoreEntries.rend(); ++it) {
        if (!it->material) continue;
        __try {
            it->material->texture = it->texture;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_weaponTextureRestoreEntries.clear();
}

void OrcRestoreWeaponHeldTextureOverrides() {
    if (g_weaponTextureHeldRestoreEntries.empty())
        return;
    for (auto it = g_weaponTextureHeldRestoreEntries.rbegin(); it != g_weaponTextureHeldRestoreEntries.rend(); ++it) {
        if (!it->material) continue;
        __try {
            it->material->texture = it->texture;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_weaponTextureHeldRestoreEntries.clear();
}

void OrcWeaponHeldTextureDeferBegin() {
    ++g_weaponTextureHeldDeferDepth;
}

void OrcWeaponHeldTextureDeferEnd() {
    if (g_weaponTextureHeldDeferDepth > 0)
        --g_weaponTextureHeldDeferDepth;
}

static void DestroyWeaponTextureAssets() {
    ClearWeaponHudIconCache();
    OrcRestoreWeaponTextureOverrides();
    OrcRestoreWeaponHeldTextureOverrides();
    g_weaponTextureHeldDeferDepth = 0;
    g_weaponTextureAssetRemapCatalogByKey.clear();
    g_weaponTextureAssetRemapSelectionByKey.clear();
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
    if (asset.txdSlot >= 0 && GetTxdDictionaryByIndexMain(asset.txdSlot))
        return true;
    // GTA may flush `RwTexDictionary*` while leaving the CTxdStore slot — HUD TryGet kept seeing nullptr and
    // `loadAttempted` blocked reload forever.
    if (asset.txdSlot >= 0 && asset.loadAttempted && !GetTxdDictionaryByIndexMain(asset.txdSlot)) {
        OrcLogInfoThrottled(411,
            8000u,
            "weapon texture \"%s\": TXD RwDictionary evicted (slot=%d); reloading",
            asset.displayName.c_str(),
            asset.txdSlot);
        asset.loadAttempted = false;
    }
    if (asset.loadAttempted)
        return false;
    asset.loadAttempted = true;

    if (asset.txdPath.empty() || !OrcFileExistsA(asset.txdPath.c_str())) {
        if (!asset.loadFailedLogged) {
            asset.loadFailedLogged = true;
            OrcLogError("weapon texture \"%s\": TXD missing %s", asset.displayName.c_str(), asset.txdPath.c_str());
        }
        return false;
    }

    std::string txdName = "orc_wtex_" + asset.key;
    for (char& c : txdName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    int txdSlot = CTxdStore::FindTxdSlot(txdName.c_str());
    if (txdSlot == -1)
        txdSlot = CTxdStore::AddTxdSlot(txdName.c_str());
    if (txdSlot == -1) {
        OrcLogError("weapon texture \"%s\": CTxdStore::AddTxdSlot failed", asset.displayName.c_str());
        return false;
    }

    bool txdOk = false;
    RwStream* txdStream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, (void*)asset.txdPath.c_str());
    if (txdStream) {
        ClearWeaponHudIconCache(txdSlot);
        txdOk = CTxdStore::LoadTxd(txdSlot, txdStream);
        RwStreamClose(txdStream, nullptr);
    }
    if (!txdOk) {
        OrcLogError("weapon texture \"%s\": LoadTxd failed", asset.displayName.c_str());
        return false;
    }

    asset.txdSlot = txdSlot;
    asset.loadFailedLogged = false;
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

static WeaponTextureAsset* PickRandomWeaponTextureAsset(const std::string& mapKey) {
    auto it = g_weaponTextureRandomBySkin.find(mapKey);
    if (it == g_weaponTextureRandomBySkin.end() || it->second.empty())
        return nullptr;
    std::vector<int>& bag = g_weaponTextureRandomBags[mapKey];
    if (bag.empty()) {
        bag = it->second;
        for (int i = (int)bag.size() - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(bag[(size_t)i], bag[(size_t)j]);
        }
    }
    const int assetIndex = bag.back();
    bag.pop_back();
    if (assetIndex < 0 || assetIndex >= (int)g_weaponTextureAssets.size())
        return nullptr;
    return &g_weaponTextureAssets[(size_t)assetIndex];
}

static WeaponTextureAsset* PickStickyRandomWeaponTextureAsset(CPed* ped, const std::string& mapKey) {
    if (!ped)
        return nullptr;
    PruneRandomChoicesForDeadPeds(
        g_weaponTextureRandomChoiceByPed,
        g_weaponTextureSequentialStates,
        g_weaponTextureNoRepeatStates);
    const int pedRef = OrcSafeGetPedRef(ped);
    if (pedRef <= 0)
        return PickRandomWeaponTextureAsset(mapKey);

    const std::string choiceKey = std::to_string(pedRef) + "|" + mapKey;
    auto chosen = g_weaponTextureRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponTextureRandomChoiceByPed.end()) {
        const int assetIndex = chosen->second;
        if (assetIndex >= 0 && assetIndex < (int)g_weaponTextureAssets.size()) {
            if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL)
                g_weaponTextureSequentialStates[mapKey].activeChoiceKeys.insert(choiceKey);
            else if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT)
                g_weaponTextureNoRepeatStates[mapKey].activeChoiceKeys.insert(choiceKey);
            return &g_weaponTextureAssets[(size_t)assetIndex];
        }
    }

    int assetIndex = -1;
    if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL) {
        auto poolIt = g_weaponTextureRandomBySkin.find(mapKey);
        if (poolIt == g_weaponTextureRandomBySkin.end() || poolIt->second.empty())
            return nullptr;
        assetIndex = OrcSequentialPickSticky(
            g_weaponTextureSequentialStates[mapKey],
            g_weaponTextureRandomChoiceByPed,
            choiceKey,
            poolIt->second,
            -1);
    } else if (g_weaponTextureRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        auto poolIt = g_weaponTextureRandomBySkin.find(mapKey);
        if (poolIt == g_weaponTextureRandomBySkin.end() || poolIt->second.empty())
            return nullptr;
        assetIndex = OrcRandomNoRepeatPickSticky(
            g_weaponTextureNoRepeatStates[mapKey],
            g_weaponTextureRandomChoiceByPed,
            choiceKey,
            poolIt->second,
            -1);
    } else {
        WeaponTextureAsset* asset = PickRandomWeaponTextureAsset(mapKey);
        if (!asset)
            return nullptr;
        assetIndex = (int)(asset - g_weaponTextureAssets.data());
        if (assetIndex >= 0)
            g_weaponTextureRandomChoiceByPed[choiceKey] = assetIndex;
    }
    if (assetIndex < 0 || assetIndex >= (int)g_weaponTextureAssets.size())
        return nullptr;
    return &g_weaponTextureAssets[(size_t)assetIndex];
}

/// `wprand:<weapon_folder>:<dff_basename>` (e.g. `wprand:desert_eagle:markvii`) — same match key as
/// `Weapons\Guns\<weapon>\<basename>.txd` indexed under `weapon|basename`.
static bool WeaponTextureParseWprandReplacementKey(const std::vector<std::string>& weaponLowerExpected,
    const std::string& replacementKey,
    std::string* variantBasenameLowerOut) {
    static constexpr char kPref[] = "wprand:";
    if (replacementKey.rfind(kPref, 0) != 0 || !variantBasenameLowerOut)
        return false;
    const std::string rest = replacementKey.substr(sizeof(kPref) - 1);
    const size_t col = rest.find(':');
    if (col == std::string::npos || col == 0 || col + 1 >= rest.size())
        return false;
    const std::string weaponPart = OrcToLowerAscii(rest.substr(0, col));
    const std::string basePartRaw = rest.substr(col + 1);
    if (std::find(weaponLowerExpected.begin(), weaponLowerExpected.end(), weaponPart) == weaponLowerExpected.end())
        return false;
    *variantBasenameLowerOut = OrcToLowerAscii(OrcBaseNameNoExt(basePartRaw));
    return !variantBasenameLowerOut->empty();
}

static WeaponTextureAsset* ResolveWeaponTextureAssetForPed(CPed* ped,
    int wt,
    bool allowRandom,
    const std::string* replacementKeyHint) {
    if (!g_enabled || !g_weaponTexturesEnabled || !ped || wt <= 0)
        return nullptr;
    std::vector<std::string> weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return nullptr;

    if (g_weaponTextureNickMode && samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = OrcToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                for (const std::string& weaponName : weaponNames) {
                    const std::string nickKey = MakeWeaponReplacementKey(weaponName, nickLower);
                    auto nickIt = g_weaponTextureByNick.find(nickKey);
                    if (nickIt != g_weaponTextureByNick.end() &&
                        nickIt->second >= 0 && nickIt->second < (int)g_weaponTextureAssets.size()) {
                        return &g_weaponTextureAssets[(size_t)nickIt->second];
                    }
                }
            }
        }
    }

    const std::string* wprandKeyPtr = replacementKeyHint;
    std::string resolvedReplKeyBuf;
    if ((!wprandKeyPtr || wprandKeyPtr->empty()) && g_weaponReplacementEnabled) {
        resolvedReplKeyBuf = OrcResolveUsableWeaponReplacementKeyForPed(ped, wt, allowRandom);
        if (!resolvedReplKeyBuf.empty())
            wprandKeyPtr = &resolvedReplKeyBuf;
    }
    if (wprandKeyPtr && !wprandKeyPtr->empty()) {
        std::string variantLower;
        if (WeaponTextureParseWprandReplacementKey(weaponNames, *wprandKeyPtr, &variantLower)) {
            const std::string replWeaponLower = OrcToLowerAscii(wprandKeyPtr->substr(7, wprandKeyPtr->find(':', 7) - 7));
            const std::string replTexKey = MakeWeaponReplacementKey(replWeaponLower, variantLower);
            auto replIt = g_weaponTextureBySkin.find(replTexKey);
            if (replIt != g_weaponTextureBySkin.end() &&
                replIt->second >= 0 && replIt->second < (int)g_weaponTextureAssets.size()) {
                OrcLogInfoThrottled(402, 10000u,
                    "weapon texture: Guns\\%s\\%s.txd via replacement key \"%s\" (wt=%d)",
                    replWeaponLower.c_str(),
                    variantLower.c_str(),
                    wprandKeyPtr->c_str(),
                    wt);
                return &g_weaponTextureAssets[(size_t)replIt->second];
            }
        }
    }

    const std::string skinLowerRaw = OrcToLowerAscii(GetPedStdSkinDffName(ped));

    if (!skinLowerRaw.empty()) {
        for (const std::string& weaponName : weaponNames) {
            const std::string skinKey = MakeWeaponReplacementKey(weaponName, skinLowerRaw);
            auto skinIt = g_weaponTextureBySkin.find(skinKey);
            if (skinIt != g_weaponTextureBySkin.end() &&
                skinIt->second >= 0 && skinIt->second < (int)g_weaponTextureAssets.size()) {
                return &g_weaponTextureAssets[(size_t)skinIt->second];
            }
            if (allowRandom && g_weaponTextureRandomMode) {
                if (WeaponTextureAsset* picked = PickStickyRandomWeaponTextureAsset(ped, skinKey))
                    return picked;
            }
        }
    }

    // `Weapons\Guns\<weapon>\<weapon>.txd` is indexed as match key `<weapon>|<weapon>` (same string twice).
    // Used when ped skin has no `<weapon>\<dff>.txd` entry (many packs ship one bundle as desert_eagle\desert_eagle.txd).
    for (const std::string& weaponName : weaponNames) {
        const std::string defaultWeaponSkinKey = MakeWeaponReplacementKey(weaponName, weaponName);
        auto defIt = g_weaponTextureBySkin.find(defaultWeaponSkinKey);
        if (defIt != g_weaponTextureBySkin.end() &&
            defIt->second >= 0 && defIt->second < (int)g_weaponTextureAssets.size()) {
            OrcLogInfoThrottled(401, 8000u,
                "weapon texture: using default Guns\\%s\\%s.txd (wt=%d pedSkin=\"%s\")",
                weaponName.c_str(),
                weaponName.c_str(),
                wt,
                GetPedStdSkinDffName(ped).c_str());
            return &g_weaponTextureAssets[(size_t)defIt->second];
        }
    }
    return nullptr;
}

WeaponTextureAsset* OrcResolveUsableWeaponTextureAssetForPed(CPed* ped,
    int wt,
    bool allowRandom,
    const std::string* replacementKeyHint) {
    WeaponTextureAsset* asset = ResolveWeaponTextureAssetForPed(ped, wt, allowRandom, replacementKeyHint);
    if (!asset)
        return nullptr;
    if (EnsureWeaponTextureAssetLoaded(*asset))
        return asset;
    if (asset->key.rfind("skinrandom:", 0) == 0) {
        const int failedIndex = (int)(asset - g_weaponTextureAssets.data());
        for (auto it = g_weaponTextureRandomChoiceByPed.begin(); it != g_weaponTextureRandomChoiceByPed.end();) {
            if (it->second == failedIndex) {
                EraseSequentialChoiceKeyFromStates(g_weaponTextureSequentialStates, it->first);
                ReleaseNoRepeatChoiceKeyFromStates(g_weaponTextureNoRepeatStates, g_weaponTextureRandomChoiceByPed, it->first, false);
                it = g_weaponTextureRandomChoiceByPed.erase(it);
            } else {
                ++it;
            }
        }
    }
    return nullptr;
}

namespace {
struct WeaponHudIconCacheEntry {
    const void* asset = nullptr;
    const std::string* replacementKey = nullptr;
    RwTexDictionary* dict = nullptr;
    unsigned int stockHash = 0;
    int txdSlot = -1;
    int weaponType = 0;
    RwTexture* texture = nullptr;
};

WeaponHudIconCacheEntry g_textureHudIcon;
WeaponHudIconCacheEntry g_replacementHudIcon;

struct HudIconCandidates {
    std::string vanillaIcon;
    std::string folderIcon;
    std::string matchIcon;
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
    if (key.compare(0, 7, "wprand:") != 0)
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
        score = 6;
    else if (!candidates.folderIcon.empty() && name == candidates.folderIcon)
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
    // Only reuse the borrowed texture while its slot still owns the same dictionary.
    // Our loaders invalidate this slot before replacing the dictionary, including address reuse.
    RwTexDictionary* dict = GetTxdDictionaryByIndexMain(asset.txdSlot);
    if (cache.asset == &asset && cache.txdSlot == asset.txdSlot && cache.dict == dict && cache.stockHash == stockHash &&
        cache.weaponType == weaponType && cache.replacementKey == &replacementKey) {
        return cache.texture;
    }

    cache = { &asset, &replacementKey, dict, stockHash, asset.txdSlot, weaponType, nullptr };
    if (!dict)
        return nullptr;

    RwTexture* texture = RwTexDictionaryFindHashNamedTexture(dict, stockHash);
    if (!texture || !texture->raster) {
        const std::string vanilla = OrcGetWeaponModelBaseNameLower(weaponType);
        HudIconCandidates candidates{
            vanilla.empty() ? std::string{} : vanilla + "icon",
            asset.weaponNameLower.empty() ? std::string{} : asset.weaponNameLower + "icon",
            asset.matchNameLower.empty() ? std::string{} : asset.matchNameLower + "icon",
            HudReplacementBasename(replacementKey), asset.weaponNameLower, asset.matchNameLower
        };
        RwTexDictionaryForAllTextures(dict, &CollectHudIconCandidate, &candidates);
        texture = candidates.best;
        if (!texture && candidates.count == 1)
            texture = candidates.lone;
    }

    cache.texture = texture;
    OrcLogInfoThrottled(430, 4000u, "hud icon: wt=%d hash=%08X txd=%d asset=%s texture=%s", weaponType, stockHash,
        asset.txdSlot, asset.displayName.c_str(), texture ? texture->name : "-");
    return texture;
}
}  // namespace

static void ClearWeaponHudIconCache(int txdSlot) {
    if (txdSlot < 0 || g_textureHudIcon.txdSlot == txdSlot)
        g_textureHudIcon = {};
    if (txdSlot < 0 || g_replacementHudIcon.txdSlot == txdSlot)
        g_replacementHudIcon = {};
}

RwTexture* OrcWeaponHudResolveIconTexture(CPed* ped, int weaponType, unsigned int stockHash) {
    if (!ped || weaponType <= 0)
        return nullptr;

    WeaponReplacementAsset* replacement = g_weaponReplacementEnabled
        ? OrcResolveUsableWeaponReplacementAssetForPed(ped, weaponType, true)
        : nullptr;
    if (g_weaponTexturesEnabled) {
        const std::string* hint = replacement ? &replacement->key : nullptr;
        if (WeaponTextureAsset* asset = OrcResolveUsableWeaponTextureAssetForPed(ped, weaponType, true, hint)) {
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
    if (!replacement || replacement == material->texture)
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

void OrcApplyWeaponTextureToRwObject(RwObject* object, WeaponTextureAsset* asset) {
    if (!object || !asset || !EnsureWeaponTextureAssetLoaded(*asset))
        return;
    WeaponTextureApplyCtx ctx;
    ctx.dict = GetTxdDictionaryByIndexMain(asset->txdSlot);
    if (!ctx.dict)
        return;
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
    if (!weaponMeshIsReplacement)
        OrcApplyWeaponStockRemapToObject(ped, wt, object);
    if (customAsset && weaponMeshIsReplacement)
        OrcApplyWeaponTextureAssetTxdRemapVariants(ped, wt, object, customAsset);
    OrcApplyWeaponTextureToRwObject(object, customAsset);
}

static int PopWeaponReplacementRandomChoice(const std::string& bagPoolKey, const std::vector<int>& sourcePool) {
    if (sourcePool.empty())
        return -2;
    std::vector<int>& bag = g_weaponReplacementRandomBags[bagPoolKey];
    if (bag.empty()) {
        bag = sourcePool;
        if (g_weaponReplacementRandomIncludeVanilla)
            bag.push_back(kWeaponReplacementVanillaChoice);
        for (int i = (int)bag.size() - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(bag[(size_t)i], bag[(size_t)j]);
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
    PruneRandomChoicesForDeadPeds(
        g_weaponReplacementRandomChoiceByPed,
        g_weaponReplacementSequentialStates,
        g_weaponReplacementNoRepeatStates);
    const int pedRef = OrcSafeGetPedRef(ped);
    if (pedRef <= 0)
        return PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool);
    const std::string choiceKey = std::to_string(pedRef) + "|" + stickySuffix;
    auto chosen = g_weaponReplacementRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponReplacementRandomChoiceByPed.end()) {
        if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL)
            g_weaponReplacementSequentialStates[bagPoolKey].activeChoiceKeys.insert(choiceKey);
        else if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT)
            g_weaponReplacementNoRepeatStates[bagPoolKey].activeChoiceKeys.insert(choiceKey);
        return chosen->second;
    }
    if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_SEQUENTIAL) {
        const std::vector<int> sequentialPool = BuildWeaponReplacementSequentialPool(sourcePool);
        return OrcSequentialPickSticky(
            g_weaponReplacementSequentialStates[bagPoolKey],
            g_weaponReplacementRandomChoiceByPed,
            choiceKey,
            sequentialPool,
            -2);
    }
    if (g_weaponReplacementRandomPickMode == ORC_RANDOM_PICK_RANDOM_NO_REPEAT) {
        const std::vector<int> noRepeatPool = BuildWeaponReplacementSequentialPool(sourcePool);
        return OrcRandomNoRepeatPickSticky(
            g_weaponReplacementNoRepeatStates[bagPoolKey],
            g_weaponReplacementRandomChoiceByPed,
            choiceKey,
            noRepeatPool,
            -2);
    }
    const int pick = PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool);
    int finalPick = pick;
    auto avoidIt = g_weaponReplacementRerollAvoidChoiceByPed.find(choiceKey);
    if (avoidIt != g_weaponReplacementRerollAvoidChoiceByPed.end() &&
        WeaponReplacementChoiceMatchesSnapshot(finalPick, avoidIt->second)) {
        std::vector<int> skipped;
        skipped.push_back(finalPick);
        const size_t maxAttempts = sourcePool.size() + (g_weaponReplacementRandomIncludeVanilla ? 1u : 0u);
        for (size_t attempt = 0; attempt < maxAttempts; ++attempt) {
            const int alt = PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool);
            if (alt == -2)
                break;
            if (!WeaponReplacementChoiceMatchesSnapshot(alt, avoidIt->second)) {
                finalPick = alt;
                break;
            }
            skipped.push_back(alt);
        }
        std::vector<int>& bag = g_weaponReplacementRandomBags[bagPoolKey];
        for (int skippedPick : skipped)
            bag.push_back(skippedPick);
        g_weaponReplacementRerollAvoidChoiceByPed.erase(avoidIt);
    }
    if (finalPick != -2)
        g_weaponReplacementRandomChoiceByPed[choiceKey] = finalPick;
    return finalPick;
}

WeaponReplacementAsset* OrcResolveWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
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
                return forced;
        }
    }
    const std::vector<std::string> weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return nullptr;

    if (samp_bridge::IsSampBuildKnown()) {
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
                        return &g_weaponReplacementAssets[(size_t)nickIt->second];
                    }
                }
            }
        }
    }

    if (allowRandom) {
        const std::string skinLower = OrcToLowerAscii(GetPedStdSkinDffName(ped));
        for (const std::string& weaponName : weaponNames) {
            // CJ can have no base skin DFF. The weapon pool only needs the weapon name.
            if (!skinLower.empty()) {
                const std::string skinKey = MakeWeaponReplacementKey(weaponName, skinLower);
                auto skinIt = g_weaponReplacementRandomBySkin.find(skinKey);
                if (skinIt != g_weaponReplacementRandomBySkin.end() && !skinIt->second.empty()) {
                    const int pick = PickStickyWeaponReplacementChoice(ped, "sr|" + skinKey, skinKey, skinIt->second);
                    if (pick == kWeaponReplacementVanillaChoice)
                        return nullptr;
                    if (pick >= 0 && pick < (int)g_weaponReplacementAssets.size())
                        return &g_weaponReplacementAssets[(size_t)pick];
                }
            }
            auto wIt = g_weaponReplacementRandomByWeapon.find(weaponName);
            if (wIt != g_weaponReplacementRandomByWeapon.end() && !wIt->second.empty()) {
                const std::string bagKey = std::string("w|") + weaponName;
                const int pick = PickStickyWeaponReplacementChoice(ped, "wr|" + weaponName, bagKey, wIt->second);
                if (pick == kWeaponReplacementVanillaChoice)
                    return nullptr;
                if (pick >= 0 && pick < (int)g_weaponReplacementAssets.size())
                    return &g_weaponReplacementAssets[(size_t)pick];
            }
        }
    }
    return nullptr;
}

WeaponReplacementAsset* OrcResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponReplacementAsset* asset = OrcResolveWeaponReplacementAssetForPed(ped, wt, allowRandom);
    if (!asset)
        return nullptr;
    if (!EnsureWeaponReplacementAssetLoaded(*asset)) {
        if (asset->key.rfind("skinrandom:", 0) == 0 || asset->key.rfind("wprand:", 0) == 0) {
            const int failedIndex = (int)(asset - g_weaponReplacementAssets.data());
            for (auto it = g_weaponReplacementRandomChoiceByPed.begin(); it != g_weaponReplacementRandomChoiceByPed.end();) {
                if (it->second == failedIndex) {
                    EraseSequentialChoiceKeyFromStates(g_weaponReplacementSequentialStates, it->first);
                    ReleaseNoRepeatChoiceKeyFromStates(g_weaponReplacementNoRepeatStates, g_weaponReplacementRandomChoiceByPed, it->first, false);
                    it = g_weaponReplacementRandomChoiceByPed.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return nullptr;
    }
    return asset;
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
    const std::vector<std::string> weaponNames = WeaponLookupNamesLower(wt);
    if (weaponNames.empty())
        return false;

    const std::string keyLower = OrcToLowerAscii(replacementKey);
    int assetIndex = -1;
    for (int i = 0; i < (int)g_weaponReplacementAssets.size(); ++i) {
        if (OrcToLowerAscii(g_weaponReplacementAssets[(size_t)i].key) == keyLower) {
            assetIndex = i;
            break;
        }
    }
    if (assetIndex < 0)
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
    const std::vector<std::string> weaponNames = WeaponLookupNamesLower(wt);
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
    ClearWeaponStockRemapRuntime();
    DestroyWeaponReplacementAssets();
    DestroyWeaponTextureAssets();
}

bool OrcWeaponReplacementIsStickyVanillaChoice(CPed* ped, int wt) {
    if (!ped || wt <= 0)
        return false;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return false;
    const std::vector<std::string> weaponNames = WeaponLookupNamesLower(wt);
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

