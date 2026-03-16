#include <gtest/gtest.h>

#include <sys/stat.h>

#include <string>
#include <unordered_map>
#include <utility>

#include "rule_engine.h"

namespace {

using FileMode = decltype(std::declval<struct stat>().st_mode);

struct FakeFs {
    std::unordered_map<std::string, FileMode> kinds;
};

int FakeLstat(void* context, const char* path, struct stat* st) {
    auto* fs = static_cast<FakeFs*>(context);
    auto it = fs->kinds.find(path);
    if (it == fs->kinds.end()) {
        return -1;
    }
    st->st_mode = it->second;
    return 0;
}

TEST(CompilerTest, InfersKindsFromLstatAndFallbacks) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "- DCIM/A-TEST\n"
        "- Download/private.txt\n"
        "+ Missing/Folder/\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    FakeFs fs{{
        {"/storage/emulated/0/DCIM/A-TEST", S_IFDIR},
        {"/storage/emulated/0/Download/private.txt", S_IFREG},
    }};
    fm::FileSystemProbe probe{&fs, &FakeLstat};
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm:tools", probe, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 4u);
    EXPECT_EQ(policy.rules[0].path_kind, fm::PathKind::kDirectory);
    EXPECT_EQ(policy.rules[1].path_kind, fm::PathKind::kFile);
    EXPECT_EQ(policy.rules[2].path_kind, fm::PathKind::kDirectory);
    EXPECT_EQ(policy.rules[3].path_kind, fm::PathKind::kAuto);
}

TEST(CompilerTest, CompilesDeleteRulesSeparately) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "! DCIM/A-TEST\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    FakeFs fs{{
        {"/storage/emulated/0/DCIM/A-TEST", S_IFDIR},
    }};
    fm::FileSystemProbe probe{&fs, &FakeLstat};
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", probe, &policy, &error)) << error;
    EXPECT_TRUE(policy.rules.empty());
    ASSERT_EQ(policy.delete_rules.size(), 1u);
    EXPECT_EQ(policy.delete_rules[0].path, "/storage/emulated/0/DCIM/A-TEST");
    EXPECT_EQ(policy.delete_rules[0].path_kind, fm::PathKind::kDirectory);
}

TEST(CompilerTest, CompilesDeleteOptions) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "delete_existing = true\n"
        "delete_dirs = empty\n"
        "! Download/Cache\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    EXPECT_TRUE(policy.delete_existing);
    EXPECT_EQ(policy.delete_dir_mode, fm::DeleteDirMode::kEmpty);
}

TEST(CompilerTest, CompilesMediaQueryOption) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "media_query = true\n"
        "+ Pictures/Share\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    EXPECT_EQ(policy.media_query, fm::MediaQueryMode::kEnable);
}

TEST(CompilerTest, NormalizesRedirectTypeFilters) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera @types=JPG,.png,Heic\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 1u);
    ASSERT_EQ(policy.rules[0].extensions.size(), 3u);
    EXPECT_EQ(policy.rules[0].extensions[0], "heic");
    EXPECT_EQ(policy.rules[0].extensions[1], "jpg");
    EXPECT_EQ(policy.rules[0].extensions[2], "png");
}

TEST(CompilerTest, CompilesPathGlobRules) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "- DCIM/*/IMG_*.jpg\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 1u);
    EXPECT_TRUE(policy.rules[0].has_path_glob);
    EXPECT_FALSE(policy.rules[0].has_glob);
    EXPECT_EQ(policy.rules[0].glob_base_prefix, "/storage/emulated/0/DCIM");
    EXPECT_EQ(policy.rules[0].path_glob_pattern, "/storage/emulated/0/DCIM/*/IMG_*.jpg");
}

TEST(CompilerTest, ExpandsPackagePlaceholder) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "DCIM/Camera -> Android/data/<pkg>/cache/Camera\n"
        "+ Android/data/<pkg>/\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    FakeFs fs{};
    fm::FileSystemProbe probe{&fs, &FakeLstat};
    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", probe, &policy, &error)) << error;
    ASSERT_EQ(policy.rules.size(), 2u);
    EXPECT_EQ(policy.rules[0].redirect_target, "/storage/emulated/0/Android/data/com.tencent.mm/cache/Camera");
    EXPECT_EQ(policy.rules[1].path, "/storage/emulated/0/Android/data/com.tencent.mm");
}

TEST(CompilerTest, SkipsDisabledSection) {
    const std::string text =
        "[com.tencent.mm]\n"
        "enabled = false\n"
        "[com.tencent.mobileqq]\n"
        "mode = whitelist\n"
        "+ Pictures/QQ\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    EXPECT_FALSE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error));
    EXPECT_EQ(error, "no matching policy");
}

TEST(CompilerTest, SkipsDisabledAccessibleAndExport) {
    const std::string text =
        "[com.tencent.mm]\n"
        "enabled = false\n"
        "[com.tencent.mm.accessible.demo]\n"
        "to = com.tencent.mobileqq\n"
        "path = tencent\n"
        "[com.tencent.mm.export.demo]\n"
        "source = tencent/QQfile_recv\n"
        "target = Download/QQ\n"
        "[com.tencent.mobileqq]\n"
        "mode = whitelist\n"
        "+ Pictures/QQ\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mobileqq", {}, &policy, &error)) << error;
    EXPECT_TRUE(policy.accessible_rules.empty());
    EXPECT_TRUE(policy.export_rules.empty());
}

TEST(CompilerTest, CompilesTrashOptions) {
    const std::string text =
        "[com.tencent.mm]\n"
        "mode = blacklist\n"
        "trash = true\n"
        "trash_redirect = false\n"
        "trash_dir = Android/data/<pkg>/trash\n"
        "min_age_days = 3\n"
        "min_size_mb = 5\n"
        "! Download/Cache\n";

    fm::ParsedRules parsed;
    ASSERT_TRUE(fm::ParseRulesIni(text, &parsed));

    fm::AppPolicy policy;
    std::string error;
    ASSERT_TRUE(fm::CompilePolicyForProcess(parsed, "com.tencent.mm", {}, &policy, &error)) << error;
    EXPECT_TRUE(policy.trash_enabled);
    EXPECT_FALSE(policy.trash_on_redirect);
    EXPECT_EQ(policy.trash_dir, "/storage/emulated/0/Android/data/com.tencent.mm/trash");
    EXPECT_EQ(policy.min_age_days, 3);
    EXPECT_EQ(policy.min_size_mb, 5);
}

}  // namespace
