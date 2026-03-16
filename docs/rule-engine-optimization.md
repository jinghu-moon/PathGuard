# rule_engine / path_mapper 热路径优化建议

基于对 `rule_engine.cpp`、`path_mapper.cpp`、`path_kind_cache.cpp` 的完整阅读。  
优先级按实际调用频率和分配代价排列，从最高收益到最低。

---

## 优化一：PathKindCache 透明哈希（消除热循环内的 string 构造）

**问题**（`path_kind_cache.cpp:7` / `rule_engine.cpp:441`）

`TryGet` 和 `Put` 的签名接收 `std::string_view`，但内部立刻将其构造为 `std::string`：

```cpp
// path_kind_cache.cpp — 现状
auto it = values_.find(std::string(path));  // 每次都堆分配
values_[std::string(path)] = kind;           // 同上
```

`ResolveEffectiveRuleKind` 在 `match_rules` 循环内为**每条规则**都调用缓存，而 `rule.path` 已经是 `std::string`，传给 `TryGet(std::string_view)` 再被复制是纯粹浪费。

**根本原因**：`unordered_map<std::string, ...>` 的 `find` 在 C++17 之前不支持异质查找（heterogeneous lookup）。

**修正**：为 `unordered_map` 提供透明哈希器，让 `string_view` 直接查找，零分配。

```cpp
// path_kind_cache.h

namespace fm {

// 透明哈希：同时接受 std::string 和 std::string_view
struct TransparentStringHash {
    using is_transparent = void;  // 启用异质查找的标记
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

class ResolvedPathKindCache {
    // ...
private:
    mutable std::shared_mutex mutex_;
    // 第三参数：哈希器；第四参数：比较器（std::equal_to<> 同样 is_transparent）
    std::unordered_map<std::string, PathKind,
                       TransparentStringHash,
                       std::equal_to<>> values_;
};

}  // namespace fm
```

```cpp
// path_kind_cache.cpp — 修正后（所有 std::string(path) 移除）

bool ResolvedPathKindCache::TryGet(std::string_view path, PathKind* out_kind) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = values_.find(path);   // 直接传 string_view，零分配
    if (it == values_.end()) return false;
    if (out_kind != nullptr) *out_kind = it->second;
    return true;
}

void ResolvedPathKindCache::Put(std::string_view path, PathKind kind) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    values_.insert_or_assign(std::string(path), kind);  // 只在真正插入时分配
    // 注：insert_or_assign 比 operator[] 更清晰，且在 key 已存在时不重复分配 key
}

void ResolvedPathKindCache::Erase(std::string_view path) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    values_.erase(path);            // 零分配
}
```

**收益**：`match_rules` 循环内每条 kAuto 规则减少 1-2 次堆分配（TryGet + 可能的 Put）。  
**代价**：零。C++17 标准特性，无任何运行时额外开销。  
**要求**：C++17（`APP_CFLAGS := -std=c++17` 已满足）。

---

## 优化二：NormalizeRulePath 热路径消除中间 string 分配

**问题**（`rule_engine.cpp:59-95`）

`MatchPath` 的第一件事（`rule_engine.cpp:1089`）是对每个输入路径调用 `NormalizeRulePath`。  
对于最常见的外部存储路径（已经以 `/storage/emulated/0` 开头），当前代码的分配链为：

```cpp
// 现状（外部路径快速路径）：最多 3 次堆分配

const std::string trimmed(Trim(input));  // 分配 #1：从 string_view 构造 string
// ...
combined = trimmed;                       // 分配 #2：复制 trimmed
while (...) combined.pop_back();
*output = combined;                       // 分配 #3：复制到 output
```

`Trim` 返回的是 `std::string_view`（指向原始 `input` 的子范围），完全不需要物化为 `std::string`。

**修正**：第一步改为 `string_view`，快速路径减少到 1 次分配。

```cpp
// rule_engine.cpp — NormalizeRulePath 修正

bool NormalizeRulePath(std::string_view input, std::string* output) {
    if (output == nullptr) return false;

    // 修正：Trim 返回 string_view，无需物化为 std::string
    std::string_view trimmed = Trim(input);
    if (trimmed.empty()) return false;

    constexpr size_t kRootLen = sizeof(kExternalRoot) - 1;

    if (trimmed[0] == '/') {
        if (StartsWith(trimmed, kExternalRoot)
            && (trimmed.size() == kRootLen || trimmed[kRootLen] == '/')) {
            // 快速路径：在 string_view 上直接去尾部斜杠，零分配
            while (trimmed.size() > kRootLen && trimmed.back() == '/') {
                trimmed.remove_suffix(1);
            }
            output->assign(trimmed.data(), trimmed.size());  // 只此一次分配
            return true;
        }
        // 非外部绝对路径：走 fm_normalize_path（原有逻辑）
        char normalized[1024] = {};
        if (!fm_normalize_path(std::string(trimmed).c_str(), normalized, sizeof(normalized))) {
            return false;
        }
        output->assign(normalized);
        return true;
    }

    // 相对路径：拼接 kExternalRoot 前缀
    std::string combined;
    combined.reserve(kRootLen + 1 + trimmed.size());
    combined.assign(kExternalRoot);
    combined.push_back('/');
    combined.append(trimmed);

    char normalized[1024] = {};
    if (!fm_normalize_path(combined.c_str(), normalized, sizeof(normalized))) {
        return false;
    }
    output->assign(normalized);
    return true;
}
```

