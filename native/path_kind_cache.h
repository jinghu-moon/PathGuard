#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fm {

enum class PathKind {
    kAuto = 0,
    kFile = 1,
    kDirectory = 2,
};

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
    size_t operator()(const std::string& value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

class ResolvedPathKindCache {
public:
    bool TryGet(std::string_view path, PathKind* out_kind) const;
    void Put(std::string_view path, PathKind kind);
    void Erase(std::string_view path);
    void Clear();
    size_t Size() const;
    size_t EstimatedMemoryUsage() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PathKind, TransparentStringHash, std::equal_to<>> values_;
};

}  // namespace fm
