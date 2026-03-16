#include "rule_config.h"

#include <atomic>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "path_mapper.h"
#include "rule_engine.h"

namespace {

struct FmRuleEngineState {
    fm::AppPolicy policy;
    fm::ResolvedPathKindCache cache;
    std::unique_ptr<std::atomic<uint64_t>[]> rule_hit_counts;
    size_t rule_hit_count_size = 0;
};

FmRuleEngineState *fm_get_engine_state(FmRuleSet *rule_set) {
    return rule_set != nullptr ? static_cast<FmRuleEngineState *>(rule_set->engine_state) : nullptr;
}

const FmRuleEngineState *fm_get_engine_state(const FmRuleSet *rule_set) {
    return rule_set != nullptr ? static_cast<const FmRuleEngineState *>(rule_set->engine_state) : nullptr;
}

fm::RuleAction fm_map_rule_type(FmRuleType type) {
    switch (type) {
        case FM_RULE_ALLOW:
            return fm::RuleAction::kAllow;
        case FM_RULE_BLOCK:
            return fm::RuleAction::kDeny;
        case FM_RULE_REDIRECT:
            return fm::RuleAction::kRedirect;
        case FM_RULE_REDIRECT_DYNAMIC:
            return fm::RuleAction::kRedirectDynamic;
        case FM_RULE_INVALID:
        default:
            return fm::RuleAction::kAllow;
    }
}

FmRuleType fm_map_rule_type(fm::RuleAction action) {
    switch (action) {
        case fm::RuleAction::kAllow:
            return FM_RULE_ALLOW;
        case fm::RuleAction::kDeny:
            return FM_RULE_BLOCK;
        case fm::RuleAction::kRedirect:
            return FM_RULE_REDIRECT;
        case fm::RuleAction::kRedirectDynamic:
            return FM_RULE_REDIRECT_DYNAMIC;
        case fm::RuleAction::kDelete:
            return FM_RULE_INVALID;
        default:
            return FM_RULE_INVALID;
    }
}

FmRulePathKind fm_map_path_kind(fm::PathKind kind) {
    switch (kind) {
        case fm::PathKind::kFile:
            return FM_RULE_PATH_FILE;
        case fm::PathKind::kDirectory:
            return FM_RULE_PATH_DIRECTORY;
        case fm::PathKind::kAuto:
        default:
            return FM_RULE_PATH_AUTO;
    }
}

FmMediaQueryMode fm_map_media_query_mode(fm::MediaQueryMode mode) {
    switch (mode) {
        case fm::MediaQueryMode::kEnable:
            return FM_MEDIA_QUERY_ENABLE;
        case fm::MediaQueryMode::kDisable:
            return FM_MEDIA_QUERY_DISABLE;
        case fm::MediaQueryMode::kAuto:
        default:
            return FM_MEDIA_QUERY_AUTO;
    }
}

fm::PathOperation fm_map_operation(FmPathOperation operation) {
    switch (operation) {
        case FM_PATH_OP_OPEN:
            return fm::PathOperation::kOpen;
        case FM_PATH_OP_OPEN_DIRECTORY:
            return fm::PathOperation::kOpenDirectory;
        case FM_PATH_OP_CREATE_FILE:
            return fm::PathOperation::kCreateFile;
        case FM_PATH_OP_CREATE_DIRECTORY:
            return fm::PathOperation::kCreateDirectory;
        case FM_PATH_OP_STAT:
            return fm::PathOperation::kStat;
        case FM_PATH_OP_ENUMERATE_DIRECTORY:
            return fm::PathOperation::kEnumerateDirectory;
        case FM_PATH_OP_READLINK:
            return fm::PathOperation::kReadLink;
        case FM_PATH_OP_REMOVE_FILE:
            return fm::PathOperation::kRemoveFile;
        case FM_PATH_OP_REMOVE_DIRECTORY:
            return fm::PathOperation::kRemoveDirectory;
        case FM_PATH_OP_UNKNOWN:
        default:
            return fm::PathOperation::kUnknown;
    }
}

bool fm_read_file_from_fd(int fd, std::string *output) {
#if defined(_WIN32)
    (void)fd;
    (void)output;
    return false;
#else
    if (fd < 0 || output == nullptr) {
        return false;
    }

    output->clear();
    char buffer[4096];
    for (;;) {
        ssize_t read_bytes = read(fd, buffer, sizeof(buffer));
        if (read_bytes < 0) {
            return false;
        }
        if (read_bytes == 0) {
            return true;
        }
        output->append(buffer, static_cast<size_t>(read_bytes));
    }
#endif
}

int fm_lstat_callback(void *context, const char *path, struct stat *st) {
#if defined(_WIN32)
    (void)context;
    (void)path;
    (void)st;
    return -1;
#else
    (void)context;
    return lstat(path, st);
#endif
}

bool fm_copy_string(const std::string &value, char *output, size_t output_size) {
    if (output == nullptr || output_size == 0 || value.size() + 1 > output_size) {
        return false;
    }
    memcpy(output, value.c_str(), value.size() + 1);
    return true;
}

bool fm_populate_public_rules(FmRuleSet *rule_set, const fm::AppPolicy &policy) {
    if (rule_set == nullptr) {
        return false;
    }

    rule_set->count = 0;
    rule_set->accessible_count = 0;
    rule_set->export_count = 0;
    rule_set->mode = policy.mode == fm::PolicyMode::kWhitelist ? FM_RULE_MODE_WHITELIST : FM_RULE_MODE_BLACKLIST;
    rule_set->media_query_mode = fm_map_media_query_mode(policy.media_query);

    for (const fm::CompiledRule &compiled_rule : policy.rules) {
        if (rule_set->count >= FM_MAX_RULES) {
            break;
        }

        FmRule &public_rule = rule_set->rules[rule_set->count++];
        memset(&public_rule, 0, sizeof(public_rule));
        public_rule.type = fm_map_rule_type(compiled_rule.action);
        public_rule.path_kind = fm_map_path_kind(compiled_rule.path_kind);
        if (!fm_copy_string(policy.package_name, public_rule.package_name, sizeof(public_rule.package_name))) {
            return false;
        }
        if (!fm_copy_string(compiled_rule.path, public_rule.target_path, sizeof(public_rule.target_path))) {
            return false;
        }
        if (compiled_rule.action == fm::RuleAction::kRedirect
            || compiled_rule.action == fm::RuleAction::kRedirectDynamic) {
            if (!fm_copy_string(compiled_rule.redirect_target, public_rule.source_path, sizeof(public_rule.source_path))) {
                return false;
            }
        }
    }

    for (const fm::CompiledAccessibleFolderRule &compiled_rule : policy.accessible_rules) {
        if (rule_set->accessible_count >= FM_MAX_ACCESSIBLE_RULES) {
            break;
        }

        FmAccessibleFolderRule &public_rule = rule_set->accessible_rules[rule_set->accessible_count++];
        memset(&public_rule, 0, sizeof(public_rule));
        public_rule.path_kind = fm_map_path_kind(compiled_rule.path_kind);
        if (!fm_copy_string(compiled_rule.id, public_rule.id, sizeof(public_rule.id))
            || !fm_copy_string(compiled_rule.anchor_package, public_rule.anchor_package, sizeof(public_rule.anchor_package))
            || !fm_copy_string(compiled_rule.from_package, public_rule.from_package, sizeof(public_rule.from_package))
            || !fm_copy_string(compiled_rule.to_package, public_rule.to_package, sizeof(public_rule.to_package))
            || !fm_copy_string(compiled_rule.path, public_rule.path, sizeof(public_rule.path))
            || !fm_copy_string(compiled_rule.description, public_rule.description, sizeof(public_rule.description))) {
            return false;
        }
    }

    for (const fm::CompiledExportFolderRule &compiled_rule : policy.export_rules) {
        if (rule_set->export_count >= FM_MAX_EXPORT_RULES) {
            break;
        }

        FmExportFolderRule &public_rule = rule_set->export_rules[rule_set->export_count++];
        memset(&public_rule, 0, sizeof(public_rule));
        public_rule.media_scan = compiled_rule.media_scan;
        public_rule.add_to_downloads = compiled_rule.add_to_downloads;
        public_rule.allow_child = compiled_rule.allow_child;
        if (!fm_copy_string(compiled_rule.id, public_rule.id, sizeof(public_rule.id))
            || !fm_copy_string(compiled_rule.package_name, public_rule.package_name, sizeof(public_rule.package_name))
            || !fm_copy_string(compiled_rule.source_path, public_rule.source_path, sizeof(public_rule.source_path))
            || !fm_copy_string(compiled_rule.target_path, public_rule.target_path, sizeof(public_rule.target_path))
            || !fm_copy_string(compiled_rule.title, public_rule.title, sizeof(public_rule.title))
            || !fm_copy_string(compiled_rule.description, public_rule.description, sizeof(public_rule.description))) {
            return false;
        }
    }

    return rule_set->count > 0 || rule_set->accessible_count > 0 || rule_set->export_count > 0;
}

bool fm_normalize_rule_path(const char *input_path, char *output_path, size_t output_size) {
    static constexpr const char kExternalRoot[] = "/storage/emulated/0";
    if (input_path == nullptr || output_path == nullptr || output_size == 0) {
        return false;
    }
    if (input_path[0] == '/') {
        return fm_normalize_path(input_path, output_path, output_size);
    }
    int written = snprintf(output_path, output_size, "%s/%s", kExternalRoot, input_path);
    if (written < 0 || static_cast<size_t>(written) >= output_size) {
        return false;
    }
    return fm_normalize_path(output_path, output_path, output_size);
}

bool fm_build_rule_set_from_text(const std::string &text, const char *process_name, FmRuleSet *out_rule_set) {
    if (process_name == nullptr || out_rule_set == nullptr) {
        return false;
    }

    fm_reset_rule_set(out_rule_set);

    fm::ParsedRules parsed_rules;
    if (!fm::ParseRulesIni(text, &parsed_rules)) {
        return false;
    }

    auto *state = new FmRuleEngineState();
    fm::FileSystemProbe probe{nullptr, &fm_lstat_callback};
    std::string error;
    if (!fm::CompilePolicyForProcess(parsed_rules, process_name, probe, &state->policy, &error)) {
        delete state;
        if (error == "no matching policy") {
            return true;
        }
        return false;
    }

    state->rule_hit_count_size = state->policy.rules.size();
    if (state->rule_hit_count_size > 0) {
        state->rule_hit_counts = std::make_unique<std::atomic<uint64_t>[]>(state->rule_hit_count_size);
        for (size_t index = 0; index < state->rule_hit_count_size; ++index) {
            state->rule_hit_counts[index].store(0, std::memory_order_relaxed);
        }
    }

    if (!fm_populate_public_rules(out_rule_set, state->policy)) {
        delete state;
        fm_reset_rule_set(out_rule_set);
        return false;
    }

    out_rule_set->engine_state = state;
    return true;
}

}  // namespace

