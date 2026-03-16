#include <gtest/gtest.h>

#include <string>

#include "rule_config.h"

namespace {

TEST(RuleConfigTest, TracksRuleHitCounts) {
    const char *text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/A-TEST\n"
        "+ Pictures/Share\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n";

    FmRuleSet rule_set{};
    ASSERT_TRUE(fm_load_rules_from_text(text, "com.tencent.mm", &rule_set));
    ASSERT_TRUE(fm_has_matching_rule(&rule_set));

    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_OPEN;

    FmPathDecision decision;
    fm_decide_path_with_context(&rule_set, "/storage/emulated/0/DCIM/A-TEST/1.jpg", &context, &decision);
    fm_decide_path_with_context(&rule_set, "/storage/emulated/0/Pictures/Share/1.jpg", &context, &decision);
    fm_decide_path_with_context(&rule_set, "/storage/emulated/0/DCIM/Camera/1.jpg", &context, &decision);
    fm_decide_path_with_context(&rule_set, "/storage/emulated/0/DCIM/Camera/2.jpg", &context, &decision);

    uint64_t count = 0;
    ASSERT_TRUE(fm_get_rule_hit_count(&rule_set, 0, &count));
    EXPECT_EQ(count, 1u);
    ASSERT_TRUE(fm_get_rule_hit_count(&rule_set, 1, &count));
    EXPECT_EQ(count, 1u);
    ASSERT_TRUE(fm_get_rule_hit_count(&rule_set, 2, &count));
    EXPECT_EQ(count, 2u);
    EXPECT_FALSE(fm_get_rule_hit_count(&rule_set, 99, &count));
}

TEST(RuleConfigTest, LoadFromTextHandlesDisabledPackage) {
    const char *text =
        "[com.tencent.mm]\n"
        "enabled = false\n"
        "mode = blacklist\n"
        "- DCIM/A-TEST\n";

    FmRuleSet rule_set{};
    ASSERT_TRUE(fm_load_rules_from_text(text, "com.tencent.mm", &rule_set));
    EXPECT_FALSE(fm_has_matching_rule(&rule_set));
}

TEST(RuleConfigTest, MediaQueryModePropagates) {
    const char *text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "media_query = false\n"
        "+ Pictures/Share\n";

    FmRuleSet rule_set{};
    ASSERT_TRUE(fm_load_rules_from_text(text, "com.tencent.mm", &rule_set));
    EXPECT_EQ(rule_set.media_query_mode, FM_MEDIA_QUERY_DISABLE);
}

}  // namespace
