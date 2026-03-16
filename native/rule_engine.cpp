#include "rule_engine.h"

#include <fcntl.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_set>

#include "path_mapper.h"

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#ifndef O_CREAT
#define O_CREAT 0
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

namespace fm {
namespace {

constexpr char kExternalRoot[] = "/storage/emulated/0";
constexpr char kPackagePlaceholder[] = "<pkg>";

std::string_view Trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

size_t FindTypeFilterToken(std::string_view value) {
    size_t offset = 0;
    while (offset < value.size()) {
        size_t pos = value.find("@types=", offset);
        if (pos == std::string_view::npos) {
            return std::string_view::npos;
        }
        if (pos == 0 || std::isspace(static_cast<unsigned char>(value[pos - 1])) != 0) {
            return pos;
        }
        offset = pos + 7;
    }
    return std::string_view::npos;
}

void ToLowerInPlace(std::string* value) {
    if (value == nullptr) {
        return;
    }
    for (char& ch : *value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
}

bool ParseTypeFilter(std::string_view input,
                     std::string_view* output_path,
                     std::vector<std::string>* output_types) {
    if (output_path == nullptr || output_types == nullptr) {
        return false;
    }
    output_types->clear();

    size_t pos = FindTypeFilterToken(input);
    if (pos == std::string_view::npos) {
        *output_path = Trim(input);
        return true;
    }

    std::string_view path_part = Trim(input.substr(0, pos));
    std::string_view types_part = Trim(input.substr(pos + 7));
    if (path_part.empty() || types_part.empty()) {
        return false;
    }

    size_t start = 0;
    while (start < types_part.size()) {
        size_t comma = types_part.find(',', start);
        if (comma == std::string_view::npos) {
            comma = types_part.size();
        }
        std::string_view token = Trim(types_part.substr(start, comma - start));
        if (token.empty()) {
            return false;
        }
        if (!token.empty() && token.front() == '.') {
            token.remove_prefix(1);
        }
        if (token.empty()) {
            return false;
        }
        std::string normalized(token);
        ToLowerInPlace(&normalized);
        output_types->push_back(std::move(normalized));
        start = comma + 1;
    }

    if (output_types->empty()) {
        return false;
    }
    *output_path = path_part;
    return true;
}

void NormalizeExtensions(std::vector<std::string>* extensions) {
    if (extensions == nullptr) {
        return;
    }
    for (std::string& ext : *extensions) {
        if (!ext.empty() && ext.front() == '.') {
            ext.erase(ext.begin());
        }
        ToLowerInPlace(&ext);
    }
    extensions->erase(std::remove_if(extensions->begin(), extensions->end(),
                                     [](const std::string& value) { return value.empty(); }),
                      extensions->end());
    std::sort(extensions->begin(), extensions->end());
    extensions->erase(std::unique(extensions->begin(), extensions->end()), extensions->end());
}

bool HasGlobChars(std::string_view value) {
    return value.find_first_of("*?") != std::string_view::npos;
}

enum class GlobPatternKind {
    kNone = 0,
    kFileName = 1,
    kPath = 2,
    kInvalid = 3,
};

GlobPatternKind AnalyzeGlobPattern(const std::string& normalized_path,
                                   std::string* parent,
                                   std::string* pattern,
                                   std::string* base_prefix,
                                   std::string* path_pattern) {
    size_t glob_pos = normalized_path.find_first_of("*?");
    if (glob_pos == std::string::npos) {
        return GlobPatternKind::kNone;
    }

    size_t last_slash = normalized_path.find_last_of('/');
    if (last_slash == std::string::npos || last_slash + 1 >= normalized_path.size()) {
        return GlobPatternKind::kInvalid;
    }

    bool has_double_star = normalized_path.find("**") != std::string::npos;
    bool glob_in_dir = glob_pos < last_slash;
    if (has_double_star || glob_in_dir) {
        size_t base_end = normalized_path.rfind('/', glob_pos);
        if (base_end == std::string::npos) {
            return GlobPatternKind::kInvalid;
        }
        if (base_prefix != nullptr) {
            base_prefix->assign(normalized_path.substr(0, base_end));
        }
        if (path_pattern != nullptr) {
            path_pattern->assign(normalized_path);
        }
        return GlobPatternKind::kPath;
    }

    if (parent != nullptr) {
        parent->assign(normalized_path.substr(0, last_slash));
    }
    if (pattern != nullptr) {
        pattern->assign(normalized_path.substr(last_slash + 1));
    }
    if (base_prefix != nullptr) {
        base_prefix->assign(normalized_path.substr(0, last_slash));
    }
    return GlobPatternKind::kFileName;
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
    std::vector<std::string_view> pattern_segments;
    std::vector<std::string_view> text_segments;
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

        bool matched = false;
        if (p == p_len && t == t_len) {
            matched = true;
        } else if (p == p_len) {
            matched = false;
        } else {
            std::string_view token = pattern_segments[p];
            if (token == "**") {
                matched = dfs(p + 1, t) || (t < t_len && dfs(p, t + 1));
            } else if (t < t_len && MatchGlob(token, text_segments[t])) {
                matched = dfs(p + 1, t + 1);
            }
        }

        memo[idx] = matched ? 1 : 0;
        return matched;
    };

    return dfs(0, 0);
}


bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool IsProcessMatch(std::string_view package_name, std::string_view process_name) {
    return StartsWith(process_name, package_name)
        && (process_name.size() == package_name.size() || process_name[package_name.size()] == ':');
}

bool NormalizeRulePath(std::string_view input, std::string* output) {
    if (output == nullptr) {
        return false;
    }

    std::string_view trimmed = Trim(input);
    if (trimmed.empty()) {
        return false;
    }

    constexpr size_t kRootLen = sizeof(kExternalRoot) - 1;

    if (trimmed[0] == '/') {
        if (StartsWith(trimmed, kExternalRoot)
            && (trimmed.size() == kRootLen || trimmed[kRootLen] == '/')) {
            while (trimmed.size() > kRootLen && trimmed.back() == '/') {
                trimmed.remove_suffix(1);
            }
            output->assign(trimmed.data(), trimmed.size());
            return true;
        }

        std::string combined(trimmed);
        char normalized[1024] = {0};
        if (!fm_normalize_path(combined.c_str(), normalized, sizeof(normalized))) {
            return false;
        }
        output->assign(normalized);
        return true;
    }

    std::string combined;
    combined.reserve(kRootLen + 1 + trimmed.size());
    combined.assign(kExternalRoot);
    combined.push_back('/');
    combined.append(trimmed);

    char normalized[1024] = {0};
    if (!fm_normalize_path(combined.c_str(), normalized, sizeof(normalized))) {
        return false;
    }

    output->assign(normalized);
    return true;
}

bool ExpandPackagePlaceholder(std::string_view input, std::string_view package_name, std::string* output) {
    if (output == nullptr) {
        return false;
    }

    output->assign(input);
    size_t pos = 0;
    while ((pos = output->find(kPackagePlaceholder, pos)) != std::string::npos) {
        output->replace(pos, strlen(kPackagePlaceholder), package_name);
        pos += package_name.size();
    }
    return true;
}

bool IsExternalPath(std::string_view normalized_path) {
    return StartsWith(normalized_path, kExternalRoot)
        && (normalized_path.size() == sizeof(kExternalRoot) - 1 || normalized_path[sizeof(kExternalRoot) - 1] == '/');
}

bool GetExternalTopSecondSegmentView(std::string_view normalized_path,
                                     std::string_view* top_segment,
                                     std::string_view* second_segment) {
    if (top_segment == nullptr || !IsExternalPath(normalized_path)) {
        return false;
    }
    if (normalized_path.size() <= sizeof(kExternalRoot) - 1) {
        return false;
    }
    size_t start = sizeof(kExternalRoot) - 1;
    if (normalized_path[start] == '/') {
        ++start;
    }
    if (start >= normalized_path.size()) {
        return false;
    }
    size_t first_slash = normalized_path.find('/', start);
    if (first_slash == std::string_view::npos) {
        *top_segment = normalized_path.substr(start);
        if (second_segment != nullptr) {
            *second_segment = std::string_view();
        }
        return true;
    }
    *top_segment = normalized_path.substr(start, first_slash - start);
    if (second_segment != nullptr) {
        size_t second_start = first_slash + 1;
        if (second_start >= normalized_path.size()) {
            *second_segment = std::string_view();
        } else {
            size_t second_slash = normalized_path.find('/', second_start);
            *second_segment = second_slash == std::string_view::npos
                ? normalized_path.substr(second_start)
                : normalized_path.substr(second_start, second_slash - second_start);
        }
    }
    return true;
}

bool GetExternalTopSegmentView(std::string_view normalized_path, std::string_view* segment) {
    std::string_view top;
    std::string_view second;
    if (!GetExternalTopSecondSegmentView(normalized_path, &top, &second)) {
        return false;
    }
    if (segment != nullptr) {
        *segment = top;
    }
    return true;
}

bool LooksLikeFile(std::string_view normalized_path) {
    size_t slash = normalized_path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? normalized_path : normalized_path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot != std::string_view::npos && dot > 0 && dot + 1 < name.size();
}

bool MatchesTypeFilter(const CompiledRule& rule, std::string_view path) {
    if (rule.extensions.empty()) {
        return true;
    }
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    if (slash != std::string_view::npos && dot <= slash) {
        return false;
    }
    if (dot + 1 >= path.size()) {
        return false;
    }
    std::string ext(path.substr(dot + 1));
    ToLowerInPlace(&ext);
    return std::binary_search(rule.extensions.begin(), rule.extensions.end(), ext);
}

const RootBucket* FindRootBucket(const std::vector<RootBucket>& buckets, std::string_view segment) {
    if (buckets.empty()) {
        return nullptr;
    }
    auto it = std::lower_bound(buckets.begin(), buckets.end(), segment,
                               [](const RootBucket& bucket, std::string_view value) {
                                   return bucket.segment < value;
                               });
    if (it != buckets.end() && it->segment == segment) {
        return &(*it);
    }
    return nullptr;
}

const SecondBucket* FindSecondBucket(const RootBucket& bucket, std::string_view segment) {
    if (bucket.second_buckets.empty()) {
        return nullptr;
    }
    auto it = std::lower_bound(bucket.second_buckets.begin(), bucket.second_buckets.end(), segment,
                               [](const SecondBucket& entry, std::string_view value) {
                                   return entry.segment < value;
                               });
    if (it != bucket.second_buckets.end() && it->segment == segment) {
        return &(*it);
    }
    return nullptr;
}

int ActionPriority(RuleAction action) {
    switch (action) {
        case RuleAction::kDeny:
            return 4;
        case RuleAction::kRedirect:
            return 3;
        case RuleAction::kRedirectDynamic:
            return 2;
        case RuleAction::kDelete:
            return 0;
        case RuleAction::kAllow:
        default:
            return 1;
    }
}

int KindPriority(PathKind kind) {
    switch (kind) {
        case PathKind::kFile:
            return 3;
        case PathKind::kDirectory:
            return 2;
        case PathKind::kAuto:
        default:
            return 1;
    }
}

PathKind InferKindFromLstat(const std::string& normalized_path, const FileSystemProbe& probe) {
    if (probe.lstat == nullptr) {
        return PathKind::kAuto;
    }

    struct stat st = {};
    if (probe.lstat(probe.context, normalized_path.c_str(), &st) != 0) {
        return PathKind::kAuto;
    }

    if (S_ISDIR(st.st_mode)) {
        return PathKind::kDirectory;
    }
    if (S_ISREG(st.st_mode)) {
        return PathKind::kFile;
    }
    return PathKind::kAuto;
}

PathKind InferCompiledRuleKind(std::string_view raw_path, const std::string& normalized_path, const FileSystemProbe& probe) {
    const std::string_view trimmed = Trim(raw_path);
    if (!trimmed.empty() && trimmed.back() == '/') {
        return PathKind::kDirectory;
    }

    PathKind lstat_kind = InferKindFromLstat(normalized_path, probe);
    if (lstat_kind != PathKind::kAuto) {
        return lstat_kind;
    }

    if (LooksLikeFile(normalized_path)) {
        return PathKind::kFile;
    }

    return PathKind::kAuto;
}

PathKind InferContextKind(const RuntimeContext& context) {
    switch (context.operation) {
        case PathOperation::kOpenDirectory:
        case PathOperation::kCreateDirectory:
        case PathOperation::kEnumerateDirectory:
        case PathOperation::kRemoveDirectory:
            return PathKind::kDirectory;
        case PathOperation::kCreateFile:
        case PathOperation::kRemoveFile:
            return PathKind::kFile;
        case PathOperation::kOpen:
            if ((context.open_flags & O_DIRECTORY) != 0) {
                return PathKind::kDirectory;
            }
            if ((context.open_flags & O_CREAT) != 0 && (context.open_flags & O_DIRECTORY) == 0) {
                return PathKind::kFile;
            }
            return PathKind::kAuto;
        case PathOperation::kStat:
        case PathOperation::kReadLink:
        case PathOperation::kUnknown:
        default:
            return PathKind::kAuto;
    }
}

bool IsDirectoryMatch(std::string_view input_path, std::string_view rule_path) {
    if (input_path == rule_path) {
        return true;
    }
    return input_path.size() > rule_path.size()
        && StartsWith(input_path, rule_path)
        && input_path[rule_path.size()] == '/';
}

bool IsSameOrParentPathOf(std::string_view candidate_path, std::string_view full_path) {
    std::string current(full_path);
    for (;;) {
        if (candidate_path == current) {
            return true;
        }
        if (current == "/") {
            return false;
        }
        size_t slash = current.find_last_of('/');
        if (slash == std::string::npos) {
            return false;
        }
        if (slash == 0) {
            current = "/";
        } else {
            current.resize(slash);
        }
    }
}

bool IsAppOwnedExternalPath(const AppPolicy& policy, std::string_view normalized_input) {
    if (policy.package_name.empty() || !IsExternalPath(normalized_input)) {
        return false;
    }

    return IsSameOrParentPathOf(normalized_input, policy.owned_data_root)
        || IsDirectoryMatch(normalized_input, policy.owned_data_root)
        || IsSameOrParentPathOf(normalized_input, policy.owned_media_root)
        || IsDirectoryMatch(normalized_input, policy.owned_media_root);
}

void AppendWhitelistAncestorPaths(const std::string& normalized_path,
                                  std::vector<std::string>* output) {
    if (output == nullptr || normalized_path.empty()) {
        return;
    }
    if (!IsExternalPath(normalized_path)) {
        return;
    }
    std::string current = normalized_path;
    for (;;) {
        output->push_back(current);
        if (current == kExternalRoot) {
            break;
        }
        size_t slash = current.find_last_of('/');
        if (slash == std::string::npos) {
            break;
        }
        if (slash == 0) {
            current = "/";
        } else {
            current.resize(slash);
        }
        if (current == "/") {
            break;
        }
    }
}

bool FindWhitelistAllowPath(const AppPolicy& policy, std::string_view normalized_input) {
    const auto& paths = policy.whitelist_allow_paths;
    if (paths.empty()) {
        return false;
    }
    auto it = std::lower_bound(paths.begin(), paths.end(), normalized_input,
                               [](const std::string& lhs, std::string_view rhs) {
                                   return lhs < rhs;
                               });
    return it != paths.end() && *it == normalized_input;
}

bool ShouldImplicitlyAllowWhitelistPath(const AppPolicy& policy, std::string_view normalized_input) {
    if (!IsExternalPath(normalized_input)) {
        return true;
    }

    if (IsAppOwnedExternalPath(policy, normalized_input)) {
        return true;
    }

    if (FindWhitelistAllowPath(policy, normalized_input)) {
        return true;
    }
    for (const CompiledAccessibleFolderRule& rule : policy.accessible_rules) {
        if (IsDirectoryMatch(normalized_input, rule.path)) {
            return true;
        }
    }
    return false;
}

bool BuildRedirectPath(std::string_view input_path,
                       const CompiledRule& rule,
                       PathKind effective_kind,
                       std::string* output_path) {
    if (output_path == nullptr) {
        return false;
    }

    if (rule.has_path_glob) {
        if (!StartsWith(input_path, rule.glob_base_prefix)) {
            return false;
        }
        std::string suffix;
        if (input_path.size() > rule.glob_base_prefix.size()) {
            suffix.assign(input_path.substr(rule.glob_base_prefix.size()));
        }
        output_path->clear();
        output_path->reserve(rule.redirect_target.size() + suffix.size());
        output_path->append(rule.redirect_target);
        output_path->append(suffix);
        return true;
    }

    if (rule.has_glob) {
        size_t slash = input_path.find_last_of('/');
        if (slash == std::string_view::npos || slash + 1 >= input_path.size()) {
            return false;
        }
        std::string_view filename = input_path.substr(slash + 1);
        output_path->clear();
        output_path->reserve(rule.redirect_target.size() + 1 + filename.size());
        output_path->append(rule.redirect_target);
        if (!output_path->empty() && output_path->back() != '/') {
            output_path->push_back('/');
        }
        output_path->append(filename);
        return true;
    }

    if (effective_kind == PathKind::kFile) {
        *output_path = rule.redirect_target;
        return true;
    }

    if (!IsDirectoryMatch(input_path, rule.path)) {
        return false;
    }

    std::string suffix;
    if (input_path.size() > rule.path.size()) {
        suffix.assign(input_path.substr(rule.path.size()));
    }

    output_path->clear();
    output_path->reserve(rule.redirect_target.size() + suffix.size());
    output_path->append(rule.redirect_target);
    output_path->append(suffix);
    return true;
}

PathKind ResolveEffectiveRuleKind(const CompiledRule& rule,
                                  std::string_view input_path,
                                  PathKind context_kind,
                                  ResolvedPathKindCache* cache) {
    if (rule.path_kind != PathKind::kAuto) {
        return rule.path_kind;
    }

    if (context_kind != PathKind::kAuto && input_path == rule.path) {
        if (cache != nullptr) {
            cache->Put(rule.path, context_kind);
        }
        return context_kind;
    }

    PathKind cached = PathKind::kAuto;
    if (cache != nullptr && cache->TryGet(rule.path, &cached) && cached != PathKind::kAuto) {
        return cached;
    }

    if (context_kind != PathKind::kAuto) {
        if (cache != nullptr) {
            cache->Put(rule.path, context_kind);
        }
        return context_kind;
    }

    return PathKind::kDirectory;
}

void AddError(ParsedRules* parsed, size_t line_number, std::string_view message) {
    if (parsed == nullptr) {
        return;
    }

    parsed->errors.emplace_back("line " + std::to_string(line_number) + ": " + std::string(message));
}

bool ParseSubSectionName(std::string_view section_name,
                         std::string_view marker,
                         std::string* anchor_package,
                         std::string* rule_id) {
    size_t marker_pos = section_name.find(marker);
    if (marker_pos == std::string_view::npos) {
        return false;
    }

    const size_t suffix_pos = marker_pos + marker.size();
    std::string_view anchor = Trim(section_name.substr(0, marker_pos));
    std::string_view id = Trim(section_name.substr(suffix_pos));
    if (anchor.empty() || id.empty()) {
        return false;
    }

    if (anchor_package != nullptr) {
        anchor_package->assign(anchor);
    }
    if (rule_id != nullptr) {
        rule_id->assign(id);
    }
    return true;
}

bool SplitKeyValueAssignment(std::string_view line,
                             std::string_view* key,
                             std::string_view* value) {
    if (key == nullptr || value == nullptr) {
        return false;
    }

    size_t equal = line.find('=');
    if (equal == std::string_view::npos) {
        return false;
    }

    *key = Trim(line.substr(0, equal));
    *value = Trim(line.substr(equal + 1));
    return !key->empty();
}

bool ParseBooleanValue(std::string_view value, bool* output) {
    if (output == nullptr) {
        return false;
    }
    if (value == "true") {
        *output = true;
        return true;
    }
    if (value == "false") {
        *output = false;
        return true;
    }
    return false;
}

bool ParseDeleteDirMode(std::string_view value, DeleteDirMode* output) {
    if (output == nullptr) {
        return false;
    }
    if (value == "none") {
        *output = DeleteDirMode::kNone;
        return true;
    }
    if (value == "empty") {
        *output = DeleteDirMode::kEmpty;
        return true;
    }
    if (value == "recursive") {
        *output = DeleteDirMode::kRecursive;
        return true;
    }
    if (value == "true") {
        *output = DeleteDirMode::kRecursive;
        return true;
    }
    if (value == "false") {
        *output = DeleteDirMode::kNone;
        return true;
    }
    return false;
}

bool ParseMediaQueryMode(std::string_view value, MediaQueryMode* output) {
    if (output == nullptr) {
        return false;
    }
    if (value == "auto") {
        *output = MediaQueryMode::kAuto;
        return true;
    }
    if (value == "true") {
        *output = MediaQueryMode::kEnable;
        return true;
    }
    if (value == "false") {
        *output = MediaQueryMode::kDisable;
        return true;
    }
    return false;
}

bool ParseIntegerValue(std::string_view value, long long* output) {
    if (output == nullptr) {
        return false;
    }
    std::string trimmed(value);
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
    if (trimmed.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long long parsed = strtoll(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed < 0) {
        return false;
    }
    *output = parsed;
    return true;
}

bool ParseAccessibleField(ParsedAccessibleSection* section,
                          std::string_view key,
                          std::string_view value) {
    if (section == nullptr) {
        return false;
    }
    if (key == "from") {
        section->from_package.assign(value);
        return true;
    }
    if (key == "to") {
        section->to_package.assign(value);
        return true;
    }
    if (key == "path") {
        section->path.assign(value);
        return true;
    }
    if (key == "description") {
        section->description.assign(value);
        return true;
    }
    return false;
}

bool ParseExportField(ParsedExportSection* section,
                      std::string_view key,
                      std::string_view value) {
    if (section == nullptr) {
        return false;
    }
    if (key == "source") {
        section->source_path.assign(value);
        return true;
    }
    if (key == "target") {
        section->target_path.assign(value);
        return true;
    }
    if (key == "title") {
        section->title.assign(value);
        return true;
    }
    if (key == "description") {
        section->description.assign(value);
        return true;
    }
    if (key == "media_scan") {
        return ParseBooleanValue(value, &section->media_scan);
    }
    if (key == "add_to_downloads") {
        return ParseBooleanValue(value, &section->add_to_downloads);
    }
    if (key == "allow_child") {
        return ParseBooleanValue(value, &section->allow_child);
    }
    return false;
}

void ValidateParsedRules(ParsedRules* parsed) {
    if (parsed == nullptr) {
        return;
    }

    for (const ParsedSection& section : parsed->sections) {
        if (!section.enabled) {
            continue;
        }
        if (!section.has_mode) {
            parsed->errors.emplace_back("package " + section.package_name + ": missing mode");
        }
    }

    for (const ParsedAccessibleSection& section : parsed->accessible_sections) {
        if (section.path.empty()) {
            parsed->errors.emplace_back("line " + std::to_string(section.line_number) + ": accessible section missing path");
        }
        if (section.from_package.empty() && section.to_package.empty()) {
            parsed->errors.emplace_back("line " + std::to_string(section.line_number) + ": accessible section requires from or to");
        }
    }

    for (const ParsedExportSection& section : parsed->export_sections) {
        if (section.source_path.empty()) {
            parsed->errors.emplace_back("line " + std::to_string(section.line_number) + ": export section missing source");
        }
        if (section.target_path.empty()) {
            parsed->errors.emplace_back("line " + std::to_string(section.line_number) + ": export section missing target");
        }
    }
}

const ParsedSection* FindMatchingSection(const ParsedRules& parsed_rules, std::string_view process_name) {
    for (const ParsedSection& section : parsed_rules.sections) {
        if (!section.enabled) {
            continue;
        }
        if (IsProcessMatch(section.package_name, process_name)) {
            return &section;
        }
    }
    return nullptr;
}

bool IsPackageEnabled(const ParsedRules& parsed_rules, std::string_view package_name) {
    for (const ParsedSection& section : parsed_rules.sections) {
        if (section.package_name == package_name) {
            return section.enabled;
        }
    }
    return true;
}

}  // namespace

bool ParseRulesIni(std::string_view text, ParsedRules* out_rules) {
    if (out_rules == nullptr) {
        return false;
    }

    out_rules->sections.clear();
    out_rules->accessible_sections.clear();
    out_rules->export_sections.clear();
    out_rules->errors.clear();

    enum class ActiveSection {
        kNone,
        kPolicy,
        kAccessible,
        kExport,
    };

    ActiveSection active_section = ActiveSection::kNone;
    ParsedSection* current_policy = nullptr;
    ParsedAccessibleSection* current_accessible = nullptr;
    ParsedExportSection* current_export = nullptr;
    std::unordered_set<std::string> section_names;
    std::unordered_set<std::string> packages;
    size_t line_number = 0;
    size_t position = 0;

    while (position <= text.size()) {
        size_t line_end = text.find('\n', position);
        if (line_end == std::string_view::npos) {
            line_end = text.size();
        }

        std::string_view line = text.substr(position, line_end - position);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        ++line_number;
        std::string_view trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                current_policy = nullptr;
                current_accessible = nullptr;
                current_export = nullptr;
                active_section = ActiveSection::kNone;

                std::string_view section_name = Trim(trimmed.substr(1, trimmed.size() - 2));
                if (section_name.empty()) {
                    AddError(out_rules, line_number, "empty section");
                } else if (!section_names.insert(std::string(section_name)).second) {
                    AddError(out_rules, line_number, "duplicate section");
                } else {
                    std::string anchor_package;
                    std::string rule_id;
                    if (ParseSubSectionName(section_name, ".accessible.", &anchor_package, &rule_id)) {
                        out_rules->accessible_sections.push_back({});
                        current_accessible = &out_rules->accessible_sections.back();
                        current_accessible->anchor_package = std::move(anchor_package);
                        current_accessible->rule_id = std::move(rule_id);
                        current_accessible->line_number = line_number;
                        active_section = ActiveSection::kAccessible;
                    } else if (ParseSubSectionName(section_name, ".export.", &anchor_package, &rule_id)) {
                        out_rules->export_sections.push_back({});
                        current_export = &out_rules->export_sections.back();
                        current_export->anchor_package = std::move(anchor_package);
                        current_export->rule_id = std::move(rule_id);
                        current_export->line_number = line_number;
                        active_section = ActiveSection::kExport;
                    } else if (!packages.insert(std::string(section_name)).second) {
                        AddError(out_rules, line_number, "duplicate package section");
                    } else {
                        out_rules->sections.push_back({});
                        current_policy = &out_rules->sections.back();
                        current_policy->package_name.assign(section_name);
                        active_section = ActiveSection::kPolicy;
                    }
                }
            } else if (active_section == ActiveSection::kPolicy) {
                if (StartsWith(trimmed, "mode")) {
                    if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "mode without section");
                    } else {
                        size_t equal = trimmed.find('=');
                        if (equal == std::string_view::npos) {
                            AddError(out_rules, line_number, "invalid mode assignment");
                        } else {
                            std::string_view value = Trim(trimmed.substr(equal + 1));
                            if (value == "whitelist") {
                                current_policy->mode = PolicyMode::kWhitelist;
                                current_policy->has_mode = true;
                            } else if (value == "blacklist") {
                                current_policy->mode = PolicyMode::kBlacklist;
                                current_policy->has_mode = true;
                            } else {
                                AddError(out_rules, line_number, "invalid mode value");
                            }
                        }
                    }
                } else if (StartsWith(trimmed, "enabled") || StartsWith(trimmed, "disabled")) {
                    if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "enabled without section");
                    } else {
                        std::string_view key;
                        std::string_view value;
                        if (!SplitKeyValueAssignment(trimmed, &key, &value)) {
                            AddError(out_rules, line_number, "invalid enabled assignment");
                        } else if (key == "enabled") {
                            bool enabled = true;
                            if (!ParseBooleanValue(value, &enabled)) {
                                AddError(out_rules, line_number, "invalid enabled value");
                            } else {
                                current_policy->enabled = enabled;
                            }
                        } else if (key == "disabled") {
                            bool disabled = false;
                            if (!ParseBooleanValue(value, &disabled)) {
                                AddError(out_rules, line_number, "invalid disabled value");
                            } else {
                                current_policy->enabled = !disabled;
                            }
                        } else {
                            AddError(out_rules, line_number, "unknown enabled key");
                        }
                    }
                } else if (StartsWith(trimmed, "delete_existing") || StartsWith(trimmed, "delete_dirs")) {
                    if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "delete option without section");
                    } else {
                        std::string_view key;
                        std::string_view value;
                        if (!SplitKeyValueAssignment(trimmed, &key, &value)) {
                            AddError(out_rules, line_number, "invalid delete option");
                        } else if (key == "delete_existing") {
                            bool enabled = false;
                            if (!ParseBooleanValue(value, &enabled)) {
                                AddError(out_rules, line_number, "invalid delete_existing value");
                            } else {
                                current_policy->delete_existing = enabled;
                            }
                        } else if (key == "delete_dirs") {
                            DeleteDirMode mode = DeleteDirMode::kNone;
                            if (!ParseDeleteDirMode(value, &mode)) {
                                AddError(out_rules, line_number, "invalid delete_dirs value");
                            } else {
                                current_policy->delete_dir_mode = mode;
                            }
                        } else {
                            AddError(out_rules, line_number, "unknown delete option");
                        }
                    }
                } else if (StartsWith(trimmed, "media_query")) {
                    if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "media_query option without section");
                    } else {
                        std::string_view key;
                        std::string_view value;
                        if (!SplitKeyValueAssignment(trimmed, &key, &value)) {
                            AddError(out_rules, line_number, "invalid media_query option");
                        } else if (key == "media_query") {
                            MediaQueryMode mode = MediaQueryMode::kAuto;
                            if (!ParseMediaQueryMode(value, &mode)) {
                                AddError(out_rules, line_number, "invalid media_query value");
                            } else {
                                current_policy->media_query = mode;
                            }
                        } else {
                            AddError(out_rules, line_number, "unknown media_query option");
                        }
                    }
                } else if (StartsWith(trimmed, "trash") || StartsWith(trimmed, "trash_dir")
                    || StartsWith(trimmed, "trash_redirect")
                    || StartsWith(trimmed, "min_age_days")
                    || StartsWith(trimmed, "min_size_mb")) {
                    if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "trash option without section");
                    } else {
                        std::string_view key;
                        std::string_view value;
                        if (!SplitKeyValueAssignment(trimmed, &key, &value)) {
                            AddError(out_rules, line_number, "invalid trash option");
                        } else if (key == "trash") {
                            bool enabled = false;
                            if (!ParseBooleanValue(value, &enabled)) {
                                AddError(out_rules, line_number, "invalid trash value");
                            } else {
                                current_policy->trash_enabled = enabled;
                            }
                        } else if (key == "trash_redirect") {
                            bool enabled = false;
                            if (!ParseBooleanValue(value, &enabled)) {
                                AddError(out_rules, line_number, "invalid trash_redirect value");
                            } else {
                                current_policy->trash_on_redirect = enabled;
                            }
                        } else if (key == "trash_dir") {
                            if (value.empty()) {
                                AddError(out_rules, line_number, "invalid trash_dir value");
                            } else {
                                current_policy->trash_dir.assign(value);
                            }
                        } else if (key == "min_age_days") {
                            long long days = 0;
                            if (!ParseIntegerValue(value, &days)) {
                                AddError(out_rules, line_number, "invalid min_age_days value");
                            } else {
                                current_policy->min_age_days = static_cast<int>(days);
                            }
                        } else if (key == "min_size_mb") {
                            long long mb = 0;
                            if (!ParseIntegerValue(value, &mb)) {
                                AddError(out_rules, line_number, "invalid min_size_mb value");
                            } else {
                                current_policy->min_size_mb = mb;
                            }
                        } else {
                            AddError(out_rules, line_number, "unknown trash option");
                        }
                    }
                } else if (trimmed.front() == '+' || trimmed.front() == '-' || trimmed.front() == '!') {
                    if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "rule without section");
                    } else {
                        std::string_view rule_path = Trim(trimmed.substr(1));
                        if (rule_path.empty()) {
                            AddError(out_rules, line_number, "empty rule path");
                        } else if (FindTypeFilterToken(rule_path) != std::string_view::npos) {
                            AddError(out_rules, line_number, "types only supported on redirect rules");
                        } else {
                            ParsedRuleLine rule;
                            if (trimmed.front() == '+') {
                                rule.action = RuleAction::kAllow;
                            } else if (trimmed.front() == '-') {
                                rule.action = RuleAction::kDeny;
                            } else {
                                rule.action = RuleAction::kDelete;
                            }
                            rule.path.assign(rule_path);
                            rule.line_number = line_number;
                            current_policy->rules.push_back(std::move(rule));
                        }
                    }
                } else {
                    size_t arrow = trimmed.find("->");
                    size_t alias = trimmed.find("=>");
                    size_t split = std::string_view::npos;
                    size_t arrow_width = 0;
                    if (arrow != std::string_view::npos && (alias == std::string_view::npos || arrow < alias)) {
                        split = arrow;
                        arrow_width = 2;
                    } else if (alias != std::string_view::npos) {
                        split = alias;
                        arrow_width = 2;
                    }

                    if (split == std::string_view::npos) {
                        AddError(out_rules, line_number, "unknown rule syntax");
                    } else if (current_policy == nullptr) {
                        AddError(out_rules, line_number, "redirect without section");
                    } else {
                        std::string_view left = Trim(trimmed.substr(0, split));
                        std::string_view right_raw = Trim(trimmed.substr(split + arrow_width));
                        if (left.empty() || right_raw.empty()) {
                            AddError(out_rules, line_number, "invalid redirect rule");
                        } else if (FindTypeFilterToken(left) != std::string_view::npos) {
                            AddError(out_rules, line_number, "types only supported on redirect target");
                        } else {
                            std::string_view right;
                            std::vector<std::string> extensions;
                            if (!ParseTypeFilter(right_raw, &right, &extensions)) {
                                AddError(out_rules, line_number, "invalid types filter");
                                continue;
                            }
                            ParsedRuleLine rule;
                            rule.action = (split == alias) ? RuleAction::kRedirectDynamic : RuleAction::kRedirect;
                            rule.path.assign(left);
                            rule.redirect_target.assign(right);
                            rule.extensions = std::move(extensions);
                            rule.line_number = line_number;
                            current_policy->rules.push_back(std::move(rule));
                        }
                    }
                }
            } else if (active_section == ActiveSection::kAccessible) {
                std::string_view key;
                std::string_view value;
                if (!SplitKeyValueAssignment(trimmed, &key, &value)) {
                    AddError(out_rules, line_number, "invalid accessible assignment");
                } else if (!ParseAccessibleField(current_accessible, key, value)) {
                    AddError(out_rules, line_number, "unknown accessible field");
                }
            } else if (active_section == ActiveSection::kExport) {
                std::string_view key;
                std::string_view value;
                if (!SplitKeyValueAssignment(trimmed, &key, &value)) {
                    AddError(out_rules, line_number, "invalid export assignment");
                } else if (!ParseExportField(current_export, key, value)) {
                    AddError(out_rules, line_number, "unknown export field");
                }
            } else {
                AddError(out_rules, line_number, "rule without section");
            }
        }

        if (line_end == text.size()) {
            break;
        }
        position = line_end + 1;
    }

    ValidateParsedRules(out_rules);
    return out_rules->errors.empty()
        && (!out_rules->sections.empty() || !out_rules->accessible_sections.empty() || !out_rules->export_sections.empty());
}

