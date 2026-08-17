#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

constexpr std::uint32_t kOrcRpAtomicRenderFlag = 4u;

constexpr std::uint32_t OrcInitialGunflashAtomicFlags(std::uint32_t flags) noexcept {
    return flags & ~kOrcRpAtomicRenderFlag;
}

constexpr std::uint32_t OrcInitialBodyAttachmentAtomicFlags(
    bool belongsToGunflash,
    std::uint32_t flags) noexcept {
    return belongsToGunflash ? OrcInitialGunflashAtomicFlags(flags) : flags;
}

enum class OrcBodyAttachmentFrameClass {
    Ordinary,
    Gunflash,
    Invalid,
};

template <typename Frame, typename IsValidFn, typename IsGunflashFn, typename GetParentFn>
OrcBodyAttachmentFrameClass OrcClassifyBodyAttachmentFramePath(Frame* frame,
    IsValidFn isValid,
    IsGunflashFn isGunflash,
    GetParentFn getParent) {
    constexpr std::size_t kMaxAncestors = 64u;
    if (!frame)
        return OrcBodyAttachmentFrameClass::Invalid;

    std::array<Frame*, kMaxAncestors> visited{};
    bool belongsToGunflash = false;
    for (std::size_t depth = 0; depth < visited.size(); ++depth) {
        for (std::size_t i = 0; i < depth; ++i) {
            if (visited[i] == frame)
                return OrcBodyAttachmentFrameClass::Invalid;
        }
        visited[depth] = frame;

        if (!isValid(frame))
            return OrcBodyAttachmentFrameClass::Invalid;
        belongsToGunflash = belongsToGunflash || isGunflash(frame);

        frame = getParent(frame);
        if (!frame) {
            return belongsToGunflash
                ? OrcBodyAttachmentFrameClass::Gunflash
                : OrcBodyAttachmentFrameClass::Ordinary;
        }
    }
    return OrcBodyAttachmentFrameClass::Invalid;
}

enum class OrcGunflashFrameBeforeCloneDestroy : std::uint8_t {
    KeepCurrent,
    UseStock,
    Clear,
};

constexpr OrcGunflashFrameBeforeCloneDestroy OrcSelectGunflashFrameBeforeCloneDestroy(
    bool currentFrameBelongsToClone,
    bool stockFrameIsRenderable) noexcept {
    if (!currentFrameBelongsToClone)
        return OrcGunflashFrameBeforeCloneDestroy::KeepCurrent;
    return stockFrameIsRenderable ? OrcGunflashFrameBeforeCloneDestroy::UseStock
                                  : OrcGunflashFrameBeforeCloneDestroy::Clear;
}
