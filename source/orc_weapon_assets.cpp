#include "plugin.h"

#include "CPed.h"
#include "CPools.h"
#include "CWeaponInfo.h"
#include "CModelInfo.h"
#include "CTxdStore.h"
#include "RenderWare.h"
#include "eWeaponType.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "samp_bridge.h"

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_types.h"
#include "orc_weapon_assets.h"
#include "orc_weapons.h"

using namespace plugin;

void OrcClearAllWeaponReplacementInstances();
static std::vector<WeaponReplacementAsset> g_weaponReplacementAssets;
static std::unordered_map<std::string, int> g_weaponReplacementByNick;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomByWeapon;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBags;
static std::unordered_map<std::string, int> g_weaponReplacementRandomChoiceByPed;
static WeaponReplacementStats g_weaponReplacementStats;

static constexpr int kWeaponReplacementVanillaChoice = -1;

static std::string MakeWeaponReplacementKey(const std::string& weaponLower, const std::string& matchLower) {
    return weaponLower + "|" + matchLower;
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
        std::string name = OrcGetWeaponModelBaseNameLower(wt);
        if (!name.empty())
            names.push_back(name);
        if (wt > 0 && wt < (int)g_cfg.size() && g_cfg[(size_t)wt].name && g_cfg[(size_t)wt].name[0])
            names.push_back(OrcToLowerAscii(g_cfg[(size_t)wt].name));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() > b.size();
        return a < b;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}



