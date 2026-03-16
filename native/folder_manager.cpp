#include <android/log.h>
#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <android/dlext.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <linux/stat.h>
#include <stdint.h>
#include <mutex>
#include <string>
#include <vector>
#include <shared_mutex>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

#include "path_mapper.h"
#include "rule_config.h"
#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

#define FM_LOG_TAG "FolderManager"
#define FM_MAX_HOOK_TARGETS 512

#define FM_LOGI(...) __android_log_print(ANDROID_LOG_INFO, FM_LOG_TAG, __VA_ARGS__)
#define FM_LOGW(...) __android_log_print(ANDROID_LOG_WARN, FM_LOG_TAG, __VA_ARGS__)
#define FM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FM_LOG_TAG, __VA_ARGS__)

struct FmElfIdentity {
    dev_t dev;
    ino_t inode;
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static FmRuleSet g_rule_set;
static std::shared_mutex g_rule_mutex;
static std::atomic<bool> g_should_hook{false};
static std::atomic<bool> g_reload_thread_running{false};
static pthread_t g_reload_thread;
static int g_module_dir_fd = -1;
static std::string g_process_name;
static std::atomic<uint64_t> g_rules_mtime_ns{0};
static std::atomic<size_t> g_rule_count{0};
static bool g_enable_media_query_hook = false;
static bool g_enable_provider_observe = true;
static std::atomic<bool> g_lsposed_gate_active{false};
static char g_lsposed_marker_path[FM_MAX_PATH_LEN] = {0};
static Api *g_api = nullptr;
static std::mutex g_hook_mutex;
static std::vector<FmElfIdentity> g_hooked_identities;

static int (*orig_open)(const char *, int, ... ) = nullptr;
static int (*orig_open64)(const char *, int, ... ) = nullptr;
static jboolean (*orig_transact_native)(JNIEnv *, jobject, jint, jobject, jobject, jint) = nullptr;
static int (*orig___open_2)(const char *, int) = nullptr;
static int (*orig_openat)(int, const char *, int, ... ) = nullptr;
static int (*orig_openat64)(int, const char *, int, ... ) = nullptr;
static int (*orig___openat_2)(int, const char *, int) = nullptr;
static int (*orig_mkdir)(const char *, mode_t) = nullptr;
static int (*orig_mkdirat)(int, const char *, mode_t) = nullptr;
static FILE *(*orig_fopen)(const char *, const char *) = nullptr;
static FILE *(*orig_fopen64)(const char *, const char *) = nullptr;
static int (*orig_access)(const char *, int) = nullptr;
static int (*orig_faccessat)(int, const char *, int, int) = nullptr;
static int (*orig_unlink)(const char *) = nullptr;
static int (*orig_unlinkat)(int, const char *, int) = nullptr;
static int (*orig_rmdir)(const char *) = nullptr;
static int (*orig_stat)(const char *, struct stat *) = nullptr;
static int (*orig_lstat)(const char *, struct stat *) = nullptr;
static int (*orig_stat64)(const char *, struct stat64 *) = nullptr;
static int (*orig_lstat64)(const char *, struct stat64 *) = nullptr;
static int (*orig_fstatat)(int, const char *, struct stat *, int) = nullptr;
static int (*orig_fstatat64)(int, const char *, struct stat64 *, int) = nullptr;
static int (*orig_statx)(int, const char *, int, unsigned int, struct statx *) = nullptr;
static DIR *(*orig_opendir)(const char *) = nullptr;
static struct dirent *(*orig_readdir)(DIR *) = nullptr;
static int (*orig_getdents64)(unsigned int, struct linux_dirent64 *, unsigned int) = nullptr;
static int (*orig_scandir)(const char *, struct dirent***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **)) = nullptr;
static int (*orig_scandirat)(int, const char *, struct dirent***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **)) = nullptr;
static ssize_t (*orig_readlink)(const char *, char *, size_t) = nullptr;
static ssize_t (*orig_readlinkat)(int, const char *, char *, size_t) = nullptr;
static char *(*orig_realpath)(const char *, char *) = nullptr;
static int (*orig_rename)(const char *, const char *) = nullptr;
static int (*orig_renameat)(int, const char *, int, const char *) = nullptr;
static int (*orig_renameat2)(int, const char *, int, const char *, unsigned int) = nullptr;
static int (*orig_link)(const char *, const char *) = nullptr;
static int (*orig_linkat)(int, const char *, int, const char *, int) = nullptr;
static int (*orig_symlink)(const char *, const char *) = nullptr;
static int (*orig_symlinkat)(const char *, int, const char *) = nullptr;
static void *(*orig_dlopen)(const char *, int) = nullptr;
static void *(*orig_android_dlopen_ext)(const char *, int, const android_dlextinfo *) = nullptr;

static unsigned long long g_hit_total = 0;
static unsigned long long g_hit_block = 0;
static unsigned long long g_hit_redirect = 0;

static jint g_query_transaction = -1;
static jint g_insert_transaction = -1;
static jint g_delete_transaction = -1;
static jint g_bulk_insert_transaction = -1;
static jint g_open_file_transaction = -1;
static jint g_open_asset_file_transaction = -1;
static jint g_open_typed_asset_file_transaction = -1;
static jclass g_cls_string = nullptr;
static jobject g_uri_creator = nullptr;
static jobject g_attribution_source_creator = nullptr;

static jmethodID g_mid_binderproxy_get_interface_descriptor = nullptr;
static jmethodID g_mid_parcel_obtain = nullptr;
static jmethodID g_mid_parcel_recycle = nullptr;
static jmethodID g_mid_parcel_data_position = nullptr;
static jmethodID g_mid_parcel_set_data_position = nullptr;
static jmethodID g_mid_parcel_enforce_interface = nullptr;
static jmethodID g_mid_parcel_write_interface_token = nullptr;
static jmethodID g_mid_parcel_read_string = nullptr;
static jmethodID g_mid_parcel_write_string = nullptr;
static jmethodID g_mid_parcel_read_int = nullptr;
static jmethodID g_mid_parcel_write_int = nullptr;
static jmethodID g_mid_parcel_create_string_array = nullptr;
static jmethodID g_mid_parcel_write_string_array = nullptr;
static jmethodID g_mid_parcel_read_bundle = nullptr;
static jmethodID g_mid_parcel_write_bundle = nullptr;
static jmethodID g_mid_parcel_read_strong_binder = nullptr;
static jmethodID g_mid_parcel_write_strong_binder = nullptr;
static jmethodID g_mid_parcel_marshall = nullptr;
static jmethodID g_mid_parcel_unmarshall = nullptr;

static jmethodID g_mid_uri_write_to_parcel = nullptr;
static jmethodID g_mid_uri_get_authority = nullptr;
static jmethodID g_mid_uri_to_string = nullptr;
static jmethodID g_mid_creator_create_from_parcel = nullptr;
static jmethodID g_mid_attribution_source_get_package_name = nullptr;
static jmethodID g_mid_attribution_source_get_attribution_tag = nullptr;

static jmethodID g_mid_bundle_ctor = nullptr;
static jmethodID g_mid_bundle_get_string = nullptr;
static jmethodID g_mid_bundle_put_string = nullptr;
static jmethodID g_mid_bundle_get_string_array = nullptr;
static jmethodID g_mid_bundle_put_string_array = nullptr;

static jstring g_jstr_query_arg_sql_selection = nullptr;
static jstring g_jstr_query_arg_sql_selection_args = nullptr;
static bool fm_should_enable_media_query_hook(const FmRuleSet *rule_set) {
    if (rule_set == nullptr) {
        return false;
    }
    if (rule_set->media_query_mode == FM_MEDIA_QUERY_ENABLE) {
        return true;
    }
    if (rule_set->media_query_mode == FM_MEDIA_QUERY_DISABLE) {
        return false;
    }
    if (rule_set->mode == FM_RULE_MODE_WHITELIST) {
        return true;
    }
    for (size_t index = 0; index < rule_set->count; ++index) {
        if (rule_set->rules[index].type == FM_RULE_BLOCK) {
            return true;
        }
    }
    return false;
}

