# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v4.0

> **参考来源**：Magisk 官方文档 · Zygisk API（topjohnwu/zygisk-module-sample）· AOSP 存储与 SELinux 文档 · StorageRedirect · FakeXposed · rvmm-zygisk-mount · NoHello · ZygiskNext · ReZygisk · v3.0 系统审查报告 · rule-format-plan.md（规则格式专项设计）
>
> **v4 核心变更**：全新规则格式（INI + 黑白名单 + `+/-/->` 语法）· 规则引擎升级为 Trie 前缀树 · 编译期规则对象（运行时零字符串解析）· 三层路径类型推断 · 重定向支持 · 优先级冲突解决 · IPC fail-open 超时 · Ring buffer 日志 · Audit 模式 · 完整测试清单

---

## 目录

1. [方案总览](#1-方案总览)
2. [核心技术原理](#2-核心技术原理)
3. [整体架构](#3-整体架构)
4. [规则格式设计](#4-规则格式设计)
5. [规则引擎设计](#5-规则引擎设计)
6. [Mount 执行层设计](#6-mount-执行层设计)
7. [Companion Daemon 设计](#7-companion-daemon-设计)
8. [Zygisk 模块主流程](#8-zygisk-模块主流程)
9. [核心代码实现](#9-核心代码实现)
10. [日志系统设计](#10-日志系统设计)
11. [模块文件结构](#11-模块文件结构)
12. [构建与安装](#12-构建与安装)
13. [功能设计规划](#13-功能设计规划)
14. [测试与发布清单](#14-测试与发布清单)
15. [开发路线图](#15-开发路线图)
16. [参考资源](#16-参考资源)

---

## 1. 方案总览

### 1.1 核心方案（不变）

> **Zygisk + Per-App Mount Namespace + tmpfs 覆盖 + bind mount 隐蔽层**

在 `preAppSpecialize` 阶段（App 进程 fork 完成、任何 App 代码尚未执行之前），在 App 私有 Mount Namespace 内执行路径覆盖。

### 1.2 v4 相比 v3 的核心升级

| 维度 | v3 | v4 |
|---|---|---|
| **规则格式** | `pkg\|path` 单行格式，无白名单，无重定向 | INI 分组 + `mode` + `+/-/->` 极简语法 |
| **规则引擎** | `unordered_map` + 线性路径比对 | 编译期 `AppPolicy` 对象 + Trie 前缀树 |
| **路径类型** | 静态 stat，无运行时修正 | 三层推断（加载期 + 运行期 syscall + 缓存） |
| **重定向** | 不支持 | `src -> dst`，bind mount 实现 |
| **冲突解决** | 深路径优先，无优先级 | 路径长度 > 文件优先 > 动作优先级（deny > redirect > allow） |
| **IPC 超时** | 无，Companion 挂起则 App 阻塞 | fail-open 超时（默认 50ms），App 不受 Companion 影响 |
| **日志** | 同步写文件，无限速 | Ring buffer + 异步批量刷盘 + 采样策略 |
| **Audit 模式** | 无 | 支持，只记录"would block"，不实际拦截 |
| **STL** | 依赖 NDK STL | 遵循官方要求：`APP_STL=none`，使用 libcxx submodule |

### 1.3 三层防御体系

```
Layer 1（Phase 1，当前实现）
  tmpfs overlay + bind mount 隐蔽层 + 重定向支持
  → App 看到空目录（deny）/ 重定向目录（redirect）
  → 覆盖所有等价存储路径

Layer 2（Phase 3，SELinux 加固）
  bind mount 保留原始 SELinux label
  → setfscreatecon 三级降级策略

Layer 3（Phase 2，可选 syscall fallback）
  openat hook 过滤 /proc/self/mountinfo
  → 针对主动读取 mountinfo 做检测的 App
```

---

## 2. 核心技术原理

### 2.1 Android 存储路径映射（必须完整覆盖）

`/sdcard` 是多层符号链接 + bind mount + FUSE 的映射体系：

```
/sdcard
  └─(symlink)→ /storage/self/primary
                 └─(bind)→ /mnt/user/{userId}/primary
                              └─(symlink)→ /storage/emulated/{userId}
                                              └─(bind)→ /mnt/runtime/{VIEW}/emulated/{userId}
                                                          └─(FUSE/sdcardfs)→ /data/media/{userId}
```

Android 6.0+ 三种存储视图（VIEW）：`default`、`read`、`write`，根据 App 权限注入不同视图。**必须同时覆盖所有等价路径**。

### 2.2 Zygisk 切入时机

`preAppSpecialize` 是唯一正确的 mount 操作时机：

```
Zygote fork App
    │
    ├─ [Zygisk] preAppSpecialize()   ← ✅ 在此操作
    │    mount namespace 已独立，沙盒尚未启用
    │
    ├─ App specialize（UID drop、SELinux 域切换）
    ├─ [Zygisk] postAppSpecialize()  ← ❌ 沙盒已启用
    └─ App 代码执行
```

### 2.3 tmpfs + bind mount 双层隐蔽

```
Step 1: mkdir  /data/adb/modules/path_guard/.tmp/pg_{random}
Step 2: mount  tmpfs → /data/adb/modules/path_guard/.tmp/pg_{random}
Step 3: mount  --bind → /storage/emulated/0/secret

// mountinfo 中呈现为 bind mount，而非直接 tmpfs，不易触发检测
```

### 2.4 重定向实现原理

```
用户规则：DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera

实现步骤：
Step 1: 确认目标（dst）路径存在，或创建它
Step 2: bind mount dst → src
        即：把真实的 dst 内容绑定到 src 位置
        App 访问 src（DCIM/Camera），看到的是 dst 的内容
```

---

## 3. 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                         PathGuard v4                                 │
│                                                                      │
│  ┌─────────────────────┐     ┌────────────────────────────────────┐  │
│  │   Zygisk Module     │     │        Companion Daemon            │  │
│  │  (libpathguard.so)  │◄───►│     (root 守护进程，常驻)           │  │
│  │                     │     │                                    │  │
│  │ preAppSpecialize:   │     │  ┌──────────────────────────────┐  │  │
│  │  1. IPC (50ms 超时)  │     │  │       Rule Engine            │  │  │
│  │  2. 获取 AppPolicy  │     │  │  • INI 解析器                 │  │  │
│  │  3. MS_PRIVATE      │     │  │  • AppPolicy 编译器           │  │  │
│  │  4. 路径展开+去重    │     │  │  • Trie 前缀树（路径匹配）     │  │  │
│  │  5. 深路径排序       │     │  │  • 三层路径类型推断           │  │  │
│  │  6. RAII mount      │     │  │  • 优先级冲突解决             │  │  │
│  │  7. 处理重定向       │     │  │  • shared_mutex 读写分离     │  │  │
│  └─────────────────────┘     │  └──────────────────────────────┘  │  │
│                              │  ┌──────────────────────────────┐  │  │
│                              │  │       Log System             │  │  │
│                              │  │  • Ring buffer (异步)        │  │  │
│                              │  │  • 采样策略（deny > allow）   │  │  │
│                              │  │  • 批量刷盘（每秒 or 阈值）   │  │  │
│                              │  │  • 日志轮转（1MB，保留 3 个） │  │  │
│                              │  └──────────────────────────────┘  │  │
│                              └────────────────────────────────────┘  │
│                                              ▲                       │
│                                              │ Unix Socket (IPC)     │
│  ┌─────────────────────┐     ┌──────────────┴─────────────────────┐  │
│  │  Path Resolution    │     │          Manager App               │  │
│  │  Cache（per-app）   │     │  • 规则编辑器（INI 可视化）         │  │
│  │                     │     │  • Audit 日志查看                  │  │
│  │ • 展开后路径集合     │     │  • mount 状态可视化               │  │
│  │ • 已 mount 路径集合  │     │  • 规则冲突检视                   │  │
│  │ • 路径类型缓存       │     │  • 规则测试（spawn test proc）    │  │
│  └─────────────────────┘     └────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 4. 规则格式设计

### 4.1 设计原则

来自 rule-format-plan.md 的核心设计目标：

- 用户一眼能看懂，手写成本极低
- 不要求用户理解 `tree` / `exact` 等实现细节
- 支持黑名单与白名单两种模式
- 相对路径自动补全，支持中文和空格路径
- 对外极简，对内编译成高效结构化对象

### 4.2 规则文件格式

```ini
# PathGuard 规则文件 v4
# 文件位置：config/rules.ini
# 编码：UTF-8，LF 换行
#
# 三句话记住全部规则：
#   + 路径              → 允许访问
#   - 路径              → 禁止/隐藏
#   源路径 -> 目标路径   → 重定向
#
# 路径不以 / 开头时，自动补全为 /storage/emulated/0/<路径>
# 程序会自动推断路径是文件还是文件夹
# 程序会自动展开所有等价存储路径

[com.tencent.mm]
mode = whitelist

+ Pictures/Share
+ Download/Send
+ DCIM/Camera

- DCIM/A-TEST
- Download/private.txt

DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
Movies/Clip -> Android/data/com.tencent.mm/cache/Clip

[com.tencent.mobileqq]
mode = blacklist

- DCIM/敏感目录
- Download/private
Tencent/QQfile_recv -> Android/data/com.tencent.mobileqq/files/hidden
```

### 4.3 语法规范

**分组**：使用真实包名，不使用别名（UI 层可显示别名）
```ini
[com.tencent.mm]
```

**`mode`**：必填，无默认值，强制显式声明
```ini
mode = whitelist    # 默认拒绝，只有 + 的路径可见
mode = blacklist    # 默认允许，只有 - 的路径被屏蔽
```

**`+ 路径`**：允许访问，用于 whitelist 模式显式放行
```ini
+ Pictures/Share
+ Download/Send/file.txt
```

**`- 路径`**：禁止访问，App 看到空目录或文件不存在
```ini
- DCIM/A-TEST
- Movies/Nagram
- Download/private.txt
```

**`源 -> 目标`**：访问源路径时重定向到目标路径（bind mount 实现）
```ini
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
My Album/A -> Android/data/com.tencent.mm/files/A
```

### 4.4 路径补全与规范化

**相对路径**：自动补全为 `/storage/emulated/0/<路径>`
```
Pictures/Share → /storage/emulated/0/Pictures/Share
```

**绝对路径**：保持原样
```
/data/media/0/DCIM/A-TEST → 不做补全
```

**路径规范化**（加载期统一处理）：
```
/sdcard/... → /storage/emulated/0/...
/storage/self/primary/... → /storage/emulated/0/...
去除末尾多余 /
realpath() 规范化（路径存在时）
```

**空格与中文路径**：
```ini
# + / - 行：去掉前缀符号后，剩余整行视为路径
- DCIM/My Album
- DCIM/隐私目录

# 重定向行：按第一个 -> 切分
My Album/Camera -> Android/data/com.tencent.mm/cache/My Album/Camera
```

### 4.5 路径类型推断（三层）

来自 rule-format-plan.md 第 7 节，这是核心设计之一：

**绝不在热路径（每次 mount 判断时）重复做磁盘探测，只在加载期判断一次。**

#### 第一层：加载期静态推断

按优先级顺序：

1. **尾部斜杠提示**：`DCIM/A-TEST/` → 直接视为目录
2. **`lstat()` 检查**：路径已存在时，`S_ISDIR` → 目录，`S_ISREG` → 文件
3. **扩展名启发式**：路径不存在但有 `.txt/.jpg/.png/.mp4/.db/.json/.xml/.apk` 等扩展名 → 推断为文件
4. **默认兜底**：既无斜杠，又不存在，也无扩展名 → **默认视为目录**（用户写无扩展名路径绝大多数是目录）

#### 第二层：运行期 syscall 上下文修正（Layer 3 / Phase 2）

当 openat hook 启用时，结合 syscall 语义修正类型缓存：

| syscall 行为 | 推断 |
|---|---|
| `mkdir()` / `mkdirat()` | → 目录 |
| `rmdir()` | → 目录 |
| `opendir()` / `open(O_DIRECTORY)` | → 目录 |
| `open(O_CREAT)` 且无 `O_DIRECTORY` | → 文件 |
| `fopen()` / `fopen64()` | → 文件 |

#### 第三层：内存路径类型缓存

```cpp
// Companion 端全局缓存
std::unordered_map<std::string, PathKind> g_path_kind_cache;
// PathKind: DIR | FILE | AUTO
// 加载期静态推断结果写入；运行期 syscall 推断结果更新
```

---

## 5. 规则引擎设计

### 5.1 内部 AppPolicy 编译结构

规则只在配置加载期解析一次，运行时（`preAppSpecialize`）只使用编译好的结构对象，**不做任何字符串解析**：

```cpp
enum class Action { ALLOW, DENY, REDIRECT };
enum class PathKind { FILE, DIR, AUTO };

struct CompiledRule {
    Action    action;
    PathKind  kind;
    std::string path;          // 规范化后的绝对路径
    std::string redirectTo;    // 仅 REDIRECT 时有效
    int         priority;      // 路径长度（更长 = 更高优先级）
};

struct AppPolicy {
    std::string package;
    Mode        mode;          // WHITELIST | BLACKLIST
    std::vector<CompiledRule> rules;  // 已按优先级降序排列
    // 快速查找索引（由 Trie 提供）
};
```

完整编译过程：
```
读取 rules.ini
    → 解析 INI 分组
    → 路径补全（相对 → 绝对）
    → 路径规范化（alias 统一）
    → 三层路径类型推断
    → 展开等价路径（/sdcard/* → 6条路径）
    → 去重
    → 计算优先级（路径深度）
    → 排序（深路径优先）
    → 编译为 AppPolicy 对象
    → 存入 Companion g_policies（unordered_map）
```

### 5.2 Trie 前缀树路径匹配

参考文件系统过滤驱动的最佳实践：规则匹配复杂度应为 `O(path_length)` 而非 `O(rule_count)`，否则在规则数量较多时性能会线性下降。

```cpp
struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;
    CompiledRule *rule = nullptr;  // 若此节点有规则，指向它
};

class PathTrie {
    TrieNode root;
public:
    void insert(const std::string &path, CompiledRule *rule) {
        // 按 '/' 分割路径，逐层插入节点
        auto *cur = &root;
        for (auto &seg : splitPath(path)) {
            auto &child = cur->children[seg];
            if (!child) child = std::make_unique<TrieNode>();
            cur = child.get();
        }
        cur->rule = rule;
    }

    // 查找：返回最长前缀匹配的规则（最具体的规则优先）
    const CompiledRule *match(const std::string &path) const {
        auto *cur = &root;
        const CompiledRule *best = nullptr;
        for (auto &seg : splitPath(path)) {
            auto it = cur->children.find(seg);
            if (it == cur->children.end()) break;
            cur = it->second.get();
            if (cur->rule) best = cur->rule;  // 更深的规则覆盖更浅的
        }
        return best;
    }
};
```

**复杂度**：
- 构建：`O(total_path_chars)`，一次性
- 查找：`O(query_path_length)`，与规则数量无关

### 5.3 优先级与冲突解决

来自 rule-format-plan.md 第 11 节，v4 严格实现三级优先级：

1. **路径更长的规则优先**（更具体覆盖更泛）
2. **同路径：文件规则优先于目录规则**
3. **同具体程度：动作优先级为 `DENY > REDIRECT > ALLOW`**

```cpp
const CompiledRule *resolveConflict(const CompiledRule *a, const CompiledRule *b) {
    // 1. 路径更深的优先
    if (a->priority != b->priority) return a->priority > b->priority ? a : b;
    // 2. 文件规则优先于目录规则
    if (a->kind != b->kind) {
        return a->kind == PathKind::FILE ? a : b;
    }
    // 3. 动作优先级
    auto rank = [](Action act) {
        switch (act) {
            case Action::DENY:     return 3;
            case Action::REDIRECT: return 2;
            case Action::ALLOW:    return 1;
        }
        return 0;
    };
    return rank(a->action) >= rank(b->action) ? a : b;
}
```

### 5.4 目录路径的安全匹配

来自 rule-format-plan.md 第 9.1 节：目录规则不能直接用 `startsWith(rule_path)` 进行前缀匹配，否则会误伤兄弟路径。

```
规则：/storage/emulated/0/DCIM/A-TEST

✅ 匹配：/storage/emulated/0/DCIM/A-TEST
✅ 匹配：/storage/emulated/0/DCIM/A-TEST/photo.jpg
✅ 匹配：/storage/emulated/0/DCIM/A-TEST/sub/dir

❌ 不匹配：/storage/emulated/0/DCIM/A-TEST_backup  ← 兄弟路径，不能误伤
```

正确匹配语义：
```cpp
bool matchDir(const std::string &rulePath, const std::string &queryPath) {
    return queryPath == rulePath ||
           queryPath.rfind(rulePath + "/", 0) == 0;  // 必须加 "/" 分隔符
}
```

### 5.5 黑白名单模式的判定逻辑

来自 rule-format-plan.md 第 10 节：

**BLACKLIST 模式**（默认允许）：
```
命中 DENY 规则       → 拒绝访问
命中 REDIRECT 规则   → 执行重定向
命中 ALLOW 规则      → 放行（显式允许，优先级高于默认放行）
未命中任何规则       → 放行
```

**WHITELIST 模式**（默认拒绝）：
```
命中 ALLOW 规则      → 放行
命中 REDIRECT 规则   → 放行并执行重定向
命中 DENY 规则       → 拒绝（显式拒绝，优先于默认拒绝）
未命中任何规则       → 拒绝（tmpfs 覆盖）
```

---

## 6. Mount 执行层设计

### 6.1 完整执行流程

```
preAppSpecialize(args)
    │
    ├─ 1. connectCompanion() → 获取 AppPolicy（50ms 超时，超时则 fail-open）
    │
    ├─ 2. policy.mode == WHITELIST?
    │       是 → 对所有存储根路径执行 deny（默认拒绝）
    │       否 → 仅处理显式规则
    │
    ├─ 3. isolateMountNamespace()（MS_REC | MS_PRIVATE）
    │       失败 → 放弃全部操作，记录日志
    │
    ├─ 4. 对每条 CompiledRule：
    │       → resolveStoragePaths()：展开等价路径
    │       → buildFinalPathList()：规范化 + 去重 + 深路径排序
    │       → 按 action 分发：
    │             DENY     → applyDeny()（tmpfs overlay）
    │             REDIRECT → applyRedirect()（bind dst → src）
    │             ALLOW    → 在 WHITELIST 模式下对此路径放行
    │
    └─ 5. 异步记录日志（不阻塞主流程）
```

### 6.2 Deny（tmpfs 覆盖）

见 v3 的完整实现，v4 保持不变：
- RAII `MountGuard` 自动回滚
- 模块私有目录 `/data/adb/modules/path_guard/.tmp/pg_{random}`
- `getrandom()` 安全随机数
- SELinux 三级降级策略
- 完整 errno 分类处理

### 6.3 Redirect（bind mount 重定向）

```cpp
void applyRedirect(const std::string &src, const std::string &dst) {
    // src: 用户访问的路径（如 /storage/emulated/0/DCIM/Camera）
    // dst: 重定向目标（如 /storage/emulated/0/Android/data/com.tencent.mm/cache/Camera）

    // 确保目标路径存在
    if (mkdirs(dst.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE("redirect: cannot create dst: %s (%s)", dst.c_str(), strerror(errno));
        return;
    }

    // 确保源路径存在（若不存在则创建占位目录）
    if (mkdirs(src.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE("redirect: cannot create src: %s (%s)", src.c_str(), strerror(errno));
        return;
    }

    // bind mount：访问 src 时看到的是 dst 的内容
    if (mount(dst.c_str(), src.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        LOGE("redirect bind mount failed: %s -> %s (%d: %s)",
             src.c_str(), dst.c_str(), errno, strerror(errno));
        return;
    }

    LOGI("redirect: %s -> %s", src.c_str(), dst.c_str());
}
```

### 6.4 WHITELIST 模式的存储根覆盖

在 whitelist 模式下，需要先将整个存储根用 tmpfs 覆盖（拒绝所有默认访问），再对 `ALLOW` 规则的路径使用 bind mount 恢复真实内容：

```cpp
void applyWhitelistMode(const AppPolicy &policy, int userId) {
    std::string storageRoot = "/storage/emulated/" + std::to_string(userId);

    // 1. 覆盖整个存储根（拒绝默认）
    applyDeny(storageRoot, ...);

    // 2. 对 ALLOW 规则：bind mount 恢复真实内容
    for (auto &rule : policy.rules) {
        if (rule.action == Action::ALLOW) {
            // 在已覆盖的 tmpfs 上创建目录，再 bind mount 真实路径
            std::string inTmpfs = storageRoot + "/" + rule.relPath;
            mkdirs(inTmpfs.c_str(), rule.mode);
            mount(rule.path.c_str(), inTmpfs.c_str(), nullptr, MS_BIND | MS_REC, nullptr);
        }
    }
}
```

### 6.5 IPC fail-open 超时

**来自审查报告核心建议**：若 Companion 进程挂起，绝不能让 App 无限等待。

```cpp
// 在 preAppSpecialize 中
AppPolicy policy;
int fd = api->connectCompanion();
if (fd < 0) {
    LOGW("Companion unavailable, fail-open for %s", pkg.c_str());
    return;  // fail-open：Companion 不可用时放行 App
}

// 设置 50ms 超时
struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

sendString(fd, pkg);
bool ok = receivePolicy(fd, &policy);
close(fd);

if (!ok) {
    LOGW("IPC timeout, fail-open for %s", pkg.c_str());
    return;  // 超时则 fail-open，App 正常启动
}

// policy 有效，继续 mount 操作
```

---

## 7. Companion Daemon 设计

### 7.1 规则加载与热更新

```cpp
// 全局状态（shared_mutex 保护）
static std::shared_mutex g_mutex;
static std::unordered_map<std::string, AppPolicy> g_policies;
static std::atomic<bool> g_reloading{false};

void reloadRules() {
    bool expected = false;
    if (!g_reloading.compare_exchange_strong(expected, true)) return;

    auto newPolicies = compileRulesFile(RULES_PATH);  // 完整编译流程

    {
        std::unique_lock lk(g_mutex);
        g_policies = std::move(newPolicies);
    }

    g_reloading.store(false);
    LOGI("Rules reloaded: %zu policies", g_policies.size());
}

void companionHandler(int fd) {
    std::string pkg = receiveString(fd);

    {
        std::shared_lock lk(g_mutex);  // 读锁：并发查询安全
        auto it = g_policies.find(pkg);
        if (it != g_policies.end()) {
            sendPolicy(fd, it->second);
        } else {
            sendEmptyPolicy(fd);  // 无规则 = fail-open
        }
    }

    logAccessAsync(pkg);  // 异步，不阻塞 IPC
}

__attribute__((constructor)) void companionInit() {
    mkdir(TMP_ROOT.c_str(), 0700);
    cleanupStaleTmpDirs();
    reloadRules();
    signal(SIGUSR1, [](int) { reloadRules(); });
}
```

### 7.2 规则编译器

```cpp
std::unordered_map<std::string, AppPolicy> compileRulesFile(const char *path) {
    std::unordered_map<std::string, AppPolicy> policies;
    IniParser parser(path);
    std::string curPkg;

    while (parser.nextLine()) {
        if (parser.isSection()) {
            curPkg = parser.sectionName();
            policies[curPkg].package = curPkg;
            continue;
        }
        if (curPkg.empty()) continue;

        auto &policy = policies[curPkg];

        if (parser.isMode()) {
            policy.mode = parser.mode();
        } else if (parser.isAllow()) {
            compileRule(policy, Action::ALLOW, parser.path());
        } else if (parser.isDeny()) {
            compileRule(policy, Action::DENY, parser.path());
        } else if (parser.isRedirect()) {
            compileRedirect(policy, parser.srcPath(), parser.dstPath());
        }
    }

    // 编译完成后对每个 policy 排序
    for (auto &[pkg, policy] : policies) {
        sortRules(policy.rules);
        buildTrie(policy);
    }

    return policies;
}

void compileRule(AppPolicy &policy, Action action, const std::string &rawPath) {
    std::string absPath = toAbsolutePath(rawPath);  // 相对 → 绝对
    absPath = normalizePath(absPath);                // 规范化（alias 统一）

    PathKind kind = inferPathKind(absPath);          // 三层类型推断（加载期）
    int priority  = countSlashes(absPath);           // 路径深度 = 优先级

    policy.rules.push_back({ action, kind, absPath, "", priority });
}
```

---

## 8. Zygisk 模块主流程

根据 [Zygisk 官方文档](https://github.com/topjohnwu/zygisk-module-sample)，`APP_STL=none`，使用官方 libcxx submodule：

```cpp
#include "zygisk.hpp"
#include "companion.h"
#include "mount.h"
#include "path_utils.h"

using namespace zygisk;

class PathGuard : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args->nice_name) return;

        // 获取包名
        const char *pkgC = env->GetStringUTFChars(args->nice_name, nullptr);
        std::string pkg(pkgC);
        env->ReleaseStringUTFChars(args->nice_name, pkgC);

        // 从 Companion 获取 AppPolicy（50ms 超时，fail-open）
        AppPolicy policy;
        if (!fetchPolicy(pkg, &policy)) return;

        // 阻断 mount propagation（失败则放弃）
        if (!isolateMountNamespace()) return;

        int userId = static_cast<int>(args->uid / 100000);

        // WHITELIST 模式：先覆盖存储根，再按规则放行
        if (policy.mode == Mode::WHITELIST) {
            applyWhitelistMode(policy, userId);
            return;
        }

        // BLACKLIST 模式：仅处理 DENY 和 REDIRECT 规则
        for (auto &rule : policy.rules) {
            auto allPaths = buildFinalPathList(rule.path, userId);
            for (auto &p : allPaths) {
                if (rule.action == Action::DENY) {
                    applyOverlayFull(p, getMissingPolicy());
                } else if (rule.action == Action::REDIRECT) {
                    auto dstPaths = buildFinalPathList(rule.redirectTo, userId);
                    if (!dstPaths.empty()) applyRedirect(p, dstPaths[0]);
                }
            }
        }
    }

private:
    Api *api;
    JNIEnv *env;

    bool fetchPolicy(const std::string &pkg, AppPolicy *out) {
        int fd = api->connectCompanion();
        if (fd < 0) return false;

        // 50ms 超时
        struct timeval tv = { 0, 50000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sendString(fd, pkg);
        bool ok = receivePolicy(fd, out);
        close(fd);
        return ok && !out->rules.empty();
    }
};

REGISTER_ZYGISK_MODULE(PathGuard)
REGISTER_ZYGISK_COMPANION(companionHandler)
```

---

## 9. 核心代码实现

### 9.1 项目源码结构

```
module/jni/
├── zygisk.hpp          # Zygisk API（来自官方模板，勿修改）
├── libcxx/             # 官方 libcxx submodule（APP_STL=none 时使用）
├── module.cpp          # Zygisk 模块主体
├── companion.cpp       # Companion 守护进程
├── rules/
│   ├── ini_parser.cpp  # INI 格式解析
│   ├── compiler.cpp    # 规则编译器（INI → AppPolicy）
│   ├── trie.cpp        # Trie 前缀树
│   └── policy.h        # AppPolicy / CompiledRule 结构体定义
├── mount/
│   ├── mount.cpp       # applyDeny / applyRedirect / isolateNS
│   ├── mount_guard.h   # RAII MountGuard
│   └── selinux.cpp     # SELinux 三级降级策略
├── utils/
│   ├── path_utils.cpp  # resolveStoragePaths / normalizePath / ...
│   ├── ipc.cpp         # IPC 收发（含 50ms 超时）
│   ├── random.cpp      # secureRandomHex（getrandom()）
│   └── log.cpp         # Ring buffer 日志
├── Android.mk
└── Application.mk
```

### 9.2 Android.mk

```makefile
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE        := path_guard
LOCAL_SRC_FILES     := module.cpp companion.cpp \
                       rules/ini_parser.cpp rules/compiler.cpp rules/trie.cpp \
                       mount/mount.cpp mount/selinux.cpp \
                       utils/path_utils.cpp utils/ipc.cpp \
                       utils/random.cpp utils/log.cpp
LOCAL_CFLAGS        := -std=c++17 -fvisibility=hidden \
                       -ffunction-sections -fdata-sections \
                       -Wall -Wextra -O2
LOCAL_LDFLAGS       := -Wl,--gc-sections
LOCAL_LDLIBS        := -llog -lselinux

# 使用官方 libcxx（APP_STL=none 时必须）
# 参考 zygisk-module-sample 的 Android.mk 配置方式
LOCAL_STATIC_LIBRARIES := libcxx

include $(BUILD_SHARED_LIBRARY)
$(call import-module, libcxx)
```

### 9.3 Application.mk

```makefile
APP_ABI      := arm64-v8a armeabi-v7a x86_64 x86
APP_PLATFORM := android-26       # Android 8.0+（getrandom() 最低要求）
APP_STL      := none             # 官方要求：不得修改
APP_CFLAGS   := -std=c++17
```

---

## 10. 日志系统设计

**核心原则**：日志写入绝不能在热路径（preAppSpecialize）中同步执行，否则每次 App 启动都会因 IO 延迟而卡顿。

### 10.1 Ring Buffer 架构

```
preAppSpecialize
    │
    └─ logAsync(event)           ← 写入 ring buffer（内存操作，微秒级）
                                    不做任何 IO

Companion 后台线程（每秒或 ring buffer 达到阈值时）
    ├─ 批量读取 ring buffer
    ├─ 过滤（采样策略）
    └─ 批量写入日志文件（一次 IO）
```

```cpp
// Ring buffer：固定大小，最旧的条目被最新条目覆盖
constexpr size_t RING_SIZE = 1024;
struct LogEntry { time_t ts; std::string pkg; std::string path; Action action; };
static std::array<LogEntry, RING_SIZE> g_ring;
static std::atomic<size_t> g_head{0};

void logAsync(const std::string &pkg, const std::string &path, Action action) {
    size_t idx = g_head.fetch_add(1) % RING_SIZE;
    g_ring[idx] = { time(nullptr), pkg, path, action };  // 原子写，无锁
}
```

### 10.2 采样策略

```
记录：DENY（被拦截的访问，总是记录）
记录：REDIRECT（重定向操作，总是记录）
采样：ALLOW（放行的访问，默认不记录，调试模式下采样 1%）
跳过：系统进程（package 为空或以 android 开头）
```

### 10.3 日志文件管理

```
路径：/data/adb/pathguard/debug.log
权限：0600（仅 root 可读，保护路径隐私）
轮转：超过 1MB 时 rotate（保留最近 3 个）
格式：2025-01-01T12:00:00 DENY com.tencent.mm /storage/emulated/0/DCIM/A-TEST
```

### 10.4 Audit 模式

```ini
# config/rules.ini 全局选项
[global]
audit = true    # 只记录"would block"，不实际执行 mount
```

Audit 模式下：
- 所有应该执行的 mount 操作改为只记录日志
- App 正常启动，正常访问所有路径
- 日志显示"WOULD_DENY"或"WOULD_REDIRECT"
- 便于在生产部署前验证规则正确性，不影响 App 可用性

---

## 11. 模块文件结构

```
path-guard/
├── module.prop              # 模块元信息
├── skip_mount               # 跳过 magic mount（无需 system 文件替换）
│
├── zygisk/                  # Zygisk native 库
│   ├── arm64-v8a.so
│   ├── armeabi-v7a.so
│   ├── x86_64.so
│   └── x86.so
│
├── config/
│   └── rules.ini            # 规则文件（新格式）
│
├── action.sh                # Magisk App「执行」按钮
└── uninstall.sh             # rm -rf .tmp/ 和日志目录
```

### module.prop

```properties
id=path_guard
name=PathGuard - 路径访问控制
version=v4.0.0
versionCode=4
author=YourName
description=透明隔离 App 对敏感路径的访问。支持黑白名单、重定向。Zygisk + mount namespace + tmpfs/bind overlay。
updateJson=https://your-server.com/path-guard/update.json
```

---

## 12. 构建与安装

```bash
# 1. 克隆官方模板（含 libcxx submodule）
git clone --recurse-submodules https://github.com/topjohnwu/zygisk-module-sample path-guard
cd path-guard

# 2. 替换 module/jni/ 为本项目源码

# 3. 编译（NDK r21+，官方最低要求）
cd module
$ANDROID_NDK_ROOT/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk

# 4. 复制产物
cp libs/arm64-v8a/libpath_guard.so   ../zygisk/arm64-v8a.so
cp libs/armeabi-v7a/libpath_guard.so ../zygisk/armeabi-v7a.so
cp libs/x86_64/libpath_guard.so      ../zygisk/x86_64.so
cp libs/x86/libpath_guard.so         ../zygisk/x86.so

# 5. 创建规则文件
cat > config/rules.ini << 'EOF'
[com.example.test]
mode = blacklist

- DCIM/private
EOF

# 6. 打包
cd ..
zip -r path-guard-v4.0.zip module.prop zygisk/ config/ action.sh uninstall.sh skip_mount

# 7. 安装 → Magisk Manager → 本地安装 → 重启

# 8. 热更新规则（无需重启）
adb shell "kill -USR1 \$(pidof magiskd)"

# 9. 查看日志
adb logcat -s PathGuard
adb shell cat /data/adb/pathguard/debug.log
```

---

## 13. 功能设计规划

### 13.1 当前阶段（Phase 1）支持的功能

按照 rule-format-plan.md 第 12 节"当前阶段边界"：

- INI 分组 + `mode = whitelist | blacklist`
- `+` / `-` / `->` 三种操作
- 相对路径自动补全
- 路径规范化（alias 统一）
- 中文路径与空格路径
- 加载期文件/目录三层推断
- 内存路径类型缓存
- Trie 前缀树匹配

**当前阶段不支持**（保持 KISS）：

- 通配符（`*`）
- 正则表达式
- 别名语法（`[wechat]`）
- 多层继承
- 复杂作用域
- 运行期频繁磁盘探测

### 13.2 Manager App 功能规划

| 功能 | 说明 |
|---|---|
| 规则编辑器 | INI 可视化（按包名分组，展示 mode/allow/deny/redirect） |
| Audit 日志 | 展示"would block"事件，验证规则正确性 |
| mount 状态查看 | 展示当前 App 的 namespace 中的活跃挂载点 |
| 规则测试 | fork 测试进程，执行 open/stat/listdir，返回实际结果 |
| 规则冲突检视 | 当规则存在父子路径冲突时给出警告 |
| 热更新触发 | 一键 SIGUSR1 |
| 导入/导出 | 规则文件备份与恢复 |

---

## 14. 测试与发布清单

### 14.1 规则格式测试

```
[ ] 相对路径自动补全（Pictures/X → /storage/emulated/0/Pictures/X）
[ ] 绝对路径保持原样
[ ] 中文路径（DCIM/隐私目录）正确解析
[ ] 空格路径（DCIM/My Album）正确解析
[ ] 重定向语法（A -> B）按第一个 -> 切分
[ ] mode 未填写时报告配置错误
[ ] 同一包名多条规则正确合并
```

### 14.2 规则引擎测试

```
[ ] 目录规则：匹配目录本身及所有后代，不误伤兄弟路径
[ ] 文件规则：只匹配文件本身，不匹配父目录
[ ] 父子路径冲突：子路径规则优先
[ ] 相同路径文件/目录冲突：文件规则优先
[ ] 动作冲突：deny > redirect > allow
[ ] 白名单模式：未命中规则默认拒绝
[ ] 黑名单模式：未命中规则默认放行
[ ] Trie 匹配结果与线性匹配结果一致性验证
```

### 14.3 功能测试

```
[ ] DENY：App 读不到受保护路径（open/stat/listdir 均返回空/ENOENT）
[ ] ALLOW（whitelist）：允许的路径正常可见
[ ] REDIRECT：访问 src 路径时看到的是 dst 内容
[ ] Audit 模式：所有 mount 操作只记录日志，不实际执行
[ ] IPC 超时：Companion 挂起 50ms 后 App 正常启动（fail-open）
[ ] 热更新：kill -USR1 后新规则对下次启动的 App 生效
[ ] 卸载清理：uninstall.sh 后 .tmp/ 和日志目录被完整删除
[ ] 多路径展开：/sdcard/X 和 /storage/emulated/0/X 均被覆盖
```

### 14.4 Android 版本兼容

```
[ ] Android 8.0 (API 26) — getrandom() 基准版本
[ ] Android 9/10 (API 28/29) — Scoped Storage 引入
[ ] Android 11 (API 30) — sdcardfs → FUSE
[ ] Android 12/13/14 (API 31-34)
```

### 14.5 OEM ROM 兼容

```
[ ] MIUI / HyperOS — SELinux 严格，setfscreatecon 可能受限
[ ] EMUI / MagicOS — 存储路径定制
[ ] ColorOS — /mnt/runtime 结构差异
[ ] OneUI — Knox 额外检测
[ ] AOSP / LineageOS — 基准
```

### 14.6 Magisk 生态兼容

```
[ ] Magisk stable — 主要平台
[ ] KernelSU + ZygiskNext / ReZygisk — 重要替代平台
[ ] APatch + ZygiskNext
[ ] 与 LSPosed 共存
[ ] 与 Shamiko 共存
```

### 14.7 性能测试

```
[ ] 3条规则：App 启动延迟增量 < 5ms
[ ] 1000条规则：App 启动延迟增量 < 15ms
[ ] Trie 查找 P99 延迟 < 0.1ms（应显著优于 O(N) 线性匹配）
[ ] Companion IPC P99 延迟 < 5ms
[ ] Ring buffer 写入不阻塞 preAppSpecialize
```

---

## 15. 开发路线图

| 阶段 | 目标 | 核心任务 | 工期 |
|---|---|---|---|
| **Phase 1（MVP）** | 新规则格式 + 稳定隔离 | INI 解析 · AppPolicy 编译 · Trie 匹配 · Deny/Redirect · WHITELIST 模式 · RAII 回滚 · fail-open · Ring buffer 日志 · Audit 模式 | 10-14 天 |
| **Phase 2（syscall fallback）** | mountinfo 检测对抗 | openat hook（shadowhook）· /proc/self/mountinfo 过滤 · 运行期路径类型缓存更新 | 7-10 天 |
| **Phase 3（Manager App）** | 可视化管理 | Android App · 规则编辑器 · Audit 日志 · mount 状态 · 规则测试工具 | 10-14 天 |
| **Phase 4（生态）** | 发布 + 兼容 | ZygiskNext / ReZygisk 兼容 · 自动更新 JSON · 通配符支持 · 文档 | 5-7 天 |

---

## 16. 参考资源

| 资源 | 说明 | 地址 |
|---|---|---|
| Magisk 官方开发文档 | 模块结构、脚本、sepolicy 规范 | https://topjohnwu.github.io/Magisk/guides.html |
| Zygisk 模块模板 | 官方 C++ 框架、libcxx submodule、`APP_STL=none` 要求 | https://github.com/topjohnwu/zygisk-module-sample |
| AOSP 存储文档 | mount namespace、FUSE、sdcardfs、多用户存储 | https://source.android.com/docs/core/storage |
| AOSP sdcardfs 废弃 | Android 11+ FUSE 实现 | https://source.android.com/docs/core/storage/sdcardfs-deprecate |
| AOSP SELinux 兼容性 | label 冲突与 setfscreatecon 说明 | https://source.android.com/docs/security/features/selinux/compatibility |
| setfscreatecon(3) | SELinux fscreate context API | https://manpages.ubuntu.com/manpages/bionic/en/man3/setfscreatecon.3.html |
| StorageRedirect | bind mount + namespace 设计参考 | https://github.com/RikkaApps/StorageRedirect-assets |
| rvmm-zygisk-mount | preAppSpecialize mount 注入实例 | https://github.com/j-hc/rvmm-zygisk-mount |
| NoHello | preAppSpecialize umount、线程安全实践 | https://github.com/MhmRdd/NoHello |
| FakeXposed | syscall hook（openat）方案，Phase 2 参考 | https://github.com/sanfengAndroid/FakeXposed |
| ZygiskNext / ReZygisk | KernelSU/APatch 兼容层 | https://github.com/Dr-TSNG/ZygiskNext |
| shadowhook | Android inline/PLT hook 库 | https://github.com/bytedance/android-inline-hook |
| rule-format-plan.md | 规则格式专项设计（项目内部文档） | config/rule-format-plan.md |

---

> **法律与伦理声明**：PathGuard 涉及 root 权限下的深度系统干预。发布时请在 README 和 Manager App 中明确告知兼容性限制、稳定性风险、卸载与恢复指南，并禁止用于规避合法安全检测或实施侵权行为。

---

*PathGuard Technical Design v4.0 · 整合 v3 全量修复 + rule-format-plan.md 规则格式设计 + 性能/稳定性审查建议（Trie、fail-open、ring buffer、audit 模式）*
