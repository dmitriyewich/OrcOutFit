#include "orc_weapon_render.h"

#include "plugin.h"

#include "CPed.h"
#include "CVisibilityPlugins.h"
#include "NodeName.h"

#include "external/MinHook/include/MinHook.h"

#include "orc_app.h"
#include "orc_log.h"
#include "orc_weapon_assets.h"
#include "orc_weapon_gunflash_state.h"
#include "orc_weapon_runtime.h"
#include "orc_weapon_render_policy.h"

#include <cstdint>
#include <unordered_set>

namespace {

constexpr uintptr_t kRenderWeaponPedsForPcAddress = 0x732F30u;
using RenderWeaponPedsForPcFn = void(__cdecl*)();

unsigned g_vanillaCustomWeaponDrawDepth = 0;
unsigned g_bodyAttachmentDrawDepth = 0;
bool g_weaponBatchHookAttempted = false;
bool g_heldBatchBeforeSehLogged = false;
bool g_heldBatchAfterSehLogged = false;
bool g_textureDrawSehLogged = false;
bool g_bodyTextureDrawSehLogged = false;
RenderWeaponPedsForPcFn g_renderWeaponPedsForPcOriginal = nullptr;

void __cdecl RenderWeaponPedsForPcDetour() {
    if (OrcIsRuntimeShuttingDown()) {
        if (g_renderWeaponPedsForPcOriginal)
            g_renderWeaponPedsForPcOriginal();
        return;
    }

    __try {
        OrcHeldOnVanillaWeaponBatchBefore();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_heldBatchBeforeSehLogged) {
            g_heldBatchBeforeSehLogged = true;
            OrcLogError("weapon batch: held before SEH ex=0x%08X", GetExceptionCode());
        }
    }
    if (g_renderWeaponPedsForPcOriginal) {
        g_renderWeaponPedsForPcOriginal();
    }
    __try {
        OrcHeldOnVanillaWeaponBatchAfter();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_heldBatchAfterSehLogged) {
            g_heldBatchAfterSehLogged = true;
            OrcLogError("weapon batch: held after SEH ex=0x%08X", GetExceptionCode());
        }
    }
}

void LogPreparationErrorOnce(int modelId, unsigned reason, const char* text) {
    static std::unordered_set<std::uint64_t> logged;
    const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(modelId)) << 32u) | reason;
    if (logged.insert(key).second)
        OrcLogError("weapon render prepare failed: model=%d reason=%s", modelId, text ? text : "unknown");
}

OrcWeaponObjectKind GetObjectKind(const RwObject* object) {
    if (!object)
        return OrcWeaponObjectKind::Unsupported;
    if (object->type == rpCLUMP)
        return OrcWeaponObjectKind::Clump;
    if (object->type == rpATOMIC)
        return OrcWeaponObjectKind::Atomic;
    return OrcWeaponObjectKind::Unsupported;
}

bool WrapAtomicInClump(RwObject*& object) {
    auto* atomic = reinterpret_cast<RpAtomic*>(object);
    RpClump* clump = RpClumpCreate();
    if (!clump)
        return false;

    RwFrame* frame = RpAtomicGetFrame(atomic);
    if (!frame) {
        frame = RwFrameCreate();
        if (!frame) {
            RpClumpDestroy(clump);
            return false;
        }
        RpAtomicSetFrame(atomic, frame);
    }

    RpClumpSetFrame(clump, frame);
    RpClumpAddAtomic(clump, atomic);
    object = reinterpret_cast<RwObject*>(clump);
    return true;
}

struct PrepareAtomicContext {
    OrcWeaponBodyAtomicCallbackPolicy callbackPolicy =
        OrcWeaponBodyAtomicCallbackPolicy::ForceDefaultCallback;
    int modelId = 0;
    const char* sourceName = "unknown";
    bool traceDetails = false;
    unsigned currentAtomicIndex = 0;
    unsigned currentMaterialIndex = 0;
    unsigned count = 0;
    unsigned invalidCount = 0;
    unsigned invalidFrameHierarchyCount = 0;
    unsigned materialCount = 0;
    unsigned textureCount = 0;
    unsigned rasterCount = 0;
    unsigned gunflashAtomicCount = 0;
    unsigned gunflashRenderFlagsCleared = 0;
    unsigned gunflashVisibleAfterPrepare = 0;
};