static bool fm_clear_jni_exception(JNIEnv *env, const char *stage) {
    if (env == nullptr || !env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    FM_LOGW("provider observe skipped, jni exception at %s", stage != nullptr ? stage : "(unknown)");
    return true;
}

static jclass fm_find_class_optional(JNIEnv *env, const char *class_name) {
    jclass cls = env->FindClass(class_name);
    if (cls == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return cls;
}

static jmethodID fm_get_method_id_optional(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (env == nullptr || cls == nullptr) {
        return nullptr;
    }
    jmethodID method = env->GetMethodID(cls, name, signature);
    if (method == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return method;
}

static jfieldID fm_get_static_field_id_optional(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (env == nullptr || cls == nullptr) {
        return nullptr;
    }
    jfieldID field = env->GetStaticFieldID(cls, name, signature);
    if (field == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return field;
}

static jint fm_get_static_int_field_or_default(JNIEnv *env, jclass cls, const char *name, jint fallback) {
    jfieldID field = fm_get_static_field_id_optional(env, cls, name, "I");
    return field != nullptr ? env->GetStaticIntField(cls, field) : fallback;
}

static bool fm_copy_jstring(JNIEnv *env, jstring value, char *output, size_t output_size) {
    if (output == nullptr || output_size == 0) {
        return false;
    }
    output[0] = '\0';
    if (env == nullptr || value == nullptr) {
        return true;
    }
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return false;
    }
    strncpy(output, chars, output_size - 1);
    output[output_size - 1] = '\0';
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

static const char *fm_provider_operation_name(jint code) {
    if (code == g_query_transaction) return "query";
    if (code == g_insert_transaction) return "insert";
    if (code == g_delete_transaction) return "delete";
    if (code == g_bulk_insert_transaction) return "bulkInsert";
    if (code == g_open_file_transaction) return "openFile";
    if (code == g_open_asset_file_transaction) return "openAssetFile";
    if (code == g_open_typed_asset_file_transaction) return "openTypedAssetFile";
    return "unknown";
}

static bool fm_should_observe_provider_transaction(jint code) {
    return code == g_query_transaction || code == g_insert_transaction || code == g_delete_transaction || code == g_bulk_insert_transaction || code == g_open_file_transaction || code == g_open_asset_file_transaction || code == g_open_typed_asset_file_transaction;
}

static bool fm_should_observe_provider_authority(const char *authority) {
    return authority != nullptr && (strcmp(authority, "media") == 0 || strcmp(authority, "downloads") == 0);
}

static bool fm_read_provider_caller(JNIEnv *env, jobject parcel, char *calling_pkg, size_t calling_pkg_size, char *attribution_tag, size_t attribution_tag_size) {
    if (calling_pkg == nullptr || calling_pkg_size == 0 || attribution_tag == nullptr || attribution_tag_size == 0) {
        return false;
    }
    calling_pkg[0] = '\0';
    attribution_tag[0] = '\0';

    if (g_attribution_source_creator != nullptr && g_mid_attribution_source_get_package_name != nullptr) {
        jobject attribution_source = env->CallObjectMethod(g_attribution_source_creator, g_mid_creator_create_from_parcel, parcel);
        if (fm_clear_jni_exception(env, "read attribution source") || attribution_source == nullptr) {
            if (attribution_source != nullptr) env->DeleteLocalRef(attribution_source);
            return false;
        }
        jstring package_name = static_cast<jstring>(env->CallObjectMethod(attribution_source, g_mid_attribution_source_get_package_name));
        if (!fm_clear_jni_exception(env, "get attribution package") && package_name != nullptr) {
            fm_copy_jstring(env, package_name, calling_pkg, calling_pkg_size);
            env->DeleteLocalRef(package_name);
        }
        if (g_mid_attribution_source_get_attribution_tag != nullptr) {
            jstring feature_id = static_cast<jstring>(env->CallObjectMethod(attribution_source, g_mid_attribution_source_get_attribution_tag));
            if (!fm_clear_jni_exception(env, "get attribution tag") && feature_id != nullptr) {
                fm_copy_jstring(env, feature_id, attribution_tag, attribution_tag_size);
                env->DeleteLocalRef(feature_id);
            }
        }
        env->DeleteLocalRef(attribution_source);
        return true;
    }

    jstring calling_pkg_j = static_cast<jstring>(env->CallObjectMethod(parcel, g_mid_parcel_read_string));
    if (fm_clear_jni_exception(env, "read calling package")) return false;
    if (calling_pkg_j != nullptr) { fm_copy_jstring(env, calling_pkg_j, calling_pkg, calling_pkg_size); env->DeleteLocalRef(calling_pkg_j); }
    jstring attribution_tag_j = static_cast<jstring>(env->CallObjectMethod(parcel, g_mid_parcel_read_string));
    if (fm_clear_jni_exception(env, "read attribution tag")) return false;
    if (attribution_tag_j != nullptr) { fm_copy_jstring(env, attribution_tag_j, attribution_tag, attribution_tag_size); env->DeleteLocalRef(attribution_tag_j); }
    return true;
}

static void fm_observe_content_provider_transact(JNIEnv *env, jstring descriptor, jint code, jobject data) {
    if (!g_enable_provider_observe || data == nullptr || !fm_should_observe_provider_transaction(code)) {
        return;
    }

    jint original_position = env->CallIntMethod(data, g_mid_parcel_data_position);
    if (fm_clear_jni_exception(env, "get data position")) return;
    env->CallVoidMethod(data, g_mid_parcel_set_data_position, 0);
    if (fm_clear_jni_exception(env, "reset data position")) return;
    env->CallVoidMethod(data, g_mid_parcel_enforce_interface, descriptor);
    if (fm_clear_jni_exception(env, "enforce provider descriptor")) { env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position); return; }

    char calling_pkg[FM_MAX_PACKAGE_LEN] = {0};
    char attribution_tag[FM_MAX_TEXT_LEN] = {0};
    if (!fm_read_provider_caller(env, data, calling_pkg, sizeof(calling_pkg), attribution_tag, sizeof(attribution_tag))) { env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position); return; }

    jobject uri = env->CallObjectMethod(g_uri_creator, g_mid_creator_create_from_parcel, data);
    if (fm_clear_jni_exception(env, "read provider uri") || uri == nullptr) { if (uri != nullptr) env->DeleteLocalRef(uri); env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position); return; }

    char authority[64] = {0};
    char uri_text[FM_MAX_PATH_LEN] = {0};
    jstring authority_j = static_cast<jstring>(env->CallObjectMethod(uri, g_mid_uri_get_authority));
    if (!fm_clear_jni_exception(env, "uri.getAuthority") && authority_j != nullptr) { fm_copy_jstring(env, authority_j, authority, sizeof(authority)); env->DeleteLocalRef(authority_j); }
    jstring uri_string_j = static_cast<jstring>(env->CallObjectMethod(uri, g_mid_uri_to_string));
    if (!fm_clear_jni_exception(env, "uri.toString") && uri_string_j != nullptr) { fm_copy_jstring(env, uri_string_j, uri_text, sizeof(uri_text)); env->DeleteLocalRef(uri_string_j); }

    if (fm_should_observe_provider_authority(authority)) {
        FM_LOGI("provider observe op=%s authority=%s pkg=%s attr=%s uri=%s", fm_provider_operation_name(code), authority, calling_pkg[0] != '\0' ? calling_pkg : "(null)", attribution_tag[0] != '\0' ? attribution_tag : "(null)", uri_text[0] != '\0' ? uri_text : "(null)");
    }

    env->DeleteLocalRef(uri);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
    fm_clear_jni_exception(env, "restore data position");
}
static bool fm_apply_path_decision_with_context(const char *input_path, const FmRuntimeContext *context, FmPathDecision *decision) {
    if (!g_should_hook.load(std::memory_order_acquire)) {
        return false;
    }
    if (!g_lsposed_gate_active.load(std::memory_order_acquire)) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(g_rule_mutex);
    return fm_decide_path_with_context(&g_rule_set, input_path, context, decision);
}

static bool fm_apply_path_decision(const char *input_path, FmPathDecision *decision) {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    return fm_apply_path_decision_with_context(input_path, &context, decision);
}

static FmRuntimeContext fm_make_open_context(int flags) {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.open_flags = flags;
    if ((flags & O_DIRECTORY) != 0) {
        context.operation = FM_PATH_OP_OPEN_DIRECTORY;
    } else if ((flags & O_CREAT) != 0) {
        context.operation = FM_PATH_OP_CREATE_FILE;
    } else {
        context.operation = FM_PATH_OP_OPEN;
    }
    return context;
}

static FmRuntimeContext fm_make_fopen_context(const char *mode) {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_OPEN;
    if (mode != nullptr && (strchr(mode, 'w') != nullptr || strchr(mode, 'a') != nullptr || strchr(mode, '+') != nullptr)) {
        context.operation = FM_PATH_OP_CREATE_FILE;
    }
    return context;
}

static FmRuntimeContext fm_make_stat_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_STAT;
    return context;
}

static FmRuntimeContext fm_make_dir_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_OPEN_DIRECTORY;
    return context;
}

static FmRuntimeContext fm_make_enum_dir_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_ENUMERATE_DIRECTORY;
    return context;
}

static FmRuntimeContext fm_make_readlink_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_READLINK;
    return context;
}

static FmRuntimeContext fm_make_remove_file_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_REMOVE_FILE;
    return context;
}

static FmRuntimeContext fm_make_remove_dir_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_REMOVE_DIRECTORY;
    return context;
}

static FmRuntimeContext fm_make_create_dir_context() {
    FmRuntimeContext context;
    fm_init_runtime_context(&context);
    context.operation = FM_PATH_OP_CREATE_DIRECTORY;
    return context;
}

static bool fm_should_block(const FmPathDecision *decision) {
    return decision != nullptr && decision->type == FM_DECISION_BLOCK;
}

static bool fm_should_redirect(const FmPathDecision *decision) {
    return decision != nullptr && decision->type == FM_DECISION_REDIRECT && decision->redirected_path[0] != '\0';
}


static void fm_log_path_hit(const char *symbol_name, const char *input_path, const FmPathDecision *decision);

static bool fm_resolve_fd_path(int fd, char *output_path, size_t output_size) {
    if (fd < 0 || output_path == nullptr || output_size < 2) {
        return false;
    }

    char proc_path[64] = {0};
    int proc_length = snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    if (proc_length <= 0 || static_cast<size_t>(proc_length) >= sizeof(proc_path)) {
        return false;
    }

    ssize_t path_length = syscall(__NR_readlinkat, AT_FDCWD, proc_path, output_path, output_size - 1);
    if (path_length <= 0 || static_cast<size_t>(path_length) >= output_size) {
        return false;
    }

    output_path[path_length] = '\0';
    return fm_normalize_path(output_path, output_path, output_size);
}

static bool fm_resolve_at_path(int dirfd, const char *path, char *output_path, size_t output_size) {
    if (path == nullptr || output_path == nullptr || output_size == 0) {
        return false;
    }
    if (path[0] == '/') {
        return fm_normalize_path(path, output_path, output_size);
    }
    if (dirfd == AT_FDCWD) {
        return fm_normalize_path(path, output_path, output_size);
    }
    char base_path[FM_MAX_PATH_LEN] = {0};
    if (!fm_resolve_fd_path(dirfd, base_path, sizeof(base_path))) {
        return false;
    }
    return fm_join_path(base_path, path, output_path, output_size);
}

static bool fm_try_resolve_at_path(int dirfd, const char *path, char *output_path, size_t output_size) {
    if (path == nullptr || output_path == nullptr || output_size == 0) {
        return false;
    }
    if (path[0] == '/') {
        return fm_normalize_path(path, output_path, output_size);
    }
    if (dirfd == AT_FDCWD) {
        return fm_normalize_path(path, output_path, output_size);
    }
    if (!fm_resolve_fd_path(dirfd, output_path, output_size)) {
        return false;
    }
    return fm_join_path(output_path, path, output_path, output_size);
}

static bool fm_should_block_rename_decision(int src_decision, int dst_decision) {
    return src_decision == FM_DECISION_BLOCK || dst_decision == FM_DECISION_BLOCK;
}

static bool fm_read_lsposed_gate_flag() {
    if (g_lsposed_marker_path[0] == '\0') {
        return true;
    }
    struct stat st = {};
    return stat(g_lsposed_marker_path, &st) == 0;
}

static void fm_update_lsposed_gate() {
    if (g_lsposed_marker_path[0] == '\0') {
        g_lsposed_gate_active.store(true, std::memory_order_release);
        return;
    }
    bool active = fm_read_lsposed_gate_flag();
    g_lsposed_gate_active.store(active, std::memory_order_release);
}


static bool fm_is_duplicate_identity(const FmElfIdentity *identities, size_t count, dev_t dev, ino_t inode);
static size_t fm_collect_hook_targets(FmElfIdentity *identities, size_t capacity);

