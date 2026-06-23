#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct OrcSequentialPickState {
    size_t cursor = 0;
    std::unordered_set<std::string> activeChoiceKeys;
};

struct OrcRandomNoRepeatPickState {
    std::vector<int> poolSnapshot;
    std::vector<int> bag;
    std::unordered_set<std::string> activeChoiceKeys;
};

inline bool OrcSequentialPoolContains(const std::vector<int>& pool, int value) {
    return std::find(pool.begin(), pool.end(), value) != pool.end();
}

inline bool OrcRandomNoRepeatBagContains(const std::vector<int>& bag, int value) {
    return std::find(bag.begin(), bag.end(), value) != bag.end();
}

inline void OrcRandomNoRepeatShuffle(std::vector<int>& bag) {
    for (int i = (int)bag.size() - 1; i > 0; --i) {
        const int j = std::rand() % (i + 1);
        std::swap(bag[(size_t)i], bag[(size_t)j]);
    }
}

inline void OrcRandomNoRepeatEnsurePool(OrcRandomNoRepeatPickState& state, const std::vector<int>& pool) {
    if (state.poolSnapshot == pool)
        return;
    state.poolSnapshot = pool;
    state.bag.clear();
}

inline void OrcRandomNoRepeatInsertIntoBag(OrcRandomNoRepeatPickState& state, int value) {
    if (!OrcSequentialPoolContains(state.poolSnapshot, value) ||
        OrcRandomNoRepeatBagContains(state.bag, value))
        return;
    const size_t pos = state.bag.empty() ? 0u : (size_t)(std::rand() % (int)(state.bag.size() + 1));
    state.bag.insert(state.bag.begin() + (ptrdiff_t)pos, value);
}

inline void OrcSequentialEraseChoice(OrcSequentialPickState& state,
    std::unordered_map<std::string, int>& assignments,
    const std::string& choiceKey) {
    assignments.erase(choiceKey);
    state.activeChoiceKeys.erase(choiceKey);
}

inline void OrcRandomNoRepeatReleaseChoice(OrcRandomNoRepeatPickState& state,
    const std::unordered_map<std::string, int>& assignments,
    const std::string& choiceKey,
    bool recycle = true) {
    auto activeIt = state.activeChoiceKeys.find(choiceKey);
    if (activeIt == state.activeChoiceKeys.end())
        return;
    if (recycle) {
        auto assignIt = assignments.find(choiceKey);
        if (assignIt != assignments.end())
            OrcRandomNoRepeatInsertIntoBag(state, assignIt->second);
    }
    state.activeChoiceKeys.erase(activeIt);
}

inline void OrcRandomNoRepeatEraseChoice(OrcRandomNoRepeatPickState& state,
    std::unordered_map<std::string, int>& assignments,
    const std::string& choiceKey,
    bool recycle = true) {
    OrcRandomNoRepeatReleaseChoice(state, assignments, choiceKey, recycle);
    assignments.erase(choiceKey);
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

inline int OrcRandomNoRepeatPopFromBag(OrcRandomNoRepeatPickState& state,
    const std::vector<int>& pool,
    const std::unordered_map<int, int>& activeCounts,
    bool avoidActive,
    int invalidChoice) {
    for (;;) {
        while (!state.bag.empty()) {
            const int picked = state.bag.back();
            state.bag.pop_back();
            if (!OrcSequentialPoolContains(pool, picked))
                continue;
            if (avoidActive && activeCounts.find(picked) != activeCounts.end())
                continue;
            return picked;
        }

        for (int value : pool) {
            if (avoidActive && activeCounts.find(value) != activeCounts.end())
                continue;
            state.bag.push_back(value);
        }
        if (state.bag.empty())
            return invalidChoice;
        OrcRandomNoRepeatShuffle(state.bag);
    }
}

inline int OrcRandomNoRepeatPickSticky(OrcRandomNoRepeatPickState& state,
    std::unordered_map<std::string, int>& assignments,
    const std::string& choiceKey,
    const std::vector<int>& pool,
    int invalidChoice = -2) {
    if (pool.empty() || choiceKey.empty())
        return invalidChoice;

    OrcRandomNoRepeatEnsurePool(state, pool);

    auto existing = assignments.find(choiceKey);
    if (existing != assignments.end()) {
        if (OrcSequentialPoolContains(pool, existing->second)) {
            state.activeChoiceKeys.insert(choiceKey);
            return existing->second;
        }
        OrcRandomNoRepeatEraseChoice(state, assignments, choiceKey, false);
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

    bool hasFreeChoice = false;
    for (int value : pool) {
        if (activeCounts.find(value) == activeCounts.end()) {
            hasFreeChoice = true;
            break;
        }
    }

    const int picked = OrcRandomNoRepeatPopFromBag(
        state,
        pool,
        activeCounts,
        hasFreeChoice,
        invalidChoice);
    if (picked == invalidChoice)
        return invalidChoice;

    assignments[choiceKey] = picked;
    state.activeChoiceKeys.insert(choiceKey);
    return picked;
}
