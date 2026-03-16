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

TEST(RealWorldTest, WeChatWhitelistWithRedirectAndAccessible) {
    const auto policy = BuildPolicy(
        "[com.tencent.mm]\n"
        "mode = whitelist\n"
        "+ Pictures/Share\n"
        "- DCIM/Secret\n"
        "DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera\n"
        "DCIM/WeiXin => Pictures/WeChat_Archive\n"
        "[com.tencent.mm.accessible.qqinput_temp]\n"
        "from = com.tencent.qqpinyin\n"
        "path = tencent/QQInput/Ext/Temp\n");

    fm::ResolvedPathKindCache cache;
    fm::RuntimeContext context;
    context.operation = fm::PathOperation::kOpen;

    auto share = fm::MatchPath(policy, "/storage/self/primary/Pictures/Share/a.jpg", context, &cache);
    EXPECT_EQ(share.decision, fm::MatchDecision::kAllow);

    auto secret = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Secret/a.jpg", context, &cache);
    EXPECT_EQ(secret.decision, fm::MatchDecision::kBlock);

    auto camera = fm::MatchPath(policy, "/storage/emulated/0/DCIM/Camera/a.jpg", context, &cache);
    EXPECT_EQ(camera.decision, fm::MatchDecision::kRedirect);
    EXPECT_EQ(camera.redirect_path, "/storage/emulated/0/Android/data/com.tencent.mm/cache/Camera/a.jpg");

    auto weixin = fm::MatchPath(policy, "/storage/emulated/0/DCIM/WeiXin/a.jpg", context, &cache);
    EXPECT_EQ(weixin.decision, fm::MatchDecision::kAllow);

    auto temp_ok = fm::MatchPath(policy, "/storage/emulated/0/tencent/QQInput/Ext/Temp/sticker.png", context, &cache);
    EXPECT_EQ(temp_ok.decision, fm::MatchDecision::kAllow);

    auto temp_block = fm::MatchPath(policy, "/storage/emulated/0/tencent/QQInput/Ext/Other/sticker.png", context, &cache);
    EXPECT_EQ(temp_block.decision, fm::MatchDecision::kBlock);

    auto parent = fm::MatchPath(policy, "/storage/emulated/0/Pictures", context, &cache);
    EXPECT_EQ(parent.decision, fm::MatchDecision::kAllow);
}

}  // namespace