static void fm_track_hook_identity(const FmElfIdentity *identity) {
    if (identity == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (g_hooked_identities.size() >= FM_MAX_HOOK_TARGETS) {
        return;
    }
    if (fm_is_duplicate_identity(g_hooked_identities.data(), g_hooked_identities.size(), identity->dev, identity->inode)) {
        return;
    }
    g_hooked_identities.push_back(*identity);
}

static size_t fm_collect_new_hook_targets(FmElfIdentity *identities, size_t capacity) {
    FmElfIdentity candidates[FM_MAX_HOOK_TARGETS];
    memset(candidates, 0, sizeof(candidates));
    size_t total = fm_collect_hook_targets(candidates, FM_MAX_HOOK_TARGETS);
    if (total == 0) {
        return 0;
    }

    size_t out_count = 0;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    for (size_t index = 0; index < total && out_count < capacity; ++index) {
        const FmElfIdentity &cand = candidates[index];
        if (fm_is_duplicate_identity(g_hooked_identities.data(), g_hooked_identities.size(), cand.dev, cand.inode)) {
            continue;
        }
        identities[out_count++] = cand;
    }
    return out_count;
}

static void fm_register_hooks_for_identity(Api *api, const FmElfIdentity *identity);

static bool fm_install_incremental_plt_hooks(Api *api) {
    if (api == nullptr) {
        return false;
    }

    FmElfIdentity identities[FM_MAX_HOOK_TARGETS];
    memset(identities, 0, sizeof(identities));

    size_t identity_count = fm_collect_new_hook_targets(identities, FM_MAX_HOOK_TARGETS);
    if (identity_count == 0) {
        return false;
    }

    for (size_t index = 0; index < identity_count; ++index) {
        fm_register_hooks_for_identity(api, &identities[index]);
    }

    bool committed = api->pltHookCommit();
    if (committed) {
        for (size_t index = 0; index < identity_count; ++index) {
            fm_track_hook_identity(&identities[index]);
        }
    }
    return committed;
}

static void *my_dlopen(const char *filename, int flag) {
    if (orig_dlopen == nullptr) {
        return nullptr;
    }
    void *handle = orig_dlopen(filename, flag);
    if (handle != nullptr && g_api != nullptr) {
        fm_install_incremental_plt_hooks(g_api);
    }
    return handle;
}

static void *my_android_dlopen_ext(const char *filename, int flag, const android_dlextinfo *extinfo) {
    if (orig_android_dlopen_ext == nullptr) {
        return nullptr;
    }
    void *handle = orig_android_dlopen_ext(filename, flag, extinfo);
    if (handle != nullptr && g_api != nullptr) {
        fm_install_incremental_plt_hooks(g_api);
    }
    return handle;
}

static void fm_invalidate_cache_path(const char *path) {
    if (path == nullptr || !g_should_hook.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock<std::shared_mutex> lock(g_rule_mutex);
    fm_invalidate_path_kind(&g_rule_set, path);
}

static void fm_log_blocked_dir_entry(const char *symbol_name, const char *dir_path, const char *entry_name) {
    char child_path[FM_MAX_PATH_LEN] = {0};
    FmPathDecision decision;

    if (symbol_name == nullptr || dir_path == nullptr || entry_name == nullptr) {
        return;
    }
    if (!fm_join_path(dir_path, entry_name, child_path, sizeof(child_path))) {
        return;
    }

    fm_init_path_decision(&decision);
    decision.type = FM_DECISION_BLOCK;
    fm_log_path_hit(symbol_name, child_path, &decision);
}

static bool fm_should_hide_dirent_from_dir(const char *dir_path, const char *entry_name) {
    if (!g_should_hook.load(std::memory_order_acquire) || dir_path == nullptr || entry_name == nullptr) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(g_rule_mutex);
    return fm_should_hide_dir_entry(&g_rule_set, dir_path, entry_name);
}

static void fm_log_path_hit(const char *symbol_name, const char *input_path, const FmPathDecision *decision) {
    if (symbol_name == nullptr || input_path == nullptr || decision == nullptr) {
        return;
    }

    if (fm_should_block(decision)) {
        unsigned long long total = __sync_add_and_fetch(&g_hit_total, 1);
        unsigned long long blocked = __sync_add_and_fetch(&g_hit_block, 1);
        unsigned long long redirected = __sync_fetch_and_add(&g_hit_redirect, 0);
        FM_LOGI("hit symbol=%s action=block path=%s total=%llu block=%llu redirect=%llu rules=%zu", symbol_name, input_path, total, blocked, redirected, g_rule_count.load(std::memory_order_relaxed));
        return;
    }

    if (fm_should_redirect(decision)) {
        unsigned long long total = __sync_add_and_fetch(&g_hit_total, 1);
        unsigned long long blocked = __sync_fetch_and_add(&g_hit_block, 0);
        unsigned long long redirected = __sync_add_and_fetch(&g_hit_redirect, 1);
        FM_LOGI("hit symbol=%s action=redirect from=%s to=%s total=%llu block=%llu redirect=%llu rules=%zu", symbol_name, input_path, decision->redirected_path, total, blocked, redirected, g_rule_count.load(std::memory_order_relaxed));
    }
}

static uint64_t fm_timespec_to_ns(const struct timespec *ts) {
    if (ts == nullptr) {
        return 0;
    }
    return static_cast<uint64_t>(ts->tv_sec) * 1000000000ull + static_cast<uint64_t>(ts->tv_nsec);
}

static bool fm_read_rules_mtime_ns(int dir_fd, uint64_t *out_mtime_ns) {
    if (dir_fd < 0 || out_mtime_ns == nullptr) {
        return false;
    }
    struct stat st = {};
    if (fstatat(dir_fd, "config/rules.ini", &st, 0) != 0) {
        return false;
    }
#if defined(__APPLE__)
    *out_mtime_ns = fm_timespec_to_ns(&st.st_mtimespec);
#else
    *out_mtime_ns = fm_timespec_to_ns(&st.st_mtim);
#endif
    return true;
}

static bool fm_reload_rules_locked() {
    if (g_module_dir_fd < 0 || g_process_name.empty()) {
        return false;
    }

    FmRuleSet new_rule_set;
    bool loaded = fm_load_rules_from_module_dir_fd(g_module_dir_fd, g_process_name.c_str(), &new_rule_set);
    if (!loaded) {
        return false;
    }

    fm_reset_rule_set(&g_rule_set);
    g_rule_set = new_rule_set;
    return true;
}

static void *fm_reload_thread_main(void *) {
    while (g_reload_thread_running.load(std::memory_order_acquire)) {
        uint64_t mtime_ns = 0;
        if (fm_read_rules_mtime_ns(g_module_dir_fd, &mtime_ns)) {
            uint64_t last = g_rules_mtime_ns.load(std::memory_order_relaxed);
            if (mtime_ns != 0 && mtime_ns != last) {
                std::unique_lock<std::shared_mutex> lock(g_rule_mutex);
                if (fm_reload_rules_locked()) {
                    g_rules_mtime_ns.store(mtime_ns, std::memory_order_release);
                    g_should_hook.store(fm_has_matching_rule(&g_rule_set), std::memory_order_release);
                    g_rule_count.store(g_rule_set.count, std::memory_order_release);
                    g_enable_media_query_hook = fm_should_enable_media_query_hook(&g_rule_set);
                    fm_update_lsposed_gate();
                    FM_LOGI("rules reloaded, count=%zu", g_rule_set.count);
                }
            }
        }
        sleep(1);
    }
    return nullptr;
}

static void fm_start_reload_thread_if_needed() {
    if (g_module_dir_fd < 0) {
        return;
    }
    bool expected = false;
    if (!g_reload_thread_running.compare_exchange_strong(expected, true)) {
        return;
    }
    int result = pthread_create(&g_reload_thread, nullptr, fm_reload_thread_main, nullptr);
    if (result != 0) {
        g_reload_thread_running.store(false);
        return;
    }
    pthread_detach(g_reload_thread);
}

static bool fm_jni_ready() {
    return orig_transact_native != nullptr
        && g_mid_binderproxy_get_interface_descriptor != nullptr
        && g_mid_parcel_obtain != nullptr
        && g_mid_parcel_recycle != nullptr
        && g_mid_parcel_data_position != nullptr
        && g_mid_parcel_set_data_position != nullptr
        && g_mid_parcel_enforce_interface != nullptr
        && g_mid_parcel_write_interface_token != nullptr
        && g_mid_parcel_read_string != nullptr
        && g_mid_parcel_write_string != nullptr
        && g_mid_parcel_read_int != nullptr
        && g_mid_parcel_write_int != nullptr
        && g_mid_parcel_create_string_array != nullptr
        && g_mid_parcel_write_string_array != nullptr
        && g_mid_parcel_read_bundle != nullptr
        && g_mid_parcel_write_bundle != nullptr
        && g_mid_parcel_read_strong_binder != nullptr
        && g_mid_parcel_write_strong_binder != nullptr
        && g_mid_parcel_marshall != nullptr
        && g_mid_parcel_unmarshall != nullptr
        && g_mid_uri_write_to_parcel != nullptr
        && g_mid_uri_get_authority != nullptr
        && g_mid_uri_to_string != nullptr
        && g_mid_creator_create_from_parcel != nullptr
        && g_mid_bundle_ctor != nullptr
        && g_mid_bundle_get_string != nullptr
        && g_mid_bundle_put_string != nullptr
        && g_mid_bundle_get_string_array != nullptr
        && g_mid_bundle_put_string_array != nullptr
        && g_jstr_query_arg_sql_selection != nullptr
        && g_jstr_query_arg_sql_selection_args != nullptr
        && g_uri_creator != nullptr
        && g_cls_string != nullptr
        && g_query_transaction >= 0;
}
static bool fm_path_to_relative_pattern(const char *target_path, bool exact, char *output, size_t output_size) {
    char relative_path[FM_MAX_PATH_LEN] = {0};

    if (target_path == nullptr || output == nullptr || output_size == 0) {
        return false;
    }
    if (!fm_get_relative_external_path(target_path, relative_path, sizeof(relative_path))) {
        return false;
    }

    return snprintf(output, output_size, exact ? "%s" : "%s/%%", relative_path) > 0;
}

static bool fm_path_to_data_pattern(const char *target_path, bool exact, char *output, size_t output_size) {
    char normalized_path[FM_MAX_PATH_LEN] = {0};

    if (target_path == nullptr || output == nullptr || output_size == 0) {
        return false;
    }
    if (!fm_normalize_path(target_path, normalized_path, sizeof(normalized_path))) {
        return false;
    }

    return snprintf(output, output_size, exact ? "%s" : "%s/%%", normalized_path) > 0;
}

static bool fm_append_selection_text(char *selection, size_t selection_size, const char *text) {
    if (selection == nullptr || text == nullptr || selection_size == 0) {
        return false;
    }
    size_t current_length = strlen(selection);
    size_t add_length = strlen(text);
    if (current_length + add_length + 1 > selection_size) {
        return false;
    }
    memcpy(selection + current_length, text, add_length + 1);
    return true;
}

static bool fm_push_arg(char args[][FM_MAX_PATH_LEN], size_t capacity, size_t *arg_count, const char *value) {
    if (args == nullptr || arg_count == nullptr || value == nullptr || *arg_count >= capacity) {
        return false;
    }
    strncpy(args[*arg_count], value, FM_MAX_PATH_LEN - 1);
    args[*arg_count][FM_MAX_PATH_LEN - 1] = '\0';
    (*arg_count)++;
    return true;
}

static bool fm_append_media_path_clause(char *selection,
                                        size_t selection_size,
                                        char args[][FM_MAX_PATH_LEN],
                                        size_t capacity,
                                        size_t *arg_count,
                                        const char *target_path,
                                        FmRulePathKind path_kind,
                                        bool positive) {
    char relative_pattern[FM_MAX_PATH_LEN] = {0};
    char data_pattern[FM_MAX_PATH_LEN] = {0};
    bool exact = path_kind == FM_RULE_PATH_FILE;
    bool has_relative = target_path != nullptr && fm_path_to_relative_pattern(target_path, exact, relative_pattern, sizeof(relative_pattern));
    bool has_data = target_path != nullptr && fm_path_to_data_pattern(target_path, exact, data_pattern, sizeof(data_pattern));

    if (target_path == nullptr || (!has_relative && !has_data)) {
        return false;
    }

    if (!fm_append_selection_text(selection, selection_size, "(")) {
        return false;
    }

    bool has_part = false;
    if (has_data) {
        if (!fm_append_selection_text(selection, selection_size, positive ? (exact ? "_data = ?" : "_data LIKE ?") : (exact ? "_data != ?" : "_data NOT LIKE ?"))) {
            return false;
        }
        if (!fm_push_arg(args, capacity, arg_count, data_pattern)) {
            return false;
        }
        has_part = true;
    }

    if (has_relative) {
        if (has_part) {
            if (!fm_append_selection_text(selection, selection_size, positive ? " OR " : " AND ")) {
                return false;
            }
        }
        if (!fm_append_selection_text(selection, selection_size, positive ? (exact ? "relative_path = ?" : "relative_path LIKE ?") : (exact ? "relative_path != ?" : "relative_path NOT LIKE ?"))) {
            return false;
        }
        if (!fm_push_arg(args, capacity, arg_count, relative_pattern)) {
            return false;
        }
    }

    return fm_append_selection_text(selection, selection_size, ")");
}

static bool fm_build_media_negative_selection(char *selection, size_t selection_size, char args[][FM_MAX_PATH_LEN], size_t capacity, size_t *arg_count) {
    if (selection == nullptr || args == nullptr || arg_count == nullptr || selection_size == 0) {
        return false;
    }

    selection[0] = '\0';
    *arg_count = 0;

    for (size_t index = 0; index < g_rule_set.count; ++index) {
        const FmRule *rule = &g_rule_set.rules[index];
        if (rule->type != FM_RULE_BLOCK) {
            continue;
        }
        if (selection[0] != '\0' && !fm_append_selection_text(selection, selection_size, " AND ")) {
            return false;
        }
        if (!fm_append_media_path_clause(selection, selection_size, args, capacity, arg_count, rule->target_path, rule->path_kind, false)) {
            return false;
        }
    }

    return selection[0] != '\0';
}

static bool fm_build_media_positive_selection(char *selection, size_t selection_size, char args[][FM_MAX_PATH_LEN], size_t capacity, size_t *arg_count) {
    if (selection == nullptr || args == nullptr || arg_count == nullptr || selection_size == 0) {
        return false;
    }

    selection[0] = '\0';
    *arg_count = 0;

    for (size_t index = 0; index < g_rule_set.count; ++index) {
        const FmRule *rule = &g_rule_set.rules[index];
        if (rule->type != FM_RULE_ALLOW
            && rule->type != FM_RULE_REDIRECT
            && rule->type != FM_RULE_REDIRECT_DYNAMIC) {
            continue;
        }
        if (selection[0] != '\0' && !fm_append_selection_text(selection, selection_size, " OR ")) {
            return false;
        }
        if (!fm_append_media_path_clause(selection, selection_size, args, capacity, arg_count, rule->target_path, rule->path_kind, true)) {
            return false;
        }
    }

    for (size_t index = 0; index < g_rule_set.accessible_count; ++index) {
        const FmAccessibleFolderRule *rule = &g_rule_set.accessible_rules[index];
        if (selection[0] != '\0' && !fm_append_selection_text(selection, selection_size, " OR ")) {
            return false;
        }
        if (!fm_append_media_path_clause(selection, selection_size, args, capacity, arg_count, rule->path, rule->path_kind, true)) {
            return false;
        }
    }

    return selection[0] != '\0';
}

static jobjectArray fm_merge_string_arrays(JNIEnv *env, jobjectArray original_array, char extra_args[][FM_MAX_PATH_LEN], size_t extra_count) {
    jsize original_count = original_array != nullptr ? env->GetArrayLength(original_array) : 0;
    jobjectArray merged = env->NewObjectArray(original_count + static_cast<jsize>(extra_count), g_cls_string, nullptr);
    if (merged == nullptr) {
        return nullptr;
    }

    for (jsize index = 0; index < original_count; ++index) {
        jobject value = env->GetObjectArrayElement(original_array, index);
        env->SetObjectArrayElement(merged, index, value);
        env->DeleteLocalRef(value);
    }

    for (size_t index = 0; index < extra_count; ++index) {
        jstring value = env->NewStringUTF(extra_args[index]);
        env->SetObjectArrayElement(merged, original_count + static_cast<jsize>(index), value);
        env->DeleteLocalRef(value);
    }

    return merged;
}

static jobject fm_build_filtered_query_args(JNIEnv *env, jobject original_bundle) {
    std::shared_lock<std::shared_mutex> lock(g_rule_mutex);
    char allow_selection[8192] = {0};
    char deny_selection[8192] = {0};
    char allow_args[(FM_MAX_RULES + FM_MAX_ACCESSIBLE_RULES) * 4][FM_MAX_PATH_LEN];
    char deny_args[FM_MAX_RULES * 4][FM_MAX_PATH_LEN];
    memset(allow_args, 0, sizeof(allow_args));
    memset(deny_args, 0, sizeof(deny_args));
    size_t allow_arg_count = 0;
    size_t deny_arg_count = 0;

    bool has_allow = fm_build_media_positive_selection(allow_selection, sizeof(allow_selection), allow_args, (FM_MAX_RULES + FM_MAX_ACCESSIBLE_RULES) * 4, &allow_arg_count);
    bool has_deny = fm_build_media_negative_selection(deny_selection, sizeof(deny_selection), deny_args, FM_MAX_RULES * 4, &deny_arg_count);

    char final_selection[12288] = {0};
    char merged_rule_args[(FM_MAX_RULES * 4 + (FM_MAX_RULES + FM_MAX_ACCESSIBLE_RULES) * 4)][FM_MAX_PATH_LEN];
    memset(merged_rule_args, 0, sizeof(merged_rule_args));
    size_t merged_rule_arg_count = 0;

    if (g_rule_set.mode == FM_RULE_MODE_WHITELIST) {
        if (has_allow) {
            strncpy(final_selection, allow_selection, sizeof(final_selection) - 1);
            for (size_t index = 0; index < allow_arg_count; ++index) {
                strncpy(merged_rule_args[merged_rule_arg_count++], allow_args[index], FM_MAX_PATH_LEN - 1);
            }
        } else {
            strncpy(final_selection, "0 = 1", sizeof(final_selection) - 1);
        }

        if (has_deny && final_selection[0] != '\0') {
            char combined[12288] = {0};
            snprintf(combined, sizeof(combined), "(%s) AND (%s)", final_selection, deny_selection);
            strncpy(final_selection, combined, sizeof(final_selection) - 1);
            for (size_t index = 0; index < deny_arg_count; ++index) {
                strncpy(merged_rule_args[merged_rule_arg_count++], deny_args[index], FM_MAX_PATH_LEN - 1);
            }
        }
    } else {
        if (!has_deny) {
            return nullptr;
        }
        strncpy(final_selection, deny_selection, sizeof(final_selection) - 1);
        for (size_t index = 0; index < deny_arg_count; ++index) {
            strncpy(merged_rule_args[merged_rule_arg_count++], deny_args[index], FM_MAX_PATH_LEN - 1);
        }
    }

    if (final_selection[0] == '\0') {
        return nullptr;
    }

    jobject bundle = original_bundle;
    if (bundle == nullptr) {
        bundle = env->NewObject(env->FindClass("android/os/Bundle"), g_mid_bundle_ctor);
    }

    if (bundle == nullptr) {
        return nullptr;
    }

    jstring original_selection_j = static_cast<jstring>(env->CallObjectMethod(bundle, g_mid_bundle_get_string, g_jstr_query_arg_sql_selection));
    jobjectArray original_args = static_cast<jobjectArray>(env->CallObjectMethod(bundle, g_mid_bundle_get_string_array, g_jstr_query_arg_sql_selection_args));

    const char *original_selection = nullptr;
    if (original_selection_j != nullptr) {
        original_selection = env->GetStringUTFChars(original_selection_j, nullptr);
    }

    char merged_selection[12288] = {0};
    if (original_selection != nullptr && original_selection[0] != '\0') {
        snprintf(merged_selection, sizeof(merged_selection), "(%s) AND (%s)", original_selection, final_selection);
    } else {
        strncpy(merged_selection, final_selection, sizeof(merged_selection) - 1);
    }

    jstring merged_selection_j = env->NewStringUTF(merged_selection);
    env->CallVoidMethod(bundle, g_mid_bundle_put_string, g_jstr_query_arg_sql_selection, merged_selection_j);

    jobjectArray merged_args = fm_merge_string_arrays(env, original_args, merged_rule_args, merged_rule_arg_count);
    if (merged_args != nullptr) {
        env->CallVoidMethod(bundle, g_mid_bundle_put_string_array, g_jstr_query_arg_sql_selection_args, merged_args);
        env->DeleteLocalRef(merged_args);
    }

    if (original_selection != nullptr) {
        env->ReleaseStringUTFChars(original_selection_j, original_selection);
    }

    env->DeleteLocalRef(merged_selection_j);
    if (original_selection_j != nullptr) {
        env->DeleteLocalRef(original_selection_j);
    }
    if (original_args != nullptr) {
        env->DeleteLocalRef(original_args);
    }

    return bundle;
}

static jobject fm_build_updated_query_args(JNIEnv *env, jobject original_bundle, jobject bundle) {
    if (bundle == nullptr) {
        return original_bundle;
    }
    if (original_bundle == nullptr) {
        return bundle;
    }
    env->DeleteLocalRef(original_bundle);
    return bundle;
}

static void fm_write_filter_bundle(JNIEnv *env, jobject reply, jobject bundle) {
    if (env == nullptr || reply == nullptr || bundle == nullptr) {
        return;
    }
    env->CallVoidMethod(reply, g_mid_parcel_write_bundle, bundle);
    fm_clear_jni_exception(env, "write bundle");
}

static bool fm_should_filter_media_query(JNIEnv *env, jstring descriptor, jint code, jobject data) {
    if (!g_enable_media_query_hook) {
        return false;
    }
    if (descriptor == nullptr || code != g_query_transaction) {
        return false;
    }
    const char *desc_text = env->GetStringUTFChars(descriptor, nullptr);
    if (desc_text == nullptr) {
        return false;
    }
    bool is_provider = strcmp(desc_text, "android.content.IContentProvider") == 0;
    env->ReleaseStringUTFChars(descriptor, desc_text);
    return is_provider;
}

static jobject fm_read_query_args(JNIEnv *env, jobject data) {
    if (env == nullptr || data == nullptr) {
        return nullptr;
    }
    jint original_position = env->CallIntMethod(data, g_mid_parcel_data_position);
    if (fm_clear_jni_exception(env, "get data position")) {
        return nullptr;
    }
    env->CallVoidMethod(data, g_mid_parcel_set_data_position, 0);
    if (fm_clear_jni_exception(env, "reset data position")) {
        return nullptr;
    }
    env->CallVoidMethod(data, g_mid_parcel_enforce_interface, env->NewStringUTF("android.content.IContentProvider"));
    if (fm_clear_jni_exception(env, "enforce interface")) {
        return nullptr;
    }

    jobject uri = env->CallObjectMethod(g_uri_creator, g_mid_creator_create_from_parcel, data);
    if (fm_clear_jni_exception(env, "read query uri") || uri == nullptr) {
        env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
        return nullptr;
    }

    jobjectArray projection = static_cast<jobjectArray>(env->CallObjectMethod(data, g_mid_parcel_create_string_array));
    if (fm_clear_jni_exception(env, "read projection")) {
        env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
        env->DeleteLocalRef(uri);
        return nullptr;
    }

    jstring selection = static_cast<jstring>(env->CallObjectMethod(data, g_mid_parcel_read_string));
    if (fm_clear_jni_exception(env, "read selection")) {
        env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
        if (projection != nullptr) env->DeleteLocalRef(projection);
        env->DeleteLocalRef(uri);
        return nullptr;
    }

    jobjectArray selection_args = static_cast<jobjectArray>(env->CallObjectMethod(data, g_mid_parcel_create_string_array));
    if (fm_clear_jni_exception(env, "read selection args")) {
        env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
        if (projection != nullptr) env->DeleteLocalRef(projection);
        if (selection != nullptr) env->DeleteLocalRef(selection);
        env->DeleteLocalRef(uri);
        return nullptr;
    }

    jstring sort_order = static_cast<jstring>(env->CallObjectMethod(data, g_mid_parcel_read_string));
    if (fm_clear_jni_exception(env, "read sort order")) {
        env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
        if (projection != nullptr) env->DeleteLocalRef(projection);
        if (selection != nullptr) env->DeleteLocalRef(selection);
        if (selection_args != nullptr) env->DeleteLocalRef(selection_args);
        env->DeleteLocalRef(uri);
        return nullptr;
    }

    jobject bundle = env->CallObjectMethod(data, g_mid_parcel_read_bundle, g_cls_string);
    if (fm_clear_jni_exception(env, "read query bundle")) {
        env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
        if (projection != nullptr) env->DeleteLocalRef(projection);
        if (selection != nullptr) env->DeleteLocalRef(selection);
        if (selection_args != nullptr) env->DeleteLocalRef(selection_args);
        if (sort_order != nullptr) env->DeleteLocalRef(sort_order);
        env->DeleteLocalRef(uri);
        return nullptr;
    }

    jobject query_args = env->NewObject(env->FindClass("android/os/Bundle"), g_mid_bundle_ctor);
    env->CallVoidMethod(query_args, g_mid_bundle_put_string, g_jstr_query_arg_sql_selection, selection);
    env->CallVoidMethod(query_args, g_mid_bundle_put_string_array, g_jstr_query_arg_sql_selection_args, selection_args);

    if (projection != nullptr) env->DeleteLocalRef(projection);
    if (selection != nullptr) env->DeleteLocalRef(selection);
    if (selection_args != nullptr) env->DeleteLocalRef(selection_args);
    if (sort_order != nullptr) env->DeleteLocalRef(sort_order);
    if (bundle != nullptr) env->DeleteLocalRef(bundle);
    env->DeleteLocalRef(uri);

    env->CallVoidMethod(data, g_mid_parcel_set_data_position, original_position);
    fm_clear_jni_exception(env, "restore data position");

    return query_args;
}

static void fm_on_provider_query(JNIEnv *env, jstring descriptor, jint code, jobject data, jobject reply) {
    if (!fm_should_filter_media_query(env, descriptor, code, data)) {
        return;
    }

    jobject original_bundle = fm_read_query_args(env, data);
    if (original_bundle == nullptr) {
        return;
    }

    jobject filtered = fm_build_filtered_query_args(env, original_bundle);
    jobject updated_bundle = fm_build_updated_query_args(env, original_bundle, filtered);
    if (updated_bundle == nullptr) {
        return;
    }

    fm_write_filter_bundle(env, reply, updated_bundle);
    FM_LOGI("media query filtered");
}

static jboolean my_transact_native(JNIEnv *env, jobject thiz, jint code, jobject data, jobject reply, jint flags) {
    if (orig_transact_native == nullptr) {
        return JNI_FALSE;
    }

    jstring descriptor = static_cast<jstring>(env->CallObjectMethod(thiz, g_mid_binderproxy_get_interface_descriptor));
    if (descriptor != nullptr) {
        fm_observe_content_provider_transact(env, descriptor, code, data);
        fm_on_provider_query(env, descriptor, code, data, reply);
        env->DeleteLocalRef(descriptor);
    }

    return orig_transact_native(env, thiz, code, data, reply, flags);
}

static void fm_init_jni_cache(JNIEnv *env) {
    jclass cls_string_local = env->FindClass("java/lang/String");
    g_cls_string = static_cast<jclass>(env->NewGlobalRef(cls_string_local));

    jclass cls_binderproxy = env->FindClass("android/os/BinderProxy");
    g_mid_binderproxy_get_interface_descriptor = env->GetMethodID(cls_binderproxy, "getInterfaceDescriptor", "()Ljava/lang/String;");

    jclass cls_parcel = env->FindClass("android/os/Parcel");
    g_mid_parcel_obtain = env->GetStaticMethodID(cls_parcel, "obtain", "()Landroid/os/Parcel;");
    g_mid_parcel_recycle = env->GetMethodID(cls_parcel, "recycle", "()V");
    g_mid_parcel_data_position = env->GetMethodID(cls_parcel, "dataPosition", "()I");
    g_mid_parcel_set_data_position = env->GetMethodID(cls_parcel, "setDataPosition", "(I)V");
    g_mid_parcel_enforce_interface = env->GetMethodID(cls_parcel, "enforceInterface", "(Ljava/lang/String;)V");
    g_mid_parcel_write_interface_token = env->GetMethodID(cls_parcel, "writeInterfaceToken", "(Ljava/lang/String;)V");
    g_mid_parcel_read_string = env->GetMethodID(cls_parcel, "readString", "()Ljava/lang/String;");
    g_mid_parcel_write_string = env->GetMethodID(cls_parcel, "writeString", "(Ljava/lang/String;)V");
    g_mid_parcel_read_int = env->GetMethodID(cls_parcel, "readInt", "()I");
    g_mid_parcel_write_int = env->GetMethodID(cls_parcel, "writeInt", "(I)V");
    g_mid_parcel_create_string_array = env->GetMethodID(cls_parcel, "createStringArray", "()[Ljava/lang/String;");
    g_mid_parcel_write_string_array = env->GetMethodID(cls_parcel, "writeStringArray", "([Ljava/lang/String;)V");
    g_mid_parcel_read_bundle = env->GetMethodID(cls_parcel, "readBundle", "(Ljava/lang/ClassLoader;)Landroid/os/Bundle;");
    g_mid_parcel_write_bundle = env->GetMethodID(cls_parcel, "writeBundle", "(Landroid/os/Bundle;)V");
    g_mid_parcel_read_strong_binder = env->GetMethodID(cls_parcel, "readStrongBinder", "()Landroid/os/IBinder;");
    g_mid_parcel_write_strong_binder = env->GetMethodID(cls_parcel, "writeStrongBinder", "(Landroid/os/IBinder;)V");
    g_mid_parcel_marshall = env->GetMethodID(cls_parcel, "marshall", "()[B");
    g_mid_parcel_unmarshall = env->GetMethodID(cls_parcel, "unmarshall", "([BII)V");

    g_mid_creator_create_from_parcel = env->GetMethodID(env->FindClass("android/os/Parcelable$Creator"), "createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;");

    jclass cls_uri = env->FindClass("android/net/Uri");
    g_mid_uri_write_to_parcel = env->GetMethodID(cls_uri, "writeToParcel", "(Landroid/os/Parcel;I)V");
    g_mid_uri_get_authority = env->GetMethodID(cls_uri, "getAuthority", "()Ljava/lang/String;");
    g_mid_uri_to_string = env->GetMethodID(cls_uri, "toString", "()Ljava/lang/String;");

    jclass cls_bundle = env->FindClass("android/os/Bundle");
    g_mid_bundle_ctor = env->GetMethodID(cls_bundle, "<init>", "()V");
    g_mid_bundle_get_string = env->GetMethodID(cls_bundle, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    g_mid_bundle_put_string = env->GetMethodID(cls_bundle, "putString", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_mid_bundle_get_string_array = env->GetMethodID(cls_bundle, "getStringArray", "(Ljava/lang/String;)[Ljava/lang/String;");
    g_mid_bundle_put_string_array = env->GetMethodID(cls_bundle, "putStringArray", "(Ljava/lang/String;[Ljava/lang/String;)V");

    jclass cls_content_provider = env->FindClass("android/content/ContentProvider");
    g_query_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "QUERY_TRANSACTION", -1);
    g_insert_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "INSERT_TRANSACTION", -1);
    g_delete_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "DELETE_TRANSACTION", -1);
    g_bulk_insert_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "BULK_INSERT_TRANSACTION", -1);
    g_open_file_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "OPEN_FILE_TRANSACTION", -1);
    g_open_asset_file_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "OPEN_ASSET_FILE_TRANSACTION", -1);
    g_open_typed_asset_file_transaction = fm_get_static_int_field_or_default(env, cls_content_provider, "OPEN_TYPED_ASSET_FILE_TRANSACTION", -1);

    jclass cls_content_resolver = env->FindClass("android/content/ContentResolver");
    jfieldID fid_query_arg_sql_selection = env->GetStaticFieldID(cls_content_resolver, "QUERY_ARG_SQL_SELECTION", "Ljava/lang/String;");
    jfieldID fid_query_arg_sql_selection_args = env->GetStaticFieldID(cls_content_resolver, "QUERY_ARG_SQL_SELECTION_ARGS", "Ljava/lang/String;");
    g_jstr_query_arg_sql_selection = static_cast<jstring>(env->NewGlobalRef(env->GetStaticObjectField(cls_content_resolver, fid_query_arg_sql_selection)));
    g_jstr_query_arg_sql_selection_args = static_cast<jstring>(env->NewGlobalRef(env->GetStaticObjectField(cls_content_resolver, fid_query_arg_sql_selection_args)));

    jfieldID fid_uri_creator = env->GetStaticFieldID(cls_uri, "CREATOR", "Landroid/os/Parcelable$Creator;");
    jobject uri_creator_local = env->GetStaticObjectField(cls_uri, fid_uri_creator);
    g_uri_creator = env->NewGlobalRef(uri_creator_local);

    jclass cls_attribution_source = fm_find_class_optional(env, "android/content/AttributionSource");
    if (cls_attribution_source != nullptr) {
        jfieldID fid_attribution_source_creator = fm_get_static_field_id_optional(env, cls_attribution_source, "CREATOR", "Landroid/os/Parcelable$Creator;");
        if (fid_attribution_source_creator != nullptr) {
            jobject attribution_source_creator_local = env->GetStaticObjectField(cls_attribution_source, fid_attribution_source_creator);
            if (!fm_clear_jni_exception(env, "load AttributionSource.CREATOR") && attribution_source_creator_local != nullptr) {
                g_attribution_source_creator = env->NewGlobalRef(attribution_source_creator_local);
                env->DeleteLocalRef(attribution_source_creator_local);
            }
        }
        g_mid_attribution_source_get_package_name = fm_get_method_id_optional(env, cls_attribution_source, "getPackageName", "()Ljava/lang/String;");
        g_mid_attribution_source_get_attribution_tag = fm_get_method_id_optional(env, cls_attribution_source, "getAttributionTag", "()Ljava/lang/String;");
        env->DeleteLocalRef(cls_attribution_source);
    }

    env->DeleteLocalRef(cls_string_local);
    env->DeleteLocalRef(cls_binderproxy);
    env->DeleteLocalRef(cls_parcel);
    env->DeleteLocalRef(cls_uri);
    env->DeleteLocalRef(cls_bundle);
    env->DeleteLocalRef(cls_content_provider);
    env->DeleteLocalRef(cls_content_resolver);
    env->DeleteLocalRef(uri_creator_local);
}