const char* BodyAttachmentFrameClassName(OrcBodyAttachmentFrameClass frameClass) {
    switch (frameClass) {
    case OrcBodyAttachmentFrameClass::Ordinary:
        return "ordinary";
    case OrcBodyAttachmentFrameClass::Gunflash:
        return "gunflash";
    case OrcBodyAttachmentFrameClass::Invalid:
        return "invalid";
    }
    return "unknown";
}

const char* BodyWeaponSourceName(OrcWeaponBodyInstanceSource source) {
    switch (source) {
    case OrcWeaponBodyInstanceSource::Stock:
        return "stock";
    case OrcWeaponBodyInstanceSource::StockFallback:
        return "stockFallback";
    case OrcWeaponBodyInstanceSource::Replacement:
        return "orcReplacement";
    }
    return "unknown";
}

OrcBodyAttachmentFrameClass ClassifyBodyWeaponAtomicFrame(RwFrame* frame) {
    return OrcClassifyBodyAttachmentFramePath(
        frame,
        [](const RwFrame* current) { return current->object.type == rwFRAME; },
        [](RwFrame* current) {
            const char* const nodeName = GetFrameNodeName(current);
            return nodeName && _stricmp(nodeName, "gunflash") == 0;
        },
        [](RwFrame* current) {
            return reinterpret_cast<RwFrame*>(rwObjectGetParent(reinterpret_cast<RwObject*>(current)));
        });
}

RpMaterial* ValidateBodyWeaponMaterialCb(RpMaterial* material, void* data) {
    auto* context = reinterpret_cast<PrepareAtomicContext*>(data);
    if (!material) {
        if (context)
            ++context->invalidCount;
        return material;
    }
    if (context)
        ++context->materialCount;
    if (context && material->texture) {
        ++context->textureCount;
        if (RwTextureGetRaster(material->texture))
            ++context->rasterCount;
    }
    if (context && context->traceDetails) {
        RwTexture* const texture = material->texture;
        RwRaster* const raster = texture ? RwTextureGetRaster(texture) : nullptr;
        const char* const textureName = texture ? RwTextureGetName(texture) : nullptr;
        OrcLogInfo(
            "weapon body material: model=%d source=%s atomic=%u material=%u ptr=%p pipe=%p rgba=%u,%u,%u,%u surface=%.3f,%.3f,%.3f texture=%p name=%s raster=%p",
            context->modelId,
            context->sourceName,
            context->currentAtomicIndex,
            context->currentMaterialIndex++,
            static_cast<void*>(material),
            static_cast<void*>(material->pipeline),
            static_cast<unsigned>(material->color.red),
            static_cast<unsigned>(material->color.green),
            static_cast<unsigned>(material->color.blue),
            static_cast<unsigned>(material->color.alpha),
            material->surfaceProps.ambient,
            material->surfaceProps.specular,
            material->surfaceProps.diffuse,
            static_cast<void*>(texture),
            textureName && textureName[0] ? textureName : "-",
            static_cast<void*>(raster));
    }
    return material;
}

