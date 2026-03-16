# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v2.0

> **参考来源**：Magisk 官方文档 · Zygisk API（topjohnwu/zygisk-module-sample）· Android AOSP 存储文档 · StorageRedirect（RikkaApps）· FakeXposed · rvmm-zygisk-mount · NeoZygisk · 审查报告 v1

---

## 目录

1. [方案选型](#1-方案选型)
2. [核心技术原理](#2-核心技术原理)
3. [架构设计](#3-架构设计)
4. [关键问题与修复方案](#4-关键问题与修复方案)
5. [模块文件结构](#5-模块文件结构)
6. [核心代码实现](#6-核心代码实现)
7. [配置格式规范](#7-配置格式规范)
8. [构建与安装](#8-构建与安装)
9. [功能设计](#9-功能设计)
10. [开发路线图](#10-开发路线图)
11. [参考资源](#11-参考资源)

---

## 1. 方案选型

### 1.1 核心方案

> **Zygisk + Per-App Mount Namespace + tmpfs 覆盖（+ bind mount 隐蔽层）**

在 App 进程 fork 完成、任何 App 代码尚未执行之前，在其私有 Mount Namespace 中将目标路径用空 tmpfs 覆盖。App 读到的是正常的空目录，既无报错，也不会崩溃，完全察觉不到真实内容的存在。

### 1.2 各方案对比

| 方案 | App 能察觉吗 | 会崩溃吗 | 路径精确度 | 实现难度 |
|---|---|---|---|---|
| 直接返回 `EACCES` | ❌ 能（报错可被检测） | ⚠️ 可能 | 文件级 | 低 |
| SELinux deny 规则 | ❌ 能（报错可被检测） | ⚠️ 可能 | 类型级 | 中 |
| Syscall Hook 返回假数据 | ✅ 较难察觉 | ✅ 不崩溃 | 文件/行级 | 高 |
| **tmpfs 覆盖（本方案）** | ✅ **完全透明** | ✅ 不崩溃 | 目录级 | 中 |
| tmpfs + bind mount 隐蔽（升级） | ✅ **最佳** | ✅ 不崩溃 | 目录级 | 中偏高 |

### 1.3 与同类软件对比

| 软件 | 核心技术 | 本方案异同 |
|---|---|---|
| StorageRedirect | bind mount + namespace | **最接近**，本方案在此基础上增加 tmpfs+bind 双层隐蔽 |
| XPrivacyLua | Java Hook（Xposed） | 权限级别，无法精确到路径 |
| FakeXposed | Native syscall hook | 更精细但实现复杂，可作为 fallback |
| Island | Work Profile 隔离 | 系统级，无法精确控制路径 |

**本方案定位**：`tmpfs overlay` + `bind mount 隐蔽` + `可选 syscall hook fallback`，形成三层防御。

---

## 2. 核心技术原理

### 2.1 Android 存储挂载结构（必须理解）

这是 v1 方案的最大盲区。**Android 的 `/sdcard` 并不是一个简单路径**，而是一套多层符号链接 + bind mount + FUSE/sdcardfs 的复杂映射体系。

根据 [Android AOSP 官方文档](https://source.android.com/docs/core/storage) 和 [Android StackExchange 分析](https://android.stackexchange.com/questions/205430)，App 进程内的真实路径链如下：

```
# App 进程内（Zygote fork 后的私有 namespace）
/sdcard                   →(symlink)→  /storage/self/primary
/storage/self             →(bind)→     /mnt/user/{USER_ID}
/mnt/user/{USER_ID}/primary →(symlink)→ /storage/emulated/{USER_ID}
/storage/emulated         →(bind)→     /mnt/runtime/{VIEW}/emulated
/mnt/runtime/{VIEW}/emulated →(FUSE/sdcardfs)→ /data/media
```

其中 `{VIEW}` 根据 App 权限不同分为三种（Android 6.0+）：

| VIEW | 适用场景 |
|---|---|
| `default` | 无存储权限的 App，及 root/global namespace |
| `read` | 持有 `READ_EXTERNAL_STORAGE` 的 App |
| `write` | 持有 `WRITE_EXTERNAL_STORAGE` 的 App |

**Android 11+ (FUSE scoped storage)**：sdcardfs 已被废弃，替换为新的 FUSE 实现，但路径映射结构基本一致。

**结论：若只 mount `/sdcard/secret`，仅覆盖了符号链接入口，App 通过 `/storage/emulated/0/secret` 或 `/mnt/runtime/write/emulated/0/secret` 仍可访问真实内容。**

### 2.2 Per-App Mount Namespace 机制

[Android AOSP External Storage Technical Information](https://android.googlesource.com/platform/docs/source.android.com) 指出：

> 平台在每个 Zygote fork 的进程中创建独立的 mount 表，再通过 bind mount 将用户特定的 primary external storage 注入该私有 namespace。

```
Zygote（全局 namespace，VIEW=default）
    │
    ├─ fork → App A 的私有 namespace
    │         继承 Zygote 的 mount 表副本
    │         vold 可通过 setns 进入此 namespace 并 bind mount 升级 VIEW
    │
    └─ fork → App B 的私有 namespace
              彼此完全隔离
```

**PathGuard 就在这个 fork 之后、App 代码执行之前的窗口内操作。**

### 2.3 Zygisk 生命周期切入点

根据 [Zygisk 官方 API（zygisk.hpp）](https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp)：

```
Zygote fork App 进程
    │
    ├─ [Zygisk] preAppSpecialize()    ← ✅ 在此操作
    │    • mount namespace 已从 Zygote 复制，已独立
    │    • App 任何代码尚未运行（ART 尚未初始化）
    │    • 进程仍持有较高权限，mount() 调用可成功
    │    • 可通过 connectCompanion() 获取 root 守护进程连接
    │
    ├─ App 进程 specialize（沙盒、UID drop 等）
    ├─ [Zygisk] postAppSpecialize()   ← 此时沙盒已启用，mount 可能失败
    └─ App 代码开始执行               ← App 看到的已是处理后的目录
```

> **重要**：根据 Zygisk API 注释，`preAppSpecialize` 在 specialize 之前运行，此时进程尚未进入 App 沙盒，是执行 mount 操作的唯一正确时机。

### 2.4 tmpfs + bind mount 双层覆盖原理

**v1 的单层 tmpfs 覆盖** 存在被 `/proc/self/mountinfo` 检测到的风险（mountinfo 会显示 `tmpfs /sdcard/secret`，异常明显）。

**v2 升级为 tmpfs → 隐蔽临时目录 → bind mount → 目标路径**：

```
步骤 1：在 /dev/pathguard_{random} 挂载 tmpfs（空目录）
步骤 2：bind mount /dev/pathguard_{random} → /storage/emulated/0/secret
步骤 3：同步覆盖所有等价路径（见 2.1）

mountinfo 中显示的是：
  /dev/pathguard_{random} on /storage/emulated/0/secret type tmpfs ...
  
这比直接 tmpfs 更接近系统正常的 bind mount 行为，不易触发异常检测。
```

---

## 3. 架构设计

### 3.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                        PathGuard                            │
│                                                             │
│  ┌─────────────────┐    ┌──────────────────────────────┐   │
│  │  Zygisk Module  │    │      Companion Daemon        │   │
│  │  (libpathguard) │◄──►│  (root 守护进程，常驻)        │   │
│  │                 │    │                              │   │
│  │ preAppSpecialize│    │  • 持有 rules 最新副本        │   │
│  │ • 查询规则       │    │  • 规则热更新（无需重启）     │   │
│  │ • 解析路径       │    │  • 日志写入                  │   │
│  │ • 执行 mount    │    │  • 文件权限读取               │   │
│  └─────────────────┘    └──────────────────────────────┘   │
│                                    ▲                        │
│                                    │ IPC                    │
│  ┌─────────────────┐               │                        │
│  │   Rule Engine   │    ┌──────────┴───────────────────┐   │
│  │                 │    │       Manager App            │   │
│  │ • unordered_map │    │  • 规则编辑 UI               │   │
│  │ • glob/fnmatch  │    │  • mount 状态可视化           │   │
│  │ • realpath 规范化│    │  • 日志查看                  │   │
│  │ • 多路径展开     │    │  • 规则测试工具               │   │
│  └─────────────────┘    └──────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Mount 操作流程

```
preAppSpecialize(args)
    │
    ├─ 1. connectCompanion() → 获取最新规则
    │
    ├─ 2. 匹配包名（unordered_map O(1)）
    │        未命中 → 直接返回
    │
    ├─ 3. MS_REC | MS_PRIVATE → 防止 mount 泄漏到父 namespace
    │
    ├─ 4. resolveStoragePaths(path, userId) → 展开所有等价路径
    │        /sdcard/X
    │        /storage/emulated/{user}/X
    │        /storage/self/primary/X
    │        /mnt/runtime/default/emulated/{user}/X
    │        /mnt/runtime/read/emulated/{user}/X
    │        /mnt/runtime/write/emulated/{user}/X
    │
    ├─ 5. sortPathsByDepth(paths) → 深路径优先，防止父目录 mount 覆盖子目录
    │
    └─ 6. 对每个路径执行 applyOverlay():
              stat() → 仅处理存在的目录
              lstat() → 解引用符号链接
              复制原始 mode/uid/gid
              mount tmpfs → /dev/pathguard_{random}  （隐蔽临时点）
              bind mount  → 目标路径               （覆盖原始内容）
              setfscreatecon() → 匹配 SELinux context
              错误日志记录
```

---

## 4. 关键问题与修复方案

> 以下逐条回应审查报告中的 6 个架构级问题 + 7 个工程级问题。

### 4.1 [架构问题 1] `/sdcard` 多路径映射

**问题**：只 mount `/sdcard/X` 无法覆盖 App 通过 `/storage/emulated/0/X` 的访问。

**修复**：实现 `resolveStoragePaths()`，自动展开所有等价路径：

```cpp
std::vector<std::string> resolveStoragePaths(
    const std::string &logicalPath, int userId) {

    // 提取相对于存储根的子路径
    // 输入：/sdcard/secret 或 /storage/emulated/0/secret
    std::string rel = extractRelativePath(logicalPath);  // → "secret"

    std::vector<std::string> result;
    auto add = [&](std::string base) {
        result.push_back(base + "/" + rel);
    };

    std::string uid = std::to_string(userId);
    add("/sdcard");
    add("/storage/emulated/" + uid);
    add("/storage/self/primary");
    add("/mnt/runtime/default/emulated/" + uid);
    add("/mnt/runtime/read/emulated/" + uid);
    add("/mnt/runtime/write/emulated/" + uid);

    return result;
}
```

`userId` 从 `AppSpecializeArgs::uid` 推导：`userId = uid / 100000`（Android 多用户规范）。

---

### 4.2 [架构问题 2] Mount Propagation 泄漏

**问题**：父 mount 为 `shared` 时，我们的 mount 可能传播到其他 namespace。

**修复**：在所有 mount 操作之前，将当前 namespace 的根设为 `private`：

```cpp
void isolateMountNamespace() {
    // 将整个 namespace 设为 MS_PRIVATE，阻断传播
    // 与 StorageRedirect 相同处理方式
    mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
}
```

在 `preAppSpecialize` 的最开始调用，仅对目标 App 执行。

---

### 4.3 [架构问题 3] 增加 bind mount 隐蔽层

**问题**：单纯 `tmpfs` 挂载在 `/proc/self/mountinfo` 中特征明显。

**修复**：改用 tmpfs → 隐蔽点 → bind mount → 目标路径：

```cpp
void applyOverlay(const std::string &target, uid_t uid, gid_t gid, mode_t mode) {
    // 在 /dev 下创建随机隐蔽挂载点（/dev 对 App 不可见）
    std::string tmpPoint = "/dev/pathguard_" + randomHex(8);
    mkdir(tmpPoint.c_str(), 0755);

    // 挂载 tmpfs 到隐蔽点
    std::string opts = "mode=" + modeToOctal(mode) +
                       ",uid=" + std::to_string(uid) +
                       ",gid=" + std::to_string(gid);
    if (mount("tmpfs", tmpPoint.c_str(), "tmpfs",
              MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME,
              opts.c_str()) != 0) {
        LOGE("tmpfs mount failed: %s -> %s: %s",
             tmpPoint.c_str(), target.c_str(), strerror(errno));
        rmdir(tmpPoint.c_str());
        return;
    }

    // bind mount 到目标路径（隐蔽原始内容）
    if (mount(tmpPoint.c_str(), target.c_str(), nullptr,
              MS_BIND, nullptr) != 0) {
        LOGE("bind mount failed: %s -> %s: %s",
             tmpPoint.c_str(), target.c_str(), strerror(errno));
        umount2(tmpPoint.c_str(), MNT_DETACH);
        rmdir(tmpPoint.c_str());
    }
    // tmpPoint 保留，作为 bind 的 source（App 的 /proc/self/mountinfo 中看不到 /dev）
}
```

---

### 4.4 [架构问题 4] 规则从 Companion Process 获取

**问题**：在 `onLoad()` 中直接读取文件，在某些进程（system_server 等）中 `/data/adb/...` 不可访问。

**修复**：规则完全由 Companion Process 持有并分发。

根据 [Zygisk API 文档](https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp)，Companion 运行在 root 守护进程中，是读取 `/data/adb/` 的正确位置：

```cpp
// ── 模块端 preAppSpecialize ──
void preAppSpecialize(AppSpecializeArgs *args) override {
    // 仅在需要时连接（有规则匹配时）
    int companionFd = api->connectCompanion();
    
    // 发送包名，接收规则列表
    std::string pkg = getPackageName(args);
    sendString(companionFd, pkg);
    
    std::vector<std::string> paths = receiveStringList(companionFd);
    close(companionFd);
    
    if (!paths.empty()) {
        isolateMountNamespace();
        applyMounts(paths, args->uid);
    }
}

// ── Companion 端（运行于 root 进程）──
static void companionHandler(int fd) {
    std::string pkg = receiveString(fd);
    
    // 从内存缓存中查找（unordered_map，O(1)）
    auto it = g_rules.find(pkg);
    if (it != g_rules.end()) {
        sendStringList(fd, it->second);
    } else {
        sendStringList(fd, {});  // 空列表
    }
}

REGISTER_ZYGISK_COMPANION(companionHandler)
```

Companion 在启动时从 `rules.conf` 加载规则到 `unordered_map`，并支持通过信号（`SIGUSR1`）触发热更新。

---

### 4.5 [架构问题 5] 规则查找性能优化

**问题**：`vector<Rule>` 线性查找，大规模规则集下有性能问题。

**修复**：使用 `unordered_map`，O(1) 精确查找：

```cpp
// Companion 全局规则表
std::unordered_map<std::string, std::vector<std::string>> g_rules;
// key:   "com.tencent.mm"
// value: ["/storage/emulated/0/private", "/storage/emulated/0/backup"]

// glob 规则单独存储（fnmatch 仍需遍历，但数量通常很少）
std::vector<std::pair<std::string, std::vector<std::string>>> g_glob_rules;
// e.g. {"com.badapp.*", [...]}
```

---

### 4.6 [架构问题 6] 路径规范化

**问题**：App 可能通过 `/sdcard/secret/../secret` 等变体绕过 mount 覆盖。

**修复**：规则加载时对所有路径调用 `realpath()` 规范化；mount 时同样规范化目标路径：

```cpp
std::string normalizePath(const std::string &path) {
    char resolved[PATH_MAX];
    // realpath 要求路径存在；若不存在，手动规范化 ".." 和 "."
    if (realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
    // 回退：手动折叠 ".." 和去除末尾 "/"
    return manualNormalize(path);
}
```

---

### 4.7 [工程问题 1] mount flags 补全

```cpp
// 旧：MS_NOSUID | MS_NODEV
// 新：
constexpr unsigned long TMPFS_FLAGS =
    MS_NOSUID   |  // 禁止 setuid/setgid
    MS_NODEV    |  // 禁止设备文件
    MS_NOEXEC   |  // 禁止在 tmpfs 内执行代码（防止注入）
    MS_RELATIME;   // 合理的 atime 更新策略（与系统一致）
```

---

### 4.8 [工程问题 2] UID/GID 动态获取

```cpp
// 旧：硬编码 uid=1000,gid=1000
// 新：从 AppSpecializeArgs 动态获取
void applyMounts(const std::vector<std::string> &paths,
                 uid_t appUid, gid_t appGid) {
    for (auto &path : paths) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) continue;

        // 优先使用 stat 得到的原始 mode/uid/gid，保持一致
        applyOverlay(path, st.st_uid, st.st_gid, st.st_mode & 07777);
    }
}
```

---

### 4.9 [工程问题 3] 完整错误处理

```cpp
#define LOGE(fmt, ...) \
    __android_log_print(ANDROID_LOG_ERROR, "PathGuard", fmt, ##__VA_ARGS__)

if (mount(...) != 0) {
    LOGE("[PathGuard] mount failed: target=%s errno=%d (%s)",
         target.c_str(), errno, strerror(errno));
    // 回写错误到 Companion 日志文件
    logToFile(target, errno);
    return;
}
```

---

### 4.10 [工程问题 4] 复制原始目录权限

```cpp
// stat 原始目录 → 复制 mode、uid、gid 到 tmpfs
struct stat st;
stat(target.c_str(), &st);

std::string opts = "mode=" + modeToString(st.st_mode & 07777) +
                   ",uid=" + std::to_string(st.st_uid) +
                   ",gid=" + std::to_string(st.st_gid);
// 挂载时传入 opts
```

---

### 4.11 [工程问题 5] SELinux Context 匹配

`/sdcard/Android/data` 等路径有特殊 SELinux label（如 `u:object_r:sdcard_external:s0`），若 tmpfs 使用默认 label，可能触发 EACCES。

**修复**：在 mount 前使用 `setfscreatecon()` 设定创建上下文：

```cpp
#include <selinux/selinux.h>

void applyOverlayWithContext(const std::string &target, ...) {
    // 读取目标路径的 SELinux label
    char *context = nullptr;
    getfilecon(target.c_str(), &context);

    if (context) {
        setfscreatecon(context);  // 新挂载点继承此 label
        freecon(context);
    }

    // 执行 mount...

    setfscreatecon(nullptr);  // 恢复默认
}
```

---

### 4.12 [工程问题 6] 符号链接处理

```cpp
void resolveAndBlock(const std::string &path, ...) {
    struct stat lst;
    lstat(path.c_str(), &lst);  // 不跟随符号链接

    if (S_ISLNK(lst.st_mode)) {
        // 解引用后对真实路径操作
        char resolved[PATH_MAX];
        realpath(path.c_str(), resolved);
        applyOverlay(std::string(resolved), ...);
    } else if (S_ISDIR(lst.st_mode)) {
        applyOverlay(path, ...);
    }
    // 非目录/非链接：跳过（见 4.1 说明）
}
```

---

### 4.13 [工程问题 7] Mount 顺序（深路径优先）

若规则中同时存在 `/sdcard` 和 `/sdcard/secret`，必须先 mount 深路径，否则父目录的 tmpfs 会遮盖子目录的 mount：

```cpp
void sortPathsByDepthDesc(std::vector<std::string> &paths) {
    std::sort(paths.begin(), paths.end(),
        [](const std::string &a, const std::string &b) {
            // 以路径深度（'/' 数量）降序排列
            return std::count(a.begin(), a.end(), '/') >
                   std::count(b.begin(), b.end(), '/');
        }
    );
}
```

---

## 5. 模块文件结构

根据 [Magisk 官方开发文档](https://topjohnwu.github.io/Magisk/guides.html)：

```
path-guard/
├── module.prop                  # 模块元信息（严格格式，LF 换行）
│
├── zygisk/                      # Zygisk native 库（官方规定目录名）
│   ├── arm64-v8a.so             # 主流 64 位 ARM 设备
│   ├── armeabi-v7a.so           # 32 位 ARM 兼容
│   ├── x86_64.so                # x86 模拟器
│   └── x86.so                   # x86 32 位模拟器
│
├── config/                      # 用户配置（模块自定义目录）
│   └── rules.conf               # 规则文件（格式见第 7 节）
│
├── action.sh                    # Magisk App「执行」按钮：检查/重载规则
├── uninstall.sh                 # 卸载清理脚本
└── skip_mount                   # 无需系统文件替换，创建此文件跳过 system 挂载
```

### module.prop

```properties
id=path_guard
name=PathGuard - 路径访问控制
version=v2.0.0
versionCode=2
author=YourName
description=透明隔离指定 App 对敏感路径的访问。基于 Zygisk + per-app mount namespace + tmpfs overlay，App 看到空目录，无报错，无崩溃。
updateJson=https://your-server.com/path-guard/update.json
```

> `id` 必须匹配正则 `^[a-zA-Z][a-zA-Z0-9._-]+$`，发布后不得修改。

---

## 6. 核心代码实现

### 6.1 项目结构

```
path-guard/
└── module/
    └── jni/
        ├── zygisk.hpp         # Zygisk API 头文件（来自官方模板）
        ├── module.cpp         # 主模块逻辑
        ├── companion.cpp      # Companion 守护进程逻辑
        ├── rules.h / rules.cpp # 规则解析与管理
        ├── mount.h / mount.cpp # mount 操作封装
        ├── path_utils.h       # 路径工具函数
        ├── Android.mk
        └── Application.mk
```

### 6.2 module.cpp（主模块）

```cpp
#include "zygisk.hpp"
#include "rules.h"
#include "mount.h"
#include "path_utils.h"
#include <android/log.h>
#include <sys/mount.h>
#include <unistd.h>

#define LOG_TAG "PathGuard"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace zygisk;

class PathGuard : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        // 1. 获取包名
        if (!args->nice_name) return;
        auto pkgJStr = args->nice_name;
        const char *pkgCStr = env->GetStringUTFChars(pkgJStr, nullptr);
        std::string pkg(pkgCStr);
        env->ReleaseStringUTFChars(pkgJStr, pkgCStr);

        // 2. 从 Companion 获取规则（companion 运行在 root 进程，可读文件）
        int fd = api->connectCompanion();
        sendString(fd, pkg);
        std::vector<std::string> blockedPaths = receiveStringList(fd);
        close(fd);

        if (blockedPaths.empty()) return;

        LOGI("Applying %zu path blocks for %s", blockedPaths.size(), pkg.c_str());

        // 3. 阻断 mount propagation
        isolateMountNamespace();

        // 4. 展开所有等价存储路径
        int userId = args->uid / 100000;
        std::vector<std::string> allPaths;
        for (auto &p : blockedPaths) {
            auto expanded = resolveStoragePaths(p, userId);
            allPaths.insert(allPaths.end(), expanded.begin(), expanded.end());
        }

        // 5. 深路径优先排序
        sortPathsByDepthDesc(allPaths);

        // 6. 应用覆盖
        for (auto &path : allPaths) {
            applyOverlay(path);
        }
    }

private:
    Api *api;
    JNIEnv *env;
};

REGISTER_ZYGISK_MODULE(PathGuard)
REGISTER_ZYGISK_COMPANION(companionHandler)
```

### 6.3 companion.cpp（Root 守护进程端）

```cpp
#include "rules.h"
#include "path_utils.h"
#include <unistd.h>
#include <signal.h>

// 全局规则表（精确包名）
std::unordered_map<std::string, std::vector<std::string>> g_rules;
// glob 规则（包名含通配符）
std::vector<std::pair<std::string, std::vector<std::string>>> g_glob_rules;

const char *RULES_PATH = "/data/adb/modules/path_guard/config/rules.conf";

void loadRules() {
    g_rules.clear();
    g_glob_rules.clear();

    std::ifstream f(RULES_PATH);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto sep = line.find('|');
        if (sep == std::string::npos) continue;

        std::string pkg  = trim(line.substr(0, sep));
        std::string path = normalizePath(trim(line.substr(sep + 1)));

        bool isGlob = pkg.find('*') != std::string::npos ||
                      pkg.find('?') != std::string::npos;

        if (isGlob) {
            // glob 规则：追加到 g_glob_rules
            auto it = std::find_if(g_glob_rules.begin(), g_glob_rules.end(),
                [&](auto &r){ return r.first == pkg; });
            if (it != g_glob_rules.end()) it->second.push_back(path);
            else g_glob_rules.push_back({pkg, {path}});
        } else {
            g_rules[pkg].push_back(path);
        }
    }
}

// 信号处理：SIGUSR1 触发热更新
void onReloadSignal(int) { loadRules(); }

void companionHandler(int fd) {
    std::string pkg = receiveString(fd);
    std::vector<std::string> result;

    // 精确匹配（O(1)）
    auto it = g_rules.find(pkg);
    if (it != g_rules.end()) {
        result = it->second;
    }

    // glob 匹配（fnmatch，仅在精确未命中或需合并时）
    for (auto &[pattern, paths] : g_glob_rules) {
        if (fnmatch(pattern.c_str(), pkg.c_str(), 0) == 0) {
            result.insert(result.end(), paths.begin(), paths.end());
        }
    }

    sendStringList(fd, result);

    // 可选：记录访问日志
    appendLog(pkg, result);
}

// Companion 启动时调用
__attribute__((constructor)) void companionInit() {
    loadRules();
    signal(SIGUSR1, onReloadSignal);
}
```

### 6.4 mount.cpp（mount 封装）

```cpp
#include "mount.h"
#include "path_utils.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <selinux/selinux.h>
#include <android/log.h>
#include <cstring>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PathGuard", __VA_ARGS__)

constexpr unsigned long TMPFS_FLAGS =
    MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME;

void isolateMountNamespace() {
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        LOGE("isolate namespace failed: %s", strerror(errno));
    }
}

void applyOverlay(const std::string &target) {
    struct stat lst;
    if (lstat(target.c_str(), &lst) != 0) return;  // 路径不存在，跳过

    // 处理符号链接：解引用后操作真实路径
    std::string realTarget = target;
    if (S_ISLNK(lst.st_mode)) {
        char resolved[PATH_MAX];
        if (realpath(target.c_str(), resolved) == nullptr) return;
        realTarget = resolved;
        if (stat(realTarget.c_str(), &lst) != 0) return;
    }

    if (!S_ISDIR(lst.st_mode)) return;  // 只处理目录

    // 创建隐蔽挂载点
    std::string tmpPoint = "/dev/pathguard_" + randomHex(8);
    mkdir(tmpPoint.c_str(), 0755);

    // 构建 tmpfs options（复制原始权限）
    std::string opts = "mode=" + modeOctal(lst.st_mode & 07777) +
                       ",uid=" + std::to_string(lst.st_uid) +
                       ",gid=" + std::to_string(lst.st_gid);

    // 匹配 SELinux context
    char *ctx = nullptr;
    if (getfilecon(realTarget.c_str(), &ctx) >= 0 && ctx) {
        setfscreatecon(ctx);
        freecon(ctx);
    }

    // Step 1: tmpfs → 隐蔽点
    if (mount("tmpfs", tmpPoint.c_str(), "tmpfs", TMPFS_FLAGS, opts.c_str()) != 0) {
        LOGE("tmpfs mount failed: %s (%s)", tmpPoint.c_str(), strerror(errno));
        setfscreatecon(nullptr);
        rmdir(tmpPoint.c_str());
        return;
    }

    // Step 2: bind mount → 目标路径
    if (mount(tmpPoint.c_str(), realTarget.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        LOGE("bind mount failed: %s -> %s (%s)",
             tmpPoint.c_str(), realTarget.c_str(), strerror(errno));
        umount2(tmpPoint.c_str(), MNT_DETACH);
        rmdir(tmpPoint.c_str());
    }

    setfscreatecon(nullptr);
}
```

### 6.5 Android.mk / Application.mk

```makefile
# module/jni/Android.mk
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE        := path_guard
LOCAL_SRC_FILES     := module.cpp companion.cpp rules.cpp mount.cpp path_utils.cpp
LOCAL_CFLAGS        := -std=c++17 -fvisibility=hidden -ffunction-sections -fdata-sections
LOCAL_LDFLAGS       := -Wl,--gc-sections
LOCAL_LDLIBS        := -llog -lselinux

include $(BUILD_SHARED_LIBRARY)
```

```makefile
# module/jni/Application.mk
APP_ABI      := arm64-v8a armeabi-v7a x86_64 x86
APP_PLATFORM := android-26
APP_STL      := none         # 官方要求：不得修改
APP_CFLAGS   := -std=c++17
```

---

## 7. 配置格式规范

### 7.1 rules.conf 格式

```ini
# PathGuard 规则文件
# 格式：<package_name>|<absolute_path>
# 支持：精确包名、glob 通配符（fnmatch 语法）
# 路径：绝对路径，会自动展开所有等价存储路径
# 编码：UTF-8，LF 换行

# ── 示例：精确包名 ──
com.tencent.mm|/sdcard/私密相册
com.tencent.mm|/sdcard/Documents/Personal

# ── 多路径：同一包名多行 ──
com.example.badapp|/sdcard/Backup
com.example.badapp|/sdcard/Download/work

# ── glob 通配符 ──
com.adware.*|/sdcard/DCIM

# ── 白名单模式（规划中，Phase 2）──
# mode=whitelist com.myapp
# allow com.myapp|/sdcard/Photos
# deny  com.myapp|*
```

### 7.2 多用户路径规范

规则中填写逻辑路径即可，模块自动按当前 App 的 userId 展开：

```
用户输入：/sdcard/secret
自动展开（以 user 0 为例）：
  /sdcard/secret
  /storage/emulated/0/secret
  /storage/self/primary/secret
  /mnt/runtime/default/emulated/0/secret
  /mnt/runtime/read/emulated/0/secret
  /mnt/runtime/write/emulated/0/secret
```

多用户（user 10）时自动展开为对应的 `/storage/emulated/10/...`。

---

## 8. 构建与安装

```bash
# 1. 克隆官方 Zygisk 模块模板（不含具体逻辑，仅提供框架）
git clone https://github.com/topjohnwu/zygisk-module-sample path-guard
cd path-guard

# 2. 将本项目源码放入 module/jni/

# 3. 确认 NDK 已安装（推荐 r25+）
export ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/25.2.9519653

# 4. 编译
cd module
$ANDROID_NDK_ROOT/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk

# 5. 复制产物
cp libs/arm64-v8a/libpath_guard.so   ../zygisk/arm64-v8a.so
cp libs/armeabi-v7a/libpath_guard.so ../zygisk/armeabi-v7a.so
cp libs/x86_64/libpath_guard.so      ../zygisk/x86_64.so
cp libs/x86/libpath_guard.so         ../zygisk/x86.so

# 6. 打包
cd ..
mkdir -p config
cp rules.conf config/
zip -r path-guard-v2.0.zip module.prop zygisk/ config/ action.sh uninstall.sh skip_mount

# 7. 安装（通过 Magisk Manager 或 adb）
adb push path-guard-v2.0.zip /sdcard/
# 在 Magisk → 模块 → 从本地安装 → 重启

# 8. 修改规则后热更新（无需重启）
adb shell "kill -USR1 \$(cat /proc/$(pidof magiskd)/status | grep Pid | awk '{print \$2}')"
# 注：新规则对已启动的 App 无效，需 App 重启
```

### 调试日志

```bash
# 实时查看 PathGuard 日志
adb logcat -s PathGuard

# 查看详细 mount 日志文件
adb shell cat /data/adb/pathguard.log
```

---

## 9. 功能设计

### 9.1 规则模式（Phase 1 → Phase 2）

| 模式 | 描述 | 实现阶段 |
|---|---|---|
| `blacklist`（默认） | 列出的路径对目标 App 不可见 | Phase 1 |
| `whitelist` | 只有列出的路径可见，其余全部屏蔽 | Phase 2 |
| glob 包名 | `com.badapp.*` 匹配所有子包 | Phase 1 |
| 多用户支持 | 自动适配 `{user}` 占位符 | Phase 1 |

### 9.2 Manager App 功能规划

| 功能 | 说明 |
|---|---|
| 规则编辑器 | 可视化增删包名/路径规则 |
| mount 状态查看 | 展示指定 App 当前 namespace 中的 tmpfs 挂载点 |
| 规则测试 | fork 测试进程验证 App 是否仍可访问目标路径 |
| 实时日志 | 显示哪些 App 触发了拦截（需 syscall hook 支持） |
| 热更新触发 | 一键 SIGUSR1，无需重启 |

### 9.3 三层防御（完整演进路径）

```
Layer 1：tmpfs + bind mount（当前 Phase 1）
    → App 看到空目录，无报错，无崩溃
    → 覆盖所有等价存储路径

Layer 2：SELinux label 匹配（Phase 3）
    → 确保系统路径的 context 一致性
    → 防止因 label 不匹配导致的隐性 EACCES

Layer 3：可选 syscall hook fallback（Phase 2，针对高风险 App）
    → 对持久读取 /proc/self/mountinfo 检测的 App
    → hook openat，对 mountinfo 内容进行过滤
    → 类似 FakeXposed 方案
```

---

## 10. 开发路线图

| 阶段 | 目标 | 核心任务 | 预估工期 |
|---|---|---|---|
| **Phase 1（MVP）** | 基础路径隔离 | Zygisk 框架 + Companion + tmpfs/bind mount + 多路径展开 + rules.conf 解析 | 5-7 天 |
| **Phase 2（完善）** | 规则增强 + syscall fallback | glob 包名 + whitelist 模式 + openat hook（shadowhook） + 热更新完善 | 7-10 天 |
| **Phase 3（加固）** | SELinux + App | SELinux context 匹配 + Manager App（Android） | 10-14 天 |
| **Phase 4（生态）** | 发布 + 兼容 | ZygiskNext 兼容（KernelSU/APatch）+ 自动更新 JSON + 文档 | 5-7 天 |

---

## 11. 参考资源

| 资源 | 说明 | 地址 |
|---|---|---|
| Magisk 官方开发文档 | 模块结构、脚本规范、sepolicy | https://topjohnwu.github.io/Magisk/guides.html |
| Zygisk 模块模板 | 官方 C++ 框架、编译配置 | https://github.com/topjohnwu/zygisk-module-sample |
| Android AOSP 存储文档 | mount namespace、FUSE、sdcardfs | https://source.android.com/docs/core/storage |
| Android 存储路径链分析 | `/sdcard` → `/storage/emulated/0` 映射 | https://android.stackexchange.com/questions/205430 |
| sdcardfs 废弃说明 | Android 11+ FUSE 新实现 | https://source.android.com/docs/core/storage/sdcardfs-deprecate |
| StorageRedirect | mount namespace 隔离方案参考 | https://github.com/RikkaApps/StorageRedirect-assets |
| FakeXposed | syscall hook 方案参考 | https://github.com/sanfengAndroid/FakeXposed |
| rvmm-zygisk-mount | preAppSpecialize mount 注入实例 | https://github.com/j-hc/rvmm-zygisk-mount |
| NeoZygisk | 高级 namespace 管理参考 | https://github.com/JingMatrix/NeoZygisk |
| ZygiskNext | 跨 Magisk/KernelSU 兼容方案 | https://github.com/Dr-TSNG/ZygiskNext |
| shadowhook | Android inline/PLT hook 库 | https://github.com/bytedance/android-inline-hook |

---

*PathGuard Technical Design v2.0 · 修订：整合系统级审查报告全部 13 项问题，参照 AOSP 存储文档与主流同类软件设计*
