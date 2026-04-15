#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "samp_bridge.h"
#include "external/MinHook/include/MinHook.h"

namespace samp_bridge {

namespace {

enum class SampVersion {
    R1,
    R2,
    R3,
    R3_1,
    R4,
    R4_2,
    R5_1,
    DL_R1,
};

struct SampVersionInfo {
    DWORD entryPointRva;
    SampVersion version;
    const char* name;
    std::uint32_t sendCommandOffset;
};

constexpr std::array<SampVersionInfo, 8> kSampVersions{{
    {0x31DF13, SampVersion::R1,    "R1",    0x00065C60},
    {0x3195DD, SampVersion::R2,    "R2",    0x00065D30},
    {0x0CC490, SampVersion::R3,    "R3",    0x00069190},
    {0x0CC4D0, SampVersion::R3_1,  "R3-1",  0x00069190},
    {0x0CBCB0, SampVersion::R4,    "R4",    0x000698C0},
    {0x0CBCD0, SampVersion::R4_2,  "R4-2",  0x00069900},
    {0x0CBC90, SampVersion::R5_1,  "R5-1",  0x00069900},
    {0x0FDB60, SampVersion::DL_R1, "DL-R1", 0x00069340},
}};

using SampSendCommandFn = void(__thiscall*)(void*, const char*);

struct RuntimeState {
    HMODULE module = nullptr;
    std::uintptr_t base = 0;
    const SampVersionInfo* version = nullptr;
    bool commandHookInstalled = false;
    SampSendCommandFn originalSendCommand = nullptr;
    const char* command = nullptr;
    ToggleCallback onToggle = nullptr;
    bool minHookInitialized = false;
} g_state;

bool StrEqualNoCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

const SampVersionInfo* DetectSampVersion(HMODULE sampModule) {
    if (!sampModule) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(sampModule);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const std::uint8_t*>(sampModule) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const DWORD ep = nt->OptionalHeader.AddressOfEntryPoint;
    for (const auto& it : kSampVersions) {
        if (it.entryPointRva == ep) return &it;
    }
    return nullptr;
}

void __fastcall SendCommandDetour(void* thisPtr, void* /*edx*/, const char* text) {
    if (text && g_state.command && StrEqualNoCase(text, g_state.command)) {
        if (g_state.onToggle) g_state.onToggle();
        return;
    }
    if (g_state.originalSendCommand) {
        g_state.originalSendCommand(thisPtr, text);
    }
}

} // namespace

void Poll(const char* command, ToggleCallback onToggle) {
    g_state.command = command;
    g_state.onToggle = onToggle;
    if (g_state.commandHookInstalled) return;

    HMODULE sampModule = GetModuleHandleA("samp.dll");
    if (!sampModule) return;

    g_state.module = sampModule;
    g_state.base = reinterpret_cast<std::uintptr_t>(sampModule);
    g_state.version = DetectSampVersion(sampModule);
    if (!g_state.version) return;
    if (!g_state.minHookInitialized) {
        if (MH_Initialize() != MH_OK) return;
        g_state.minHookInitialized = true;
    }

    void* sendCommand = reinterpret_cast<void*>(g_state.base + g_state.version->sendCommandOffset);
    if (MH_CreateHook(sendCommand, reinterpret_cast<void*>(&SendCommandDetour),
                      reinterpret_cast<void**>(&g_state.originalSendCommand)) != MH_OK) return;
    if (MH_EnableHook(sendCommand) != MH_OK) return;
    g_state.commandHookInstalled = true;
}

bool IsSampPresent() {
    return g_state.module != nullptr || GetModuleHandleA("samp.dll") != nullptr;
}

bool IsCommandHookReady() {
    return g_state.commandHookInstalled;
}

const char* GetVersionName() {
    return g_state.version ? g_state.version->name : "";
}

} // namespace samp_bridge

