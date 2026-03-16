#include <gtest/gtest.h>

#include <string>

#include "rule_engine.h"

namespace {

fm::ParsedRules ParseRules(const std::string& text) {
    fm::ParsedRules parsed;
    EXPECT_TRUE(fm::ParseRulesIni(text, &parsed));
    return parsed;
}

fm::AppPolicy BuildPolicyForProcess(const std::string& text, const std::string& process_name) {
    fm::ParsedRules parsed = ParseRules(text);
    fm::AppPolicy policy;
    std::string error;
    EXPECT_TRUE(fm::CompilePolicyForProcess(parsed, process_name, {}, &policy, &error)) << error;
    return policy;
}

fm::AppPolicy BuildPolicy(const std::string& text) {
    return BuildPolicyForProcess(text, "com.tencent.mm");
}

TEST(ParserTest, ParsesAccessibleAndExportSections) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n"
        "[com.tencent.mm.accessible.qqinput_temp]\n"
        "from = com.tencent.qqpinyin\n"
        "path = tencent/QQInput/Ext/Temp\n"
        "description = 修复 QQ 拼音向微信发送图片\n"
        "[com.tencent.mm.accessible.tencent_filemanager]\n"
        "to = com.tencent.FileManager\n"
        "path = tencent\n"
        "[com.tencent.mm.export.saved_images]\n"
        "source = tencent/MicroMsg/WeiXin\n"
        "target = Pictures/WeChat\n"
        "title = 保存的图片\n"
        "media_scan = true\n"
        "add_to_downloads = false\n"
        "allow_child = false\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));
    ASSERT_TRUE(parsed.errors.empty());
    ASSERT_EQ(parsed.sections.size(), 1u);
    ASSERT_EQ(parsed.accessible_sections.size(), 2u);
    ASSERT_EQ(parsed.export_sections.size(), 1u);

    EXPECT_EQ(parsed.accessible_sections[0].anchor_package, "com.tencent.mm");
    EXPECT_EQ(parsed.accessible_sections[0].rule_id, "qqinput_temp");
    EXPECT_EQ(parsed.accessible_sections[0].from_package, "com.tencent.qqpinyin");
    EXPECT_EQ(parsed.accessible_sections[0].path, "tencent/QQInput/Ext/Temp");

    EXPECT_EQ(parsed.accessible_sections[1].rule_id, "tencent_filemanager");
    EXPECT_EQ(parsed.accessible_sections[1].to_package, "com.tencent.FileManager");

    EXPECT_EQ(parsed.export_sections[0].rule_id, "saved_images");
    EXPECT_EQ(parsed.export_sections[0].source_path, "tencent/MicroMsg/WeiXin");
    EXPECT_TRUE(parsed.export_sections[0].media_scan);
    EXPECT_FALSE(parsed.export_sections[0].add_to_downloads);
}

TEST(CompileTest, CompilesAccessibleAndExportSectionsWithDefaults) {
    const auto policy = BuildPolicyForProcess(
        "[com.tencent.mobileqq]\n"
        "mode = whitelist\n"
        "+ Pictures/QQ\n"
        "[com.tencent.mobileqq.accessible.qqinput_temp]\n"
        "from = com.tencent.qqpinyin\n"
        "path = tencent/QQInput/Ext/Temp\n"
        "description = 修复 QQ 拼音向 QQ 发送图片\n"
        "[com.tencent.mobileqq.accessible.tencent_filemanager]\n"
        "to = com.tencent.FileManager\n"
        "path = tencent\n"
        "[com.tencent.mobileqq.export.saved_images]\n"
        "source = tencent/QQ_Images\n"
        "target = Pictures/QQ\n"
        "title = 保存的图片\n"
        "media_scan = true\n"
        "add_to_downloads = false\n"
        "allow_child = false\n",
        "com.tencent.mobileqq");

    ASSERT_EQ(policy.accessible_rules.size(), 1u);
    EXPECT_EQ(policy.accessible_rules[0].id, "qqinput_temp");
    EXPECT_EQ(policy.accessible_rules[0].anchor_package, "com.tencent.mobileqq");
    EXPECT_EQ(policy.accessible_rules[0].from_package, "com.tencent.qqpinyin");
    EXPECT_EQ(policy.accessible_rules[0].to_package, "com.tencent.mobileqq");
    EXPECT_EQ(policy.accessible_rules[0].path, "/storage/emulated/0/tencent/QQInput/Ext/Temp");
    EXPECT_EQ(policy.accessible_rules[0].path_kind, fm::PathKind::kDirectory);

    ASSERT_EQ(policy.export_rules.size(), 1u);
    EXPECT_EQ(policy.export_rules[0].id, "saved_images");
    EXPECT_EQ(policy.export_rules[0].package_name, "com.tencent.mobileqq");
    EXPECT_EQ(policy.export_rules[0].source_path, "/storage/emulated/0/tencent/QQ_Images");
    EXPECT_EQ(policy.export_rules[0].target_path, "/storage/emulated/0/Pictures/QQ");
    EXPECT_EQ(policy.export_rules[0].title, "保存的图片");
    EXPECT_TRUE(policy.export_rules[0].media_scan);
    EXPECT_FALSE(policy.export_rules[0].add_to_downloads);
    EXPECT_FALSE(policy.export_rules[0].allow_child);
}

