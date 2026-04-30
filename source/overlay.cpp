#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "overlay.h"

#include "orc_log.h"
#include "samp_bridge.h"
#include "external/MinHook/include/MinHook.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "game_sa/CPad.h"
#include "game_sa/rw/skeleton.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

extern RsGlobalType& RsGlobal;

namespace overlay {
namespace {

constexpr char kDummyWindowClassName[] = "OrcOutFitD3D9DummyWindow";
constexpr uintptr_t kGtaWindowHandleAddress = 0x00C8CF88u;
constexpr uintptr_t kCursorKeyboardCallAddr = 0x541DF5u;
constexpr uintptr_t kCursorMouseStateCallAddr = 0x53F417u;
constexpr uintptr_t kCursorMouseStateBranchAddr = 0x53F41Fu;
constexpr uintptr_t kCursorSetPosEntryAddr = 0x6194A0u;

using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

std::atomic_bool g_attachStarted{ false };
std::atomic_bool g_shutdownRequested{ false };
std::atomic_bool g_hooksInstalled{ false };

HANDLE  g_initThread     = nullptr;
bool    g_imguiInited    = false;
bool    g_menuOpen       = false;
bool    g_needCreateObj  = false;
bool    g_cursorPatched  = false;
bool    g_lastWantCursor = false;
bool    g_hadNoOverlayUi = true;
bool    g_stickyMouseCapture = false;
HWND    g_hwnd           = nullptr;
WNDPROC g_origProc       = nullptr;
DrawFn  g_drawFn         = nullptr;
int     g_toggleVk       = VK_F7;
bool    g_hotkeyEnabled  = true;

void* g_endSceneTarget = nullptr;
void* g_presentTarget  = nullptr;
void* g_resetTarget    = nullptr;

EndSceneFn g_originalEndScene = nullptr;
PresentFn  g_originalPresent  = nullptr;
ResetFn    g_originalReset    = nullptr;

struct CursorPatchSnapshot {
    BYTE keyboardCall[5] = {};
    BYTE mouseStateCall[5] = {};
    BYTE mouseStateBranch[4] = {};
    BYTE setPosEntry = 0;
    bool valid = false;
} g_cursorPatchSnapshot;

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
HRESULT __stdcall EndSceneDetour(IDirect3DDevice9* device);
HRESULT __stdcall PresentDetour(
    IDirect3DDevice9* device,
    const RECT* sourceRect,
    const RECT* destRect,
    HWND overrideWindow,
    const RGNDATA* dirtyRegion);
HRESULT __stdcall ResetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);

bool IsMouseMsg(UINT message) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

bool IsKeyMsg(UINT message) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_IME_CHAR:
    case WM_IME_COMPOSITION:
        return true;
    default:
        return false;
    }
}

bool WantsUiCursorNow() {
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0;
}

void WriteBytes(uintptr_t addr, const void* src, size_t n) {
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &oldProt))
        return;
    std::memcpy(reinterpret_cast<void*>(addr), src, n);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), n, oldProt, &tmp);
}

void WriteByte(uintptr_t addr, BYTE value) {
    WriteBytes(addr, &value, 1);
}

void LoadUiFont(ImGuiIO& io) {
    static constexpr const char* kFontCandidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();
    for (const char* path : kFontCandidates) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (io.Fonts->AddFontFromFileTTF(path, 14.0f, nullptr, ranges)) {
            OrcLogInfo("overlay: loaded UI font %s", path);
            return;
        }
    }

    ImFontConfig cfg{};
    cfg.SizePixels = 14.0f;
    io.Fonts->AddFontDefault(&cfg);
    OrcLogError("overlay: failed to load Cyrillic UI font, using ImGui default");
}

