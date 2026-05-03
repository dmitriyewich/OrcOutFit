#include "orc_weapons.h"

#include "plugin.h"
#include "CWeaponInfo.h"
#include "eWeaponType.h"

#include "orc_log.h"

#include "external/MinHook/include/MinHook.h"

#include <cstring>
#include <cstdlib>

namespace {

bool g_weaponDatHookInstalled = false;
int(__cdecl* g_LoadWeaponObject_Orig)(const char* line) = nullptr;

// SA `CFileLoader::LoadWeaponObject` receives a processed line, not raw weapon.dat:
//   "<modelId> <dffName> <txdName> ..."  e.g. "346 colt45 colt45 colt45 1 30 0"
int __cdecl LoadWeaponObject_Detour(const char* line) {
    int modelId = 0;
    if (g_LoadWeaponObject_Orig) modelId = g_LoadWeaponObject_Orig(line);

    if (!line) return modelId;

    const char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    char* endNum = nullptr;
    const long parsedId = strtol(p, &endNum, 10);
    const int idFromLine = (endNum != p && parsedId > 0) ? (int)parsedId : 0;

    char dff[96] = {};
    p = endNum;
    while (*p == ' ' || *p == '\t') ++p;
    int di = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && di < (int)sizeof(dff) - 1)
        dff[di++] = *p++;
    dff[di] = 0;

    const int resolvedModel = (modelId > 0) ? modelId : idFromLine;
    if (resolvedModel <= 0) return modelId;

    int wt = WEAPONTYPE_UNARMED;
    if (dff[0]) {
        wt = (int)CWeaponInfo::FindWeaponType(dff);
        if (wt <= 0) {
            char up[96];
            strncpy_s(up, dff, _TRUNCATE);
            for (char* c = up; *c; ++c) {
                if (*c >= 'a' && *c <= 'z') *c = (char)(*c - ('a' - 'A'));
            }
            wt = (int)CWeaponInfo::FindWeaponType(up);
        }
    }

    if (wt <= 0) {
        __try {
            for (int t = 1; t <= 255; ++t) {
                CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo((eWeaponType)t, 1);
                if (!wi) continue;
                if (wi->m_nModelId == resolvedModel || wi->m_nModelId2 == resolvedModel) {
                    wt = t;
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            const int cap = MAX_WEAPON_INFOS - 1;
            __try {
                for (int t = 1; t <= cap; ++t) {
                    if (aWeaponInfo[t].m_nModelId == resolvedModel || aWeaponInfo[t].m_nModelId2 == resolvedModel) {
                        wt = t;
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    if (wt > 0 && wt <= 255) {
        if ((int)g_weaponDatModelId.size() <= wt) g_weaponDatModelId.resize(wt + 1, 0);
        g_weaponDatModelId[wt] = resolvedModel;
        if (dff[0]) {
            if ((int)g_weaponDatIdeName.size() <= wt) g_weaponDatIdeName.resize(wt + 1);
            g_weaponDatIdeName[wt] = dff;
        }
    }

    return modelId;
}

} // namespace

std::vector<int> g_weaponDatModelId;
std::vector<std::string> g_weaponDatIdeName;

void OrcWeaponsEnsureWeaponDatHookInstalled() {
    if (g_weaponDatHookInstalled) return;
    g_weaponDatHookInstalled = true;
    g_weaponDatModelId.assign(256 + 1, 0);
    g_weaponDatIdeName.assign(256 + 1, {});

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("LoadWeaponObject hook: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(0x5B3FB0),
                      reinterpret_cast<void*>(&LoadWeaponObject_Detour),
                      reinterpret_cast<void**>(&g_LoadWeaponObject_Orig)) != MH_OK) {
        OrcLogError("LoadWeaponObject hook: MH_CreateHook failed");
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(0x5B3FB0));
    if (st != MH_OK)
        OrcLogError("LoadWeaponObject hook: MH_EnableHook -> %s", MH_StatusToString(st));
    else
        OrcLogInfo("LoadWeaponObject hook installed (0x5B3FB0)");
}
