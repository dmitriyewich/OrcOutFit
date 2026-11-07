#pragma once

#include "CPed.h"

struct OrcNativeSkinActiveInfo;
struct RpClump;

void OrcTextureRemapInstallHooks();
void OrcTextureRemapOnGameProcess();
void OrcTextureRemapRefreshAdditionalTxds();
void OrcTextureRemapInvalidateStates();
bool OrcSkinsGetOverlayTextureRemapInfo(CPed* ped, OrcNativeSkinActiveInfo& out);
void OrcTextureRemapOnPedSetModel(CPed* ped, int model);
void OrcTextureRemapApplyBefore(CPed* ped);
void OrcTextureRemapApplyToClumpBefore(CPed* ped, RpClump* clump, const char* dffName, const char* fallbackDffName, int txdIndex);
void OrcTextureRemapRestoreAfter();
void OrcTextureRemapClearRuntimeState();
