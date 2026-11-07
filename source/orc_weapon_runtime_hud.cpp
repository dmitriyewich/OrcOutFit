#include "plugin.h"

#include "CPed.h"
#include "CPlayerPed.h"

#include <iterator>

#include "external/MinHook/include/MinHook.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_runtime.h"

namespace {
constexpr uintptr_t kDrawWeaponIcon = 0x58D7D0;
constexpr uintptr_t kFindWeaponIconCall = 0x58D84B;
constexpr uintptr_t kFindHashNamedTexture = 0x734E50;

void(__cdecl* g_drawWeaponIconOriginal)(CPed*, int, int, float) = nullptr;
CPed* g_localHudPed = nullptr;
bool g_weaponIconHookInstalled = false;

uintptr_t ReadWeaponIconLookupTarget() {
    if (plugin::patch::GetUChar(kFindWeaponIconCall) != 0xE8)
        return 0;
    return kFindWeaponIconCall + 5 + plugin::patch::GetInt(kFindWeaponIconCall + 1);
}

RwTexture* __cdecl FindWeaponIconForHud(RwTexDictionary* dict, unsigned int hash) {
    CPed* ped = g_localHudPed;
    if (ped && ped->m_nSelectedWepSlot < std::size(ped->m_aWeapons)) {
        const int weaponType = ped->m_aWeapons[ped->m_nSelectedWepSlot].m_eWeaponType;
        if (RwTexture* texture = OrcWeaponHudResolveIconTexture(ped, weaponType, hash))
            return texture;
    }
    return RwTexDictionaryFindHashNamedTexture(dict, hash);
}

void __cdecl CHud_DrawWeaponIcon_Detour(CPed* ped, int x, int y, float alpha) {
    CPed* previous = g_localHudPed;
    g_localHudPed = nullptr;
    if (g_enabled && g_weaponHudIconFromGunsTxd && (g_weaponTexturesEnabled || g_weaponReplacementEnabled) &&
        ped && ped == FindPlayerPed(0)) {
        g_localHudPed = ped;
    }
    g_drawWeaponIconOriginal(ped, x, y, alpha);
    g_localHudPed = previous;
}
}  // namespace

void OrcWeaponHudEnsureDrawWeaponIconHookInstalled() {
    if (g_weaponIconHookInstalled)
        return;

    // Only replace the verified call inside DrawWeaponIcon. DrawCrossHairs also uses the hash lookup.
    if (ReadWeaponIconLookupTarget() != kFindHashNamedTexture) {
        OrcLogError("HUD weapon icon: unexpected call at 0x%08X", static_cast<unsigned>(kFindWeaponIconCall));
        return;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("HUD weapon icon: MH_Initialize -> %s", MH_StatusToString(status));
        return;
    }

    void* entry = reinterpret_cast<void*>(kDrawWeaponIcon);
    status = MH_CreateHook(entry, reinterpret_cast<void*>(&CHud_DrawWeaponIcon_Detour),
        reinterpret_cast<void**>(&g_drawWeaponIconOriginal));
    if (status != MH_OK) {
        OrcLogError("HUD weapon icon: MH_CreateHook -> %s", MH_StatusToString(status));
        return;
    }
    status = MH_EnableHook(entry);
    if (status != MH_OK) {
        MH_RemoveHook(entry);
        OrcLogError("HUD weapon icon: MH_EnableHook -> %s", MH_StatusToString(status));
        return;
    }

    plugin::patch::RedirectCall(kFindWeaponIconCall, reinterpret_cast<void*>(&FindWeaponIconForHud));
    if (ReadWeaponIconLookupTarget() != reinterpret_cast<uintptr_t>(&FindWeaponIconForHud)) {
        MH_RemoveHook(entry);
        OrcLogError("HUD weapon icon: call patch failed at 0x%08X", static_cast<unsigned>(kFindWeaponIconCall));
        return;
    }
    g_weaponIconHookInstalled = true;
    OrcLogInfo("HUD weapon icon: native hash lookup hooked at 0x%08X", static_cast<unsigned>(kFindWeaponIconCall));
}