**收益**：外部路径快速路径从 3 次堆分配降至 1 次（仅 `output->assign()`），此路径覆盖绝大多数调用。  
**代价**：无。`string_view::remove_suffix` 是指针运算，零开销。

---

## 优化三：MatchPath 内 InferContextKind 提取出循环

**问题**（`rule_engine.cpp:1105` / `rule_engine.cpp:432`）

`match_rules` lambda 对 bucket 内每条规则都调用 `ResolveEffectiveRuleKind`，后者每次都调用 `InferContextKind(context)`：

```cpp
// ResolveEffectiveRuleKind（现状，line 424-453）
PathKind inferred = InferContextKind(context);   // context 在整个 MatchPath 内恒定
if (inferred != PathKind::kAuto && input_path == rule.path) { ... }
// ...
if (inferred != PathKind::kAuto) { ... }
```

`context` 在整个 `MatchPath` 调用期间不变，`InferContextKind` 的结果是常量，但被重复计算了「rules 条数」次。

**修正**：在 `MatchPath` 内提前计算一次，以参数传入。

```cpp
// rule_engine.cpp — MatchPath 修正

MatchResult MatchPath(const AppPolicy& policy,
                      std::string_view input_path,
                      const RuntimeContext& context,
                      ResolvedPathKindCache* cache) {
    // ...（快速返回检查）

    std::string normalized_input;
    if (!NormalizeRulePath(input_path, &normalized_input)) { ... }

    // 提取到循环外：整个 MatchPath 内 context_kind 是常量
    const PathKind context_kind = InferContextKind(context);

    if (cache != nullptr && context_kind != PathKind::kAuto) {
        cache->Put(normalized_input, context_kind);
    }

    auto match_rules = [&](const std::vector<const CompiledRule*>& rules) -> bool {
        for (const CompiledRule* rule : rules) {
            if (rule == nullptr) continue;

            // 传入预计算的 context_kind，不再重复调用 InferContextKind
            PathKind effective_kind =
                ResolveEffectiveRuleKind(*rule, normalized_input, context_kind, cache);
            // ...（匹配逻辑不变）
        }
        return false;
    };
    // ...
}
```

同时更新 `ResolveEffectiveRuleKind` 签名，接受 `PathKind context_kind` 而不是 `const RuntimeContext&`：

```cpp
// 修正后的 ResolveEffectiveRuleKind
PathKind ResolveEffectiveRuleKind(const CompiledRule& rule,
                                  std::string_view input_path,
                                  PathKind context_kind,   // 改：直接传入，不再推导
                                  ResolvedPathKindCache* cache) {
    if (rule.path_kind != PathKind::kAuto) {
        return rule.path_kind;  // 大多数规则在此直接返回（最快路径）
    }

    if (context_kind != PathKind::kAuto && input_path == rule.path) {
        if (cache != nullptr) cache->Put(rule.path, context_kind);
        return context_kind;
    }

    PathKind cached = PathKind::kAuto;
    if (cache != nullptr && cache->TryGet(rule.path, &cached) && cached != PathKind::kAuto) {
        return cached;
    }

    if (context_kind != PathKind::kAuto) {
        if (cache != nullptr) cache->Put(rule.path, context_kind);
        return context_kind;
    }

    return PathKind::kDirectory;
}
```

**收益**：每次 `MatchPath` 减少「bucket 内规则数」次 `InferContextKind` 调用（switch 语句，轻量，但在高频路径上累积显著）。同时消除 `RuntimeContext` 在 lambda capture 中的无谓引用。  
**代价**：`ResolveEffectiveRuleKind` 签名变化，需同步更新 `.h` 文件。

---

## 优化四：ShouldHideDirEntry 消除两次 string 构造

**问题**（`rule_engine.cpp:1193`）

`ShouldHideDirEntry` 在目录枚举时对每个条目调用，内部有两次不必要的 `std::string` 构造：

```cpp
// 现状（line 1193）
if (!fm_join_path(std::string(dir_path).c_str(),   // 分配 #1
                  std::string(entry_name).c_str(),  // 分配 #2
                  child_path, sizeof(child_path))) {
```

`fm_join_path` 需要 `const char*`，但 `dir_path` 和 `entry_name` 是 `string_view`，不一定 null-terminated，所以不得不构造 `std::string`。

然而在 `ShouldHideDirEntry` 的调用场景中，`dir_path` 通常是已归一化的完整路径，`entry_name` 是文件系统目录项名（无斜杠、无空字节）。可以直接在栈上拼接，跳过 `fm_join_path`：

