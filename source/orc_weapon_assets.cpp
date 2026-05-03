#include "plugin.h"

#include "CPed.h"
#include "CPools.h"
#include "CTxdStore.h"
#include "RenderWare.h"

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
static std::unordered_map<std::string, int> g_weaponReplacementBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBySkin;
static std::unordered_map<std::string, std::vector<int>> g_weaponReplacementRandomBags;
static std::unordered_map<std::string, int> g_weaponReplacementRandomChoiceByPed;
static WeaponReplacementStats g_weaponReplacementStats;

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
    g_weaponReplacementBySkin.clear();
    g_weaponReplacementRandomBySkin.clear();
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
                                      std::unordered_map<std::string, std::vector<int>>* randomMap) {
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
    if (randomMap)
        (*randomMap)[mapKey].push_back(index);
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
                        const std::string skinLower = OrcToLowerAscii(base);
                        AddWeaponReplacementAsset(
                            "skin:" + weaponLower + ":" + skinLower,
                            weaponLower,
                            skinLower,
                            weaponFolder + "/" + base,
                            OrcJoinPath(weaponDir, fname),
                            OrcFindBestTxdPath(weaponDir, base),
                            &g_weaponReplacementBySkin,
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
                                &g_weaponReplacementRandomBySkin);
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
                    nullptr);
            } while (FindNextFileA(hn, &nickData));
            FindClose(hn);
        }
    }

    g_weaponReplacementStats.uniqueSkinWeapons = (int)g_weaponReplacementBySkin.size();
    g_weaponReplacementStats.randomSkinWeapons = 0;
    for (const auto& kv : g_weaponReplacementRandomBySkin)
        g_weaponReplacementStats.randomSkinWeapons += (int)kv.second.size();
    g_weaponReplacementStats.nickWeapons = (int)g_weaponReplacementByNick.size();
    OrcLogInfo("DiscoverWeaponReplacements: skin=%d random=%d nick=%d",
        g_weaponReplacementStats.uniqueSkinWeapons,
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
static WeaponTextureStats g_weaponTextureStats;

static RwTexDictionary* GetTxdDictionaryByIndexMain(int txdIndex) {
    if (txdIndex < 0 || !CTxdStore::ms_pTxdPool || !CTxdStore::ms_pTxdPool->m_pObjects)
        return nullptr;
    if (txdIndex >= CTxdStore::ms_pTxdPool->m_nSize)
        return nullptr;
    return CTxdStore::ms_pTxdPool->m_pObjects[txdIndex].m_pRwDictionary;
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

static void DestroyWeaponTextureAssets() {
    OrcRestoreWeaponTextureOverrides();
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
    DestroyWeaponTextureAssets();
    const std::string texturesDir = g_gameWeaponTexturesDir;
    DWORD attr = GetFileAttributesA(texturesDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;

    std::string weaponMask = OrcJoinPath(texturesDir, "*");
    WIN32_FIND_DATAA weaponData{};
    HANDLE hw = FindFirstFileA(weaponMask.c_str(), &weaponData);
    if (hw != INVALID_HANDLE_VALUE) {
        do {
            if (!(weaponData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::string weaponFolder = weaponData.cFileName;
            if (weaponFolder == "." || weaponFolder == ".." || _stricmp(weaponFolder.c_str(), "Nick") == 0)
                continue;
            const std::string weaponLower = OrcToLowerAscii(weaponFolder);
            const std::string weaponDir = OrcJoinPath(texturesDir, weaponFolder);

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
    const std::string nickDir = OrcJoinPath(texturesDir, "Nick");
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

    g_weaponTextureStats.uniqueSkinTextures = (int)g_weaponTextureBySkin.size();
    g_weaponTextureStats.randomSkinTextures = 0;
    for (const auto& kv : g_weaponTextureRandomBySkin)
        g_weaponTextureStats.randomSkinTextures += (int)kv.second.size();
    g_weaponTextureStats.nickTextures = (int)g_weaponTextureByNick.size();
    OrcLogInfo("DiscoverWeaponTextures: skin=%d random=%d nick=%d",
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

static WeaponTextureAsset* ResolveWeaponTextureAssetForPed(CPed* ped, int wt, bool allowRandom) {
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

    const std::string skinLower = OrcToLowerAscii(GetPedStdSkinDffName(ped));
    if (skinLower.empty())
        return nullptr;
    const std::string skinKey = MakeWeaponReplacementKey(weaponLower, skinLower);
    auto skinIt = g_weaponTextureBySkin.find(skinKey);
    if (skinIt != g_weaponTextureBySkin.end() &&
        skinIt->second >= 0 && skinIt->second < (int)g_weaponTextureAssets.size()) {
        return &g_weaponTextureAssets[(size_t)skinIt->second];
    }
    if (allowRandom && g_weaponTextureRandomMode)
        return PickStickyRandomWeaponTextureAsset(ped, skinKey);
    return nullptr;
}

WeaponTextureAsset* OrcResolveUsableWeaponTextureAssetForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponTextureAsset* asset = ResolveWeaponTextureAssetForPed(ped, wt, allowRandom);
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
    g_weaponTextureRestoreEntries.push_back({ material, material->texture });
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

static WeaponReplacementAsset* PickRandomWeaponReplacementAsset(const std::string& mapKey) {
    auto it = g_weaponReplacementRandomBySkin.find(mapKey);
    if (it == g_weaponReplacementRandomBySkin.end() || it->second.empty())
        return nullptr;
    std::vector<int>& bag = g_weaponReplacementRandomBags[mapKey];
    if (bag.empty()) {
        bag = it->second;
        for (int i = (int)bag.size() - 1; i > 0; --i) {
            const int j = rand() % (i + 1);
            std::swap(bag[(size_t)i], bag[(size_t)j]);
        }
    }
    const int assetIndex = bag.back();
    bag.pop_back();
    if (assetIndex < 0 || assetIndex >= (int)g_weaponReplacementAssets.size())
        return nullptr;
    return &g_weaponReplacementAssets[(size_t)assetIndex];
}

static WeaponReplacementAsset* PickStickyRandomWeaponReplacementAsset(CPed* ped, const std::string& mapKey) {
    if (!ped)
        return nullptr;
    const int pedRef = CPools::GetPedRef(ped);
    if (pedRef <= 0)
        return PickRandomWeaponReplacementAsset(mapKey);

    const std::string choiceKey = std::to_string(pedRef) + "|" + mapKey;
    auto chosen = g_weaponReplacementRandomChoiceByPed.find(choiceKey);
    if (chosen != g_weaponReplacementRandomChoiceByPed.end()) {
        const int assetIndex = chosen->second;
        if (assetIndex >= 0 && assetIndex < (int)g_weaponReplacementAssets.size())
            return &g_weaponReplacementAssets[(size_t)assetIndex];
    }

    WeaponReplacementAsset* asset = PickRandomWeaponReplacementAsset(mapKey);
    if (!asset)
        return nullptr;
    const int assetIndex = (int)(asset - g_weaponReplacementAssets.data());
    if (assetIndex >= 0)
        g_weaponReplacementRandomChoiceByPed[choiceKey] = assetIndex;
    return asset;
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
    auto skinIt = g_weaponReplacementBySkin.find(skinKey);
    if (skinIt != g_weaponReplacementBySkin.end() &&
        skinIt->second >= 0 && skinIt->second < (int)g_weaponReplacementAssets.size()) {
        return &g_weaponReplacementAssets[(size_t)skinIt->second];
    }
    if (allowRandom)
        return PickStickyRandomWeaponReplacementAsset(ped, skinKey);
    return nullptr;
}

WeaponReplacementAsset* OrcResolveUsableWeaponReplacementAssetForPed(CPed* ped, int wt, bool allowRandom) {
    WeaponReplacementAsset* asset = OrcResolveWeaponReplacementAssetForPed(ped, wt, allowRandom);
    if (!asset)
        return nullptr;
    if (!EnsureWeaponReplacementAssetLoaded(*asset)) {
        if (asset->key.rfind("skinrandom:", 0) == 0) {
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
    DestroyWeaponReplacementAssets();
    DestroyWeaponTextureAssets();
}

size_t OrcWeaponAssetsDbgReplacementSkinKeys() {
    return g_weaponReplacementBySkin.size();
}

size_t OrcWeaponAssetsDbgReplacementNickKeys() {
    return g_weaponReplacementByNick.size();
}

size_t OrcWeaponAssetsDbgRandomReplacementPools() {
    return g_weaponReplacementRandomBySkin.size();
}

