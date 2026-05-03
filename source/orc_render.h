#pragma once

#include "CVector.h"
#include "RenderWare.h"

class CPed;

RwMatrix* OrcGetBoneMatrix(CPed* ped, int boneNodeId);
void OrcApplyAttachmentLightingForPed(CPed* ped, const CVector& sampleWorldPos, float colourScale = 0.5f);
bool OrcTryPedSetupLighting(CPed* ped);
void OrcTryPedRemoveLighting(CPed* ped);
void OrcTryRpClumpRender(RpClump* clump);

void OrcApplyAttachmentOffset(RwMatrix* m, float ox, float oy, float oz);
void OrcRotateAttachmentMatrix(RwMatrix* m, float rx, float ry, float rz);
