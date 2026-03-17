#include <gtest/gtest.h>
#include <string>
#include "rule_engine.h"

namespace {

// 测试通配包名 [*] 的解析
TEST(WildcardPackageTest, ParsesWildcardSection) {
    const std::string text =
        "[*]\n"
        "mode = blacklist\n"
        "/storage/emulated/0/Download/ -> /storage/emulated/0/Documents/Auto/\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    EXPECT_EQ(parsed.sections[0].package_name, "*");
    ASSERT_EQ(parsed.sections[0].rules.size(), 1u);
    EXPECT_EQ(parsed.sections[0].rules[0].action, fm::RuleAction::kRedirect);
}

TEST(WildcardPackageTest, WildcardAndPackageSectionCoexist) {
    const std::string text =
        "[com.example.app]\n"
        "mode = blacklist\n"
        "- Download/secret\n"
        "[*]\n"
        "mode = blacklist\n"
        "/storage/emulated/0/Download/ -> /storage/emulated/0/Documents/Auto/\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 2u);
    EXPECT_EQ(parsed.sections[0].package_name, "com.example.app");
    EXPECT_EQ(parsed.sections[1].package_name, "*");
}

TEST(WildcardPackageTest, WildcardWithExtensionFilter) {
    const std::string text =
        "[*]\n"
        "mode = blacklist\n"
        "/storage/emulated/0/DCIM/ -> /storage/emulated/0/Pictures/Sorted/ @types=jpg,png,heic\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    ASSERT_EQ(parsed.sections[0].rules.size(), 1u);
    ASSERT_EQ(parsed.sections[0].rules[0].extensions.size(), 3u);
    EXPECT_EQ(parsed.sections[0].rules[0].extensions[0], "jpg");
    EXPECT_EQ(parsed.sections[0].rules[0].extensions[1], "png");
    EXPECT_EQ(parsed.sections[0].rules[0].extensions[2], "heic");
}

TEST(WildcardPackageTest, WildcardSectionEnabled) {
    const std::string text =
        "[*]\n"
        "enabled = false\n"
        "/storage/emulated/0/Download/ -> /storage/emulated/0/Documents/Auto/\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_FALSE(parsed.sections[0].enabled);
}

}  // namespace
