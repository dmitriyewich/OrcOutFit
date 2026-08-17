#pragma once

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

/// Immutable-after-startup model -> weapon type index. Runtime readers never call
/// externally hookable `CWeaponInfo::GetWeaponInfo` while resolving arbitrary IDs.
class OrcWeaponModelTypeCache final {
public:
    void Rebuild(const std::vector<int>& availableTypes,
        const std::vector<int>& primaryModelIds,
        const std::vector<int>& secondaryModelIds) {
        std::unordered_map<int, int> primaryTypes;
        std::unordered_map<int, int> secondaryTypes;
        primaryTypes.reserve(availableTypes.size());
        secondaryTypes.reserve(availableTypes.size());
        for (int wt : availableTypes) {
            if (wt <= 0)
                continue;
            AddModel(primaryTypes, GetModelId(primaryModelIds, wt), wt);
            AddModel(secondaryTypes, GetModelId(secondaryModelIds, wt), wt);
        }
        m_typeByModel = std::move(secondaryTypes);
        m_typeByModel.reserve(m_typeByModel.size() + primaryTypes.size());
        for (const auto& entry : primaryTypes)
            m_typeByModel[entry.first] = entry.second;
    }

    int FindType(int modelId) const {
        if (modelId <= 0)
            return 0;
        const auto it = m_typeByModel.find(modelId);
        return it != m_typeByModel.end() ? it->second : 0;
    }

    std::size_t Size() const {
        return m_typeByModel.size();
    }

private:
    static int GetModelId(const std::vector<int>& modelIds, int wt) {
        return wt < static_cast<int>(modelIds.size()) ? modelIds[wt] : 0;
    }

    static void AddModel(std::unordered_map<int, int>& typeByModel, int modelId, int wt) {
        if (modelId <= 0)
            return;
        const auto it = typeByModel.find(modelId);
        if (it == typeByModel.end()) {
            typeByModel.emplace(modelId, wt);
        } else if (wt < it->second) {
            it->second = wt;
        }
    }

    std::unordered_map<int, int> m_typeByModel;
};

inline void OrcMarkBodyWeaponTypeSuppressed(std::vector<char>* suppress, int wt) {
    if (suppress && wt > 0 && wt < static_cast<int>(suppress->size()))
        (*suppress)[wt] = 1;
}

/// Extended client IDs may have a model but no vanilla `CWeaponInfo` entry. Only the
/// proven GTA range may cross an externally hooked GetWeaponInfo boundary.
constexpr bool OrcWeaponTypeCanUseVanillaInfoContract(int wt, int vanillaMaxType) {
    return wt > 0 && wt <= vanillaMaxType;
}
