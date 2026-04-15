#include "orc_ui.h"

#include "orc_app.h"

#include "overlay.h"
#include "samp_bridge.h"

#include "imgui.h"
#include "eWeaponType.h"

#include <cstdio>

struct BoneOption {
    int id;
    const char* label;
};
static const BoneOption kBones[] = {
    { 0,              "(none)" },
    { 1,              "Root" },
    { BONE_PELVIS,    "Pelvis" },
    { BONE_SPINE1,    "Spine1" },
    { 4,              "Spine" },
    { 5,              "Neck" },
    { 6,              "Head" },
    { BONE_R_CLAVIC,  "R Clavicle" },
    { BONE_R_UPARM,   "R UpperArm" },
    { 23,             "R Forearm" },
    { 24,             "R Hand" },
    { BONE_L_CLAVIC,  "L Clavicle" },
    { BONE_L_UPARM,   "L UpperArm" },
    { 33,             "L Forearm" },
    { 34,             "L Hand" },
    { BONE_L_THIGH,   "L Thigh" },
    { BONE_L_CALF,    "L Calf" },
    { 43,             "L Foot" },
    { BONE_R_THIGH,   "R Thigh" },
    { BONE_R_CALF,    "R Calf" },
    { 53,             "R Foot" },
};

static int g_uiWeaponIdx = WEAPONTYPE_M4;
static int g_uiCustomIdx = 0;

int g_uiSkinIdx = 0;
int g_uiSkinEditIdx = -1;
static char g_uiSkinNickBuf[512] = {};

static int BoneComboIndex(int boneId) {
    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++)
        if (kBones[i].id == boneId) return i;
    return 0;
}