static void fm_install_java_hooks(Api *api, JNIEnv *env) {
    JNINativeMethod methods[] = {
        { const_cast<char *>("transactNative"), const_cast<char *>("(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z"), reinterpret_cast<void *>(my_transact_native) },
    };

    api->hookJniNativeMethods(env, "android/os/BinderProxy", methods, 1);
    orig_transact_native = reinterpret_cast<jboolean (*)(JNIEnv *, jobject, jint, jobject, jobject, jint)>(methods[0].fnPtr);
}
static int fm_call_open(int (*function_ptr)(const char *, int, ...), const char *path, int flags, bool has_mode, mode_t mode) {
    if (has_mode) {
        return function_ptr(path, flags, mode);
    }
    return function_ptr(path, flags);
}

static int fm_call_openat(int (*function_ptr)(int, const char *, int, ...), int dirfd, const char *path, int flags, bool has_mode, mode_t mode) {
    if (has_mode) {
        return function_ptr(dirfd, path, flags, mode);
    }
    return function_ptr(dirfd, path, flags);
}

static int fm_handle_open_like(const char *path, int flags, bool has_mode, mode_t mode) {
    if (orig_open == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    FmRuntimeContext context = fm_make_open_context(flags);
    FmPathDecision decision;
    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return fm_call_open(orig_open, path, flags, has_mode, mode);
    }

    fm_log_path_hit("open", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return fm_call_open(orig_open, decision.redirected_path, flags, has_mode, mode);
    }

    return fm_call_open(orig_open, path, flags, has_mode, mode);
}

