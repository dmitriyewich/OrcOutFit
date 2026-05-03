#include "orc_ui.h"

#include "orc_ui_bones.h"
#include "orc_ui_shared.h"
#include "orc_weapons_ui.h"

#include "orc_app.h"
#include "orc_locale.h"

#include "overlay.h"
#include "samp_bridge.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "eWeaponType.h"
#include "ePedType.h"
#include "eModelID.h"
#include "CWeaponInfo.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <utility>
#include <vector>

void OrcUiPedSkinListLabel(char* buf, size_t bufChars, const char* dffName, int modelId) {
    _snprintf_s(buf, bufChars, _TRUNCATE, "%s [%d]", dffName && dffName[0] ? dffName : "?", modelId);
}

static const char* T(OrcTextId id) {
    return OrcText(id);
}

static float UiLayoutScale() {
    const float scale = overlay::GetCurrentUiScale();
    if (!std::isfinite(scale) || scale <= 0.0f)
        return 1.0f;
    return std::max(0.70f, scale);
}

float OrcUiScaled(float value) {
    return value * UiLayoutScale();
}

static float UiControlWidth(float avail) {
    const float minWidth = OrcUiScaled(138.0f);
    const float maxWidth = OrcUiScaled(240.0f);
    return std::min(maxWidth, std::max(minWidth, avail * 0.48f));
}

static float UiEdgeGutter() {
    return std::max(4.0f, OrcUiScaled(6.0f));
}

static float UiContentWidth() {
    return std::max(1.0f, ImGui::GetContentRegionAvail().x - UiEdgeGutter());
}

bool OrcUiButtonFullWidth(const char* label) {
    return ImGui::Button(label, ImVec2(UiContentWidth(), 0.0f));
}

static bool SelectCurrentPedSkinIndex(const std::vector<std::pair<std::string, int>>& pedSkins, int* index) {
    if (!index || pedSkins.empty())
        return false;
    CPlayerPed* ped = FindPlayerPed(0);
    const std::string current = ped ? GetPedStdSkinDffName(ped) : std::string{};
    if (current.empty())
        return false;
    const std::string currentLower = OrcUiLowerAscii(current);
    for (int i = 0; i < (int)pedSkins.size(); ++i) {
        if (OrcUiLowerAscii(pedSkins[(size_t)i].first) == currentLower) {
            if (*index == i)
                return false;
            *index = i;
            return true;
        }
    }
    return false;
}

bool OrcUiPedSkinPickerRowWithMySkin(const char* id,
    const char* labelText,
    const std::vector<std::pair<std::string, int>>& pedSkins,
    int* index) {
    if (!index || pedSkins.empty())
        return false;
    if (*index < 0 || *index >= (int)pedSkins.size())
        *index = 0;

    bool changed = false;
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float avail = UiContentWidth();
    const float spacing = style.ItemSpacing.x;
    const char* mySkinLbl = OrcText(OrcTextId::MySkin);
    const float buttonIdealW =
        std::min(OrcUiScaled(92.0f), std::max(OrcUiScaled(72.0f), ImGui::CalcTextSize(mySkinLbl).x + style.FramePadding.x * 2.0f));
    float comboW = UiControlWidth(avail);
    float buttonW = buttonIdealW;
    float labelW = std::max(1.0f, avail - comboW - buttonW - spacing * 2.0f);
    if (labelW + buttonW + comboW + spacing * 2.0f > avail) {
        const float contentW = std::max(1.0f, avail - spacing * 2.0f);
        labelW = std::max(1.0f, contentW * 0.36f);
        buttonW = std::min(buttonIdealW, std::max(OrcUiScaled(54.0f), contentW * 0.23f));
        comboW = std::max(1.0f, contentW - labelW - buttonW);
    }

    if (ImGui::BeginTable("##ped_skin_combo_current", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings, ImVec2(avail, 0.0f))) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelW);
        ImGui::TableSetupColumn("##mine", ImGuiTableColumnFlags_WidthFixed, buttonW);
        ImGui::TableSetupColumn("##combo", ImGuiTableColumnFlags_WidthFixed, comboW);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(labelText);
        ImGui::PopTextWrapPos();

        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(mySkinLbl, ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), 0.0f)))
            changed = SelectCurrentPedSkinIndex(pedSkins, index) || changed;

        ImGui::TableSetColumnIndex(2);
        const auto& cur = pedSkins[(size_t)*index];
        char comboLbl[192];
        OrcUiPedSkinListLabel(comboLbl, sizeof(comboLbl), cur.first.c_str(), cur.second);
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##value", comboLbl)) {
            CPlayerPed* pl = FindPlayerPed(0);
            const std::string onMe = pl ? GetPedStdSkinDffName(pl) : std::string{};
            for (int i = 0; i < (int)pedSkins.size(); i++) {
                const bool sel = (i == *index);
                const bool onPlayer = !onMe.empty() && OrcUiLowerAscii(pedSkins[(size_t)i].first) == OrcUiLowerAscii(onMe);
                char rowLbl[192];
                OrcUiPedSkinListLabel(rowLbl, sizeof(rowLbl), pedSkins[(size_t)i].first.c_str(), pedSkins[(size_t)i].second);
                if (ImGui::Selectable(rowLbl, sel)) {
                    if (*index != i) {
                        *index = i;
                        changed = true;
                    }
                }
                if (onPlayer) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 mn = ImGui::GetItemRectMin();
                    const ImVec2 mx = ImGui::GetItemRectMax();
                    dl->AddRectFilled(mn, ImVec2(mn.x + OrcUiScaled(3.0f), mx.y), IM_COL32(60, 200, 120, 200), 0.0f);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return changed;
}

static bool UiPedSkinComboWithMySkin(const char* id,
    const char* label,
    const std::vector<std::pair<std::string, int>>& pedSkins,
    int* index) {
    return OrcUiPedSkinPickerRowWithMySkin(id, label, pedSkins, index);
}

static bool OrcUiBeginControlRowEx(const char* id, const char* label, float* outControlWidth = nullptr) {
    ImGui::PushID(id);
    const float avail = UiContentWidth();
    const float controlW = UiControlWidth(avail);
    const float labelW = std::max(1.0f, avail - controlW);

    if (!ImGui::BeginTable("##control_row", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings, ImVec2(avail, 0.0f))) {
        ImGui::PopID();
        return false;
    }
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelW);
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthFixed, controlW);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    ImGui::TableSetColumnIndex(1);
    const float innerControlW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    if (outControlWidth)
        *outControlWidth = innerControlW;
    ImGui::SetNextItemWidth(innerControlW);
    return true;
}

bool OrcUiBeginControlRow(const char* id, const char* label) {
    return OrcUiBeginControlRowEx(id, label, nullptr);
}

void OrcUiEndControlRow() {
    ImGui::EndTable();
    ImGui::PopID();
}

static void UiBeginWideControl(const char* id, const char* label) {
    ImGui::PushID(id);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + UiContentWidth());
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(UiContentWidth());
}

static void UiEndWideControl() {
    ImGui::PopID();
}

bool OrcUiCheckbox(const char* id, const char* label, bool* value) {
    if (!OrcUiBeginControlRow(id, label))
        return false;
    const bool changed = ImGui::Checkbox("##value", value);
    OrcUiEndControlRow();
    return changed;
}

static bool UiRadioButton(const char* id, const char* label, bool active) {
    if (!OrcUiBeginControlRow(id, label))
        return false;
    const bool changed = ImGui::RadioButton("##value", active);
    OrcUiEndControlRow();
    return changed;
}

static bool UiInputText(const char* id, const char* label, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags = 0) {
    if (!OrcUiBeginControlRow(id, label))
        return false;
    const bool changed = ImGui::InputText("##value", buffer, bufferSize, flags);
    OrcUiEndControlRow();
    return changed;
}

static void UiHelpMarker(const char* tooltip) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(tooltip);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

static bool UiInputTextWithHelp(const char* id, const char* label, const char* tooltip, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags = 0) {
    float controlW = 0.0f;
    if (!OrcUiBeginControlRowEx(id, label, &controlW))
        return false;

    const char* marker = "(?)";
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float markerW = ImGui::CalcTextSize(marker).x;
    const float inputW = std::max(OrcUiScaled(72.0f), controlW - spacing - markerW);
    ImGui::SetNextItemWidth(inputW);
    const bool changed = ImGui::InputText("##value", buffer, bufferSize, flags);
    ImGui::SameLine(0.0f, spacing);
    UiHelpMarker(tooltip);
    OrcUiEndControlRow();
    return changed;
}

