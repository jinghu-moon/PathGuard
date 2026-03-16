#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "daemon_utils.h"

namespace {

bool ContainsValue(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

TEST(DaemonUtilsTest, SanitizeBindValueReplacesColon) {
    EXPECT_EQ(fm::SanitizeBindValue("a:b:c"), "a_b_c");
    EXPECT_EQ(fm::SanitizeBindValue("plain"), "plain");
}

TEST(DaemonUtilsTest, GetBasenameHandlesTrailingSlash) {
    EXPECT_EQ(fm::GetBasename("/storage/emulated/0/Download/test.txt"), "test.txt");
    EXPECT_EQ(fm::GetBasename("/storage/emulated/0/Download/"), "Download");
    EXPECT_EQ(fm::GetBasename("/"), "");
}

TEST(DaemonUtilsTest, GuessMimeTypeHandlesKnownAndUnknown) {
    EXPECT_EQ(fm::GuessMimeType("demo.JPG"), "image/jpeg");
    EXPECT_EQ(fm::GuessMimeType("demo.unknown"), "application/octet-stream");
}

TEST(DaemonUtilsTest, BuildDownloadsInsertArgsIncludesCoreFields) {
    fm::DownloadsInsertRequest request;
    request.storage_path = "/storage/emulated/0/Download/demo.jpg";
    request.package_name = "com.tencent.mm";
    request.size_bytes = 123;
    request.mtime_ms = 456;
    request.media_scan = true;
    request.user_id = 10;

    const auto args = fm::BuildDownloadsInsertArgs(request);
    ASSERT_GE(args.size(), 4u);
    EXPECT_EQ(args[0], fm::kContentCmdPath);
    EXPECT_EQ(args[1], "insert");
    EXPECT_EQ(args[2], "--uri");
    EXPECT_EQ(args[3], fm::kDownloadsContentUri);
    EXPECT_TRUE(ContainsValue(args, "--user"));
    EXPECT_TRUE(ContainsValue(args, "10"));
    EXPECT_TRUE(ContainsValue(args, "title:s:demo.jpg"));
    EXPECT_TRUE(ContainsValue(args, "description:s:FolderManager 导出 com.tencent.mm"));
    EXPECT_TRUE(ContainsValue(args, "mimetype:s:image/jpeg"));
    EXPECT_TRUE(ContainsValue(args, "_data:s:/storage/emulated/0/Download/demo.jpg"));
    EXPECT_TRUE(ContainsValue(args, "total_bytes:l:123"));
    EXPECT_TRUE(ContainsValue(args, "current_bytes:l:123"));
    EXPECT_TRUE(ContainsValue(args, "media_scanned:i:0"));
    EXPECT_TRUE(ContainsValue(args, "lastmod:l:456"));
}

TEST(DaemonUtilsTest, BuildDownloadsInsertArgsSkipsUserWhenNegative) {
    fm::DownloadsInsertRequest request;
    request.storage_path = "/storage/emulated/0/Download/demo.zip";
    request.package_name = "";
    request.size_bytes = 0;
    request.mtime_ms = 0;
    request.media_scan = false;
    request.user_id = -1;

    const auto args = fm::BuildDownloadsInsertArgs(request);
    EXPECT_FALSE(ContainsValue(args, "--user"));
    EXPECT_TRUE(ContainsValue(args, "description:s:FolderManager 导出"));
    EXPECT_TRUE(ContainsValue(args, "media_scanned:i:2"));
}

}  // namespace
