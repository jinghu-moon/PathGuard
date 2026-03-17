#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#if !defined(__ANDROID__)
#include <sys/fanotify.h>
#endif
#if defined(__ANDROID__)
#include <android/api-level.h>
#endif
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/capability.h>
#include <linux/fanotify.h>
#include <linux/fs.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "daemon_utils.h"
#include "rule_engine.h"

#ifndef FAN_REPORT_DFID_NAME
#define FAN_REPORT_DFID_NAME 0x4000
#endif

#ifndef FAN_EVENT_INFO_FIRST
#define FAN_EVENT_INFO_FIRST(meta) reinterpret_cast<const struct fanotify_event_info_header*>( \
    reinterpret_cast<const char*>(meta) + sizeof(struct fanotify_event_metadata))
#endif

#ifndef FAN_EVENT_INFO_NEXT
#define FAN_EVENT_INFO_NEXT(info, end) reinterpret_cast<const struct fanotify_event_info_header*>( \
    reinterpret_cast<const char*>(info) + (info)->len < reinterpret_cast<const char*>(end) \
        ? reinterpret_cast<const char*>(info) + (info)->len \
        : reinterpret_cast<const char*>(end))
#endif

#if defined(__ANDROID__)
static int fm_fanotify_init(unsigned int flags, unsigned int event_f_flags) {
    return static_cast<int>(syscall(__NR_fanotify_init, flags, event_f_flags));
}

static int fm_fanotify_mark(int fan_fd, unsigned int flags, uint64_t mask, int dirfd, const char *pathname) {
    return static_cast<int>(syscall(__NR_fanotify_mark, fan_fd, flags, mask, dirfd, pathname));
}
#else
static int fm_fanotify_init(unsigned int flags, unsigned int event_f_flags) {
    return fanotify_init(flags, event_f_flags);
}

static int fm_fanotify_mark(int fan_fd, unsigned int flags, uint64_t mask, int dirfd, const char *pathname) {
    return fanotify_mark(fan_fd, flags, mask, dirfd, pathname);
}
#endif

#ifndef FAN_EVENT_INFO_TYPE_DFID_NAME
#define FAN_EVENT_INFO_TYPE_DFID_NAME 3
#endif

#ifndef FAN_EVENT_INFO_TYPE_PIDFD
#define FAN_EVENT_INFO_TYPE_PIDFD 6
#endif

#if defined(__ANDROID__)
struct file_handle {
    unsigned int handle_bytes;
    int handle_type;
    unsigned char f_handle[0];
};
#endif

#ifndef FAN_NOPIDFD
#define FAN_NOPIDFD 0x40000000
#endif

#ifndef FAN_EPIDFD
#define FAN_EPIDFD 0x20000000
#endif

namespace {

constexpr char kLogTag[] = "FolderManager";
constexpr char kDefaultConfigPath[] = "/data/adb/modules/folder_manager/config/rules.ini";
constexpr char kDataMediaRoot[] = "/data/media/";
constexpr char kStorageEmulatedRoot[] = "/storage/emulated/";
constexpr size_t kEventBufferSize = 16 * 1024;
constexpr size_t kQueueLimit = 256;
constexpr int kRetryDelayMs = 500;
constexpr int kInitialDelayMs = 50;
constexpr int kMaxRetryCount = 3;
constexpr int kCapSysAdmin = 21;
constexpr int kCapDacReadSearch = 2;
constexpr int kPrimaryUserId = 0;
constexpr char kDefaultTrashDir[] = "/storage/emulated/0/.FolderManagerTrash";
constexpr char kDefaultTrashDirName[] = ".FolderManagerTrash";
constexpr char kQueueLogPath[] = "/data/adb/modules/folder_manager/run/task.log";

struct DynamicRule {
    std::string package_name;
    std::string source_path;
    std::string target_path;
    fm::PathKind path_kind = fm::PathKind::kDirectory;
    std::vector<std::string> extensions;
    bool has_glob = false;
    bool has_path_glob = false;
    std::string glob_parent;
    std::string glob_pattern;
    std::string glob_base_prefix;
    std::string path_glob_pattern;
    bool trash_on_redirect = false;
    std::string trash_dir;
};

struct DeleteRule {
    std::string package_name;
    std::string source_path;
    fm::PathKind path_kind = fm::PathKind::kDirectory;
    bool delete_existing = false;
    fm::DeleteDirMode delete_dir_mode = fm::DeleteDirMode::kNone;
    bool has_glob = false;
    bool has_path_glob = false;
    std::string glob_parent;
    std::string glob_pattern;
    std::string glob_base_prefix;
    std::string path_glob_pattern;
    bool trash_enabled = false;
    std::string trash_dir;
    int min_age_days = 0;
    long long min_size_bytes = 0;
};

struct ExportRule {
    std::string package_name;
    std::string source_path;
    std::string target_path;
    fm::PathKind path_kind = fm::PathKind::kDirectory;
    bool allow_child = false;
    bool media_scan = false;
    bool add_to_downloads = false;
};

struct DynamicRuleSet {
    std::unordered_map<std::string, std::vector<DynamicRule>> rules_by_package;
    std::unordered_map<std::string, std::vector<ExportRule>> export_rules_by_package;
    std::unordered_map<std::string, std::vector<DeleteRule>> delete_rules_by_package;
    // [*] 通配包名规则，匹配任意来源
    std::vector<DynamicRule> wildcard_rules;
    std::vector<DeleteRule> wildcard_delete_rules;
};

enum class TaskAction {
    kMove = 0,
    kDelete = 1,
};

struct MoveTask {
    TaskAction action = TaskAction::kMove;
    std::string source_path;
    std::string target_path;
    std::string package_name;
    pid_t writer_pid = -1;
    int retry_count = 0;
    std::string delete_root_path;
    fm::DeleteDirMode delete_dir_mode = fm::DeleteDirMode::kNone;
    bool use_trash = false;
    std::string trash_dir;
};

class TaskQueue {
public:
    bool Push(MoveTask task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kQueueLimit) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(task));
        condition_.notify_one();
        return true;
    }

    bool Pop(MoveTask* task) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() { return !queue_.empty() || !running_; });
        if (!running_ || queue_.empty()) {
            return false;
        }
        *task = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<MoveTask> queue_;
    bool running_ = true;
};

constexpr char kModuleConfPath[] = "/data/adb/modules/folder_manager/config/module.conf";

// 读取 module.conf，返回 enabled 字段（默认 true）
bool ReadModuleEnabled(const char* conf_path) {
    FILE* f = fopen(conf_path, "r");
    if (!f) return true; // 文件不存在视为 enabled
    char line[256];
    bool enabled = true;
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {}, val[64] = {};
        if (sscanf(line, " %63[^= ] = %63s", key, val) == 2) {
            if (strcmp(key, "enabled") == 0 && strcmp(val, "false") == 0) {
                enabled = false;
            }
        }
    }
    fclose(f);
    return enabled;
}