void CaptureCursorPatchSnapshot() {
    if (g_cursorPatchSnapshot.valid)
        return;

    __try {
        std::memcpy(
            g_cursorPatchSnapshot.keyboardCall,
            reinterpret_cast<const void*>(kCursorKeyboardCallAddr),
            sizeof(g_cursorPatchSnapshot.keyboardCall));
        std::memcpy(
            g_cursorPatchSnapshot.mouseStateCall,
            reinterpret_cast<const void*>(kCursorMouseStateCallAddr),
            sizeof(g_cursorPatchSnapshot.mouseStateCall));
        std::memcpy(
            g_cursorPatchSnapshot.mouseStateBranch,
            reinterpret_cast<const void*>(kCursorMouseStateBranchAddr),
            sizeof(g_cursorPatchSnapshot.mouseStateBranch));
        g_cursorPatchSnapshot.setPosEntry = *reinterpret_cast<const BYTE*>(kCursorSetPosEntryAddr);
        g_cursorPatchSnapshot.valid = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_logged = false;
        if (!s_logged) {
            OrcLogError("overlay: failed to snapshot GTA cursor patch bytes ex=0x%08X", GetExceptionCode());
            s_logged = true;
        }
    }
}

void PatchCursor(bool show) {
    if (show == g_cursorPatched)
        return;

    if (show) {
        CaptureCursorPatchSnapshot();
        static const BYTE nop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        static const BYTE patch4[4] = { 0x33, 0xC0, 0x0F, 0x84 };
        WriteBytes(kCursorKeyboardCallAddr, nop5, sizeof(nop5));
        WriteBytes(kCursorMouseStateCallAddr, nop5, sizeof(nop5));
        WriteBytes(kCursorMouseStateBranchAddr, patch4, sizeof(patch4));
        WriteByte(kCursorSetPosEntryAddr, 0xC3);
    } else {
        static const BYTE o1[5] = { 0xE8, 0x46, 0xF3, 0xFE, 0xFF };
        static const BYTE o2[5] = { 0xE8, 0xB4, 0x7A, 0x20, 0x00 };
        static const BYTE o3[4] = { 0x85, 0xC0, 0x0F, 0x8C };
        WriteBytes(
            kCursorKeyboardCallAddr,
            g_cursorPatchSnapshot.valid ? g_cursorPatchSnapshot.keyboardCall : o1,
            sizeof(o1));
        WriteBytes(
            kCursorMouseStateCallAddr,
            g_cursorPatchSnapshot.valid ? g_cursorPatchSnapshot.mouseStateCall : o2,
            sizeof(o2));
        WriteBytes(
            kCursorMouseStateBranchAddr,
            g_cursorPatchSnapshot.valid ? g_cursorPatchSnapshot.mouseStateBranch : o3,
            sizeof(o3));
        WriteByte(kCursorSetPosEntryAddr, g_cursorPatchSnapshot.valid ? g_cursorPatchSnapshot.setPosEntry : 0xE9);
    }

    g_cursorPatched = show;
}

void ResetPadMouseState() {
    std::memset(&CPad::NewMouseControllerState, 0, sizeof(CMouseControllerState));
    __try {
        reinterpret_cast<void(__cdecl*)()>(0x541BD0)();
        reinterpret_cast<void(__cdecl*)()>(0x541DD0)();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SetPlayerControlsBlocked(bool blocked) {
    if (CPad* pad = CPad::GetPad(0))
        pad->DisablePlayerControls = blocked ? 0xFFFF : 0;
}

void ApplyCursorState(IDirect3DDevice9* device, bool wantCursor) {
    const bool sampCursorApi = samp_bridge::IsSampBuildKnown();
    ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;

    if (sampCursorApi) {
        if (g_cursorPatched)
            PatchCursor(false);
        samp_bridge::SyncSampOverlayCursor(wantCursor);
    } else if (wantCursor != g_lastWantCursor) {
        PatchCursor(wantCursor);
        if (device)
            device->ShowCursor(wantCursor ? TRUE : FALSE);
    }

    if (io)
        io->MouseDrawCursor = wantCursor && !sampCursorApi;

    if (wantCursor || g_lastWantCursor != wantCursor)
        ResetPadMouseState();
    SetPlayerControlsBlocked(wantCursor);
    g_lastWantCursor = wantCursor;
}

bool TryGetModuleForAddress(const void* address, HMODULE* outModule, wchar_t* outPath, DWORD outPathCapacity) {
    if (outModule)
        *outModule = nullptr;
    if (outPath && outPathCapacity > 0)
        outPath[0] = L'\0';
    if (!address)
        return false;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),
            &module)) {
        return false;
    }

    if (outModule)
        *outModule = module;
    if (outPath && outPathCapacity > 0)
        GetModuleFileNameW(module, outPath, outPathCapacity);
    return true;
}

