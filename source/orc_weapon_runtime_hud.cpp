#include "plugin.h"

#include "CPlayerPed.h"
#include "CPed.h"
#include "CPools.h"
#include "CHud.h"
#include "CSprite2d.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#include "CClumpModelInfo.h"
#include "CModelInfo.h"
#include "CTimer.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"
#include "eWeaponType.h"
#include "CEntity.h"
#include "CWeapon.h"
#include "CVector.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "external/MinHook/include/MinHook.h"

#include "orc_app.h"
#include "orc_attach.h"
#include "orc_log.h"
#include "orc_path.h"
#include "orc_render.h"
#include "orc_types.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_runtime.h"

#include "samp_bridge.h"
using namespace plugin;

// ---------------------------------------------------------------------------
// HUD weapon icon (local ped): `CHud::DrawAmmo` / `DrawWeaponIcon` push Orc TXD scope; `CSprite2d::SetTexture` (+mask)
// resolves `*icon` from that dictionary. `OrcWeaponHudTryRwTexDictionaryFindOverride` + multi-raster borrow across TXD
// slots in `orc_weapon_assets.cpp` covers SA:MP clients that cache sprites / split icons across dictionaries.
// ---------------------------------------------------------------------------

static bool g_drawWeaponIconHookInstalled = false;
static bool g_hudSprTexMonoHookInstalled = false;
static bool g_hudSprTexMaskHookInstalled = false;
static void(__cdecl* g_CHud_DrawWeaponIcon_Orig)(CPed*, int, int, float) = nullptr;
using Sprite2dSetTexture_orig_fn = void(__thiscall*)(CSprite2d*, char*);
static Sprite2dSetTexture_orig_fn g_CSprite2d_SetTexture_Orig = nullptr;
using Sprite2dSetTextureMasked_orig_fn = void(__thiscall*)(CSprite2d*, char*, char*);
static Sprite2dSetTextureMasked_orig_fn g_CSprite2d_SetTextureMasked_Orig = nullptr;

// Non-null while inside `DrawAmmo` / `DrawWeaponIcon` when Orc Guns/replacement dict resolves for local ped.
static RwTexDictionary* g_hudWeaponOrcTexDictDuringDrawWeaponIcon = nullptr;
static RwTexDictionary* g_hudOrcSpriteScopeStack[8];
static unsigned g_hudOrcSpriteScopeStackSz = 0;

static void HudPushOrcSpriteDictScope(RwTexDictionary* d) {
    if (!d || g_hudOrcSpriteScopeStackSz >= 8u)
        return;
    g_hudOrcSpriteScopeStack[g_hudOrcSpriteScopeStackSz++] = d;
    g_hudWeaponOrcTexDictDuringDrawWeaponIcon = d;
}

static void HudPopOrcSpriteDictScope() {
    if (!g_hudOrcSpriteScopeStackSz)
        return;
    --g_hudOrcSpriteScopeStackSz;
    g_hudWeaponOrcTexDictDuringDrawWeaponIcon =
        g_hudOrcSpriteScopeStackSz ? g_hudOrcSpriteScopeStack[g_hudOrcSpriteScopeStackSz - 1u] : nullptr;
}