RpAtomic* PrepareBodyWeaponAtomicCb(RpAtomic* atomic, void* data) {
    auto* context = reinterpret_cast<PrepareAtomicContext*>(data);
    if (!atomic || !atomic->geometry || !RpAtomicGetFrame(atomic) ||
        atomic->geometry->numVertices <= 0 || atomic->geometry->numMorphTargets <= 0 ||
        !atomic->geometry->morphTarget || !atomic->geometry->mesh) {
        if (context)
            ++context->invalidCount;
        return atomic;
    }
    const unsigned atomicIndex = context ? context->count : 0u;
    if (context) {
        ++context->count;
        context->currentAtomicIndex = atomicIndex;
        context->currentMaterialIndex = 0u;
    }
    const RwUInt32 flagsBefore = RpAtomicGetFlags(atomic);
    RxPipeline* const atomicPipelineBefore = atomic->pipeline;
    const unsigned materialCountBefore = context ? context->materialCount : 0u;
    RpGeometryForAllMaterials(atomic->geometry, ValidateBodyWeaponMaterialCb, context);
    if (context && context->materialCount == materialCountBefore)
        ++context->invalidCount;
    const OrcBodyAttachmentFrameClass frameClass =
        ClassifyBodyWeaponAtomicFrame(RpAtomicGetFrame(atomic));
    if (frameClass == OrcBodyAttachmentFrameClass::Invalid) {
        if (context)
            ++context->invalidFrameHierarchyCount;
    } else if (context && frameClass == OrcBodyAttachmentFrameClass::Gunflash) {
        ++context->gunflashAtomicCount;
        const RwUInt32 flags = RpAtomicGetFlags(atomic);
        const RwUInt32 initialFlags = static_cast<RwUInt32>(
            OrcInitialBodyAttachmentAtomicFlags(true, static_cast<std::uint32_t>(flags)));
        if (initialFlags != flags) {
            RpAtomicSetFlags(atomic, initialFlags);
            ++context->gunflashRenderFlagsCleared;
        }
        if ((RpAtomicGetFlags(atomic) & kOrcRpAtomicRenderFlag) != 0u)
            ++context->gunflashVisibleAfterPrepare;
    }
    if (context && context->callbackPolicy == OrcWeaponBodyAtomicCallbackPolicy::ForceDefaultCallback)
        CVisibilityPlugins::SetAtomicRenderCallback(atomic, nullptr);
    if (context && context->traceDetails) {
        RwFrame* const frame = RpAtomicGetFrame(atomic);
        const char* const frameName = frame ? GetFrameNodeName(frame) : nullptr;
        const void* const callback = atomic->renderCallBack
            ? reinterpret_cast<const void*>(atomic->renderCallBack)
            : nullptr;
        OrcLogInfo(
            "weapon body atomic: model=%d source=%s index=%u frame=%p name=%s class=%s flags=0x%08X->0x%08X callback=%p atomicPipe=%p->%p geometry=%p geomFlags=0x%08X vertices=%d triangles=%d atomicRepEntry=%p geomRepEntry=%p",
            context->modelId,
            context->sourceName,
            atomicIndex,
            static_cast<void*>(frame),
            frameName && frameName[0] ? frameName : "-",
            BodyAttachmentFrameClassName(frameClass),
            static_cast<unsigned>(flagsBefore),
            static_cast<unsigned>(RpAtomicGetFlags(atomic)),
            callback,
            static_cast<void*>(atomicPipelineBefore),
            static_cast<void*>(atomic->pipeline),
            static_cast<void*>(atomic->geometry),
            static_cast<unsigned>(atomic->geometry->flags),
            atomic->geometry->numVertices,
            atomic->geometry->numTriangles,
            static_cast<void*>(atomic->repEntry),
            static_cast<void*>(atomic->geometry->repEntry));
    }
    return atomic;
}

struct BodyMaterialInitContext {
    std::vector<RpMaterial*>* pinnedMaterials = nullptr;
};

RpMaterial* InitStockBodyWeaponMaterialCb(RpMaterial* material, void* data) {
    auto* context = reinterpret_cast<BodyMaterialInitContext*>(data);
    if (!material || !context || !context->pinnedMaterials)
        return material;
    ++material->refCount;
    context->pinnedMaterials->push_back(material);
    material->color = { 255, 255, 255, 255 };
    return material;
}

RpAtomic* InitStockBodyWeaponAtomicCb(RpAtomic* atomic, void* data) {
    if (atomic && atomic->geometry) {
        atomic->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        RpGeometryForAllMaterials(atomic->geometry, InitStockBodyWeaponMaterialCb, data);
    }
    return atomic;
}

} // namespace

bool OrcNormalizeWeaponObjectToClumpForDefaultRender(RwObject*& object) {
    const OrcWeaponObjectKind kind = GetObjectKind(object);
    if (kind == OrcWeaponObjectKind::Clump)
        return true;
    if (kind != OrcWeaponObjectKind::Atomic)
        return false;
    return WrapAtomicInClump(object);
}

