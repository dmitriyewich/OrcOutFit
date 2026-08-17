#pragma once

enum class OrcWeaponObjectKind {
    Unsupported,
    Atomic,
    Clump,
};

enum class OrcWeaponObjectPreparation {
    Reject,
    WrapAtomicInClump,
    PrepareClump,
};

enum class OrcWeaponCloneUsage {
    BodyAttachment,
    HeldReplacement,
};

enum class OrcWeaponCloneRenderContract {
    DedicatedBodyAttachment,
    DefaultAtomicCallback,
};

enum class OrcWeaponBodyRenderStage {
    DrawingEventAttachmentScene,
};

constexpr OrcWeaponBodyRenderStage OrcSelectWeaponBodyRenderStage() {
    return OrcWeaponBodyRenderStage::DrawingEventAttachmentScene;
}

constexpr bool OrcShouldRenderWeaponBodyInDrawingEvent(bool pluginEnabled,
    bool hasCachedBodyWeapons) {
    return pluginEnabled && hasCachedBodyWeapons;
}

constexpr bool OrcShouldEnterAttachmentScene(bool hasCachedBodyWeapons,
    bool renderObjectsAndSkins) {
    return hasCachedBodyWeapons || renderObjectsAndSkins;
}

constexpr OrcWeaponCloneRenderContract OrcSelectWeaponCloneRenderContract(OrcWeaponCloneUsage usage) {
    return usage == OrcWeaponCloneUsage::BodyAttachment
        ? OrcWeaponCloneRenderContract::DedicatedBodyAttachment
        : OrcWeaponCloneRenderContract::DefaultAtomicCallback;
}

constexpr OrcWeaponObjectPreparation OrcSelectWeaponBodyObjectPreparation(OrcWeaponObjectKind kind) {
    if (kind == OrcWeaponObjectKind::Clump)
        return OrcWeaponObjectPreparation::PrepareClump;
    if (kind == OrcWeaponObjectKind::Atomic)
        return OrcWeaponObjectPreparation::WrapAtomicInClump;
    return OrcWeaponObjectPreparation::Reject;
}

enum class OrcWeaponBodyInstanceSource {
    Stock,
    StockFallback,
    Replacement,
};

enum class OrcWeaponBodyAtomicCallbackPolicy {
    ForceDefaultCallback,
};

constexpr OrcWeaponBodyInstanceSource OrcSelectWeaponBodyInstanceSource(
    bool replacementRequested,
    bool replacementPrepared) {
    if (replacementPrepared)
        return OrcWeaponBodyInstanceSource::Replacement;
    return replacementRequested
        ? OrcWeaponBodyInstanceSource::StockFallback
        : OrcWeaponBodyInstanceSource::Stock;
}

constexpr OrcWeaponBodyAtomicCallbackPolicy OrcSelectWeaponBodyAtomicCallbackPolicy(
    OrcWeaponBodyInstanceSource source) {
    (void)source;
    return OrcWeaponBodyAtomicCallbackPolicy::ForceDefaultCallback;
}

enum class OrcWeaponBodyMaterialPolicy {
    Preserve,
    GitHubWhiteModulatePinned,
};

constexpr OrcWeaponBodyMaterialPolicy OrcSelectWeaponBodyMaterialPolicy(
    OrcWeaponBodyInstanceSource source) {
    return source == OrcWeaponBodyInstanceSource::Replacement
        ? OrcWeaponBodyMaterialPolicy::Preserve
        : OrcWeaponBodyMaterialPolicy::GitHubWhiteModulatePinned;
}

enum class OrcWeaponAtomicDispatch {
    ReplacementInterception,
    VanillaWeaponCallback,
};

constexpr OrcWeaponAtomicDispatch OrcSelectWeaponAtomicDispatch(bool preparedCustomWeaponDraw,
    bool bodyAttachmentDraw = false) {
    return preparedCustomWeaponDraw || bodyAttachmentDraw
        ? OrcWeaponAtomicDispatch::VanillaWeaponCallback
        : OrcWeaponAtomicDispatch::ReplacementInterception;
}

constexpr bool OrcShouldRunWeaponBodyCacheSync(bool pluginEnabled, bool playerPresent) {
    return pluginEnabled && playerPresent;
}

constexpr bool OrcShouldApplyObjectSuppressionToWeaponBody(bool attachmentSceneRendererAvailable) {
    return attachmentSceneRendererAvailable;
}
