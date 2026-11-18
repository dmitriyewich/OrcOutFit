#pragma once

#include "CPed.h"

struct OrcNativeSkinActiveInfo;
struct RpClump;

void OrcTextureRemapInstallHooks();
void OrcTextureRemapOnGameProcess();
void OrcTextureRemapRefreshAdditionalTxds();
void OrcTextureRemapInvalidateStates();
void OrcTextureRemapForgetOverlayClump(RpClump* clump, int txdSlot);
bool OrcSkinsGetOverlayTextureRemapInfo(CPed* ped, OrcNativeSkinActiveInfo& out);
void OrcTextureRemapOnPedSetModel(CPed* ped, int model);
// Pair each Apply call with RestoreAfter, even when the current draw needs no swaps.
void OrcTextureRemapApplyBefore(CPed* ped);
void OrcTextureRemapApplyToClumpBefore(CPed* ped, RpClump* clump, const char* dffName, const char* fallbackDffName, int txdIndex);
void OrcTextureRemapRestoreAfter();
void OrcTextureRemapRestoreAll();
void OrcTextureRemapClearRuntimeState();
