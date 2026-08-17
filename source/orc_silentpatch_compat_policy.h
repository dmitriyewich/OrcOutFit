#pragma once

#include <cstdint>

constexpr std::uint32_t kOrcGtaFrontEndInstructionAddress = 0x0053E9ACu;
constexpr std::uint32_t kOrcGtaFrontEndOperandAddress = 0x0053E9ADu;
constexpr std::uint32_t kOrcGtaFrontEndFlagAddress = 0x00BA67A4u;

enum class OrcSilentPatchFrontEndBindingAction {
    None,
    Stabilize,
    Reject,
};

constexpr std::uint32_t OrcDecodeX86RelativeCallTarget(std::uint32_t instructionAddress,
    std::uint32_t displacement) {
    return instructionAddress + 5u + static_cast<std::int32_t>(displacement);
}

constexpr OrcSilentPatchFrontEndBindingAction OrcSelectSilentPatchFrontEndBindingAction(
    std::uint8_t gtaOpcode,
    std::uint32_t silentPatchOperandAddress) {
    if (silentPatchOperandAddress != kOrcGtaFrontEndOperandAddress)
        return OrcSilentPatchFrontEndBindingAction::None;
    if (gtaOpcode == 0xA0u)
        return OrcSilentPatchFrontEndBindingAction::None;
    if (gtaOpcode == 0xE8u)
        return OrcSilentPatchFrontEndBindingAction::Stabilize;
    return OrcSilentPatchFrontEndBindingAction::Reject;
}