static void DestroyWeaponReplacementAssets() {
    for (auto& asset : g_weaponReplacementAssets) {
        OrcDestroyRwObjectInstance(asset.rwObject);
        asset.txdSlot = -1;
    }
    g_weaponReplacementAssets.clear();
    g_weaponReplacementByNick.clear();
    g_weaponReplacementRandomBySkin.clear();
    g_weaponReplacementRandomByWeapon.clear();
    g_weaponReplacementRandomBags.clear();
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
            RpClumpForAllAtomics(c, OrcInitAttachmentAtomicCB, nullptr);
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
                    OrcInitAttachmentAtomicCB(a, nullptr);
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

RwObject* OrcCloneWeaponReplacementObject(WeaponReplacementAsset& asset) {
    if (!EnsureWeaponReplacementAssetLoaded(asset) || !asset.rwObject)
        return nullptr;
    if (asset.rwObject->type == rpCLUMP) {
        RpClump* clone = RpClumpClone(reinterpret_cast<RpClump*>(asset.rwObject));
        if (!clone) return nullptr;
        RpClumpForAllAtomics(clone, OrcInitAttachmentAtomicCB, nullptr);
        return reinterpret_cast<RwObject*>(clone);
    }
    if (asset.rwObject->type == rpATOMIC) {
        RpAtomic* clone = RpAtomicClone(reinterpret_cast<RpAtomic*>(asset.rwObject));
        if (!clone) return nullptr;
        RwFrame* frame = RwFrameCreate();
        if (frame) RpAtomicSetFrame(clone, frame);
        OrcInitAttachmentAtomicCB(clone, nullptr);
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

void DiscoverWeaponReplacements() {
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

    g_weaponReplacementStats.randomWeaponWeapons = 0;
    for (const auto& kv : g_weaponReplacementRandomByWeapon)
        g_weaponReplacementStats.randomWeaponWeapons += (int)kv.second.size();
    g_weaponReplacementStats.randomSkinWeapons = 0;
    for (const auto& kv : g_weaponReplacementRandomBySkin)
        g_weaponReplacementStats.randomSkinWeapons += (int)kv.second.size();
    g_weaponReplacementStats.nickWeapons = (int)g_weaponReplacementByNick.size();
    OrcLogInfo("DiscoverWeaponReplacements: weaponRandom=%d skinRandom=%d nick=%d",
        g_weaponReplacementStats.randomWeaponWeapons,
        g_weaponReplacementStats.randomSkinWeapons,
        g_weaponReplacementStats.nickWeapons);
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
    if (txdIndex < 0 || !CTxdStore::ms_pTxdPool || !CTxdStore::ms_pTxdPool->m_pObjects)
        return nullptr;
    if (txdIndex >= CTxdStore::ms_pTxdPool->m_nSize)
        return nullptr;
    return CTxdStore::ms_pTxdPool->m_pObjects[txdIndex].m_pRwDictionary;
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
    g_weaponTextureRandomChoiceByPed.clear();
    g_weaponTextureStats = {};
}

static bool EnsureWeaponTextureAssetLoaded(WeaponTextureAsset& asset) {
    if (asset.txdSlot >= 0 && GetTxdDictionaryByIndexMain(asset.txdSlot))
        return true;
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
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return PickRandomWeaponTextureAsset(mapKey);

    const std::string choiceKey = std::to_string(pedRef) + "|" + mapKey;
    auto chosen = g_weaponTextureRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponTextureRandomChoiceByPed.end()) {
        const int assetIndex = chosen->second;
        if (assetIndex >= 0 && assetIndex < (int)g_weaponTextureAssets.size())
            return &g_weaponTextureAssets[(size_t)assetIndex];
    }

    WeaponTextureAsset* asset = PickRandomWeaponTextureAsset(mapKey);
    if (!asset)
        return nullptr;
    const int assetIndex = (int)(asset - g_weaponTextureAssets.data());
    if (assetIndex >= 0)
        g_weaponTextureRandomChoiceByPed[choiceKey] = assetIndex;
    return asset;
}

/// `wprand:<weapon_folder>:<dff_basename>` (e.g. `wprand:desert_eagle:markvii`) — same match key as
/// `Weapons\Guns\<weapon>\<basename>.txd` indexed under `weapon|basename`.
static bool WeaponTextureParseWprandReplacementKey(const std::string& weaponLowerExpected,
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
    if (weaponPart != weaponLowerExpected)
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
    const std::string weaponLower = OrcGetWeaponModelBaseNameLower(wt);
    if (weaponLower.empty())
        return nullptr;

    if (g_weaponTextureNickMode && samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = OrcToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                const std::string nickKey = MakeWeaponReplacementKey(weaponLower, nickLower);
                auto nickIt = g_weaponTextureByNick.find(nickKey);
                if (nickIt != g_weaponTextureByNick.end() &&
                    nickIt->second >= 0 && nickIt->second < (int)g_weaponTextureAssets.size()) {
                    return &g_weaponTextureAssets[(size_t)nickIt->second];
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
        if (WeaponTextureParseWprandReplacementKey(weaponLower, *wprandKeyPtr, &variantLower)) {
            const std::string replTexKey = MakeWeaponReplacementKey(weaponLower, variantLower);
            auto replIt = g_weaponTextureBySkin.find(replTexKey);
            if (replIt != g_weaponTextureBySkin.end() &&
                replIt->second >= 0 && replIt->second < (int)g_weaponTextureAssets.size()) {
                OrcLogInfoThrottled(402, 10000u,
                    "weapon texture: Guns\\%s\\%s.txd via replacement key \"%s\" (wt=%d)",
                    weaponLower.c_str(),
                    variantLower.c_str(),
                    wprandKeyPtr->c_str(),
                    wt);
                return &g_weaponTextureAssets[(size_t)replIt->second];
            }
        }
    }

    const std::string skinLowerRaw = OrcToLowerAscii(GetPedStdSkinDffName(ped));
    // `Weapons\Guns\<weapon>\<weapon>.txd` is indexed as match key `<weapon>|<weapon>` (same string twice).
    // Used when ped skin has no `<weapon>\<dff>.txd` entry (many packs ship one bundle as desert_eagle\desert_eagle.txd).
    const std::string defaultWeaponSkinKey = MakeWeaponReplacementKey(weaponLower, weaponLower);

    if (!skinLowerRaw.empty()) {
        const std::string skinKey = MakeWeaponReplacementKey(weaponLower, skinLowerRaw);
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

    auto defIt = g_weaponTextureBySkin.find(defaultWeaponSkinKey);
    if (defIt != g_weaponTextureBySkin.end() &&
        defIt->second >= 0 && defIt->second < (int)g_weaponTextureAssets.size()) {
        OrcLogInfoThrottled(401, 8000u,
            "weapon texture: using default Guns\\%s\\%s.txd (wt=%d pedSkin=\"%s\")",
            weaponLower.c_str(),
            weaponLower.c_str(),
            wt,
            GetPedStdSkinDffName(ped).c_str());
        return &g_weaponTextureAssets[(size_t)defIt->second];
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
            if (it->second == failedIndex)
                it = g_weaponTextureRandomChoiceByPed.erase(it);
            else
                ++it;
        }
    }
    return nullptr;
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

static int PickStickyWeaponReplacementChoice(CPed* ped,
    const std::string& stickySuffix,
    const std::string& bagPoolKey,
    const std::vector<int>& sourcePool) {
    if (sourcePool.empty())
        return -2;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool);
    const std::string choiceKey = std::to_string(pedRef) + "|" + stickySuffix;
    auto chosen = g_weaponReplacementRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponReplacementRandomChoiceByPed.end())
        return chosen->second;
    const int pick = PopWeaponReplacementRandomChoice(bagPoolKey, sourcePool);
    if (pick != -2)
        g_weaponReplacementRandomChoiceByPed[choiceKey] = pick;
    return pick;
}

WeaponReplacementAsset* OrcResolveWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
    if (!g_weaponReplacementEnabled || !ped || wt <= 0)
        return nullptr;
    const std::string weaponLower = OrcGetWeaponModelBaseNameLower(wt);
    if (weaponLower.empty())
        return nullptr;

    if (samp_bridge::IsSampBuildKnown()) {
        char nick[64] = {};
        bool isLocal = false;
        if (samp_bridge::GetPedNickname(ped, nick, sizeof(nick), &isLocal)) {
            const std::string nickLower = OrcToLowerAscii(StripSampColorCodes(nick));
            if (!nickLower.empty()) {
                const std::string nickKey = MakeWeaponReplacementKey(weaponLower, nickLower);
                auto nickIt = g_weaponReplacementByNick.find(nickKey);
                if (nickIt != g_weaponReplacementByNick.end() &&
                    nickIt->second >= 0 && nickIt->second < (int)g_weaponReplacementAssets.size()) {
                    return &g_weaponReplacementAssets[(size_t)nickIt->second];
                }
            }
        }
    }

    const std::string skinLower = OrcToLowerAscii(GetPedStdSkinDffName(ped));
    if (skinLower.empty())
        return nullptr;
    const std::string skinKey = MakeWeaponReplacementKey(weaponLower, skinLower);
    if (allowRandom) {
        auto skinIt = g_weaponReplacementRandomBySkin.find(skinKey);
        if (skinIt != g_weaponReplacementRandomBySkin.end() && !skinIt->second.empty()) {
            const int pick = PickStickyWeaponReplacementChoice(ped, "sr|" + skinKey, skinKey, skinIt->second);
            if (pick == kWeaponReplacementVanillaChoice)
                return nullptr;
            if (pick >= 0 && pick < (int)g_weaponReplacementAssets.size())
                return &g_weaponReplacementAssets[(size_t)pick];
        }
        auto wIt = g_weaponReplacementRandomByWeapon.find(weaponLower);
        if (wIt != g_weaponReplacementRandomByWeapon.end() && !wIt->second.empty()) {
            const std::string bagKey = std::string("w|") + weaponLower;
            const int pick = PickStickyWeaponReplacementChoice(ped, "wr|" + weaponLower, bagKey, wIt->second);
            if (pick == kWeaponReplacementVanillaChoice)
                return nullptr;
            if (pick >= 0 && pick < (int)g_weaponReplacementAssets.size())
                return &g_weaponReplacementAssets[(size_t)pick];
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
                if (it->second == failedIndex)
                    it = g_weaponReplacementRandomChoiceByPed.erase(it);
                else
                    ++it;
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
    std::string weaponLower = OrcGetWeaponModelBaseNameLower(wt);
    if (weaponLower.empty())
        return false;
    const std::string pedPrefix = std::to_string(pedRef) + "|";
    const std::string skinRaw = GetPedStdSkinDffName(ped);
    if (!skinRaw.empty()) {
        const std::string skinKey = MakeWeaponReplacementKey(weaponLower, OrcToLowerAscii(skinRaw));
        auto itSkin = g_weaponReplacementRandomChoiceByPed.find(pedPrefix + "sr|" + skinKey);
        if (itSkin != g_weaponReplacementRandomChoiceByPed.end() &&
            itSkin->second == kWeaponReplacementVanillaChoice)
            return true;
    }
    auto itW = g_weaponReplacementRandomChoiceByPed.find(pedPrefix + "wr|" + weaponLower);
    return itW != g_weaponReplacementRandomChoiceByPed.end() &&
        itW->second == kWeaponReplacementVanillaChoice;
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

