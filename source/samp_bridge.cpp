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
    std::uint32_t getPlayerPoolOffset;
    std::uint32_t getNameByIdOffset;
    std::uint32_t localPlayerIdOffset;
    std::uint32_t idFindOffset;
    std::uint32_t refGameOffset;
    std::uint32_t setCursorModeOffset;
};

constexpr std::array<SampVersionInfo, 8> kSampVersions{{
    {0x31DF13, SampVersion::R1,    "R1",    0x00065C60, 0x00001160, 0x00013CE0, 0x00000004, 0x00010420, 0x0021A10C, 0x0009BD30},
    {0x3195DD, SampVersion::R2,    "R2",    0x00065D30, 0x00001170, 0x00013DA0, 0x00000000, 0x000104C0, 0x0021A114, 0x0009BDD0},
    {0x0CC490, SampVersion::R3,    "R3",    0x00069190, 0x00001160, 0x00016F00, 0x00002F1C, 0x00013570, 0x0026E8F4, 0x0009FFE0},
    {0x0CC4D0, SampVersion::R3_1,  "R3-1",  0x00069190, 0x00001160, 0x00016F00, 0x00002F1C, 0x00013570, 0x0026E8F4, 0x0009FFE0},
    {0x0CBCB0, SampVersion::R4,    "R4",    0x000698C0, 0x00001170, 0x00017570, 0x0000000C, 0x00013890, 0x0026EA24, 0x000A0720},
    {0x0CBCD0, SampVersion::R4_2,  "R4-2",  0x00069900, 0x00001170, 0x000175C0, 0x00000004, 0x000138C0, 0x0026EA24, 0x000A0750},
    {0x0CBC90, SampVersion::R5_1,  "R5-1",  0x00069900, 0x00001170, 0x000175C0, 0x00000004, 0x000138C0, 0x0026EBAC, 0x000A06F0},
    {0x0FDB60, SampVersion::DL_R1, "DL-R1", 0x00069340, 0x00001170, 0x000170D0, 0x00000000, 0x000137C0, 0x002ACA3C, 0x000A0530},
}};

using SampSendCommandFn = void(__thiscall*)(void*, const char*);
using GetPlayerPoolFn = void*(__thiscall*)(void*);
using GetNameByIdFn = const char*(__thiscall*)(void*, unsigned int);
using IdFindFn = unsigned short(__thiscall*)(void*, const void*);
using SetCursorModeFn = char*(__thiscall*)(void*, int, bool);

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

int g_sampOverlayCursorMode = -1;
bool g_sampOverlayCursorEnabled = false;

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

bool GetPedNickname(const void* gtaPed, char* outName, int outNameLen, bool* isLocal) {
    if (outName && outNameLen > 0) outName[0] = '\0';
    if (isLocal) *isLocal = false;
    if (!gtaPed || !g_state.version || !g_state.base) return false;

    __try {
        std::uint32_t netGameRef = 0;
        switch (g_state.version->version) {
        case SampVersion::R1:    netGameRef = 0x0021A0F8; break;
        case SampVersion::R2:    netGameRef = 0x0021A100; break;
        case SampVersion::R3:
        case SampVersion::R3_1:  netGameRef = 0x0026E8DC; break;
        case SampVersion::R4:
        case SampVersion::R4_2:  netGameRef = 0x0026EA0C; break;
        case SampVersion::R5_1:  netGameRef = 0x0026EB94; break;
        case SampVersion::DL_R1: netGameRef = 0x002ACA24; break;
        default: return false;
        }
        void* netGame = *reinterpret_cast<void**>(g_state.base + netGameRef);
        if (!netGame) return false;

        auto getPlayerPool = reinterpret_cast<GetPlayerPoolFn>(g_state.base + g_state.version->getPlayerPoolOffset);
        void* playerPool = getPlayerPool(netGame);
        if (!playerPool) return false;

        auto idFind = reinterpret_cast<IdFindFn>(g_state.base + g_state.version->idFindOffset);
        unsigned short id = idFind(playerPool, gtaPed);
        if (id == 0xFFFF) return false;

        auto getNameById = reinterpret_cast<GetNameByIdFn>(g_state.base + g_state.version->getNameByIdOffset);
        const char* name = getNameById(playerPool, (unsigned int)id);
        if (!name || !name[0]) return false;

        if (isLocal) {
            unsigned short localId = *reinterpret_cast<unsigned short*>(
                reinterpret_cast<std::uint8_t*>(playerPool) + g_state.version->localPlayerIdOffset);
            *isLocal = (localId == id);
        }
        if (outName && outNameLen > 1) {
            lstrcpynA(outName, name, outNameLen);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsSampPresent() {
    return g_state.module != nullptr || GetModuleHandleA("samp.dll") != nullptr;
}

bool IsSampBuildKnown() {
    return g_state.version != nullptr;
}

bool IsCommandHookReady() {
    return g_state.commandHookInstalled;
}

const char* GetVersionName() {
    return g_state.version ? g_state.version->name : "";
}

void SyncSampOverlayCursor(bool wantUiCursor) {
    if (!g_state.version || !g_state.base)
        return;
    constexpr int kModeNone = 0;
    constexpr int kModeLockCam = 3;
    const int mode = wantUiCursor ? kModeLockCam : kModeNone;
    const bool enabled = wantUiCursor;
    if (g_sampOverlayCursorMode == mode && g_sampOverlayCursorEnabled == enabled)
        return;

    __try {
        const auto ref = reinterpret_cast<const std::uint32_t*>(g_state.base + g_state.version->refGameOffset);
        const std::uint32_t game = *ref;
        if (!game)
            return;
        auto setCursorMode = reinterpret_cast<SetCursorModeFn>(
            g_state.base + g_state.version->setCursorModeOffset);
        setCursorMode(reinterpret_cast<void*>(static_cast<std::uintptr_t>(game)), mode, enabled);
        g_sampOverlayCursorMode = mode;
        g_sampOverlayCursorEnabled = enabled;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace samp_bridge