const wchar_t* BaseNameFromPath(const wchar_t* path) {
    if (!path)
        return L"";
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }
    return base;
}

bool IsAddressInModuleNamed(const void* address, const wchar_t* expectedBaseName, wchar_t* outPath, DWORD outPathCapacity) {
    HMODULE module = nullptr;
    if (!TryGetModuleForAddress(address, &module, outPath, outPathCapacity))
        return false;
    return lstrcmpiW(BaseNameFromPath(outPath), expectedBaseName) == 0;
}

void TraceModuleForAddress(const char* label, const void* address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (TryGetModuleForAddress(address, &module, path, MAX_PATH)) {
        const auto rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
        OrcLogInfo("overlay: d3d target %s=%p module=%ls rva=0x%X", label, address, path, static_cast<unsigned>(rva));
        return;
    }
    OrcLogInfo("overlay: d3d target %s=%p module=<unknown>", label, address);
}

HWND ReadGtaWindowHandle() {
    HWND hwnd = nullptr;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(kGtaWindowHandleAddress),
            &hwnd,
            sizeof(hwnd),
            &bytesRead) || bytesRead != sizeof(hwnd)) {
        return nullptr;
    }
    return IsWindow(hwnd) ? hwnd : nullptr;
}

HWND ResolveGameWindow(IDirect3DDevice9* device) {
    if (g_hwnd && IsWindow(g_hwnd))
        return g_hwnd;

    if (RsGlobal.ps && RsGlobal.ps->window && IsWindow(RsGlobal.ps->window))
        return RsGlobal.ps->window;

    if (HWND gtaWindow = ReadGtaWindowHandle())
        return gtaWindow;

    if (device) {
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (SUCCEEDED(device->GetCreationParameters(&params)) && IsWindow(params.hFocusWindow))
            return params.hFocusWindow;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground && IsWindow(foreground)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        if (pid == GetCurrentProcessId())
            return foreground;
    }

    return nullptr;
}

void DestroyDummyWindow(HWND window, bool registeredWindowClass) {
    if (window)
        DestroyWindow(window);
    if (registeredWindowClass)
        UnregisterClassA(kDummyWindowClassName, GetModuleHandleA(nullptr));
}

bool CreateDummyDevice(IDirect3DDevice9** outDevice, HWND* outWindow, bool* outRegisteredWindowClass) {
    if (!outDevice || !outWindow || !outRegisteredWindowClass)
        return false;

    *outDevice = nullptr;
    *outWindow = nullptr;
    *outRegisteredWindowClass = false;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kDummyWindowClassName;

    SetLastError(0);
    const ATOM atom = RegisterClassExA(&wc);
    const DWORD registerError = GetLastError();
    if (!atom && registerError != ERROR_CLASS_ALREADY_EXISTS) {
        OrcLogError("overlay: RegisterClassExA failed gle=%lu", registerError);
        return false;
    }
    *outRegisteredWindowClass = atom != 0;

    HWND window = CreateWindowExA(
        0,
        kDummyWindowClassName,
        kDummyWindowClassName,
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        100,
        100,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
    if (!window) {
        OrcLogError("overlay: CreateWindowExA failed gle=%lu", GetLastError());
        DestroyDummyWindow(nullptr, *outRegisteredWindowClass);
        *outRegisteredWindowClass = false;
        return false;
    }

    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) {
        OrcLogError("overlay: Direct3DCreate9 failed");
        DestroyDummyWindow(window, *outRegisteredWindowClass);
        *outRegisteredWindowClass = false;
        return false;
    }

    D3DPRESENT_PARAMETERS params = {};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.BackBufferFormat = D3DFMT_UNKNOWN;

    struct Attempt {
        D3DDEVTYPE type;
        DWORD flags;
        const char* label;
    };
    static const Attempt attempts[] = {
        { D3DDEVTYPE_NULLREF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "nullref" },
        { D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "hal" },
    };

    HRESULT hr = D3DERR_INVALIDCALL;
    const char* selected = nullptr;
    for (const Attempt& attempt : attempts) {
        hr = d3d9->CreateDevice(
            D3DADAPTER_DEFAULT,
            attempt.type,
            window,
            attempt.flags,
            &params,
            outDevice);
        if (SUCCEEDED(hr) && *outDevice) {
            selected = attempt.label;
            break;
        }
        if (*outDevice) {
            (*outDevice)->Release();
            *outDevice = nullptr;
        }
    }

    d3d9->Release();

    if (FAILED(hr) || !*outDevice) {
        OrcLogError("overlay: dummy IDirect3D9::CreateDevice failed hr=0x%08lX", static_cast<unsigned long>(hr));
        DestroyDummyWindow(window, *outRegisteredWindowClass);
        *outRegisteredWindowClass = false;
        return false;
    }

    OrcLogInfo("overlay: dummy D3D9 device created via %s", selected ? selected : "<unknown>");
    *outWindow = window;
    return true;
}

