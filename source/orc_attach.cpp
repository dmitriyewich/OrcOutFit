#include "orc_attach.h"

#include "CVisibilityPlugins.h"

#include <cstring>

void OrcDestroyRwObjectInstance(RwObject*& obj) {
    if (!obj) return;
    if (obj->type == rpCLUMP) {
        RpClumpDestroy(reinterpret_cast<RpClump*>(obj));
    } else if (obj->type == rpATOMIC) {
        auto* a = reinterpret_cast<RpAtomic*>(obj);
        RwFrame* f = RpAtomicGetFrame(a);
        RpAtomicDestroy(a);
        if (f) RwFrameDestroy(f);
    }
    obj = nullptr;
}

static RpMaterial* OrcAddRefMatCB(RpMaterial* m, void*) {
    if (!m) return m;
    m->refCount++;
    return m;
}

static RpMaterial* OrcWhiteMatCB(RpMaterial* m, void*) {
    if (!m) return m;
    m->color = { 255, 255, 255, 255 };
    return m;
}

RpAtomic* OrcPrepAtomicCB(RpAtomic* a, void*) {
    if (!a) return a;
    if (a->geometry) {
        a->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
    }
    return a;
}

RpAtomic* OrcInitAtomicCB(RpAtomic* a, void*) {
    if (!a) return a;
    CVisibilityPlugins::SetAtomicRenderCallback(a, nullptr);
    if (a->geometry) {
        RpGeometryForAllMaterials(a->geometry, OrcAddRefMatCB, nullptr);
    }
    return a;
}

RpAtomic* OrcInitAttachmentAtomicCB(RpAtomic* a, void*) {
    OrcInitAtomicCB(a, nullptr);
    if (a && a->geometry) {
        a->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        RpGeometryForAllMaterials(a->geometry, OrcWhiteMatCB, nullptr);
    }
    return a;
}
