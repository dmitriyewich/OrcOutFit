#pragma once

namespace overlay {

using DrawFn = void(*)();

// Called from drawingEvent after the game has reached a drawable frame.
// It installs D3D9 hooks once; actual UI rendering is driven by Present/EndScene.
void Init();
void DrawFrame();    // legacy no-op for the existing drawingEvent call site
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