template <typename Fn>
bool CreateAndEnableHook(void* target, void* detour, Fn* original, const char* label) {
    if (!target || !detour || !original)
        return false;

    MH_STATUS st = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (st != MH_OK) {
        OrcLogError("overlay: MH_CreateHook %s -> %s", label, MH_StatusToString(st));
        *original = nullptr;
        return false;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        OrcLogError("overlay: MH_EnableHook %s -> %s", label, MH_StatusToString(st));
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }

    return true;
}

bool InstallGraphicsHooks() {
    if (g_hooksInstalled.load())
        return true;

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) {
        OrcLogError("overlay: MH_Initialize -> %s", MH_StatusToString(mh));
        return false;
    }

    IDirect3DDevice9* dummyDevice = nullptr;
    HWND dummyWindow = nullptr;
    bool dummyClassRegistered = false;
    if (!CreateDummyDevice(&dummyDevice, &dummyWindow, &dummyClassRegistered))
        return false;

    auto cleanupDummy = [&]() {
        if (dummyDevice) {
            dummyDevice->Release();
            dummyDevice = nullptr;
        }
        DestroyDummyWindow(dummyWindow, dummyClassRegistered);
        dummyWindow = nullptr;
        dummyClassRegistered = false;
    };

    void** vtable = *reinterpret_cast<void***>(dummyDevice);
    if (!vtable) {
        OrcLogError("overlay: dummy D3D9 vtable is null");
        cleanupDummy();
        return false;
    }

    g_resetTarget = vtable[16];
    g_presentTarget = vtable[17];
    g_endSceneTarget = vtable[42];
    OrcLogInfo(
        "overlay: D3D9 vtable Reset=%p Present=%p EndScene=%p",
        g_resetTarget,
        g_presentTarget,
        g_endSceneTarget);
    TraceModuleForAddress("Reset", g_resetTarget);
    TraceModuleForAddress("Present", g_presentTarget);
    TraceModuleForAddress("EndScene", g_endSceneTarget);

    bool anyRenderHook = false;
    if (CreateAndEnableHook(g_endSceneTarget, reinterpret_cast<void*>(&EndSceneDetour), &g_originalEndScene, "EndScene")) {
        anyRenderHook = true;
    } else {
        g_endSceneTarget = nullptr;
    }

    if (CreateAndEnableHook(g_presentTarget, reinterpret_cast<void*>(&PresentDetour), &g_originalPresent, "Present")) {
        anyRenderHook = true;
    } else {
        g_presentTarget = nullptr;
    }

    wchar_t resetModulePath[MAX_PATH] = {};
    const bool skipResetHook = IsAddressInModuleNamed(g_resetTarget, L"apphelp.dll", resetModulePath, MAX_PATH);
    if (skipResetHook) {
        OrcLogError(
            "overlay: Reset hook skipped because target is apphelp.dll (%ls); Present/EndScene remain active",
            resetModulePath);
        g_resetTarget = nullptr;
        g_originalReset = nullptr;
    } else if (!CreateAndEnableHook(g_resetTarget, reinterpret_cast<void*>(&ResetDetour), &g_originalReset, "Reset")) {
        OrcLogError("overlay: Reset hook unavailable; Present/EndScene remain active");
        g_resetTarget = nullptr;
        g_originalReset = nullptr;
    }

    cleanupDummy();

    if (!anyRenderHook) {
        OrcLogError("overlay: no D3D9 render hook installed");
        return false;
    }

    g_hooksInstalled.store(true);
    OrcLogInfo(
        "overlay: D3D9 hooks installed Present=%d EndScene=%d Reset=%d",
        g_originalPresent ? 1 : 0,
        g_originalEndScene ? 1 : 0,
        g_originalReset ? 1 : 0);
    return true;
}