std::atomic<bool> g_running{true};
std::atomic<bool> g_reload_requested{false};
std::shared_mutex g_rules_mutex;
DynamicRuleSet g_dynamic_rules;
TaskQueue g_task_queue;
int g_media_fd = -1;

void LogPrint(int priority, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(priority, kLogTag, fmt, args);
    va_end(args);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

bool HasCapability(int cap) {
    __user_cap_header_struct header = {};
    __user_cap_data_struct data[2] = {};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
    if (syscall(__NR_capget, &header, &data) != 0) {
        return false;
    }
    if (cap < 0) {
        return false;
    }
    size_t index = static_cast<size_t>(cap / 32);
    size_t bit = static_cast<size_t>(cap % 32);
    size_t data_count = sizeof(data) / sizeof(data[0]);
    if (index >= data_count) {
        return false;
    }
    return (data[index].effective & (1u << bit)) != 0;
}

int RunCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
        return -1;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execv(args[0].c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

void GetRuleCounts(const DynamicRuleSet& rules, size_t* dynamic_count, size_t* export_count, size_t* delete_count) {
    size_t dynamic_total = 0;
    size_t export_total = 0;
    size_t delete_total = 0;
    for (const auto& entry : rules.rules_by_package) {
        dynamic_total += entry.second.size();
    }
    for (const auto& entry : rules.export_rules_by_package) {
        export_total += entry.second.size();
    }
    for (const auto& entry : rules.delete_rules_by_package) {
        delete_total += entry.second.size();
    }
    if (dynamic_count != nullptr) {
        *dynamic_count = dynamic_total;
    }
    if (export_count != nullptr) {
        *export_count = export_total;
    }
    if (delete_count != nullptr) {
        *delete_count = delete_total;
    }
}

int LstatCallback(void* /*context*/, const char* path, struct stat* st) {
    return lstat(path, st);
}

bool ReadFileContent(const char* path, std::string* output) {
    if (path == nullptr || output == nullptr) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    output->clear();
    char buffer[4096];
    for (;;) {
        ssize_t read_bytes = read(fd, buffer, sizeof(buffer));
        if (read_bytes < 0) {
            close(fd);
            return false;
        }
        if (read_bytes == 0) {
            break;
        }
        output->append(buffer, static_cast<size_t>(read_bytes));
    }
    close(fd);
    return true;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsDirectoryMatch(std::string_view input_path, std::string_view rule_path) {
    if (input_path == rule_path) {
        return true;
    }
    return input_path.size() > rule_path.size()
        && StartsWith(input_path, rule_path)
        && input_path[rule_path.size()] == '/';
}

void ToLowerInPlace(std::string* value) {
    if (value == nullptr) {
        return;
    }
    for (char& ch : *value) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
}

bool MatchesTypeFilter(const std::vector<std::string>& extensions, std::string_view path) {
    if (extensions.empty()) {
        return true;
    }
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size()) {
        return false;
    }
    if (slash != std::string_view::npos && dot <= slash) {
        return false;
    }
    std::string ext(path.substr(dot + 1));
    ToLowerInPlace(&ext);
    return std::binary_search(extensions.begin(), extensions.end(), ext);
}

bool MatchGlob(std::string_view pattern, std::string_view text) {
    size_t p = 0;
    size_t t = 0;
    size_t star = std::string_view::npos;
    size_t match = 0;
    while (t < text.size()) {
        if (p < pattern.size()
            && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
            continue;
        }
        if (star != std::string_view::npos) {
            p = star + 1;
            t = ++match;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

void SplitPathSegments(std::string_view path, std::vector<std::string_view>* segments) {
    if (segments == nullptr) {
        return;
    }
    segments->clear();
    size_t start = 0;
    while (true) {
        size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            segments->push_back(path.substr(start));
            break;
        }
        segments->push_back(path.substr(start, slash - start));
        start = slash + 1;
        if (start > path.size()) {
            break;
        }
    }
}

bool MatchPathGlob(std::string_view pattern, std::string_view text) {
    static thread_local std::vector<std::string_view> pattern_segments;
    static thread_local std::vector<std::string_view> text_segments;
    SplitPathSegments(pattern, &pattern_segments);
    SplitPathSegments(text, &text_segments);

    const size_t p_len = pattern_segments.size();
    const size_t t_len = text_segments.size();

    const size_t width = t_len + 1;
    static thread_local std::vector<int8_t> memo;
    memo.assign((p_len + 1) * width, -1);

    std::function<bool(size_t, size_t)> dfs = [&](size_t p, size_t t) -> bool {
        size_t idx = p * width + t;
        int8_t cached = memo[idx];
        if (cached >= 0) {
            return cached != 0;
        }
        if (p == p_len) {
            memo[idx] = (t == t_len) ? 1 : 0;
            return memo[idx] != 0;
        }
        std::string_view segment = pattern_segments[p];
        if (segment == "**") {
            size_t next = p;
            while (next < p_len && pattern_segments[next] == "**") {
                ++next;
            }
            for (size_t k = t; k <= t_len; ++k) {
                if (dfs(next, k)) {
                    memo[idx] = 1;
                    return true;
                }
            }
            memo[idx] = 0;
            return false;
        }
        if (t >= t_len) {
            memo[idx] = 0;
            return false;
        }
        if (!MatchGlob(segment, text_segments[t])) {
            memo[idx] = 0;
            return false;
        }
        bool ok = dfs(p + 1, t + 1);
        memo[idx] = ok ? 1 : 0;
        return ok;
    };

    return dfs(0, 0);
}

bool ParseDataMediaPath(const std::string& path, int* user_id, std::string* rel_path) {
    if (user_id == nullptr || rel_path == nullptr) {
        return false;
    }
    if (!StartsWith(path, kDataMediaRoot)) {
        return false;
    }
    std::string_view rest(path.c_str() + strlen(kDataMediaRoot));
    size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    std::string user_str(rest.substr(0, slash));
    char* end = nullptr;
    long parsed = strtol(user_str.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0) {
        return false;
    }
    *user_id = static_cast<int>(parsed);
    rel_path->assign(rest.substr(slash + 1));
    return true;
}

bool BuildStoragePath(int user_id, std::string_view rel_path, std::string* output) {
    if (output == nullptr) {
        return false;
    }
    *output = std::string(kStorageEmulatedRoot) + std::to_string(user_id) + "/" + std::string(rel_path);
    return true;
}

bool BuildCanonicalStoragePath(int user_id, std::string_view rel_path, std::string* output) {
    if (output == nullptr) {
        return false;
    }
    *output = std::string(kStorageEmulatedRoot) + "0/" + std::string(rel_path);
    return true;
}

bool StoragePathToDataMedia(std::string_view storage_path, int user_id, std::string* output) {
    if (output == nullptr) {
        return false;
    }
    if (!StartsWith(storage_path, kStorageEmulatedRoot)) {
        *output = std::string(storage_path);
        return true;
    }
    std::string_view rest = storage_path.substr(strlen(kStorageEmulatedRoot));
    size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    std::string_view rel = rest.substr(slash + 1);
    *output = std::string(kDataMediaRoot) + std::to_string(user_id) + "/" + std::string(rel);
    return true;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    size_t offset = 0;
    while (offset < path.size()) {
        size_t next = path.find('/', offset);
        if (next == std::string::npos) {
            next = path.size();
        }
        std::string segment = path.substr(0, next);
        if (!segment.empty() && segment != "/") {
            if (mkdir(segment.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        offset = next + 1;
    }
    return true;
}

bool EnsureParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return false;
    }
    return EnsureDirectory(path.substr(0, slash));
}

bool CopyFileChunked(const std::string& source, const std::string& target, const struct stat& st) {
    int src_fd = open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        return false;
    }
    int dst_fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
    if (dst_fd < 0) {
        close(src_fd);
        return false;
    }
    off_t offset = 0;
    off_t remaining = st.st_size;
    while (remaining > 0) {
        ssize_t sent = sendfile(dst_fd, src_fd, &offset, static_cast<size_t>(remaining));
        if (sent <= 0) {
            close(src_fd);
            close(dst_fd);
            return false;
        }
        remaining -= sent;
    }
    fsync(dst_fd);
    close(src_fd);
    close(dst_fd);
    return true;
}

bool MoveFile(const std::string& source, const std::string& target) {
    if (source == target) {
        return true;
    }
    if (!EnsureParentDir(target)) {
        return false;
    }
    if (rename(source.c_str(), target.c_str()) == 0) {
        return true;
    }
    if (errno != EXDEV) {
        return false;
    }
    struct stat st = {};
    if (lstat(source.c_str(), &st) != 0) {
        return false;
    }
    if (!CopyFileChunked(source, target, st)) {
        unlink(target.c_str());
        return false;
    }
    unlink(source.c_str());
    return true;
}

bool CopyFile(const std::string& source, const std::string& target) {
    struct stat st = {};
    if (lstat(source.c_str(), &st) != 0) {
        return false;
    }
    if (!EnsureParentDir(target)) {
        return false;
    }
    if (!CopyFileChunked(source, target, st)) {
        unlink(target.c_str());
        return false;
    }
    return true;
}

bool IsSafeDeletePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (!StartsWith(path, kDataMediaRoot)) {
        return false;
    }
    return path.size() > strlen(kDataMediaRoot);
}

bool DeleteFile(const std::string& path) {
    if (!IsSafeDeletePath(path)) {
        return false;
    }
    if (unlink(path.c_str()) == 0) {
        return true;
    }
    return errno == ENOENT;
}

bool GetParentPath(const std::string& path, std::string* parent) {
    if (parent == nullptr) {
        return false;
    }
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return false;
    }
    parent->assign(path.substr(0, slash));
    return true;
}

bool DeleteTreeInternal(const std::string& path, fm::DeleteDirMode mode) {
    if (!IsSafeDeletePath(path)) {
        return false;
    }
    struct stat st = {};
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(st.st_mode)) {
        return DeleteFile(path);
    }

    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        std::string child = path;
        child.push_back('/');
        child.append(entry->d_name);
        DeleteTreeInternal(child, mode);
    }
    closedir(dir);

    if (mode == fm::DeleteDirMode::kNone) {
        return true;
    }
    if (rmdir(path.c_str()) == 0) {
        return true;
    }
    if (mode == fm::DeleteDirMode::kEmpty && errno == ENOTEMPTY) {
        return true;
    }
    return false;
}