static int fm_handle_openat_like(int dirfd, const char *path, int flags, bool has_mode, mode_t mode) {
    if (orig_openat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    FmRuntimeContext context = fm_make_open_context(flags);
    FmPathDecision decision;
    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return fm_call_openat(orig_openat, dirfd, path, flags, has_mode, mode);
    }

    fm_log_path_hit("openat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return fm_call_openat(orig_openat, dirfd, decision.redirected_path, flags, has_mode, mode);
    }

    return fm_call_openat(orig_openat, dirfd, path, flags, has_mode, mode);
}

static int my_open(const char *path, int flags, ...) {
    bool has_mode = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) != 0;
    mode_t mode = 0;

    if (has_mode) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    return fm_handle_open_like(path, flags, has_mode, mode);
}

static int my_open64(const char *path, int flags, ...) {
    bool has_mode = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) != 0;
    mode_t mode = 0;

    if (orig_open64 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (has_mode) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    FmRuntimeContext context = fm_make_open_context(flags);
    FmPathDecision decision;
    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return fm_call_open(orig_open64, path, flags, has_mode, mode);
    }

    fm_log_path_hit("open64", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return fm_call_open(orig_open64, decision.redirected_path, flags, has_mode, mode);
    }

    return fm_call_open(orig_open64, path, flags, has_mode, mode);
}