bool CompilePolicyForProcess(const ParsedRules& parsed_rules,
                             std::string_view process_name,
                             const FileSystemProbe& probe,
                             AppPolicy* out_policy,
                             std::string* error_message) {
    if (out_policy == nullptr) {
        return false;
    }

    out_policy->package_name.clear();
    out_policy->mode = PolicyMode::kBlacklist;
    out_policy->delete_existing = false;
    out_policy->delete_dir_mode = DeleteDirMode::kNone;
    out_policy->media_query = MediaQueryMode::kAuto;
    out_policy->trash_enabled = false;
    out_policy->trash_on_redirect = false;
    out_policy->trash_dir.clear();
    out_policy->min_age_days = 0;
    out_policy->min_size_mb = 0;
    out_policy->rules.clear();
    out_policy->ordered_rules.clear();
    out_policy->delete_rules.clear();
    out_policy->accessible_rules.clear();
    out_policy->export_rules.clear();
    out_policy->owned_data_root.clear();
    out_policy->owned_media_root.clear();
    out_policy->whitelist_allow_paths.clear();
    out_policy->root_buckets.clear();
    out_policy->external_root_rules.clear();

    if (!parsed_rules.errors.empty()) {
        if (error_message != nullptr) {
            *error_message = parsed_rules.errors.front();
        }
        return false;
    }

    const ParsedSection* section = FindMatchingSection(parsed_rules, process_name);
    if (section == nullptr) {
        if (error_message != nullptr) {
            *error_message = "no matching policy";
        }
        return false;
    }

    out_policy->package_name = section->package_name;
    out_policy->mode = section->mode;
    out_policy->delete_existing = section->delete_existing;
    out_policy->delete_dir_mode = section->delete_dir_mode;
    out_policy->media_query = section->media_query;
    out_policy->trash_enabled = section->trash_enabled;
    out_policy->trash_on_redirect = section->trash_on_redirect;
    out_policy->min_age_days = section->min_age_days;
    out_policy->min_size_mb = section->min_size_mb;
    if (!section->trash_dir.empty()) {
        std::string expanded_trash;
        if (!ExpandPackagePlaceholder(section->trash_dir, section->package_name, &expanded_trash)
            || !NormalizeRulePath(expanded_trash, &out_policy->trash_dir)) {
            if (error_message != nullptr) {
                *error_message = "failed to normalize trash_dir";
            }
            return false;
        }
    }
    out_policy->owned_data_root = std::string(kExternalRoot) + "/Android/data/" + out_policy->package_name;
    out_policy->owned_media_root = std::string(kExternalRoot) + "/Android/media/" + out_policy->package_name;
    out_policy->rules.reserve(section->rules.size());
    out_policy->delete_rules.reserve(section->rules.size());
    out_policy->accessible_rules.reserve(parsed_rules.accessible_sections.size());
    out_policy->export_rules.reserve(parsed_rules.export_sections.size());

    for (const ParsedRuleLine& parsed_rule : section->rules) {
        CompiledRule compiled;
        compiled.action = parsed_rule.action;
        compiled.line_number = parsed_rule.line_number;
        compiled.rule_index = 0;

        std::string expanded_path;
        if (!ExpandPackagePlaceholder(parsed_rule.path, section->package_name, &expanded_path)
            || !NormalizeRulePath(expanded_path, &compiled.path)) {
            if (error_message != nullptr) {
                *error_message = "failed to normalize rule path at line " + std::to_string(parsed_rule.line_number);
            }
            return false;
        }

        if (HasGlobChars(compiled.path)) {
            std::string glob_parent;
            std::string glob_pattern;
            std::string glob_base;
            std::string path_pattern;
            GlobPatternKind kind = AnalyzeGlobPattern(compiled.path,
                                                      &glob_parent,
                                                      &glob_pattern,
                                                      &glob_base,
                                                      &path_pattern);
            if (kind == GlobPatternKind::kInvalid) {
                if (error_message != nullptr) {
                    *error_message = "invalid glob path at line " + std::to_string(parsed_rule.line_number);
                }
                return false;
            }
            if (kind == GlobPatternKind::kFileName) {
                compiled.has_glob = true;
                compiled.glob_parent = std::move(glob_parent);
                compiled.glob_pattern = std::move(glob_pattern);
                compiled.glob_base_prefix = std::move(glob_base);
            } else if (kind == GlobPatternKind::kPath) {
                compiled.has_path_glob = true;
                compiled.path_glob_pattern = std::move(path_pattern);
                compiled.glob_base_prefix = std::move(glob_base);
            }
        }

        if (compiled.action == RuleAction::kRedirect || compiled.action == RuleAction::kRedirectDynamic) {
            std::string expanded_target;
            if (!ExpandPackagePlaceholder(parsed_rule.redirect_target, section->package_name, &expanded_target)
                || !NormalizeRulePath(expanded_target, &compiled.redirect_target)) {
                if (error_message != nullptr) {
                    *error_message = "failed to normalize redirect target at line " + std::to_string(parsed_rule.line_number);
                }
                return false;
            }
            if (HasGlobChars(compiled.redirect_target)) {
                if (error_message != nullptr) {
                    *error_message = "redirect target must not contain glob at line " + std::to_string(parsed_rule.line_number);
                }
                return false;
            }
        }

        compiled.extensions = parsed_rule.extensions;
        NormalizeExtensions(&compiled.extensions);

        compiled.path_kind = InferCompiledRuleKind(parsed_rule.path, compiled.path, probe);
        if (compiled.has_glob || compiled.has_path_glob) {
            compiled.path_kind = PathKind::kFile;
        }
        if (compiled.action == RuleAction::kDelete) {
            compiled.rule_index = out_policy->delete_rules.size();
            out_policy->delete_rules.push_back(std::move(compiled));
        } else {
            compiled.rule_index = out_policy->rules.size();
            out_policy->rules.push_back(std::move(compiled));
        }
    }

    out_policy->ordered_rules.reserve(out_policy->rules.size());
    for (const CompiledRule& rule : out_policy->rules) {
        out_policy->ordered_rules.push_back(&rule);
    }

    std::sort(out_policy->ordered_rules.begin(), out_policy->ordered_rules.end(), [](const CompiledRule* lhs, const CompiledRule* rhs) {
        if (lhs->path.size() != rhs->path.size()) {
            return lhs->path.size() > rhs->path.size();
        }
        if (lhs->path_kind != rhs->path_kind) {
            return KindPriority(lhs->path_kind) > KindPriority(rhs->path_kind);
        }
        if (lhs->action != rhs->action) {
            return ActionPriority(lhs->action) > ActionPriority(rhs->action);
        }
        auto glob_rank = [](const CompiledRule* rule) {
            if (rule->has_path_glob) {
                return 0;
            }
            if (rule->has_glob) {
                return 1;
            }
            return 2;
        };
        int lhs_rank = glob_rank(lhs);
        int rhs_rank = glob_rank(rhs);
        if (lhs_rank != rhs_rank) {
            return lhs_rank > rhs_rank;
        }
        if (lhs->extensions.empty() != rhs->extensions.empty()) {
            return !lhs->extensions.empty();
        }
        return lhs->line_number < rhs->line_number;
    });

    for (const ParsedAccessibleSection& parsed_accessible : parsed_rules.accessible_sections) {
        if (!IsPackageEnabled(parsed_rules, parsed_accessible.anchor_package)) {
            continue;
        }
        const std::string effective_to = parsed_accessible.to_package.empty()
            ? parsed_accessible.anchor_package
            : parsed_accessible.to_package;
        if (!IsProcessMatch(effective_to, process_name)) {
            continue;
        }

        CompiledAccessibleFolderRule compiled;
        compiled.id = parsed_accessible.rule_id;
        compiled.anchor_package = parsed_accessible.anchor_package;
        compiled.from_package = parsed_accessible.from_package.empty()
            ? parsed_accessible.anchor_package
            : parsed_accessible.from_package;
        compiled.to_package = effective_to;
        compiled.description = parsed_accessible.description;
        compiled.line_number = parsed_accessible.line_number;
        std::string expanded_path;
        if (!ExpandPackagePlaceholder(parsed_accessible.path, compiled.anchor_package, &expanded_path)
            || !NormalizeRulePath(expanded_path, &compiled.path)) {
            if (error_message != nullptr) {
                *error_message = "failed to normalize accessible path at line " + std::to_string(parsed_accessible.line_number);
            }
            return false;
        }
        if (HasGlobChars(compiled.path)) {
            if (error_message != nullptr) {
                *error_message = "accessible path must not contain glob at line " + std::to_string(parsed_accessible.line_number);
            }
            return false;
        }
        compiled.path_kind = PathKind::kDirectory;
        out_policy->accessible_rules.push_back(std::move(compiled));
    }

    for (const ParsedExportSection& parsed_export : parsed_rules.export_sections) {
        if (!IsPackageEnabled(parsed_rules, parsed_export.anchor_package)) {
            continue;
        }
        if (parsed_export.anchor_package != section->package_name) {
            continue;
        }

        CompiledExportFolderRule compiled;
        compiled.id = parsed_export.rule_id;
        compiled.package_name = parsed_export.anchor_package;
        compiled.title = parsed_export.title.empty() ? parsed_export.rule_id : parsed_export.title;
        compiled.media_scan = parsed_export.media_scan;
        compiled.add_to_downloads = parsed_export.add_to_downloads;
        compiled.allow_child = parsed_export.allow_child;
        compiled.description = parsed_export.description;
        compiled.line_number = parsed_export.line_number;
        std::string expanded_source;
        if (!ExpandPackagePlaceholder(parsed_export.source_path, compiled.package_name, &expanded_source)
            || !NormalizeRulePath(expanded_source, &compiled.source_path)) {
            if (error_message != nullptr) {
                *error_message = "failed to normalize export source at line " + std::to_string(parsed_export.line_number);
            }
            return false;
        }
        if (HasGlobChars(compiled.source_path)) {
            if (error_message != nullptr) {
                *error_message = "export source must not contain glob at line " + std::to_string(parsed_export.line_number);
            }
            return false;
        }
        std::string expanded_target;
        if (!ExpandPackagePlaceholder(parsed_export.target_path, compiled.package_name, &expanded_target)
            || !NormalizeRulePath(expanded_target, &compiled.target_path)) {
            if (error_message != nullptr) {
                *error_message = "failed to normalize export target at line " + std::to_string(parsed_export.line_number);
            }
            return false;
        }
        if (HasGlobChars(compiled.target_path)) {
            if (error_message != nullptr) {
                *error_message = "export target must not contain glob at line " + std::to_string(parsed_export.line_number);
            }
            return false;
        }
        compiled.path_kind = InferCompiledRuleKind(parsed_export.source_path, compiled.source_path, probe);
        if (compiled.path_kind == PathKind::kAuto) {
            compiled.path_kind = PathKind::kDirectory;
        }
        out_policy->export_rules.push_back(std::move(compiled));
    }

    if (out_policy->mode == PolicyMode::kWhitelist) {
        for (const CompiledRule& rule : out_policy->rules) {
            if (rule.action == RuleAction::kAllow
                || rule.action == RuleAction::kRedirect
                || rule.action == RuleAction::kRedirectDynamic) {
                AppendWhitelistAncestorPaths(rule.path, &out_policy->whitelist_allow_paths);
            }
        }
        for (const CompiledAccessibleFolderRule& rule : out_policy->accessible_rules) {
            AppendWhitelistAncestorPaths(rule.path, &out_policy->whitelist_allow_paths);
        }
        if (!out_policy->whitelist_allow_paths.empty()) {
            std::sort(out_policy->whitelist_allow_paths.begin(), out_policy->whitelist_allow_paths.end());
            out_policy->whitelist_allow_paths.erase(
                std::unique(out_policy->whitelist_allow_paths.begin(), out_policy->whitelist_allow_paths.end()),
                out_policy->whitelist_allow_paths.end());
        }
    }

    auto find_or_create_bucket = [](std::vector<RootBucket>* buckets, std::string_view segment) -> RootBucket* {
        if (buckets == nullptr) {
            return nullptr;
        }
        for (auto& bucket : *buckets) {
            if (bucket.segment == segment) {
                return &bucket;
            }
        }
        RootBucket bucket;
        bucket.segment.assign(segment);
        buckets->push_back(std::move(bucket));
        return &buckets->back();
    };

    auto find_or_create_second_bucket = [](std::vector<SecondBucket>* buckets,
                                           std::string_view segment) -> SecondBucket* {
        if (buckets == nullptr) {
            return nullptr;
        }
        for (auto& bucket : *buckets) {
            if (bucket.segment == segment) {
                return &bucket;
            }
        }
        SecondBucket bucket;
        bucket.segment.assign(segment);
        buckets->push_back(std::move(bucket));
        return &buckets->back();
    };

    for (const CompiledRule* rule : out_policy->ordered_rules) {
        std::string_view top_segment;
        std::string_view second_segment;
        std::string_view bucket_path = rule->has_path_glob
            ? std::string_view(rule->glob_base_prefix)
            : std::string_view(rule->path);
        if (GetExternalTopSecondSegmentView(bucket_path, &top_segment, &second_segment)) {
            RootBucket* bucket = find_or_create_bucket(&out_policy->root_buckets, top_segment);
            if (bucket != nullptr) {
                if (!second_segment.empty()) {
                    SecondBucket* second_bucket =
                        find_or_create_second_bucket(&bucket->second_buckets, second_segment);
                    if (second_bucket != nullptr) {
                        second_bucket->rules.push_back(rule);
                    }
                } else {
                    bucket->rules.push_back(rule);
                }
            }
        } else if (bucket_path == kExternalRoot) {
            out_policy->external_root_rules.push_back(rule);
        }
    }

    if (!out_policy->root_buckets.empty()) {
        std::sort(out_policy->root_buckets.begin(), out_policy->root_buckets.end(),
                  [](const RootBucket& lhs, const RootBucket& rhs) {
                      return lhs.segment < rhs.segment;
                  });
        for (auto& bucket : out_policy->root_buckets) {
            if (!bucket.second_buckets.empty()) {
                std::sort(bucket.second_buckets.begin(), bucket.second_buckets.end(),
                          [](const SecondBucket& lhs, const SecondBucket& rhs) {
                              return lhs.segment < rhs.segment;
                          });
            }
        }
    }

    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}