void fm_reset_rule_set(FmRuleSet *rule_set) {
    if (rule_set == nullptr) {
        return;
    }

    delete fm_get_engine_state(rule_set);
    memset(rule_set, 0, sizeof(*rule_set));
    rule_set->mode = FM_RULE_MODE_BLACKLIST;
    rule_set->media_query_mode = FM_MEDIA_QUERY_AUTO;
}

bool fm_has_matching_rule(const FmRuleSet *rule_set) {
    return rule_set != nullptr
        && (rule_set->count > 0 || rule_set->accessible_count > 0 || rule_set->export_count > 0)
        && fm_get_engine_state(rule_set) != nullptr;
}

void fm_init_runtime_context(FmRuntimeContext *context) {
    if (context == nullptr) {
        return;
    }
    memset(context, 0, sizeof(*context));
    context->operation = FM_PATH_OP_UNKNOWN;
}

void fm_init_path_decision(FmPathDecision *decision) {
    if (decision == nullptr) {
        return;
    }
    memset(decision, 0, sizeof(*decision));
    decision->type = FM_DECISION_ALLOW;
}

#if defined(_WIN32)
bool fm_load_rules_from_module_dir_fd(int module_dir_fd, const char *process_name, FmRuleSet *out_rule_set) {
    (void)module_dir_fd;
    (void)process_name;
    (void)out_rule_set;
    return false;
}
#else
bool fm_load_rules_from_module_dir_fd(int module_dir_fd, const char *process_name, FmRuleSet *out_rule_set) {
    if (module_dir_fd < 0 || process_name == nullptr || out_rule_set == nullptr) {
        return false;
    }

    int config_fd = openat(module_dir_fd, "config/rules.ini", O_RDONLY | O_CLOEXEC);
    if (config_fd < 0) {
        return false;
    }

    std::string text;
    bool read_ok = fm_read_file_from_fd(config_fd, &text);
    close(config_fd);
    if (!read_ok) {
        return false;
    }

    return fm_build_rule_set_from_text(text, process_name, out_rule_set);
}
#endif

