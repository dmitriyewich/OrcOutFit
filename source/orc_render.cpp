#include "orc_render.h"

#include "plugin.h"
#include "CPed.h"
#include "CPointLights.h"
#include "RenderWare.h"
#include "game_sa/rw/rphanim.h"

#include "orc_log.h"

#include <cmath>
#include <cstring>

RwMatrix* OrcGetBoneMatrix(CPed* ped, int boneNodeId) {
    if (!ped || !ped->m_pRwClump) return nullptr;
    RpHAnimHierarchy* h = GetAnimHierarchyFromSkinClump(ped->m_pRwClump);
    if (!h) return nullptr;
    RwInt32 id = RpHAnimIDGetIndex(h, boneNodeId);
    if (id < 0) return nullptr;
    return &h->pMatrixArray[id];
}

void OrcApplyAttachmentLightingForPed(CPed* ped, const CVector& sampleWorldPos, float colourScale) {
    if (!ped) return;
    ActivateDirectional();
    SetAmbientColours();
    float totalLighting = 0.0f;
    const float mult = CPointLights::GenerateLightsAffectingObject(&sampleWorldPos, &totalLighting, ped);
    (void)totalLighting;
    float v = mult * colourScale;
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    SetLightColoursForPedsCarsAndObjects(v);
}

bool OrcTryPedSetupLighting(CPed* ped) {
    if (!ped) return false;
    __try {
        return ped->SetupLighting();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SEH CPed::SetupLighting ex=0x%08X ped=%p", GetExceptionCode(), ped);
        return false;
    }
}

void OrcTryPedRemoveLighting(CPed* ped) {
    if (!ped) return;
    __try {
        ped->RemoveLighting();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SEH CPed::RemoveLighting ex=0x%08X ped=%p", GetExceptionCode(), ped);
    }
}

void OrcTryRpClumpRender(RpClump* clump) {
    if (!clump) return;
    __try {
        RpClumpRender(clump);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("SEH RpClumpRender ex=0x%08X clump=%p", GetExceptionCode(), clump);
    }
}

void OrcApplyAttachmentOffset(RwMatrix* m, float ox, float oy, float oz) {
    if (!m) return;
    m->pos.x += m->right.x * ox + m->up.x * oy + m->at.x * oz;
    m->pos.y += m->right.y * ox + m->up.y * oy + m->at.y * oz;
    m->pos.z += m->right.z * ox + m->up.z * oy + m->at.z * oz;
}

void OrcRotateAttachmentMatrix(RwMatrix* m, float rx, float ry, float rz) {
    if (!m) return;
    if (rx == 0 && ry == 0 && rz == 0) return;
    float cx = cosf(rx), sx = sinf(rx);
    float cy = cosf(ry), sy = sinf(ry);
    float cz = cosf(rz), sz = sinf(rz);
    float r00 =  cy*cz,            r01 = -cy*sz,           r02 =  sy;
    float r10 =  sx*sy*cz + cx*sz, r11 = -sx*sy*sz + cx*cz, r12 = -sx*cy;
    float r20 = -cx*sy*cz + sx*sz, r21 =  cx*sy*sz + sx*cz, r22 =  cx*cy;

    RwV3d rg = m->right, up = m->up, at = m->at;
    m->right.x = rg.x*r00 + up.x*r10 + at.x*r20;
    m->right.y = rg.y*r00 + up.y*r10 + at.y*r20;
    m->right.z = rg.z*r00 + up.z*r10 + at.z*r20;
    m->up.x    = rg.x*r01 + up.x*r11 + at.x*r21;
    m->up.y    = rg.y*r01 + up.y*r11 + at.y*r21;
    m->up.z    = rg.z*r01 + up.z*r11 + at.z*r21;
    m->at.x    = rg.x*r02 + up.x*r12 + at.x*r22;
    m->at.y    = rg.y*r02 + up.y*r12 + at.y*r22;
    m->at.z    = rg.z*r02 + up.z*r12 + at.z*r22;
}
