#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Shared ImGui row helpers (implemented in orc_ui.cpp, used by orc_weapons_ui.cpp).
bool OrcUiButtonFullWidth(const char* label);
void OrcUiButtonPair(const char* first, const char* second, bool* firstClicked, bool* secondClicked);
bool OrcUiBeginControlRow(const char* id, const char* label);
void OrcUiEndControlRow();
bool OrcUiCheckbox(const char* id, const char* label, bool* value);
bool OrcUiInputInt(const char* id, const char* label, int* value, int step, int stepFast = 0, int flags = 0);
bool OrcUiDragFloat(const char* id, const char* label, float* value, float speed, float minValue, float maxValue, const char* format);
void OrcUiPedSkinListLabel(char* buf, size_t bufChars, const char* dffName, int modelId);
std::string OrcUiLowerAscii(std::string s);
float OrcUiScaled(float value);
/// Label + "My skin" button + ped skin combo (returns true if selection index changed).
bool OrcUiPedSkinPickerRowWithMySkin(const char* id,
    const char* labelText,
    const std::vector<std::pair<std::string, int>>& pedSkins,
    int* index);
