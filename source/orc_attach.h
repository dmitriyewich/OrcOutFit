#pragma once

#include "RenderWare.h"

void OrcDestroyRwObjectInstance(RwObject*& obj);

RpAtomic* OrcInitAtomicCB(RpAtomic* a, void* data);
RpAtomic* OrcInitAttachmentAtomicCB(RpAtomic* a, void* data);
RpAtomic* OrcPrepAtomicCB(RpAtomic* a, void* data);
