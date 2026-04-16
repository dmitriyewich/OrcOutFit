#include "orc_ui.h"

#include "orc_app.h"

#include "overlay.h"
#include "samp_bridge.h"

#include "imgui.h"
#include "eWeaponType.h"
#include "CWeaponInfo.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>

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

static std::string LowerAsciiUi(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

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

static void WeaponFilterEditor(CustomObjectCfg& obj) {
    ImGui::Separator();
    ImGui::TextUnformatted("Weapon condition");
    ImGui::TextWrapped("Select weapon(s) that enable this object. If none selected, object renders always.");

    const bool any = !obj.weaponRequireAll;
    if (ImGui::RadioButton("Any selected weapon", any)) obj.weaponRequireAll = false;
    if (ImGui::RadioButton("All selected weapons", !any)) obj.weaponRequireAll = true;

    ImGui::Checkbox("Hide selected weapon(s) on body when object renders", &obj.hideSelectedWeapons);

    const float childH = 150.0f;
    if (ImGui::BeginChild("##obj_weapon_filter_list", ImVec2(-FLT_MIN, childH), true)) {
        for (int wt : g_availableWeaponTypes) {
            if (wt <= 0 || wt >= (int)g_cfg.size()) continue;
            if (!g_cfg[wt].name) continue;
            bool sel = std::find(obj.weaponTypes.begin(), obj.weaponTypes.end(), wt) != obj.weaponTypes.end();
            char lbl[96];
            _snprintf_s(lbl, _TRUNCATE, "%s [%d]", g_cfg[wt].name, wt);
            if (ImGui::Checkbox(lbl, &sel)) {
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

    if (ImGui::Button("Clear weapon selection", ImVec2(-FLT_MIN, 0))) {
        obj.weaponTypes.clear();
    }
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

    // Shared selection state (Objects->Other and Weapons->per-skin overrides).
    static int g_uiOtherObjIdx = 0;
    static unsigned int g_uiOtherModelKey = 0;

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
            if (ImGui::Checkbox("Consider weapon skills (dual wield)", &g_considerWeaponSkills)) {
                SaveMainIni();
            }

            bool reload = false, rescanObj = false;
            BtnHalfRow("Reload INI", "Rescan objects", &reload, &rescanObj);
            if (reload) {
                LoadConfig();
                DiscoverCustomObjectsAndEnsureIni();
                DiscoverCustomSkins();
                DiscoverOtherOverridesAndObjects();
            }
            if (rescanObj) {
                DiscoverCustomObjectsAndEnsureIni();
                DiscoverOtherOverridesAndObjects();
                if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
            }

            ImGui::Separator();
            // What are we editing right now?
            enum EditTarget { GlobalIni = 0, LocalSkinIni = 1, OtherSkinIni = 2 };
            static int editTarget = GlobalIni;
            ImGui::TextUnformatted("Edit target");
            ImGui::RadioButton("Global (OrcOutFit.ini)", &editTarget, GlobalIni);
            ImGui::RadioButton("Local skin (weapons.ini)", &editTarget, LocalSkinIni);
            ImGui::RadioButton("Other skin (weapons.ini)", &editTarget, OtherSkinIni);

            WeaponCfg* activeArr = g_cfg.empty() ? nullptr : g_cfg.data();
            WeaponCfg* activeArr2 = g_cfg2.empty() ? nullptr : g_cfg2.data();
            SkinOtherOverrides* activeSkin = nullptr;

            if (editTarget == LocalSkinIni) {
                activeSkin = EnsureOtherOverridesForLocalSkin();
                if (activeSkin) { activeArr = activeSkin->weaponCfg.data(); activeArr2 = activeSkin->weaponCfg2.data(); }
            } else if (editTarget == OtherSkinIni) {
                if (g_otherByModelKey.empty()) {
                    ImGui::TextDisabled("No folders in object\\other.");
                } else {
                    std::vector<unsigned int> keys;
                    keys.reserve(g_otherByModelKey.size());
                    for (const auto& kv : g_otherByModelKey) keys.push_back(kv.first);
                    std::sort(keys.begin(), keys.end(), [](unsigned int a, unsigned int b) {
                        auto ita = g_otherByModelKey.find(a);
                        auto itb = g_otherByModelKey.find(b);
                        if (ita == g_otherByModelKey.end() || itb == g_otherByModelKey.end()) return a < b;
                        return LowerAsciiUi(ita->second.skinName) < LowerAsciiUi(itb->second.skinName);
                    });
                    if (g_uiOtherModelKey == 0 || g_otherByModelKey.find(g_uiOtherModelKey) == g_otherByModelKey.end())
                        g_uiOtherModelKey = keys.front();
                    activeSkin = &g_otherByModelKey[g_uiOtherModelKey];
                    activeArr = activeSkin->weaponCfg.data();
                    if (!activeSkin->hasWeaponOverrides) {
                        // Create per-skin configs from current global defaults.
                        activeSkin->weaponCfg = g_cfg;
                        activeSkin->weaponCfg2 = g_cfg2;
                        activeSkin->hasWeaponOverrides = true;
                    }

                    char prevSkin[128];
                    _snprintf_s(prevSkin, _TRUNCATE, "%s", activeSkin->skinName.c_str());
                    ImGui::TextUnformatted("Skin folder");
                    if (ImGui::BeginCombo("##weapon_other_skin_pick", prevSkin)) {
                        for (auto k : keys) {
                            auto& so = g_otherByModelKey[k];
                            const bool isSel = (k == g_uiOtherModelKey);
                            if (ImGui::Selectable(so.skinName.c_str(), isSel)) g_uiOtherModelKey = k;
                        }
                        ImGui::EndCombo();
                    }
                }
            }

            if ((editTarget == LocalSkinIni || editTarget == OtherSkinIni) && !activeSkin) {
                ImGui::TextWrapped("Skin overrides are not available (rescan object\\other / ensure local player).");
            }

            if (activeSkin && (editTarget == LocalSkinIni || editTarget == OtherSkinIni)) {
                ImGui::TextWrapped("Editing: %s", activeSkin->weaponsIniPath.c_str());
            }

            ImGui::Separator();
            ImGui::PushItemWidth(-FLT_MIN);
            static bool g_uiWeaponSecondary = false;
            auto IsDualCapable = [](int wt) -> bool {
                if (wt <= 0 || g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
                CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo((eWeaponType)wt, 1);
                return wi && wi->m_nFlags.bTwinPistol;
            };
            char preview[128];
            const WeaponCfg* pc = (g_uiWeaponIdx >= 0 && g_uiWeaponIdx < (int)g_cfg.size()) ? &g_cfg[g_uiWeaponIdx] : nullptr;
            const int previewModelId = (g_uiWeaponIdx > 0 && g_uiWeaponIdx < (int)g_weaponModelId.size())
                ? (g_uiWeaponSecondary ? g_weaponModelId2[g_uiWeaponIdx] : g_weaponModelId[g_uiWeaponIdx])
                : 0;
            _snprintf_s(preview, _TRUNCATE, "%s%s [%d][%d]",
                         (pc && pc->name) ? pc->name : "Weapon",
                         g_uiWeaponSecondary ? " 2" : "",
                         g_uiWeaponIdx, previewModelId);
            ImGui::TextUnformatted("Weapon");
            if (ImGui::BeginCombo("##weapon", preview)) {
                std::vector<char> localHas;
                localHas.assign(g_cfg.size(), 0);
                CPlayerPed* ped = FindPlayerPed(0);
                if (ped) {
                    for (int s = 0; s < 13; s++) {
                        auto& w = ped->m_aWeapons[s];
                        const int wt = (int)w.m_eWeaponType;
                        if (wt <= 0 || wt >= (int)localHas.size()) continue;
                        CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo(static_cast<eWeaponType>(wt), 1);
                        const bool needsAmmo = wi && wi->m_nSlot >= 2 && wi->m_nSlot <= 9;
                        if (needsAmmo && w.m_nAmmoTotal == 0) continue;
                        localHas[wt] = 1;
                    }
                }
                // Full range list (for inspection): 0..256
                const int maxWtUi = std::min(256, (int)g_cfg.size() - 1);
                for (int wt = 0; wt <= maxWtUi; wt++) {
                    const char* baseName = (wt >= 0 && wt < (int)g_cfg.size() && g_cfg[wt].name) ? g_cfg[wt].name : "Weapon";
                    char lbl[128];
                    const int modelId = (wt > 0 && wt < (int)g_weaponModelId.size()) ? g_weaponModelId[wt] : 0;
                    _snprintf_s(lbl, _TRUNCATE, "%s [%d][%d]", baseName, wt, modelId);
                    const bool hasNow = (wt > 0 && wt < (int)localHas.size() && localHas[wt] != 0);
                    if (ImGui::Selectable(lbl, (wt == g_uiWeaponIdx) && !g_uiWeaponSecondary)) { g_uiWeaponIdx = wt; g_uiWeaponSecondary = false; }
                    if (hasNow) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 mn = ImGui::GetItemRectMin();
                        const ImVec2 mx = ImGui::GetItemRectMax();
                        dl->AddRectFilled(mn, ImVec2(mn.x + 4.0f, mx.y), IM_COL32(60, 200, 120, 160), 0.0f);
                    }
                    if (g_considerWeaponSkills && IsDualCapable(wt)) {
                        char lbl2[128];
                        const int modelId2 = (wt > 0 && wt < (int)g_weaponModelId2.size()) ? g_weaponModelId2[wt] : 0;
                        _snprintf_s(lbl2, _TRUNCATE, "%s 2 [%d][%d]", baseName, wt, modelId2);
                        if (ImGui::Selectable(lbl2, (wt == g_uiWeaponIdx) && g_uiWeaponSecondary)) { g_uiWeaponIdx = wt; g_uiWeaponSecondary = true; }
                        if (hasNow) {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 mn = ImGui::GetItemRectMin();
                            const ImVec2 mx = ImGui::GetItemRectMax();
                            dl->AddRectFilled(mn, ImVec2(mn.x + 4.0f, mx.y), IM_COL32(60, 200, 120, 160), 0.0f);
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TextUnformatted("Weapon slot / id");
            int idx = g_uiWeaponIdx;
            if (ImGui::InputInt("##weaponid", &idx, 1, 1)) {
                if (idx >= 1 && idx < (int)g_cfg.size()) { g_uiWeaponIdx = idx; g_uiWeaponSecondary = false; }
            }

            if (g_considerWeaponSkills && IsDualCapable(g_uiWeaponIdx)) {
                ImGui::Checkbox("Edit second weapon (dual wield)", &g_uiWeaponSecondary);
            } else {
                g_uiWeaponSecondary = false;
            }

            ImGui::Separator();
            WeaponCfg* editingArr = g_uiWeaponSecondary ? activeArr2 : activeArr;
            auto& c = editingArr[g_uiWeaponIdx];
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
            if (editTarget == GlobalIni) {
                bool saveOne = false, saveAll = false;
                BtnHalfRow("Save weapon", "Save all weapons", &saveOne, &saveAll);
                if (saveOne) {
                    if (g_uiWeaponSecondary) SaveWeaponSection2(g_uiWeaponIdx);
                    else SaveWeaponSection(g_uiWeaponIdx);
                    char mainBuf[32];
                    _snprintf_s(mainBuf, _TRUNCATE, "%d", g_enabled ? 1 : 0);
                    WritePrivateProfileStringA("Main", "Enabled", mainBuf, g_iniPath);
                    _snprintf_s(mainBuf, _TRUNCATE, "%d", g_renderAllPedsWeapons ? 1 : 0);
                    WritePrivateProfileStringA("Main", "RenderAllPedsWeapons", mainBuf, g_iniPath);
                    _snprintf_s(mainBuf, _TRUNCATE, "%.0f", g_renderAllPedsRadius);
                    WritePrivateProfileStringA("Main", "RenderAllPedsRadius", mainBuf, g_iniPath);
                    _snprintf_s(mainBuf, _TRUNCATE, "%d", g_considerWeaponSkills ? 1 : 0);
                    WritePrivateProfileStringA("Main", "ConsiderWeaponSkills", mainBuf, g_iniPath);
                }
                if (saveAll) {
                    for (int wt : g_availableWeaponTypes) if (g_cfg[wt].name || g_cfg[wt].boneId) SaveWeaponSection(wt);
                    for (int wt : g_availableWeaponTypes) if (g_cfg2[wt].enabled || g_cfg2[wt].boneId) SaveWeaponSection2(wt);
                    char mainBuf[32];
                    _snprintf_s(mainBuf, _TRUNCATE, "%d", g_enabled ? 1 : 0);
                    WritePrivateProfileStringA("Main", "Enabled", mainBuf, g_iniPath);
                    _snprintf_s(mainBuf, _TRUNCATE, "%d", g_renderAllPedsWeapons ? 1 : 0);
                    WritePrivateProfileStringA("Main", "RenderAllPedsWeapons", mainBuf, g_iniPath);
                    _snprintf_s(mainBuf, _TRUNCATE, "%.0f", g_renderAllPedsRadius);
                    WritePrivateProfileStringA("Main", "RenderAllPedsRadius", mainBuf, g_iniPath);
                    _snprintf_s(mainBuf, _TRUNCATE, "%d", g_considerWeaponSkills ? 1 : 0);
                    WritePrivateProfileStringA("Main", "ConsiderWeaponSkills", mainBuf, g_iniPath);
                }
            } else {
                if (activeSkin) {
                    if (ImGui::Button("Save weapons.ini", ImVec2(-FLT_MIN, 0))) {
                        SaveOtherSkinWeaponsIni(*activeSkin);
                    }
                }
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
            if (ImGui::BeginTabBar("ObjectsLocalOtherTabs", ImGuiTabBarFlags_None)) {
                // ------------------------------------------------------------
                // Local
                // ------------------------------------------------------------
                if (ImGui::BeginTabItem("Local")) {
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
                        if (ImGui::BeginCombo("##objpick_local", oprev)) {
                            for (int i = 0; i < (int)g_customObjects.size(); i++) {
                                if (ImGui::Selectable(g_customObjects[i].name.c_str(), i == g_uiCustomIdx)) g_uiCustomIdx = i;
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::Checkbox("Show", &obj.enabled);

                        int bi = BoneComboIndex(obj.boneId);
                        const char* bonePreview = kBones[bi].label;
                        ImGui::TextUnformatted("Bone");
                        if (ImGui::BeginCombo("##objbone_local", bonePreview)) {
                            for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                                if (ImGui::Selectable(kBones[i].label, i == bi))
                                    obj.boneId = kBones[i].id;
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::TextUnformatted("Offset X");
                        ImGui::DragFloat("##ox_local", &obj.x, 0.005f, -2.0f, 2.0f, "%.3f");
                        ImGui::TextUnformatted("Offset Y");
                        ImGui::DragFloat("##oy_local", &obj.y, 0.005f, -2.0f, 2.0f, "%.3f");
                        ImGui::TextUnformatted("Offset Z");
                        ImGui::DragFloat("##oz_local", &obj.z, 0.005f, -2.0f, 2.0f, "%.3f");

                        float rxd = obj.rx / D2R, ryd = obj.ry / D2R, rzd = obj.rz / D2R;
                        ImGui::TextUnformatted("Rotation X (deg)");
                        if (ImGui::DragFloat("##orx_local", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.rx = rxd * D2R;
                        ImGui::TextUnformatted("Rotation Y (deg)");
                        if (ImGui::DragFloat("##ory_local", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.ry = ryd * D2R;
                        ImGui::TextUnformatted("Rotation Z (deg)");
                        if (ImGui::DragFloat("##orz_local", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.rz = rzd * D2R;

                        ImGui::TextUnformatted("Scale");
                        ImGui::DragFloat("##osc_local", &obj.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                        ImGui::PopItemWidth();

                        WeaponFilterEditor(obj);

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

                // ------------------------------------------------------------
                // Other (per standard skin)
                // ------------------------------------------------------------
                if (ImGui::BeginTabItem("Other")) {
                    ImGui::TextWrapped("object\\other skins");
                    ImGui::Separator();

                    if (g_otherByModelKey.empty()) {
                        ImGui::TextDisabled("No folders in object\\other (nothing to edit).");
                        if (ImGui::Button("Rescan object\\other", ImVec2(-FLT_MIN, 0)))
                            DiscoverOtherOverridesAndObjects();
                    } else {
                        // Build stable selection list
                        std::vector<unsigned int> keys;
                        keys.reserve(g_otherByModelKey.size());
                        for (const auto& kv : g_otherByModelKey) keys.push_back(kv.first);
                        std::sort(keys.begin(), keys.end(), [](unsigned int a, unsigned int b) {
                            // fallback order by pointer-less lookup: empty name means move to the end
                            auto ita = g_otherByModelKey.find(a);
                            auto itb = g_otherByModelKey.find(b);
                            if (ita == g_otherByModelKey.end() || itb == g_otherByModelKey.end()) return a < b;
                            return LowerAsciiUi(ita->second.skinName) < LowerAsciiUi(itb->second.skinName);
                        });

                        if (g_uiOtherModelKey == 0 || g_otherByModelKey.find(g_uiOtherModelKey) == g_otherByModelKey.end())
                            g_uiOtherModelKey = keys.front();

                        SkinOtherOverrides* selSo = nullptr;
                        for (auto k : keys) {
                            if (k == g_uiOtherModelKey) {
                                selSo = &g_otherByModelKey[k];
                                break;
                            }
                        }

                        if (!selSo) {
                            ImGui::TextDisabled("Internal: selected skin not found.");
                        } else {
                            char prevSkin[128];
                            _snprintf_s(prevSkin, _TRUNCATE, "%s", selSo->skinName.c_str());
                            if (ImGui::BeginCombo("##skin_other_pick", prevSkin)) {
                                for (auto k : keys) {
                                    auto& so = g_otherByModelKey[k];
                                    const bool isSel = (k == g_uiOtherModelKey);
                                    if (ImGui::Selectable(so.skinName.c_str(), isSel)) g_uiOtherModelKey = k;
                                }
                                ImGui::EndCombo();
                            }

                            // ------------------------
                            // Objects editor for skin
                            // ------------------------
                            ImGui::Separator();
                            if (ImGui::Button("Rescan this skin objects", ImVec2(-FLT_MIN, 0))) {
                                const std::string keepName = selSo->skinName;
                                DiscoverOtherOverridesAndObjects();
                                g_uiOtherObjIdx = 0;
                                // Restore selection by name if still present
                                for (const auto& kv : g_otherByModelKey) {
                                    if (LowerAsciiUi(kv.second.skinName) == LowerAsciiUi(keepName)) {
                                        g_uiOtherModelKey = kv.first;
                                        break;
                                    }
                                }
                            }
                            if (selSo->objects.empty()) {
                                ImGui::TextDisabled("No *.dff found in this skin folder.");
                            } else {
                                if (g_uiOtherObjIdx < 0 || g_uiOtherObjIdx >= (int)selSo->objects.size()) g_uiOtherObjIdx = 0;
                                auto& obj = selSo->objects[g_uiOtherObjIdx];

                                ImGui::PushItemWidth(-FLT_MIN);
                                char oprev[160];
                                _snprintf_s(oprev, _TRUNCATE, "%s [%d/%d]", obj.name.c_str(), g_uiOtherObjIdx + 1, (int)selSo->objects.size());
                                ImGui::TextUnformatted("Object (Other)");
                                if (ImGui::BeginCombo("##objpick_other", oprev)) {
                                    for (int i = 0; i < (int)selSo->objects.size(); i++) {
                                        if (ImGui::Selectable(selSo->objects[i].name.c_str(), i == g_uiOtherObjIdx))
                                            g_uiOtherObjIdx = i;
                                    }
                                    ImGui::EndCombo();
                                }

                                ImGui::Checkbox("Show##other", &obj.enabled);

                                int bi = BoneComboIndex(obj.boneId);
                                const char* bonePreview = kBones[bi].label;
                                ImGui::TextUnformatted("Bone");
                                if (ImGui::BeginCombo("##objbone_other", bonePreview)) {
                                    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                                        if (ImGui::Selectable(kBones[i].label, i == bi))
                                            obj.boneId = kBones[i].id;
                                    }
                                    ImGui::EndCombo();
                                }

                                ImGui::TextUnformatted("Offset X");
                                ImGui::DragFloat("##ox_other", &obj.x, 0.005f, -2.0f, 2.0f, "%.3f");
                                ImGui::TextUnformatted("Offset Y");
                                ImGui::DragFloat("##oy_other", &obj.y, 0.005f, -2.0f, 2.0f, "%.3f");
                                ImGui::TextUnformatted("Offset Z");
                                ImGui::DragFloat("##oz_other", &obj.z, 0.005f, -2.0f, 2.0f, "%.3f");

                                float rxd = obj.rx / D2R, ryd = obj.ry / D2R, rzd = obj.rz / D2R;
                                ImGui::TextUnformatted("Rotation X (deg)");
                                if (ImGui::DragFloat("##orx_other", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.rx = rxd * D2R;
                                ImGui::TextUnformatted("Rotation Y (deg)");
                                if (ImGui::DragFloat("##ory_other", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.ry = ryd * D2R;
                                ImGui::TextUnformatted("Rotation Z (deg)");
                                if (ImGui::DragFloat("##orz_other", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) obj.rz = rzd * D2R;

                                ImGui::TextUnformatted("Scale");
                                ImGui::DragFloat("##osc_other", &obj.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                                ImGui::PopItemWidth();

                                WeaponFilterEditor(obj);

                                ImGui::Separator();
                                bool so = false, sao = false;
                                BtnHalfRow("Save object##other", "Save all objects##other", &so, &sao);
                                if (so) SaveCustomObjectIni(obj);
                                if (sao) {
                                    for (const auto& it : selSo->objects) SaveCustomObjectIni(it);
                                }
                            }
                        }
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Skins")) {
            ImGui::TextWrapped("%s", g_gameSkinDir);
            ImGui::Separator();

            ImGui::Checkbox("Skin mode enabled", &g_skinModeEnabled);
            ImGui::Checkbox("Hide base ped", &g_skinHideBasePed);
            ImGui::Checkbox("Random skin pools (per ped)", &g_skinRandomFromPools);
            ImGui::TextWrapped("SKINS\\random\\<model_name>\\*.dff — folder name = ped model (e.g. wmyclot).");
            if (g_skinRandomPoolModels > 0)
                ImGui::TextDisabled("Loaded: %d model folder(s), %d variant(s).", g_skinRandomPoolModels, g_skinRandomPoolVariants);
            const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
            if (sampNickUiOff)
                ImGui::TextWrapped("Unsupported SA:MP build — nick binding inactive (SP mode).");
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
                ImGui::TextWrapped("Nicks (comma-separated).");
                if (ImGui::InputTextWithHint("##skinnicks", "Testovik,Walcher_Flett,OtherNick", g_uiSkinNickBuf, IM_ARRAYSIZE(g_uiSkinNickBuf))) {
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
