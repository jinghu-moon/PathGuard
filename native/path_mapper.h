#ifndef FOLDER_MANAGER_PATH_MAPPER_H
#define FOLDER_MANAGER_PATH_MAPPER_H

#include <stdbool.h>
#include <stddef.h>

bool fm_normalize_path(const char *input_path, char *output_path, size_t output_size);
bool fm_get_relative_external_path(const char *input_path, char *output_path, size_t output_size);
bool fm_join_path(const char *base_path, const char *entry_name, char *output_path, size_t output_size);
bool fm_is_same_path(const char *lhs_path, const char *rhs_path);
bool fm_is_dot_or_dotdot(const char *name);

#endif