// `__thiscall` hook entry must use `__fastcall` trampoline (this in ECX); see AddWeaponModel_Hook above.
static void __fastcall CSprite2d_SetTexture_HudWeaponOverlay(CSprite2d* self, void* /*edx*/, char* name) {
    if (!g_CSprite2d_SetTexture_Orig)
        return;
    if (!name || !g_weaponHudIconFromGunsTxd || !g_enabled ||
        !OrcWeaponHudSpriteNamePassesSetTextureConvention(name)) {
        g_CSprite2d_SetTexture_Orig(self, name);
        return;
    }
    RwTexDictionary* interceptDict = OrcWeaponHudGetSampSpriteInterceptDict();
    RwTexDictionary* orcDict = g_hudWeaponOrcTexDictDuringDrawWeaponIcon;
    if (!orcDict)
        orcDict = interceptDict;
    // SA:MP can render the weapon slot before `drawingEvent` fills the intercept cache — build lazily once.
    if (!orcDict) {
        OrcWeaponHudRefreshSampSpriteInterceptCache();
        orcDict = OrcWeaponHudGetSampSpriteInterceptDict();
    }
    if (!orcDict) {
        OrcLogInfoThrottled(
            433,
            6000u,
            "hud icon: SetTexture \"%s\": no Orc dict after lazy refresh — client may not use this hook for ammo icon",
            name);
        g_CSprite2d_SetTexture_Orig(self, name);
        return;
    }
    __try {
        RwTexture* t = OrcWeaponHudResolveSpriteTexture(orcDict, name);
        if (!t) {
            const char* rf = OrcWeaponHudGetIconSpriteRemapFrom();
            const char* rt = OrcWeaponHudGetIconSpriteRemapTo();
            if (rf && rt && _stricmp(name, rf) == 0)
                t = OrcWeaponHudResolveSpriteTexture(orcDict, rt);
        }
        if (t) {
            self->m_pTexture = t;
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    OrcLogInfoThrottled(
        434,
        6000u,
        "hud icon: SetTexture \"%s\": texture not in Orc dict=%p remap %s->%s (falling back to vanilla SetTexture)",
        name,
        orcDict,
        OrcWeaponHudGetIconSpriteRemapFrom() ? OrcWeaponHudGetIconSpriteRemapFrom() : "-",
        OrcWeaponHudGetIconSpriteRemapTo() ? OrcWeaponHudGetIconSpriteRemapTo() : "-");
    g_CSprite2d_SetTexture_Orig(self, name);
}

static void __fastcall CSprite2d_SetTextureMasked_HudWeaponOverlay(CSprite2d* self,
    void* /*edx*/,
    char* name,
    char* maskName) {
    if (!g_CSprite2d_SetTextureMasked_Orig)
        return;
    if (!name || !g_weaponHudIconFromGunsTxd || !g_enabled ||
        !OrcWeaponHudSpriteNamePassesSetTextureConvention(name)) {
        g_CSprite2d_SetTextureMasked_Orig(self, name, maskName);
        return;
    }
    RwTexDictionary* orcDict = g_hudWeaponOrcTexDictDuringDrawWeaponIcon;
    if (!orcDict)
        orcDict = OrcWeaponHudGetSampSpriteInterceptDict();
    if (!orcDict) {
        OrcWeaponHudRefreshSampSpriteInterceptCache();
        orcDict = OrcWeaponHudGetSampSpriteInterceptDict();
    }
    if (!orcDict) {
        OrcLogInfoThrottled(
            436,
            6000u,
            "hud icon: SetTexture(mask) \"%s\": no Orc dict after lazy refresh", name);
        g_CSprite2d_SetTextureMasked_Orig(self, name, maskName);
        return;
    }
    __try {
        RwTexture* t = OrcWeaponHudResolveSpriteTexture(orcDict, name);
        if (!t) {
            const char* rf = OrcWeaponHudGetIconSpriteRemapFrom();
            const char* rt = OrcWeaponHudGetIconSpriteRemapTo();
            if (rf && rt && _stricmp(name, rf) == 0)
                t = OrcWeaponHudResolveSpriteTexture(orcDict, rt);
        }
        if (t) {
            self->m_pTexture = t;
            (void)maskName;
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    OrcLogInfoThrottled(
        442,
        6000u,
        "hud icon: SetTexture(mask) \"%s\": miss Orc dict=%p remap %s->%s",
        name,
        orcDict,
        OrcWeaponHudGetIconSpriteRemapFrom() ? OrcWeaponHudGetIconSpriteRemapFrom() : "-",
        OrcWeaponHudGetIconSpriteRemapTo() ? OrcWeaponHudGetIconSpriteRemapTo() : "-");
    g_CSprite2d_SetTextureMasked_Orig(self, name, maskName);
}

static void __cdecl CHud_DrawWeaponIcon_Detour(CPed* ped, int x, int y, float alpha) {
    if (!g_CHud_DrawWeaponIcon_Orig)
        return;

    int txdSlot = -1;
    RwTexDictionary* orcSpriteDict = nullptr;
    OrcWeaponHudTryGetIconOverrideTxdSlot(ped, &txdSlot);
    if (txdSlot >= 0)
        orcSpriteDict = OrcWeaponStreamingGetRwTxdDictionary(txdSlot);

    // `DrawAmmo` / `DrawWeaponIcon` paths re-bind hud.txd and call `CSprite2d::SetTexture`; keep Orc dict scoped for
    // the whole ammo draw — SA:MP R1 sometimes never hits `DrawWeaponIcon` or binds textures earlier in `DrawAmmo`.
    HudPushOrcSpriteDictScope(orcSpriteDict);
    g_CHud_DrawWeaponIcon_Orig(ped, x, y, alpha);
    HudPopOrcSpriteDictScope();
}

static bool g_drawAmmoHookInstalled = false;
static void(__cdecl* g_CHud_DrawAmmo_Orig)(CPed*, int, int, float) = nullptr;

static void __cdecl CHud_DrawAmmo_Detour(CPed* ped, int x, int y, float alpha) {
    if (!g_CHud_DrawAmmo_Orig)
        return;

    int txdSlot = -1;
    RwTexDictionary* orcSpriteDict = nullptr;
    OrcWeaponHudTryGetIconOverrideTxdSlot(ped, &txdSlot);
    if (txdSlot >= 0)
        orcSpriteDict = OrcWeaponStreamingGetRwTxdDictionary(txdSlot);

    HudPushOrcSpriteDictScope(orcSpriteDict);
    g_CHud_DrawAmmo_Orig(ped, x, y, alpha);
    HudPopOrcSpriteDictScope();
}

void OrcWeaponHudEnsureDrawWeaponIconHookInstalled() {
    if (g_drawWeaponIconHookInstalled && g_drawAmmoHookInstalled && g_hudSprTexMonoHookInstalled &&
        g_hudSprTexMaskHookInstalled)
        return;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("HUD weapon icon hooks: MH_Initialize -> %s", MH_StatusToString(st));
        return;
    }

    if (!g_hudSprTexMonoHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x727270),
                reinterpret_cast<void*>(&CSprite2d_SetTexture_HudWeaponOverlay),
                reinterpret_cast<void**>(&g_CSprite2d_SetTexture_Orig)) != MH_OK) {
            OrcLogError("CSprite2d::SetTexture HUD hook: MH_CreateHook failed (0x727270)");
            return;
        }
        st = MH_EnableHook(reinterpret_cast<void*>(0x727270));
        if (st != MH_OK) {
            OrcLogError("CSprite2d::SetTexture HUD hook: MH_EnableHook -> %s", MH_StatusToString(st));
            return;
        }
        g_hudSprTexMonoHookInstalled = true;
        OrcLogInfo("CSprite2d::SetTexture hooked (0x727270) for Hud weapon sprite (Orc dictionary)");
    }

    if (!g_hudSprTexMaskHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x7272B0),
                reinterpret_cast<void*>(&CSprite2d_SetTextureMasked_HudWeaponOverlay),
                reinterpret_cast<void**>(&g_CSprite2d_SetTextureMasked_Orig)) != MH_OK) {
            OrcLogError("CSprite2d::SetTexture(mask) HUD hook: MH_CreateHook failed (0x7272B0)");
        } else {
            st = MH_EnableHook(reinterpret_cast<void*>(0x7272B0));
            if (st != MH_OK)
                OrcLogError(
                    "CSprite2d::SetTexture(mask) HUD hook: MH_EnableHook -> %s", MH_StatusToString(st));
            else {
                g_hudSprTexMaskHookInstalled = true;
                OrcLogInfo("CSprite2d::SetTexture mask hooked (0x7272B0) for Hud weapon sprite");
            }
        }
    }

    if (!g_drawWeaponIconHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x58D7D0),
                reinterpret_cast<void*>(&CHud_DrawWeaponIcon_Detour),
                reinterpret_cast<void**>(&g_CHud_DrawWeaponIcon_Orig)) != MH_OK) {
            OrcLogError("DrawWeaponIcon hook: MH_CreateHook failed");
            return;
        }
        st = MH_EnableHook(reinterpret_cast<void*>(0x58D7D0));
        if (st != MH_OK) {
            OrcLogError("DrawWeaponIcon hook: MH_EnableHook -> %s", MH_StatusToString(st));
            return;
        }
        g_drawWeaponIconHookInstalled = true;
        OrcLogInfo("CHud::DrawWeaponIcon hooked (0x58D7D0) for Guns/replacement HUD icon context");
    }

    if (!g_drawAmmoHookInstalled) {
        if (MH_CreateHook(reinterpret_cast<void*>(0x5893B0),
                reinterpret_cast<void*>(&CHud_DrawAmmo_Detour),
                reinterpret_cast<void**>(&g_CHud_DrawAmmo_Orig)) != MH_OK) {
            OrcLogError("DrawAmmo hook: MH_CreateHook failed");
            return;
        }
        st = MH_EnableHook(reinterpret_cast<void*>(0x5893B0));
        if (st != MH_OK) {
            OrcLogError("DrawAmmo hook: MH_EnableHook -> %s", MH_StatusToString(st));
            return;
        }
        g_drawAmmoHookInstalled = true;
        OrcLogInfo("CHud::DrawAmmo hooked (0x5893B0) for Guns/replacement HUD icon context");
    }
}