static int my___open_2(const char *path, int flags) {
    FmRuntimeContext context = fm_make_open_context(flags);
    FmPathDecision decision;
    if (orig___open_2 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig___open_2(path, flags);
    }

    fm_log_path_hit("__open_2", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig___open_2(decision.redirected_path, flags);
    }

    return orig___open_2(path, flags);
}

static int my_openat(int dirfd, const char *path, int flags, ...) {
    bool has_mode = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) != 0;
    mode_t mode = 0;

    if (has_mode) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    return fm_handle_openat_like(dirfd, path, flags, has_mode, mode);
}

static int my_openat64(int dirfd, const char *path, int flags, ...) {
    bool has_mode = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) != 0;
    mode_t mode = 0;

    if (orig_openat64 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (has_mode) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    FmRuntimeContext context = fm_make_open_context(flags);
    FmPathDecision decision;
    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return fm_call_openat(orig_openat64, dirfd, path, flags, has_mode, mode);
    }

    fm_log_path_hit("openat64", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return fm_call_openat(orig_openat64, dirfd, decision.redirected_path, flags, has_mode, mode);
    }

    return fm_call_openat(orig_openat64, dirfd, path, flags, has_mode, mode);
}

static int my___openat_2(int dirfd, const char *path, int flags) {
    FmRuntimeContext context = fm_make_open_context(flags);
    FmPathDecision decision;
    if (orig___openat_2 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig___openat_2(dirfd, path, flags);
    }

    fm_log_path_hit("__openat_2", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig___openat_2(dirfd, decision.redirected_path, flags);
    }

    return orig___openat_2(dirfd, path, flags);
}

static int my_mkdir(const char *path, mode_t mode) {
    FmRuntimeContext context = fm_make_create_dir_context();
    FmPathDecision decision;
    if (orig_mkdir == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_mkdir(path, mode);
    }

    fm_log_path_hit("mkdir", path, &decision);

    if (fm_should_block(&decision)) {
        errno = EACCES;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_mkdir(decision.redirected_path, mode);
    }

    return orig_mkdir(path, mode);
}

static int my_mkdirat(int dirfd, const char *path, mode_t mode) {
    FmRuntimeContext context = fm_make_create_dir_context();
    FmPathDecision decision;
    if (orig_mkdirat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_mkdirat(dirfd, path, mode);
    }

    fm_log_path_hit("mkdirat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = EACCES;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_mkdirat(dirfd, decision.redirected_path, mode);
    }

    return orig_mkdirat(dirfd, path, mode);
}

static FILE *my_fopen(const char *path, const char *mode) {
    FmRuntimeContext context = fm_make_fopen_context(mode);
    FmPathDecision decision;
    if (orig_fopen == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_fopen(path, mode);
    }

    fm_log_path_hit("fopen", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return nullptr;
    }

    if (fm_should_redirect(&decision)) {
        return orig_fopen(decision.redirected_path, mode);
    }

    return orig_fopen(path, mode);
}

static FILE *my_fopen64(const char *path, const char *mode) {
    FmRuntimeContext context = fm_make_fopen_context(mode);
    FmPathDecision decision;
    if (orig_fopen64 == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_fopen64(path, mode);
    }

    fm_log_path_hit("fopen64", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return nullptr;
    }

    if (fm_should_redirect(&decision)) {
        return orig_fopen64(decision.redirected_path, mode);
    }

    return orig_fopen64(path, mode);
}

static int my_access(const char *path, int mode) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_access == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_access(path, mode);
    }

    fm_log_path_hit("access", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_access(decision.redirected_path, mode);
    }

    return orig_access(path, mode);
}

static int my_stat(const char *path, struct stat *buffer) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_stat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_stat(path, buffer);
    }

    fm_log_path_hit("stat", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_stat(decision.redirected_path, buffer);
    }

    return orig_stat(path, buffer);
}

static int my_lstat(const char *path, struct stat *buffer) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_lstat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_lstat(path, buffer);
    }

    fm_log_path_hit("lstat", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_lstat(decision.redirected_path, buffer);
    }

    return orig_lstat(path, buffer);
}

static int my_stat64(const char *path, struct stat64 *buffer) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_stat64 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_stat64(path, buffer);
    }

    fm_log_path_hit("stat64", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_stat64(decision.redirected_path, buffer);
    }

    return orig_stat64(path, buffer);
}

static int my_lstat64(const char *path, struct stat64 *buffer) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_lstat64 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_lstat64(path, buffer);
    }

    fm_log_path_hit("lstat64", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_lstat64(decision.redirected_path, buffer);
    }

    return orig_lstat64(path, buffer);
}

static ssize_t my_readlink(const char *path, char *buffer, size_t buffer_size) {
    FmRuntimeContext context = fm_make_readlink_context();
    FmPathDecision decision;
    if (orig_readlink == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_readlink(path, buffer, buffer_size);
    }

    fm_log_path_hit("readlink", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_readlink(decision.redirected_path, buffer, buffer_size);
    }

    return orig_readlink(path, buffer, buffer_size);
}

static char *my_realpath(const char *path, char *resolved_path) {
    FmRuntimeContext context = fm_make_readlink_context();
    FmPathDecision decision;
    if (orig_realpath == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_realpath(path, resolved_path);
    }

    fm_log_path_hit("realpath", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return nullptr;
    }

    if (fm_should_redirect(&decision)) {
        return orig_realpath(decision.redirected_path, resolved_path);
    }

    return orig_realpath(path, resolved_path);
}

static int my_faccessat(int dirfd, const char *path, int mode, int flags) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_faccessat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_faccessat(dirfd, path, mode, flags);
    }

    fm_log_path_hit("faccessat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_faccessat(dirfd, decision.redirected_path, mode, flags);
    }

    return orig_faccessat(dirfd, path, mode, flags);
}

static int my_unlink(const char *path) {
    FmRuntimeContext context = fm_make_remove_file_context();
    FmPathDecision decision;
    if (orig_unlink == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_unlink(path);
    }

    fm_log_path_hit("unlink", path, &decision);

    if (fm_should_block(&decision)) {
        errno = EACCES;
        return -1;
    }

    const char *target_path = fm_should_redirect(&decision) ? decision.redirected_path : path;
    int result = orig_unlink(target_path);
    if (result == 0) {
        fm_invalidate_cache_path(path);
        if (fm_should_redirect(&decision)) {
            fm_invalidate_cache_path(decision.redirected_path);
        }
    }
    return result;
}

static int my_unlinkat(int dirfd, const char *path, int flags) {
    if (orig_unlinkat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    const bool remove_dir = (flags & AT_REMOVEDIR) != 0;
    FmRuntimeContext context = remove_dir ? fm_make_remove_dir_context() : fm_make_remove_file_context();
    FmPathDecision decision;

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_unlinkat(dirfd, path, flags);
    }

    fm_log_path_hit("unlinkat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = EACCES;
        return -1;
    }

    const char *target_path = fm_should_redirect(&decision) ? decision.redirected_path : path;
    int result = orig_unlinkat(dirfd, target_path, flags);
    if (result == 0) {
        fm_invalidate_cache_path(input_path);
        if (fm_should_redirect(&decision)) {
            fm_invalidate_cache_path(decision.redirected_path);
        }
    }
    return result;
}

static int my_rmdir(const char *path) {
    FmRuntimeContext context = fm_make_remove_dir_context();
    FmPathDecision decision;
    if (orig_rmdir == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_rmdir(path);
    }

    fm_log_path_hit("rmdir", path, &decision);

    if (fm_should_block(&decision)) {
        errno = EACCES;
        return -1;
    }

    const char *target_path = fm_should_redirect(&decision) ? decision.redirected_path : path;
    int result = orig_rmdir(target_path);
    if (result == 0) {
        fm_invalidate_cache_path(path);
        if (fm_should_redirect(&decision)) {
            fm_invalidate_cache_path(decision.redirected_path);
        }
    }
    return result;
}

static int my_fstatat(int dirfd, const char *path, struct stat *buffer, int flags) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_fstatat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_fstatat(dirfd, path, buffer, flags);
    }

    fm_log_path_hit("fstatat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_fstatat(dirfd, decision.redirected_path, buffer, flags);
    }

    return orig_fstatat(dirfd, path, buffer, flags);
}

static int my_fstatat64(int dirfd, const char *path, struct stat64 *buffer, int flags) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_fstatat64 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_fstatat64(dirfd, path, buffer, flags);
    }

    fm_log_path_hit("fstatat64", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_fstatat64(dirfd, decision.redirected_path, buffer, flags);
    }

    return orig_fstatat64(dirfd, path, buffer, flags);
}

static int my_statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *buffer) {
    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision decision;
    if (orig_statx == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_statx(dirfd, path, flags, mask, buffer);
    }

    fm_log_path_hit("statx", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_statx(dirfd, decision.redirected_path, flags, mask, buffer);
    }

    return orig_statx(dirfd, path, flags, mask, buffer);
}
static DIR *my_opendir(const char *path) {
    FmRuntimeContext context = fm_make_dir_context();
    FmPathDecision decision;
    if (orig_opendir == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_opendir(path);
    }

    fm_log_path_hit("opendir", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return nullptr;
    }

    if (fm_should_redirect(&decision)) {
        return orig_opendir(decision.redirected_path);
    }

    return orig_opendir(path);
}

static bool fm_read_dir_path(DIR *dir, char *output_path, size_t output_size) {
    if (dir == nullptr || output_path == nullptr || output_size == 0) {
        return false;
    }

    int fd = dirfd(dir);
    if (fd < 0) {
        return false;
    }

    return fm_resolve_fd_path(fd, output_path, output_size);
}