bool fm_load_rules_from_text(const char *text, const char *process_name, FmRuleSet *out_rule_set) {
    if (text == nullptr || process_name == nullptr || out_rule_set == nullptr) {
        return false;
    }
    return fm_build_rule_set_from_text(text, process_name, out_rule_set);
}

bool fm_decide_path(FmRuleSet *rule_set, const char *input_path, FmPathDecision *decision) {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    return fm_decide_path_with_context(rule_set, input_path, &context, decision);
}

bool fm_decide_path_with_context(FmRuleSet *rule_set, const char *input_path, const FmRuntimeContext *context, FmPathDecision *decision) {
    if (rule_set == nullptr || input_path == nullptr || decision == nullptr) {
        return false;
    }

    FmRuleEngineState *state = fm_get_engine_state(rule_set);
    if (state == nullptr) {
        return false;
    }

    fm_init_path_decision(decision);

    fm::RuntimeContext runtime_context;
    if (context != nullptr) {
        runtime_context.operation = fm_map_operation(context->operation);
        runtime_context.open_flags = context->open_flags;
        runtime_context.already_redirected = context->already_redirected;
    }

    fm::MatchResult result = fm::MatchPath(state->policy, input_path, runtime_context, &state->cache);
    if (result.matched_rule_index != fm::kInvalidRuleIndex
        && result.matched_rule_index < state->rule_hit_count_size
        && state->rule_hit_counts) {
        state->rule_hit_counts[result.matched_rule_index].fetch_add(1, std::memory_order_relaxed);
    }
    if (result.decision == fm::MatchDecision::kBlock) {
        decision->type = FM_DECISION_BLOCK;
        return true;
    }
    if (result.decision == fm::MatchDecision::kRedirect) {
        if (!fm_copy_string(result.redirect_path, decision->redirected_path, sizeof(decision->redirected_path))) {
            return false;
        }
        decision->type = FM_DECISION_REDIRECT;
        return true;
    }
    return false;
}