bool DeleteTree(const std::string& path, fm::DeleteDirMode mode) {
    if (mode == fm::DeleteDirMode::kNone) {
        return DeleteTreeInternal(path, mode);
    }
    return DeleteTreeInternal(path, mode);
}

bool DeleteMatchingFilesInDir(const std::string& dir_path,
                              const std::string& pattern,
                              size_t* deleted,
                              size_t* failed) {
    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        return false;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        if (!MatchGlob(pattern, entry->d_name)) {
            continue;
        }
        std::string child = dir_path;
        child.push_back('/');
        child.append(entry->d_name);
        struct stat st = {};
        if (lstat(child.c_str(), &st) != 0) {
            if (failed != nullptr) {
                (*failed)++;
            }
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            continue;
        }
        if (DeleteFile(child)) {
            if (deleted != nullptr) {
                (*deleted)++;
            }
        } else if (failed != nullptr) {
            (*failed)++;
        }
    }
    closedir(dir);
    return true;
}

bool DeleteMatchingFilesByPathGlobInternal(const std::string& dir_path,
                                           std::string_view pattern,
                                           fm::DeleteDirMode mode,
                                           size_t* deleted,
                                           size_t* failed) {
    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        return false;
    }
    bool ok = true;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        std::string child = dir_path;
        child.push_back('/');
        child.append(entry->d_name);
        struct stat st = {};
        if (lstat(child.c_str(), &st) != 0) {
            if (failed != nullptr) {
                (*failed)++;
            }
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!DeleteMatchingFilesByPathGlobInternal(child, pattern, mode, deleted, failed)) {
                ok = false;
            }
            continue;
        }
        if (!MatchPathGlob(pattern, child)) {
            continue;
        }
        if (DeleteFile(child)) {
            if (deleted != nullptr) {
                (*deleted)++;
            }
        } else {
            if (failed != nullptr) {
                (*failed)++;
            }
            ok = false;
        }
    }
    closedir(dir);

    if (mode != fm::DeleteDirMode::kNone && IsSafeDeletePath(dir_path)) {
        if (rmdir(dir_path.c_str()) != 0 && errno != ENOTEMPTY) {
            ok = false;
        }
    }
    return ok;
}

bool DeleteMatchingFilesByPathGlob(const std::string& base_dir,
                                   std::string_view pattern,
                                   fm::DeleteDirMode mode,
                                   size_t* deleted,
                                   size_t* failed) {
    if (!IsSafeDeletePath(base_dir)) {
        return false;
    }
    return DeleteMatchingFilesByPathGlobInternal(base_dir, pattern, mode, deleted, failed);
}