static struct dirent *my_readdir(DIR *dir) {
    if (orig_readdir == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }

    struct dirent *entry = orig_readdir(dir);
    while (entry != nullptr) {
        if (fm_is_dot_or_dotdot(entry->d_name)) {
            return entry;
        }

        char path_buffer[FM_MAX_PATH_LEN] = {0};
        if (!fm_read_dir_path(dir, path_buffer, sizeof(path_buffer))) {
            return entry;
        }

        if (!fm_should_hide_dirent_from_dir(path_buffer, entry->d_name)) {
            return entry;
        }

        fm_log_blocked_dir_entry("readdir", path_buffer, entry->d_name);
        entry = orig_readdir(dir);
    }

    return entry;
}

static int my_getdents64(unsigned int fd, struct linux_dirent64 *dirp, unsigned int count) {
    if (orig_getdents64 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    int read_bytes = orig_getdents64(fd, dirp, count);
    if (read_bytes <= 0) {
        return read_bytes;
    }

    char dir_path[FM_MAX_PATH_LEN] = {0};
    if (!fm_resolve_fd_path(static_cast<int>(fd), dir_path, sizeof(dir_path))) {
        return read_bytes;
    }

    unsigned int offset = 0;
    while (offset < static_cast<unsigned int>(read_bytes)) {
        struct linux_dirent64 *entry = reinterpret_cast<struct linux_dirent64 *>(reinterpret_cast<char *>(dirp) + offset);
        if (!fm_is_dot_or_dotdot(entry->d_name) && fm_should_hide_dirent_from_dir(dir_path, entry->d_name)) {
            fm_log_blocked_dir_entry("getdents64", dir_path, entry->d_name);
            unsigned int remaining = read_bytes - (offset + entry->d_reclen);
            memmove(entry, reinterpret_cast<char *>(entry) + entry->d_reclen, remaining);
            read_bytes -= entry->d_reclen;
            continue;
        }
        offset += entry->d_reclen;
    }

    return read_bytes;
}

static int my_scandir(const char *path, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*comparator)(const struct dirent **, const struct dirent **)) {
    FmRuntimeContext context = fm_make_dir_context();
    FmPathDecision decision;
    if (orig_scandir == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!fm_apply_path_decision_with_context(path, &context, &decision)) {
        return orig_scandir(path, namelist, filter, comparator);
    }

    fm_log_path_hit("scandir", path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_scandir(decision.redirected_path, namelist, filter, comparator);
    }

    return orig_scandir(path, namelist, filter, comparator);
}

static int my_scandirat(int dirfd, const char *path, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*comparator)(const struct dirent **, const struct dirent **)) {
    FmRuntimeContext context = fm_make_dir_context();
    FmPathDecision decision;
    if (orig_scandirat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_scandirat(dirfd, path, namelist, filter, comparator);
    }

    fm_log_path_hit("scandirat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_scandirat(dirfd, decision.redirected_path, namelist, filter, comparator);
    }

    return orig_scandirat(dirfd, path, namelist, filter, comparator);
}

