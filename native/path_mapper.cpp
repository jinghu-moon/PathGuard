#include "path_mapper.h"

#include <stdio.h>
#include <string.h>

#define FM_EXTERNAL_PRIMARY_PREFIX "/storage/emulated/0"

static bool fm_has_prefix_boundary(const char *text, const char *prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    size_t prefix_length = strlen(prefix);
    if (strncmp(text, prefix, prefix_length) != 0) {
        return false;
    }

    return text[prefix_length] == '\0' || text[prefix_length] == '/';
}

static bool fm_copy_trimmed_path(const char *input_path, char *output_path, size_t output_size) {
    if (input_path == nullptr || output_path == nullptr || output_size == 0) {
        return false;
    }

    size_t input_length = strlen(input_path);
    if (input_length + 1 > output_size) {
        return false;
    }

    memcpy(output_path, input_path, input_length + 1);
    while (input_length > 1 && output_path[input_length - 1] == '/') {
        output_path[input_length - 1] = '\0';
        input_length--;
    }
    return true;
}

static bool fm_collapse_absolute_path(const char *input_path, char *output_path, size_t output_size) {
    if (input_path == nullptr || output_path == nullptr || output_size < 2 || input_path[0] != '/') {
        return false;
    }

    size_t out_len = 1;
    output_path[0] = '/';
    output_path[1] = '\0';

    size_t index = 0;
    while (input_path[index] != '\0') {
        while (input_path[index] == '/') {
            index++;
        }
        if (input_path[index] == '\0') {
            break;
        }

        size_t seg_start = index;
        while (input_path[index] != '\0' && input_path[index] != '/') {
            index++;
        }
        size_t seg_len = index - seg_start;

        if (seg_len == 1 && input_path[seg_start] == '.') {
            continue;
        }
        if (seg_len == 2 && input_path[seg_start] == '.' && input_path[seg_start + 1] == '.') {
            if (out_len > 1) {
                if (output_path[out_len - 1] == '/') {
                    out_len--;
                }
                while (out_len > 1 && output_path[out_len - 1] != '/') {
                    out_len--;
                }
                output_path[out_len] = '\0';
            }
            continue;
        }

        if (out_len > 1 && output_path[out_len - 1] != '/') {
            if (out_len + 1 >= output_size) {
                return false;
            }
            output_path[out_len++] = '/';
        }

        if (out_len + seg_len + 1 > output_size) {
            return false;
        }
        memcpy(output_path + out_len, input_path + seg_start, seg_len);
        out_len += seg_len;
        output_path[out_len] = '\0';
    }

    if (out_len == 0) {
        if (output_size < 2) {
            return false;
        }
        output_path[0] = '/';
        output_path[1] = '\0';
    }
    return true;
}


bool fm_normalize_path(const char *input_path, char *output_path, size_t output_size) {
    static const char *aliases[] = {
        "/storage/emulated/0",
        "/storage/self/primary",
        "/storage/emulated/legacy",
        "/sdcard",
        "/mnt/sdcard",
    };

    if (input_path == nullptr || output_path == nullptr || output_size == 0 || input_path[0] == '\0') {
        return false;
    }

    const char *source = input_path;
    char mapped[1024] = {0};

    for (size_t index = 0; index < sizeof(aliases) / sizeof(aliases[0]); ++index) {
        if (!fm_has_prefix_boundary(input_path, aliases[index])) {
            continue;
        }

        const char *suffix = input_path + strlen(aliases[index]);
        int written = snprintf(mapped, sizeof(mapped), "%s%s", FM_EXTERNAL_PRIMARY_PREFIX, suffix);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(mapped)) {
            return false;
        }
        source = mapped;
        break;
    }

    if (source[0] == '/') {
        return fm_collapse_absolute_path(source, output_path, output_size);
    }

    return fm_copy_trimmed_path(source, output_path, output_size);
}

bool fm_get_relative_external_path(const char *input_path, char *output_path, size_t output_size) {
    char normalized_path[1024] = {0};
    if (!fm_normalize_path(input_path, normalized_path, sizeof(normalized_path))) {
        return false;
    }

    if (!fm_has_prefix_boundary(normalized_path, FM_EXTERNAL_PRIMARY_PREFIX)) {
        return false;
    }

    const char *relative = normalized_path + strlen(FM_EXTERNAL_PRIMARY_PREFIX);
    if (relative[0] == '/') {
        relative++;
    }
    if (relative[0] == '\0') {
        return false;
    }

    return fm_copy_trimmed_path(relative, output_path, output_size);
}

bool fm_join_path(const char *base_path, const char *entry_name, char *output_path, size_t output_size) {
    char normalized_base[1024] = {0};
    if (base_path == nullptr || entry_name == nullptr || output_path == nullptr || output_size == 0) {
        return false;
    }
    if (entry_name[0] == '\0') {
        return false;
    }
    if (entry_name[0] == '/') {
        return fm_normalize_path(entry_name, output_path, output_size);
    }
    if (!fm_normalize_path(base_path, normalized_base, sizeof(normalized_base))) {
        return false;
    }

    int written = 0;
    if (strcmp(normalized_base, "/") == 0) {
        written = snprintf(output_path, output_size, "/%s", entry_name);
    } else {
        written = snprintf(output_path, output_size, "%s/%s", normalized_base, entry_name);
    }
    if (written < 0 || static_cast<size_t>(written) >= output_size) {
        return false;
    }

    return fm_normalize_path(output_path, output_path, output_size);
}

bool fm_is_same_path(const char *lhs_path, const char *rhs_path) {
    char normalized_lhs[1024] = {0};
    char normalized_rhs[1024] = {0};

    if (!fm_normalize_path(lhs_path, normalized_lhs, sizeof(normalized_lhs))) {
        return false;
    }
    if (!fm_normalize_path(rhs_path, normalized_rhs, sizeof(normalized_rhs))) {
        return false;
    }

    return strcmp(normalized_lhs, normalized_rhs) == 0;
}

bool fm_is_dot_or_dotdot(const char *name) {
    return name != nullptr && (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}
