#include <gtest/gtest.h>

#include <string>

#include "rule_engine.h"

namespace {

TEST(ParserTest, ParsesUtf8SpacesAndRedirectAlias) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "\n"
        "+ Pictures/?? ??\n"
        "- Download/private.txt\n"
        "DCIM/Camera => Android/data/com.tencent.mm/cache/Camera\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    EXPECT_EQ(parsed.sections[0].package_name, "com.tencent.mm");
    EXPECT_EQ(parsed.sections[0].mode, fm::PolicyMode::kWhitelist);
    ASSERT_EQ(parsed.sections[0].rules.size(), 3u);
    EXPECT_EQ(parsed.sections[0].rules[0].action, fm::RuleAction::kAllow);
    EXPECT_EQ(parsed.sections[0].rules[0].path, "Pictures/?? ??");
    EXPECT_EQ(parsed.sections[0].rules[2].action, fm::RuleAction::kRedirectDynamic);
    EXPECT_EQ(parsed.sections[0].rules[2].path, "DCIM/Camera");
    EXPECT_EQ(parsed.sections[0].rules[2].redirect_target, "Android/data/com.tencent.mm/cache/Camera");
}

TEST(ParserTest, RejectsMalformedIni) {
    const std::string text =
        "mode = whitelist\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    ASSERT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
}

TEST(ParserTest, ParsesEnabledSwitches) {
    const std::string text =
        "[com.tencent.mm]\n"
        "enabled = false\n"
        "[com.tencent.mobileqq]\n"
        "disabled = true\n"
        "mode = whitelist\n"
        "+ Pictures/QQ\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 2u);
    EXPECT_FALSE(parsed.sections[0].enabled);
    EXPECT_TRUE(parsed.sections[1].has_mode);
    EXPECT_FALSE(parsed.sections[1].enabled);
}

TEST(ParserTest, ParsesDeleteRule) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "! Download/Cache\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    ASSERT_EQ(parsed.sections[0].rules.size(), 1u);
    EXPECT_EQ(parsed.sections[0].rules[0].action, fm::RuleAction::kDelete);
    EXPECT_EQ(parsed.sections[0].rules[0].path, "Download/Cache");
}

TEST(ParserTest, ParsesDeleteOptions) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "delete_existing = true\n"
        "delete_dirs = recursive\n"
        "! Download/Cache\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    EXPECT_TRUE(parsed.sections[0].delete_existing);
    EXPECT_EQ(parsed.sections[0].delete_dir_mode, fm::DeleteDirMode::kRecursive);
}

TEST(ParserTest, ParsesMediaQueryOption) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "media_query = false\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    EXPECT_EQ(parsed.sections[0].media_query, fm::MediaQueryMode::kDisable);
}

TEST(ParserTest, ParsesRedirectTypesFilter) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera @types=JPG,png,heic\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    ASSERT_EQ(parsed.sections[0].rules.size(), 1u);
    EXPECT_EQ(parsed.sections[0].rules[0].action, fm::RuleAction::kRedirect);
    ASSERT_EQ(parsed.sections[0].rules[0].extensions.size(), 3u);
    EXPECT_EQ(parsed.sections[0].rules[0].extensions[0], "jpg");
    EXPECT_EQ(parsed.sections[0].rules[0].extensions[1], "png");
    EXPECT_EQ(parsed.sections[0].rules[0].extensions[2], "heic");
}

TEST(ParserTest, ParsesTrashAndThresholdOptions) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "trash = true\n"
        "trash_redirect = true\n"
        "trash_dir = Android/data/<pkg>/trash\n"
        "min_age_days = 7\n"
        "min_size_mb = 12\n"
        "! Download/Cache\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    const auto& section = parsed.sections[0];
    EXPECT_TRUE(section.trash_enabled);
    EXPECT_TRUE(section.trash_on_redirect);
    EXPECT_EQ(section.trash_dir, "Android/data/<pkg>/trash");
    EXPECT_EQ(section.min_age_days, 7);
    EXPECT_EQ(section.min_size_mb, 12);
}

}  // namespace