DWORD WINAPI InitializeThread(LPVOID) {
    OrcLogInfo("overlay: D3D hook thread started after game startup");
    for (int attempt = 1; attempt <= 30 && !g_shutdownRequested.load(); ++attempt) {
        if (InstallGraphicsHooks()) {
            OrcLogInfo("overlay: D3D hook thread finished");
            return 0;
        }
        if (attempt == 1 || attempt % 5 == 0)
            OrcLogError("overlay: D3D hook attempt %d failed, retrying", attempt);
        Sleep(1000);
    }
    OrcLogError("overlay: D3D hooks were not installed");
    return 0;
}

bool EnsureWndProcHookInstalled() {
    if (g_origProc || !g_hwnd)
        return g_origProc != nullptr;

    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc));
    if (previous == 0 && GetLastError() != 0) {
        OrcLogError("overlay: SetWindowLongPtrA failed gle=%lu", GetLastError());
        return false;
    }

    g_origProc = reinterpret_cast<WNDPROC>(previous);
    OrcLogInfo("overlay: WndProc hook installed hwnd=%p", g_hwnd);
    return true;
}

void RestoreWndProc() {
    if (!g_hwnd || !g_origProc)
        return;

    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origProc));
    g_origProc = nullptr;
}

bool IsPrimaryRenderTarget(IDirect3DDevice9* device) {
    if (!device)
        return false;

    IDirect3DSurface9* current = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    IUnknown* currentIdentity = nullptr;
    IUnknown* backBufferIdentity = nullptr;
    const HRESULT currentHr = device->GetRenderTarget(0, &current);
    const HRESULT backHr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    bool primary = false;
    if (SUCCEEDED(currentHr) && SUCCEEDED(backHr) && current && backBuffer) {
        if (SUCCEEDED(current->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&currentIdentity)))
            && SUCCEEDED(backBuffer->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&backBufferIdentity)))) {
            primary = currentIdentity == backBufferIdentity;
        }
    }
    if (currentIdentity)
        currentIdentity->Release();
    if (backBufferIdentity)
        backBufferIdentity->Release();
    if (current)
        current->Release();
    if (backBuffer)
        backBuffer->Release();
    return primary;
}

