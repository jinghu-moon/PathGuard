#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include "rule_engine.h"

namespace {

fm::AppPolicy BuildBenchPolicy(int blocked_count = 32, int redirect_count = 32) {
    std::string text = "[com.tencent.mm]\nmode = blacklist\n";
    for (int i = 0; i < blocked_count; ++i) {
        text += "- DCIM/Blocked" + std::to_string(i) + "\n";
    }
    for (int i = 0; i < redirect_count; ++i) {
        text += "Folder" + std::to_string(i) + " -> Android/data/com.tencent.mm/cache/Folder" + std::to_string(i) + "\n";
    }

    fm::ParsedRules parsed;
    fm::ParseRulesIni(text, &parsed);
    fm::AppPolicy policy;
    std::string error;
    fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error);
    return policy;
}

fm::AppPolicy BuildBenchWhitelistPolicy() {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n"
        "+ Download/Send\n"
        "- DCIM/Secret\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n"
        "[com.tencent.mm.accessible.qqinput_temp]\n"
        "from = com.tencent.qqpinyin\n"
        "path = tencent/QQInput/Ext/Temp\n";

    fm::ParsedRules parsed;
    fm::ParseRulesIni(text, &parsed);
    fm::AppPolicy policy;
    std::string error;
    fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error);
    return policy;
}

size_t EstimateStringMemory(const std::string& value) {
    return value.capacity();
}

size_t EstimatePolicyMemory(const fm::AppPolicy& policy) {
    size_t total = sizeof(policy);
    total += policy.rules.capacity() * sizeof(fm::CompiledRule);
    total += policy.ordered_rules.capacity() * sizeof(fm::CompiledRule*);
    total += policy.accessible_rules.capacity() * sizeof(fm::CompiledAccessibleFolderRule);
    total += policy.export_rules.capacity() * sizeof(fm::CompiledExportFolderRule);
    total += EstimateStringMemory(policy.package_name);
    for (const auto& rule : policy.rules) {
        total += EstimateStringMemory(rule.path);
        total += EstimateStringMemory(rule.redirect_target);
    }
    for (const auto& rule : policy.accessible_rules) {
        total += EstimateStringMemory(rule.id);
        total += EstimateStringMemory(rule.anchor_package);
        total += EstimateStringMemory(rule.from_package);
        total += EstimateStringMemory(rule.to_package);
        total += EstimateStringMemory(rule.path);
        total += EstimateStringMemory(rule.description);
    }
    for (const auto& rule : policy.export_rules) {
        total += EstimateStringMemory(rule.id);
        total += EstimateStringMemory(rule.package_name);
        total += EstimateStringMemory(rule.source_path);
        total += EstimateStringMemory(rule.target_path);
        total += EstimateStringMemory(rule.title);
        total += EstimateStringMemory(rule.description);
    }
    total += policy.whitelist_allow_paths.capacity() * sizeof(std::string);
    for (const auto& path : policy.whitelist_allow_paths) {
        total += EstimateStringMemory(path);
    }
    total += policy.root_buckets.capacity() * sizeof(fm::RootBucket);
    for (const auto& bucket : policy.root_buckets) {
        total += EstimateStringMemory(bucket.segment);
        total += bucket.rules.capacity() * sizeof(fm::CompiledRule*);
        total += bucket.second_buckets.capacity() * sizeof(fm::SecondBucket);
        for (const auto& second : bucket.second_buckets) {
            total += EstimateStringMemory(second.segment);
            total += second.rules.capacity() * sizeof(fm::CompiledRule*);
        }
    }
    total += policy.external_root_rules.capacity() * sizeof(fm::CompiledRule*);
    return total;
}

