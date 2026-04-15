#pragma once

namespace samp_bridge {

using ToggleCallback = void(*)();

// Tries to detect SA:MP and install command hook once.
// Safe to call every frame.
void Poll(const char* command, ToggleCallback onToggle);

bool IsSampPresent();
bool IsCommandHookReady();
const char* GetVersionName();

} // namespace samp_bridge

