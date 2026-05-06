#include "orc_ini_cache.h"

#include "orc_path.h"

#include <string>
#include <unordered_map>
#include <utility>

struct OrcIniCacheEntry {
    uint64_t lastWriteTicks = 0;
    OrcIniDocument doc;
};

static std::unordered_map<std::string, OrcIniCacheEntry> g_orcIniDocCache;

const OrcIniDocument* OrcIniCacheGet(const char* pathUtf8) {
    if (!pathUtf8 || !pathUtf8[0]) return nullptr;

    const std::string key(pathUtf8);
    const uint64_t wt = OrcFileLastWriteUtcTicks(pathUtf8);
    if (wt == 0 && !OrcFileExistsA(pathUtf8)) {
        g_orcIniDocCache.erase(key);
        return nullptr;
    }

    auto it = g_orcIniDocCache.find(key);
    if (it != g_orcIniDocCache.end() && it->second.lastWriteTicks == wt && it->second.doc.IsLoaded())
        return &it->second.doc;

    OrcIniDocument fresh;
    if (!fresh.LoadFromFile(pathUtf8)) {
        g_orcIniDocCache.erase(key);
        return nullptr;
    }

    OrcIniCacheEntry entry;
    entry.lastWriteTicks = wt;
    entry.doc = std::move(fresh);
    const auto ins = g_orcIniDocCache.insert_or_assign(key, std::move(entry));
    return &ins.first->second.doc;
}

void OrcIniCacheInvalidatePath(const char* pathUtf8) {
    if (!pathUtf8 || !pathUtf8[0]) return;
    g_orcIniDocCache.erase(std::string(pathUtf8));
}

void OrcIniCacheInvalidateAll() { g_orcIniDocCache.clear(); }
