#include "path_kind_cache.h"

#include <mutex>

namespace fm {

bool ResolvedPathKindCache::TryGet(std::string_view path, PathKind* out_kind) const {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = values_.find(path);
    if (it == values_.end()) {
        return false;
    }
    if (out_kind != nullptr) {
        *out_kind = it->second;
    }
    return true;
}

void ResolvedPathKindCache::Put(std::string_view path, PathKind kind) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = values_.find(path);
    if (it != values_.end()) {
        it->second = kind;
        return;
    }
    values_.emplace(std::string(path), kind);
}

void ResolvedPathKindCache::Erase(std::string_view path) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = values_.find(path);
    if (it != values_.end()) {
        values_.erase(it);
    }
}

void ResolvedPathKindCache::Clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    values_.clear();
}

size_t ResolvedPathKindCache::Size() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return values_.size();
}

size_t ResolvedPathKindCache::EstimatedMemoryUsage() const {
    std::unique_lock<std::mutex> lock(mutex_);
    size_t total = sizeof(*this);
    total += values_.bucket_count() * sizeof(void*);
    for (const auto& entry : values_) {
        total += sizeof(entry);
        total += entry.first.capacity();
    }
    return total;
}

}  // namespace fm