void UpdateMatchStats(MatchStats* stats, MatchDecision decision, uint64_t elapsed_ns) {
    if (stats == nullptr) {
        return;
    }
    stats->calls.fetch_add(1, std::memory_order_relaxed);
    stats->total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    uint64_t current_max = stats->max_ns.load(std::memory_order_relaxed);
    while (elapsed_ns > current_max
        && !stats->max_ns.compare_exchange_weak(current_max, elapsed_ns, std::memory_order_relaxed)) {
    }
    switch (decision) {
        case MatchDecision::kAllow:
            stats->allow.fetch_add(1, std::memory_order_relaxed);
            break;
        case MatchDecision::kBlock:
            stats->block.fetch_add(1, std::memory_order_relaxed);
            break;
        case MatchDecision::kRedirect:
            stats->redirect.fetch_add(1, std::memory_order_relaxed);
            break;
        case MatchDecision::kNoMatch:
        default:
            stats->no_match.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

MatchResult MatchPath(const AppPolicy& policy,
                      std::string_view input_path,
                      const RuntimeContext& context,
                      ResolvedPathKindCache* cache) {
    MatchResult result;

    if (context.already_redirected) {
        result.decision = MatchDecision::kAllow;
        return result;
    }

    if (policy.mode == PolicyMode::kBlacklist
        && policy.ordered_rules.empty()
        && policy.root_buckets.empty()
        && policy.external_root_rules.empty()) {
        result.decision = MatchDecision::kAllow;
        return result;
    }

    std::string normalized_input;
    if (!NormalizeRulePath(input_path, &normalized_input)) {
        result.decision = MatchDecision::kNoMatch;
        return result;
    }

    const PathKind context_kind = InferContextKind(context);
    if (cache != nullptr && context_kind != PathKind::kAuto) {
        cache->Put(normalized_input, context_kind);
    }

    auto match_rules = [&](const std::vector<const CompiledRule*>& rules) -> bool {
        for (const CompiledRule* rule : rules) {
            if (rule == nullptr) {
                continue;
            }
            if (rule->action == RuleAction::kDelete) {
                continue;
            }

            PathKind effective_kind = ResolveEffectiveRuleKind(*rule, normalized_input, context_kind, cache);
            bool matched = false;
            if (rule->has_path_glob) {
                effective_kind = PathKind::kFile;
                if (StartsWith(normalized_input, rule->glob_base_prefix)
                    && MatchPathGlob(rule->path_glob_pattern, normalized_input)) {
                    matched = true;
                }
            } else if (rule->has_glob) {
                effective_kind = PathKind::kFile;
                size_t slash = normalized_input.find_last_of('/');
                if (slash != std::string::npos && slash + 1 < normalized_input.size()) {
                    std::string parent(normalized_input.substr(0, slash));
                    std::string name(normalized_input.substr(slash + 1));
                    if (parent == rule->glob_parent && MatchGlob(rule->glob_pattern, name)) {
                        matched = true;
                    }
                }
            } else if (effective_kind == PathKind::kFile) {
                matched = normalized_input == rule->path;
            } else {
                matched = IsDirectoryMatch(normalized_input, rule->path);
            }

            if (!matched) {
                continue;
            }
            if (!MatchesTypeFilter(*rule, normalized_input)) {
                continue;
            }

            result.matched_rule = rule;
            result.resolved_kind = effective_kind;
            result.matched_rule_index = rule->rule_index;
            switch (rule->action) {
                case RuleAction::kDeny:
                    result.decision = MatchDecision::kBlock;
                    return true;
                case RuleAction::kAllow:
                    result.decision = MatchDecision::kAllow;
                    return true;
                case RuleAction::kRedirect:
                    if (BuildRedirectPath(normalized_input, *rule, effective_kind, &result.redirect_path)) {
                        result.decision = MatchDecision::kRedirect;
                        return true;
                    }
                    break;
                case RuleAction::kRedirectDynamic:
                    result.decision = MatchDecision::kAllow;
                    return true;
                case RuleAction::kDelete:
                    break;
            }
        }
        return false;
    };

    if (IsExternalPath(normalized_input)) {
        std::string_view top_segment;
        std::string_view second_segment;
        if (GetExternalTopSecondSegmentView(normalized_input, &top_segment, &second_segment)) {
            const RootBucket* bucket = FindRootBucket(policy.root_buckets, top_segment);
            if (bucket != nullptr) {
                if (!second_segment.empty()) {
                    const SecondBucket* second_bucket = FindSecondBucket(*bucket, second_segment);
                    if (second_bucket != nullptr && match_rules(second_bucket->rules)) {
                        return result;
                    }
                }
                if (!bucket->rules.empty() && match_rules(bucket->rules)) {
                    return result;
                }
            }
        }
        if (!policy.external_root_rules.empty() && match_rules(policy.external_root_rules)) {
            return result;
        }
    } else if (match_rules(policy.ordered_rules)) {
        return result;
    }

    if (policy.mode == PolicyMode::kWhitelist) {
        result.decision = ShouldImplicitlyAllowWhitelistPath(policy, normalized_input)
            ? MatchDecision::kAllow
            : MatchDecision::kBlock;
    } else {
        result.decision = MatchDecision::kAllow;
    }
    return result;
}

MatchResult MatchPathWithStats(const AppPolicy& policy,
                               std::string_view input_path,
                               const RuntimeContext& context,
                               ResolvedPathKindCache* cache,
                               MatchStats* stats) {
    const auto start = std::chrono::steady_clock::now();
    MatchResult result = MatchPath(policy, input_path, context, cache);
    const auto end = std::chrono::steady_clock::now();
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    UpdateMatchStats(stats, result.decision, elapsed_ns);
    return result;
}

bool ShouldHideDirEntry(const AppPolicy& policy,
                        std::string_view dir_path,
                        std::string_view entry_name,
                        ResolvedPathKindCache* cache) {
    if (entry_name.empty() || entry_name == "." || entry_name == "..") {
        return false;
    }

    size_t dir_len = dir_path.size();
    size_t name_len = entry_name.size();
    bool add_slash = dir_len > 0 && dir_path.back() != '/';
    size_t total = dir_len + (add_slash ? 1 : 0) + name_len;
    char child_path[1024] = {0};
    if (total >= sizeof(child_path)) {
        return false;
    }
    if (dir_len > 0) {
        memcpy(child_path, dir_path.data(), dir_len);
    }
    if (add_slash) {
        child_path[dir_len] = '/';
    }
    memcpy(child_path + dir_len + (add_slash ? 1 : 0), entry_name.data(), name_len);
    child_path[total] = '\0';

    RuntimeContext context;
    context.operation = PathOperation::kEnumerateDirectory;
    MatchResult result = MatchPath(policy, std::string_view(child_path, total), context, cache);
    return result.decision == MatchDecision::kBlock;
}

}  // namespace fm