static bool UiInputTextWithHint(const char* id, const char* label, const char* hint, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags = 0) {
    if (!OrcUiBeginControlRow(id, label))
        return false;
    const bool changed = ImGui::InputTextWithHint("##value", hint, buffer, bufferSize, flags);
    OrcUiEndControlRow();
    return changed;
}

bool OrcUiInputInt(const char* id, const char* label, int* value, int step, int stepFast, ImGuiInputTextFlags flags) {
    if (!OrcUiBeginControlRow(id, label))
        return false;
    const bool changed = ImGui::InputInt("##value", value, step, stepFast, flags);
    OrcUiEndControlRow();
    return changed;
}

static bool UiSliderFloat(const char* id, const char* label, float* value, float minValue, float maxValue, const char* format, ImGuiSliderFlags flags = 0) {
    UiBeginWideControl(id, label);
    const bool changed = ImGui::SliderFloat("##value", value, minValue, maxValue, format, flags);
    UiEndWideControl();
    return changed;
}

static bool UiSliderInt(const char* id, const char* label, int* value, int minValue, int maxValue, const char* format, ImGuiSliderFlags flags = 0) {
    UiBeginWideControl(id, label);
    const bool changed = ImGui::SliderInt("##value", value, minValue, maxValue, format, flags);
    UiEndWideControl();
    return changed;
}

bool OrcUiDragFloat(const char* id, const char* label, float* value, float speed, float minValue, float maxValue, const char* format) {
    UiBeginWideControl(id, label);
    const bool changed = ImGui::DragFloat("##value", value, speed, minValue, maxValue, format);
    UiEndWideControl();
    return changed;
}

static bool UiDragFloat3(const char* id, const char* label, float* values, float speed, float minValue, float maxValue, const char* format) {
    UiBeginWideControl(id, label);
    const bool changed = ImGui::DragFloat3("##value", values, speed, minValue, maxValue, format);
    UiEndWideControl();
    return changed;
}

static bool UiButtonTextFits(const char* label, float width) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f <= width;
}

void OrcUiButtonPair(const char* first, const char* second, bool* firstClicked, bool* secondClicked) {
    *firstClicked = false;
    *secondClicked = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float avail = UiContentWidth();
    const float halfWidth = (avail - spacing) * 0.5f;
    const bool fitsInline = UiButtonTextFits(first, halfWidth) && UiButtonTextFits(second, halfWidth);

    if (fitsInline) {
        if (ImGui::Button(first, ImVec2(halfWidth, 0.0f)))
            *firstClicked = true;
        ImGui::SameLine();
        if (ImGui::Button(second, ImVec2(halfWidth, 0.0f)))
            *secondClicked = true;
        return;
    }

    if (OrcUiButtonFullWidth(first))
        *firstClicked = true;
    if (OrcUiButtonFullWidth(second))
        *secondClicked = true;
}

static constexpr float kMainWindowMargin = 12.0f;
static bool g_mainWindowInitialized = false;
static ImVec2 g_mainWindowPos(60.0f, 40.0f);
static ImVec2 g_mainWindowSize(410.0f, 680.0f);
static float g_mainWindowAppliedScale = 1.0f;
static bool g_mainWindowApplyRect = true;

static void GetMainWindowLimits(const ImVec2& displaySize, float scale, ImVec2& minSize, ImVec2& maxSize) {
    const float margin = kMainWindowMargin * scale;
    maxSize.x = std::max(1.0f, displaySize.x - margin * 2.0f);
    maxSize.y = std::max(1.0f, displaySize.y - margin * 2.0f);
    minSize.x = std::min(340.0f * scale, maxSize.x);
    minSize.y = std::min(520.0f * scale, maxSize.y);
}

static void ClampMainWindowRect(const ImVec2& displaySize, ImVec2& position, ImVec2& size, float scale) {
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f)
        return;

    ImVec2 minSize;
    ImVec2 maxSize;
    GetMainWindowLimits(displaySize, scale, minSize, maxSize);

    size.x = std::min(maxSize.x, std::max(minSize.x, size.x));
    size.y = std::min(maxSize.y, std::max(minSize.y, size.y));

    const float margin = kMainWindowMargin * scale;
    const float maxX = displaySize.x - size.x - margin;
    if (maxX >= margin) {
        position.x = std::min(maxX, std::max(margin, position.x));
    } else {
        position.x = std::max(0.0f, (displaySize.x - size.x) * 0.5f);
    }

    const float maxY = displaySize.y - size.y - margin;
    if (maxY >= margin) {
        position.y = std::min(maxY, std::max(margin, position.y));
    } else {
        position.y = std::max(0.0f, (displaySize.y - size.y) * 0.5f);
    }
}

static bool UiVec2Changed(const ImVec2& before, const ImVec2& after, float epsilon = 0.5f) {
    return std::fabs(before.x - after.x) > epsilon || std::fabs(before.y - after.y) > epsilon;
}

static bool ClampActiveMainWindowDrag(const ImVec2& displaySize, ImVec2& position, ImVec2& size, float scale) {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || !context->MovingWindow || displaySize.x <= 0.0f || displaySize.y <= 0.0f)
        return false;

    ImGuiWindow* movingWindow = context->MovingWindow->RootWindow ? context->MovingWindow->RootWindow : context->MovingWindow;
    if (!movingWindow || context->ActiveId != movingWindow->MoveId || !context->IO.MouseDown[ImGuiMouseButton_Left])
        return false;
    if (!movingWindow->Name || std::strcmp(movingWindow->Name, T(OrcTextId::WindowTitle)) != 0)
        return false;

    ImVec2 targetPos(
        context->IO.MousePos.x - context->ActiveIdClickOffset.x,
        context->IO.MousePos.y - context->ActiveIdClickOffset.y);
    ImVec2 targetSize = movingWindow->SizeFull;
    if (targetSize.x <= 0.0f || targetSize.y <= 0.0f)
        targetSize = size;

    ClampMainWindowRect(displaySize, targetPos, targetSize, scale);
    position = targetPos;
    size = targetSize;
    return true;
}

std::string OrcUiLowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

int g_uiCustomIdx = 0;

int g_uiSkinIdx = 0;
int g_uiSkinEditIdx = -1;
static char g_uiSkinNickBuf[512] = {};
static char g_uiTextureNickBuf[512] = {};

static int g_uiObjSkinListIdx = 0;
static CustomObjectSkinParams g_uiObjParams{};
static bool g_uiObjParamsLoaded = false;
static int g_uiStdObjectModelId = 0;
static int g_uiStdObjectIdx = 0;
static int g_uiStdObjectSkinListIdx = 0;
static CustomObjectSkinParams g_uiStdObjParams{};
static bool g_uiStdObjParamsLoaded = false;
static bool g_uiStdObjectAddFailed = false;

static int g_uiStdSkinListIdx = 0;
static int g_uiStdSkinEditModelId = -1;
static char g_uiStdSkinNickBuf[512] = {};
static int g_uiSkinPreviewSource = SKIN_PREVIEW_STANDARD;
static int g_uiSkinPreviewStdIdx = 0;
static int g_uiSkinPreviewCustomIdx = 0;
static int g_uiSkinPreviewRandomIdx = 0;
static float g_uiSkinPreviewYaw = 25.0f;

static void WeaponFilterEditorParams(CustomObjectSkinParams& obj) {
    ImGui::Separator();
    ImGui::TextUnformatted(T(OrcTextId::WeaponCondition));
    ImGui::TextWrapped("%s", T(OrcTextId::WeaponConditionHint));

    const bool any = !obj.weaponRequireAll;
    if (UiRadioButton("obj_weapon_any", T(OrcTextId::AnySelectedWeapon), any)) obj.weaponRequireAll = false;
    if (UiRadioButton("obj_weapon_all", T(OrcTextId::AllSelectedWeapons), !any)) obj.weaponRequireAll = true;

    OrcUiCheckbox("obj_hide_weapons", T(OrcTextId::HideSelectedWeapons), &obj.hideSelectedWeapons);

    const float childH = OrcUiScaled(140.0f);
    if (ImGui::BeginChild("##obj_weapon_filter_list", ImVec2(UiContentWidth(), childH), true)) {
        for (int wt : g_availableWeaponTypes) {
            if (wt <= 0 || wt >= (int)g_cfg.size()) continue;
            if (!g_cfg[wt].name) continue;
            bool sel = std::find(obj.weaponTypes.begin(), obj.weaponTypes.end(), wt) != obj.weaponTypes.end();
            char lbl[96];
            _snprintf_s(lbl, _TRUNCATE, "%s [%d]", g_cfg[wt].name, wt);
            ImGui::PushID(wt);
            const bool changed = OrcUiCheckbox("weapon_filter", lbl, &sel);
            ImGui::PopID();
            if (changed) {
                if (sel) {
                    if (std::find(obj.weaponTypes.begin(), obj.weaponTypes.end(), wt) == obj.weaponTypes.end())
                        obj.weaponTypes.push_back(wt);
                } else {
                    obj.weaponTypes.erase(std::remove(obj.weaponTypes.begin(), obj.weaponTypes.end(), wt), obj.weaponTypes.end());
                }
            }
        }
    }
    ImGui::EndChild();

    if (OrcUiButtonFullWidth(T(OrcTextId::ClearWeaponSelection))) {
        obj.weaponTypes.clear();
    }
}

