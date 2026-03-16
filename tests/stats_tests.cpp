#include <gtest/gtest.h>

#include <string>

#include "rule_engine.h"

namespace {

fm::AppPolicy BuildPolicy(const std::string& text) {
    fm::ParsedRules parsed;
    EXPECT_TRUE(fm::ParseRulesIni(text, &parsed));
    fm::AppPolicy policy;
    std::string error;
    EXPECT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    return policy;
}

TEST(MatchStatsTest, RecordsCounters) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/Secret\n"
        "+ Pictures/Share\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n");

    fm::ResolvedPathKindCache cache;
    fm::MatchStats stats;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    fm::MatchPathWithStats(policy, "/storage/emulated/0/DCIM/Secret/a.jpg", context, &cache, &stats);
    fm::MatchPathWithStats(policy, "/storage/emulated/0/Pictures/Share/a.jpg", context, &cache, &stats);
    fm::MatchPathWithStats(policy, "/storage/emulated/0/DCIM/Camera/a.jpg", context, &cache, &stats);
    fm::MatchPathWithStats(policy, "/storage/emulated/0/Movies/Clip/a.jpg", context, &cache, &stats);

    EXPECT_EQ(stats.calls.load(), 4u);
    EXPECT_EQ(stats.block.load(), 1u);
    EXPECT_EQ(stats.allow.load(), 2u);
    EXPECT_EQ(stats.redirect.load(), 1u);
    EXPECT_EQ(stats.no_match.load(), 0u);
    EXPECT_GE(stats.total_ns.load(), 0u);
    EXPECT_GE(stats.max_ns.load(), 0u);
}

TEST(MatchStatsTest, CountsNoMatchInWhitelist) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n");

    fm::ResolvedPathKindCache cache;
    fm::MatchStats stats;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    fm::MatchPathWithStats(policy, "/storage/emulated/0/Movies/Clip/a.jpg", context, &cache, &stats);
    EXPECT_EQ(stats.calls.load(), 1u);
    EXPECT_EQ(stats.block.load(), 1u);
}

}  // namespace