void CleanupEmptyParents(std::string path, const std::string& stop_at) {
    if (path.empty() || stop_at.empty()) {
        return;
    }
    while (true) {
        if (!IsDirectoryMatch(path, stop_at)) {
            if (path != stop_at) {
                break;
            }
        }
        if (!IsSafeDeletePath(path)) {
            break;
        }
        if (rmdir(path.c_str()) != 0) {
            break;
        }
        if (path == stop_at) {
            break;
        }
        size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) {
            break;
        }
        path.resize(slash);
    }
}

void ApplyDeleteExistingRules(const DynamicRuleSet& rules, int user_id) {
    size_t applied = 0;
    size_t failed = 0;
    for (const auto& entry : rules.delete_rules_by_package) {
        for (const auto& rule : entry.second) {
            if (!rule.delete_existing) {
                continue;
            }
            bool ok = false;
            if (rule.has_path_glob) {
                std::string data_root;
                std::string data_pattern;
                if (!StoragePathToDataMedia(rule.glob_base_prefix, user_id, &data_root)
                    || !StoragePathToDataMedia(rule.path_glob_pattern, user_id, &data_pattern)) {
                    failed++;
                    continue;
                }
                size_t deleted = 0;
                size_t failed_local = 0;
                ok = DeleteMatchingFilesByPathGlob(data_root, data_pattern, rule.delete_dir_mode, &deleted, &failed_local);
                applied += deleted;
                failed += failed_local;
                if (ok) {
                    continue;
                }
            } else if (rule.has_glob) {
                std::string data_dir;
                if (!StoragePathToDataMedia(rule.glob_parent, user_id, &data_dir)) {
                    failed++;
                    continue;
                }
                size_t deleted = 0;
                size_t failed_local = 0;
                ok = DeleteMatchingFilesInDir(data_dir, rule.glob_pattern, &deleted, &failed_local);
                applied += deleted;
                failed += failed_local;
                if (rule.delete_dir_mode == fm::DeleteDirMode::kEmpty) {
                    CleanupEmptyParents(data_dir, data_dir);
                } else if (rule.delete_dir_mode == fm::DeleteDirMode::kRecursive) {
                    DeleteTree(data_dir, rule.delete_dir_mode);
                }
                if (ok) {
                    continue;
                }
            } else {
                std::string data_path;
                if (!StoragePathToDataMedia(rule.source_path, user_id, &data_path)) {
                    failed++;
                    continue;
                }
                if (rule.path_kind == fm::PathKind::kFile) {
                    ok = DeleteFile(data_path);
                } else {
                    ok = DeleteTree(data_path, rule.delete_dir_mode);
                }
            }
            if (ok) {
                applied++;
            } else {
                failed++;
            }
        }
    }
    if (applied > 0 || failed > 0) {
        LogPrint(ANDROID_LOG_INFO,
                 "delete_existing applied: ok=%zu failed=%zu",
                 applied,
                 failed);
    }
}

bool IsFileOpenByPid(const std::string& path, pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", pid);
    DIR* dir = opendir(fd_dir);
    if (dir == nullptr) {
        return false;
    }
    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char link_path[128];
        snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir, entry->d_name);
        struct stat lst = {};
        if (stat(link_path, &lst) == 0 && lst.st_ino == st.st_ino) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

bool ResolvePidFromPidfd(int pidfd, pid_t* pid) {
    if (pid == nullptr || pidfd < 0) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", pidfd);
    FILE* fp = fopen(path, "r");
    if (fp == nullptr) {
        return false;
    }
    char line[128];
    pid_t result = -1;
    while (fgets(line, sizeof(line), fp) != nullptr) {
        if (sscanf(line, "Pid:\t%d", &result) == 1) {
            break;
        }
    }
    fclose(fp);
    if (result <= 0) {
        return false;
    }
    *pid = result;
    return true;
}

bool ReadPackageFromPid(pid_t pid, std::string* package_name) {
    if (package_name == nullptr || pid <= 0) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    std::string cmdline;
    if (!ReadFileContent(path, &cmdline) || cmdline.empty()) {
        return false;
    }
    size_t end = cmdline.find('\0');
    if (end != std::string::npos) {
        cmdline.resize(end);
    }
    size_t colon = cmdline.find(':');
    if (colon != std::string::npos) {
        cmdline.resize(colon);
    }
    if (cmdline.empty()) {
        return false;
    }
    *package_name = cmdline;
    return true;
}

bool RebuildPathFromFid(const struct fanotify_event_info_fid* fid_info, std::string* output) {
    if (fid_info == nullptr || output == nullptr || g_media_fd < 0) {
        return false;
    }
    const struct file_handle* handle = reinterpret_cast<const struct file_handle*>(fid_info->handle);
    if (handle == nullptr) {
        return false;
    }
    int dir_fd = syscall(__NR_open_by_handle_at, g_media_fd, handle, O_PATH | O_CLOEXEC);
    if (dir_fd < 0) {
        if (errno == ESTALE) {
            return false;
        }
        return false;
    }
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", dir_fd);
    char dir_path[PATH_MAX] = {0};
    ssize_t len = readlink(fd_path, dir_path, sizeof(dir_path) - 1);
    close(dir_fd);
    if (len <= 0) {
        return false;
    }
    dir_path[len] = '\0';
    const char* name = reinterpret_cast<const char*>(handle->f_handle + handle->handle_bytes);
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    *output = std::string(dir_path) + "/" + std::string(name);
    return true;
}

void TriggerMediaScan(const std::string& target_path) {
    if (access("/system/bin/am", X_OK) != 0) {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        std::string uri = std::string("file://") + target_path;
        execl("/system/bin/am", "am", "broadcast",
              "-a", "android.intent.action.MEDIA_SCANNER_SCAN_FILE",
              "-d", uri.c_str(), static_cast<char*>(nullptr));
        _exit(0);
    }
}