void OrcUiDraw() {
    ImGuiIO& io = ImGui::GetIO();
    const float uiScale = UiLayoutScale();
    ImVec2 minSize;
    ImVec2 maxSize;
    GetMainWindowLimits(io.DisplaySize, uiScale, minSize, maxSize);
    bool applyWindowRect = g_mainWindowApplyRect;
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        if (!g_mainWindowInitialized) {
            const float margin = kMainWindowMargin * uiScale;
            g_mainWindowSize.x = std::min(410.0f * uiScale, maxSize.x);
            g_mainWindowSize.y = std::min(680.0f * uiScale, maxSize.y);
            g_mainWindowPos.x = std::max(margin, io.DisplaySize.x - g_mainWindowSize.x - OrcUiScaled(18.0f));
            g_mainWindowPos.y = std::max(margin, OrcUiScaled(40.0f));
            g_mainWindowInitialized = true;
            g_mainWindowAppliedScale = uiScale;
            applyWindowRect = true;
        } else if (std::fabs(uiScale - g_mainWindowAppliedScale) > 0.001f) {
            const float ratio = uiScale / std::max(g_mainWindowAppliedScale, 0.001f);
            g_mainWindowPos.x *= ratio;
            g_mainWindowPos.y *= ratio;
            g_mainWindowSize.x *= ratio;
            g_mainWindowSize.y *= ratio;
            g_mainWindowAppliedScale = uiScale;
            applyWindowRect = true;
        }

        const ImVec2 beforeClampPos = g_mainWindowPos;
        const ImVec2 beforeClampSize = g_mainWindowSize;
        ClampMainWindowRect(io.DisplaySize, g_mainWindowPos, g_mainWindowSize, uiScale);
        applyWindowRect = applyWindowRect || UiVec2Changed(beforeClampPos, g_mainWindowPos) || UiVec2Changed(beforeClampSize, g_mainWindowSize);
        if (ClampActiveMainWindowDrag(io.DisplaySize, g_mainWindowPos, g_mainWindowSize, uiScale))
            applyWindowRect = true;
    }

    if (applyWindowRect) {
        ImGui::SetNextWindowPos(g_mainWindowPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(g_mainWindowSize, ImGuiCond_Always);
        g_mainWindowApplyRect = false;
    }
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);

    bool open = true;
    const ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin(T(OrcTextId::WindowTitle), &open, wflags)) {
        const ImVec2 actualPos = ImGui::GetWindowPos();
        const ImVec2 actualSize = ImGui::GetWindowSize();
        g_mainWindowPos = actualPos;
        g_mainWindowSize = actualSize;
        ClampMainWindowRect(io.DisplaySize, g_mainWindowPos, g_mainWindowSize, uiScale);
        g_mainWindowApplyRect = UiVec2Changed(actualPos, g_mainWindowPos) || UiVec2Changed(actualSize, g_mainWindowSize);
        ImGui::End();
        if (!open) overlay::SetOpen(false);
        return;
    }
    const ImVec2 actualPos = ImGui::GetWindowPos();
    const ImVec2 actualSize = ImGui::GetWindowSize();
    g_mainWindowPos = actualPos;
    g_mainWindowSize = actualSize;
    ClampMainWindowRect(io.DisplaySize, g_mainWindowPos, g_mainWindowSize, uiScale);
    g_mainWindowApplyRect = UiVec2Changed(actualPos, g_mainWindowPos) || UiVec2Changed(actualSize, g_mainWindowSize);
    if (!open) overlay::SetOpen(false);

    if (ImGui::BeginTabBar("OrcOutFitTabs", ImGuiTabBarFlags_None)) {

        // ------------------------------------------------------------------
        // Main
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem(T(OrcTextId::TabMain))) {
            OrcUiCheckbox("plugin_enabled", T(OrcTextId::PluginEnabled), &g_enabled);

            ImGui::Separator();
            ImGui::TextUnformatted(T(OrcTextId::Features));
            OrcUiCheckbox("render_all_peds_weapons", T(OrcTextId::RenderWeaponsForAllPeds), &g_renderAllPedsWeapons);
            OrcUiCheckbox("render_all_peds_objects", T(OrcTextId::RenderObjectsForAllPeds), &g_renderAllPedsObjects);
            if (g_renderAllPedsWeapons || g_renderAllPedsObjects) {
                UiSliderFloat("all_peds_radius", T(OrcTextId::AllPedsRadius), &g_renderAllPedsRadius, 5.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            }
            OrcUiCheckbox("consider_weapon_skills", T(OrcTextId::ConsiderWeaponSkills), &g_considerWeaponSkills);
            OrcUiCheckbox("render_custom_objects", T(OrcTextId::RenderCustomObjects), &g_renderCustomObjects);
            OrcUiCheckbox("render_standard_objects", T(OrcTextId::RenderStandardObjects), &g_renderStandardObjects);
            OrcUiCheckbox("skin_mode", T(OrcTextId::SkinMode), &g_skinModeEnabled);
            OrcUiCheckbox("skin_hide_base_ped", T(OrcTextId::SkinHideBasePed), &g_skinHideBasePed);
            const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
            if (sampNickUiOff)
                ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampNickBinding));
            ImGui::BeginDisabled(sampNickUiOff);
            OrcUiCheckbox("skin_nick_binding", T(OrcTextId::SkinNickBinding), &g_skinNickMode);
            ImGui::EndDisabled();
            OrcUiCheckbox("skin_always_selected", T(OrcTextId::SkinAlwaysSelectedForMe), &g_skinLocalPreferSelected);
            ImGui::TextWrapped("%s", T(OrcTextId::SkinAlwaysSelectedHint));
            int logCombo = static_cast<int>(g_orcLogLevel);
            const OrcTextId logLabels[] = { OrcTextId::LogOff, OrcTextId::LogErrorsOnly, OrcTextId::LogInfoFull };
            if (logCombo < 0) logCombo = 0;
            if (logCombo > 2) logCombo = 2;
            if (OrcUiBeginControlRow("debug_log", T(OrcTextId::DebugLog))) {
                if (ImGui::BeginCombo("##value", T(logLabels[logCombo]))) {
                    for (int i = 0; i < 3; ++i) {
                        if (ImGui::Selectable(T(logLabels[i]), logCombo == i)) {
                            logCombo = i;
                            g_orcLogLevel = static_cast<OrcLogLevel>(logCombo);
                        }
                    }
                    ImGui::EndCombo();
                }
                OrcUiEndControlRow();
            }
            ImGui::TextDisabled("%s", OrcLogGetPath());

            ImGui::Separator();
            if (OrcUiButtonFullWidth(T(OrcTextId::SaveMainFeatures))) {
                SaveMainIni();
                RefreshActivationRouting();
                OrcLogInfo("UI: saved main INI + skin mode flags");
            }
            ImGui::TextWrapped("%s", g_iniPath);
            ImGui::TextWrapped("%s", OrcFormat(OrcTextId::DataPathFormat, g_gameObjDir).c_str());
            ImGui::TextWrapped("%s", OrcFormat(OrcTextId::WeaponsPathFormat, g_gameWeaponsDir).c_str());
            ImGui::TextWrapped("%s", OrcFormat(OrcTextId::SkinsPathFormat, g_gameSkinDir).c_str());

            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        // Weapons
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem(T(OrcTextId::TabWeapons))) {
            OrcWeaponsUiDrawWeaponsTab();
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        // Objects
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem(T(OrcTextId::TabObjects))) {
            if (OrcUiButtonFullWidth(T(OrcTextId::RescanObjects))) {
                DiscoverCustomObjectsAndEnsureIni();
                LoadStandardObjectsFromIni();
                if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
                g_uiObjParamsLoaded = false;
                g_uiStdObjParamsLoaded = false;
            }
            ImGui::Separator();
            if (ImGui::BeginTabBar("OrcOutFitObjectSubTabs", ImGuiTabBarFlags_None)) {
                if (ImGui::BeginTabItem(T(OrcTextId::TabCustomObjects))) {
                    ImGui::TextWrapped("%s", g_gameObjDir);
                    ImGui::Separator();

            if (g_customObjects.empty()) {
                g_livePreviewObjectActive = false;
                g_livePreviewObjectIniPath.clear();
                g_livePreviewObjectSkinDff.clear();
                ImGui::TextDisabled("%s", T(OrcTextId::NoDffObjectsFolder));
                if (OrcUiButtonFullWidth(T(OrcTextId::Rescan)))
                    DiscoverCustomObjectsAndEnsureIni();
            } else {
                if (g_uiCustomIdx < 0 || g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
                auto& obj = g_customObjects[g_uiCustomIdx];

                char oprev[160];
                _snprintf_s(oprev, _TRUNCATE, "%s [%d/%d]", obj.name.c_str(), g_uiCustomIdx + 1, (int)g_customObjects.size());
                if (OrcUiBeginControlRow("objpick", T(OrcTextId::Object))) {
                    if (ImGui::BeginCombo("##value", oprev)) {
                        for (int i = 0; i < (int)g_customObjects.size(); i++) {
                            if (ImGui::Selectable(g_customObjects[i].name.c_str(), i == g_uiCustomIdx)) {
                                g_uiCustomIdx = i;
                                g_uiObjParamsLoaded = false;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    OrcUiEndControlRow();
                }

                std::vector<std::pair<std::string, int>> pedSkins;
                OrcCollectPedSkins(pedSkins);
                if (pedSkins.empty()) {
                    ImGui::TextDisabled("%s", T(OrcTextId::NoPedModelsInCache));
                } else {
                    if (g_uiObjSkinListIdx < 0 || g_uiObjSkinListIdx >= (int)pedSkins.size())
                        g_uiObjSkinListIdx = 0;
                    if (!g_uiObjParamsLoaded) {
                        const std::string& sdff = pedSkins[(size_t)g_uiObjSkinListIdx].first;
                        if (!LoadObjectSkinParamsFromIni(obj.iniPath.c_str(), sdff.c_str(), g_uiObjParams)) {
                            g_uiObjParams = CustomObjectSkinParams{};
                            g_uiObjParams.enabled = true;
                            g_uiObjParams.boneId = BONE_R_THIGH;
                        }
                        g_uiObjParamsLoaded = true;
                    }
                    if (UiPedSkinComboWithMySkin("objskin", T(OrcTextId::PedSkinDffName), pedSkins, &g_uiObjSkinListIdx)) {
                        const std::string& sdff = pedSkins[(size_t)g_uiObjSkinListIdx].first;
                        if (!LoadObjectSkinParamsFromIni(obj.iniPath.c_str(), sdff.c_str(), g_uiObjParams)) {
                            g_uiObjParams = CustomObjectSkinParams{};
                            g_uiObjParams.enabled = true;
                            g_uiObjParams.boneId = BONE_R_THIGH;
                        }
                    }
                }

                OrcUiCheckbox("obj_show", T(OrcTextId::Show), &g_uiObjParams.enabled);

                int bi = OrcUiBoneComboIndex(g_uiObjParams.boneId);
                const OrcUiBoneRow* boneRows = OrcUiBoneRows();
                const char* bonePreview = T(boneRows[bi].label);
                if (OrcUiBeginControlRow("objbone", T(OrcTextId::Bone))) {
                    if (ImGui::BeginCombo("##value", bonePreview)) {
                        for (int i = 0; i < OrcUiBoneRowCount(); i++) {
                            if (ImGui::Selectable(T(boneRows[i].label), i == bi))
                                g_uiObjParams.boneId = boneRows[i].id;
                        }
                        ImGui::EndCombo();
                    }
                    OrcUiEndControlRow();
                }

                OrcUiDragFloat("ox", T(OrcTextId::OffsetX), &g_uiObjParams.x, 0.005f, -2.0f, 2.0f, "%.3f");
                OrcUiDragFloat("oy", T(OrcTextId::OffsetY), &g_uiObjParams.y, 0.005f, -2.0f, 2.0f, "%.3f");
                OrcUiDragFloat("oz", T(OrcTextId::OffsetZ), &g_uiObjParams.z, 0.005f, -2.0f, 2.0f, "%.3f");

                float rxd = g_uiObjParams.rx / D2R, ryd = g_uiObjParams.ry / D2R, rzd = g_uiObjParams.rz / D2R;
                if (OrcUiDragFloat("orx", T(OrcTextId::RotationX), &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.rx = rxd * D2R;
                if (OrcUiDragFloat("ory", T(OrcTextId::RotationY), &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.ry = ryd * D2R;
                if (OrcUiDragFloat("orz", T(OrcTextId::RotationZ), &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.rz = rzd * D2R;

                OrcUiDragFloat("osc", T(OrcTextId::Scale), &g_uiObjParams.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                UiDragFloat3("oscxyz", T(OrcTextId::ScaleXyz), &g_uiObjParams.scaleX, 0.01f, 0.05f, 10.0f, "%.3f");

                WeaponFilterEditorParams(g_uiObjParams);

                ImGui::Separator();
                if (!pedSkins.empty()) {
                    if (OrcUiButtonFullWidth(T(OrcTextId::SaveSkinSectionToObjectIni))) {
                        SaveObjectSkinParamsToIni(obj.iniPath.c_str(), pedSkins[(size_t)g_uiObjSkinListIdx].first.c_str(), g_uiObjParams);
                        g_livePreviewObjectActive = false;
                        g_livePreviewObjectIniPath.clear();
                        g_livePreviewObjectSkinDff.clear();
                    }
                }
                if (!pedSkins.empty() &&
                    g_uiObjSkinListIdx >= 0 &&
                    g_uiObjSkinListIdx < (int)pedSkins.size()) {
                    g_livePreviewObjectActive = true;
                    g_livePreviewObjectIniPath = obj.iniPath;
                    g_livePreviewObjectSkinDff = pedSkins[(size_t)g_uiObjSkinListIdx].first;
                    g_livePreviewObjectParams = g_uiObjParams;
                } else {
                    g_livePreviewObjectActive = false;
                    g_livePreviewObjectIniPath.clear();
                    g_livePreviewObjectSkinDff.clear();
                }
            }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(T(OrcTextId::TabStandardObjects))) {
                    OrcUiInputInt("std_object_id", T(OrcTextId::StandardObjectModelId), &g_uiStdObjectModelId, 1, 10, ImGuiInputTextFlags_CharsDecimal);
                    if (OrcUiButtonFullWidth(T(OrcTextId::AddStandardObject))) {
                        g_uiStdObjectAddFailed = !AddStandardObjectSlot(g_uiStdObjectModelId);
                        if (!g_uiStdObjectAddFailed) {
                            g_uiStdObjectIdx = (int)g_standardObjects.size() - 1;
                            g_uiStdObjParamsLoaded = false;
                        }
                    }
                    if (g_uiStdObjectAddFailed)
                        ImGui::TextDisabled("%s", T(OrcTextId::InvalidStandardObjectModel));

                    ImGui::Separator();
                    std::vector<std::pair<std::string, int>> pedSkins;
                    OrcCollectPedSkins(pedSkins);
                    if (g_standardObjects.empty()) {
                        g_livePreviewStandardObjectActive = false;
                        ImGui::TextDisabled("%s", T(OrcTextId::StandardObjectListEmpty));
                    } else {
                        if (g_uiStdObjectIdx < 0 || g_uiStdObjectIdx >= (int)g_standardObjects.size())
                            g_uiStdObjectIdx = 0;
                        const StandardObjectSlotCfg& obj = g_standardObjects[(size_t)g_uiStdObjectIdx];
                        char preview[80];
                        _snprintf_s(preview, _TRUNCATE, "%d#%d [%d/%d]",
                            obj.modelId, obj.slot, g_uiStdObjectIdx + 1, (int)g_standardObjects.size());
                        if (OrcUiBeginControlRow("std_obj_pick", T(OrcTextId::Object))) {
                            if (ImGui::BeginCombo("##value", preview)) {
                                for (int i = 0; i < (int)g_standardObjects.size(); ++i) {
                                    const StandardObjectSlotCfg& row = g_standardObjects[(size_t)i];
                                    char rowLabel[64];
                                    _snprintf_s(rowLabel, _TRUNCATE, "%d#%d", row.modelId, row.slot);
                                    if (ImGui::Selectable(rowLabel, i == g_uiStdObjectIdx)) {
                                        g_uiStdObjectIdx = i;
                                        g_uiStdObjParamsLoaded = false;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            OrcUiEndControlRow();
                        }

                        if (pedSkins.empty()) {
                            g_livePreviewStandardObjectActive = false;
                            ImGui::TextDisabled("%s", T(OrcTextId::NoPedModelsInCache));
                        } else {
                            if (g_uiStdObjectSkinListIdx < 0 || g_uiStdObjectSkinListIdx >= (int)pedSkins.size())
                                g_uiStdObjectSkinListIdx = 0;
                            if (!g_uiStdObjParamsLoaded) {
                                const std::string& sdff = pedSkins[(size_t)g_uiStdObjectSkinListIdx].first;
                                if (!LoadStandardObjectSkinParamsFromIni(obj.modelId, obj.slot, sdff.c_str(), g_uiStdObjParams)) {
                                    g_uiStdObjParams = CustomObjectSkinParams{};
                                    g_uiStdObjParams.enabled = true;
                                    g_uiStdObjParams.boneId = BONE_R_THIGH;
                                }
                                g_uiStdObjParamsLoaded = true;
                            }
                            if (UiPedSkinComboWithMySkin("std_obj_skin", T(OrcTextId::PedSkinDffName), pedSkins, &g_uiStdObjectSkinListIdx)) {
                                const std::string& sdff = pedSkins[(size_t)g_uiStdObjectSkinListIdx].first;
                                if (!LoadStandardObjectSkinParamsFromIni(obj.modelId, obj.slot, sdff.c_str(), g_uiStdObjParams)) {
                                    g_uiStdObjParams = CustomObjectSkinParams{};
                                    g_uiStdObjParams.enabled = true;
                                    g_uiStdObjParams.boneId = BONE_R_THIGH;
                                }
                            }

                            OrcUiCheckbox("std_obj_show", T(OrcTextId::Show), &g_uiStdObjParams.enabled);
                            int bi = OrcUiBoneComboIndex(g_uiStdObjParams.boneId);
                            const OrcUiBoneRow* stdBoneRows = OrcUiBoneRows();
                            const char* bonePreview = T(stdBoneRows[bi].label);
                            if (OrcUiBeginControlRow("std_obj_bone", T(OrcTextId::Bone))) {
                                if (ImGui::BeginCombo("##value", bonePreview)) {
                                    for (int i = 0; i < OrcUiBoneRowCount(); ++i) {
                                        if (ImGui::Selectable(T(stdBoneRows[i].label), i == bi))
                                            g_uiStdObjParams.boneId = stdBoneRows[i].id;
                                    }
                                    ImGui::EndCombo();
                                }
                                OrcUiEndControlRow();
                            }

                            OrcUiDragFloat("std_ox", T(OrcTextId::OffsetX), &g_uiStdObjParams.x, 0.005f, -2.0f, 2.0f, "%.3f");
                            OrcUiDragFloat("std_oy", T(OrcTextId::OffsetY), &g_uiStdObjParams.y, 0.005f, -2.0f, 2.0f, "%.3f");
                            OrcUiDragFloat("std_oz", T(OrcTextId::OffsetZ), &g_uiStdObjParams.z, 0.005f, -2.0f, 2.0f, "%.3f");

                            float rxd = g_uiStdObjParams.rx / D2R, ryd = g_uiStdObjParams.ry / D2R, rzd = g_uiStdObjParams.rz / D2R;
                            if (OrcUiDragFloat("std_orx", T(OrcTextId::RotationX), &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiStdObjParams.rx = rxd * D2R;
                            if (OrcUiDragFloat("std_ory", T(OrcTextId::RotationY), &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiStdObjParams.ry = ryd * D2R;
                            if (OrcUiDragFloat("std_orz", T(OrcTextId::RotationZ), &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiStdObjParams.rz = rzd * D2R;

                            OrcUiDragFloat("std_osc", T(OrcTextId::Scale), &g_uiStdObjParams.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                            UiDragFloat3("std_oscxyz", T(OrcTextId::ScaleXyz), &g_uiStdObjParams.scaleX, 0.01f, 0.05f, 10.0f, "%.3f");

                            WeaponFilterEditorParams(g_uiStdObjParams);

                            ImGui::Separator();
                            if (OrcUiButtonFullWidth(T(OrcTextId::SaveStandardObjectSkinSection))) {
                                SaveStandardObjectSkinParamsToIni(obj.modelId, obj.slot, pedSkins[(size_t)g_uiStdObjectSkinListIdx].first.c_str(), g_uiStdObjParams);
                                g_livePreviewStandardObjectActive = false;
                            }
                            if (OrcUiButtonFullWidth(T(OrcTextId::RemoveFromList))) {
                                RemoveStandardObjectSlot((size_t)g_uiStdObjectIdx);
                                if (g_uiStdObjectIdx >= (int)g_standardObjects.size())
                                    g_uiStdObjectIdx = (int)g_standardObjects.size() - 1;
                                g_uiStdObjParamsLoaded = false;
                                if (g_standardObjects.empty())
                                    g_livePreviewStandardObjectActive = false;
                            }

                            if (!g_standardObjects.empty()) {
                                const StandardObjectSlotCfg& liveObj = g_standardObjects[(size_t)std::max(0, g_uiStdObjectIdx)];
                                g_livePreviewStandardObjectActive = true;
                                g_livePreviewStandardObjectModelId = liveObj.modelId;
                                g_livePreviewStandardObjectSlot = liveObj.slot;
                                g_livePreviewStandardObjectSkinDff = pedSkins[(size_t)g_uiStdObjectSkinListIdx].first;
                                g_livePreviewStandardObjectParams = g_uiStdObjParams;
                            }
                        }
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        // Skins
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem(T(OrcTextId::TabSkins))) {
            if (ImGui::BeginTabBar("OrcOutFitSkinSubTabs", ImGuiTabBarFlags_None)) {
                if (ImGui::BeginTabItem(T(OrcTextId::TabCustomSkins))) {
                    ImGui::TextWrapped("%s", g_gameSkinDir);
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", T(OrcTextId::CustomSkinsHint));

                    if (g_customSkins.empty()) {
                        ImGui::TextDisabled("%s", T(OrcTextId::NoDffSkinsFolder));
                    } else {
                        if (g_uiSkinIdx < 0 || g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
                        char previewSkin[160];
                        _snprintf_s(previewSkin, _TRUNCATE, "%s [%d/%d]", g_customSkins[g_uiSkinIdx].name.c_str(), g_uiSkinIdx + 1, (int)g_customSkins.size());
                        if (OrcUiBeginControlRow("skinpick", T(OrcTextId::Skin))) {
                            if (ImGui::BeginCombo("##value", previewSkin)) {
                                for (int i = 0; i < (int)g_customSkins.size(); i++) {
                                    if (ImGui::Selectable(g_customSkins[i].name.c_str(), i == g_uiSkinIdx)) {
                                        g_uiSkinIdx = i;
                                        g_skinSelectedName = g_customSkins[i].name;
                                        g_skinSelectedSource = SKIN_SELECTED_CUSTOM;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            OrcUiEndControlRow();
                        }

                        auto& skin = g_customSkins[g_uiSkinIdx];
                        const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
                        if (sampNickUiOff)
                            ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampNickBinding));
                        ImGui::BeginDisabled(sampNickUiOff);
                        if (OrcUiCheckbox("bind_skin_to_nicks", T(OrcTextId::BindSkinToNicks), &skin.bindToNick))
                            InvalidateCustomSkinLookupCache();
                        if (g_uiSkinEditIdx != g_uiSkinIdx) {
                            g_uiSkinEditIdx = g_uiSkinIdx;
                            _snprintf_s(g_uiSkinNickBuf, _TRUNCATE, "%s", skin.nickListCsv.c_str());
                        }
                        if (UiInputTextWithHint("skinnicks", T(OrcTextId::NicksCommaSeparated), T(OrcTextId::NickPlaceholder), g_uiSkinNickBuf, IM_ARRAYSIZE(g_uiSkinNickBuf))) {
                            skin.nickListCsv = g_uiSkinNickBuf;
                            skin.nicknames = ParseNickCsv(skin.nickListCsv);
                            InvalidateCustomSkinLookupCache();
                        }
                        ImGui::EndDisabled();
                        if (OrcUiButtonFullWidth(T(OrcTextId::SaveSkinIni))) {
                            skin.nickListCsv = g_uiSkinNickBuf;
                            skin.nicknames = ParseNickCsv(skin.nickListCsv);
                            InvalidateCustomSkinLookupCache();
                            SaveSkinCfgToIni(skin);
                        }
                    }

                    ImGui::Separator();
                    bool sm = false, rs = false;
                    OrcUiButtonPair(T(OrcTextId::SaveSkinModeSelection), T(OrcTextId::RescanSkins), &sm, &rs);
                    if (sm) SaveSkinModeIni();
                    if (rs) {
                        DiscoverCustomSkins();
                        if (g_uiSkinIdx < (int)g_customSkins.size()) g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(T(OrcTextId::TabStandardSkins))) {
                    ImGui::TextWrapped("%s", T(OrcTextId::StandardSkinsHint));
                    ImGui::Separator();

                    std::vector<std::pair<std::string, int>> pedSkins;
                    OrcCollectPedSkins(pedSkins);
                    if (pedSkins.empty()) {
                        ImGui::TextDisabled("%s", T(OrcTextId::NoPedModelsInCacheReconnect));
                    } else {
                        if (g_standardSkinSelectedModelId >= 0) {
                            for (int i = 0; i < (int)pedSkins.size(); ++i) {
                                if (pedSkins[(size_t)i].second == g_standardSkinSelectedModelId) {
                                    g_uiStdSkinListIdx = i;
                                    break;
                                }
                            }
                        }
                        if (g_uiStdSkinListIdx < 0 || g_uiStdSkinListIdx >= (int)pedSkins.size())
                            g_uiStdSkinListIdx = 0;

                        auto cur = pedSkins[(size_t)g_uiStdSkinListIdx];
                        char comboLbl[192];
                        OrcUiPedSkinListLabel(comboLbl, sizeof(comboLbl), cur.first.c_str(), cur.second);
                        if (OrcUiBeginControlRow("std_skin_pick", T(OrcTextId::Skin))) {
                            if (ImGui::BeginCombo("##value", comboLbl)) {
                                CPlayerPed* pl = FindPlayerPed(0);
                                const std::string onMe = pl ? GetPedStdSkinDffName(pl) : std::string{};
                                for (int i = 0; i < (int)pedSkins.size(); ++i) {
                                    const bool sel = (i == g_uiStdSkinListIdx);
                                    const bool onPlayer = !onMe.empty() && OrcUiLowerAscii(pedSkins[(size_t)i].first) == OrcUiLowerAscii(onMe);
                                    char rowLbl[192];
                                    OrcUiPedSkinListLabel(rowLbl, sizeof(rowLbl), pedSkins[(size_t)i].first.c_str(), pedSkins[(size_t)i].second);
                                    if (ImGui::Selectable(rowLbl, sel)) {
                                        g_uiStdSkinListIdx = i;
                                        g_standardSkinSelectedModelId = pedSkins[(size_t)i].second;
                                        g_skinSelectedSource = SKIN_SELECTED_STANDARD;
                                        g_uiStdSkinEditModelId = -1;
                                        cur = pedSkins[(size_t)g_uiStdSkinListIdx];
                                    }
                                    if (onPlayer) {
                                        ImDrawList* dl = ImGui::GetWindowDrawList();
                                        const ImVec2 mn = ImGui::GetItemRectMin();
                                        const ImVec2 mx = ImGui::GetItemRectMax();
                                        dl->AddRectFilled(mn, ImVec2(mn.x + OrcUiScaled(3.0f), mx.y), IM_COL32(60, 200, 120, 200), 0.0f);
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            OrcUiEndControlRow();
                        }

                        const OrcTextId sourceLabels[] = { OrcTextId::SelectedCustomSkin, OrcTextId::SelectedStandardSkin };
                        if (g_skinSelectedSource != SKIN_SELECTED_STANDARD && g_skinSelectedSource != SKIN_SELECTED_CUSTOM)
                            g_skinSelectedSource = SKIN_SELECTED_CUSTOM;
                        if (OrcUiBeginControlRow("skin_selected_source", T(OrcTextId::SelectedSkinSource))) {
                            if (ImGui::BeginCombo("##value", T(sourceLabels[g_skinSelectedSource]))) {
                                if (ImGui::Selectable(T(OrcTextId::SelectedCustomSkin), g_skinSelectedSource == SKIN_SELECTED_CUSTOM))
                                    g_skinSelectedSource = SKIN_SELECTED_CUSTOM;
                                if (ImGui::Selectable(T(OrcTextId::SelectedStandardSkin), g_skinSelectedSource == SKIN_SELECTED_STANDARD)) {
                                    g_skinSelectedSource = SKIN_SELECTED_STANDARD;
                                    g_standardSkinSelectedModelId = cur.second;
                                }
                                ImGui::EndCombo();
                            }
                            OrcUiEndControlRow();
                        }

                        if (g_uiStdSkinListIdx < 0 || g_uiStdSkinListIdx >= (int)pedSkins.size())
                            g_uiStdSkinListIdx = 0;
                        cur = pedSkins[(size_t)g_uiStdSkinListIdx];
                        if (g_skinSelectedSource == SKIN_SELECTED_STANDARD)
                            g_standardSkinSelectedModelId = cur.second;
                        StandardSkinCfg* skin = OrcGetStandardSkinCfgByModelId(cur.second, true);
                        if (skin) {
                            const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
                            if (sampNickUiOff)
                                ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampNickBinding));
                            ImGui::BeginDisabled(sampNickUiOff);
                            if (OrcUiCheckbox("bind_std_skin_to_nicks", T(OrcTextId::BindSkinToNicks), &skin->bindToNick))
                                InvalidateStandardSkinLookupCache();
                            if (g_uiStdSkinEditModelId != cur.second) {
                                g_uiStdSkinEditModelId = cur.second;
                                _snprintf_s(g_uiStdSkinNickBuf, _TRUNCATE, "%s", skin->nickListCsv.c_str());
                            }
                            if (UiInputTextWithHint("stdskinnicks", T(OrcTextId::NicksCommaSeparated), T(OrcTextId::NickPlaceholder), g_uiStdSkinNickBuf, IM_ARRAYSIZE(g_uiStdSkinNickBuf))) {
                                skin->nickListCsv = g_uiStdSkinNickBuf;
                                skin->nicknames = ParseNickCsv(skin->nickListCsv);
                                InvalidateStandardSkinLookupCache();
                            }
                            ImGui::EndDisabled();

                            if (OrcUiButtonFullWidth(T(OrcTextId::WearThisSkin))) {
                                OrcApplyLocalPlayerModelById(cur.second);
                            }
                            if (OrcUiButtonFullWidth(T(OrcTextId::SaveSkinIni))) {
                                skin->nickListCsv = g_uiStdSkinNickBuf;
                                skin->nicknames = ParseNickCsv(skin->nickListCsv);
                                SaveStandardSkinCfgToIni(*skin);
                            }
                        }
                    }

                    ImGui::Separator();
                    bool sm = false, rs = false;
                    OrcUiButtonPair(T(OrcTextId::SaveSkinModeSelection), T(OrcTextId::RescanSkins), &sm, &rs);
                    if (sm) SaveSkinModeIni();
                    if (rs) {
                        LoadStandardSkinsFromIni();
                        g_uiStdSkinEditModelId = -1;
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(T(OrcTextId::TabSkinPreview))) {
                    ImGui::TextWrapped("%s", T(OrcTextId::SkinPreviewHint));
                    ImGui::Separator();

                    const OrcTextId previewSourceLabels[] = {
                        OrcTextId::SkinPreviewStandard,
                        OrcTextId::SkinPreviewCustom,
                        OrcTextId::SkinPreviewRandom
                    };
                    if (g_uiSkinPreviewSource < SKIN_PREVIEW_STANDARD || g_uiSkinPreviewSource > SKIN_PREVIEW_RANDOM)
                        g_uiSkinPreviewSource = SKIN_PREVIEW_STANDARD;
                    if (OrcUiBeginControlRow("skin_preview_source", T(OrcTextId::SkinPreviewSource))) {
                        if (ImGui::BeginCombo("##value", T(previewSourceLabels[g_uiSkinPreviewSource]))) {
                            for (int i = SKIN_PREVIEW_STANDARD; i <= SKIN_PREVIEW_RANDOM; ++i) {
                                if (ImGui::Selectable(T(previewSourceLabels[i]), g_uiSkinPreviewSource == i))
                                    g_uiSkinPreviewSource = i;
                            }
                            ImGui::EndCombo();
                        }
                        OrcUiEndControlRow();
                    }

                    int previewModelId = -1;
                    int previewVariantIndex = -1;
                    std::string previewName;
                    bool canPreview = false;

                    if (g_uiSkinPreviewSource == SKIN_PREVIEW_STANDARD) {
                        std::vector<std::pair<std::string, int>> pedSkins;
                        OrcCollectPedSkins(pedSkins);
                        if (pedSkins.empty()) {
                            ImGui::TextDisabled("%s", T(OrcTextId::NoPedModelsInCacheReconnect));
                        } else {
                            if (g_uiSkinPreviewStdIdx < 0 || g_uiSkinPreviewStdIdx >= (int)pedSkins.size())
                                g_uiSkinPreviewStdIdx = 0;
                            const auto& cur = pedSkins[(size_t)g_uiSkinPreviewStdIdx];
                            char comboLbl[192];
                            OrcUiPedSkinListLabel(comboLbl, sizeof(comboLbl), cur.first.c_str(), cur.second);
                            if (OrcUiBeginControlRow("skin_preview_standard", T(OrcTextId::Skin))) {
                                if (ImGui::BeginCombo("##value", comboLbl)) {
                                    for (int i = 0; i < (int)pedSkins.size(); ++i) {
                                        char rowLbl[192];
                                        OrcUiPedSkinListLabel(rowLbl, sizeof(rowLbl), pedSkins[(size_t)i].first.c_str(), pedSkins[(size_t)i].second);
                                        if (ImGui::Selectable(rowLbl, i == g_uiSkinPreviewStdIdx))
                                            g_uiSkinPreviewStdIdx = i;
                                    }
                                    ImGui::EndCombo();
                                }
                                OrcUiEndControlRow();
                            }
                            previewModelId = pedSkins[(size_t)g_uiSkinPreviewStdIdx].second;
                            previewName = pedSkins[(size_t)g_uiSkinPreviewStdIdx].first;
                            canPreview = true;
                        }
                    } else if (g_uiSkinPreviewSource == SKIN_PREVIEW_CUSTOM) {
                        if (g_customSkins.empty()) {
                            ImGui::TextDisabled("%s", T(OrcTextId::NoDffSkinsFolder));
                        } else {
                            if (g_uiSkinPreviewCustomIdx < 0 || g_uiSkinPreviewCustomIdx >= (int)g_customSkins.size())
                                g_uiSkinPreviewCustomIdx = 0;
                            const CustomSkinCfg& skin = g_customSkins[(size_t)g_uiSkinPreviewCustomIdx];
                            if (OrcUiBeginControlRow("skin_preview_custom", T(OrcTextId::Skin))) {
                                if (ImGui::BeginCombo("##value", skin.name.c_str())) {
                                    for (int i = 0; i < (int)g_customSkins.size(); ++i) {
                                        if (ImGui::Selectable(g_customSkins[(size_t)i].name.c_str(), i == g_uiSkinPreviewCustomIdx))
                                            g_uiSkinPreviewCustomIdx = i;
                                    }
                                    ImGui::EndCombo();
                                }
                                OrcUiEndControlRow();
                            }
                            previewName = g_customSkins[(size_t)g_uiSkinPreviewCustomIdx].name;
                            canPreview = true;
                        }
                    } else {
                        std::vector<SkinPreviewRandomVariantInfo> variants;
                        OrcCollectRandomSkinPreviewVariants(variants);
                        if (variants.empty()) {
                            ImGui::TextDisabled("%s", T(OrcTextId::NoDffSkinsFolder));
                        } else {
                            if (g_uiSkinPreviewRandomIdx < 0 || g_uiSkinPreviewRandomIdx >= (int)variants.size())
                                g_uiSkinPreviewRandomIdx = 0;
                            const SkinPreviewRandomVariantInfo& variant = variants[(size_t)g_uiSkinPreviewRandomIdx];
                            if (OrcUiBeginControlRow("skin_preview_random", T(OrcTextId::SkinPreviewVariant))) {
                                if (ImGui::BeginCombo("##value", variant.label.c_str())) {
                                    for (int i = 0; i < (int)variants.size(); ++i) {
                                        if (ImGui::Selectable(variants[(size_t)i].label.c_str(), i == g_uiSkinPreviewRandomIdx))
                                            g_uiSkinPreviewRandomIdx = i;
                                    }
                                    ImGui::EndCombo();
                                }
                                OrcUiEndControlRow();
                            }
                            previewModelId = variant.modelId;
                            previewVariantIndex = variant.variantIndex;
                            previewName = variant.label;
                            canPreview = true;
                        }
                    }

                    OrcUiDragFloat("skin_preview_yaw", T(OrcTextId::SkinPreviewYaw), &g_uiSkinPreviewYaw, 0.5f, -180.0f, 180.0f, "%.1f");

                    ImGui::Separator();
                    ImGui::Text("%s", T(OrcTextId::StandardSkinPreview));
                    const int previewW = 512;
                    const int previewH = 768;
                    ImVec2 childSize(UiContentWidth(), ImGui::GetContentRegionAvail().y);
                    if (childSize.y < OrcUiScaled(300.0f))
                        childSize.y = OrcUiScaled(300.0f);
                    if (ImGui::BeginChild("##skin_preview_canvas", childSize, true, ImGuiWindowFlags_NoScrollbar)) {
                        if (canPreview)
                            OrcRequestSkinPreview(g_uiSkinPreviewSource, previewModelId, previewVariantIndex, previewName.c_str(), previewW, previewH, g_uiSkinPreviewYaw);
                        void* previewTexture = canPreview ? OrcGetSkinPreviewTexture() : nullptr;
                        if (previewTexture) {
                            ImVec2 avail = ImGui::GetContentRegionAvail();
                            const float aspect = (float)previewW / (float)previewH;
                            ImVec2 imageSize = avail;
                            if (imageSize.x / std::max(1.0f, imageSize.y) > aspect)
                                imageSize.x = imageSize.y * aspect;
                            else
                                imageSize.y = imageSize.x / aspect;
                            imageSize.x = std::max(1.0f, imageSize.x);
                            imageSize.y = std::max(1.0f, imageSize.y);
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - imageSize.x) * 0.5f));
                            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - imageSize.y) * 0.5f));
                            ImGui::Image((ImTextureID)previewTexture, imageSize);
                        } else {
                            ImGui::TextDisabled("%s", T(OrcTextId::StandardSkinPreviewUnavailable));
                        }
                        ImGui::EndChild();
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(T(OrcTextId::TabRandomSkins))) {
                    OrcUiCheckbox("enable_random_skins", T(OrcTextId::EnableRandomSkins), &g_skinRandomFromPools);
                    ImGui::TextWrapped("%s", T(OrcTextId::RandomSkinsHint));
                    ImGui::Text("%s", OrcFormat(OrcTextId::RandomSkinPoolsFormat, g_skinRandomPoolModels, g_skinRandomPoolVariants).c_str());

                    std::vector<SkinRandomPoolInfo> pools;
                    OrcCollectRandomSkinPools(pools);
                    if (!pools.empty() && ImGui::BeginChild("##random_skin_pools", ImVec2(UiContentWidth(), OrcUiScaled(150.0f)), true)) {
                        for (const auto& pool : pools) {
                            ImGui::Text("%s", OrcFormat(
                                OrcTextId::RandomSkinPoolRowFormat,
                                pool.dffName.c_str(),
                                pool.variants).c_str());
                        }
                        ImGui::EndChild();
                    }

                    bool sm = false, rs = false;
                    OrcUiButtonPair(T(OrcTextId::SaveSkinModeSelection), T(OrcTextId::RescanSkins), &sm, &rs);
                    if (sm) SaveSkinModeIni();
                    if (rs) {
                        DiscoverCustomSkins();
                        LoadStandardSkinsFromIni();
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(T(OrcTextId::TabTexture))) {
                    OrcUiCheckbox("enable_texture_remaps", T(OrcTextId::EnableTextureRemaps), &g_skinTextureRemapEnabled);
                    const bool textureNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
                    if (textureNickUiOff)
                        ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampTextureNickBinding));
                    ImGui::BeginDisabled(textureNickUiOff);
                    OrcUiCheckbox("texture_nick_binding", T(OrcTextId::TextureNickBinding), &g_skinTextureRemapNickMode);
                    OrcUiCheckbox("texture_auto_nick_binding", T(OrcTextId::TextureAutoNickBinding), &g_skinTextureRemapAutoNickMode);
                    ImGui::EndDisabled();
                    const OrcTextId randomModeNames[] = { OrcTextId::RandomModePerTexture, OrcTextId::RandomModeLinkedVariant };
                    if (g_skinTextureRemapRandomMode < TEXTURE_REMAP_RANDOM_PER_TEXTURE ||
                        g_skinTextureRemapRandomMode > TEXTURE_REMAP_RANDOM_LINKED_VARIANT) {
                        g_skinTextureRemapRandomMode = TEXTURE_REMAP_RANDOM_LINKED_VARIANT;
                    }
                    if (OrcUiBeginControlRow("texture_random_mode", T(OrcTextId::RandomMode))) {
                        if (ImGui::BeginCombo("##value", T(randomModeNames[g_skinTextureRemapRandomMode]))) {
                            for (int i = TEXTURE_REMAP_RANDOM_PER_TEXTURE; i <= TEXTURE_REMAP_RANDOM_LINKED_VARIANT; ++i) {
                                if (ImGui::Selectable(T(randomModeNames[i]), g_skinTextureRemapRandomMode == i))
                                    g_skinTextureRemapRandomMode = i;
                            }
                            ImGui::EndCombo();
                        }
                        OrcUiEndControlRow();
                    }
                    ImGui::TextWrapped("%s", T(OrcTextId::TextureRemapHint));
                    if (OrcUiButtonFullWidth(T(OrcTextId::SaveTextureSettings)))
                        SaveSkinModeIni();

                    ImGui::Separator();
                    TextureRemapPedInfo localInfo;
                    if (!OrcGetLocalPedTextureRemaps(localInfo)) {
                        ImGui::TextDisabled("%s", T(OrcTextId::NoLocalPedYet));
                    } else {
                        const char* dff = localInfo.dffName.empty() ? "?" : localInfo.dffName.c_str();
                        ImGui::Text("%s", OrcFormat(OrcTextId::LocalPedFormat, dff, localInfo.modelId).c_str());
                        ImGui::Text("%s", OrcFormat(OrcTextId::TxdSlotFormat, localInfo.txdIndex).c_str());

                        bool randomize = false, original = false;
                        OrcUiButtonPair(T(OrcTextId::RandomizeLocal), T(OrcTextId::OriginalTextures), &randomize, &original);
                        if (randomize) {
                            OrcRandomizeLocalPedTextureRemaps();
                            OrcGetLocalPedTextureRemaps(localInfo);
                        }
                        if (original) {
                            OrcSetAllLocalPedTextureRemaps(-1);
                            OrcGetLocalPedTextureRemaps(localInfo);
                        }

                        if (localInfo.slots.empty()) {
                            ImGui::TextDisabled("%s", T(OrcTextId::NoRemapTexturesFound));
                        } else {
                            for (int i = 0; i < (int)localInfo.slots.size(); ++i) {
                                const TextureRemapSlotInfo& slot = localInfo.slots[(size_t)i];

                                int selected = slot.selected;
                                const char* preview = T(OrcTextId::Original);
                                if (selected >= 0 && selected < (int)slot.remapNames.size())
                                    preview = slot.remapNames[(size_t)selected].c_str();

                                char comboId[32];
                                _snprintf_s(comboId, _TRUNCATE, "##texremap%d", i);
                                if (OrcUiBeginControlRow(comboId, slot.originalName.c_str())) {
                                    if (ImGui::BeginCombo("##value", preview)) {
                                        if (ImGui::Selectable(T(OrcTextId::Original), selected == -1)) {
                                            OrcSetLocalPedTextureRemap(i, -1);
                                            selected = -1;
                                        }
                                        for (int r = 0; r < (int)slot.remapNames.size(); ++r) {
                                            if (ImGui::Selectable(slot.remapNames[(size_t)r].c_str(), selected == r)) {
                                                OrcSetLocalPedTextureRemap(i, r);
                                                selected = r;
                                            }
                                        }
                                        ImGui::EndCombo();
                                    }
                                    OrcUiEndControlRow();
                                }
                            }
                        }

                        ImGui::Separator();
                        UiInputTextWithHint("texturenicks", T(OrcTextId::NickBinding), T(OrcTextId::NickPlaceholder), g_uiTextureNickBuf, IM_ARRAYSIZE(g_uiTextureNickBuf));
                        bool saveBind = false, reloadBind = false;
                        OrcUiButtonPair(T(OrcTextId::SaveCurrentTextureBinding), T(OrcTextId::ReloadTextureBindings), &saveBind, &reloadBind);
                        if (saveBind)
                            OrcSaveLocalPedTextureRemapNickBinding(g_uiTextureNickBuf);
                        if (reloadBind)
                            OrcReloadTextureRemapNickBindings();

                        std::vector<TextureRemapNickBindingInfo> bindings;
                        OrcCollectLocalPedTextureRemapNickBindings(bindings);
                        ImGui::Text("%s", OrcFormat(OrcTextId::TextureNickBindingsCountFormat, (int)bindings.size()).c_str());
                        int deleteBindingId = -1;
                        for (const auto& binding : bindings) {
                            ImGui::Text("%s", OrcFormat(
                                OrcTextId::TextureBindingRowFormat,
                                binding.id,
                                binding.nickListCsv.empty() ? "?" : binding.nickListCsv.c_str(),
                                binding.slotCount).c_str());
                            ImGui::SameLine();
                            char deleteId[64];
                            _snprintf_s(deleteId, _TRUNCATE, "%s##texnick%d", T(OrcTextId::Delete), binding.id);
                            if (ImGui::SmallButton(deleteId))
                                deleteBindingId = binding.id;
                        }
                        if (deleteBindingId >= 0)
                            OrcDeleteLocalPedTextureRemapNickBinding(deleteBindingId);
                    }

                    ImGui::Separator();
                    std::vector<TextureRemapPedInfo> known;
                    OrcCollectPedTextureRemapStats(known);
                    ImGui::Text("%s", OrcFormat(OrcTextId::KnownRemapPedModelsFormat, (int)known.size()).c_str());
                    if (!known.empty() && ImGui::BeginChild("##texture_known_models", ImVec2(UiContentWidth(), OrcUiScaled(110.0f)), true)) {
                        for (const auto& info : known) {
                            const char* dff = info.dffName.empty() ? "?" : info.dffName.c_str();
                            ImGui::Text("%s", OrcFormat(
                                OrcTextId::TextureStatsFormat,
                                dff,
                                info.modelId,
                                info.totalRemapTextures,
                                (int)info.slots.size()).c_str());
                        }
                        ImGui::EndChild();
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        // Settings
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem(T(OrcTextId::TabSettings))) {
            const OrcUiLanguage languages[] = { OrcUiLanguage::Russian, OrcUiLanguage::English };
            if (OrcUiBeginControlRow("language", T(OrcTextId::Language))) {
                if (ImGui::BeginCombo("##value", OrcLanguageDisplayName(g_orcUiLanguage))) {
                    for (OrcUiLanguage language : languages) {
                        const bool selected = language == g_orcUiLanguage;
                        if (ImGui::Selectable(OrcLanguageDisplayName(language), selected)) {
                            g_orcUiLanguage = language;
                            SaveMainIni();
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                OrcUiEndControlRow();
            }

            ImGui::Separator();
            ImGui::TextUnformatted(T(OrcTextId::Interface));
            OrcUiCheckbox("ui_auto_scale", T(OrcTextId::UiAutoScale), &g_uiAutoScale);
            if (UiSliderFloat("ui_scale", T(OrcTextId::UiScale), &g_uiScale, 0.75f, 1.60f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                g_uiScale = std::min(1.60f, std::max(0.75f, g_uiScale));
            int fontSize = static_cast<int>(std::round(g_uiFontSize));
            if (UiSliderInt("ui_font_size", T(OrcTextId::UiFontSize), &fontSize, 13, 22, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                g_uiFontSize = static_cast<float>(fontSize);
            }

            ImGui::Separator();
            ImGui::TextUnformatted(T(OrcTextId::Activation));
            static char actKeyBuf[32] = "F7";
            static bool actKeyBufInited = false;
            if (!actKeyBufInited) {
                _snprintf_s(actKeyBuf, _TRUNCATE, "%s", VkToString(g_activationVk));
                actKeyBufInited = true;
            }
            if (UiInputTextWithHelp("actkey", T(OrcTextId::ToggleKey), T(OrcTextId::ToggleKeyHelp), actKeyBuf, sizeof(actKeyBuf), ImGuiInputTextFlags_CharsNoBlank)) {
                g_activationVk = ParseActivationVk(actKeyBuf);
                RefreshActivationRouting();
            }

            static char cmdBuf[96] = {};
            if (cmdBuf[0] == 0)
                _snprintf_s(cmdBuf, _TRUNCATE, "%s", g_toggleCommand.c_str());
            if (UiInputText("cmd", T(OrcTextId::ChatCommand), cmdBuf, sizeof(cmdBuf))) {
                g_toggleCommand = cmdBuf;
                if (!g_toggleCommand.empty() && g_toggleCommand[0] != '/')
                    g_toggleCommand.insert(g_toggleCommand.begin(), '/');
            }
            if (OrcUiCheckbox("samp_allow_toggle_key", T(OrcTextId::SampAllowToggleKey), &g_sampAllowActivationKey))
                RefreshActivationRouting();

            ImGui::Separator();
            if (OrcUiButtonFullWidth(T(OrcTextId::SaveSettings))) {
                SaveMainIni();
                RefreshActivationRouting();
                OrcLogInfo("UI: saved settings");
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