bool InitializeImGuiIfNeeded(IDirect3DDevice9* device) {
    if (g_imguiInited)
        return true;
    if (!device)
        return false;

    g_hwnd = ResolveGameWindow(device);
    if (!g_hwnd) {
        static DWORD s_lastLog = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastLog >= 1000) {
            s_lastLog = now;
            OrcLogInfo("overlay: waiting for game window");
        }
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigNavCaptureKeyboard = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;

    LoadUiFont(io);

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;
    st.FrameRounding = 3.0f;
    st.GrabRounding = 3.0f;

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        OrcLogError("overlay: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(device)) {
        OrcLogError("overlay: ImGui_ImplDX9_Init failed");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    if (!EnsureWndProcHookInstalled()) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    g_imguiInited = true;
    OrcLogInfo("overlay: ImGui initialized hwnd=%p", g_hwnd);
    return true;
}

void RenderImGuiFrame(IDirect3DDevice9* device) {
    if (!InitializeImGuiIfNeeded(device))
        return;

    if (g_needCreateObj) {
        ImGui_ImplDX9_CreateDeviceObjects();
        g_needCreateObj = false;
    }

    if (!g_menuOpen) {
        g_hadNoOverlayUi = true;
        g_stickyMouseCapture = false;
        ApplyCursorState(device, false);
        return;
    }

    const bool enteredOverlayFromNone = g_hadNoOverlayUi;
    g_hadNoOverlayUi = false;

    IDirect3DStateBlock9* stateBlock = nullptr;
    IDirect3DVertexDeclaration9* vertexDecl = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;

    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
        stateBlock->Capture();
    device->GetVertexDeclaration(&vertexDecl);
    device->GetVertexShader(&vertexShader);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    if (enteredOverlayFromNone)
        io.ClearInputMouse();
    if (g_hwnd) {
        POINT pt = {};
        if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt))
            io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
    }

    ImGui::NewFrame();

    const bool wantCursor = WantsUiCursorNow();
    if (wantCursor)
        ImGui::SetNextFrameWantCaptureMouse(true);

    if (g_drawFn)
        g_drawFn();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    const bool stickyMouseCapture = wantCursor || ImGui::GetIO().WantCaptureMouse;

    if (vertexShader) {
        device->SetVertexShader(vertexShader);
        vertexShader->Release();
    }
    if (vertexDecl) {
        device->SetVertexDeclaration(vertexDecl);
        vertexDecl->Release();
    }
    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }

    ApplyCursorState(device, wantCursor);
    g_stickyMouseCapture = stickyMouseCapture;
}

void CleanupImGui() {
    if (!g_imguiInited)
        return;

    RestoreWndProc();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imguiInited = false;
    g_hwnd = nullptr;
    g_needCreateObj = false;
}

void ForceCursorAndControlsOff(IDirect3DDevice9* device) {
    if (samp_bridge::IsSampBuildKnown()) {
        samp_bridge::SyncSampOverlayCursor(false);
    } else {
        PatchCursor(false);
        if (device)
            device->ShowCursor(FALSE);
    }

    if (ImGui::GetCurrentContext())
        ImGui::GetIO().MouseDrawCursor = false;
    SetPlayerControlsBlocked(false);
    ResetPadMouseState();
    g_lastWantCursor = false;
    g_stickyMouseCapture = false;
}

void SafeRenderImGuiFrame(IDirect3DDevice9* device) {
    __try {
        RenderImGuiFrame(device);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_logged = false;
        if (!s_logged) {
            OrcLogError("overlay: RenderImGuiFrame SEH ex=0x%08X; overlay disabled", GetExceptionCode());
            s_logged = true;
        }
        g_menuOpen = false;
        ForceCursorAndControlsOff(device);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_imguiInited && ImGui::GetCurrentContext()) {
        if (g_hotkeyEnabled && message == WM_KEYDOWN && static_cast<int>(wparam) == g_toggleVk
            && (HIWORD(lparam) & KF_REPEAT) == 0) {
            g_menuOpen = !g_menuOpen;
            return TRUE;
        }

        if (g_menuOpen) {
            ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
            const ImGuiIO& io = ImGui::GetIO();
            const bool wantsUiCursor = WantsUiCursorNow();
            const bool wantsMouseCapture = wantsUiCursor && (io.WantCaptureMouse || g_stickyMouseCapture || IsMouseMsg(message));
            const bool wantsKeyboardCapture = io.WantCaptureKeyboard;
            if (wantsMouseCapture && IsMouseMsg(message))
                return TRUE;
            if (wantsKeyboardCapture && IsKeyMsg(message))
                return TRUE;
        }
    }

    if (g_origProc)
        return CallWindowProcA(g_origProc, hwnd, message, wparam, lparam);
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

HRESULT __stdcall EndSceneDetour(IDirect3DDevice9* device) {
    if (!g_shutdownRequested.load() && !g_originalPresent && IsPrimaryRenderTarget(device))
        SafeRenderImGuiFrame(device);
    return g_originalEndScene ? g_originalEndScene(device) : D3D_OK;
}

HRESULT __stdcall PresentDetour(
    IDirect3DDevice9* device,
    const RECT* sourceRect,
    const RECT* destRect,
    HWND overrideWindow,
    const RGNDATA* dirtyRegion) {
    if (!g_shutdownRequested.load() && IsPrimaryRenderTarget(device))
        SafeRenderImGuiFrame(device);
    return g_originalPresent ? g_originalPresent(device, sourceRect, destRect, overrideWindow, dirtyRegion) : D3D_OK;
}

HRESULT __stdcall ResetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    if (g_imguiInited)
        ImGui_ImplDX9_InvalidateDeviceObjects();

    const HRESULT hr = g_originalReset ? g_originalReset(device, params) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && g_imguiInited)
        ImGui_ImplDX9_CreateDeviceObjects();
    else if (FAILED(hr))
        g_needCreateObj = true;
    return hr;
}

} // namespace

