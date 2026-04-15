#pragma once

namespace overlay {

using DrawFn = void(*)();

// Should be called each frame from drawingEvent.
void Init();         // no-op if already inited or device/hwnd not ready
void DrawFrame();    // NewFrame -> user draw -> Render
void Shutdown();

bool IsOpen();
void SetOpen(bool);
void Toggle();
void SetToggleVirtualKey(int vk);
int  GetToggleVirtualKey();
void SetHotkeyEnabled(bool enabled);

void SetDrawCallback(DrawFn);

// Called by Events::d3dResetEvent to rebuild DX9 objects on lost device.
void OnResetBefore();
void OnResetAfter();

} // namespace overlay
