#pragma once

#include "CPed.h"

void OrcTextureRemapInstallHooks();
void OrcTextureRemapOnProcessScripts();
void OrcTextureRemapOnPedSetModel(CPed* ped, int model);
void OrcTextureRemapApplyBefore(CPed* ped);
void OrcTextureRemapRestoreAfter();
void OrcTextureRemapClearRuntimeState();
