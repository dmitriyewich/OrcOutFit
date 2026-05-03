#include "orc_weapons_ui.h"

#include "orc_app.h"
#include "orc_locale.h"
#include "orc_types.h"
#include "orc_ui_shared.h"
#include "orc_ui_bones.h"

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
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

static const char* WT(OrcTextId id) {
    return OrcText(id);
}

static int g_uiWeaponIdx = WEAPONTYPE_M4;
static std::vector<WeaponCfg> g_uiWeapon1;
static std::vector<WeaponCfg> g_uiWeapon2;
static int g_uiWeaponSkinListIdx = 0;
static bool g_uiWeaponBuffersReady = false;

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
        if (OrcUiLowerAscii(skins[(size_t)i].first) == OrcUiLowerAscii(want)) {
            g_uiWeaponSkinListIdx = i;
            return;
        }
    }
}

void OrcWeaponsUiDrawWeaponsTab() {
    if (ImGui::BeginTabBar("OrcOutFitWeaponSubTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(WT(OrcTextId::TabWeaponRender))) {
            if (!g_uiWeaponBuffersReady) {
                TryInitWeaponSkinListToLocalPed();
                SyncWeaponUiBuffersFromSkinPick();
            }

            bool reload = false, rescanObj = false;
            OrcUiButtonPair(WT(OrcTextId::ReloadIni), WT(OrcTextId::RescanObjects), &reload, &rescanObj);
            if (reload) {
                LoadConfig();
                DiscoverCustomObjectsAndEnsureIni();
                LoadStandardObjectsFromIni();
                DiscoverCustomSkins();
                LoadStandardSkinsFromIni();
                SyncWeaponUiBuffersFromSkinPick();
            }
            if (rescanObj) {
                DiscoverCustomObjectsAndEnsureIni();
                LoadStandardObjectsFromIni();
                if (g_uiCustomIdx >= (int)g_customObjects.size()) g_uiCustomIdx = 0;
            }

            ImGui::Separator();
            std::vector<std::pair<std::string, int>> pedSkins;
            OrcCollectPedSkins(pedSkins);
            if (pedSkins.empty()) {
                ImGui::TextDisabled("%s", WT(OrcTextId::NoPedModelsInCacheReconnect));
            } else {
                if (g_uiWeaponSkinListIdx < 0 || g_uiWeaponSkinListIdx >= (int)pedSkins.size())
                    g_uiWeaponSkinListIdx = 0;
                const auto& cur = pedSkins[(size_t)g_uiWeaponSkinListIdx];
                char comboLbl[192];
                OrcUiPedSkinListLabel(comboLbl, sizeof(comboLbl), cur.first.c_str(), cur.second);
                if (OrcUiBeginControlRow("wskinpick", WT(OrcTextId::PedSkinEditingTarget))) {
                    if (ImGui::BeginCombo("##value", comboLbl)) {
                        CPlayerPed* pl = FindPlayerPed(0);
                        const std::string onMe = pl ? GetPedStdSkinDffName(pl) : std::string{};
                        for (int i = 0; i < (int)pedSkins.size(); i++) {
                            const bool sel = (i == g_uiWeaponSkinListIdx);
                            const bool onPlayer = !onMe.empty() && OrcUiLowerAscii(pedSkins[(size_t)i].first) == OrcUiLowerAscii(onMe);
                            char rowLbl[192];
                            OrcUiPedSkinListLabel(rowLbl, sizeof(rowLbl), pedSkins[(size_t)i].first.c_str(), pedSkins[(size_t)i].second);
                            if (ImGui::Selectable(rowLbl, sel)) {
                                g_uiWeaponSkinListIdx = i;
                                SyncWeaponUiBuffersFromSkinPick();
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
                         (pc && pc->name) ? pc->name : WT(OrcTextId::Weapon),
                         g_uiWeaponSecondary ? " 2" : "",
                         g_uiWeaponIdx, previewModelId);
            if (OrcUiBeginControlRow("weapon", WT(OrcTextId::Weapon))) {
                if (ImGui::BeginCombo("##value", preview)) {
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
                        const char* baseName = (wt >= 0 && wt < (int)g_cfg.size() && g_cfg[wt].name) ? g_cfg[wt].name : WT(OrcTextId::Weapon);
                        char lbl[128];
                        const int modelId = (wt > 0 && wt < (int)g_weaponModelId.size()) ? g_weaponModelId[wt] : 0;
                        _snprintf_s(lbl, _TRUNCATE, "%s [%d][%d]", baseName, wt, modelId);
                        const bool hasNow = (wt > 0 && wt < (int)localHas.size() && localHas[wt] != 0);
                        if (ImGui::Selectable(lbl, (wt == g_uiWeaponIdx) && !g_uiWeaponSecondary)) { g_uiWeaponIdx = wt; g_uiWeaponSecondary = false; }
                        if (hasNow) {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 mn = ImGui::GetItemRectMin();
                            const ImVec2 mx = ImGui::GetItemRectMax();
                            dl->AddRectFilled(mn, ImVec2(mn.x + OrcUiScaled(3.0f), mx.y), IM_COL32(60, 200, 120, 160), 0.0f);
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
                                dl->AddRectFilled(mn, ImVec2(mn.x + OrcUiScaled(3.0f), mx.y), IM_COL32(60, 200, 120, 160), 0.0f);
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                OrcUiEndControlRow();
            }

            int idx = g_uiWeaponIdx;
            if (OrcUiInputInt("weaponid", WT(OrcTextId::WeaponSlotId), &idx, 1, 1)) {
                if (idx >= 1 && idx < (int)g_cfg.size()) { g_uiWeaponIdx = idx; g_uiWeaponSecondary = false; }
            }

            if (g_considerWeaponSkills && IsDualCapable(g_uiWeaponIdx)) {
                OrcUiCheckbox("edit_second_weapon", WT(OrcTextId::EditSecondWeapon), &g_uiWeaponSecondary);
            } else {
                g_uiWeaponSecondary = false;
            }

            ImGui::Separator();
            WeaponCfg* editingArr = g_uiWeaponSecondary ? activeArr2 : activeArr;
            const int editingCount = g_uiWeaponSecondary ? activeCount2 : activeCount;
            const bool canEdit = (editingArr != nullptr && g_uiWeaponIdx >= 0 && g_uiWeaponIdx < editingCount);
            if (!canEdit) {
                ImGui::TextDisabled("%s", WT(OrcTextId::WeaponEditorUnavailable));
            } else {
                auto& c = editingArr[g_uiWeaponIdx];
                OrcUiCheckbox("show_on_body", WT(OrcTextId::ShowOnBody), &c.enabled);
                if (ImGui::SmallButton(WT(OrcTextId::Copy))) {
                    g_weaponBuf.valid = true;
                    g_weaponBuf.secondary = g_uiWeaponSecondary;
                    g_weaponBuf.wt = g_uiWeaponIdx;
                    g_weaponBuf.cfg = c;
                    g_weaponBuf.cfg.name = nullptr;
                }
                ImGui::SameLine();
                const bool canPaste = g_weaponBuf.valid && ValidateWeaponCfg(g_weaponBuf.cfg);
                if (!canPaste) ImGui::BeginDisabled();
                if (ImGui::SmallButton(WT(OrcTextId::Paste))) {
                    const char* keepName = c.name;
                    c = g_weaponBuf.cfg;
                    c.name = keepName;
                }
                if (!canPaste) ImGui::EndDisabled();

                int bi = OrcUiBoneComboIndex(c.boneId);
                const OrcUiBoneRow* rows = OrcUiBoneRows();
                const char* bonePreview = WT(rows[bi].label);
                if (OrcUiBeginControlRow("wbone", WT(OrcTextId::Bone))) {
                    if (ImGui::BeginCombo("##value", bonePreview)) {
                        for (int i = 0; i < OrcUiBoneRowCount(); i++) {
                            if (ImGui::Selectable(WT(rows[i].label), i == bi))
                                c.boneId = rows[i].id;
                        }
                        ImGui::EndCombo();
                    }
                    OrcUiEndControlRow();
                }

                OrcUiDragFloat("wx", WT(OrcTextId::OffsetX), &c.x, 0.005f, -2.0f, 2.0f, "%.3f");
                OrcUiDragFloat("wy", WT(OrcTextId::OffsetY), &c.y, 0.005f, -2.0f, 2.0f, "%.3f");
                OrcUiDragFloat("wz", WT(OrcTextId::OffsetZ), &c.z, 0.005f, -2.0f, 2.0f, "%.3f");

                float rxd = c.rx / D2R, ryd = c.ry / D2R, rzd = c.rz / D2R;
                if (OrcUiDragFloat("wrx", WT(OrcTextId::RotationX), &rxd, 0.5f, -180.0f, 180.0f, "%.1f")) c.rx = rxd * D2R;
                if (OrcUiDragFloat("wry", WT(OrcTextId::RotationY), &ryd, 0.5f, -180.0f, 180.0f, "%.1f")) c.ry = ryd * D2R;
                if (OrcUiDragFloat("wrz", WT(OrcTextId::RotationZ), &rzd, 0.5f, -180.0f, 180.0f, "%.1f")) c.rz = rzd * D2R;

                OrcUiDragFloat("wsc", WT(OrcTextId::Scale), &c.scale, 0.01f, 0.05f, 10.0f, "%.3f");

                ImGui::Separator();
                if (OrcUiButtonFullWidth(WT(OrcTextId::SaveToGlobal))) {
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
                const bool blockSkinSave = pl && pl->m_nPedType == PED_TYPE_PLAYER1 &&
                    (int)pl->m_nModelIndex == MODEL_PLAYER && !samp_bridge::IsSampPresent();
                if (blockSkinSave) {
                    ImGui::TextDisabled("%s", WT(OrcTextId::PerSkinPresetDisabledForCj));
                } else if (!pedSkins.empty()) {
                    if (OrcUiButtonFullWidth(WT(OrcTextId::SaveToSkinWeapons))) {
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
                    ImGui::TextDisabled("%s", WT(OrcTextId::UnsupportedSampSpMode));
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(WT(OrcTextId::TabWeaponReplacement))) {
            OrcUiCheckbox("weapon_replacement_enabled", WT(OrcTextId::EnableWeaponReplacement), &g_weaponReplacementEnabled);
            OrcUiCheckbox("weapon_replacement_body", WT(OrcTextId::ReplaceWeaponsOnBody), &g_weaponReplacementOnBody);
            OrcUiCheckbox("weapon_replacement_hands", WT(OrcTextId::ReplaceWeaponsInHands), &g_weaponReplacementInHands);
            OrcUiCheckbox("weapon_replacement_hide_base_held", WT(OrcTextId::HideBaseHeldWeapon), &g_weaponReplacementHideBaseHeld);
            ImGui::TextWrapped("%s", WT(OrcTextId::HideBaseHeldWeaponHint));
            ImGui::TextWrapped("%s", WT(OrcTextId::WeaponReplacementHint));
            WeaponReplacementStats stats = OrcGetWeaponReplacementStats();
            ImGui::Text("%s", OrcFormat(
                OrcTextId::WeaponReplacementStatsFormat,
                stats.uniqueSkinWeapons,
                stats.randomSkinWeapons,
                stats.nickWeapons).c_str());
            ImGui::TextWrapped("%s", g_gameWeaponGunsDir);
            ImGui::TextWrapped("%s", g_gameWeaponGunsNickDir);

            bool save = false, rescan = false;
            OrcUiButtonPair(WT(OrcTextId::SaveMainFeatures), WT(OrcTextId::RescanWeaponReplacement), &save, &rescan);
            if (save)
                SaveMainIni();
            if (rescan)
                DiscoverWeaponReplacements();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(WT(OrcTextId::TabWeaponTextures))) {
            OrcUiCheckbox("weapon_textures_enabled", WT(OrcTextId::EnableWeaponTextures), &g_weaponTexturesEnabled);
            const bool textureNickUiOff = samp_bridge::IsSampPresent() && !samp_bridge::IsSampBuildKnown();
            if (textureNickUiOff)
                ImGui::TextWrapped("%s", WT(OrcTextId::UnsupportedSampTextureNickBinding));
            ImGui::BeginDisabled(textureNickUiOff);
            OrcUiCheckbox("weapon_texture_nick", WT(OrcTextId::WeaponTextureNickBinding), &g_weaponTextureNickMode);
            ImGui::EndDisabled();
            OrcUiCheckbox("weapon_texture_random", WT(OrcTextId::WeaponTextureRandomMode), &g_weaponTextureRandomMode);
            ImGui::TextWrapped("%s", WT(OrcTextId::WeaponTextureHint));
            WeaponTextureStats stats = OrcGetWeaponTextureStats();
            ImGui::Text("%s", OrcFormat(
                OrcTextId::WeaponTextureStatsFormat,
                stats.uniqueSkinTextures,
                stats.randomSkinTextures,
                stats.nickTextures).c_str());
            ImGui::TextWrapped("%s", g_gameWeaponTexturesDir);

            bool save = false, rescan = false;
            OrcUiButtonPair(WT(OrcTextId::SaveMainFeatures), WT(OrcTextId::RescanWeaponTextures), &save, &rescan);
            if (save)
                SaveMainIni();
            if (rescan)
                DiscoverWeaponTextures();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}
