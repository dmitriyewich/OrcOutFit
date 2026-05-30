// OrcOutFit Lite — заглушки символов исключённых модулей.
//
// В Lite-сборке (ORC_LITE) из проекта исключены TU: skins, texture remap,
// audio (OpenAL), оружие в руках (held / held_fx / hud). Объекты (orc_objects.cpp)
// в Lite ВКЛЮЧЕНЫ. Однако «ядро» (main.cpp, orc_weapon_runtime*.cpp body/glue,
// orc_weapon_assets.cpp, overlay.cpp, UI) местами дёргает функции исключённых модулей
// из монолитных путей (SyncAndRender / OnPedRender* / OnGameProcess / shutdown /
// overlay / weapon_assets). Здесь — безопасные no-op реализации, чтобы не резать
// хирургически горячие функции. Поведение Lite: рендер оружия на теле + объекты.

#ifdef ORC_LITE

#include <string>
#include <vector>

#include "orc_app.h"
#include "orc_types.h"

class OrcIniDocument;

// ---------------------------------------------------------------------------
// Скиновые контейнеры, к которым обращается монолитный SyncAndRender
// (размеры/счётчик active). В Lite всегда пустые — никакой скин-работы.
// Объекты (g_customObjects/g_standardObjects, OrcObjects*, OrcIsValidStandardPedModelForLocalApply)
// в Lite собираются из orc_objects.cpp — здесь НЕ стабятся.
// ---------------------------------------------------------------------------
std::vector<CustomSkinCfg> g_customSkins;
std::vector<StandardSkinCfg> g_standardSkins;

// ---------------------------------------------------------------------------
// Skins (orc_skins.cpp). OrcCollectPedSkins / OrcIsValidStandardSkinModel — в orc_ped_index.cpp.
// ---------------------------------------------------------------------------
void DiscoverCustomSkins() {}
void LoadStandardSkinsFromIni() {}
void OrcSkinsRenderForPeds(CPlayerPed*) {}
void OrcSkinsReleaseAllInstancesAndPreview() {}
bool OrcSkinsLocalSelectionAddsActiveWork() { return false; }
void OrcSkinsOnPedRenderBefore(CPed*) {}
void OrcSkinsOnPedRenderAfter(CPed*) {}
void OrcSkinsShutdown() {}
void OrcAppendSkinFeatureIniValues(std::vector<OrcIniValue>&) {}
void OrcAppendSkinModeIniValues(std::vector<OrcIniValue>&) {}
void InvalidateCustomSkinLookupCache() {}
void InvalidateStandardSkinLookupCache() {}

// ---------------------------------------------------------------------------
// Texture remap (orc_texture_remap.cpp)
// ---------------------------------------------------------------------------
void OrcTextureRemapApplyBefore(CPed*) {}
void OrcTextureRemapRestoreAfter() {}
void OrcTextureRemapClearRuntimeState() {}

// ---------------------------------------------------------------------------
// Weapon audio (orc_weapon_audio_*.cpp)
// ---------------------------------------------------------------------------
void OrcWeaponAudioOnGameProcess() {}
void OrcWeaponAudioShutdown() {}
void OrcWeaponAudioInvalidateCaches() {}
void OrcWeaponAudioConfigApplyFromMainIni(const OrcIniDocument&) {}
void OrcWeaponAudioConfigAppendMainIniValues(std::vector<OrcIniValue>&) {}

// ---------------------------------------------------------------------------
// Held weapon (orc_weapon_runtime_held*.cpp) + HUD (orc_weapon_runtime_hud.cpp)
// ---------------------------------------------------------------------------
void OrcDestroyAllHeldWeaponReplacementInstances() {}
void OrcPruneHeldWeaponReplacementInstances() {}
void OrcPrepareHeldWeaponTextureBefore(CPed*) {}
void OrcPrepareHeldWeaponReplacementBefore(CPed*) {}
void OrcRestoreHeldWeaponReplacementAfter(CPed*) {}
void OrcFlushDeferredHeldWeaponSlotRestore() {}
void OrcHeldPoseBeginSimFrame() {}
void OrcHeldWeaponTraceGameProcessTick() {}
void OrcWeaponSuppressBodyForHeldVisualWeapon(CPed*, std::vector<char>*) {}
int  OrcResolveWeaponHeldVisualWeaponType(CPed*) { return -1; }
int  OrcWeaponHudGetHeldReplacementWeaponTypeIfAny(CPed*) { return -1; }
bool OrcGetHeldReplacementKeyForPed(CPed*, int, std::string&) { return false; }
// OrcWeaponHudRefreshSampSpriteInterceptCache определён в orc_weapon_assets.cpp (KEEP) — не стабим.

#endif // ORC_LITE