bool OrcPrepareWeaponObjectForBodyAttachment(RwObject*& object,
    int stockWeaponModelId,
    OrcWeaponBodyInstanceSource source,
    std::vector<RpMaterial*>& outPinnedMaterials) {
    outPinnedMaterials.clear();
    const OrcWeaponObjectKind initialKind = GetObjectKind(object);
    const OrcWeaponObjectPreparation plan =
        OrcSelectWeaponBodyObjectPreparation(initialKind);
    if (plan == OrcWeaponObjectPreparation::Reject) {
        LogPreparationErrorOnce(stockWeaponModelId, 1u, "unsupportedRwObject");
        return false;
    }

    if (plan == OrcWeaponObjectPreparation::WrapAtomicInClump && !WrapAtomicInClump(object)) {
        LogPreparationErrorOnce(stockWeaponModelId, 2u, "wrapAtomicFailed");
        return false;
    }

    auto* clump = reinterpret_cast<RpClump*>(object);
    if (!RpClumpGetFrame(clump)) {
        LogPreparationErrorOnce(stockWeaponModelId, 3u, "missingClumpFrame");
        return false;
    }
    PrepareAtomicContext context{};
    context.callbackPolicy = OrcSelectWeaponBodyAtomicCallbackPolicy(source);
    context.modelId = stockWeaponModelId;
    context.sourceName = BodyWeaponSourceName(source);
    if (source != OrcWeaponBodyInstanceSource::Replacement) {
        static std::unordered_set<std::uint64_t> tracedStockModels;
        const auto traceKey =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(stockWeaponModelId)) << 8u) |
            static_cast<unsigned>(source);
        context.traceDetails = tracedStockModels.insert(traceKey).second;
    }
    __try {
        RpClumpForAllAtomics(clump, PrepareBodyWeaponAtomicCb, &context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogPreparationErrorOnce(stockWeaponModelId, 4u, "atomicValidationSeh");
        return false;
    }
    if (context.count == 0u || context.invalidCount != 0u || context.invalidFrameHierarchyCount != 0u) {
        LogPreparationErrorOnce(stockWeaponModelId,
            context.count == 0u ? 5u : (context.invalidCount != 0u ? 6u : 7u),
            context.count == 0u
                ? "emptyClump"
                : (context.invalidCount != 0u ? "invalidAtomic" : "invalidFrameHierarchy"));
        return false;
    }

    const OrcWeaponBodyMaterialPolicy materialPolicy =
        OrcSelectWeaponBodyMaterialPolicy(source);
    if (materialPolicy == OrcWeaponBodyMaterialPolicy::GitHubWhiteModulatePinned) {
        outPinnedMaterials.reserve(context.materialCount);
        BodyMaterialInitContext materialContext{ &outPinnedMaterials };
        __try {
            RpClumpForAllAtomics(clump, InitStockBodyWeaponAtomicCb, &materialContext);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OrcReleaseWeaponBodyMaterialPins(outPinnedMaterials);
            LogPreparationErrorOnce(stockWeaponModelId, 9u, "attachmentInitSeh");
            return false;
        }
    }

    const char* materialContract =
        materialPolicy == OrcWeaponBodyMaterialPolicy::GitHubWhiteModulatePinned
        ? "whiteModulatePinned"
        : "preserved";

    if (context.traceDetails) {
        OrcLogInfo(
            "weapon body prepared exact: model=%d object=%s mesh=%s atomics=%u materials=%u textures=%u rasters=%u gunflash=%u cleared=%u visibleAfter=%u callback=%s material=%s pins=%zu",
            stockWeaponModelId,
            initialKind == OrcWeaponObjectKind::Atomic ? "atomicWrapped" : "clump",
            context.sourceName,
            context.count,
            context.materialCount,
            context.textureCount,
            context.rasterCount,
            context.gunflashAtomicCount,
            context.gunflashRenderFlagsCleared,
            context.gunflashVisibleAfterPrepare,
            "AtomicDefault",
            materialContract,
            outPinnedMaterials.size());
    }
    OrcLogInfoThrottled(497,
        5000u,
        "weapon body prepared: model=%d object=%s mesh=%s atomics=%u materials=%u textures=%u rasters=%u gunflash=%u cleared=%u visibleAfter=%u callback=%s material=%s pins=%zu",
        stockWeaponModelId,
        initialKind == OrcWeaponObjectKind::Atomic ? "atomicWrapped" : "clump",
        context.sourceName,
        context.count,
        context.materialCount,
        context.textureCount,
        context.rasterCount,
        context.gunflashAtomicCount,
        context.gunflashRenderFlagsCleared,
        context.gunflashVisibleAfterPrepare,
        "AtomicDefault",
        materialContract,
        outPinnedMaterials.size());
    return true;
}

void OrcReleaseWeaponBodyMaterialPins(std::vector<RpMaterial*>& pinnedMaterials) {
    __try {
        for (RpMaterial* material : pinnedMaterials) {
            if (material && material->refCount > 0)
                --material->refCount;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon body: material pin release SEH ex=0x%08X pins=%zu",
            GetExceptionCode(), pinnedMaterials.size());
    }
    pinnedMaterials.clear();
}

