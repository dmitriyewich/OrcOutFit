#include "orc_ui.h"

#include "orc_app.h"

#include "overlay.h"
#include "samp_bridge.h"

#include "imgui.h"
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
#include <cmath>
#include <utility>
#include <vector>

static void PedSkinListLabel(char* buf, size_t bufChars, const char* dffName, int modelId) {
    _snprintf_s(buf, bufChars, _TRUNCATE, "%s [%d]", dffName && dffName[0] ? dffName : "?", modelId);
}

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
static char g_uiTextureNickBuf[512] = {};

static std::vector<WeaponCfg> g_uiWeapon1;
static std::vector<WeaponCfg> g_uiWeapon2;
static int g_uiWeaponSkinListIdx = 0;
static bool g_uiWeaponBuffersReady = false;

static int g_uiObjSkinListIdx = 0;
static CustomObjectSkinParams g_uiObjParams{};
static bool g_uiObjParamsLoaded = false;

static int BoneComboIndex(int boneId) {
    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++)
        if (kBones[i].id == boneId) return i;
    return 0;
}

static void WeaponFilterEditorParams(CustomObjectSkinParams& obj) {
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

static void SyncWeaponUiBuffersFromSkinPick() {
    std::vector<std::pair<std::string, int>> skins;
    OrcCollectPedSkins(skins);
    if (skins.empty()) {
        g_uiWeapon1 = g_cfg;
        g_uiWeapon2 = g_cfg2;
        g_uiWeaponBuffersReady = true;
        return;
    }
    if (g_uiWeaponSkinListIdx < 0 || g_uiWeaponSkinListIdx >= (int)skins.size())
        g_uiWeaponSkinListIdx = 0;

    const std::string& dff = skins[(size_t)g_uiWeaponSkinListIdx].first;
    char wpath[MAX_PATH];
    if (ResolveWeaponsIniForSkinDff(dff.c_str(), wpath, sizeof(wpath)))
        OrcLoadWeaponPresetFile(wpath, g_uiWeapon1, g_uiWeapon2);
    else {
        g_uiWeapon1 = g_cfg;
        g_uiWeapon2 = g_cfg2;
    }
    for (size_t i = 0; i < g_uiWeapon1.size() && i < g_cfg.size(); i++)
        g_uiWeapon1[i].name = g_cfg[i].name;
    g_uiWeaponBuffersReady = true;
}

static void TryInitWeaponSkinListToLocalPed() {
    std::vector<std::pair<std::string, int>> skins;
    OrcCollectPedSkins(skins);
    if (skins.empty()) return;
    CPlayerPed* ped = FindPlayerPed(0);
    const std::string want = ped ? GetPedStdSkinDffName(ped) : std::string{};
    if (want.empty()) return;
    for (int i = 0; i < (int)skins.size(); i++) {
        if (LowerAsciiUi(skins[(size_t)i].first) == LowerAsciiUi(want)) {
            g_uiWeaponSkinListIdx = i;
            return;
        }
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

    if (ImGui::BeginTabBar("OrcOutFitTabs", ImGuiTabBarFlags_None)) {

        // ------------------------------------------------------------------
        // Main
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Main")) {
            ImGui::Checkbox("Plugin enabled", &g_enabled);
            ImGui::TextUnformatted("Toggle key (SP / optional in SA:MP)");
            ImGui::PushItemWidth(-FLT_MIN);
            static char actKeyBuf[32] = "F7";
            static bool actKeyBufInited = false;
            if (!actKeyBufInited) {
                _snprintf_s(actKeyBuf, _TRUNCATE, "%s", VkToString(g_activationVk));
                actKeyBufInited = true;
            }
            if (ImGui::InputText("##actkey", actKeyBuf, sizeof(actKeyBuf), ImGuiInputTextFlags_CharsNoBlank))
            {
                g_activationVk = ParseActivationVk(actKeyBuf);
                RefreshActivationRouting();
            }

            ImGui::TextUnformatted("Chat command (SA:MP)");
            static char cmdBuf[96] = {};
            if (cmdBuf[0] == 0) _snprintf_s(cmdBuf, _TRUNCATE, "%s", g_toggleCommand.c_str());
            if (ImGui::InputText("##cmd", cmdBuf, sizeof(cmdBuf))) {
                g_toggleCommand = cmdBuf;
                if (!g_toggleCommand.empty() && g_toggleCommand[0] != '/') g_toggleCommand.insert(g_toggleCommand.begin(), '/');
            }
            if (ImGui::Checkbox("SA:MP: also allow toggle key", &g_sampAllowActivationKey))
                RefreshActivationRouting();

            ImGui::Separator();
            ImGui::TextUnformatted("Features");
            ImGui::Checkbox("Render weapons for all peds", &g_renderAllPedsWeapons);
            ImGui::Checkbox("Render objects for all peds", &g_renderAllPedsObjects);
            if (g_renderAllPedsWeapons || g_renderAllPedsObjects) {
                ImGui::SliderFloat("All peds radius (m)", &g_renderAllPedsRadius, 5.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            }
            ImGui::Checkbox("Consider weapon skills (dual wield)", &g_considerWeaponSkills);
            ImGui::Checkbox("Render custom objects (Objects folder)", &g_renderCustomObjects);
            ImGui::Checkbox("Skin mode (custom Skins)", &g_skinModeEnabled);
            ImGui::Checkbox("Skin: hide base ped", &g_skinHideBasePed);
            const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
            if (sampNickUiOff)
                ImGui::TextWrapped("Unsupported SA:MP build — nick binding inactive (SP mode).");
            ImGui::BeginDisabled(sampNickUiOff);
            ImGui::Checkbox("Skin: nick binding (SA:MP)", &g_skinNickMode);
            ImGui::EndDisabled();
            ImGui::Checkbox("Skin: always use selected skin for me", &g_skinLocalPreferSelected);
            ImGui::TextWrapped(
                "If on: your ped uses the skin chosen in the Skins tab (after Save skin mode selection). "
                "Nick-bound skin still wins when nick binding is on and your name matches. "
                "Works even when nick binding is off.");
            int logCombo = static_cast<int>(g_orcLogLevel);
            if (ImGui::Combo("Debug log (OrcOutFit.log)", &logCombo,
                    "Off\0Errors only\0Info (full)\0\0")) {
                if (logCombo < 0) logCombo = 0;
                if (logCombo > 2) logCombo = 2;
                g_orcLogLevel = static_cast<OrcLogLevel>(logCombo);
            }
            ImGui::TextDisabled("%s", OrcLogGetPath());
            ImGui::PopItemWidth();

            ImGui::Separator();
            if (ImGui::Button("Save main / features", ImVec2(-FLT_MIN, 0))) {
                SaveMainIni();
                SaveSkinModeIni();
                RefreshActivationRouting();
                OrcLogInfo("UI: saved main INI + skin mode flags");
            }
            ImGui::TextWrapped("%s", g_iniPath);
            ImGui::TextWrapped("Data: %s", g_gameObjDir);
            ImGui::TextWrapped("Weapons: %s", g_gameWeaponsDir);
            ImGui::TextWrapped("Skins: %s", g_gameSkinDir);

            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        // Weapons
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Weapons")) {
            if (!g_uiWeaponBuffersReady) {
                TryInitWeaponSkinListToLocalPed();
                SyncWeaponUiBuffersFromSkinPick();
            }

            bool reload = false, rescanObj = false;
            BtnHalfRow("Reload INI", "Rescan objects", &reload, &rescanObj);
            if (reload) {
                LoadConfig();
                DiscoverCustomObjectsAndEnsureIni();
                DiscoverCustomSkins();
                SyncWeaponUiBuffersFromSkinPick();
            }
            if (rescanObj) {
                DiscoverCustomObjectsAndEnsureIni();
                if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
            }

            ImGui::Separator();
            std::vector<std::pair<std::string, int>> pedSkins;
            OrcCollectPedSkins(pedSkins);
            ImGui::TextWrapped("Ped skin (editing target) — from ped.dat / LoadPedObject cache");
            if (pedSkins.empty()) {
                ImGui::TextDisabled("No ped models in cache yet (load game world / reconnect).");
            } else {
                if (g_uiWeaponSkinListIdx < 0 || g_uiWeaponSkinListIdx >= (int)pedSkins.size())
                    g_uiWeaponSkinListIdx = 0;
                const auto& cur = pedSkins[(size_t)g_uiWeaponSkinListIdx];
                char comboLbl[192];
                PedSkinListLabel(comboLbl, sizeof(comboLbl), cur.first.c_str(), cur.second);
                if (ImGui::BeginCombo("##wskinpick", comboLbl)) {
                    CPlayerPed* pl = FindPlayerPed(0);
                    const std::string onMe = pl ? GetPedStdSkinDffName(pl) : std::string{};
                    for (int i = 0; i < (int)pedSkins.size(); i++) {
                        const bool sel = (i == g_uiWeaponSkinListIdx);
                        const bool onPlayer = !onMe.empty() && LowerAsciiUi(pedSkins[(size_t)i].first) == LowerAsciiUi(onMe);
                        char rowLbl[192];
                        PedSkinListLabel(rowLbl, sizeof(rowLbl), pedSkins[(size_t)i].first.c_str(), pedSkins[(size_t)i].second);
                        if (ImGui::Selectable(rowLbl, sel)) {
                            g_uiWeaponSkinListIdx = i;
                            SyncWeaponUiBuffersFromSkinPick();
                        }
                        if (onPlayer) {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 mn = ImGui::GetItemRectMin();
                            const ImVec2 mx = ImGui::GetItemRectMax();
                            dl->AddRectFilled(mn, ImVec2(mn.x + 4.0f, mx.y), IM_COL32(60, 200, 120, 200), 0.0f);
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Wear this skin (local player)", ImVec2(-FLT_MIN, 0))) {
                    OrcApplyLocalPlayerModelById(cur.second);
                    SyncWeaponUiBuffersFromSkinPick();
                }
            }
            if (!pedSkins.empty() &&
                g_uiWeaponSkinListIdx >= 0 &&
                g_uiWeaponSkinListIdx < (int)pedSkins.size()) {
                g_livePreviewWeaponsActive = true;
                g_livePreviewWeaponSkinDff = pedSkins[(size_t)g_uiWeaponSkinListIdx].first;
                g_livePreviewWeapon1 = g_uiWeapon1;
                g_livePreviewWeapon2 = g_uiWeapon2;
            } else {
                g_livePreviewWeaponsActive = false;
                g_livePreviewWeaponSkinDff.clear();
                g_livePreviewWeapon1.clear();
                g_livePreviewWeapon2.clear();
            }

            WeaponCfg* activeArr = g_uiWeapon1.empty() ? nullptr : g_uiWeapon1.data();
            WeaponCfg* activeArr2 = g_uiWeapon2.empty() ? nullptr : g_uiWeapon2.data();
            const int activeCount = (int)g_uiWeapon1.size();
            const int activeCount2 = (int)g_uiWeapon2.size();

            ImGui::Separator();
            ImGui::PushItemWidth(-FLT_MIN);
            static bool g_uiWeaponSecondary = false;
            struct WeaponCopyBuf {
                bool valid = false;
                bool secondary = false;
                int wt = 0;
                WeaponCfg cfg{};
            };
            static WeaponCopyBuf g_weaponBuf;
            auto ValidateWeaponCfg = [](const WeaponCfg& c) -> bool {
                auto Fin = [](float v) { return std::isfinite(v) != 0; };
                if (!Fin(c.x) || !Fin(c.y) || !Fin(c.z) || !Fin(c.rx) || !Fin(c.ry) || !Fin(c.rz) || !Fin(c.scale)) return false;
                if (c.scale <= 0.0f || c.scale > 1000.0f) return false;
                if (c.boneId < 0 || c.boneId > 1000) return false;
                if (std::fabs(c.x) > 100.0f || std::fabs(c.y) > 100.0f || std::fabs(c.z) > 100.0f) return false;
                if (std::fabs(c.rx) > 1000.0f || std::fabs(c.ry) > 1000.0f || std::fabs(c.rz) > 1000.0f) return false;
                return true;
            };
            auto IsDualCapable = [](int wt) -> bool {
                if (wt <= 0 || g_cfg.empty() || wt >= (int)g_cfg.size()) return false;
                CWeaponInfo* wi2 = CWeaponInfo::GetWeaponInfo((eWeaponType)wt, 2);
                if (wi2 && wi2->m_nFlags.bTwinPistol) return true;
                CWeaponInfo* wi1 = CWeaponInfo::GetWeaponInfo((eWeaponType)wt, 1);
                return wi1 && wi1->m_nFlags.bTwinPistol;
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
            const int editingCount = g_uiWeaponSecondary ? activeCount2 : activeCount;
            const bool canEdit = (editingArr != nullptr && g_uiWeaponIdx >= 0 && g_uiWeaponIdx < editingCount);
            if (!canEdit) {
                ImGui::TextDisabled("Weapon editor is not available.");
                ImGui::PopItemWidth();
            } else {
                auto& c = editingArr[g_uiWeaponIdx];
                ImGui::Checkbox("Show on body", &c.enabled);
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy")) {
                    g_weaponBuf.valid = true;
                    g_weaponBuf.secondary = g_uiWeaponSecondary;
                    g_weaponBuf.wt = g_uiWeaponIdx;
                    g_weaponBuf.cfg = c;
                    g_weaponBuf.cfg.name = nullptr;
                }
                ImGui::SameLine();
                const bool canPaste = g_weaponBuf.valid && ValidateWeaponCfg(g_weaponBuf.cfg);
                if (!canPaste) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Paste")) {
                    const char* keepName = c.name;
                    c = g_weaponBuf.cfg;
                    c.name = keepName;
                }
                if (!canPaste) ImGui::EndDisabled();

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
                if (ImGui::Button("Save to Global (OrcOutFit.ini)", ImVec2(-FLT_MIN, 0))) {
                    g_cfg = g_uiWeapon1;
                    g_cfg2 = g_uiWeapon2;
                    SaveAllWeaponsToIniFile(g_iniPath, g_cfg, g_cfg2);
                    InvalidatePerSkinWeaponCache();
                    SyncWeaponUiBuffersFromSkinPick();
                    g_livePreviewWeaponsActive = false;
                    g_livePreviewWeaponSkinDff.clear();
                    g_livePreviewWeapon1.clear();
                    g_livePreviewWeapon2.clear();
                }
                CPlayerPed* pl = FindPlayerPed(0);
                // PLAYER1 stays set for the local ped even after SetModelIndex (Wear this skin);
                // only block true story CJ in single-player (per-skin Weapons\<dff>.ini does not apply to default CJ).
                const bool blockSkinSave = pl && pl->m_nPedType == PED_TYPE_PLAYER1 &&
                    (int)pl->m_nModelIndex == MODEL_PLAYER && !samp_bridge::IsSampPresent();
                if (blockSkinSave) {
                    ImGui::TextDisabled("Per-skin preset: disabled for single-player CJ only. Use SA:MP or Wear this skin to change model.");
                } else if (!pedSkins.empty()) {
                    if (ImGui::Button("Save to skin (OrcOutFit\\Weapons)", ImVec2(-FLT_MIN, 0))) {
                        const std::string& dff = pedSkins[(size_t)g_uiWeaponSkinListIdx].first;
                        char outPath[MAX_PATH];
                        _snprintf_s(outPath, _TRUNCATE, "%s\\%s.ini", g_gameWeaponsDir, dff.c_str());
                        SaveAllWeaponsToIniFile(outPath, g_uiWeapon1, g_uiWeapon2);
                        InvalidatePerSkinWeaponCache();
                        g_livePreviewWeaponsActive = false;
                        g_livePreviewWeaponSkinDff.clear();
                        g_livePreviewWeapon1.clear();
                        g_livePreviewWeapon2.clear();
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

        // ------------------------------------------------------------------
        // Objects
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Objects")) {
            ImGui::TextWrapped("%s", g_gameObjDir);
            ImGui::Separator();

            if (g_customObjects.empty()) {
                g_livePreviewObjectActive = false;
                g_livePreviewObjectIniPath.clear();
                g_livePreviewObjectSkinDff.clear();
                ImGui::TextDisabled("No *.dff in Objects folder.");
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
                        if (ImGui::Selectable(g_customObjects[i].name.c_str(), i == g_uiCustomIdx)) {
                            g_uiCustomIdx = i;
                            g_uiObjParamsLoaded = false;
                        }
                    }
                    ImGui::EndCombo();
                }

                std::vector<std::pair<std::string, int>> pedSkins;
                OrcCollectPedSkins(pedSkins);
                ImGui::TextUnformatted("Ped skin (DFF name)");
                if (pedSkins.empty()) {
                    ImGui::TextDisabled("No ped models in cache yet.");
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
                    const auto& curS = pedSkins[(size_t)g_uiObjSkinListIdx];
                    char objSkinCombo[192];
                    PedSkinListLabel(objSkinCombo, sizeof(objSkinCombo), curS.first.c_str(), curS.second);
                    if (ImGui::BeginCombo("##objskin", objSkinCombo)) {
                        CPlayerPed* pl = FindPlayerPed(0);
                        const std::string onMe = pl ? GetPedStdSkinDffName(pl) : std::string{};
                        for (int i = 0; i < (int)pedSkins.size(); i++) {
                            const bool sel = (i == g_uiObjSkinListIdx);
                            const bool onPlayer = !onMe.empty() && LowerAsciiUi(pedSkins[(size_t)i].first) == LowerAsciiUi(onMe);
                            char rowLbl[192];
                            PedSkinListLabel(rowLbl, sizeof(rowLbl), pedSkins[(size_t)i].first.c_str(), pedSkins[(size_t)i].second);
                            if (ImGui::Selectable(rowLbl, sel)) {
                                g_uiObjSkinListIdx = i;
                                const std::string& sdff = pedSkins[(size_t)i].first;
                                if (!LoadObjectSkinParamsFromIni(obj.iniPath.c_str(), sdff.c_str(), g_uiObjParams)) {
                                    g_uiObjParams = CustomObjectSkinParams{};
                                    g_uiObjParams.enabled = true;
                                    g_uiObjParams.boneId = BONE_R_THIGH;
                                }
                            }
                            if (onPlayer) {
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                const ImVec2 mn = ImGui::GetItemRectMin();
                                const ImVec2 mx = ImGui::GetItemRectMax();
                                dl->AddRectFilled(mn, ImVec2(mn.x + 4.0f, mx.y), IM_COL32(60, 200, 120, 200), 0.0f);
                            }
                        }
                        ImGui::EndCombo();
                    }
                }

                ImGui::Checkbox("Show", &g_uiObjParams.enabled);

                int bi = BoneComboIndex(g_uiObjParams.boneId);
                const char* bonePreview = kBones[bi].label;
                ImGui::TextUnformatted("Bone");
                if (ImGui::BeginCombo("##objbone", bonePreview)) {
                    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                        if (ImGui::Selectable(kBones[i].label, i == bi))
                            g_uiObjParams.boneId = kBones[i].id;
                    }
                    ImGui::EndCombo();
                }

                ImGui::TextUnformatted("Offset X");
                ImGui::DragFloat("##ox", &g_uiObjParams.x, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted("Offset Y");
                ImGui::DragFloat("##oy", &g_uiObjParams.y, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted("Offset Z");
                ImGui::DragFloat("##oz", &g_uiObjParams.z, 0.005f, -2.0f, 2.0f, "%.3f");

                float rxd = g_uiObjParams.rx / D2R, ryd = g_uiObjParams.ry / D2R, rzd = g_uiObjParams.rz / D2R;
                ImGui::TextUnformatted("Rotation X (deg)");
                if (ImGui::DragFloat("##orx", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.rx = rxd * D2R;
                ImGui::TextUnformatted("Rotation Y (deg)");
                if (ImGui::DragFloat("##ory", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.ry = ryd * D2R;
                ImGui::TextUnformatted("Rotation Z (deg)");
                if (ImGui::DragFloat("##orz", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.rz = rzd * D2R;

                ImGui::TextUnformatted("Scale");
                ImGui::DragFloat("##osc", &g_uiObjParams.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                ImGui::TextUnformatted("Scale X/Y/Z");
                ImGui::DragFloat3("##oscxyz", &g_uiObjParams.scaleX, 0.01f, 0.05f, 10.0f, "%.3f");
                ImGui::PopItemWidth();

                WeaponFilterEditorParams(g_uiObjParams);

                ImGui::Separator();
                if (!pedSkins.empty()) {
                    if (ImGui::Button("Save [Skin.*] to object .ini", ImVec2(-FLT_MIN, 0))) {
                        SaveObjectSkinParamsToIni(obj.iniPath.c_str(), pedSkins[(size_t)g_uiObjSkinListIdx].first.c_str(), g_uiObjParams);
                        g_livePreviewObjectActive = false;
                        g_livePreviewObjectIniPath.clear();
                        g_livePreviewObjectSkinDff.clear();
                    }
                }
                if (ImGui::Button("Rescan Objects folder", ImVec2(-FLT_MIN, 0))) {
                    DiscoverCustomObjectsAndEnsureIni();
                    if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
                    g_uiObjParamsLoaded = false;
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

        // ------------------------------------------------------------------
        // Skins
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Skins")) {
            if (ImGui::BeginTabBar("OrcOutFitSkinSubTabs", ImGuiTabBarFlags_None)) {
                if (ImGui::BeginTabItem("Custom skins")) {
                    ImGui::TextWrapped("%s", g_gameSkinDir);
                    ImGui::Separator();
                    ImGui::TextWrapped("Custom skins - DFF/TXD in Skins folder. Random pools UI is disabled until implemented.");

                    if (g_customSkins.empty()) {
                        ImGui::TextDisabled("No *.dff in Skins folder.");
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
                        const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
                        if (sampNickUiOff)
                            ImGui::TextWrapped("Unsupported SA:MP build - nick binding inactive (SP mode).");
                        ImGui::BeginDisabled(sampNickUiOff);
                        ImGui::Checkbox("Bind this skin to nick(s)", &skin.bindToNick);
                        if (g_uiSkinEditIdx != g_uiSkinIdx) {
                            g_uiSkinEditIdx = g_uiSkinIdx;
                            _snprintf_s(g_uiSkinNickBuf, _TRUNCATE, "%s", skin.nickListCsv.c_str());
                        }
                        ImGui::TextWrapped("Nicks (comma-separated).");
                        if (ImGui::InputTextWithHint("##skinnicks", "Nick1,Nick2", g_uiSkinNickBuf, IM_ARRAYSIZE(g_uiSkinNickBuf))) {
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
                    BtnHalfRow("Save skin mode selection", "Rescan skins", &sm, &rs);
                    if (sm) SaveSkinModeIni();
                    if (rs) {
                        DiscoverCustomSkins();
                        if (g_uiSkinIdx < (int)g_customSkins.size()) g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Texture")) {
                    ImGui::Checkbox("Enable texture remaps (*_remap)", &g_skinTextureRemapEnabled);
                    const bool textureNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
                    if (textureNickUiOff)
                        ImGui::TextWrapped("Unsupported SA:MP build - texture nick binding inactive.");
                    ImGui::BeginDisabled(textureNickUiOff);
                    ImGui::Checkbox("Texture nick binding (SA:MP)", &g_skinTextureRemapNickMode);
                    ImGui::EndDisabled();
                    const char* randomModeNames[] = { "Per texture", "Linked variant" };
                    if (g_skinTextureRemapRandomMode < TEXTURE_REMAP_RANDOM_PER_TEXTURE ||
                        g_skinTextureRemapRandomMode > TEXTURE_REMAP_RANDOM_LINKED_VARIANT) {
                        g_skinTextureRemapRandomMode = TEXTURE_REMAP_RANDOM_LINKED_VARIANT;
                    }
                    ImGui::PushItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("Random mode", randomModeNames[g_skinTextureRemapRandomMode])) {
                        for (int i = TEXTURE_REMAP_RANDOM_PER_TEXTURE; i <= TEXTURE_REMAP_RANDOM_LINKED_VARIANT; ++i) {
                            if (ImGui::Selectable(randomModeNames[i], g_skinTextureRemapRandomMode == i))
                                g_skinTextureRemapRandomMode = i;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();
                    ImGui::TextWrapped("PedFuncs-style remap works on standard ped TXDs loaded by the game.");
                    if (ImGui::Button("Save texture settings", ImVec2(-FLT_MIN, 0)))
                        SaveSkinModeIni();

                    ImGui::Separator();
                    TextureRemapPedInfo localInfo;
                    if (!OrcGetLocalPedTextureRemaps(localInfo)) {
                        ImGui::TextDisabled("No local ped yet.");
                    } else {
                        const char* dff = localInfo.dffName.empty() ? "?" : localInfo.dffName.c_str();
                        ImGui::Text("Local ped: %s [%d]", dff, localInfo.modelId);
                        ImGui::Text("TXD slot: %d", localInfo.txdIndex);

                        bool randomize = false, original = false;
                        BtnHalfRow("Randomize local", "Original textures", &randomize, &original);
                        if (randomize) {
                            OrcRandomizeLocalPedTextureRemaps();
                            OrcGetLocalPedTextureRemaps(localInfo);
                        }
                        if (original) {
                            OrcSetAllLocalPedTextureRemaps(-1);
                            OrcGetLocalPedTextureRemaps(localInfo);
                        }

                        if (localInfo.slots.empty()) {
                            ImGui::TextDisabled("No *_remap textures found in the loaded TXD.");
                        } else {
                            ImGui::PushItemWidth(-FLT_MIN);
                            for (int i = 0; i < (int)localInfo.slots.size(); ++i) {
                                const TextureRemapSlotInfo& slot = localInfo.slots[(size_t)i];
                                ImGui::TextUnformatted(slot.originalName.c_str());

                                int selected = slot.selected;
                                const char* preview = "Original";
                                if (selected >= 0 && selected < (int)slot.remapNames.size())
                                    preview = slot.remapNames[(size_t)selected].c_str();

                                char comboId[32];
                                _snprintf_s(comboId, _TRUNCATE, "##texremap%d", i);
                                if (ImGui::BeginCombo(comboId, preview)) {
                                    if (ImGui::Selectable("Original", selected == -1)) {
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
                            }
                            ImGui::PopItemWidth();
                        }

                        ImGui::Separator();
                        ImGui::TextUnformatted("Nick binding");
                        ImGui::PushItemWidth(-FLT_MIN);
                        ImGui::InputTextWithHint("##texturenicks", "Nick1,Nick2", g_uiTextureNickBuf, IM_ARRAYSIZE(g_uiTextureNickBuf));
                        ImGui::PopItemWidth();
                        bool saveBind = false, reloadBind = false;
                        BtnHalfRow("Save current texture binding", "Reload texture bindings", &saveBind, &reloadBind);
                        if (saveBind)
                            OrcSaveLocalPedTextureRemapNickBinding(g_uiTextureNickBuf);
                        if (reloadBind)
                            OrcReloadTextureRemapNickBindings();

                        std::vector<TextureRemapNickBindingInfo> bindings;
                        OrcCollectLocalPedTextureRemapNickBindings(bindings);
                        ImGui::Text("Nick bindings for this ped: %d", (int)bindings.size());
                        int deleteBindingId = -1;
                        for (const auto& binding : bindings) {
                            ImGui::Text("#%d: %s (%d slot(s))",
                                        binding.id,
                                        binding.nickListCsv.empty() ? "?" : binding.nickListCsv.c_str(),
                                        binding.slotCount);
                            ImGui::SameLine();
                            char deleteId[32];
                            _snprintf_s(deleteId, _TRUNCATE, "Delete##texnick%d", binding.id);
                            if (ImGui::SmallButton(deleteId))
                                deleteBindingId = binding.id;
                        }
                        if (deleteBindingId >= 0)
                            OrcDeleteLocalPedTextureRemapNickBinding(deleteBindingId);
                    }

                    ImGui::Separator();
                    std::vector<TextureRemapPedInfo> known;
                    OrcCollectPedTextureRemapStats(known);
                    ImGui::Text("Known remap ped models: %d", (int)known.size());
                    if (!known.empty() && ImGui::BeginChild("##texture_known_models", ImVec2(-FLT_MIN, 120.0f), true)) {
                        for (const auto& info : known) {
                            const char* dff = info.dffName.empty() ? "?" : info.dffName.c_str();
                            ImGui::Text("%s [%d]: %d texture(s), %d slot(s)", dff, info.modelId, info.totalRemapTextures, (int)info.slots.size());
                        }
                        ImGui::EndChild();
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
