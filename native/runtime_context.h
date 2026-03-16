#pragma once

namespace fm {

enum class PathOperation {
    kUnknown = 0,
    kOpen = 1,
    kOpenDirectory = 2,
    kCreateFile = 3,
    kCreateDirectory = 4,
    kStat = 5,
    kEnumerateDirectory = 6,
    kReadLink = 7,
    kRemoveFile = 8,
    kRemoveDirectory = 9,
};

struct RuntimeContext {
    PathOperation operation = PathOperation::kUnknown;
    int open_flags = 0;
    bool already_redirected = false;
};

}  // namespace fm
