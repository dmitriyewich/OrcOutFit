#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

constexpr std::uint32_t kOrcRpAtomicRenderFlag = 4u;

template<class Point>
class OrcGunflashMuzzleTip {
public:
    explicit OrcGunflashMuzzleTip(const Point& reference) : y_(reference.y), z_(reference.z) {
        const float radius = 0.09f + 0.06f * std::abs(reference.x);
        radiusSquared_ = radius * radius;
    }

    void Add(const Point& vertex) {
        const float y = vertex.y - y_;
        const float z = vertex.z - z_;
        if (vertex.x > front_ && std::isfinite(vertex.x) && y * y + z * z <= radiusSquared_)
            front_ = vertex.x;
    }

    bool Found() const { return front_ > 0.0f; }
    float Front() const { return front_; }

private:
    float y_;
    float z_;
    float radiusSquared_;
    float front_ = 0.0f;
};

constexpr bool OrcCanRelocateGunshotFx(bool skeletonUpdated, float distanceSquared) noexcept {
    // The native AI path can shoot from ped position + height when its bones are stale.
    return skeletonUpdated && distanceSquared <= 0.5f * 0.5f;
}

constexpr std::uint8_t OrcGunflashAlphaForProgress(int progress, unsigned maxProgress, int maxAlpha) noexcept {
    if (progress <= 0 || maxProgress == 0 || maxAlpha <= 0)
        return 0;
    const auto alpha = static_cast<std::int64_t>(progress) * maxAlpha / maxProgress;
    return static_cast<std::uint8_t>(alpha < 255 ? alpha : 255);
}

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
