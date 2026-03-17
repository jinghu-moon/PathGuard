#include "daemon_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace fm {
namespace {

constexpr int kDownloadDestinationNonManager = 6;
constexpr int kDownloadStatusSuccess = 200;
constexpr int kDownloadVisibilityHidden = 2;
constexpr int kDownloadScannableYes = 0;
constexpr int kDownloadScannableNo = 2;
constexpr int kDownloadIsPublicApi = 1;
constexpr int kDownloadIsVisibleInUi = 1;
constexpr int kDownloadAllowWrite = 0;
constexpr int kDownloadAllowMetered = 1;
constexpr int kDownloadAllowRoaming = 1;
constexpr int kDownloadAllowedNetworkAll = -1;

}  // namespace

std::string SanitizeBindValue(std::string value) {
    std::replace(value.begin(), value.end(), ':', '_');
    return value;
}

std::string GetBasename(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return {};
    }
    size_t slash = path.rfind('/', end - 1);
    if (slash == std::string_view::npos) {
        return std::string(path.substr(0, end));
    }
    return std::string(path.substr(slash + 1, end - slash - 1));
}

std::string GuessMimeType(const std::string& path) {
    static const std::pair<std::string_view, std::string_view> kMimeMap[] = {
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"png", "image/png"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"heic", "image/heic"},
        {"mp4", "video/mp4"},
        {"mkv", "video/x-matroska"},
        {"3gp", "video/3gpp"},
        {"mp3", "audio/mpeg"},
        {"m4a", "audio/mp4"},
        {"wav", "audio/wav"},
        {"ogg", "audio/ogg"},
        {"pdf", "application/pdf"},
        {"txt", "text/plain"},
        {"json", "application/json"},
        {"zip", "application/zip"},
        {"apk", "application/vnd.android.package-archive"},
    };
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "application/octet-stream";
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto& pair : kMimeMap) {
        if (ext == pair.first) {
            return std::string(pair.second);
        }
    }
    return "application/octet-stream";
}

std::vector<std::string> BuildDownloadsInsertArgs(const DownloadsInsertRequest& request) {
    std::vector<std::string> args;
    args.emplace_back(kContentCmdPath);
    args.emplace_back("insert");
    args.emplace_back("--uri");
    args.emplace_back(kDownloadsContentUri);
    if (request.user_id >= 0) {
        args.emplace_back("--user");
        args.emplace_back(std::to_string(request.user_id));
    }

    auto add_bind = [](std::vector<std::string>* target,
                       const std::string& key,
                       const std::string& type,
                       const std::string& value) {
        target->push_back("--bind");
        target->push_back(key + ":" + type + ":" + SanitizeBindValue(value));
    };

    std::string title = GetBasename(request.storage_path);
    if (title.empty()) {
        title = "download";
    }
    std::string description = request.package_name.empty()
        ? "FolderManager 导出"
        : ("FolderManager 导出 " + request.package_name);
    std::string mime_type = GuessMimeType(request.storage_path);
    long long size = request.size_bytes < 0 ? 0 : request.size_bytes;
    long long mtime_ms = request.mtime_ms < 0 ? 0 : request.mtime_ms;

    add_bind(&args, "uri", "s", kNonDownloadManagerUri);
    add_bind(&args, "is_public_api", "i", std::to_string(kDownloadIsPublicApi));
    add_bind(&args, "title", "s", title);
    add_bind(&args, "description", "s", description);
    add_bind(&args, "mimetype", "s", mime_type);
    add_bind(&args, "destination", "i", std::to_string(kDownloadDestinationNonManager));
    add_bind(&args, "_data", "s", request.storage_path);
    add_bind(&args, "status", "i", std::to_string(kDownloadStatusSuccess));
    add_bind(&args, "total_bytes", "l", std::to_string(size));
    add_bind(&args, "current_bytes", "l", std::to_string(size));
    add_bind(&args, "media_scanned", "i", std::to_string(request.media_scan ? kDownloadScannableYes : kDownloadScannableNo));
    add_bind(&args, "visibility", "i", std::to_string(kDownloadVisibilityHidden));
    add_bind(&args, "is_visible_in_downloads_ui", "i", std::to_string(kDownloadIsVisibleInUi));
    add_bind(&args, "allow_write", "i", std::to_string(kDownloadAllowWrite));
    add_bind(&args, "allowed_network_types", "i", std::to_string(kDownloadAllowedNetworkAll));
    add_bind(&args, "allow_roaming", "i", std::to_string(kDownloadAllowRoaming));
    add_bind(&args, "allow_metered", "i", std::to_string(kDownloadAllowMetered));
    add_bind(&args, "lastmod", "l", std::to_string(mtime_ms));

    return args;
}

// ---------------------------------------------------------------------------
// access.log 审计日志实现
// ---------------------------------------------------------------------------

namespace {

std::mutex g_access_log_mutex;

// 将 access.log 截断至最多 max_lines 行（环形保留尾部）
void TruncateAccessLog(const char* path, int max_lines) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    std::deque<std::string> lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        lines.emplace_back(buf);
        if (static_cast<int>(lines.size()) > max_lines * 2) {
            lines.pop_front();
        }
    }
    fclose(f);
    if (static_cast<int>(lines.size()) <= max_lines) return;
    while (static_cast<int>(lines.size()) > max_lines) lines.pop_front();
    FILE* w = fopen(path, "w");
    if (!w) return;
    for (const auto& l : lines) fputs(l.c_str(), w);
    fclose(w);
}

}  // namespace

void FmLogAccess(const char* pkg, int pid, const char* op, const char* action, const char* path) {
    std::lock_guard<std::mutex> lock(g_access_log_mutex);
    time_t now = time(nullptr);
    struct tm tm_buf = {};
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    FILE* f = fopen(kAccessLogPath, "a");
    if (!f) return;
    fprintf(f, "%s pid=%d pkg=%s op=%s action=%s path=%s\n",
            ts, pid, pkg ? pkg : "", op ? op : "", action ? action : "", path ? path : "");
    // 获取当前行数，超过阈值才截断（每 50 条检查一次，减少 I/O）
    long pos = ftell(f);
    fclose(f);
    // 粗略估算：平均行长 ~120 字节，超过 max*120*1.2 才触发截断
    if (pos > static_cast<long>(kAccessLogMaxLines) * 144) {
        TruncateAccessLog(kAccessLogPath, kAccessLogMaxLines);
    }
}

void FmLogAccessClear() {
    std::lock_guard<std::mutex> lock(g_access_log_mutex);
    FILE* f = fopen(kAccessLogPath, "w");
    if (f) fclose(f);
}

}  // namespace fm