TEST(CompileTest, CompilesCrossPackageAccessibleRulesForTargetProcess) {
    const auto policy = BuildPolicyForProcess(
        "[com.tencent.FileManager]\n"
        "mode = whitelist\n"
        "+ Download\n"
        "[com.tencent.mobileqq.accessible.tencent_filemanager]\n"
        "to = com.tencent.FileManager\n"
        "path = tencent\n",
        "com.tencent.FileManager");

    ASSERT_EQ(policy.accessible_rules.size(), 1u);
    EXPECT_EQ(policy.accessible_rules[0].anchor_package, "com.tencent.mobileqq");
    EXPECT_EQ(policy.accessible_rules[0].from_package, "com.tencent.mobileqq");
    EXPECT_EQ(policy.accessible_rules[0].to_package, "com.tencent.FileManager");
    EXPECT_EQ(policy.accessible_rules[0].path, "/storage/emulated/0/tencent");
}

TEST(MatcherTest, DirectoryBoundarySpoofingDoesNotMatchSiblingPrefix) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/A-TEST\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kEnumerateDirectory;

    auto blocked = fm::MatchPath(policy, "/storage/emulated/0/DCIM/A-TEST/1.jpg", context, &cache);
    auto exact = fm::MatchPath(policy, "/storage/emulated/0/DCIM/A-TEST", context, &cache);
    auto sibling = fm::MatchPath(policy, "/storage/emulated/0/DCIM/A-TEST_backup/1.jpg", context, &cache);

    EXPECT_EQ(blocked.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(exact.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(sibling.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, AutoRuleUsesContextToResolvePathKind) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- Missing/Folder\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext dir_context;
    dir_context.operation = fm::PathOperation::kEnumerateDirectory;
    auto dir_block = fm::MatchPath(policy, "/storage/emulated/0/Missing/Folder/child.txt", dir_context, &cache);

    fm::RuntimeContext file_context;
    file_context.operation = fm::PathOperation::kCreateFile;
    auto file_block = fm::MatchPath(policy, "/storage/emulated/0/Missing/Folder", file_context, &cache);

    EXPECT_EQ(dir_block.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(dir_block.resolved_kind, fm::PathKind::kDirectory);
    EXPECT_EQ(file_block.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(file_block.resolved_kind, fm::PathKind::kFile);
}

TEST(MatcherTest, WhitelistAllowsExactFileOnly) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Download/private.txt\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto file_ok = fm::MatchPath(policy, "/storage/emulated/0/Download/private.txt", context, &cache);
    auto sibling_block = fm::MatchPath(policy, "/storage/emulated/0/Download/other.txt", context, &cache);
    auto dir_allow = fm::MatchPath(policy, "/storage/emulated/0/Download", context, &cache);

    EXPECT_EQ(file_ok.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(sibling_block.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(dir_allow.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, WhitelistAllowsAncestorsOfAllowedPath) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kStat;

    auto root = fm::MatchPath(policy, "/storage/emulated/0", context, &cache);
    auto parent = fm::MatchPath(policy, "/storage/emulated/0/Pictures", context, &cache);
    auto sibling = fm::MatchPath(policy, "/storage/emulated/0/Pictures/Other", context, &cache);

    EXPECT_EQ(root.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(parent.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(sibling.decision, fm::MatchDecision::kBlock);
}

TEST(MatcherTest, DeleteRuleDoesNotChangeAccessDecision) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "! Download/Cache\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto result = fm::MatchPath(policy, "/storage/emulated/0/Download/Cache/a.txt", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kBlock);
}

TEST(MatcherTest, ReturnsMatchedRuleIndex) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/A-TEST\n"
        "+ Pictures/Share\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto block_result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/A-TEST/1.jpg", context, &cache);
    auto allow_result = fm::MatchPath(policy, "/storage/emulated/0/Pictures/Share/1.jpg", context, &cache);
    auto redirect_result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/1.jpg", context, &cache);

    EXPECT_EQ(block_result.matched_rule_index, 0u);
    EXPECT_EQ(allow_result.matched_rule_index, 1u);
    EXPECT_EQ(redirect_result.matched_rule_index, 2u);
}

TEST(MatcherTest, NoMatchReturnsInvalidRuleIndex) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/A-TEST\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto result = fm::MatchPath(policy, "/storage/emulated/0/Movies/Clip/a.jpg", context, &cache);
    EXPECT_EQ(result.matched_rule_index, fm::kInvalidRuleIndex);
    EXPECT_EQ(result.matched_rule, nullptr);
}

TEST(MatcherTest, DirectoryRedirectAppendsSuffix) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/1.jpg", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(result.redirect_path, "/storage/emulated/0/Android/data/com.tencent.mm/cache/Camera/1.jpg");
}

TEST(MatcherTest, RedirectRespectsTypeFilters) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera @types=jpg\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto jpg = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/1.jpg", context, &cache);
    auto png = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/1.png", context, &cache);

    EXPECT_EQ(jpg.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(png.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, GlobMatchesFileNameOnly) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/Camera/IMG_*.jpg\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto match = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/IMG_0001.jpg", context, &cache);
    auto other_ext = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/IMG_0001.png", context, &cache);
    auto child_dir = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/Sub/IMG_0001.jpg", context, &cache);

    EXPECT_EQ(match.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(other_ext.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(child_dir.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, GlobRedirectKeepsFileName) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/Camera/IMG_*.jpg -> Android/data/com.tencent.mm/cache/Camera\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/IMG_0002.jpg", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(result.redirect_path, "/storage/emulated/0/Android/data/com.tencent.mm/cache/Camera/IMG_0002.jpg");
}

TEST(MatcherTest, PathGlobMatchesSingleSegment) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/*/IMG_*.jpg\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto match = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/IMG_0001.jpg", context, &cache);
    auto nested = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/Sub/IMG_0001.jpg", context, &cache);
    auto other = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/IMG_0001.png", context, &cache);

    EXPECT_EQ(match.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(nested.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(other.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, DoubleStarMatchesNestedPaths) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/**/IMG_*.jpg\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto direct = fm::MatchPath(policy, "/storage/emulated/0/DCIM/IMG_0001.jpg", context, &cache);
    auto nested = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/Sub/IMG_0001.jpg", context, &cache);
    auto other = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/Sub/IMG_0001.png", context, &cache);

    EXPECT_EQ(direct.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(nested.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(other.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, PathGlobRedirectKeepsSuffix) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/Camera/**/IMG_*.jpg -> Android/data/<pkg>/cache\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/Sub/IMG_0003.jpg", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(result.redirect_path, "/storage/emulated/0/Android/data/com.tencent.mm/cache/Sub/IMG_0003.jpg");
}

TEST(MatcherTest, FileRedirectMatchesExactPath) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "Download/private.txt -> Download/hidden.txt\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto exact = fm::MatchPath(policy, "/storage/emulated/0/Download/private.txt", context, &cache);
    auto sibling = fm::MatchPath(policy, "/storage/emulated/0/Download/private.txt.bak", context, &cache);

    EXPECT_EQ(exact.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(exact.redirect_path, "/storage/emulated/0/Download/hidden.txt");
    EXPECT_EQ(sibling.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, WhitelistAllowsAppPrivateExternalDirs) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto private_dir = fm::MatchPath(policy, "/storage/emulated/0/Android/data/com.tencent.mm/MicroMsg/xlog", context, &cache);
    auto private_media = fm::MatchPath(policy, "/storage/emulated/0/Android/media/com.tencent.mm/Wechat", context, &cache);

    EXPECT_EQ(private_dir.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(private_media.decision, fm::MatchDecision::kAllow);
}

TEST(MatcherTest, WhitelistAllowsAccessibleFolders) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n"
        "[com.tencent.mm.accessible.qqinput_temp]\n"
        "from = com.tencent.qqpinyin\n"
        "path = tencent/QQInput/Ext/Temp\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto parent = fm::MatchPath(policy, "/storage/emulated/0/tencent", context, &cache);
    auto allowed = fm::MatchPath(policy, "/storage/emulated/0/tencent/QQInput/Ext/Temp/sticker.gif", context, &cache);
    auto blocked = fm::MatchPath(policy, "/storage/emulated/0/tencent/QQInput/Other/sticker.gif", context, &cache);

    EXPECT_EQ(parent.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(allowed.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(blocked.decision, fm::MatchDecision::kBlock);
}

TEST(MatcherTest, ExplicitDenyOverridesAccessibleFolders) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n"
        "- tencent/QQInput/Ext/Temp\n"
        "[com.tencent.mm.accessible.qqinput_temp]\n"
        "from = com.tencent.qqpinyin\n"
        "path = tencent/QQInput/Ext/Temp\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto blocked = fm::MatchPath(policy, "/storage/emulated/0/tencent/QQInput/Ext/Temp/sticker.gif", context, &cache);
    EXPECT_EQ(blocked.decision, fm::MatchDecision::kBlock);
}

TEST(MatcherTest, RedirectHappensOnlyOnce) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n"
        "Android/data/com.tencent.mm/cache/Camera -> DCIM/Camera\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext first;
    first.operation = fm::PathOperation::kOpen;

    auto redirected = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/demo.jpg", first, &cache);
    ASSERT_EQ(redirected.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(redirected.redirect_path, "/storage/emulated/0/Android/data/com.tencent.mm/cache/Camera/demo.jpg");

    fm::RuntimeContext second;
    second.operation = fm::PathOperation::kOpen;
    second.already_redirected = true;
    auto stopped = fm::MatchPath(policy, redirected.redirect_path, second, &cache);
    EXPECT_NE(stopped.decision, fm::MatchDecision::kRedirect);
}

TEST(MatcherTest, SpecificAllowOverridesParentDenyWithSecondBucket) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM\n"
        "+ DCIM/Camera\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error));

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto camera = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/1.jpg", context, &cache);
    EXPECT_EQ(camera.decision, fm::MatchDecision::kAllow);

    auto other = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Other/1.jpg", context, &cache);
    EXPECT_EQ(other.decision, fm::MatchDecision::kBlock);
}

TEST(MatcherTest, DynamicRedirectAllowsButDoesNotRewrite) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/WeiXin => Pictures/WeiXin_Archive\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto result = fm::MatchPath(policy, "/storage/emulated/0/DCIM/WeiXin/a.jpg", context, &cache);
    EXPECT_EQ(result.decision, fm::MatchDecision::kAllow);
    EXPECT_TRUE(result.redirect_path.empty());
}

TEST(MatcherTest, WhitelistBlocksUnmatchedExternalPathOnly) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto allowed = fm::MatchPath(policy, "/storage/emulated/0/Pictures/Share/a.jpg", context, &cache);
    auto blocked = fm::MatchPath(policy, "/storage/emulated/0/Movies/Clip/a.jpg", context, &cache);
    auto outside = fm::MatchPath(policy, "/data/data/com.tencent.mm/files/runtime.log", context, &cache);

    EXPECT_EQ(allowed.decision, fm::MatchDecision::kAllow);
    EXPECT_EQ(blocked.decision, fm::MatchDecision::kBlock);
    EXPECT_EQ(outside.decision, fm::MatchDecision::kAllow);
}

}  // namespace