void OrcUiDraw() {
    ImGuiIO& io = ImGui::GetIO();
    const float winW = 440.0f;
    const float winH = 720.0f;
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 620.0f), ImVec2(io.DisplaySize.x - 16.0f, io.DisplaySize.y - 24.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - winW - 20.0f, 48.0f), ImGuiCond_FirstUseEver);

    bool open = true;
    const ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("OrcOutFit", &open, wflags)) {
        ImGui::End();
        if (!open) overlay::SetOpen(false);
        return;
    }
    if (!open) overlay::SetOpen(false);

    auto BtnHalfRow = [](const char* a, const char* b, bool* aClicked, bool* bClicked) {
        *aClicked = *bClicked = false;
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float w = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
        if (ImGui::Button(a, ImVec2(w, 0))) *aClicked = true;
        ImGui::SameLine();
        if (ImGui::Button(b, ImVec2(w, 0))) *bClicked = true;
    };

    if (ImGui::BeginTabBar("OrcOutFitTabs", ImGuiTabBarFlags_None)) {

        if (ImGui::BeginTabItem("Weapons")) {
            ImGui::Checkbox("Plugin enabled", &g_enabled);
            ImGui::Checkbox("Render weapons for all peds", &g_renderAllPedsWeapons);
            if (g_renderAllPedsWeapons) {
                ImGui::TextUnformatted("All peds radius (m)");
                ImGui::PushItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##allpedsrad", &g_renderAllPedsRadius, 5.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::PopItemWidth();
            }

            bool reload = false, rescanObj = false;
            BtnHalfRow("Reload INI", "Rescan objects", &reload, &rescanObj);
            if (reload) {
                LoadConfig();
                DiscoverCustomObjectsAndEnsureIni();
                DiscoverCustomSkins();
            }
            if (rescanObj) {
                DiscoverCustomObjectsAndEnsureIni();
                if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
            }

            ImGui::Separator();
            ImGui::PushItemWidth(-FLT_MIN);
            char preview[64];
            const auto& pc = g_cfg[g_uiWeaponIdx];
            _snprintf_s(preview, _TRUNCATE, "%s [%d]", pc.name ? pc.name : "Weapon", g_uiWeaponIdx);
            ImGui::TextUnformatted("Weapon");
            if (ImGui::BeginCombo("##weapon", preview)) {
                for (int i = 1; i < 64; i++) {
                    if (!g_cfg[i].name) continue;
                    char lbl[64];
                    _snprintf_s(lbl, _TRUNCATE, "%s [%d]", g_cfg[i].name, i);
                    if (ImGui::Selectable(lbl, i == g_uiWeaponIdx)) g_uiWeaponIdx = i;
                }
                ImGui::EndCombo();
            }

            ImGui::TextUnformatted("Weapon slot / id");
            int idx = g_uiWeaponIdx;
            if (ImGui::InputInt("##weaponid", &idx, 1, 1)) {
                if (idx >= 1 && idx < 64) g_uiWeaponIdx = idx;
            }

            ImGui::Separator();
            auto& c = g_cfg[g_uiWeaponIdx];
            ImGui::Checkbox("Show on body", &c.enabled);

            int bi = BoneComboIndex(c.boneId);
            const char* bonePreview = kBones[bi].label;
            ImGui::TextUnformatted("Bone");
            if (ImGui::BeginCombo("##wbone", bonePreview)) {
                for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                    if (ImGui::Selectable(kBones[i].label, i == bi))
                        c.boneId = kBones[i].id;
                }
                ImGui::EndCombo();
            }

            ImGui::TextUnformatted("Offset X");
            ImGui::DragFloat("##wx", &c.x, 0.005f, -2.0f, 2.0f, "%.3f");
            ImGui::TextUnformatted("Offset Y");
            ImGui::DragFloat("##wy", &c.y, 0.005f, -2.0f, 2.0f, "%.3f");
            ImGui::TextUnformatted("Offset Z");
            ImGui::DragFloat("##wz", &c.z, 0.005f, -2.0f, 2.0f, "%.3f");

            float rxd = c.rx / D2R, ryd = c.ry / D2R, rzd = c.rz / D2R;
            ImGui::TextUnformatted("Rotation X (deg)");
            if (ImGui::DragFloat("##wrx", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) c.rx = rxd * D2R;
            ImGui::TextUnformatted("Rotation Y (deg)");
            if (ImGui::DragFloat("##wry", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) c.ry = ryd * D2R;
            ImGui::TextUnformatted("Rotation Z (deg)");
            if (ImGui::DragFloat("##wrz", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) c.rz = rzd * D2R;

            ImGui::TextUnformatted("Scale");
            ImGui::DragFloat("##wsc", &c.scale, 0.01f, 0.05f, 10.0f, "%.3f");
            ImGui::PopItemWidth();

            ImGui::Separator();
            bool saveOne = false, saveAll = false;
            BtnHalfRow("Save weapon", "Save all weapons", &saveOne, &saveAll);
            if (saveOne) {
                SaveWeaponSection(g_uiWeaponIdx);
                char mainBuf[32];
                _snprintf_s(mainBuf, _TRUNCATE, "%d", g_enabled ? 1 : 0);
                WritePrivateProfileStringA("Main", "Enabled", mainBuf, g_iniPath);
                _snprintf_s(mainBuf, _TRUNCATE, "%d", g_renderAllPedsWeapons ? 1 : 0);
                WritePrivateProfileStringA("Main", "RenderAllPedsWeapons", mainBuf, g_iniPath);
                _snprintf_s(mainBuf, _TRUNCATE, "%.0f", g_renderAllPedsRadius);
                WritePrivateProfileStringA("Main", "RenderAllPedsRadius", mainBuf, g_iniPath);
            }
            if (saveAll) {
                for (int i = 1; i < 64; i++) if (g_cfg[i].name || g_cfg[i].boneId) SaveWeaponSection(i);
                char mainBuf[32];
                _snprintf_s(mainBuf, _TRUNCATE, "%d", g_enabled ? 1 : 0);
                WritePrivateProfileStringA("Main", "Enabled", mainBuf, g_iniPath);
                _snprintf_s(mainBuf, _TRUNCATE, "%d", g_renderAllPedsWeapons ? 1 : 0);
                WritePrivateProfileStringA("Main", "RenderAllPedsWeapons", mainBuf, g_iniPath);
                _snprintf_s(mainBuf, _TRUNCATE, "%.0f", g_renderAllPedsRadius);
                WritePrivateProfileStringA("Main", "RenderAllPedsRadius", mainBuf, g_iniPath);
            }

            if (samp_bridge::IsSampBuildKnown()) {
                if (g_sampAllowActivationKey)
                    ImGui::TextDisabled("Toggle: %s  |  %s", g_toggleCommand.c_str(), VkToString(g_activationVk));
                else
                    ImGui::TextWrapped("Toggle (chat): %s", g_toggleCommand.c_str());
            } else {
                ImGui::TextDisabled("Toggle key: %s", VkToString(g_activationVk));
                if (samp_bridge::IsSampPresent())
                    ImGui::TextDisabled("SA:MP build unsupported — SP mode.");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Objects")) {
            ImGui::TextWrapped("%s", g_gameObjDir);
            ImGui::Separator();

            if (g_customObjects.empty()) {
                ImGui::TextDisabled("No *.dff in object folder.");
                if (ImGui::Button("Rescan", ImVec2(-FLT_MIN, 0)))
                    DiscoverCustomObjectsAndEnsureIni();
            } else {
                if (g_uiCustomIdx < 0 || g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
                auto& obj = g_customObjects[g_uiCustomIdx];

                ImGui::PushItemWidth(-FLT_MIN);
                char oprev[160];
                _snprintf_s(oprev, _TRUNCATE, "%s [%d/%d]", obj.name.c_str(), g_uiCustomIdx + 1, (int)g_customObjects.size());
                ImGui::TextUnformatted("Object");
                if (ImGui::BeginCombo("##objpick", oprev)) {
                    for (int i = 0; i < (int)g_customObjects.size(); i++) {
                        if (ImGui::Selectable(g_customObjects[i].name.c_str(), i == g_uiCustomIdx)) g_uiCustomIdx = i;
                    }
                    ImGui::EndCombo();
                }

                ImGui::Checkbox("Show", &obj.enabled);

                int bi = BoneComboIndex(obj.boneId);
                const char* bonePreview = kBones[bi].label;
                ImGui::TextUnformatted("Bone");
                if (ImGui::BeginCombo("##objbone", bonePreview)) {
                    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                        if (ImGui::Selectable(kBones[i].label, i == bi))
                            obj.boneId = kBones[i].id;
                    }
                    ImGui::EndCombo();
                }

                ImGui::TextUnformatted("Offset X");
                ImGui::DragFloat("##ox", &obj.x, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted("Offset Y");
                ImGui::DragFloat("##oy", &obj.y, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted("Offset Z");
                ImGui::DragFloat("##oz", &obj.z, 0.005f, -2.0f, 2.0f, "%.3f");

                float rxd = obj.rx / D2R, ryd = obj.ry / D2R, rzd = obj.rz / D2R;
                ImGui::TextUnformatted("Rotation X (deg)");
                if (ImGui::DragFloat("##orx", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.rx = rxd * D2R;
                ImGui::TextUnformatted("Rotation Y (deg)");
                if (ImGui::DragFloat("##ory", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.ry = ryd * D2R;
                ImGui::TextUnformatted("Rotation Z (deg)");
                if (ImGui::DragFloat("##orz", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.rz = rzd * D2R;

                ImGui::TextUnformatted("Scale");
                ImGui::DragFloat("##osc", &obj.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                ImGui::PopItemWidth();

                ImGui::Separator();
                bool so = false, sao = false;
                BtnHalfRow("Save object", "Save all objects", &so, &sao);
                if (so) SaveCustomObjectIni(obj);
                if (sao) {
                    for (const auto& it : g_customObjects) SaveCustomObjectIni(it);
                }
                if (ImGui::Button("Rescan folder", ImVec2(-FLT_MIN, 0))) {
                    DiscoverCustomObjectsAndEnsureIni();
                    if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Skins")) {
            ImGui::TextWrapped("%s", g_gameSkinDir);
            ImGui::Separator();

            ImGui::Checkbox("Skin mode enabled", &g_skinModeEnabled);
            ImGui::Checkbox("Hide base ped", &g_skinHideBasePed);
            ImGui::Checkbox("Random skin pools (per ped)", &g_skinRandomFromPools);
            ImGui::TextDisabled("SKINS\\random\\<model_name>\\*.dff — folder name = ped model (e.g. wmyclot).");
            if (g_skinRandomPoolModels > 0)
                ImGui::TextDisabled("Loaded: %d model folder(s), %d variant(s).", g_skinRandomPoolModels, g_skinRandomPoolVariants);
            const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
            if (sampNickUiOff)
                ImGui::TextDisabled("Unsupported SA:MP build — nick binding inactive (SP mode).");
            ImGui::BeginDisabled(sampNickUiOff);
            ImGui::Checkbox("Nick binding (SA:MP)", &g_skinNickMode);
            ImGui::Checkbox("My nick uses selected skin", &g_skinLocalPreferSelected);
            ImGui::EndDisabled();

            if (g_customSkins.empty()) {
                ImGui::TextDisabled("No *.dff in SKINS folder.");
            } else {
                if (g_uiSkinIdx < 0 || g_uiSkinIdx >= (int)g_customSkins.size()) g_uiSkinIdx = 0;
                ImGui::PushItemWidth(-FLT_MIN);
                char previewSkin[160];
                _snprintf_s(previewSkin, _TRUNCATE, "%s [%d/%d]", g_customSkins[g_uiSkinIdx].name.c_str(), g_uiSkinIdx + 1, (int)g_customSkins.size());
                ImGui::TextUnformatted("Skin");
                if (ImGui::BeginCombo("##skinpick", previewSkin)) {
                    for (int i = 0; i < (int)g_customSkins.size(); i++) {
                        if (ImGui::Selectable(g_customSkins[i].name.c_str(), i == g_uiSkinIdx)) {
                            g_uiSkinIdx = i;
                            g_skinSelectedName = g_customSkins[i].name;
                        }
                    }
                    ImGui::EndCombo();
                }

                auto& skin = g_customSkins[g_uiSkinIdx];
                ImGui::BeginDisabled(sampNickUiOff);
                ImGui::Checkbox("Bind this skin to nick(s)", &skin.bindToNick);
                if (g_uiSkinEditIdx != g_uiSkinIdx) {
                    g_uiSkinEditIdx = g_uiSkinIdx;
                    _snprintf_s(g_uiSkinNickBuf, _TRUNCATE, "%s", skin.nickListCsv.c_str());
                }
                ImGui::TextWrapped("Nicks: one per line or comma-separated.");
                if (ImGui::InputTextMultiline("##skinnicks", g_uiSkinNickBuf, IM_ARRAYSIZE(g_uiSkinNickBuf), ImVec2(0, 72))) {
                    skin.nickListCsv = g_uiSkinNickBuf;
                    skin.nicknames = ParseNickCsv(skin.nickListCsv);
                }
                ImGui::EndDisabled();
                ImGui::PopItemWidth();
                if (ImGui::Button("Save skin .ini", ImVec2(-FLT_MIN, 0))) {
                    skin.nickListCsv = g_uiSkinNickBuf;
                    skin.nicknames = ParseNickCsv(skin.nickListCsv);
                    SaveSkinCfgToIni(skin);
                }
            }

            ImGui::Separator();
            bool sm = false, rs = false;
            BtnHalfRow("Save skin mode", "Rescan skins", &sm, &rs);
            if (sm) SaveSkinModeIni();
            if (rs) {
                DiscoverCustomSkins();
                if (g_uiSkinIdx < (int)g_customSkins.size()) g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
