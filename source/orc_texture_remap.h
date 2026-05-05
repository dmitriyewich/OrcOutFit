#pragma once

#include "CPed.h"

struct RpClump;

void OrcTextureRemapInstallHooks();
void OrcTextureRemapOnProcessScripts();
void OrcTextureRemapOnPedSetModel(CPed* ped, int model);
void OrcTextureRemapApplyBefore(CPed* ped);
void OrcTextureRemapApplyToClumpBefore(CPed* ped, RpClump* clump, const char* dffName, const char* fallbackDffName, int txdIndex);
void OrcTextureRemapRestoreAfter();
void OrcTextureRemapClearRuntimeState();
int OrcTextureRemapQueryMaxLinkedVariantsForTxd(int txdIndex);