void BM_MatchBlocked(benchmark::State& state) {
    const fm::AppPolicy policy = BuildBenchPolicy();
    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    for (auto _ : state) {
        auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Blocked7/demo.jpg", context, &cache);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_MatchBlocked);

void BM_MatchRedirect(benchmark::State& state) {
    const fm::AppPolicy policy = BuildBenchPolicy();
    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    for (auto _ : state) {
        auto result = fm::MatchPath(policy, "/storage/emulated/0/Folder18/demo.jpg", context, &cache);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_MatchRedirect);

void BM_MatchWhitelistAllowed(benchmark::State& state) {
    const fm::AppPolicy policy = BuildBenchWhitelistPolicy();
    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    for (auto _ : state) {
        auto result = fm::MatchPath(policy, "/storage/emulated/0/Pictures/Share/a.jpg", context, &cache);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_MatchWhitelistAllowed);

void BM_MatchWhitelistBlocked(benchmark::State& state) {
    const fm::AppPolicy policy = BuildBenchWhitelistPolicy();
    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    for (auto _ : state) {
        auto result = fm::MatchPath(policy, "/storage/emulated/0/Movies/Clip/a.jpg", context, &cache);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_MatchWhitelistBlocked);

void BM_MatchBatch10k(benchmark::State& state) {
    const fm::AppPolicy policy = BuildBenchPolicy();
    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;
    std::vector<std::string> paths;
    paths.reserve(10000);
    for (int i = 0; i < 5000; ++i) {
        paths.push_back("/storage/emulated/0/DCIM/Blocked7/" + std::to_string(i) + ".jpg");
        paths.push_back("/storage/emulated/0/Folder18/" + std::to_string(i) + ".jpg");
    }

    for (auto _ : state) {
        for (const auto& current : paths) {
            auto result = fm::MatchPath(policy, current, context, &cache);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(paths.size()));
}
BENCHMARK(BM_MatchBatch10k);

void BM_PolicyMemoryUsage(benchmark::State& state) {
    const int rule_count = static_cast<int>(state.range(0));
    const fm::AppPolicy policy = BuildBenchPolicy(rule_count, rule_count);
    const size_t bytes = EstimatePolicyMemory(policy);
    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }
    state.counters["rules"] = static_cast<double>(rule_count * 2);
    state.counters["approx_bytes"] = static_cast<double>(bytes);
}
BENCHMARK(BM_PolicyMemoryUsage)->Arg(64)->Arg(256)->Arg(1024);

void BM_PathKindCacheReadShared(benchmark::State& state) {
    static fm::ResolvedPathKindCache cache;
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 1024; ++i) {
            cache.Put("/storage/emulated/0/DCIM/Cache/" + std::to_string(i), fm::PathKind::kDirectory);
        }
        initialized = true;
    }

    for (auto _ : state) {
        fm::PathKind kind = fm::PathKind::kAuto;
        benchmark::DoNotOptimize(cache.TryGet("/storage/emulated/0/DCIM/Cache/512", &kind));
        benchmark::DoNotOptimize(kind);
    }
}
BENCHMARK(BM_PathKindCacheReadShared)->Threads(1);
BENCHMARK(BM_PathKindCacheReadShared)->Threads(8);

void BM_MatchBlockedWithStats(benchmark::State& state) {
    const fm::AppPolicy policy = BuildBenchPolicy();
    fm::ResolvedPathKindCache cache;
    fm::MatchStats stats;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    for (auto _ : state) {
        auto result = fm::MatchPathWithStats(policy, "/storage/emulated/0/DCIM/Blocked7/demo.jpg", context, &cache, &stats);
        benchmark::DoNotOptimize(result);
    }
    benchmark::DoNotOptimize(stats.calls.load());
}
BENCHMARK(BM_MatchBlockedWithStats);

void BM_PathKindCacheMemoryUsage(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    fm::ResolvedPathKindCache cache;
    for (int i = 0; i < count; ++i) {
        cache.Put("/storage/emulated/0/DCIM/Cache/" + std::to_string(i), fm::PathKind::kDirectory);
    }
    const size_t bytes = cache.EstimatedMemoryUsage();
    for (auto _ : state) {
        benchmark::DoNotOptimize(bytes);
    }
    state.counters["entries"] = static_cast<double>(count);
    state.counters["approx_bytes"] = static_cast<double>(bytes);
}
BENCHMARK(BM_PathKindCacheMemoryUsage)->Arg(256)->Arg(1024)->Arg(4096);

void BM_PathKindCacheWrite(benchmark::State& state) {
    fm::ResolvedPathKindCache cache;
    int counter = 0;
    for (auto _ : state) {
        cache.Put("/storage/emulated/0/DCIM/Write/" + std::to_string(counter++), fm::PathKind::kDirectory);
    }
    benchmark::DoNotOptimize(cache.Size());
}
BENCHMARK(BM_PathKindCacheWrite)->Threads(1);
BENCHMARK(BM_PathKindCacheWrite)->Threads(8);

}  // namespace

BENCHMARK_MAIN();