bool OrcRenderWeaponObjectLikeVanilla(RwObject* object) {
    if (!object || object->type != rpCLUMP)
        return false;

    bool rendered = false;
    ++g_vanillaCustomWeaponDrawDepth;
    __try {
        RpClumpRender(reinterpret_cast<RpClump*>(object));
        rendered = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("weapon render: RpClumpRender SEH ex=0x%08X object=%p",
            GetExceptionCode(),
            object);
    }
    if (g_vanillaCustomWeaponDrawDepth > 0u)
        --g_vanillaCustomWeaponDrawDepth;
    else
        OrcLogError("weapon render: draw guard underflow object=%p", object);
    return rendered;
}

bool OrcRenderWeaponObjectLikeVanillaWithTextures(CPed* ped,
    int weaponType,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement,
    bool applyTextureOverrides) {
    if (!applyTextureOverrides)
        return OrcRenderWeaponObjectLikeVanilla(object);

    bool rendered = false;
    __try {
        __try {
            OrcApplyWeaponTexturesCombined(
                ped, weaponType, object, customAsset, weaponMeshIsReplacement);
            rendered = OrcRenderWeaponObjectLikeVanilla(object);
        } __finally {
            OrcRestoreWeaponTextureOverrides();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_textureDrawSehLogged) {
            g_textureDrawSehLogged = true;
            OrcLogError("weapon render: texture apply/restore SEH ex=0x%08X object=%p",
                GetExceptionCode(),
                object);
        }
        rendered = false;
    }
    return rendered;
}

bool OrcRenderBodyWeaponObjectWithTextures(CPed* ped,
    int weaponType,
    RwObject* object,
    WeaponTextureAsset* customAsset,
    bool weaponMeshIsReplacement,
    bool applyTextureOverrides) {
    if (!object || object->type != rpCLUMP)
        return false;

    bool rendered = false;
    ++g_bodyAttachmentDrawDepth;
    __try {
        if (applyTextureOverrides) {
            __try {
                OrcApplyWeaponTexturesCombined(
                    ped, weaponType, object, customAsset, weaponMeshIsReplacement);
                RpClumpRender(reinterpret_cast<RpClump*>(object));
                rendered = true;
            } __finally {
                OrcRestoreWeaponTextureOverrides();
            }
        } else {
            RpClumpRender(reinterpret_cast<RpClump*>(object));
            rendered = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_bodyTextureDrawSehLogged) {
            g_bodyTextureDrawSehLogged = true;
            OrcLogError("weapon body draw SEH ex=0x%08X object=%p; instance retried next frame",
                GetExceptionCode(),
                object);
        }
        rendered = false;
    }
    if (g_bodyAttachmentDrawDepth > 0u)
        --g_bodyAttachmentDrawDepth;
    else
        OrcLogError("weapon body: draw guard underflow object=%p", object);
    return rendered;
}

bool OrcIsBodyAttachmentDrawActive() {
    return g_bodyAttachmentDrawDepth != 0u;
}

bool OrcIsVanillaCustomWeaponDrawActive() {
    return g_vanillaCustomWeaponDrawDepth != 0u;
}

void OrcWeaponRenderEnsureBatchHookInstalled() {
    if (g_weaponBatchHookAttempted)
        return;
    g_weaponBatchHookAttempted = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("weapon batch hook: MH_Initialize -> %s", MH_StatusToString(status));
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(kRenderWeaponPedsForPcAddress),
        reinterpret_cast<void*>(&RenderWeaponPedsForPcDetour),
        reinterpret_cast<void**>(&g_renderWeaponPedsForPcOriginal));
    if (status != MH_OK) {
        OrcLogError("weapon batch hook: MH_CreateHook 0x%08X -> %s",
            static_cast<unsigned>(kRenderWeaponPedsForPcAddress),
            MH_StatusToString(status));
        return;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(kRenderWeaponPedsForPcAddress));
    if (status != MH_OK) {
        OrcLogError("weapon batch hook: MH_EnableHook 0x%08X -> %s",
            static_cast<unsigned>(kRenderWeaponPedsForPcAddress),
            MH_StatusToString(status));
        return;
    }

    OrcLogInfo("weapon batch hook installed: RenderWeaponPedsForPC=0x%08X held=sharedBatch body=drawingEventAttachment",
        static_cast<unsigned>(kRenderWeaponPedsForPcAddress));
}
