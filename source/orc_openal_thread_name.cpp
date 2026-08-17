#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>

#include "external/openal-soft/common/althrd_setname.h"

namespace {

using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

SetThreadDescriptionFn ResolveSetThreadDescription() noexcept {
    constexpr const wchar_t* kModules[] = {L"Kernel32.dll", L"KernelBase.dll"};
    for (const wchar_t* moduleName : kModules) {
        const HMODULE module = GetModuleHandleW(moduleName);
        if (!module) continue;

        const auto fn = reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(module, "SetThreadDescription"));
        if (fn) return fn;
    }
    return nullptr;
}

}  // namespace

// OpenAL Soft 1.24.3 uses RaiseException(0x406D1388) for this symbol. Some ASI
// exception handlers treat that debugger notification as a crash. Defining the
// symbol in OrcOutFit keeps the legacy object out of OpenAL32.lib at link time.
void althrd_setname(const char* name) {
    if (!name || !*name) return;

    std::array<wchar_t, 64> wideName{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wideName.data(),
                                           static_cast<int>(wideName.size()));
    if (length <= 0) return;

    if (const SetThreadDescriptionFn setDescription = ResolveSetThreadDescription()) {
        (void)setDescription(GetCurrentThread(), wideName.data());
    }
}