void Init() {
    if (g_hooksInstalled.load() || g_attachStarted.exchange(true))
        return;

    g_shutdownRequested.store(false);
    OrcLogInfo("overlay: D3D hook install requested after game startup");
    g_initThread = CreateThread(nullptr, 0, &InitializeThread, nullptr, 0, nullptr);
    if (!g_initThread) {
        OrcLogError("overlay: CreateThread failed gle=%lu; installing hooks synchronously", GetLastError());
        InstallGraphicsHooks();
    }
}

void DrawFrame() {
    // Overlay rendering is now driven by D3D9 Present/EndScene hooks. This
    // function is kept for the existing drawingEvent call site.
}

void OnResetBefore() {
    if (g_imguiInited)
        ImGui_ImplDX9_InvalidateDeviceObjects();
}

void OnResetAfter() {
    if (g_imguiInited)
        g_needCreateObj = true;
}

void Shutdown() {
    OrcLogInfo("overlay: Shutdown");
    g_shutdownRequested.store(true);

    if (g_initThread) {
        if (GetCurrentThreadId() != GetThreadId(g_initThread))
            WaitForSingleObject(g_initThread, 5000);
        CloseHandle(g_initThread);
        g_initThread = nullptr;
    }

    samp_bridge::SyncSampOverlayCursor(false);
    PatchCursor(false);
    SetPlayerControlsBlocked(false);
    ResetPadMouseState();
    CleanupImGui();

    if (g_endSceneTarget) {
        MH_DisableHook(g_endSceneTarget);
        MH_RemoveHook(g_endSceneTarget);
    }
    if (g_presentTarget) {
        MH_DisableHook(g_presentTarget);
        MH_RemoveHook(g_presentTarget);
    }
    if (g_resetTarget) {
        MH_DisableHook(g_resetTarget);
        MH_RemoveHook(g_resetTarget);
    }

    g_originalEndScene = nullptr;
    g_originalPresent = nullptr;
    g_originalReset = nullptr;
    g_endSceneTarget = nullptr;
    g_presentTarget = nullptr;
    g_resetTarget = nullptr;
    g_hooksInstalled.store(false);
    g_attachStarted.store(false);
    g_lastWantCursor = false;
    g_hadNoOverlayUi = true;
    g_stickyMouseCapture = false;
    g_cursorPatchSnapshot.valid = false;
}

bool IsOpen() { return g_menuOpen; }

void SetOpen(bool open) { g_menuOpen = open; }

void Toggle() { g_menuOpen = !g_menuOpen; }

void SetToggleVirtualKey(int vk) { g_toggleVk = vk; }

int GetToggleVirtualKey() { return g_toggleVk; }

void SetHotkeyEnabled(bool enabled) { g_hotkeyEnabled = enabled; }

void SetDrawCallback(DrawFn fn) { g_drawFn = fn; }

} // namespace overlay
