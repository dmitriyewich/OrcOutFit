#include "orc_weapon_preset_async.h"

#include "orc_path.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace {

struct WeaponSkinPresetWork {
    uint64_t enqueueEpoch = 0;
    std::string skinKey;
    std::string iniPath;
    std::vector<WeaponCfg> baseW1;
    std::vector<WeaponCfg> baseW2;
};

std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<WeaponSkinPresetWork> g_pending;
std::deque<OrcWeaponSkinPresetLoaded> g_completed;
std::atomic<uint64_t> g_invalidateEpoch{0};
std::atomic<bool> g_stopWorker{false};
std::thread g_worker;
std::atomic<bool> g_workerStarted{false};

bool ReadEntireFileUtf8(const char* path, std::string& out) {
    out.clear();
    if (!path || !path[0])
        return false;
    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;
#else
    f = std::fopen(path, "rb");
    if (!f)
        return false;
#endif
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long len = std::ftell(f);
    if (len < 0) {
        std::fclose(f);
        return false;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(len));
    if (len > 0) {
        const size_t n = std::fread(&out[0], 1, static_cast<size_t>(len), f);
        if (n != static_cast<size_t>(len)) {
            std::fclose(f);
            out.clear();
            return false;
        }
    }
    std::fclose(f);
    return true;
}

void WorkerLoop() {
    for (;;) {
        WeaponSkinPresetWork work{};
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_stopWorker.load(std::memory_order_acquire) || !g_pending.empty(); });
            if (g_stopWorker.load(std::memory_order_acquire) && g_pending.empty())
                return;
            if (g_pending.empty())
                continue;
            work = std::move(g_pending.front());
            g_pending.pop_front();
        }

        OrcWeaponSkinPresetLoaded r;
        r.enqueueEpoch = work.enqueueEpoch;
        r.skinKey = work.skinKey;
        r.writeTicks = OrcFileLastWriteUtcTicks(work.iniPath.c_str());

        std::string bytes;
        const bool readOk = ReadEntireFileUtf8(work.iniPath.c_str(), bytes);
        OrcIniDocument doc;
        const bool parsed =
            readOk && !bytes.empty() && doc.LoadFromMemory(bytes.data(), bytes.size());

        OrcIniDocument useDoc;
        if (parsed)
            useDoc = std::move(doc);

        OrcBuildWeaponSkinPresetFromIniDocument(useDoc, work.baseW1, work.baseW2, r.w1, r.w2, r.h1, r.h2);

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_completed.push_back(std::move(r));
        }
    }
}

void EnsureWorkerStarted() {
    if (g_workerStarted.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_workerStarted.load(std::memory_order_acquire))
        return;
    g_stopWorker.store(false, std::memory_order_release);
    g_worker = std::thread(WorkerLoop);
    g_workerStarted.store(true, std::memory_order_release);
}

} // namespace

uint64_t OrcWeaponSkinPresetGetInvalidateEpoch() {
    return g_invalidateEpoch.load(std::memory_order_acquire);
}

void OrcWeaponSkinPresetEnqueueLoad(
    std::string skinKeyLower,
    std::string iniPathUtf8,
    std::vector<WeaponCfg> baseW1,
    std::vector<WeaponCfg> baseW2) {
    if (skinKeyLower.empty() || iniPathUtf8.empty())
        return;

    EnsureWorkerStarted();
    if (!g_workerStarted.load(std::memory_order_acquire))
        return;

    WeaponSkinPresetWork w;
    w.enqueueEpoch = g_invalidateEpoch.load(std::memory_order_acquire);
    w.skinKey = std::move(skinKeyLower);
    w.iniPath = std::move(iniPathUtf8);
    w.baseW1 = std::move(baseW1);
    w.baseW2 = std::move(baseW2);

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pending.push_back(std::move(w));
    }
    g_cv.notify_one();
}

bool OrcWeaponSkinPresetTryPopCompleted(OrcWeaponSkinPresetLoaded& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_completed.empty())
        return false;
    out = std::move(g_completed.front());
    g_completed.pop_front();
    return true;
}

void OrcWeaponSkinPresetInvalidateAsyncState() {
    g_invalidateEpoch.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lk(g_mtx);
    g_pending.clear();
    g_completed.clear();
}

void OrcWeaponSkinPresetAsyncShutdown() {
    g_stopWorker.store(true, std::memory_order_release);
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    g_workerStarted.store(false, std::memory_order_release);
    g_stopWorker.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_mtx);
    g_pending.clear();
    g_completed.clear();
}

bool OrcWeaponSkinPresetLoadInFlightForKey(const std::string& skinKeyLower) {
    if (skinKeyLower.empty())
        return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    for (const auto& w : g_pending) {
        if (w.skinKey == skinKeyLower)
            return true;
    }
    for (const auto& c : g_completed) {
        if (c.skinKey == skinKeyLower)
            return true;
    }
    return false;
}
