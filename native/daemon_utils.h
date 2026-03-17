#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fm {

// access.log 路径，启动时清空，最多保留 max_log_lines 条
inline constexpr char kAccessLogPath[] = "/data/adb/modules/folder_manager/run/access.log";
constexpr int kAccessLogMaxLines = 500;

/**
 * 写入一条结构化审计日志到 access.log。
 * 格式：timestamp pid=N pkg=X op=Y action=Z path=P
 */
void FmLogAccess(const char* pkg, int pid, const char* op, const char* action, const char* path);

/** 启动时清空 access.log */
void FmLogAccessClear();

inline constexpr char kDownloadsContentUri[] = "content://downloads/my_downloads";
inline constexpr char kNonDownloadManagerUri[] = "non-dwnldmngr-download-dont-retry2download";
inline constexpr char kContentCmdPath[] = "/system/bin/content";

struct DownloadsInsertRequest {
    std::string storage_path;
    std::string package_name;
    long long size_bytes = 0;
    long long mtime_ms = 0;
    bool media_scan = false;
    int user_id = -1;
};

std::string SanitizeBindValue(std::string value);
std::string GetBasename(std::string_view path);
std::string GuessMimeType(const std::string& path);
std::vector<std::string> BuildDownloadsInsertArgs(const DownloadsInsertRequest& request);

}  // namespace fm