bool AddToDownloads(const std::string& data_media_path,
                    int user_id,
                    const std::string& package_name,
                    bool media_scan) {
    if (access(fm::kContentCmdPath, X_OK) != 0) {
        LogPrint(ANDROID_LOG_WARN, "content cmd missing: %s", fm::kContentCmdPath);
        return false;
    }
    struct stat st = {};
    if (lstat(data_media_path.c_str(), &st) != 0) {
        LogPrint(ANDROID_LOG_WARN, "add_to_downloads lstat failed: %s errno=%d", data_media_path.c_str(), errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        LogPrint(ANDROID_LOG_WARN, "add_to_downloads skip non-regular file: %s", data_media_path.c_str());
        return false;
    }
    std::string rel_path;
    int parsed_user = user_id;
    std::string storage_path = data_media_path;
    if (ParseDataMediaPath(data_media_path, &parsed_user, &rel_path)) {
        std::string candidate;
        if (BuildStoragePath(parsed_user, rel_path, &candidate)) {
            storage_path = std::move(candidate);
            user_id = parsed_user;
        }
    }

    long long size = static_cast<long long>(st.st_size);
    long long mtime_ms = static_cast<long long>(st.st_mtime) * 1000;

    fm::DownloadsInsertRequest request;
    request.storage_path = storage_path;
    request.package_name = package_name;
    request.size_bytes = size;
    request.mtime_ms = mtime_ms;
    request.media_scan = media_scan;
    request.user_id = user_id;

    std::vector<std::string> args = fm::BuildDownloadsInsertArgs(request);

    int result = RunCommand(args);
    if (result != 0) {
        LogPrint(ANDROID_LOG_WARN, "add_to_downloads failed: exit=%d path=%s", result, storage_path.c_str());
        return false;
    }
    LogPrint(ANDROID_LOG_INFO, "add_to_downloads ok: %s", storage_path.c_str());
    return true;
}

bool BuildDynamicRuleSet(const char* config_path, DynamicRuleSet* output, std::string* error) {
    if (output == nullptr) {
        return false;
    }
    output->rules_by_package.clear();
    output->delete_rules_by_package.clear();
    output->wildcard_rules.clear();
    output->wildcard_delete_rules.clear();
    std::string content;
    if (!ReadFileContent(config_path, &content)) {
        if (error != nullptr) {
            *error = "failed to read rules.ini";
        }
        return false;
    }
    fm::ParsedRules parsed;
    if (!fm::ParseRulesIni(content, &parsed)) {
        if (error != nullptr && !parsed.errors.empty()) {
            *error = parsed.errors.front();
        }
        return false;
    }

    fm::FileSystemProbe probe{nullptr, &LstatCallback};
    for (const auto& section : parsed.sections) {
        fm::AppPolicy policy;
        std::string compile_error;
        if (!fm::CompilePolicyForProcess(parsed, section.package_name, probe, &policy, &compile_error)) {
            if (error != nullptr) {
                *error = compile_error.empty() ? "failed to compile rules" : compile_error;
            }
            return false;
        }
        for (const auto& rule : policy.rules) {
            if (rule.action != fm::RuleAction::kRedirectDynamic) {
                continue;
            }
            DynamicRule dynamic_rule;
            dynamic_rule.package_name = policy.package_name;
            dynamic_rule.source_path = rule.path;
            dynamic_rule.target_path = rule.redirect_target;
            dynamic_rule.path_kind = rule.path_kind == fm::PathKind::kAuto ? fm::PathKind::kDirectory : rule.path_kind;
            dynamic_rule.extensions = rule.extensions;
            dynamic_rule.has_glob = rule.has_glob;
            dynamic_rule.has_path_glob = rule.has_path_glob;
            dynamic_rule.glob_parent = rule.glob_parent;
            dynamic_rule.glob_pattern = rule.glob_pattern;
            dynamic_rule.glob_base_prefix = rule.glob_base_prefix;
            dynamic_rule.path_glob_pattern = rule.path_glob_pattern;
            output->rules_by_package[dynamic_rule.package_name].push_back(std::move(dynamic_rule));
        }
        for (const auto& export_rule : policy.export_rules) {
            ExportRule compiled;
            compiled.package_name = policy.package_name;
            compiled.source_path = export_rule.source_path;
            compiled.target_path = export_rule.target_path;
            compiled.allow_child = export_rule.allow_child;
            compiled.media_scan = export_rule.media_scan;
            compiled.add_to_downloads = export_rule.add_to_downloads;
            compiled.path_kind = export_rule.path_kind == fm::PathKind::kAuto
                ? fm::PathKind::kDirectory
                : export_rule.path_kind;
            output->export_rules_by_package[compiled.package_name].push_back(std::move(compiled));
        }
        for (const auto& delete_rule : policy.delete_rules) {
            DeleteRule compiled;
            compiled.package_name = policy.package_name;
            compiled.source_path = delete_rule.path;
            compiled.path_kind = delete_rule.path_kind == fm::PathKind::kAuto
                ? fm::PathKind::kDirectory
                : delete_rule.path_kind;
            compiled.delete_existing = policy.delete_existing;
            compiled.delete_dir_mode = policy.delete_dir_mode;
            compiled.has_glob = delete_rule.has_glob;
            compiled.has_path_glob = delete_rule.has_path_glob;
            compiled.glob_parent = delete_rule.glob_parent;
            compiled.glob_pattern = delete_rule.glob_pattern;
            compiled.glob_base_prefix = delete_rule.glob_base_prefix;
            compiled.path_glob_pattern = delete_rule.path_glob_pattern;
            output->delete_rules_by_package[compiled.package_name].push_back(std::move(compiled));
        }
    }

    // 提取 [*] 通配规则
    {
        auto it = output->rules_by_package.find("*");
        if (it != output->rules_by_package.end()) {
            output->wildcard_rules = std::move(it->second);
            output->rules_by_package.erase(it);
        }
    }
    {
        auto it = output->delete_rules_by_package.find("*");
        if (it != output->delete_rules_by_package.end()) {
            output->wildcard_delete_rules = std::move(it->second);
            output->delete_rules_by_package.erase(it);
        }
    }

    for (auto& entry : output->rules_by_package) {
        auto& rules = entry.second;
        std::sort(rules.begin(), rules.end(), [](const DynamicRule& lhs, const DynamicRule& rhs) {
            if (lhs.source_path.size() != rhs.source_path.size()) {
                return lhs.source_path.size() > rhs.source_path.size();
            }
            if (lhs.path_kind != rhs.path_kind) {
                return lhs.path_kind == fm::PathKind::kFile && rhs.path_kind != fm::PathKind::kFile;
            }
            auto glob_rank = [](const DynamicRule& rule) {
                if (rule.has_path_glob) {
                    return 0;
                }
                if (rule.has_glob) {
                    return 1;
                }
                return 2;
            };
            int lhs_rank = glob_rank(lhs);
            int rhs_rank = glob_rank(rhs);
            if (lhs_rank != rhs_rank) {
                return lhs_rank > rhs_rank;
            }
            if (lhs.extensions.empty() != rhs.extensions.empty()) {
                return !lhs.extensions.empty();
            }
            return lhs.source_path < rhs.source_path;
        });
    }

    for (auto& entry : output->export_rules_by_package) {
        auto& rules = entry.second;
        std::sort(rules.begin(), rules.end(), [](const ExportRule& lhs, const ExportRule& rhs) {
            if (lhs.source_path.size() != rhs.source_path.size()) {
                return lhs.source_path.size() > rhs.source_path.size();
            }
            if (lhs.path_kind != rhs.path_kind) {
                return lhs.path_kind == fm::PathKind::kFile && rhs.path_kind != fm::PathKind::kFile;
            }
            return lhs.source_path < rhs.source_path;
        });
    }

    for (auto& entry : output->delete_rules_by_package) {
        auto& rules = entry.second;
        std::sort(rules.begin(), rules.end(), [](const DeleteRule& lhs, const DeleteRule& rhs) {
            if (lhs.source_path.size() != rhs.source_path.size()) {
                return lhs.source_path.size() > rhs.source_path.size();
            }
            if (lhs.path_kind != rhs.path_kind) {
                return lhs.path_kind == fm::PathKind::kFile && rhs.path_kind != fm::PathKind::kFile;
            }
            auto glob_rank = [](const DeleteRule& rule) {
                if (rule.has_path_glob) {
                    return 0;
                }
                if (rule.has_glob) {
                    return 1;
                }
                return 2;
            };
            int lhs_rank = glob_rank(lhs);
            int rhs_rank = glob_rank(rhs);
            if (lhs_rank != rhs_rank) {
                return lhs_rank > rhs_rank;
            }
            return lhs.source_path < rhs.source_path;
        });
    }

    return true;
}

bool FindDynamicRule(const std::string& package_name,
                     std::string_view canonical_path,
                     DynamicRule* output) {
    if (output == nullptr) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(g_rules_mutex);
    // 先按包名查找
    auto it = g_dynamic_rules.rules_by_package.find(package_name);
    const std::vector<DynamicRule>* rule_list = nullptr;
    if (it != g_dynamic_rules.rules_by_package.end()) {
        rule_list = &it->second;
    } else if (!g_dynamic_rules.wildcard_rules.empty()) {
        rule_list = &g_dynamic_rules.wildcard_rules;
    }
    if (rule_list == nullptr) {
        return false;
    }
    for (const auto& rule : *rule_list) {
        if (rule.has_path_glob) {
            if (!StartsWith(canonical_path, rule.glob_base_prefix)
                || !MatchPathGlob(rule.path_glob_pattern, canonical_path)) {
                continue;
            }
            if (!MatchesTypeFilter(rule.extensions, canonical_path)) {
                continue;
            }
            *output = rule;
            return true;
        }
        if (rule.has_glob) {
            size_t slash = canonical_path.find_last_of('/');
            if (slash == std::string::npos || slash + 1 >= canonical_path.size()) {
                continue;
            }
            std::string_view parent = canonical_path.substr(0, slash);
            std::string_view name = canonical_path.substr(slash + 1);
            if (parent != rule.glob_parent || !MatchGlob(rule.glob_pattern, name)) {
                continue;
            }
            if (!MatchesTypeFilter(rule.extensions, canonical_path)) {
                continue;
            }
            *output = rule;
            return true;
        }
        if (rule.path_kind == fm::PathKind::kFile) {
            if (canonical_path == rule.source_path) {
                if (!MatchesTypeFilter(rule.extensions, canonical_path)) {
                    continue;
                }
                *output = rule;
                return true;
            }
        } else if (IsDirectoryMatch(canonical_path, rule.source_path)) {
            if (!MatchesTypeFilter(rule.extensions, canonical_path)) {
                continue;
            }
            *output = rule;
            return true;
        }
    }
    return false;
}

bool FindDeleteRule(const std::string& package_name,
                    std::string_view canonical_path,
                    DeleteRule* output) {
    if (output == nullptr) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(g_rules_mutex);
    auto it = g_dynamic_rules.delete_rules_by_package.find(package_name);
    const std::vector<DeleteRule>* rule_list = nullptr;
    if (it != g_dynamic_rules.delete_rules_by_package.end()) {
        rule_list = &it->second;
    } else if (!g_dynamic_rules.wildcard_delete_rules.empty()) {
        rule_list = &g_dynamic_rules.wildcard_delete_rules;
    }
    if (rule_list == nullptr) {
        return false;
    }
    for (const auto& rule : *rule_list) {
        if (rule.has_path_glob) {
            if (!StartsWith(canonical_path, rule.glob_base_prefix)
                || !MatchPathGlob(rule.path_glob_pattern, canonical_path)) {
                continue;
            }
            *output = rule;
            return true;
        }
        if (rule.has_glob) {
            size_t slash = canonical_path.find_last_of('/');
            if (slash == std::string_view::npos || slash + 1 >= canonical_path.size()) {
                continue;
            }
            std::string_view parent = canonical_path.substr(0, slash);
            std::string_view name = canonical_path.substr(slash + 1);
            if (parent != rule.glob_parent || !MatchGlob(rule.glob_pattern, name)) {
                continue;
            }
            *output = rule;
            return true;
        }
        if (rule.path_kind == fm::PathKind::kFile) {
            if (canonical_path == rule.source_path) {
                *output = rule;
                return true;
            }
        } else if (IsDirectoryMatch(canonical_path, rule.source_path)) {
            *output = rule;
            return true;
        }
    }
    return false;
}

bool FindExportRule(const std::string& package_name,
                    std::string_view canonical_path,
                    ExportRule* output) {
    if (output == nullptr) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(g_rules_mutex);
    auto it = g_dynamic_rules.export_rules_by_package.find(package_name);
    if (it == g_dynamic_rules.export_rules_by_package.end()) {
        return false;
    }
    for (const auto& rule : it->second) {
        if (rule.path_kind == fm::PathKind::kFile) {
            if (canonical_path == rule.source_path) {
                *output = rule;
                return true;
            }
        } else if (IsDirectoryMatch(canonical_path, rule.source_path)) {
            *output = rule;
            return true;
        }
    }
    return false;
}

bool BuildDynamicTarget(const DynamicRule& rule,
                        std::string_view canonical_path,
                        int user_id,
                        std::string* output) {
    if (output == nullptr) {
        return false;
    }
    std::string storage_target;
    if (rule.has_path_glob) {
        if (!StartsWith(canonical_path, rule.glob_base_prefix)) {
            return false;
        }
        std::string suffix;
        if (canonical_path.size() > rule.glob_base_prefix.size()) {
            suffix.assign(canonical_path.substr(rule.glob_base_prefix.size()));
        }
        storage_target = rule.target_path + suffix;
        return StoragePathToDataMedia(storage_target, user_id, output);
    }
    if (rule.has_glob) {
        size_t slash = canonical_path.find_last_of('/');
        if (slash == std::string_view::npos || slash + 1 >= canonical_path.size()) {
            return false;
        }
        std::string_view filename = canonical_path.substr(slash + 1);
        storage_target = rule.target_path;
        if (!storage_target.empty() && storage_target.back() != '/') {
            storage_target.push_back('/');
        }
        storage_target.append(filename);
        return StoragePathToDataMedia(storage_target, user_id, output);
    }
    if (rule.path_kind == fm::PathKind::kFile) {
        storage_target = rule.target_path;
    } else {
        if (!IsDirectoryMatch(canonical_path, rule.source_path)) {
            return false;
        }
        std::string suffix;
        if (canonical_path.size() > rule.source_path.size()) {
            suffix.assign(canonical_path.substr(rule.source_path.size()));
        }
        storage_target = rule.target_path + suffix;
    }
    return StoragePathToDataMedia(storage_target, user_id, output);
}

bool BuildExportTarget(const ExportRule& rule,
                       std::string_view canonical_path,
                       int user_id,
                       std::string* output) {
    if (output == nullptr) {
        return false;
    }
    if (rule.path_kind == fm::PathKind::kFile) {
        return StoragePathToDataMedia(rule.target_path, user_id, output);
    }
    if (!IsDirectoryMatch(canonical_path, rule.source_path)) {
        return false;
    }
    std::string suffix;
    if (canonical_path.size() > rule.source_path.size()) {
        suffix.assign(canonical_path.substr(rule.source_path.size()));
    }
    if (!rule.allow_child && !suffix.empty()) {
        std::string_view trimmed = suffix;
        if (trimmed.front() == '/') {
            trimmed.remove_prefix(1);
        }
        if (trimmed.find('/') != std::string_view::npos) {
            return false;
        }
    }
    std::string storage_target = rule.target_path + suffix;
    return StoragePathToDataMedia(storage_target, user_id, output);
}

void WorkerLoop() {
    MoveTask task;
    while (g_running.load()) {
        if (!g_task_queue.Pop(&task)) {
            continue;
        }
        if (!g_running.load()) {
            break;
        }
        usleep(kInitialDelayMs * 1000);
        if (IsFileOpenByPid(task.source_path, task.writer_pid)) {
            if (task.retry_count < kMaxRetryCount) {
                task.retry_count++;
                usleep(kRetryDelayMs * 1000);
                g_task_queue.Push(std::move(task));
            }
            continue;
        }
        if (task.action == TaskAction::kDelete) {
            if (!DeleteFile(task.source_path)) {
                LogPrint(ANDROID_LOG_WARN, "auto delete failed: %s errno=%d", task.source_path.c_str(), errno);
            }
            if (!task.delete_root_path.empty()) {
                if (task.delete_dir_mode == fm::DeleteDirMode::kEmpty) {
                    std::string parent;
                    if (GetParentPath(task.source_path, &parent)) {
                        CleanupEmptyParents(parent, task.delete_root_path);
                    }
                } else if (task.delete_dir_mode == fm::DeleteDirMode::kRecursive) {
                    DeleteTree(task.delete_root_path, task.delete_dir_mode);
                }
            }
            continue;
        }
        if (!MoveFile(task.source_path, task.target_path)) {
            LogPrint(ANDROID_LOG_WARN, "dynamic move failed: %s -> %s", task.source_path.c_str(), task.target_path.c_str());
        }
    }
}

void ReloadRules(const char* config_path) {
    DynamicRuleSet next;
    std::string error;
    if (!BuildDynamicRuleSet(config_path, &next, &error)) {
        LogPrint(ANDROID_LOG_ERROR, "dynamic reload failed: %s", error.c_str());
        return;
    }
    ApplyDeleteExistingRules(next, kPrimaryUserId);
    size_t dynamic_count = 0;
    size_t export_count = 0;
    size_t delete_count = 0;
    GetRuleCounts(next, &dynamic_count, &export_count, &delete_count);
    {
        std::unique_lock<std::shared_mutex> lock(g_rules_mutex);
        g_dynamic_rules = std::move(next);
    }
    LogPrint(ANDROID_LOG_INFO,
             "dynamic rules reloaded: dynamic=%zu export=%zu delete=%zu",
             dynamic_count,
             export_count,
             delete_count);
}

bool CheckRuntimeEnvironment() {
    bool ok = true;
    if (geteuid() != 0) {
        LogPrint(ANDROID_LOG_ERROR, "daemon requires root (uid=0), current=%d", geteuid());
        ok = false;
    }
    bool has_sys_admin = HasCapability(kCapSysAdmin);
    bool has_dac_read = HasCapability(kCapDacReadSearch);
    LogPrint(ANDROID_LOG_INFO,
             "capabilities: CAP_SYS_ADMIN=%d CAP_DAC_READ_SEARCH=%d",
             has_sys_admin ? 1 : 0,
             has_dac_read ? 1 : 0);
    if (!has_sys_admin || !has_dac_read) {
        LogPrint(ANDROID_LOG_ERROR, "missing required capabilities");
        ok = false;
    }
    return ok;
}

void HandleSignal(int signal) {
    if (signal == SIGUSR1) {
        g_reload_requested.store(true);
        return;
    }
    if (signal == SIGTERM || signal == SIGINT) {
        g_running.store(false);
        g_task_queue.Stop();
    }
}

int InitFanotify() {
    int fan_fd = fm_fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME | FAN_REPORT_PIDFD | FAN_CLOEXEC | FAN_NONBLOCK, O_RDONLY | O_CLOEXEC);
    if (fan_fd < 0) {
        LogPrint(ANDROID_LOG_ERROR, "fanotify_init failed: errno=%d", errno);
        return -1;
    }
    if (fm_fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_CLOSE_WRITE, AT_FDCWD, "/data/media") != 0) {
        LogPrint(ANDROID_LOG_ERROR, "fanotify_mark failed: errno=%d", errno);
        close(fan_fd);
        return -1;
    }
    return fan_fd;
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* config_path = kDefaultConfigPath;
    bool self_check = false;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config_path = argv[++index];
        } else if (strcmp(argv[index], "--self-check") == 0) {
            self_check = true;
        }
    }

    DynamicRuleSet initial_rules;
    std::string error;
    fm::FmLogAccessClear();
    if (!ReadModuleEnabled(kModuleConfPath)) {
        LogPrint(ANDROID_LOG_INFO, "module disabled by module.conf, exiting");
        return 0;
    }
    if (!BuildDynamicRuleSet(config_path, &initial_rules, &error)) {
        LogPrint(ANDROID_LOG_ERROR, "dynamic engine init failed: %s", error.c_str());
        return 1;
    }
    size_t dynamic_count = 0;
    size_t export_count = 0;
    size_t delete_count = 0;
    GetRuleCounts(initial_rules, &dynamic_count, &export_count, &delete_count);
    LogPrint(ANDROID_LOG_INFO,
             "rules loaded: dynamic=%zu export=%zu delete=%zu",
             dynamic_count,
             export_count,
             delete_count);
    ApplyDeleteExistingRules(initial_rules, kPrimaryUserId);
    if (!CheckRuntimeEnvironment()) {
        return 1;
    }
    {
        std::unique_lock<std::shared_mutex> lock(g_rules_mutex);
        g_dynamic_rules = std::move(initial_rules);
    }

    signal(SIGTERM, HandleSignal);
    signal(SIGINT, HandleSignal);
    signal(SIGUSR1, HandleSignal);

    if (self_check) {
        int temp_media_fd = open("/data/media", O_PATH | O_CLOEXEC);
        if (temp_media_fd < 0) {
            LogPrint(ANDROID_LOG_ERROR, "self-check open /data/media failed: errno=%d", errno);
            return 1;
        }
        int temp_fan_fd = InitFanotify();
        if (temp_fan_fd >= 0) {
            close(temp_fan_fd);
        }
        close(temp_media_fd);
        if (temp_fan_fd < 0) {
            LogPrint(ANDROID_LOG_ERROR, "self-check failed");
            return 1;
        }
        LogPrint(ANDROID_LOG_INFO, "self-check ok");
        return 0;
    }

    g_media_fd = open("/data/media", O_PATH | O_CLOEXEC);
    if (g_media_fd < 0) {
        LogPrint(ANDROID_LOG_ERROR, "open /data/media failed: errno=%d", errno);
        return 1;
    }

    int fan_fd = InitFanotify();
    if (fan_fd < 0) {
        close(g_media_fd);
        return 1;
    }

    std::thread worker(WorkerLoop);

    alignas(struct fanotify_event_metadata) char buffer[kEventBufferSize];
    while (g_running.load()) {
        if (g_reload_requested.exchange(false)) {
            ReloadRules(config_path);
        }
        struct pollfd pfd = {fan_fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 1000);
        if (ready <= 0) {
            continue;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }
        ssize_t bytes = read(fan_fd, buffer, sizeof(buffer));
        if (bytes <= 0) {
            continue;
        }
        const struct fanotify_event_metadata* meta = reinterpret_cast<const struct fanotify_event_metadata*>(buffer);
        while (FAN_EVENT_OK(meta, bytes)) {
            if (meta->vers != FANOTIFY_METADATA_VERSION) {
                break;
            }
            if ((meta->mask & FAN_Q_OVERFLOW) != 0) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }
            if ((meta->mask & FAN_CLOSE_WRITE) == 0) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }
            int pidfd = -1;
            const struct fanotify_event_info_fid* fid_info = nullptr;
            const struct fanotify_event_info_header* info = FAN_EVENT_INFO_FIRST(meta);
            const struct fanotify_event_info_header* end = reinterpret_cast<const struct fanotify_event_info_header*>(
                reinterpret_cast<const char*>(meta) + meta->event_len);
            while (info != nullptr && info < end) {
                if (info->info_type == FAN_EVENT_INFO_TYPE_PIDFD) {
                    const struct fanotify_event_info_pidfd* pidfd_info = reinterpret_cast<const struct fanotify_event_info_pidfd*>(info);
                    pidfd = pidfd_info->pidfd;
                } else if (info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                    fid_info = reinterpret_cast<const struct fanotify_event_info_fid*>(info);
                }
                info = FAN_EVENT_INFO_NEXT(info, end);
            }
            if (pidfd < 0 || fid_info == nullptr || (meta->mask & (FAN_NOPIDFD | FAN_EPIDFD)) != 0) {
                if (pidfd >= 0) {
                    close(pidfd);
                }
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            pid_t pid = -1;
            if (!ResolvePidFromPidfd(pidfd, &pid)) {
                close(pidfd);
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }
            close(pidfd);

            std::string package_name;
            if (!ReadPackageFromPid(pid, &package_name)) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            std::string data_media_path;
            if (!RebuildPathFromFid(fid_info, &data_media_path)) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            int user_id = 0;
            std::string rel_path;
            if (!ParseDataMediaPath(data_media_path, &user_id, &rel_path)) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            std::string canonical_path;
            if (!BuildCanonicalStoragePath(user_id, rel_path, &canonical_path)) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            DeleteRule delete_rule;
            if (FindDeleteRule(package_name, canonical_path, &delete_rule)) {
                fm::FmLogAccess(package_name.c_str(), pid, "close_write", "delete", canonical_path.c_str());
                MoveTask task;
                task.action = TaskAction::kDelete;
                task.source_path = data_media_path;
                task.package_name = package_name;
                task.writer_pid = pid;
                task.delete_dir_mode = delete_rule.delete_dir_mode;
                if (delete_rule.has_path_glob) {
                    std::string root_path;
                    if (StoragePathToDataMedia(delete_rule.glob_base_prefix, user_id, &root_path)) {
                        task.delete_root_path = std::move(root_path);
                    }
                } else if (delete_rule.has_glob) {
                    std::string root_path;
                    if (StoragePathToDataMedia(delete_rule.glob_parent, user_id, &root_path)) {
                        task.delete_root_path = std::move(root_path);
                    }
                } else if (delete_rule.path_kind != fm::PathKind::kFile) {
                    std::string root_path;
                    if (StoragePathToDataMedia(delete_rule.source_path, user_id, &root_path)) {
                        task.delete_root_path = std::move(root_path);
                    }
                }
                g_task_queue.Push(std::move(task));
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            DynamicRule dynamic_rule;
            if (FindDynamicRule(package_name, canonical_path, &dynamic_rule)) {
                fm::FmLogAccess(package_name.c_str(), pid, "close_write", "redirect", canonical_path.c_str());
                std::string target_path;
                if (BuildDynamicTarget(dynamic_rule, canonical_path, user_id, &target_path)) {
                    MoveTask task;
                    task.source_path = data_media_path;
                    task.target_path = target_path;
                    task.package_name = package_name;
                    task.writer_pid = pid;
                    g_task_queue.Push(std::move(task));
                }
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            ExportRule export_rule;
            if (!FindExportRule(package_name, canonical_path, &export_rule)) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            std::string target_path;
            if (!BuildExportTarget(export_rule, canonical_path, user_id, &target_path)) {
                meta = FAN_EVENT_NEXT(meta, bytes);
                continue;
            }

            if (!CopyFile(data_media_path, target_path)) {
                LogPrint(ANDROID_LOG_WARN, "export copy failed: %s -> %s", data_media_path.c_str(), target_path.c_str());
            } else {
                LogPrint(ANDROID_LOG_INFO, "exported: %s -> %s", data_media_path.c_str(), target_path.c_str());
                if (export_rule.media_scan) {
                    TriggerMediaScan(target_path);
                }
                if (export_rule.add_to_downloads) {
                    AddToDownloads(target_path, user_id, package_name, export_rule.media_scan);
                }
            }

            meta = FAN_EVENT_NEXT(meta, bytes);
        }
    }

    g_task_queue.Stop();
    if (worker.joinable()) {
        worker.join();
    }
    close(fan_fd);
    close(g_media_fd);
    return 0;
}
