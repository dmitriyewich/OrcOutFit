#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstring>

#include "overlay.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "game_sa/rw/skeleton.h"
#include "game_sa/CPad.h"

extern "C" void* RwD3D9GetCurrentD3DDevice();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

extern RsGlobalType& RsGlobal;

namespace overlay {

static bool         g_inited        = false;
static bool         g_menuOpen      = false;
static bool         g_prevMenuOpen  = false;
static bool         g_needCreateObj = false;
static HWND         g_hwnd          = nullptr;
static WNDPROC      g_origProc      = nullptr;
static DrawFn       g_drawFn        = nullptr;

// Курсор: патчим call-sites где игра центрует курсор каждый кадр.
// Адреса и байты из известного рецепта для SA 1.0 US.
static bool g_cursorPatched = false;

static inline void WriteBytes(uintptr_t addr, const void* src, size_t n) {
    DWORD oldProt;
    VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &oldProt);
    std::memcpy(reinterpret_cast<void*>(addr), src, n);
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(addr), n, oldProt, &tmp);
}
static inline void WriteByte(uintptr_t a, BYTE v) { WriteBytes(a, &v, 1); }

static void PatchCursor(bool show) {
    if (show == g_cursorPatched) return;
    if (show) {
        // NOP: CControllerConfigManager::AffectPadFromKeyBoard call
        static const BYTE nop5[5] = { 0x90,0x90,0x90,0x90,0x90 };
        WriteBytes(0x541DF5, nop5, 5);
        // NOP: CPad::getMouseState call
        WriteBytes(0x53F417, nop5, 5);
        // test eax,eax / jl -> xor eax,eax / jz
        static const BYTE patch4[4] = { 0x33, 0xC0, 0x0F, 0x84 };
        WriteBytes(0x53F41F, patch4, 4);
        // RsMouseSetPos -> RET
        WriteByte(0x6194A0, 0xC3);
    } else {
        // Restore originals
        static const BYTE o1[5] = { 0xE8, 0x46, 0xF3, 0xFE, 0xFF };
        WriteBytes(0x541DF5, o1, 5);
        static const BYTE o2[5] = { 0xE8, 0xB4, 0x7A, 0x20, 0x00 };
        WriteBytes(0x53F417, o2, 5);
        static const BYTE o3[4] = { 0x85, 0xC0, 0x0F, 0x8C };
        WriteBytes(0x53F41F, o3, 4);
        WriteByte(0x6194A0, 0xE9);
    }
    g_cursorPatched = show;
}
static inline void PatchUpdateMouse(bool e) { PatchCursor(e); }

static bool IsMouseMsg(UINT m) {
    switch (m) {
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            return true;
        default: return false;
    }
}
static bool IsKeyMsg(UINT m) {
    switch (m) {
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR:
            return true;
        default: return false;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_inited && ImGui::GetCurrentContext()) {
        // Global toggle: F7 (on down edge).
        if (m == WM_KEYDOWN && w == VK_F7 && (HIWORD(l) & KF_REPEAT) == 0) {
            g_menuOpen = !g_menuOpen;
            return 0;
        }

        ImGui_ImplWin32_WndProcHandler(h, m, w, l);

        if (g_menuOpen) {
            ImGuiIO& io = ImGui::GetIO();
            if (m == WM_SETCURSOR) {
                SetCursor(LoadCursorA(nullptr, IDC_ARROW));
                return TRUE;
            }
            if ((io.WantCaptureMouse    && IsMouseMsg(m)) ||
                (io.WantCaptureKeyboard && IsKeyMsg(m))) {
                return 0;
            }
        }
    }
    return CallWindowProcA(g_origProc, h, m, w, l);
}

void Init() {
    if (g_inited) return;
    auto* dev = static_cast<IDirect3DDevice9*>(RwD3D9GetCurrentD3DDevice());
    if (!dev) return;
    if (!RsGlobal.ps || !RsGlobal.ps->window) return;
    g_hwnd = RsGlobal.ps->window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;
    st.FrameRounding  = 3.0f;
    st.GrabRounding   = 3.0f;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(dev);

    g_origProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    g_inited = true;
}

void OnResetBefore() {
    if (g_inited) ImGui_ImplDX9_InvalidateDeviceObjects();
}
void OnResetAfter() {
    if (g_inited) g_needCreateObj = true;
}

void DrawFrame() {
    if (!g_inited) return;
    if (g_needCreateObj) {
        ImGui_ImplDX9_CreateDeviceObjects();
        g_needCreateObj = false;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_menuOpen && g_drawFn) g_drawFn();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    // Input / cursor: одинаково в SAMP и в одиночной.
    // Пока зажат ПКМ — отдаём управление игре (look-around), меню видимо.
    bool rmbHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool wantCursor = g_menuOpen && !rmbHeld;
    static bool prevWantCursor = false;

    if (wantCursor != prevWantCursor) {
        PatchCursor(wantCursor);
        ImGui::GetIO().MouseDrawCursor = wantCursor;
        if (auto* dev = static_cast<IDirect3DDevice9*>(RwD3D9GetCurrentD3DDevice()))
            dev->ShowCursor(wantCursor ? TRUE : FALSE);
        CPad::NewMouseControllerState.x = 0;
        CPad::NewMouseControllerState.y = 0;
        reinterpret_cast<void(__cdecl*)()>(0x541BD0)();
        reinterpret_cast<void(__cdecl*)()>(0x541DD0)();
    }
    if (wantCursor) {
        std::memset(&CPad::NewMouseControllerState, 0, sizeof(CMouseControllerState));
        if (CPad* p = CPad::GetPad(0)) p->DisablePlayerControls = 0xFFFF;
    } else {
        if (CPad* p = CPad::GetPad(0)) p->DisablePlayerControls = 0;
    }
    prevWantCursor = wantCursor;
    g_prevMenuOpen = g_menuOpen;
}

void Shutdown() {
    PatchCursor(false);
    if (!g_inited) return;
    if (g_origProc && g_hwnd) {
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origProc));
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_inited   = false;
    g_origProc = nullptr;
    g_hwnd     = nullptr;
}

bool IsOpen()               { return g_menuOpen; }
void SetOpen(bool o)        { g_menuOpen = o; }
void Toggle()               { g_menuOpen = !g_menuOpen; }
void SetDrawCallback(DrawFn f) { g_drawFn = f; }

} // namespace overlay
