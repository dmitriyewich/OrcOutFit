#include "orc_ui.h"

#include "orc_app.h"
#include "orc_locale.h"

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
    OrcTextId label;
};
static const BoneOption kBones[] = {
    { 0,              OrcTextId::BoneNone },
    { 1,              OrcTextId::BoneRoot },
    { BONE_PELVIS,    OrcTextId::BonePelvis },
    { BONE_SPINE1,    OrcTextId::BoneSpine1 },
    { 4,              OrcTextId::BoneSpine },
    { 5,              OrcTextId::BoneNeck },
    { 6,              OrcTextId::BoneHead },
    { BONE_R_CLAVIC,  OrcTextId::BoneRightClavicle },
    { BONE_R_UPARM,   OrcTextId::BoneRightUpperArm },
    { 23,             OrcTextId::BoneRightForearm },
    { 24,             OrcTextId::BoneRightHand },
    { BONE_L_CLAVIC,  OrcTextId::BoneLeftClavicle },
    { BONE_L_UPARM,   OrcTextId::BoneLeftUpperArm },
    { 33,             OrcTextId::BoneLeftForearm },
    { 34,             OrcTextId::BoneLeftHand },
    { BONE_L_THIGH,   OrcTextId::BoneLeftThigh },
    { BONE_L_CALF,    OrcTextId::BoneLeftCalf },
    { 43,             OrcTextId::BoneLeftFoot },
    { BONE_R_THIGH,   OrcTextId::BoneRightThigh },
    { BONE_R_CALF,    OrcTextId::BoneRightCalf },
    { 53,             OrcTextId::BoneRightFoot },
};

