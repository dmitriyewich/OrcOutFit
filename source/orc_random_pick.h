#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct OrcSequentialPickState {
    size_t cursor = 0;
    std::unordered_set<std::string> activeChoiceKeys;
};

inline bool OrcSequentialPoolContains(const std::vector<int>& pool, int value) {
    return std::find(pool.begin(), pool.end(), value) != pool.end();
}

inline void OrcSequentialEraseChoice(OrcSequentialPickState& state,
    std::unordered_map<std::string, int>& assignments,
    const std::string& choiceKey) {
    assignments.erase(choiceKey);
    state.activeChoiceKeys.erase(choiceKey);
}

inline int OrcSequentialPickSticky(OrcSequentialPickState& state,
    std::unordered_map<std::string, int>& assignments,
    const std::string& choiceKey,
    const std::vector<int>& pool,
    int invalidChoice = -2) {
    const size_t n = pool.size();
    if (n == 0 || choiceKey.empty())
        return invalidChoice;

    auto existing = assignments.find(choiceKey);
    if (existing != assignments.end()) {
        if (OrcSequentialPoolContains(pool, existing->second)) {
            state.activeChoiceKeys.insert(choiceKey);
            return existing->second;
        }
        OrcSequentialEraseChoice(state, assignments, choiceKey);
    }

    std::unordered_map<int, int> activeCounts;
    for (auto it = state.activeChoiceKeys.begin(); it != state.activeChoiceKeys.end();) {
        auto assignIt = assignments.find(*it);
        if (assignIt == assignments.end() || !OrcSequentialPoolContains(pool, assignIt->second)) {
            it = state.activeChoiceKeys.erase(it);
            continue;
        }
        ++activeCounts[assignIt->second];
        ++it;
    }

    if (state.cursor >= n)
        state.cursor = 0;

    size_t pickedIndex = state.cursor;
    bool foundFree = false;
    for (size_t attempt = 0; attempt < n; ++attempt) {
        const size_t idx = (state.cursor + attempt) % n;
        if (activeCounts.find(pool[idx]) == activeCounts.end()) {
            pickedIndex = idx;
            foundFree = true;
            break;
        }
    }

    if (!foundFree)
        pickedIndex = state.cursor;

    const int picked = pool[pickedIndex];
    state.cursor = (pickedIndex + 1) % n;
    assignments[choiceKey] = picked;
    state.activeChoiceKeys.insert(choiceKey);
    return picked;
}
