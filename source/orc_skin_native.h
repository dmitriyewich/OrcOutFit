#pragma once

#include "orc_types.h"

class CPed;
class CPlayerPed;
struct RpClump;

struct OrcNativeSkinActiveInfo {
    int baseModelId = -1;
    int txdSlot = -1;
    RpClump* clump = nullptr;
    const char* dffName = nullptr;
    const char* fallbackDffName = nullptr;
};

void OrcSkinNativeUpdateForPeds(CPlayerPed* localPlayer);
void OrcSkinNativeInstallHooks();
void OrcSkinNativeOnPedSetModel(CPed* ped, int modelId);
void OrcSkinNativeClearRuntimeState();
void OrcSkinNativeOnSkinAssetsReleased();
void OrcSkinNativeShutdown();
bool OrcSkinNativeGetActiveInfo(CPed* ped, OrcNativeSkinActiveInfo& out);
bool OrcSkinNativeShouldOverlayFallback(CPed* ped, const CustomSkinCfg* skin);
