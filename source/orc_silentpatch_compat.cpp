#include "orc_silentpatch_compat.h"

#include <windows.h>

#include "orc_log.h"
#include "orc_silentpatch_compat_policy.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

enum class CompatState {
    Polling,
    Complete,
    Failed,
};

constexpr unsigned kMaxStartupPolls = 8u;
constexpr std::size_t kBindingLoadPatternSize = 16u;

CompatState g_state = CompatState::Polling;
unsigned g_pollCount = 0u;
std::uintptr_t g_stableFrontEndOperand = kOrcGtaFrontEndFlagAddress;

bool MatchesBindingLoad(const std::uint8_t* code) {
    static constexpr std::uint8_t suffix[] = {
        0x57, 0x8B, 0x7C, 0x24, 0x1C, 0x8B, 0x00, 0x80, 0x38, 0x00, 0x74,
    };
    return code[0] == 0xA1u && std::memcmp(code + 5u, suffix, sizeof(suffix)) == 0;
}

bool ReadImageHeaders(HMODULE module, IMAGE_NT_HEADERS32*& ntHeaders) {
    ntHeaders = nullptr;
    if (!module)
        return false;
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;
        ntHeaders = nt;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t* FindSilentPatchBindingStorage(HMODULE module) {
    IMAGE_NT_HEADERS32* nt = nullptr;
    if (!ReadImageHeaders(module, nt))
        return nullptr;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
    std::uint32_t* match = nullptr;
    unsigned matches = 0u;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections; ++sectionIndex, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        const std::size_t sectionStart = section->VirtualAddress;
        const std::size_t sectionSize = section->Misc.VirtualSize;
        if (sectionStart >= imageSize || sectionSize < kBindingLoadPatternSize)
            continue;
        const std::size_t scanSize = (sectionSize < imageSize - sectionStart) ? sectionSize : imageSize - sectionStart;
        const auto* code = base + sectionStart;
        for (std::size_t offset = 0; offset + kBindingLoadPatternSize <= scanSize; ++offset) {
            if (!MatchesBindingLoad(code + offset))
                continue;
            std::uint32_t storageAddress = 0u;
            std::memcpy(&storageAddress, code + offset + 1u, sizeof(storageAddress));
            if (storageAddress < reinterpret_cast<std::uintptr_t>(base) ||
                storageAddress + sizeof(std::uint32_t) > reinterpret_cast<std::uintptr_t>(base) + imageSize)
                continue;
            auto* candidate = reinterpret_cast<std::uint32_t*>(storageAddress);
            MEMORY_BASIC_INFORMATION page{};
            if (VirtualQuery(candidate, &page, sizeof(page)) != sizeof(page) || page.State != MEM_COMMIT)
                continue;
            if ((page.Protect & PAGE_GUARD) != 0)
                continue;
            const DWORD writableProtection = page.Protect & 0xFFu;
            if (writableProtection != PAGE_READWRITE && writableProtection != PAGE_WRITECOPY &&
                writableProtection != PAGE_EXECUTE_READWRITE && writableProtection != PAGE_EXECUTE_WRITECOPY)
                continue;
            match = candidate;
            ++matches;
        }
    }
    return matches == 1u ? match : nullptr;
}

bool StabilizeSilentPatchBinding(std::uint32_t* bindingStorage) {
    DWORD oldProtect = 0u;
    if (!VirtualProtect(bindingStorage, sizeof(*bindingStorage), PAGE_READWRITE, &oldProtect))
        return false;
    const auto stableOperandAddress = reinterpret_cast<std::uintptr_t>(&g_stableFrontEndOperand);
    static_assert(sizeof(stableOperandAddress) == sizeof(*bindingStorage), "OrcOutFit targets x86 only");
    InterlockedExchange(reinterpret_cast<volatile LONG*>(bindingStorage), static_cast<LONG>(stableOperandAddress));
    DWORD ignored = 0u;
    VirtualProtect(bindingStorage, sizeof(*bindingStorage), oldProtect, &ignored);
    return true;
}

} // namespace

bool OrcSilentPatchCompatPoll() {
    if (g_state != CompatState::Polling)
        return false;
    ++g_pollCount;

    const auto gtaOpcode = *reinterpret_cast<volatile std::uint8_t*>(kOrcGtaFrontEndInstructionAddress);
    HMODULE silentPatch = GetModuleHandleA("SilentPatchSA.asi");
    if (!silentPatch) {
        if (g_pollCount >= kMaxStartupPolls)
            g_state = CompatState::Complete;
        return g_state == CompatState::Polling;
    }

    const auto shapeAction =
        OrcSelectSilentPatchFrontEndBindingAction(gtaOpcode, kOrcGtaFrontEndOperandAddress);
    if (shapeAction == OrcSilentPatchFrontEndBindingAction::None) {
        if (g_pollCount >= kMaxStartupPolls)
            g_state = CompatState::Complete;
        return g_state == CompatState::Polling;
    }
    if (shapeAction == OrcSilentPatchFrontEndBindingAction::Reject) {
        g_state = CompatState::Failed;
        OrcLogError("SilentPatch cursor compatibility: unsupported opcode 0x%02X at 0x%08X; binding unchanged",
            static_cast<unsigned>(gtaOpcode),
            kOrcGtaFrontEndInstructionAddress);
        return false;
    }

    std::uint32_t* bindingStorage = FindSilentPatchBindingStorage(silentPatch);
    if (!bindingStorage) {
        if (gtaOpcode == 0xE8u) {
            g_state = CompatState::Failed;
            OrcLogError("SilentPatch cursor compatibility: inline hook at 0x%08X detected, binding signature not found",
                kOrcGtaFrontEndInstructionAddress);
        } else if (g_pollCount >= kMaxStartupPolls) {
            g_state = CompatState::Complete;
        }
        return g_state == CompatState::Polling;
    }

    const std::uint32_t operandAddress = *bindingStorage;
    switch (OrcSelectSilentPatchFrontEndBindingAction(gtaOpcode, operandAddress)) {
    case OrcSilentPatchFrontEndBindingAction::None:
        if (operandAddress != kOrcGtaFrontEndOperandAddress || g_pollCount >= kMaxStartupPolls)
            g_state = CompatState::Complete;
        return g_state == CompatState::Polling;
    case OrcSilentPatchFrontEndBindingAction::Reject:
        // The opcode shape was checked above; keep this branch for an exhaustive enum switch.
        g_state = CompatState::Failed;
        return false;
    case OrcSilentPatchFrontEndBindingAction::Stabilize:
        break;
    }

    if (!StabilizeSilentPatchBinding(bindingStorage)) {
        g_state = CompatState::Failed;
        OrcLogError("SilentPatch cursor compatibility: failed to stabilize frontend binding (win32=%lu)",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    std::uint32_t displacement = 0u;
    std::memcpy(&displacement,
        reinterpret_cast<const void*>(kOrcGtaFrontEndOperandAddress),
        sizeof(displacement));
    g_state = CompatState::Complete;
    OrcLogInfo("SilentPatch cursor compatibility: stabilized frontend binding; inline CALL target=0x%08X",
        OrcDecodeX86RelativeCallTarget(kOrcGtaFrontEndInstructionAddress, displacement));
    return false;
}
