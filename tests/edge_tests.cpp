#include <gtest/gtest.h>

#include <string>

#include "path_mapper.h"
#include "rule_engine.h"

namespace {

fm::ParsedRules ParseRules(const std::string& text) {
    fm::ParsedRules parsed;
    EXPECT_TRUE(fm::ParseRulesIni(text, &parsed));
    return parsed;
}

fm::AppPolicy BuildPolicy(const std::string& text, const std::string& process_name) {
    fm::ParsedRules parsed = ParseRules(text);
    fm::AppPolicy policy;
    std::string error;
    EXPECT_TRUE(fm::CompilePolicyForProcess(parsed, process_name, {}, &policy, &error)) << error;
    return policy;
}

TEST(ParserEdgeTest, RejectsDuplicateSection) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "[com.tencent.mm]\n"
        "mode = blacklist\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("duplicate section"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsMissingMode) {
    const std::string text =
        "[com.tencent.mm]\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("missing mode"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsUnknownAccessibleField) {
    const std::string text =
        "[com.tencent.mm.accessible.demo]\n"
        "from = com.tencent.mobileqq\n"
        "path = tencent\n"
        "foo = bar\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("unknown accessible field"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsInvalidExportBoolean) {
    const std::string text =
        "[com.tencent.mm.export.demo]\n"
        "source = tencent/QQfile_recv\n"
        "target = Download/QQ\n"
        "media_scan = yes\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("unknown export field"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsInvalidEnabledValue) {
    const std::string text =
        "[com.tencent.mm]\n"
        "enabled = maybe\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("invalid enabled value"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsInvalidDeleteDirsValue) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "delete_dirs = invalid\n"
        "! Download/Cache\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("invalid delete_dirs value"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsInvalidMediaQueryValue) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "media_query = maybe\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("invalid media_query value"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsTypesOnNonRedirectRule) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "+ Download/Cache @types=jpg\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("types only supported on redirect rules"), std::string::npos);
}

TEST(ParserEdgeTest, RejectsInvalidTrashOptions) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "trash = maybe\n"
        "min_age_days = -1\n";

    fm::ParsedRules parsed;
    EXPECT_FALSE(fm::ParseRulesIni(text, &parsed));
    ASSERT_FALSE(parsed.errors.empty());
    EXPECT_NE(parsed.errors.front().find("invalid trash value"), std::string::npos);
}

TEST(ParserEdgeTest, DisabledSectionSkipsModeValidation) {
    const std::string text =
        "[com.tencent.mm]\n"
        "enabled = false\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    EXPECT_TRUE(fm::ParseRulesIni(text, &parsed));
    EXPECT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    EXPECT_FALSE(parsed.sections[0].enabled);
}

TEST(CompilerEdgeTest, RejectsNoMatchingPolicy) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    fm::AppPolicy policy;
    std::string error;
    EXPECT_FALSE(fm::CompilePolicyForProcess(parsed, "com.tencent.mobileqq", {}, &policy, &error));
    EXPECT_EQ(error, "no matching policy");
}

TEST(CompilerEdgeTest, ExpandsPlaceholderInAccessibleAndExport) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Android/data/<pkg>/cache\n"
        "[com.tencent.mm.accessible.demo]\n"
        "to = com.tencent.mm\n"
        "path = Android/data/<pkg>/shared\n"
        "[com.tencent.mm.export.demo]\n"
        "source = Android/data/<pkg>/source\n"
        "target = Android/data/<pkg>/target\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    ASSERT_FALSE(policy.accessible_rules.empty());
    EXPECT_EQ(policy.accessible_rules[0].path, "/storage/emulated/0/Android/data/com.tencent.mm/shared");
    ASSERT_FALSE(policy.export_rules.empty());
    EXPECT_EQ(policy.export_rules[0].source_path, "/storage/emulated/0/Android/data/com.tencent.mm/source");
    EXPECT_EQ(policy.export_rules[0].target_path, "/storage/emulated/0/Android/data/com.tencent.mm/target");
}

TEST(CompilerEdgeTest, InfersFileKindFromExtensionFallback) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "- Download/demo.jpg\n"
        "- Movies/Clip\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 2u);
    EXPECT_EQ(policy.rules[0].path_kind, fm::PathKind::kFile);
    EXPECT_EQ(policy.rules[1].path_kind, fm::PathKind::kAuto);
}

TEST(CompilerEdgeTest, NormalizesExternalAliases) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "- /sdcard/Movies\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 1u);
    EXPECT_EQ(policy.rules[0].path, "/storage/emulated/0/Movies");
}

TEST(CompilerEdgeTest, AcceptsPathGlobInDirectorySegment) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "- DCIM/*/IMG.jpg\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 1u);
    EXPECT_TRUE(policy.rules[0].has_path_glob);
}

TEST(MatcherEdgeTest, DenyOverridesAllowAndRedirect) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "+ DCIM/Camera\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n"
        "- DCIM/Camera\n",
        "com.tencent.mm");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;
    auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/a.jpg", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kBlock);
}

TEST(MatcherEdgeTest, FileRuleDoesNotMatchChildPath) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- Download/private.txt\n",
        "com.tencent.mm");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;
    auto result = fm::MatchPath(policy, "/storage/emulated/0/Download/private.txt.bak", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherEdgeTest, AlreadyRedirectedSkipsEvaluation) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/Camera\n",
        "com.tencent.mm");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;
    context.already_redirected = true;
    auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/a.jpg", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherEdgeTest, EmptyInputReturnsNoMatch) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/Camera\n",
        "com.tencent.mm");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;
    auto result = fm::MatchPath(policy, "", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kNoMatch);
}

TEST(MatcherEdgeTest, ShouldHideDirEntryBlocksMatchedChild) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/Secret\n",
        "com.tencent.mm");

    fm::ResolvedPathKindCache cache;
    EXPECT_TRUE(fm::ShouldHideDirEntry(policy, "/storage/emulated/0/DCIM", "Secret", &cache));
    EXPECT_FALSE(fm::ShouldHideDirEntry(policy, "/storage/emulated/0/DCIM", "Public", &cache));
}

TEST(PathMapperEdgeTest, NormalizesAliasAndTrimsSlash) {
    char output[256] = {0};
    ASSERT_TRUE(fm_normalize_path("/sdcard/Download/", output, sizeof(output)));
    EXPECT_STREQ(output, "/storage/emulated/0/Download");
}

TEST(PathMapperEdgeTest, RelativeExternalPathRejectsRoot) {
    char output[256] = {0};
    EXPECT_FALSE(fm_get_relative_external_path("/storage/emulated/0", output, sizeof(output)));
    EXPECT_FALSE(fm_get_relative_external_path("/data/local/tmp", output, sizeof(output)));
    ASSERT_TRUE(fm_get_relative_external_path("/storage/emulated/0/DCIM", output, sizeof(output)));
    EXPECT_STREQ(output, "DCIM");
}

TEST(PathMapperEdgeTest, JoinPathUsesAbsoluteEntry) {
    char output[256] = {0};
    ASSERT_TRUE(fm_join_path("/storage/emulated/0/DCIM", "/sdcard/Movies", output, sizeof(output)));
    EXPECT_STREQ(output, "/storage/emulated/0/Movies");
}

TEST(PathMapperEdgeTest, DotPathDetection) {
    EXPECT_TRUE(fm_is_dot_or_dotdot("."));
    EXPECT_TRUE(fm_is_dot_or_dotdot(".."));
    EXPECT_FALSE(fm_is_dot_or_dotdot("..."));
    EXPECT_FALSE(fm_is_dot_or_dotdot("a"));
}

}  // namespace