```cpp
bool ShouldHideDirEntry(const AppPolicy& policy,
                        std::string_view dir_path,
                        std::string_view entry_name,
                        ResolvedPathKindCache* cache) {
    if (fm_is_dot_or_dotdot(entry_name.data())) return false;  // 快速跳过 . 和 ..

    // 直接在栈缓冲区拼接，无堆分配
    char child_path[1024];
    size_t dir_len = dir_path.size();
    size_t name_len = entry_name.size();
    bool add_slash = dir_len > 0 && dir_path[dir_len - 1] != '/';
    size_t total = dir_len + (add_slash ? 1 : 0) + name_len;
    if (total >= sizeof(child_path)) return false;

    memcpy(child_path, dir_path.data(), dir_len);
    if (add_slash) child_path[dir_len++] = '/';
    memcpy(child_path + dir_len, entry_name.data(), name_len);
    child_path[dir_len + name_len] = '\0';

    RuntimeContext context;
    context.operation = PathOperation::kEnumerateDirectory;
    MatchResult result = MatchPath(policy, child_path, context, cache);
    return result.decision == MatchDecision::kBlock;
}
```

**收益**：目录枚举场景下每个条目减少 2 次堆分配。目录内容多时（如 DCIM/Camera 下数百张照片）效果显著。  
**代价**：`fm_is_dot_or_dotdot` 需在调用之前能接受非 null-terminated 的 `string_view`；当前其实现接受 `const char*`，需传 `.data()` 或改签名（entry_name 在目录枚举场景下通常来自 `dirent::d_name`，已 null-terminated）。

---

## 优化五：AppPolicy 预计算 App 私有路径根

**问题**（`rule_engine.cpp:326-327`）

`ShouldImplicitlyAllowWhitelistPath` 在白名单模式下每次未命中都调用 `IsAppOwnedExternalPath`，后者每次都构造两个临时 `std::string`：

```cpp
// IsAppOwnedExternalPath（现状，line 326-327）— 每次调用都分配
const std::string data_root = std::string(kExternalRoot) + "/Android/data/" + std::string(package_name);
const std::string media_root = std::string(kExternalRoot) + "/Android/media/" + std::string(package_name);
```

`package_name` 在 `AppPolicy` 编译后就是固定值，这两个字符串应当预计算一次。

**修正**：在 `AppPolicy` 中增加两个预计算字段，在 `CompilePolicyForProcess` 中填充。

```cpp
// rule_engine.h — AppPolicy 新增字段
struct AppPolicy {
    std::string package_name;
    PolicyMode mode = PolicyMode::kBlacklist;
    // ...（现有字段不变）

    // 新增：预计算，避免热路径重复构造
    std::string owned_data_root;   // "/storage/emulated/0/Android/data/<pkg>"
    std::string owned_media_root;  // "/storage/emulated/0/Android/media/<pkg>"
};
```

```cpp
// rule_engine.cpp — CompilePolicyForProcess 末尾填充
out_policy->owned_data_root  = std::string(kExternalRoot) + "/Android/data/"  + out_policy->package_name;
out_policy->owned_media_root = std::string(kExternalRoot) + "/Android/media/" + out_policy->package_name;
```

```cpp
// rule_engine.cpp — IsAppOwnedExternalPath 修正（改为接受 AppPolicy）
bool IsAppOwnedExternalPath(const AppPolicy& policy, std::string_view normalized_input) {
    if (policy.package_name.empty() || !IsExternalPath(normalized_input)) return false;
    return IsSameOrParentPathOf(normalized_input, policy.owned_data_root)
        || IsDirectoryMatch(normalized_input, policy.owned_data_root)
        || IsSameOrParentPathOf(normalized_input, policy.owned_media_root)
        || IsDirectoryMatch(normalized_input, policy.owned_media_root);
}
```

同步更新 `ShouldImplicitlyAllowWhitelistPath`，将 `policy.package_name` 的传入改为传整个 `policy`（已有 `const AppPolicy& policy` 参数，直接改调用即可）。

**收益**：白名单模式下每次路径未命中减少 2 次堆分配（路径构造）+ `std::string(package_name)` 的构造。  
**代价**：`AppPolicy` 增加 2 个 `std::string` 字段（约 80 字节，可忽略）；`IsAppOwnedExternalPath` 签名变化（影响范围小，仅在匿名 namespace 内）。

---

## 汇总

| 优先级 | 优化点 | 调用位置 | 每次 MatchPath 减少分配 | 改动范围 |
|---|---|---|---|---|
| ①最高 | PathKindCache 透明哈希 | 热循环内每条规则 | 1-2 次/规则 | path_kind_cache.h/.cpp |
| ② | NormalizeRulePath 去中间 string | 每次 MatchPath 开头 | 2 次 | rule_engine.cpp |
| ③ | 提取 InferContextKind | 热循环内每条规则 | 1 次/规则（计算节省）| rule_engine.cpp/.h |
| ④ | ShouldHideDirEntry 栈拼接 | 目录枚举每个条目 | 2 次/条目 | rule_engine.cpp |
| ⑤ | 预计算 owned 路径根 | 白名单每次 miss | 2 次/miss | rule_engine.h/.cpp |

**建议实施顺序**：① → ② → ⑤（改动最小，收益最直接）；③ 和 ④ 涉及签名变更，建议单独提交。