bool fm_should_hide_dir_entry(FmRuleSet *rule_set, const char *dir_path, const char *entry_name) {
    if (rule_set == nullptr || dir_path == nullptr || entry_name == nullptr) {
        return false;
    }

    FmRuleEngineState *state = fm_get_engine_state(rule_set);
    if (state == nullptr) {
        return false;
    }

    return fm::ShouldHideDirEntry(state->policy, dir_path, entry_name, &state->cache);
}

bool fm_get_rule_hit_count(const FmRuleSet *rule_set, size_t index, uint64_t *out_count) {
    if (rule_set == nullptr || out_count == nullptr) {
        return false;
    }
    const FmRuleEngineState *state = fm_get_engine_state(rule_set);
    if (state == nullptr || index >= state->rule_hit_count_size || !state->rule_hit_counts) {
        return false;
    }
    *out_count = state->rule_hit_counts[index].load(std::memory_order_relaxed);
    return true;
}

void fm_invalidate_path_kind(FmRuleSet *rule_set, const char *path) {
    if (rule_set == nullptr || path == nullptr) {
        return;
    }
    FmRuleEngineState *state = fm_get_engine_state(rule_set);
    if (state == nullptr) {
        return;
    }
    char normalized[FM_MAX_PATH_LEN] = {0};
    if (!fm_normalize_rule_path(path, normalized, sizeof(normalized))) {
        return;
    }
    state->cache.Erase(normalized);
}


size_t fm_get_accessible_rule_count(const FmRuleSet *rule_set) {
    return rule_set != nullptr ? rule_set->accessible_count : 0;
}

bool fm_get_accessible_rule(const FmRuleSet *rule_set, size_t index, FmAccessibleFolderRule *out_rule) {
    if (rule_set == nullptr || out_rule == nullptr || index >= rule_set->accessible_count) {
        return false;
    }
    *out_rule = rule_set->accessible_rules[index];
    return true;
}

size_t fm_get_export_rule_count(const FmRuleSet *rule_set) {
    return rule_set != nullptr ? rule_set->export_count : 0;
}

bool fm_get_export_rule(const FmRuleSet *rule_set, size_t index, FmExportFolderRule *out_rule) {
    if (rule_set == nullptr || out_rule == nullptr || index >= rule_set->export_count) {
        return false;
    }
    *out_rule = rule_set->export_rules[index];
    return true;
}
