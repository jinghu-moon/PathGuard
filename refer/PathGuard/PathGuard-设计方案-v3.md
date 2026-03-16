# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v3.0

> **参考来源**：Magisk 官方文档 · Zygisk API（topjohnwu/zygisk-module-sample）· AOSP 存储与 SELinux 文档 · StorageRedirect（RikkaApps）· FakeXposed · rvmm-zygisk-mount · NoHello · ZygiskNext · v2.0 系统级审查报告
>
> **v3 主要变更**：修复 tmpPoint 残留/泄露、SELinux 兼容性降级策略、Companion 线程安全、规则路径去重与冲突处理、路径不存在策略、glob 性能、UID/GID 精化、安全随机数、错误处理 RAII、调试与日志、测试清单

---

## 目录

1. [方案总览与核心决策](#1-方案总览与核心决策)
2. [核心技术原理](#2-核心技术原理)
3. [整体架构](#3-整体架构)
4. [关键问题全量修复（v2 → v3）](#4-关键问题全量修复v2--v3)
5. [模块文件结构](#5-模块文件结构)
6. [核心代码实现](#6-核心代码实现)
7. [配置格式规范](#7-配置格式规范)
8. [构建与安装](#8-构建与安装)
9. [功能设计规划](#9-功能设计规划)
10. [测试与发布清单](#10-测试与发布清单)
11. [开发路线图](#11-开发路线图)
12. [参考资源](#12-参考资源)

---

## 1. 方案总览与核心决策

### 1.1 最终方案

> **Zygisk + Per-App Mount Namespace + tmpfs 覆盖 + bind mount 隐蔽层**

在 `preAppSpecialize` 阶段（App 进程 fork 完成、任何 App 代码尚未执行之前），在 App 私有 Mount Namespace 内：

1. 将整个 namespace 设为 `MS_PRIVATE`，阻断 mount propagation
2. 将目标路径用 tmpfs 覆盖到模块私有目录，再通过 bind mount 叠加到目标路径
3. 自动展开所有等价存储路径（`/sdcard`、`/storage/emulated/{user}`、`/mnt/runtime/*/emulated/{user}` 等）
4. 规则由 Companion Process（root 守护进程）持有并分发，支持热更新

App 看到的是正常的空目录，无报错，无崩溃，无异常 errno，几乎无法检测。

### 1.2 与同类软件对比

| 软件 | 核心技术 | 本方案关系 |
|---|---|---|
| StorageRedirect | bind mount + namespace | 最接近；本方案在此基础上加 bind 隐蔽层 + SELinux 降级兼容 |
| NoHello | preAppSpecialize umount + namespace | 参考其线程安全与热更新机制 |
| FakeXposed | Native syscall hook (openat) | 可作为 Layer 3 fallback |
| XPrivacyLua | Java Xposed Hook | 权限级别，无法精确到路径 |

### 1.3 三层防御体系

```
Layer 1（Phase 1，当前实现）
  tmpfs overlay + bind mount 隐蔽层
  → App 看到空目录，无报错
  → 覆盖所有等价存储路径

Layer 2（Phase 3，SELinux 加固）
  bind mount 保留原始 SELinux label
  → 防止因 label 不匹配引发的隐性 EACCES
  → 对 setfscreatecon 不可用的 ROM 降级到 bind 方案

Layer 3（Phase 2，可选 syscall fallback）
  openat hook 过滤 /proc/self/mountinfo
  → 针对主动读取 mountinfo 做检测的 App
  → 参考 FakeXposed 实现
```

---

## 2. 核心技术原理

### 2.1 Android 存储挂载结构

这是整个方案最关键的基础认知，也是 v1 最大的盲区。`/sdcard` 并非简单路径，而是多层映射体系（参见 [AOSP 存储文档](https://source.android.com/docs/core/storage)）：

```
App 进程 namespace 内的完整路径链：

/sdcard
  └─(symlink)→ /storage/self/primary
                 └─(bind)→ /mnt/user/{userId}/primary
                              └─(symlink)→ /storage/emulated/{userId}
                                              └─(bind)→ /mnt/runtime/{VIEW}/emulated/{userId}
                                                          └─(FUSE/sdcardfs)→ /data/media/{userId}
```

Android 6.0+ 对存储设备维护三个独立视图，根据 App 持有的权限不同注入不同的 VIEW：

| VIEW | 适用场景 |
|---|---|
| `default` | 无存储权限的 App；root/全局 namespace |
| `read` | 持有 `READ_EXTERNAL_STORAGE` |
| `write` | 持有 `WRITE_EXTERNAL_STORAGE` |

**Android 11+ FUSE**：sdcardfs 被废弃，改为新 FUSE 实现，但路径映射结构基本一致（参见 [sdcardfs 废弃说明](https://source.android.com/docs/core/storage/sdcardfs-deprecate)）。

**结论**：若仅 mount `/sdcard/secret`，只覆盖了一条入口，App 仍可通过 `/storage/emulated/0/secret` 或 `/mnt/runtime/write/emulated/0/secret` 访问真实内容。必须同时覆盖所有等价路径。

### 2.2 Per-App Mount Namespace 机制

每个 App 进程 fork 自 Zygote 时，从 Zygote 的 mount namespace 复制一份私有副本。在这个副本内的 mount/umount 操作对其他进程完全不可见。

```
Zygote（全局 namespace，VIEW=default）
    │
    ├─ fork → App A 私有 namespace（已从 Zygote 复制）
    │           vold 可在此 namespace 内 bind mount 升级 VIEW
    │           PathGuard 在 preAppSpecialize 中操作此 namespace
    │
    └─ fork → App B 私有 namespace（彼此完全隔离）
```

### 2.3 Zygisk 生命周期与正确切入点

根据 [Zygisk 官方 API（zygisk.hpp）](https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp)，`preAppSpecialize` 是唯一正确的 mount 操作时机：

```
Zygote fork App 进程
    │
    ├─ [Zygisk] preAppSpecialize()    ← ✅ 在此操作
    │    • mount namespace 已独立（从 Zygote 复制）
    │    • 进程尚未进入沙盒（UID drop 尚未执行）
    │    • mount() 调用可成功
    │    • connectCompanion() 可用，可获取 root 进程连接
    │
    ├─ App specialize（UID drop、沙盒、SELinux 域切换）
    ├─ [Zygisk] postAppSpecialize()   ← ❌ 沙盒已启用，mount 会失败
    └─ App 代码开始执行               ← 此时已看到覆盖后的空目录
```

> **注意**：根据 [NoHello 实践](https://github.com/MhmRdd/NoHello/releases)，`preAppSpecialize` 是比 hook `unshare` 更稳定的方案；mount 操作失败时需要完整回滚，不可留残留。

### 2.4 tmpfs + bind mount 双层隐蔽原理

单层 tmpfs 直接挂载到目标路径时，`/proc/self/mountinfo` 会显示 `tmpfs on /sdcard/secret`，特征明显，可被检测。

v3 方案改为通过模块私有目录中转：

```
Step 1: mkdir  /data/adb/modules/path_guard/.tmp/pg_{random}
Step 2: mount  tmpfs   → /data/adb/modules/path_guard/.tmp/pg_{random}
Step 3: mount  --bind  → /storage/emulated/0/secret

mountinfo 中呈现：
  /data/adb/modules/path_guard/.tmp/pg_{random} on /storage/emulated/0/secret type tmpfs (...)

这比直接 tmpfs 更接近系统正常 bind mount 行为，不易触发异常检测。
```

**为什么不用 `/dev`（v2 的问题）**：
- `/dev` 属于全局 namespace，创建的目录对系统可见，可能被扫描工具发现
- 模块私有目录 `/data/adb/modules/path_guard/.tmp/` 只有 root 可访问，避免暴露

---

## 3. 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                           PathGuard v3                              │
│                                                                     │
│  ┌────────────────────────┐    ┌───────────────────────────────┐   │
│  │    Zygisk Module       │    │      Companion Daemon         │   │
│  │   (libpathguard.so)    │◄──►│   (root 守护进程，常驻)        │   │
│  │                        │    │                               │   │
│  │ preAppSpecialize:      │    │ • g_rules (unordered_map)     │   │
│  │  1. connectCompanion   │    │ • g_glob_rules (fnmatch)      │   │
│  │  2. 查询规则（O(1)）    │    │ • shared_mutex（读写分离）     │   │
│  │  3. MS_REC|MS_PRIVATE  │    │ • SIGUSR1 → atomic 热更新    │   │
│  │  4. resolveAllPaths    │    │ • 日志限速 + 轮转              │   │
│  │  5. sortByDepthDesc    │    │ • tmpPoint 生命周期管理        │   │
│  │  6. applyOverlay(RAII) │    │ • 文件权限读取                │   │
│  └────────────────────────┘    └───────────────────────────────┘   │
│                                              ▲                      │
│                                              │ Unix Socket IPC      │
│  ┌────────────────────────┐    ┌─────────────┴─────────────────┐   │
│  │     Rule Engine        │    │        Manager App            │   │
│  │                        │    │  • 规则编辑器                  │   │
│  │ • unordered_map O(1)   │    │  • mount 状态可视化            │   │
│  │ • fnmatch glob（倒排）  │    │  • 规则测试（spawn test proc） │   │
│  │ • realpath 规范化      │    │  • 调试日志查看               │   │
│  │ • 多路径展开 + 去重     │    │  • 热更新触发                 │   │
│  │ • 深路径优先排序        │    │  • 规则冲突检视               │   │
│  │ • 路径缺失策略          │    │  • 导入/导出规则              │   │
│  └────────────────────────┘    └───────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. 关键问题全量修复（v2 → v3）

### 4.1 🔴 [必修] tmpPoint 残留与泄露风险

**v2 问题**：tmpPoint 创建在 `/dev/pathguard_{random}`，在全局 namespace 可见；mount 失败时未完整回滚，导致空目录残留。

**v3 修复**：

1. **改用模块私有目录**：`/data/adb/modules/path_guard/.tmp/pg_{random}`，仅 root 可访问
2. **RAII 自动回滚**：使用 `MountGuard` RAII 类管理挂载生命周期，任何异常路径均自动清理
3. **Companion 统一管理临时目录生命周期**：模块启动时清理残留，卸载时全量清理

```cpp
// RAII 挂载管理器，防止任何异常路径下的资源残留
class MountGuard {
    std::string tmpPoint;
    bool committed = false;
public:
    explicit MountGuard(std::string p) : tmpPoint(std::move(p)) {}
    ~MountGuard() {
        if (!committed) {
            // 自动回滚：卸载 + 删除临时目录
            umount2(tmpPoint.c_str(), MNT_DETACH);
            rmdir(tmpPoint.c_str());
        }
    }
    void commit() { committed = true; }
};

void applyOverlay(const std::string &target, uid_t uid, gid_t gid, mode_t mode) {
    // 使用安全随机数（getrandom()）生成不可预测的目录名
    std::string tmpPoint = TMP_ROOT + "/pg_" + secureRandomHex(16);
    mkdir(tmpPoint.c_str(), 0700);

    MountGuard guard(tmpPoint);  // RAII：失败时自动回滚

    std::string opts = buildTmpfsOpts(mode, uid, gid);
    if (mount("tmpfs", tmpPoint.c_str(), "tmpfs", TMPFS_FLAGS, opts.c_str()) != 0) {
        LOGE("tmpfs failed: %s (%d: %s)", tmpPoint.c_str(), errno, strerror(errno));
        return;  // guard 析构时自动 rmdir
    }

    if (mount(tmpPoint.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        LOGE("bind failed: %s->%s (%d: %s)", tmpPoint.c_str(), target.c_str(), errno, strerror(errno));
        return;  // guard 析构时自动 umount + rmdir
    }

    guard.commit();  // 成功，临时目录保留（namespace 消亡时自动回收）
    // 注：进程 namespace 消亡后，bind 挂载自动释放；但 tmpPoint 目录本身由 Companion 定期清理
}
```

**临时目录清理策略**：
- App 进程存活期间：tmpPoint 保留（是 bind 的 source）
- App 进程退出时：namespace 消亡，挂载自动释放；但 tmpPoint 目录本身仍存在
- Companion 定期（每30分钟）扫描 `.tmp/` 目录，删除没有对应挂载的空目录
- 模块卸载时（`uninstall.sh`）：`rm -rf /data/adb/modules/path_guard/.tmp/`

---

### 4.2 🔴 [必修] SELinux Context 兼容性（降级策略）

**v2 问题**：`setfscreatecon()` 在部分 ROM（MIUI/EMUI/ColorOS）或内核配置下不可用；对 `/Android/data` 等特殊路径使用不当可能引发 EACCES。

**v3 修复**：分三级降级策略：

```
策略 1（最优）：setfscreatecon + getfilecon 复制原始 label
    ↓ 若 getfilecon 失败或 setfscreatecon 返回 -1
策略 2（降级）：bind mount 从"空目录模板"绑定
    → bind 操作会保留源目录的 SELinux label，天然继承，无需手动设置
    ↓ 若 bind 也因 SELinux 拒绝失败
策略 3（最后）：记录日志，跳过该路径，不强制阻塞启动
```

```cpp
bool applyOverlayWithSELinux(const std::string &target, uid_t uid, gid_t gid, mode_t mode) {
    // 尝试读取原始 SELinux context
    char *ctx = nullptr;
    bool hasSELinux = (getfilecon(target.c_str(), &ctx) >= 0 && ctx != nullptr);

    if (hasSELinux) {
        // 策略 1：挂载前设置 fscreate context
        if (setfscreatecon(ctx) == 0) {
            bool ok = doMount(target, uid, gid, mode);
            setfscreatecon(nullptr);  // 必须在 mount 后立即重置
            freecon(ctx);
            if (ok) return true;
            // 若 mount 失败，继续尝试策略 2
        } else {
            freecon(ctx);
        }
    }

    // 策略 2：bind mount 保留原始 label（bind 天然继承 source 目录的 label）
    // 创建一个与原目录权限一致的空模板目录（由 Companion 预创建）
    std::string tmpl = TEMPLATE_ROOT + "/mode_" + modeOctal(mode);
    ensureTemplate(tmpl, uid, gid, mode);  // Companion 端预创建
    if (mount(tmpl.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) == 0) {
        LOGI("SELinux fallback: bind from template for %s", target.c_str());
        return true;
    }

    // 策略 3：记录，跳过
    LOGE("All SELinux strategies failed for %s, skipping", target.c_str());
    return false;
}
```

> **参考**：根据 [setfscreatecon(3) man page](https://manpages.ubuntu.com/manpages/bionic/en/man3/setfscreatecon.3.html)，`setfscreatecon` 的 context 是线程本地的，信号处理器中使用时必须保存/恢复，且在 `execve` 后自动重置。

---

### 4.3 🔴 [必修] Mount Propagation 隔离（实现保障）

**v2 问题**：文档有此步骤，但实现中缺少返回值检查和日志，失败时静默继续可能导致泄露。

**v3 修复**：

```cpp
bool isolateMountNamespace() {
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        LOGE("CRITICAL: namespace isolation failed (errno=%d: %s). "
             "Aborting path block to prevent mount leakage.", errno, strerror(errno));
        return false;  // 隔离失败则放弃整个 block 操作，不留隐患
    }
    return true;
}

void preAppSpecialize(AppSpecializeArgs *args) override {
    // ...获取规则...
    if (paths.empty()) return;

    // 隔离失败则直接放弃，不尝试 mount（安全优先）
    if (!isolateMountNamespace()) return;

    // 继续 mount 操作...
}
```

---

### 4.4 🟠 [强烈建议] Companion 线程安全

**v2 问题**：`g_rules` 在 SIGUSR1 reload 时与并发查询存在 race condition。

**v3 修复**：使用 C++17 `std::shared_mutex`，配合 `atomic<bool>` 防止重复 reload：

```cpp
#include <shared_mutex>
#include <atomic>

std::shared_mutex g_rules_mutex;
std::unordered_map<std::string, std::vector<std::string>> g_rules;
std::vector<std::pair<std::string, std::vector<std::string>>> g_glob_rules;
std::atomic<bool> g_reloading{false};

void companionHandler(int fd) {
    std::string pkg = receiveString(fd);

    // 读锁：允许多个查询并发，reload 时阻塞
    std::shared_lock lk(g_rules_mutex);

    std::vector<std::string> result;
    auto it = g_rules.find(pkg);
    if (it != g_rules.end()) result = it->second;

    for (auto &[pattern, paths] : g_glob_rules) {
        if (fnmatch(pattern.c_str(), pkg.c_str(), 0) == 0) {
            result.insert(result.end(), paths.begin(), paths.end());
        }
    }

    sendStringList(fd, result);
}

void reloadRules() {
    bool expected = false;
    if (!g_reloading.compare_exchange_strong(expected, true)) {
        return;  // 已在 reload 中，跳过重复触发
    }

    auto newRules = parseRulesFile(RULES_PATH);
    auto newGlobRules = parseGlobRulesFile(RULES_PATH);

    {
        std::unique_lock lk(g_rules_mutex);  // 写锁：独占
        g_rules      = std::move(newRules);
        g_glob_rules = std::move(newGlobRules);
    }

    g_reloading.store(false);
    LOGI("Rules reloaded: %zu exact, %zu glob", g_rules.size(), g_glob_rules.size());
}

void onSIGUSR1(int) { reloadRules(); }  // 信号触发热更新
```

---

### 4.5 🟠 [强烈建议] 规则去重、路径冲突与展开顺序

**v2 问题**：展开后的等价路径可能重复，对同一路径重复 mount 会触发 `EBUSY`；父路径与子路径同时存在时仍有细节隐患。

**v3 修复**：展开 → 规范化 → 去重 → 排序的完整流水线：

```cpp
std::vector<std::string> buildFinalPathList(
    const std::vector<std::string> &rawPaths, int userId) {

    std::unordered_set<std::string> seen;
    std::vector<std::string> result;

    for (auto &raw : rawPaths) {
        // 1. 展开所有等价存储路径
        auto expanded = resolveStoragePaths(raw, userId);
        for (auto &p : expanded) {
            // 2. realpath 规范化（处理 ../. 和符号链接）
            std::string norm = normalizePath(p);
            // 3. 去重（避免对同一实际目录重复 mount）
            if (seen.insert(norm).second) {
                result.push_back(norm);
            }
        }
    }

    // 4. 深路径优先排序（子目录先 mount，避免被父目录覆盖）
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return std::count(a.begin(), a.end(), '/') >
               std::count(b.begin(), b.end(), '/');
    });

    return result;
}
```

---

### 4.6 🟠 [强烈建议] 路径不存在时的处理策略

**v2 问题**：stat 失败时静默跳过，没有提供"预创建占位"选项，文档也未说明。

**v3 修复**：提供两种行为，通过配置控制：

```ini
# rules.conf 全局选项
[global]
missing_path = skip          # 默认：路径不存在则跳过（安全）
# missing_path = create      # 可选：预创建空目录并挂载（危险，见注意事项）
```

```cpp
enum class MissingPathPolicy { SKIP, CREATE };

void handlePath(const std::string &path, MissingPathPolicy policy, ...) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (policy == MissingPathPolicy::SKIP) {
            LOGD("Path not found, skipping: %s", path.c_str());
            return;
        }
        // CREATE 模式：预创建目录并挂载（警告用户此行为可被检测）
        LOGW("Path not found, creating placeholder: %s (DETECTABLE)", path.c_str());
        if (mkdirs(path.c_str(), 0755) != 0) return;
        stat(path.c_str(), &st);  // 重新 stat
    }
    applyOverlay(path, st.st_uid, st.st_gid, st.st_mode & 07777);
}
```

> ⚠️ `create` 模式会在文件系统中留下目录，可能被 App 通过 `getattr` 或目录枚举检测到，仅建议在调试模式中使用。

---

### 4.7 🟠 [强烈建议] glob 规则性能与倒排索引

**v2 问题**：glob 规则仍需遍历，大量 glob 规则（>50条）时有性能问题。

**v3 修复**：对常见前缀做静态倒排索引，限制 glob 规则总量，Companion App 侧预警：

```cpp
// 倒排索引：包名前缀 → 相关 glob 规则集合
// 如 "com.tencent.*" → 倒排到 "com.tencent" 分桶
std::unordered_map<std::string, std::vector<size_t>> g_glob_prefix_index;

std::vector<std::string> matchGlob(const std::string &pkg) {
    std::vector<std::string> result;
    std::string prefix = extractDomainPrefix(pkg);  // "com.tencent.mm" → "com.tencent"

    // 只在相关分桶中匹配，减少 fnmatch 调用次数
    auto it = g_glob_prefix_index.find(prefix);
    if (it != g_glob_prefix_index.end()) {
        for (size_t idx : it->second) {
            if (fnmatch(g_glob_rules[idx].first.c_str(), pkg.c_str(), 0) == 0) {
                auto &paths = g_glob_rules[idx].second;
                result.insert(result.end(), paths.begin(), paths.end());
            }
        }
    }
    return result;
}
```

---

### 4.8 🟠 [强烈建议] 安全随机数

**v2 问题**：文档未明确随机数来源，使用 `rand()` 存在可预测性风险。

**v3 修复**：使用 `getrandom()` 系统调用（Linux 3.17+，Android 6.0+ 均支持）：

```cpp
#include <sys/random.h>

std::string secureRandomHex(size_t bytes) {
    std::vector<uint8_t> buf(bytes);
    ssize_t ret = getrandom(buf.data(), bytes, GRND_NONBLOCK);
    if (ret < 0 || (size_t)ret < bytes) {
        // 降级到 /dev/urandom
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        read(fd, buf.data(), bytes);
        close(fd);
    }
    std::string hex;
    for (auto b : buf) {
        char tmp[3];
        snprintf(tmp, sizeof(tmp), "%02x", b);
        hex += tmp;
    }
    return hex;
}
```

---

### 4.9 🟠 [强烈建议] 完整错误处理与分类

**v2 问题**：mount 错误未区分类型，统一打日志后继续。

**v3 修复**：对不同 errno 分类处理：

```cpp
void handleMountError(const std::string &target, int err) {
    switch (err) {
        case EPERM:
            LOGE("[PathGuard] EPERM: mount namespace not properly isolated? target=%s",
                 target.c_str());
            break;
        case EACCES:
            LOGE("[PathGuard] EACCES: SELinux denied mount. target=%s "
                 "(try SELinux fallback)", target.c_str());
            break;
        case EBUSY:
            LOGW("[PathGuard] EBUSY: already mounted (duplicate path?). target=%s",
                 target.c_str());
            break;
        case ENOENT:
            LOGD("[PathGuard] ENOENT: path not found (skip). target=%s",
                 target.c_str());
            break;
        default:
            LOGE("[PathGuard] mount failed: target=%s errno=%d (%s)",
                 target.c_str(), err, strerror(err));
    }
    // 写入 Companion 日志文件（由 Companion 端异步完成，不阻塞 preAppSpecialize）
}
```

---

### 4.10 🟠 [强烈建议] Companion IPC fd 安全

**v2 问题**：未明确对 fd 设置 `O_CLOEXEC`，可能泄露到子进程。

**v3 修复**：

```cpp
// 所有 IPC 相关 fd 创建时加 O_CLOEXEC
int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

// Companion 收到连接后同样设置
int clientFd = accept4(serverFd, nullptr, nullptr, SOCK_CLOEXEC);
```

---

### 4.11 🟢 [可选增强] 日志限速与隐私保护

```cpp
// 限速：同一路径的错误日志每秒最多记录一次
class RateLimitedLogger {
    std::unordered_map<std::string, time_t> lastLog;
    std::mutex mu;
public:
    void log(const std::string &key, std::function<void()> logFn) {
        std::lock_guard<std::mutex> lk(mu);
        auto now = time(nullptr);
        if (now - lastLog[key] >= 1) {
            logFn();
            lastLog[key] = now;
        }
    }
};

// 日志文件隐私控制
// /data/adb/pathguard/debug.log 权限：0600（仅 root 可读）
// 日志轮转：超过 1MB 时 rotate（保留最近 3 个）
// 默认关闭详细日志，只记录错误；Manager App 中开启调试模式
```

---

### 4.12 总结：v2 → v3 变更对照表

| 问题编号 | 类型 | v2 状态 | v3 修复 |
|---|---|---|---|
| tmpPoint 位置与残留 | 🔴 必修 | `/dev/pathguard_xxx`，可见，可残留 | 模块私有目录 + RAII 回滚 + Companion 定期清理 |
| SELinux 兼容性 | 🔴 必修 | 仅 setfscreatecon，无降级 | 三级降级（setfscreatecon → bind template → skip） |
| MS_PRIVATE 返回值 | 🔴 必修 | 未检查返回值 | 失败则放弃全部 mount |
| Companion 线程安全 | 🟠 强烈建议 | 无锁，有 race | shared_mutex + atomic reload |
| 路径去重 | 🟠 强烈建议 | 无去重，可能 EBUSY | 展开→规范化→去重→排序完整流水线 |
| 路径不存在策略 | 🟠 强烈建议 | 静默跳过，无选项 | skip/create 双模式，配置控制 |
| glob 性能 | 🟠 强烈建议 | 全量遍历 | 前缀倒排索引减少 fnmatch |
| 安全随机数 | 🟠 强烈建议 | 未明确 | getrandom() + /dev/urandom 降级 |
| mount 错误分类 | 🟠 强烈建议 | 统一日志 | errno 分类处理 |
| IPC fd CLOEXEC | 🟠 强烈建议 | 未设置 | SOCK_CLOEXEC + accept4 |
| 日志限速与隐私 | 🟢 可选 | 无 | 限速 + 轮转 + 权限控制 |

---

## 5. 模块文件结构

根据 [Magisk 官方开发文档](https://topjohnwu.github.io/Magisk/guides.html)：

```
path-guard/
├── module.prop              # 模块元信息（严格格式，UNIX LF 换行）
├── skip_mount               # 无需 system 文件替换，跳过 magic mount
│
├── zygisk/                  # Zygisk native 库（官方规定目录名）
│   ├── arm64-v8a.so         # 主流 64 位 ARM
│   ├── armeabi-v7a.so       # 32 位 ARM 兼容
│   ├── x86_64.so            # x86_64 模拟器
│   └── x86.so               # x86 32 位模拟器
│
├── config/                  # 用户配置
│   └── rules.conf           # 规则文件（格式见第 7 节）
│
├── action.sh                # Magisk App「执行」按钮：检查状态 / 热更新
└── uninstall.sh             # 卸载清理：删除 .tmp/ 和日志目录
```

### module.prop

```properties
id=path_guard
name=PathGuard - 路径访问控制
version=v3.0.0
versionCode=3
author=YourName
description=透明隔离 App 对敏感路径的访问。Zygisk + per-app mount namespace + tmpfs/bind overlay。App 看到空目录，无报错，无崩溃，三层防御。
updateJson=https://your-server.com/path-guard/update.json
```

> `id` 必须匹配 `^[a-zA-Z][a-zA-Z0-9._-]+$`，一旦发布不得修改。

---

## 6. 核心代码实现

### 6.1 项目源码结构

```
module/jni/
├── zygisk.hpp          # Zygisk API 头文件（来自官方模板）
├── module.cpp          # 主模块：onLoad + preAppSpecialize
├── companion.cpp       # Companion 守护进程：规则管理 + IPC
├── mount.cpp           # Mount 操作封装（含 MountGuard RAII）
├── rules.cpp           # 规则解析（精确 + glob + 倒排）
├── path_utils.cpp      # 路径工具（resolveStoragePaths / normalizePath / ...）
├── selinux_utils.cpp   # SELinux 三级降级策略
├── log.cpp             # 限速日志 + 日志文件轮转
├── Android.mk
└── Application.mk
```

### 6.2 module.cpp（主模块核心流程）

```cpp
#include "zygisk.hpp"
#include "mount.h"
#include "path_utils.h"
#include "log.h"
#include <android/log.h>

using namespace zygisk;

class PathGuard : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args->nice_name) return;

        // 1. 获取包名
        const char *pkgC = env->GetStringUTFChars(args->nice_name, nullptr);
        std::string pkg(pkgC);
        env->ReleaseStringUTFChars(args->nice_name, pkgC);

        // 2. 从 Companion（root 进程）获取规则，O_CLOEXEC 保护 fd
        int fd = api->connectCompanion();
        if (fd < 0) return;
        sendString(fd, pkg);
        auto rawPaths = receiveStringList(fd);
        close(fd);
        if (rawPaths.empty()) return;

        // 3. 阻断 mount propagation（失败则放弃，不留隐患）
        if (!isolateMountNamespace()) return;

        // 4. 展开所有等价存储路径 → 规范化 → 去重 → 深路径优先
        int userId = static_cast<int>(args->uid / 100000);
        auto paths = buildFinalPathList(rawPaths, userId);

        // 5. 对每个路径应用覆盖（RAII 保护，失败自动回滚）
        for (auto &path : paths) {
            applyOverlayFull(path, getMissingPolicy());
        }

        LOGI("PathGuard applied %zu mounts for %s (uid=%d)",
             paths.size(), pkg.c_str(), args->uid);
    }

private:
    Api *api;
    JNIEnv *env;
};

REGISTER_ZYGISK_MODULE(PathGuard)
REGISTER_ZYGISK_COMPANION(companionHandler)
```

### 6.3 companion.cpp（Companion 守护进程）

```cpp
#include "rules.h"
#include "log.h"
#include <shared_mutex>
#include <atomic>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

static std::shared_mutex g_mutex;
static std::unordered_map<std::string, std::vector<std::string>> g_rules;
static std::vector<std::pair<std::string, std::vector<std::string>>> g_glob_rules;
static std::unordered_map<std::string, std::vector<size_t>> g_glob_index;  // 倒排索引
static std::atomic<bool> g_reloading{false};

void reloadRules() {
    bool expected = false;
    if (!g_reloading.compare_exchange_strong(expected, true)) return;

    auto [newExact, newGlob] = parseRulesFile(RULES_PATH);
    auto newIndex = buildGlobIndex(newGlob);

    {
        std::unique_lock lk(g_mutex);
        g_rules      = std::move(newExact);
        g_glob_rules = std::move(newGlob);
        g_glob_index = std::move(newIndex);
    }

    g_reloading.store(false);
    LOGI("Rules reloaded: %zu exact, %zu glob patterns", g_rules.size(), g_glob_rules.size());
}

void companionHandler(int fd) {
    std::string pkg = receiveString(fd);
    std::vector<std::string> result;

    {
        std::shared_lock lk(g_mutex);  // 读锁：并发查询安全

        // O(1) 精确匹配
        auto it = g_rules.find(pkg);
        if (it != g_rules.end()) result = it->second;

        // 前缀倒排索引加速 glob 匹配
        auto prefix = extractDomainPrefix(pkg);
        auto idxIt = g_glob_index.find(prefix);
        if (idxIt != g_glob_index.end()) {
            for (size_t i : idxIt->second) {
                if (fnmatch(g_glob_rules[i].first.c_str(), pkg.c_str(), 0) == 0) {
                    auto &paths = g_glob_rules[i].second;
                    result.insert(result.end(), paths.begin(), paths.end());
                }
            }
        }
    }

    sendStringList(fd, result);
    appendLogAsync(pkg, result);  // 异步写日志，不阻塞 IPC
}

// 定期清理 .tmp/ 下的残留空目录
void cleanupTmpDirs() {
    DIR *d = opendir(TMP_ROOT.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string path = TMP_ROOT + "/" + ent->d_name;
        // 检查是否还有挂载点引用（读 /proc/mounts 判断）
        if (!isMountSource(path)) {
            rmdir(path.c_str());
        }
    }
    closedir(d);
}

__attribute__((constructor)) void companionInit() {
    // 启动时确保临时目录存在且权限正确
    mkdir(TMP_ROOT.c_str(), 0700);
    chmod(TMP_ROOT.c_str(), 0700);
    // 清理上次遗留的残留
    cleanupTmpDirs();
    // 加载规则
    reloadRules();
    // 注册热更新信号
    signal(SIGUSR1, [](int) { reloadRules(); });
    // 启动定期清理定时器（每 30 分钟）
    // 可通过 alarm() 或 timerfd 实现
}
```

### 6.4 mount.cpp（Mount 操作与 RAII）

```cpp
#include "mount.h"
#include "selinux_utils.h"
#include "log.h"
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <selinux/selinux.h>

constexpr unsigned long TMPFS_FLAGS =
    MS_NOSUID   |  // 禁止 setuid/setgid
    MS_NODEV    |  // 禁止设备文件
    MS_NOEXEC   |  // 禁止在 tmpfs 内执行代码
    MS_RELATIME;   // 合理 atime 策略（与系统一致）

// RAII 挂载守卫：析构时自动回滚
class MountGuard {
    std::string p;
    bool ok = false;
public:
    explicit MountGuard(std::string path) : p(std::move(path)) {}
    ~MountGuard() {
        if (!ok) {
            umount2(p.c_str(), MNT_DETACH);
            rmdir(p.c_str());
        }
    }
    void commit() { ok = true; }
};

bool isolateMountNamespace() {
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        LOGE("namespace isolation failed: %d (%s)", errno, strerror(errno));
        return false;
    }
    return true;
}

void applyOverlayFull(const std::string &target, MissingPathPolicy policy) {
    struct stat lst;
    if (lstat(target.c_str(), &lst) != 0) {
        if (policy == MissingPathPolicy::SKIP) return;
        // CREATE 模式：预创建（见 4.6）
        mkdirs(target.c_str(), 0755);
        if (lstat(target.c_str(), &lst) != 0) return;
    }

    // 符号链接：解引用后对真实路径操作
    std::string realTarget = target;
    if (S_ISLNK(lst.st_mode)) {
        char resolved[PATH_MAX];
        if (!realpath(target.c_str(), resolved)) return;
        realTarget = resolved;
        if (stat(realTarget.c_str(), &lst) != 0) return;
    }

    if (!S_ISDIR(lst.st_mode)) return;  // 只处理目录

    // 使用安全随机数生成不可预测的临时目录名
    std::string tmpPoint = TMP_ROOT + "/pg_" + secureRandomHex(16);
    if (mkdir(tmpPoint.c_str(), 0700) != 0) return;

    MountGuard guard(tmpPoint);  // 异常安全

    // 构建 tmpfs 挂载选项（复制原始权限）
    std::string opts = "mode=" + modeOctal(lst.st_mode & 07777) +
                       ",uid=" + std::to_string(lst.st_uid) +
                       ",gid=" + std::to_string(lst.st_gid);

    if (mount("tmpfs", tmpPoint.c_str(), "tmpfs", TMPFS_FLAGS, opts.c_str()) != 0) {
        handleMountError(tmpPoint, errno);
        return;  // guard 析构自动回滚
    }

    // SELinux 三级降级策略（见 4.2）
    if (!applyWithSELinux(tmpPoint, realTarget, lst)) {
        handleMountError(realTarget, errno);
        return;  // guard 析构自动回滚
    }

    guard.commit();  // 成功提交，tmpPoint 由 Companion 定期清理
}
```

### 6.5 Android.mk / Application.mk

```makefile
# module/jni/Android.mk
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE        := path_guard
LOCAL_SRC_FILES     := module.cpp companion.cpp mount.cpp rules.cpp \
                       path_utils.cpp selinux_utils.cpp log.cpp
LOCAL_CFLAGS        := -std=c++17 -fvisibility=hidden \
                       -ffunction-sections -fdata-sections \
                       -Wall -Wextra -Werror
LOCAL_LDFLAGS       := -Wl,--gc-sections
LOCAL_LDLIBS        := -llog -lselinux

include $(BUILD_SHARED_LIBRARY)
```

```makefile
# module/jni/Application.mk
APP_ABI      := arm64-v8a armeabi-v7a x86_64 x86
APP_PLATFORM := android-26    # Android 8.0（getrandom() 可用）
APP_STL      := none          # 官方要求：不得修改，不使用 NDK STL
APP_CFLAGS   := -std=c++17
```

---

## 7. 配置格式规范

### 7.1 rules.conf 完整格式

```ini
# PathGuard 规则文件 v3
# 编码：UTF-8，LF 换行
# 路径：绝对路径，填写任一等价路径均可（模块自动展开所有等价路径）

# ── 全局选项 ──
[global]
missing_path = skip          # skip（默认）或 create（危险，可被检测）
log_level    = error         # error / info / debug

# ── 精确包名规则 ──
# 格式：<package>|<path>
com.tencent.mm|/sdcard/私密相册
com.tencent.mm|/sdcard/Documents/Personal

# ── 同一包名多路径（换行分隔） ──
com.example.badapp|/sdcard/Backup
com.example.badapp|/sdcard/Download/work

# ── glob 通配符（fnmatch 语法） ──
com.adnetwork.*|/sdcard/DCIM

# ── 白名单模式（Phase 2，暂不实现） ──
# [whitelist]
# com.myapp|/sdcard/Photos     # 仅此路径可见
# com.myapp|*                  # 其余全部屏蔽
```

### 7.2 多用户路径自动展开

用户只需填写逻辑路径，模块根据当前 App 的 userId 自动展开：

```
输入：/sdcard/secret
自动展开（userId=0）：
  /sdcard/secret
  /storage/emulated/0/secret
  /storage/self/primary/secret
  /mnt/runtime/default/emulated/0/secret
  /mnt/runtime/read/emulated/0/secret
  /mnt/runtime/write/emulated/0/secret
  （规范化 → 去重 → 深路径优先排序）

多用户（userId=10）自动映射为：
  /storage/emulated/10/secret
  /mnt/runtime/*/emulated/10/secret
  ...
```

---

## 8. 构建与安装

```bash
# 1. 克隆官方 Zygisk 模块模板
git clone https://github.com/topjohnwu/zygisk-module-sample path-guard
cd path-guard

# 2. 将 module/jni/ 中的源文件替换为本项目代码

# 3. 确认 NDK r25+（getrandom() + C++17 shared_mutex 均支持）
export ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/25.2.9519653

# 4. 编译
cd module
$ANDROID_NDK_ROOT/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk

# 5. 复制产物到 zygisk/ 目录
cp libs/arm64-v8a/libpath_guard.so   ../zygisk/arm64-v8a.so
cp libs/armeabi-v7a/libpath_guard.so ../zygisk/armeabi-v7a.so
cp libs/x86_64/libpath_guard.so      ../zygisk/x86_64.so
cp libs/x86/libpath_guard.so         ../zygisk/x86.so

# 6. 打包为 Magisk 模块 zip
cd ..
zip -r path-guard-v3.0.zip module.prop zygisk/ config/ action.sh uninstall.sh skip_mount

# 7. 安装（通过 Magisk Manager 本地安装）
adb push path-guard-v3.0.zip /sdcard/
# Magisk → 模块 → 从本地安装 → 重启

# 8. 规则热更新（无需重启；新规则对已运行的 App 无效，App 需重启）
adb shell "kill -USR1 $(pidof magiskd)"

# 9. 调试日志
adb logcat -s PathGuard          # Logcat 实时日志
adb shell cat /data/adb/pathguard/debug.log  # 文件日志（仅 root 可读）
```

### uninstall.sh

```bash
#!/system/bin/sh
# 卸载时清理所有模块产生的文件
rm -rf /data/adb/modules/path_guard/.tmp
rm -rf /data/adb/pathguard
```

---

## 10. 测试与发布清单

### 10.1 功能测试

```
[ ] 基础功能：目标 App 读不到受保护路径（open/stat/listdir 均返回空/ENOENT）
[ ] 非目标 App：同一路径正常可见（namespace 隔离正确）
[ ] 系统进程：不受影响（preAppSpecialize 不处理系统进程）
[ ] 多路径展开：/sdcard/X 与 /storage/emulated/0/X 均被覆盖
[ ] 符号链接：/sdcard 通过链接链访问的路径也被覆盖
[ ] 父子路径冲突：/sdcard 和 /sdcard/foo 同时配置时，/sdcard/foo 优先
[ ] 路径不存在：skip 模式静默跳过，不影响 App 启动
[ ] 热更新：kill -USR1 后新规则对下次启动的 App 生效
[ ] 卸载清理：uninstall.sh 后 .tmp/ 和日志目录均被删除
```

### 10.2 Android 版本兼容性

```
[ ] Android 8.0  (API 26) — getrandom() 可用，基准版本
[ ] Android 9.0  (API 28)
[ ] Android 10.0 (API 29) — Scoped Storage 初步引入
[ ] Android 11.0 (API 30) — sdcardfs 废弃，切换 FUSE
[ ] Android 12.0 (API 31)
[ ] Android 13.0 (API 33)
[ ] Android 14.0 (API 34)
```

### 10.3 OEM ROM 兼容性

```
[ ] MIUI / HyperOS      — SELinux 定制严格，setfscreatecon 可能受限
[ ] EMUI / MagicOS      — 存储路径有定制，需验证展开逻辑
[ ] ColorOS             — 部分版本 /mnt/runtime 路径结构不同
[ ] OneUI               — Samsung Knox 可能增加额外检测
[ ] OriginOS / FuntouchOS
[ ] AOSP / LineageOS    — 基准参考
```

### 10.4 Magisk 生态兼容性

```
[ ] Magisk (stable)     — 主要平台
[ ] KernelSU + ZygiskNext — 重要替代平台
[ ] APatch + ZygiskNext
[ ] 与 LSPosed 共存测试
[ ] 与 Shamiko / Hide My Applist 共存测试
```

### 10.5 反检测测试

```
[ ] 检查 /proc/self/mountinfo 是否暴露 tmpfs 特征（tmpfs on /sdcard/X）
[ ] 检查 /proc/mounts 是否有异常条目
[ ] 目标路径的 stat 结果是否合理（非异常 inode、size 等）
[ ] 50+ 常见 App：微信、支付宝、银行类 App、游戏反作弊
[ ] SafetyNet / Play Integrity 兼容（与 Shamiko 配合）
```

### 10.6 性能测试

```
[ ] 3条规则场景下 App 启动延迟增量 < 5ms（目标）
[ ] 1000条规则压力测试下 App 启动延迟增量 < 15ms
[ ] Companion 规则查询 P99 延迟 < 1ms
[ ] 内存占用：Companion 常驻内存 < 2MB
```

### 10.7 安全审计

```
[ ] tmpPoint 路径不可被非 root 进程猜测或遍历
[ ] Companion IPC 不接受来自非授权进程的连接
[ ] 日志文件权限：/data/adb/pathguard/debug.log 仅 root 可读（0600）
[ ] 无任意文件写入漏洞（规则解析器 buffer 边界检查）
[ ] mount 操作仅在目标 App namespace 内执行，不影响全局
```

---

## 11. 开发路线图

| 阶段 | 目标 | 核心任务 | 预估工期 |
|---|---|---|---|
| **Phase 1（MVP）** | 稳定的基础路径隔离 | Zygisk + Companion 框架 · tmpfs/bind overlay · 多路径展开 · RAII 回滚 · SELinux 降级 · 线程安全 | 7-10 天 |
| **Phase 2（增强）** | 规则增强 + syscall fallback | glob 倒排索引 · whitelist 模式 · openat hook（shadowhook）过滤 mountinfo · 完整单元测试 | 7-10 天 |
| **Phase 3（Manager App）** | 可视化管理界面 | Android App · 规则编辑 · mount 状态查看 · 规则测试工具 · 热更新触发 · 导入/导出 | 10-14 天 |
| **Phase 4（生态）** | 发布与兼容 | ZygiskNext 兼容（KernelSU/APatch）· 自动更新 JSON · README / 免责声明 | 5-7 天 |

---

## 12. 参考资源

| 资源 | 说明 | 地址 |
|---|---|---|
| Magisk 官方开发文档 | 模块结构、脚本规范、sepolicy | https://topjohnwu.github.io/Magisk/guides.html |
| Zygisk 模块模板 | 官方 C++ 框架、编译配置、API | https://github.com/topjohnwu/zygisk-module-sample |
| AOSP 存储文档 | mount namespace、FUSE、sdcardfs、多用户存储 | https://source.android.com/docs/core/storage |
| AOSP sdcardfs 废弃 | Android 11+ FUSE 新实现 | https://source.android.com/docs/core/storage/sdcardfs-deprecate |
| AOSP SELinux 兼容性 | platform/vendor label 冲突处理 | https://source.android.com/docs/security/features/selinux/compatibility |
| setfscreatecon(3) man page | SELinux fscreate context API（线程本地、execve 后自动重置） | https://manpages.ubuntu.com/manpages/bionic/en/man3/setfscreatecon.3.html |
| getfilecon(3) man page | 读取文件 SELinux context API | https://linux.die.net/man/3/getfilecon |
| StorageRedirect | mount namespace 隔离方案参考 | https://github.com/RikkaApps/StorageRedirect-assets |
| rvmm-zygisk-mount | preAppSpecialize mount 注入实例 | https://github.com/j-hc/rvmm-zygisk-mount |
| NoHello | preAppSpecialize umount、线程安全、热更新实践 | https://github.com/MhmRdd/NoHello |
| FakeXposed | syscall hook（openat）方案参考，Layer 3 fallback | https://github.com/sanfengAndroid/FakeXposed |
| ZygiskNext | KernelSU/APatch 上的 Zygisk 兼容层 | https://github.com/Dr-TSNG/ZygiskNext |
| shadowhook | Android inline/PLT hook 库（Phase 2 syscall fallback） | https://github.com/bytedance/android-inline-hook |
| Magisk Magic Mount 分析 | tmpfs + bind mount 设计原理（DeepWiki） | https://deepwiki.com/topjohnwu/Magisk/2.2-module-system-and-magic-mount |

---

> **法律与伦理声明**：PathGuard 涉及 root 权限下的深度系统干预。发布时请在 README 和 Manager App 中明确告知：兼容性限制、可能的稳定性风险、卸载与恢复指南。不得用于规避合法安全检测或实施侵权行为。

---

*PathGuard Technical Design v3.0 · 整合全量审查报告（11项必修/强烈建议/可选修复）· 参照 AOSP 官方文档、Magisk 官方文档与主流同类软件设计*
