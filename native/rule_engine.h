#pragma once

#include <stddef.h>
#include <sys/stat.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "path_kind_cache.h"
#include "runtime_context.h"

namespace fm {

enum class PolicyMode {
    kBlacklist = 0,
    kWhitelist = 1,
};

enum class RuleAction {
    kAllow = 0,
    kDeny = 1,
    kRedirect = 2,
    kRedirectDynamic = 3,
    kDelete = 4,
};

enum class DeleteDirMode {
    kNone = 0,
    kEmpty = 1,
    kRecursive = 2,
};

enum class MediaQueryMode {
    kAuto = 0,
    kEnable = 1,
    kDisable = 2,
};

enum class MatchDecision {
    kNoMatch = 0,
    kAllow = 1,
    kBlock = 2,
    kRedirect = 3,
};

constexpr size_t kInvalidRuleIndex = static_cast<size_t>(-1);

struct ParsedRuleLine {
    RuleAction action = RuleAction::kAllow;
    std::string path;
    std::string redirect_target;
    std::vector<std::string> extensions;
    size_t line_number = 0;
};

struct ParsedSection {
    std::string package_name;
    bool enabled = true;
    bool has_mode = false;
    PolicyMode mode = PolicyMode::kBlacklist;
    bool delete_existing = false;
    DeleteDirMode delete_dir_mode = DeleteDirMode::kNone;
    MediaQueryMode media_query = MediaQueryMode::kAuto;
    bool trash_enabled = false;
    bool trash_on_redirect = false;
    std::string trash_dir;
    int min_age_days = 0;
    long long min_size_mb = 0;
    std::vector<ParsedRuleLine> rules;
};

struct ParsedAccessibleSection {
    std::string anchor_package;
    std::string rule_id;
    std::string from_package;
    std::string to_package;
    std::string path;
    std::string description;
    size_t line_number = 0;
};

struct ParsedExportSection {
    std::string anchor_package;
    std::string rule_id;
    std::string source_path;
    std::string target_path;
    std::string title;
    bool media_scan = false;
    bool add_to_downloads = false;
    bool allow_child = false;
    std::string description;
    size_t line_number = 0;
};

struct ParsedRules {
    std::vector<ParsedSection> sections;
    std::vector<ParsedAccessibleSection> accessible_sections;
    std::vector<ParsedExportSection> export_sections;
    std::vector<std::string> errors;
};

struct CompiledRule {
    RuleAction action = RuleAction::kAllow;
    std::string path;
    std::string redirect_target;
    std::vector<std::string> extensions;
    bool has_glob = false;
    bool has_path_glob = false;
    std::string glob_parent;
    std::string glob_pattern;
    std::string glob_base_prefix;
    std::string path_glob_pattern;
    PathKind path_kind = PathKind::kAuto;
    size_t line_number = 0;
    size_t rule_index = 0;
};

struct CompiledAccessibleFolderRule {
    std::string id;
    std::string anchor_package;
    std::string from_package;
    std::string to_package;
    std::string path;
    std::string description;
    PathKind path_kind = PathKind::kDirectory;
    size_t line_number = 0;
};

struct CompiledExportFolderRule {
    std::string id;
    std::string package_name;
    std::string source_path;
    std::string target_path;
    std::string title;
    bool media_scan = false;
    bool add_to_downloads = false;
    bool allow_child = false;
    std::string description;
    PathKind path_kind = PathKind::kDirectory;
    size_t line_number = 0;
};

struct SecondBucket {
    std::string segment;
    std::vector<const CompiledRule*> rules;
};

struct RootBucket {
    std::string segment;
    std::vector<const CompiledRule*> rules;
    std::vector<SecondBucket> second_buckets;
};

struct AppPolicy {
    std::string package_name;
    PolicyMode mode = PolicyMode::kBlacklist;
    bool delete_existing = false;
    DeleteDirMode delete_dir_mode = DeleteDirMode::kNone;
    MediaQueryMode media_query = MediaQueryMode::kAuto;
    bool trash_enabled = false;
    bool trash_on_redirect = false;
    std::string trash_dir;
    int min_age_days = 0;
    long long min_size_mb = 0;
    std::vector<CompiledRule> rules;
    std::vector<const CompiledRule*> ordered_rules;
    std::vector<CompiledRule> delete_rules;
    std::vector<CompiledAccessibleFolderRule> accessible_rules;
    std::vector<CompiledExportFolderRule> export_rules;
    std::string owned_data_root;
    std::string owned_media_root;
    std::vector<std::string> whitelist_allow_paths;
    std::vector<RootBucket> root_buckets;
    std::vector<const CompiledRule*> external_root_rules;
};

struct MatchResult {
    MatchDecision decision = MatchDecision::kNoMatch;
    std::string redirect_path;
    PathKind resolved_kind = PathKind::kAuto;
    const CompiledRule* matched_rule = nullptr;
    size_t matched_rule_index = kInvalidRuleIndex;
};

struct MatchStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};
    std::atomic<uint64_t> allow{0};
    std::atomic<uint64_t> block{0};
    std::atomic<uint64_t> redirect{0};
    std::atomic<uint64_t> no_match{0};
};

using LstatCallback = int (*)(void* context, const char* path, struct stat* st);

struct FileSystemProbe {
    void* context = nullptr;
    LstatCallback lstat = nullptr;
};

bool ParseRulesIni(std::string_view text, ParsedRules* out_rules);
bool CompilePolicyForProcess(const ParsedRules& parsed_rules,
                             std::string_view process_name,
                             const FileSystemProbe& probe,
                             AppPolicy* out_policy,
                             std::string* error_message);
MatchResult MatchPath(const AppPolicy& policy,
                      std::string_view input_path,
                      const RuntimeContext& context,
                      ResolvedPathKindCache* cache);
MatchResult MatchPathWithStats(const AppPolicy& policy,
                               std::string_view input_path,
                               const RuntimeContext& context,
                               ResolvedPathKindCache* cache,
                               MatchStats* stats);
bool ShouldHideDirEntry(const AppPolicy& policy,
                        std::string_view dir_path,
                        std::string_view entry_name,
                        ResolvedPathKindCache* cache);

}  // namespace fm
