#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fm {

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
