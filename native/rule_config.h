#ifndef FOLDER_MANAGER_RULE_CONFIG_H
#define FOLDER_MANAGER_RULE_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define FM_MAX_RULES 128
#define FM_MAX_ACCESSIBLE_RULES 64
#define FM_MAX_EXPORT_RULES 64
#define FM_MAX_PACKAGE_LEN 128
#define FM_MAX_RULE_ID_LEN 128
#define FM_MAX_PATH_LEN 1024
#define FM_MAX_TEXT_LEN 256

enum FmRuleType {
    FM_RULE_INVALID = 0,
    FM_RULE_ALLOW = 1,
    FM_RULE_BLOCK = 2,
    FM_RULE_REDIRECT = 3,
    FM_RULE_REDIRECT_DYNAMIC = 4,
};

enum FmRuleMode {
    FM_RULE_MODE_BLACKLIST = 0,
    FM_RULE_MODE_WHITELIST = 1,
};

enum FmMediaQueryMode {
    FM_MEDIA_QUERY_AUTO = 0,
    FM_MEDIA_QUERY_ENABLE = 1,
    FM_MEDIA_QUERY_DISABLE = 2,
};

enum FmRulePathKind {
    FM_RULE_PATH_AUTO = 0,
    FM_RULE_PATH_FILE = 1,
    FM_RULE_PATH_DIRECTORY = 2,
};

enum FmDecisionType {
    FM_DECISION_ALLOW = 0,
    FM_DECISION_BLOCK = 1,
    FM_DECISION_REDIRECT = 2,
};

enum FmPathOperation {
    FM_PATH_OP_UNKNOWN = 0,
    FM_PATH_OP_OPEN = 1,
    FM_PATH_OP_OPEN_DIRECTORY = 2,
    FM_PATH_OP_CREATE_FILE = 3,
    FM_PATH_OP_CREATE_DIRECTORY = 4,
    FM_PATH_OP_STAT = 5,
    FM_PATH_OP_ENUMERATE_DIRECTORY = 6,
    FM_PATH_OP_READLINK = 7,
    FM_PATH_OP_REMOVE_FILE = 8,
    FM_PATH_OP_REMOVE_DIRECTORY = 9,
};

struct FmRule {
    FmRuleType type;
    FmRulePathKind path_kind;
    char package_name[FM_MAX_PACKAGE_LEN];
    char target_path[FM_MAX_PATH_LEN];
    char source_path[FM_MAX_PATH_LEN];
};

struct FmAccessibleFolderRule {
    FmRulePathKind path_kind;
    char id[FM_MAX_RULE_ID_LEN];
    char anchor_package[FM_MAX_PACKAGE_LEN];
    char from_package[FM_MAX_PACKAGE_LEN];
    char to_package[FM_MAX_PACKAGE_LEN];
    char path[FM_MAX_PATH_LEN];
    char description[FM_MAX_TEXT_LEN];
};

struct FmExportFolderRule {
    char id[FM_MAX_RULE_ID_LEN];
    char package_name[FM_MAX_PACKAGE_LEN];
    char source_path[FM_MAX_PATH_LEN];
    char target_path[FM_MAX_PATH_LEN];
    char title[FM_MAX_TEXT_LEN];
    bool media_scan;
    bool add_to_downloads;
    bool allow_child;
    char description[FM_MAX_TEXT_LEN];
};

struct FmRuleSet {
    FmRule rules[FM_MAX_RULES];
    size_t count;
    FmAccessibleFolderRule accessible_rules[FM_MAX_ACCESSIBLE_RULES];
    size_t accessible_count;
    FmExportFolderRule export_rules[FM_MAX_EXPORT_RULES];
    size_t export_count;
    FmRuleMode mode;
    FmMediaQueryMode media_query_mode;
    void *engine_state;
};

struct FmRuntimeContext {
    FmPathOperation operation;
    int open_flags;
    bool already_redirected;
};

struct FmPathDecision {
    FmDecisionType type;
    char redirected_path[FM_MAX_PATH_LEN];
};

void fm_reset_rule_set(FmRuleSet *rule_set);
bool fm_load_rules_from_module_dir_fd(int module_dir_fd, const char *process_name, FmRuleSet *out_rule_set);
bool fm_load_rules_from_text(const char *text, const char *process_name, FmRuleSet *out_rule_set);
bool fm_has_matching_rule(const FmRuleSet *rule_set);
void fm_init_runtime_context(FmRuntimeContext *context);
void fm_init_path_decision(FmPathDecision *decision);
bool fm_decide_path(FmRuleSet *rule_set, const char *input_path, FmPathDecision *decision);
bool fm_decide_path_with_context(FmRuleSet *rule_set, const char *input_path, const FmRuntimeContext *context, FmPathDecision *decision);
bool fm_should_hide_dir_entry(FmRuleSet *rule_set, const char *dir_path, const char *entry_name);
bool fm_get_rule_hit_count(const FmRuleSet *rule_set, size_t index, uint64_t *out_count);
void fm_invalidate_path_kind(FmRuleSet *rule_set, const char *path);

size_t fm_get_accessible_rule_count(const FmRuleSet *rule_set);
bool fm_get_accessible_rule(const FmRuleSet *rule_set, size_t index, FmAccessibleFolderRule *out_rule);
size_t fm_get_export_rule_count(const FmRuleSet *rule_set);
bool fm_get_export_rule(const FmRuleSet *rule_set, size_t index, FmExportFolderRule *out_rule);

#endif