static const char* T(OrcTextId id) {
    return OrcText(id);
}

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
    ImGui::TextUnformatted(T(OrcTextId::WeaponCondition));
    ImGui::TextWrapped("%s", T(OrcTextId::WeaponConditionHint));

    const bool any = !obj.weaponRequireAll;
    if (ImGui::RadioButton(T(OrcTextId::AnySelectedWeapon), any)) obj.weaponRequireAll = false;
    if (ImGui::RadioButton(T(OrcTextId::AllSelectedWeapons), !any)) obj.weaponRequireAll = true;

    ImGui::Checkbox(T(OrcTextId::HideSelectedWeapons), &obj.hideSelectedWeapons);

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

    if (ImGui::Button(T(OrcTextId::ClearWeaponSelection), ImVec2(-FLT_MIN, 0))) {
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
    if (!ImGui::Begin(T(OrcTextId::WindowTitle), &open, wflags)) {
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
        if (ImGui::BeginTabItem(T(OrcTextId::TabMain))) {
            ImGui::Checkbox(T(OrcTextId::PluginEnabled), &g_enabled);
            ImGui::TextUnformatted(T(OrcTextId::Language));
            const OrcUiLanguage languages[] = { OrcUiLanguage::Russian, OrcUiLanguage::English };
            if (ImGui::BeginCombo("##language", OrcLanguageDisplayName(g_orcUiLanguage))) {
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
            ImGui::TextUnformatted(T(OrcTextId::ToggleKey));
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

            ImGui::TextUnformatted(T(OrcTextId::ChatCommand));
            static char cmdBuf[96] = {};
            if (cmdBuf[0] == 0) _snprintf_s(cmdBuf, _TRUNCATE, "%s", g_toggleCommand.c_str());
            if (ImGui::InputText("##cmd", cmdBuf, sizeof(cmdBuf))) {
                g_toggleCommand = cmdBuf;
                if (!g_toggleCommand.empty() && g_toggleCommand[0] != '/') g_toggleCommand.insert(g_toggleCommand.begin(), '/');
            }
            if (ImGui::Checkbox(T(OrcTextId::SampAllowToggleKey), &g_sampAllowActivationKey))
                RefreshActivationRouting();

            ImGui::Separator();
            ImGui::TextUnformatted(T(OrcTextId::Features));
            ImGui::Checkbox(T(OrcTextId::RenderWeaponsForAllPeds), &g_renderAllPedsWeapons);
            ImGui::Checkbox(T(OrcTextId::RenderObjectsForAllPeds), &g_renderAllPedsObjects);
            if (g_renderAllPedsWeapons || g_renderAllPedsObjects) {
                ImGui::SliderFloat(T(OrcTextId::AllPedsRadius), &g_renderAllPedsRadius, 5.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            }
            ImGui::Checkbox(T(OrcTextId::ConsiderWeaponSkills), &g_considerWeaponSkills);
            ImGui::Checkbox(T(OrcTextId::RenderCustomObjects), &g_renderCustomObjects);
            ImGui::Checkbox(T(OrcTextId::SkinMode), &g_skinModeEnabled);
            ImGui::Checkbox(T(OrcTextId::SkinHideBasePed), &g_skinHideBasePed);
            const bool sampNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
            if (sampNickUiOff)
                ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampNickBinding));
            ImGui::BeginDisabled(sampNickUiOff);
            ImGui::Checkbox(T(OrcTextId::SkinNickBinding), &g_skinNickMode);
            ImGui::EndDisabled();
            ImGui::Checkbox(T(OrcTextId::SkinAlwaysSelectedForMe), &g_skinLocalPreferSelected);
            ImGui::TextWrapped("%s", T(OrcTextId::SkinAlwaysSelectedHint));
            int logCombo = static_cast<int>(g_orcLogLevel);
            const OrcTextId logLabels[] = { OrcTextId::LogOff, OrcTextId::LogErrorsOnly, OrcTextId::LogInfoFull };
            if (logCombo < 0) logCombo = 0;
            if (logCombo > 2) logCombo = 2;
            if (ImGui::BeginCombo(T(OrcTextId::DebugLog), T(logLabels[logCombo]))) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(T(logLabels[i]), logCombo == i)) {
                        logCombo = i;
                        g_orcLogLevel = static_cast<OrcLogLevel>(logCombo);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("%s", OrcLogGetPath());
            ImGui::PopItemWidth();

            ImGui::Separator();
            if (ImGui::Button(T(OrcTextId::SaveMainFeatures), ImVec2(-FLT_MIN, 0))) {
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
            if (!g_uiWeaponBuffersReady) {
                TryInitWeaponSkinListToLocalPed();
                SyncWeaponUiBuffersFromSkinPick();
            }

            bool reload = false, rescanObj = false;
            BtnHalfRow(T(OrcTextId::ReloadIni), T(OrcTextId::RescanObjects), &reload, &rescanObj);
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
            ImGui::TextWrapped("%s", T(OrcTextId::PedSkinEditingTarget));
            if (pedSkins.empty()) {
                ImGui::TextDisabled("%s", T(OrcTextId::NoPedModelsInCacheReconnect));
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
                if (ImGui::Button(T(OrcTextId::WearThisSkin), ImVec2(-FLT_MIN, 0))) {
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
                         (pc && pc->name) ? pc->name : T(OrcTextId::Weapon),
                         g_uiWeaponSecondary ? " 2" : "",
                         g_uiWeaponIdx, previewModelId);
            ImGui::TextUnformatted(T(OrcTextId::Weapon));
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
                    const char* baseName = (wt >= 0 && wt < (int)g_cfg.size() && g_cfg[wt].name) ? g_cfg[wt].name : T(OrcTextId::Weapon);
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

            ImGui::TextUnformatted(T(OrcTextId::WeaponSlotId));
            int idx = g_uiWeaponIdx;
            if (ImGui::InputInt("##weaponid", &idx, 1, 1)) {
                if (idx >= 1 && idx < (int)g_cfg.size()) { g_uiWeaponIdx = idx; g_uiWeaponSecondary = false; }
            }

            if (g_considerWeaponSkills && IsDualCapable(g_uiWeaponIdx)) {
                ImGui::Checkbox(T(OrcTextId::EditSecondWeapon), &g_uiWeaponSecondary);
            } else {
                g_uiWeaponSecondary = false;
            }

            ImGui::Separator();
            WeaponCfg* editingArr = g_uiWeaponSecondary ? activeArr2 : activeArr;
            const int editingCount = g_uiWeaponSecondary ? activeCount2 : activeCount;
            const bool canEdit = (editingArr != nullptr && g_uiWeaponIdx >= 0 && g_uiWeaponIdx < editingCount);
            if (!canEdit) {
                ImGui::TextDisabled("%s", T(OrcTextId::WeaponEditorUnavailable));
                ImGui::PopItemWidth();
            } else {
                auto& c = editingArr[g_uiWeaponIdx];
                ImGui::Checkbox(T(OrcTextId::ShowOnBody), &c.enabled);
                ImGui::SameLine();
                if (ImGui::SmallButton(T(OrcTextId::Copy))) {
                    g_weaponBuf.valid = true;
                    g_weaponBuf.secondary = g_uiWeaponSecondary;
                    g_weaponBuf.wt = g_uiWeaponIdx;
                    g_weaponBuf.cfg = c;
                    g_weaponBuf.cfg.name = nullptr;
                }
                ImGui::SameLine();
                const bool canPaste = g_weaponBuf.valid && ValidateWeaponCfg(g_weaponBuf.cfg);
                if (!canPaste) ImGui::BeginDisabled();
                if (ImGui::SmallButton(T(OrcTextId::Paste))) {
                    const char* keepName = c.name;
                    c = g_weaponBuf.cfg;
                    c.name = keepName;
                }
                if (!canPaste) ImGui::EndDisabled();

                int bi = BoneComboIndex(c.boneId);
                const char* bonePreview = T(kBones[bi].label);
                ImGui::TextUnformatted(T(OrcTextId::Bone));
                if (ImGui::BeginCombo("##wbone", bonePreview)) {
                    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                        if (ImGui::Selectable(T(kBones[i].label), i == bi))
                            c.boneId = kBones[i].id;
                    }
                    ImGui::EndCombo();
                }

                ImGui::TextUnformatted(T(OrcTextId::OffsetX));
                ImGui::DragFloat("##wx", &c.x, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted(T(OrcTextId::OffsetY));
                ImGui::DragFloat("##wy", &c.y, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted(T(OrcTextId::OffsetZ));
                ImGui::DragFloat("##wz", &c.z, 0.005f, -2.0f, 2.0f, "%.3f");

                float rxd = c.rx / D2R, ryd = c.ry / D2R, rzd = c.rz / D2R;
                ImGui::TextUnformatted(T(OrcTextId::RotationX));
                if (ImGui::DragFloat("##wrx", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) c.rx = rxd * D2R;
                ImGui::TextUnformatted(T(OrcTextId::RotationY));
                if (ImGui::DragFloat("##wry", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) c.ry = ryd * D2R;
                ImGui::TextUnformatted(T(OrcTextId::RotationZ));
                if (ImGui::DragFloat("##wrz", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) c.rz = rzd * D2R;

                ImGui::TextUnformatted(T(OrcTextId::Scale));
                ImGui::DragFloat("##wsc", &c.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                ImGui::PopItemWidth();

                ImGui::Separator();
                if (ImGui::Button(T(OrcTextId::SaveToGlobal), ImVec2(-FLT_MIN, 0))) {
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
                    ImGui::TextDisabled("%s", T(OrcTextId::PerSkinPresetDisabledForCj));
                } else if (!pedSkins.empty()) {
                    if (ImGui::Button(T(OrcTextId::SaveToSkinWeapons), ImVec2(-FLT_MIN, 0))) {
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
                    ImGui::TextDisabled("%s", OrcFormat(OrcTextId::ToggleCommandAndKeyFormat, g_toggleCommand.c_str(), VkToString(g_activationVk)).c_str());
                else
                    ImGui::TextWrapped("%s", OrcFormat(OrcTextId::ToggleChatFormat, g_toggleCommand.c_str()).c_str());
            } else {
                ImGui::TextDisabled("%s", OrcFormat(OrcTextId::ToggleKeyFormat, VkToString(g_activationVk)).c_str());
                if (samp_bridge::IsSampPresent())
                    ImGui::TextDisabled("%s", T(OrcTextId::UnsupportedSampSpMode));
            }
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        // Objects
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem(T(OrcTextId::TabObjects))) {
            ImGui::TextWrapped("%s", g_gameObjDir);
            ImGui::Separator();

            if (g_customObjects.empty()) {
                g_livePreviewObjectActive = false;
                g_livePreviewObjectIniPath.clear();
                g_livePreviewObjectSkinDff.clear();
                ImGui::TextDisabled("%s", T(OrcTextId::NoDffObjectsFolder));
                if (ImGui::Button(T(OrcTextId::Rescan), ImVec2(-FLT_MIN, 0)))
                    DiscoverCustomObjectsAndEnsureIni();
            } else {
                if (g_uiCustomIdx < 0 || g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
                auto& obj = g_customObjects[g_uiCustomIdx];

                ImGui::PushItemWidth(-FLT_MIN);
                char oprev[160];
                _snprintf_s(oprev, _TRUNCATE, "%s [%d/%d]", obj.name.c_str(), g_uiCustomIdx + 1, (int)g_customObjects.size());
                ImGui::TextUnformatted(T(OrcTextId::Object));
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
                ImGui::TextUnformatted(T(OrcTextId::PedSkinDffName));
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

                ImGui::Checkbox(T(OrcTextId::Show), &g_uiObjParams.enabled);

                int bi = BoneComboIndex(g_uiObjParams.boneId);
                const char* bonePreview = T(kBones[bi].label);
                ImGui::TextUnformatted(T(OrcTextId::Bone));
                if (ImGui::BeginCombo("##objbone", bonePreview)) {
                    for (int i = 0; i < IM_ARRAYSIZE(kBones); i++) {
                        if (ImGui::Selectable(T(kBones[i].label), i == bi))
                            g_uiObjParams.boneId = kBones[i].id;
                    }
                    ImGui::EndCombo();
                }

                ImGui::TextUnformatted(T(OrcTextId::OffsetX));
                ImGui::DragFloat("##ox", &g_uiObjParams.x, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted(T(OrcTextId::OffsetY));
                ImGui::DragFloat("##oy", &g_uiObjParams.y, 0.005f, -2.0f, 2.0f, "%.3f");
                ImGui::TextUnformatted(T(OrcTextId::OffsetZ));
                ImGui::DragFloat("##oz", &g_uiObjParams.z, 0.005f, -2.0f, 2.0f, "%.3f");

                float rxd = g_uiObjParams.rx / D2R, ryd = g_uiObjParams.ry / D2R, rzd = g_uiObjParams.rz / D2R;
                ImGui::TextUnformatted(T(OrcTextId::RotationX));
                if (ImGui::DragFloat("##orx", &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.rx = rxd * D2R;
                ImGui::TextUnformatted(T(OrcTextId::RotationY));
                if (ImGui::DragFloat("##ory", &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.ry = ryd * D2R;
                ImGui::TextUnformatted(T(OrcTextId::RotationZ));
                if (ImGui::DragFloat("##orz", &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) g_uiObjParams.rz = rzd * D2R;

                ImGui::TextUnformatted(T(OrcTextId::Scale));
                ImGui::DragFloat("##osc", &g_uiObjParams.scale, 0.01f, 0.05f, 10.0f, "%.3f");
                ImGui::TextUnformatted(T(OrcTextId::ScaleXyz));
                ImGui::DragFloat3("##oscxyz", &g_uiObjParams.scaleX, 0.01f, 0.05f, 10.0f, "%.3f");
                ImGui::PopItemWidth();

                WeaponFilterEditorParams(g_uiObjParams);

                ImGui::Separator();
                if (!pedSkins.empty()) {
                    if (ImGui::Button(T(OrcTextId::SaveSkinSectionToObjectIni), ImVec2(-FLT_MIN, 0))) {
                        SaveObjectSkinParamsToIni(obj.iniPath.c_str(), pedSkins[(size_t)g_uiObjSkinListIdx].first.c_str(), g_uiObjParams);
                        g_livePreviewObjectActive = false;
                        g_livePreviewObjectIniPath.clear();
                        g_livePreviewObjectSkinDff.clear();
                    }
                }
                if (ImGui::Button(T(OrcTextId::RescanObjectsFolder), ImVec2(-FLT_MIN, 0))) {
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
                        ImGui::PushItemWidth(-FLT_MIN);
                        char previewSkin[160];
                        _snprintf_s(previewSkin, _TRUNCATE, "%s [%d/%d]", g_customSkins[g_uiSkinIdx].name.c_str(), g_uiSkinIdx + 1, (int)g_customSkins.size());
                        ImGui::TextUnformatted(T(OrcTextId::Skin));
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
                            ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampNickBinding));
                        ImGui::BeginDisabled(sampNickUiOff);
                        if (ImGui::Checkbox(T(OrcTextId::BindSkinToNicks), &skin.bindToNick))
                            InvalidateCustomSkinLookupCache();
                        if (g_uiSkinEditIdx != g_uiSkinIdx) {
                            g_uiSkinEditIdx = g_uiSkinIdx;
                            _snprintf_s(g_uiSkinNickBuf, _TRUNCATE, "%s", skin.nickListCsv.c_str());
                        }
                        ImGui::TextWrapped("%s", T(OrcTextId::NicksCommaSeparated));
                        if (ImGui::InputTextWithHint("##skinnicks", T(OrcTextId::NickPlaceholder), g_uiSkinNickBuf, IM_ARRAYSIZE(g_uiSkinNickBuf))) {
                            skin.nickListCsv = g_uiSkinNickBuf;
                            skin.nicknames = ParseNickCsv(skin.nickListCsv);
                            InvalidateCustomSkinLookupCache();
                        }
                        ImGui::EndDisabled();
                        ImGui::PopItemWidth();
                        if (ImGui::Button(T(OrcTextId::SaveSkinIni), ImVec2(-FLT_MIN, 0))) {
                            skin.nickListCsv = g_uiSkinNickBuf;
                            skin.nicknames = ParseNickCsv(skin.nickListCsv);
                            InvalidateCustomSkinLookupCache();
                            SaveSkinCfgToIni(skin);
                        }
                    }

                    ImGui::Separator();
                    bool sm = false, rs = false;
                    BtnHalfRow(T(OrcTextId::SaveSkinModeSelection), T(OrcTextId::RescanSkins), &sm, &rs);
                    if (sm) SaveSkinModeIni();
                    if (rs) {
                        DiscoverCustomSkins();
                        if (g_uiSkinIdx < (int)g_customSkins.size()) g_skinSelectedName = g_customSkins[g_uiSkinIdx].name;
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(T(OrcTextId::TabTexture))) {
                    ImGui::Checkbox(T(OrcTextId::EnableTextureRemaps), &g_skinTextureRemapEnabled);
                    const bool textureNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
                    if (textureNickUiOff)
                        ImGui::TextWrapped("%s", T(OrcTextId::UnsupportedSampTextureNickBinding));
                    ImGui::BeginDisabled(textureNickUiOff);
                    ImGui::Checkbox(T(OrcTextId::TextureNickBinding), &g_skinTextureRemapNickMode);
                    ImGui::EndDisabled();
                    const OrcTextId randomModeNames[] = { OrcTextId::RandomModePerTexture, OrcTextId::RandomModeLinkedVariant };
                    if (g_skinTextureRemapRandomMode < TEXTURE_REMAP_RANDOM_PER_TEXTURE ||
                        g_skinTextureRemapRandomMode > TEXTURE_REMAP_RANDOM_LINKED_VARIANT) {
                        g_skinTextureRemapRandomMode = TEXTURE_REMAP_RANDOM_LINKED_VARIANT;
                    }
                    ImGui::PushItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo(T(OrcTextId::RandomMode), T(randomModeNames[g_skinTextureRemapRandomMode]))) {
                        for (int i = TEXTURE_REMAP_RANDOM_PER_TEXTURE; i <= TEXTURE_REMAP_RANDOM_LINKED_VARIANT; ++i) {
                            if (ImGui::Selectable(T(randomModeNames[i]), g_skinTextureRemapRandomMode == i))
                                g_skinTextureRemapRandomMode = i;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();
                    ImGui::TextWrapped("%s", T(OrcTextId::TextureRemapHint));
                    if (ImGui::Button(T(OrcTextId::SaveTextureSettings), ImVec2(-FLT_MIN, 0)))
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
                        BtnHalfRow(T(OrcTextId::RandomizeLocal), T(OrcTextId::OriginalTextures), &randomize, &original);
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
                            ImGui::PushItemWidth(-FLT_MIN);
                            for (int i = 0; i < (int)localInfo.slots.size(); ++i) {
                                const TextureRemapSlotInfo& slot = localInfo.slots[(size_t)i];
                                ImGui::TextUnformatted(slot.originalName.c_str());

                                int selected = slot.selected;
                                const char* preview = T(OrcTextId::Original);
                                if (selected >= 0 && selected < (int)slot.remapNames.size())
                                    preview = slot.remapNames[(size_t)selected].c_str();

                                char comboId[32];
                                _snprintf_s(comboId, _TRUNCATE, "##texremap%d", i);
                                if (ImGui::BeginCombo(comboId, preview)) {
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
                            }
                            ImGui::PopItemWidth();
                        }

                        ImGui::Separator();
                        ImGui::TextUnformatted(T(OrcTextId::NickBinding));
                        ImGui::PushItemWidth(-FLT_MIN);
                        ImGui::InputTextWithHint("##texturenicks", T(OrcTextId::NickPlaceholder), g_uiTextureNickBuf, IM_ARRAYSIZE(g_uiTextureNickBuf));
                        ImGui::PopItemWidth();
                        bool saveBind = false, reloadBind = false;
                        BtnHalfRow(T(OrcTextId::SaveCurrentTextureBinding), T(OrcTextId::ReloadTextureBindings), &saveBind, &reloadBind);
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
                    if (!known.empty() && ImGui::BeginChild("##texture_known_models", ImVec2(-FLT_MIN, 120.0f), true)) {
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

        ImGui::EndTabBar();
    }

    ImGui::End();
}
