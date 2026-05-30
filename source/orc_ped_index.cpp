// OrcOutFit — общий индекс ped-моделей (валидность model id + список скинов педов).
// Выделено из orc_skins.cpp, чтобы picker педов во вкладке «Оружие» работал и в Lite-сборке,
// где модуль скинов (orc_skins.cpp) не компилируется.

#include "plugin.h"
#include "common.h"
#include "CStreaming.h"
#include "CModelInfo.h"
#include "CBaseModelInfo.h"
#include "eModelInfoType.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "orc_app.h"
#include "orc_log.h"

static CBaseModelInfo* GetExistingStandardModelInfo(int modelId) {
    if (modelId < 0 || modelId >= CModelInfo::ms_modelInfoCount) return nullptr;
    if (!CModelInfo::ms_modelInfoPtrs) return nullptr;
    CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
    if (!mi) return nullptr;
    if (mi->m_pRwObject) return mi;
    if (!CStreaming::ms_aInfoForModel) return nullptr;
    __try {
        const CStreamingInfo& info = CStreaming::ms_aInfoForModel[modelId];
        if (info.m_nCdSize > 0 || info.m_nLoadState == LOADSTATE_LOADED)
            return mi;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OrcLogError("GetExistingStandardModelInfo: streaming info SEH ex=0x%08X model=%d", GetExceptionCode(), modelId);
    }
    return nullptr;
}

bool OrcIsValidStandardSkinModel(int modelId) {
    CBaseModelInfo* mi = GetExistingStandardModelInfo(modelId);
    return mi && mi->GetModelType() == MODEL_INFO_PED;
}

void OrcCollectPedSkins(std::vector<std::pair<std::string, int>>& out) {
    out.clear();
    for (int id = 0; id < (int)g_pedModelNameById.size(); id++) {
        if (g_pedModelNameById[id].empty()) continue;
        if (!OrcIsValidStandardSkinModel(id)) continue;
        out.push_back({ g_pedModelNameById[id], id });
    }
    std::sort(out.begin(), out.end(), [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
        return a.second < b.second;
    });
}
