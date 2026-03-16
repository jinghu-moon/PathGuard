#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "path_kind_cache.h"

namespace {

TEST(PathKindCacheTest, SupportsConcurrentReadWrite) {
    fm::ResolvedPathKindCache cache;
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;

    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&cache, &start, i]() {
            while (!start.load()) {
            }
            for (int n = 0; n < 1000; ++n) {
                cache.Put("/storage/emulated/0/DCIM/" + std::to_string(i) + "/" + std::to_string(n), fm::PathKind::kDirectory);
            }
        });
    }

    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&cache, &start]() {
            while (!start.load()) {
            }
            for (int n = 0; n < 1000; ++n) {
                fm::PathKind kind = fm::PathKind::kAuto;
                cache.TryGet("/storage/emulated/0/DCIM/1/1", &kind);
            }
        });
    }

    start.store(true);
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(cache.Size(), 4000u);
}

TEST(PathKindCacheTest, EraseRemovesEntry) {
    fm::ResolvedPathKindCache cache;
    cache.Put("/storage/emulated/0/DCIM/A-TEST", fm::PathKind::kDirectory);
    cache.Erase("/storage/emulated/0/DCIM/A-TEST");
    fm::PathKind kind = fm::PathKind::kAuto;
    EXPECT_FALSE(cache.TryGet("/storage/emulated/0/DCIM/A-TEST", &kind));
}

TEST(PathKindCacheTest, EstimatedMemoryUsageIncreasesWithEntries) {
    fm::ResolvedPathKindCache cache;
    const size_t empty_bytes = cache.EstimatedMemoryUsage();
    cache.Put("/storage/emulated/0/DCIM/A-TEST", fm::PathKind::kDirectory);
    const size_t one_bytes = cache.EstimatedMemoryUsage();
    cache.Put("/storage/emulated/0/DCIM/B-TEST", fm::PathKind::kFile);
    const size_t two_bytes = cache.EstimatedMemoryUsage();
    EXPECT_GE(one_bytes, empty_bytes);
    EXPECT_GE(two_bytes, one_bytes);
    cache.Clear();
    const size_t cleared_bytes = cache.EstimatedMemoryUsage();
    EXPECT_LE(cleared_bytes, two_bytes);
}

}  // namespace