static ssize_t my_readlinkat(int dirfd, const char *path, char *buffer, size_t buffer_size) {
    FmRuntimeContext context = fm_make_readlink_context();
    FmPathDecision decision;
    if (orig_readlinkat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_path[FM_MAX_PATH_LEN] = {0};
    const char *input_path = path;
    if (fm_try_resolve_at_path(dirfd, path, resolved_path, sizeof(resolved_path))) {
        input_path = resolved_path;
    }

    if (!fm_apply_path_decision_with_context(input_path, &context, &decision)) {
        return orig_readlinkat(dirfd, path, buffer, buffer_size);
    }

    fm_log_path_hit("readlinkat", input_path, &decision);

    if (fm_should_block(&decision)) {
        errno = ENOENT;
        return -1;
    }

    if (fm_should_redirect(&decision)) {
        return orig_readlinkat(dirfd, decision.redirected_path, buffer, buffer_size);
    }

    return orig_readlinkat(dirfd, path, buffer, buffer_size);
}

static int my_rename(const char *oldpath, const char *newpath) {
    if (orig_rename == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    FmRuntimeContext context = fm_make_remove_file_context();
    FmPathDecision old_decision;
    FmPathDecision new_decision;
    bool matched_old = fm_apply_path_decision_with_context(oldpath, &context, &old_decision);
    bool matched_new = fm_apply_path_decision_with_context(newpath, &context, &new_decision);
    if (matched_old) {
        fm_log_path_hit("rename", oldpath, &old_decision);
    }
    if (matched_new) {
        fm_log_path_hit("rename", newpath, &new_decision);
    }

    if (fm_should_block_rename_decision(old_decision.type, new_decision.type)) {
        errno = EACCES;
        return -1;
    }

    return orig_rename(oldpath, newpath);
}

static int my_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    if (orig_renameat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_old[FM_MAX_PATH_LEN] = {0};
    char resolved_new[FM_MAX_PATH_LEN] = {0};
    const char *old_input = oldpath;
    const char *new_input = newpath;
    if (fm_try_resolve_at_path(olddirfd, oldpath, resolved_old, sizeof(resolved_old))) {
        old_input = resolved_old;
    }
    if (fm_try_resolve_at_path(newdirfd, newpath, resolved_new, sizeof(resolved_new))) {
        new_input = resolved_new;
    }

    FmRuntimeContext context = fm_make_remove_file_context();
    FmPathDecision old_decision;
    FmPathDecision new_decision;
    bool matched_old = fm_apply_path_decision_with_context(old_input, &context, &old_decision);
    bool matched_new = fm_apply_path_decision_with_context(new_input, &context, &new_decision);
    if (matched_old) {
        fm_log_path_hit("renameat", old_input, &old_decision);
    }
    if (matched_new) {
        fm_log_path_hit("renameat", new_input, &new_decision);
    }

    if (fm_should_block_rename_decision(old_decision.type, new_decision.type)) {
        errno = EACCES;
        return -1;
    }

    return orig_renameat(olddirfd, oldpath, newdirfd, newpath);
}

static int my_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags) {
    if (orig_renameat2 == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_old[FM_MAX_PATH_LEN] = {0};
    char resolved_new[FM_MAX_PATH_LEN] = {0};
    const char *old_input = oldpath;
    const char *new_input = newpath;
    if (fm_try_resolve_at_path(olddirfd, oldpath, resolved_old, sizeof(resolved_old))) {
        old_input = resolved_old;
    }
    if (fm_try_resolve_at_path(newdirfd, newpath, resolved_new, sizeof(resolved_new))) {
        new_input = resolved_new;
    }

    FmRuntimeContext context = fm_make_remove_file_context();
    FmPathDecision old_decision;
    FmPathDecision new_decision;
    bool matched_old = fm_apply_path_decision_with_context(old_input, &context, &old_decision);
    bool matched_new = fm_apply_path_decision_with_context(new_input, &context, &new_decision);
    if (matched_old) {
        fm_log_path_hit("renameat2", old_input, &old_decision);
    }
    if (matched_new) {
        fm_log_path_hit("renameat2", new_input, &new_decision);
    }

    if (fm_should_block_rename_decision(old_decision.type, new_decision.type)) {
        errno = EACCES;
        return -1;
    }

    return orig_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}

static int my_link(const char *oldpath, const char *newpath) {
    if (orig_link == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision old_decision;
    FmPathDecision new_decision;
    bool matched_old = fm_apply_path_decision_with_context(oldpath, &context, &old_decision);
    bool matched_new = fm_apply_path_decision_with_context(newpath, &context, &new_decision);
    if (matched_old) {
        fm_log_path_hit("link", oldpath, &old_decision);
    }
    if (matched_new) {
        fm_log_path_hit("link", newpath, &new_decision);
    }

    if (fm_should_block_rename_decision(old_decision.type, new_decision.type)) {
        errno = EACCES;
        return -1;
    }

    return orig_link(oldpath, newpath);
}

static int my_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    if (orig_linkat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_old[FM_MAX_PATH_LEN] = {0};
    char resolved_new[FM_MAX_PATH_LEN] = {0};
    const char *old_input = oldpath;
    const char *new_input = newpath;
    if (fm_try_resolve_at_path(olddirfd, oldpath, resolved_old, sizeof(resolved_old))) {
        old_input = resolved_old;
    }
    if (fm_try_resolve_at_path(newdirfd, newpath, resolved_new, sizeof(resolved_new))) {
        new_input = resolved_new;
    }

    FmRuntimeContext context = fm_make_stat_context();
    FmPathDecision old_decision;
    FmPathDecision new_decision;
    bool matched_old = fm_apply_path_decision_with_context(old_input, &context, &old_decision);
    bool matched_new = fm_apply_path_decision_with_context(new_input, &context, &new_decision);
    if (matched_old) {
        fm_log_path_hit("linkat", old_input, &old_decision);
    }
    if (matched_new) {
        fm_log_path_hit("linkat", new_input, &new_decision);
    }

    if (fm_should_block_rename_decision(old_decision.type, new_decision.type)) {
        errno = EACCES;
        return -1;
    }

    return orig_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

static int my_symlink(const char *target, const char *linkpath) {
    if (orig_symlink == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    FmRuntimeContext context = fm_make_create_dir_context();
    FmPathDecision link_decision;
    bool matched_link = fm_apply_path_decision_with_context(linkpath, &context, &link_decision);
    if (matched_link) {
        fm_log_path_hit("symlink", linkpath, &link_decision);
    }

    if (matched_link && fm_should_block(&link_decision)) {
        errno = EACCES;
        return -1;
    }

    return orig_symlink(target, linkpath);
}

static int my_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    if (orig_symlinkat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    char resolved_link[FM_MAX_PATH_LEN] = {0};
    const char *link_input = linkpath;
    if (fm_try_resolve_at_path(newdirfd, linkpath, resolved_link, sizeof(resolved_link))) {
        link_input = resolved_link;
    }

    FmRuntimeContext context = fm_make_create_dir_context();
    FmPathDecision link_decision;
    bool matched_link = fm_apply_path_decision_with_context(link_input, &context, &link_decision);
    if (matched_link) {
        fm_log_path_hit("symlinkat", link_input, &link_decision);
    }

    if (matched_link && fm_should_block(&link_decision)) {
        errno = EACCES;
        return -1;
    }

    return orig_symlinkat(target, newdirfd, linkpath);
}
static bool fm_is_duplicate_identity(const FmElfIdentity *identities, size_t count, dev_t dev, ino_t inode) {
    for (size_t index = 0; index < count; ++index) {
        if (identities[index].dev == dev && identities[index].inode == inode) {
            return true;
        }
    }

    return false;
}

static bool fm_should_hook_elf_path(const char *path) {
    if (path == nullptr || path[0] != '/') {
        return false;
    }

    if (strstr(path, "/data/adb/modules/") != nullptr) {
        return false;
    }

    if (strstr(path, ".so") != nullptr) {
        return true;
    }

    if (strstr(path, "/system/bin/") != nullptr || strstr(path, "/apex/") != nullptr) {
        return true;
    }

    return false;
}

static size_t fm_collect_hook_targets(FmElfIdentity *identities, size_t capacity) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        FM_LOGE("打开 /proc/self/maps 失败");
        return 0;
    }

    size_t count = 0;
    char line[4096];

    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long offset = 0;
        unsigned int dev_major = 0;
        unsigned int dev_minor = 0;
        unsigned long long inode_value = 0;
        char perms[5] = {0};
        char path[FM_MAX_PATH_LEN] = {0};

        int parsed = sscanf(line, "%lx-%lx %4s %lx %x:%x %llu %1023s", &start, &end, perms, &offset, &dev_major, &dev_minor, &inode_value, path);
        if (parsed < 7 || inode_value == 0 || path[0] == '\0') {
            continue;
        }

        if (!fm_should_hook_elf_path(path)) {
            continue;
        }

        dev_t dev = makedev(dev_major, dev_minor);
        ino_t inode = static_cast<ino_t>(inode_value);

        if (fm_is_duplicate_identity(identities, count, dev, inode)) {
            continue;
        }

        if (count >= capacity) {
            break;
        }

        identities[count].dev = dev;
        identities[count].inode = inode;
        count++;
    }

    fclose(maps);
    return count;
}

static void fm_register_hooks_for_identity(Api *api, const FmElfIdentity *identity) {
    api->pltHookRegister(identity->dev, identity->inode, "open", reinterpret_cast<void *>(my_open), reinterpret_cast<void **>(&orig_open));
    api->pltHookRegister(identity->dev, identity->inode, "open64", reinterpret_cast<void *>(my_open64), reinterpret_cast<void **>(&orig_open64));
    api->pltHookRegister(identity->dev, identity->inode, "__open_2", reinterpret_cast<void *>(my___open_2), reinterpret_cast<void **>(&orig___open_2));
    api->pltHookRegister(identity->dev, identity->inode, "openat", reinterpret_cast<void *>(my_openat), reinterpret_cast<void **>(&orig_openat));
    api->pltHookRegister(identity->dev, identity->inode, "openat64", reinterpret_cast<void *>(my_openat64), reinterpret_cast<void **>(&orig_openat64));
    api->pltHookRegister(identity->dev, identity->inode, "__openat_2", reinterpret_cast<void *>(my___openat_2), reinterpret_cast<void **>(&orig___openat_2));
    api->pltHookRegister(identity->dev, identity->inode, "mkdir", reinterpret_cast<void *>(my_mkdir), reinterpret_cast<void **>(&orig_mkdir));
    api->pltHookRegister(identity->dev, identity->inode, "mkdirat", reinterpret_cast<void *>(my_mkdirat), reinterpret_cast<void **>(&orig_mkdirat));
    api->pltHookRegister(identity->dev, identity->inode, "fopen", reinterpret_cast<void *>(my_fopen), reinterpret_cast<void **>(&orig_fopen));
    api->pltHookRegister(identity->dev, identity->inode, "fopen64", reinterpret_cast<void *>(my_fopen64), reinterpret_cast<void **>(&orig_fopen64));
    api->pltHookRegister(identity->dev, identity->inode, "access", reinterpret_cast<void *>(my_access), reinterpret_cast<void **>(&orig_access));
    api->pltHookRegister(identity->dev, identity->inode, "faccessat", reinterpret_cast<void *>(my_faccessat), reinterpret_cast<void **>(&orig_faccessat));
    api->pltHookRegister(identity->dev, identity->inode, "unlink", reinterpret_cast<void *>(my_unlink), reinterpret_cast<void **>(&orig_unlink));
    api->pltHookRegister(identity->dev, identity->inode, "unlinkat", reinterpret_cast<void *>(my_unlinkat), reinterpret_cast<void **>(&orig_unlinkat));
    api->pltHookRegister(identity->dev, identity->inode, "rmdir", reinterpret_cast<void *>(my_rmdir), reinterpret_cast<void **>(&orig_rmdir));
    api->pltHookRegister(identity->dev, identity->inode, "stat", reinterpret_cast<void *>(my_stat), reinterpret_cast<void **>(&orig_stat));
    api->pltHookRegister(identity->dev, identity->inode, "lstat", reinterpret_cast<void *>(my_lstat), reinterpret_cast<void **>(&orig_lstat));
    api->pltHookRegister(identity->dev, identity->inode, "stat64", reinterpret_cast<void *>(my_stat64), reinterpret_cast<void **>(&orig_stat64));
    api->pltHookRegister(identity->dev, identity->inode, "lstat64", reinterpret_cast<void *>(my_lstat64), reinterpret_cast<void **>(&orig_lstat64));
    api->pltHookRegister(identity->dev, identity->inode, "fstatat", reinterpret_cast<void *>(my_fstatat), reinterpret_cast<void **>(&orig_fstatat));
    api->pltHookRegister(identity->dev, identity->inode, "fstatat64", reinterpret_cast<void *>(my_fstatat64), reinterpret_cast<void **>(&orig_fstatat64));
    api->pltHookRegister(identity->dev, identity->inode, "statx", reinterpret_cast<void *>(my_statx), reinterpret_cast<void **>(&orig_statx));
    api->pltHookRegister(identity->dev, identity->inode, "opendir", reinterpret_cast<void *>(my_opendir), reinterpret_cast<void **>(&orig_opendir));
    api->pltHookRegister(identity->dev, identity->inode, "readdir", reinterpret_cast<void *>(my_readdir), reinterpret_cast<void **>(&orig_readdir));
    api->pltHookRegister(identity->dev, identity->inode, "getdents64", reinterpret_cast<void *>(my_getdents64), reinterpret_cast<void **>(&orig_getdents64));
    api->pltHookRegister(identity->dev, identity->inode, "scandir", reinterpret_cast<void *>(my_scandir), reinterpret_cast<void **>(&orig_scandir));
    api->pltHookRegister(identity->dev, identity->inode, "scandirat", reinterpret_cast<void *>(my_scandirat), reinterpret_cast<void **>(&orig_scandirat));
    api->pltHookRegister(identity->dev, identity->inode, "readlink", reinterpret_cast<void *>(my_readlink), reinterpret_cast<void **>(&orig_readlink));
    api->pltHookRegister(identity->dev, identity->inode, "readlinkat", reinterpret_cast<void *>(my_readlinkat), reinterpret_cast<void **>(&orig_readlinkat));
    api->pltHookRegister(identity->dev, identity->inode, "realpath", reinterpret_cast<void *>(my_realpath), reinterpret_cast<void **>(&orig_realpath));
    api->pltHookRegister(identity->dev, identity->inode, "rename", reinterpret_cast<void *>(my_rename), reinterpret_cast<void **>(&orig_rename));
    api->pltHookRegister(identity->dev, identity->inode, "renameat", reinterpret_cast<void *>(my_renameat), reinterpret_cast<void **>(&orig_renameat));
    api->pltHookRegister(identity->dev, identity->inode, "renameat2", reinterpret_cast<void *>(my_renameat2), reinterpret_cast<void **>(&orig_renameat2));
    api->pltHookRegister(identity->dev, identity->inode, "link", reinterpret_cast<void *>(my_link), reinterpret_cast<void **>(&orig_link));
    api->pltHookRegister(identity->dev, identity->inode, "linkat", reinterpret_cast<void *>(my_linkat), reinterpret_cast<void **>(&orig_linkat));
    api->pltHookRegister(identity->dev, identity->inode, "symlink", reinterpret_cast<void *>(my_symlink), reinterpret_cast<void **>(&orig_symlink));
    api->pltHookRegister(identity->dev, identity->inode, "symlinkat", reinterpret_cast<void *>(my_symlinkat), reinterpret_cast<void **>(&orig_symlinkat));
    api->pltHookRegister(identity->dev, identity->inode, "dlopen", reinterpret_cast<void *>(my_dlopen), reinterpret_cast<void **>(&orig_dlopen));
    api->pltHookRegister(identity->dev, identity->inode, "android_dlopen_ext", reinterpret_cast<void *>(my_android_dlopen_ext), reinterpret_cast<void **>(&orig_android_dlopen_ext));
    fm_track_hook_identity(identity);
}

static bool fm_install_plt_hooks(Api *api) {
    FmElfIdentity identities[FM_MAX_HOOK_TARGETS];
    memset(identities, 0, sizeof(identities));

    size_t identity_count = fm_collect_hook_targets(identities, FM_MAX_HOOK_TARGETS);
    if (identity_count == 0) {
        FM_LOGW("未找到可 Hook 的 ELF 映射");
        return false;
    }

    for (size_t index = 0; index < identity_count; ++index) {
        fm_register_hooks_for_identity(api, &identities[index]);
    }

    bool committed = api->pltHookCommit();
    FM_LOGI("PLT Hook 提交结果: %s, 目标 ELF 数量: %zu", committed ? "success" : "failed", identity_count);
    return committed;
}

class FolderManagerModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        g_api = api;
        fm_init_jni_cache(env);
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        g_should_hook.store(false, std::memory_order_release);
        g_rule_count.store(0, std::memory_order_release);
        g_rules_mtime_ns.store(0, std::memory_order_release);
        g_module_dir_fd = -1;
        g_process_name.clear();
        g_lsposed_gate_active.store(true, std::memory_order_release);
        g_lsposed_marker_path[0] = '\0';
        fm_reset_rule_set(&g_rule_set);

        if (args == nullptr || args->nice_name == nullptr) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char *process_name = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (process_name == nullptr) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        int module_dir_fd = api_->getModuleDir();
        if (module_dir_fd >= 0) {
            bool loaded = fm_load_rules_from_module_dir_fd(module_dir_fd, process_name, &g_rule_set);
            bool has_rules = loaded && fm_has_matching_rule(&g_rule_set);
            g_should_hook.store(has_rules, std::memory_order_release);
            if (has_rules) {
                g_enable_media_query_hook = fm_should_enable_media_query_hook(&g_rule_set);
                g_module_dir_fd = module_dir_fd;
                g_process_name.assign(process_name);
                if (g_module_dir_fd >= 0) {
                    int marker_fd = openat(g_module_dir_fd, "config/lsposed_enabled", O_RDONLY | O_CLOEXEC);
                    if (marker_fd >= 0) {
                        close(marker_fd);
                        snprintf(g_lsposed_marker_path, sizeof(g_lsposed_marker_path),
                                 "/proc/self/fd/%d/config/lsposed_enabled", g_module_dir_fd);
                        fm_update_lsposed_gate();
                    } else {
                        g_lsposed_marker_path[0] = '\0';
                        g_lsposed_gate_active.store(true, std::memory_order_release);
                    }
                }
                uint64_t mtime_ns = 0;
                if (fm_read_rules_mtime_ns(module_dir_fd, &mtime_ns)) {
                    g_rules_mtime_ns.store(mtime_ns, std::memory_order_release);
                }
                g_rule_count.store(g_rule_set.count, std::memory_order_release);
                fm_start_reload_thread_if_needed();
            } else {
                close(module_dir_fd);
            }
        }

        if (g_should_hook.load(std::memory_order_acquire) && (g_enable_media_query_hook || g_enable_provider_observe)) {
            fm_install_java_hooks(api_, env_);
        }

        FM_LOGI("process=%s, matched_rules=%zu", process_name, g_rule_count.load(std::memory_order_relaxed));
        env_->ReleaseStringUTFChars(args->nice_name, process_name);

        if (!g_should_hook.load(std::memory_order_acquire)) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        (void) args;

        if (!g_should_hook.load(std::memory_order_acquire)) {
            return;
        }

        if (!fm_install_plt_hooks(api_)) {
            FM_LOGE("PLT Hook 安装失败");
        }
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        (void) args;
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(FolderManagerModule)