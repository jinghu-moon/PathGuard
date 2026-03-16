# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v5.0

> **参考来源**：Magisk 官方文档 · Zygisk API · AOSP 存储/MediaProvider/SELinux 文档 · StorageRedirect · FakeXposed · rvmm-zygisk-mount · NoHello · ZygiskNext · cppreference `lexically_normal` · v4.0 深度审查报告 · rule-format-plan.md
>
> **v5 核心变更**：MediaStore 绕过漏洞明确标注与应对路线 · 纯内存字面量路径规范化（`lexically_normalize`，消灭 `../` 穿越攻击）· tmpfs 零 IPC 规则缓存（彻底抛弃热路径 Socket）· `<pkg>` 占位符语法糖 · 「沙盘推演」规则诊断器 · 重定向产品定位（卸载即焚）· FUSE 环境 bind mount 策略优化

---

## 目录

1. [方案总览](#1-方案总览)
2. [核心技术原理](#2-核心技术原理)
3. [整体架构](#3-整体架构)
4. [防绕过漏洞修复（Critical）](#4-防绕过漏洞修复critical)
5. [性能极致优化（零 IPC 架构）](#5-性能极致优化零-ipc-架构)
6. [规则格式设计](#6-规则格式设计)
7. [规则引擎设计](#7-规则引擎设计)
8. [Mount 执行层设计](#8-mount-执行层设计)
9. [Companion Daemon 设计](#9-companion-daemon-设计)
10. [Zygisk 模块主流程](#10-zygisk-模块主流程)
11. [日志系统设计](#11-日志系统设计)
12. [模块文件结构](#12-模块文件结构)
13. [构建与安装](#13-构建与安装)
14. [功能设计规划](#14-功能设计规划)
15. [测试与发布清单](#15-测试与发布清单)
16. [开发路线图](#16-开发路线图)
17. [参考资源](#17-参考资源)

---

## 1. 方案总览

### 1.1 核心方案（不变）

> **Zygisk + Per-App Mount Namespace + tmpfs 覆盖 + bind mount 隐蔽层 + 零 IPC 规则缓存**

### 1.2 v5 相比 v4 的核心升级

| 维度 | v4 | v5 |
|---|---|---|
| **热路径 IPC** | Socket + 50ms 超时 fail-open | **零 IPC**：直接 `open()+read()` 读 tmpfs 序列化文件 |
| **路径安全** | `realpath()`（依赖磁盘，可被 `../` 绕过） | **`lexicalNormalize()`**（纯内存，零磁盘 IO，消灭路径穿越） |
| **MediaStore 漏洞** | 未提及 | **明确标注已知限制**，Phase 3 给出应对路线 |
| **FUSE bind mount** | 对 FUSE 路径直接操作 | 优先使用底层 `/data/media/0/...` 规避 FUSE 损耗 |
| **规则语法** | 无占位符 | `<pkg>` 占位符，减少重复规则 |
| **产品定位** | 重定向为技术特性 | **重定向 = "卸载即焚"**，明确产品卖点 |
| **Manager App** | 日志查看 + 规则编辑 | 增加「沙盘推演」规则诊断器（所见即所得）|

### 1.3 三层防御体系

```
Layer 1（Phase 1，当前实现）
  tmpfs overlay + bind mount 隐蔽层 + 重定向
  → App 直接文件 IO（open/read/write）被拦截
  → 覆盖所有等价存储路径（含 FUSE 底层路径）

Layer 2（Phase 3，SELinux 加固 + MediaStore 对抗）
  → SELinux label 三级降级兼容
  → MediaProvider 进程特殊隔离（Xposed Hook ContentResolver）

Layer 3（Phase 2，可选 syscall fallback）
  → openat hook 过滤 /proc/self/mountinfo
  → 针对主动读取 mountinfo 的检测型 App
```

---

## 2. 核心技术原理

### 2.1 Android 存储路径映射

```
/sdcard
  └─(symlink)→ /storage/self/primary
                 └─(bind)→ /mnt/user/{userId}/primary
                              └─(symlink)→ /storage/emulated/{userId}
                                              └─(bind)→ /mnt/runtime/{VIEW}/emulated/{userId}
                                                          └─(FUSE)→ /data/media/{userId}
                                                                          ↑
                                                              底层真实路径（v5 优先操作此处）
```

Android 6.0+ 三种 VIEW：`default` / `read` / `write`。**必须同时覆盖所有等价路径**，包括底层 FUSE 路径 `/data/media/{userId}/...`。

### 2.2 MediaStore 绕过机制（v5 新增说明）

这是 **mount namespace 隔离方案最大的架构性盲区**。

根据 AOSP 文档，MediaProvider 模块维护了媒体文件的索引集合，并通过 MediaStore 公共 API 向 App 开放，同时强制执行 Android 10 引入的 Scoped Storage 安全模型。

**漏洞机制**：

```
App 进程（已被 PathGuard 隔离）
    │
    └─ ContentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, ...)
              │
              └─ Binder IPC → MediaProvider 进程（系统进程，未被注入 PathGuard 规则）
                                    │
                                    └─ 直接写入 /data/media/0/DCIM/...（物理存储）
                                         ↑
                                    ✅ 穿透了 PathGuard 的 tmpfs 隔离！
```

**根本原因**：`mount namespace` 隔离只对 App 自身进程内的文件 IO 有效。通过 ContentResolver 的写入由 MediaProvider 代为执行，MediaProvider 运行在独立进程，不在 PathGuard 的作用域内。

**v5 处理策略**：

Phase 1 明确标注为「已知限制」，Phase 3 提供完整对抗路线（见第 14 节）。

### 2.3 Zygisk 切入时机

```
Zygote fork App
    │
    ├─ [Zygisk] preAppSpecialize()   ← ✅ 唯一正确时机
    │    • mount namespace 已独立
    │    • 沙盒/UID drop 尚未执行
    │    • mount() 可成功
    │    • 直接 open()+read() 读取规则缓存文件（零 IPC）
    │
    ├─ App specialize（UID drop、SELinux 域切换）
    └─ App 代码执行
```

---

## 3. 整体架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│                            PathGuard v5                                  │
│                                                                          │
│  ┌──────────────────────────┐    ┌─────────────────────────────────────┐ │
│  │     Zygisk Module        │    │        Companion Daemon             │ │
│  │    (libpathguard.so)     │    │      (root 守护进程，常驻)            │ │
│  │                          │    │                                     │ │
│  │  preAppSpecialize:       │    │  ┌──────────────────────────────┐   │ │
│  │   1. open()+read()       │    │  │       Rule Engine            │   │ │
│  │      → 读 tmpfs 规则缓存  │    │  │  • INI 解析 + 编译           │   │ │
│  │      （零 IPC，内存级）   │    │  │  • AppPolicy 序列化          │   │ │
│  │   2. MS_PRIVATE          │    │  │  • 写入 tmpfs 规则缓存        │   │ │
│  │   3. 展开 + 去重 + 排序   │    │  │  • Trie 前缀树               │   │ │
│  │   4. RAII mount          │    │  │  • lexicalNormalize（零IO）   │   │ │
│  │   5. 处理重定向           │    │  │  • shared_mutex 热更新       │   │ │
│  └──────────────────────────┘    │  └──────────────────────────────┘   │ │
│            ▲                     │  ┌──────────────────────────────┐   │ │
│            │ open()+read()        │  │       Log System             │   │ │
│            │ 零 IPC，仅读文件      │  │  • Ring buffer（异步）       │   │ │
│  ┌─────────┴────────────────┐    │  │  • 采样策略                  │   │ │
│  │   tmpfs 规则缓存          │    │  │  • 批量刷盘 + 轮转           │   │ │
│  │  /dev/.pathguard/        │◄───┤  └──────────────────────────────┘   │ │
│  │    {pkg}.bin             │    │                                     │ │
│  │  （内存文件系统，无磁盘IO）│    └─────────────────────────────────────┘ │
│  └──────────────────────────┘               ▲ SIGUSR1 热更新              │
│                                             │                            │
│                              ┌──────────────┴──────────────────────────┐ │
│                              │           Manager App                   │ │
│                              │  • 规则编辑器（INI 可视化）              │ │
│                              │  • 「沙盘推演」规则诊断器                │ │
│                              │  • Audit 日志查看                       │ │
│                              │  • mount 状态可视化                     │ │
│                              │  • 热更新触发                           │ │
│                              └─────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 4. 防绕过漏洞修复（Critical）

### 4.1 🔴 路径穿越漏洞修复：纯内存字面量规范化

**v4 漏洞**：依赖 `realpath()` 做路径规范化，而 `realpath()` 要求路径在磁盘上存在。当路径尚未创建时，`realpath()` 失败，代码回退到原始字符串，导致 `../` 构造的路径绕过 Trie 匹配：

```
攻击路径：DCIM/Camera/../../A-TEST/hide.txt
规则：    - DCIM/A-TEST
realpath() 失败（路径不存在）→ 原始字符串进入 Trie → 未命中 → 放行 ❌
```

**v5 修复**：在路径进入 Trie 之前，强制执行纯内存字面量规范化 `lexicalNormalize()`，**不做任何磁盘 IO**，不跟随符号链接，纯字符串操作消灭所有 `.` 和 `..`。

根据 cppreference，`lexically_normal()` 是纯词法操作，不检查路径是否存在，不跟随符号链接，完全不访问文件系统。这正是安全防御需要的特性——不能让攻击者利用"路径不存在时 `realpath()` 失败"的边界条件绕过规则。

```cpp
// 纯内存字面量规范化，消灭 . 和 ..，无任何磁盘 IO
// 算法来自 POSIX 路径规范化标准，对应 C++17 path::lexically_normal()
std::string lexicalNormalize(const std::string &rawPath) {
    std::vector<std::string> parts;
    std::istringstream ss(rawPath);
    std::string seg;
    bool isAbsolute = !rawPath.empty() && rawPath[0] == '/';

    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") {
            continue;           // 跳过空段和当前目录引用
        } else if (seg == "..") {
            if (!parts.empty()) parts.pop_back();  // 回退一级
            // 绝对路径中 .. 超过根时静默忽略（安全处理）
        } else {
            parts.push_back(seg);
        }
    }

    std::string result = isAbsolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += '/';
        result += parts[i];
    }
    return result.empty() ? "/" : result;
}

// 规范化流水线：别名替换 → 字面量规范化 → Trie 查询
// 此函数在规则加载期和运行时路径查询时均调用
std::string normalizePath(const std::string &path) {
    // Step 1: 别名替换
    std::string p = path;
    replaceAlias(p, "/sdcard/",                  "/storage/emulated/0/");
    replaceAlias(p, "/storage/self/primary/",    "/storage/emulated/0/");

    // Step 2: 纯内存字面量规范化（消灭 ../ 穿越攻击）
    return lexicalNormalize(p);
}
```

**覆盖场景**：

```
DCIM/Camera/../../A-TEST/hide.txt
  → abs: /storage/emulated/0/DCIM/Camera/../../A-TEST/hide.txt
  → lexical: /storage/emulated/0/A-TEST/hide.txt
  → Trie 命中规则 "- DCIM/A-TEST" → 正确拦截 ✅

/storage/emulated/0/DCIM/A-TEST/.
  → lexical: /storage/emulated/0/DCIM/A-TEST
  → 正确匹配 ✅

/storage/emulated/0/DCIM/A-TEST_backup    ← 兄弟路径
  → Trie 精确前缀匹配（+/分隔符保护）→ 不命中 ✅
```

---

### 4.2 🔴 MediaStore 绕过漏洞：明确边界与应对路线

**Phase 1 已知限制**：mount namespace 隔离无法阻止通过 `ContentResolver` / `MediaStore` API 的写入，因为这些操作由 MediaProvider 进程代理执行，不受 App 进程 namespace 约束。

**在文档中标注**：

```
⚠️ 已知限制（Phase 1）
PathGuard 的路径保护仅对以下访问方式有效：
  ✅ Native 文件 IO（open/read/write/stat 等 syscall）
  ✅ Java 层 FileInputStream/FileOutputStream（底层仍走 open）
  ✅ JNI 文件访问

以下访问方式在 Phase 1 不受保护：
  ❌ ContentResolver.insert() / MediaStore API
  ❌ SAF（Storage Access Framework）DocumentProvider
  ❌ MTP/PTP 文件传输
```

**Phase 3 应对路线**（两个子方案，按实现成本排序）：

```
方案 A（推荐）：Xposed/LSPosed Hook 目标 App 的 ContentResolver
  → 在 App 进程内 Hook Java 层的 ContentResolver.insert/query/delete
  → 对目标路径返回虚假结果或拒绝操作
  → 代价：需要 Xposed 环境，增加依赖

方案 B：对 MediaProvider 进程做额外 mount namespace 隔离
  → 在 com.android.providers.media.module 进程 preAppSpecialize 中
    对其 namespace 也执行 tmpfs 覆盖
  → 代价：会影响全局媒体库，可能引发媒体扫描异常
  → 参考：StorageRedirect 后期版本的媒体库重定向机制
```

---

### 4.3 🔴 Mount Propagation：MS_REC 强制要求

**v4 风险**：若 `MS_PRIVATE` 未带 `MS_REC`，深层 FUSE 挂载点（如 `/storage/emulated/0` 内部的 FUSE 节点）可能保留 `shared` 传播属性，导致 tmpfs 或 bind mount 意外泄漏到全局 namespace，引发系统级连锁崩溃。

**v5 强制**：`MS_PRIVATE | MS_REC` 必须同时指定，且失败时立即放弃全部 mount 操作：

```cpp
bool isolateMountNamespace() {
    // MS_REC 是强制要求，不可省略
    // 若只有 MS_PRIVATE 无 MS_REC，FUSE 深层挂载点可能仍是 shared
    if (mount(nullptr, "/", nullptr, MS_PRIVATE | MS_REC, nullptr) != 0) {
        LOGE("CRITICAL: MS_PRIVATE|MS_REC failed (errno=%d: %s). "
             "Aborting ALL mounts to prevent namespace leakage.",
             errno, strerror(errno));
        return false;  // 安全优先，宁可不拦截也不泄漏
    }
    // 额外针对 /storage 显式隔离，增强 FUSE 场景下的可靠性
    mount(nullptr, "/storage", nullptr, MS_PRIVATE | MS_REC, nullptr);
    return true;
}
```

---

## 5. 性能极致优化：零 IPC 架构

### 5.1 架构动机

v4 的 Socket IPC + 50ms 超时方案存在以下问题：
- 开机广播并发期，Companion 过载导致大量 App 触发 fail-open，保护形同虚设
- 每次 App 启动都有 Socket 建立开销
- 50ms 延迟对冷启动依然可感知

**v5 终极方案：完全抛弃热路径 Socket 通信**

### 5.2 零 IPC 规则缓存设计

```
Companion Daemon（规则编译器）
    │
    ├─ 解析 rules.ini
    ├─ 编译为 AppPolicy 结构体
    ├─ 序列化为二进制格式
    └─ 写入 /dev/.pathguard/{pkg}.bin
              （tmpfs 内存文件系统，无磁盘 IO）

preAppSpecialize（Zygisk 模块）
    │
    └─ open("/dev/.pathguard/com.tencent.mm.bin", O_RDONLY)
       read(fd, &policy, sizeof(policy))
       close(fd)
       → 内存拷贝级别，耗时 < 1 微秒
```

**关键优势**：
- **性能**：内存拷贝级别，无上下文切换，无 IPC 开销
- **可靠性**：Companion 崩溃不影响已生成的规则文件
- **并发安全**：只读操作，无竞态条件
- **超时无关**：没有 Socket，没有超时，没有 fail-open 误判

### 5.3 tmpfs 缓存目录设计

```
/dev/.pathguard/             ← tmpfs（内存文件系统，系统重启自动清空）
├── com.tencent.mm.bin       ← 序列化的 AppPolicy
├── com.tencent.mobileqq.bin
└── .version                 ← 规则版本号（用于 Zygisk 端检测更新）
```

```bash
# Companion 启动时挂载 tmpfs 缓存目录
mount -t tmpfs -o mode=0700,uid=0,gid=0 tmpfs /dev/.pathguard
chmod 700 /dev/.pathguard
```

### 5.4 AppPolicy 二进制序列化格式

```cpp
// 紧凑的二进制序列化格式（固定大小字段 + 变长字符串区）
// 设计原则：Zygisk 端 read() 一次即可完整载入，无需动态分配

struct SerializedHeader {
    uint32_t magic;          // 0x50475244 ("PGRD")
    uint32_t version;        // 格式版本号
    uint32_t mode;           // BLACKLIST=0, WHITELIST=1
    uint32_t ruleCount;
    uint32_t dataSize;       // 后续变长数据区大小
};

struct SerializedRule {
    uint8_t  action;         // ALLOW=0, DENY=1, REDIRECT=2
    uint8_t  kind;           // FILE=0, DIR=1, AUTO=2
    uint16_t priority;       // 路径深度（/的数量）
    uint32_t pathOffset;     // 路径字符串在数据区的偏移
    uint32_t pathLen;
    uint32_t redirectOffset; // 重定向目标偏移（REDIRECT时有效）
    uint32_t redirectLen;
};

// 完整序列化结构：
// [SerializedHeader][SerializedRule × N][字符串数据区]
```

### 5.5 Zygisk 端读取流程（零 IPC）

```cpp
bool loadPolicyFromCache(const std::string &pkg, AppPolicy *out) {
    std::string cachePath = "/dev/.pathguard/" + pkg + ".bin";

    int fd = open(cachePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;  // 无规则 = fail-open

    SerializedHeader hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic != 0x50475244) {
        close(fd);
        return false;
    }

    // 一次性读取全部数据（内存拷贝，无网络/IPC）
    std::vector<uint8_t> data(hdr.dataSize);
    if (read(fd, data.data(), hdr.dataSize) != (ssize_t)hdr.dataSize) {
        close(fd);
        return false;
    }
    close(fd);

    // 反序列化（纯内存操作）
    return deserializePolicy(hdr, data, out);
}
```

### 5.6 规则热更新流程（SIGUSR1）

```
用户触发热更新（Manager App → SIGUSR1 → Companion）
    │
    ├─ Companion：unique_lock → 重新编译 rules.ini
    ├─ 原子替换：写入 .bin.tmp → rename → .bin
    │   （rename 是原子操作，Zygisk 端读到的永远是完整文件）
    └─ 更新 /dev/.pathguard/.version
```

> **注意**：热更新对已运行的 App 无效（mount 已在进程启动时完成），只对下次启动的 App 生效。

---

## 6. 规则格式设计

### 6.1 规则文件格式（来自 rule-format-plan.md，v5 新增占位符）

```ini
# PathGuard 规则文件 v5
# 文件位置：config/rules.ini
# 编码：UTF-8，LF 换行
#
# 三句话记住：
#   + 路径              → 允许访问
#   - 路径              → 禁止/隐藏
#   源路径 -> 目标路径   → 重定向（卸载即焚）
#
# 路径不以 / 开头时自动补全为 /storage/emulated/0/<路径>
# 程序自动推断文件/目录类型，自动展开所有等价存储路径
# <pkg> 占位符会被替换为当前分组的包名（v5 新增）

[global]
audit = false          # true = 只记录不拦截，用于规则验证
missing_path = skip    # skip（默认）或 create（危险，可被检测）

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

# <pkg> 占位符：自动替换为 com.tencent.mobileqq
Tencent/QQfile_recv -> Android/data/<pkg>/files/QQfile_hidden

[com.UCMobile]
mode = blacklist

# <pkg> 占位符简化重复规则
Download -> Android/data/<pkg>/files/Download
```

### 6.2 `<pkg>` 占位符（v5 新增）

**动机**：用户若想把多个浏览器的下载目录重定向到各自沙盒，需要在每个分组里重复写路径，维护成本高。

**语义**：编译期将 `<pkg>` 替换为当前分组的包名：

```ini
[com.UCMobile]
Download -> Android/data/<pkg>/files/Download

# 编译后等价于：
Download -> Android/data/com.UCMobile/files/Download
# 即：/storage/emulated/0/Download
#     → /storage/emulated/0/Android/data/com.UCMobile/files/Download
```

**实现**：

```cpp
std::string expandPlaceholders(const std::string &path, const std::string &pkg) {
    std::string result = path;
    size_t pos;
    const std::string placeholder = "<pkg>";
    while ((pos = result.find(placeholder)) != std::string::npos) {
        result.replace(pos, placeholder.size(), pkg);
    }
    return result;
}
```

### 6.3 路径补全与规范化流水线

```
用户输入：DCIM/Camera/../../A-TEST/hide.txt

Step 1: 相对路径补全
  → /storage/emulated/0/DCIM/Camera/../../A-TEST/hide.txt

Step 2: <pkg> 占位符展开
  → （若有）替换为包名

Step 3: 别名替换（/sdcard → /storage/emulated/0 等）

Step 4: 字面量规范化 lexicalNormalize()（纯内存，消灭 ../）
  → /storage/emulated/0/A-TEST/hide.txt

Step 5: 等价路径展开（覆盖所有 VIEW 和符号链接入口）
  → /sdcard/A-TEST/hide.txt
  → /storage/emulated/0/A-TEST/hide.txt
  → /storage/self/primary/A-TEST/hide.txt
  → /mnt/runtime/default/emulated/0/A-TEST/hide.txt
  → /mnt/runtime/read/emulated/0/A-TEST/hide.txt
  → /mnt/runtime/write/emulated/0/A-TEST/hide.txt
  → /data/media/0/A-TEST/hide.txt   ← v5 新增：FUSE 底层路径

Step 6: 去重 + 深路径优先排序
```

### 6.4 重定向的产品定位（v5 明确）

重定向功能的核心产品价值：**"卸载即焚"**——被重定向的数据生命周期与 App 强绑定，卸载 App 时 Android 系统自动清理 `Android/data/{pkg}/` 目录，文件随之销毁，无残留。

```
传统行为：微信把文件写入 /storage/emulated/0/Pictures/WechatImage
  → 卸载微信后，文件永久留在存储卡

PathGuard 重定向：
  Pictures/WechatImage -> Android/data/com.tencent.mm/files/WechatImage
  → 文件实际写入 Android/data/com.tencent.mm/files/WechatImage
  → 卸载微信 → 系统自动清理 Android/data/com.tencent.mm/
  → 文件随 App 一起消失，零残留
```

> 在 Manager App 的交互文案中，重点宣传此特性：这不仅是隐私隔离，更是防止流氓软件在外部存储"拉屎"的神器。

---

## 7. 规则引擎设计

（继承 v4 设计，补充 v5 新增内容）

### 7.1 内部 AppPolicy 编译结构

```cpp
enum class Action   { ALLOW, DENY, REDIRECT };
enum class PathKind { FILE, DIR, AUTO };
enum class Mode     { BLACKLIST, WHITELIST };

struct CompiledRule {
    Action      action;
    PathKind    kind;
    std::string path;        // lexicalNormalize 后的规范路径
    std::string redirectTo;  // REDIRECT 时有效（已展开 <pkg>）
    int         priority;    // 路径 '/' 数量，越深越优先
};

struct AppPolicy {
    std::string              package;
    Mode                     mode;
    std::vector<CompiledRule> rules;  // 按 priority 降序排列
    PathTrie                 trie;    // Trie 前缀树快速查找
};
```

### 7.2 Trie 前缀树匹配（O(path_length)）

规则匹配复杂度与规则数量无关，只与查询路径长度相关：

```cpp
struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;
    const CompiledRule *rule = nullptr;
};

class PathTrie {
    TrieNode root;
public:
    void insert(const std::string &path, const CompiledRule *rule) {
        auto *cur = &root;
        for (auto &seg : splitBySlash(path)) {
            auto &child = cur->children[seg];
            if (!child) child = std::make_unique<TrieNode>();
            cur = child.get();
        }
        cur->rule = rule;
    }

    // 最长前缀匹配：返回最具体的规则（深路径优先）
    const CompiledRule *match(const std::string &path) const {
        auto *cur = &root;
        const CompiledRule *best = nullptr;
        for (auto &seg : splitBySlash(path)) {
            auto it = cur->children.find(seg);
            if (it == cur->children.end()) break;
            cur = it->second.get();
            if (cur->rule) best = cur->rule;
        }
        return best;
    }
};
```

### 7.3 目录路径安全匹配（防兄弟路径误伤）

```cpp
// 必须加 "/" 分隔符检查，防止 A-TEST 误匹配 A-TEST_backup
bool matchDir(const std::string &rulePath, const std::string &queryPath) {
    return queryPath == rulePath ||
           (queryPath.size() > rulePath.size() &&
            queryPath.rfind(rulePath + "/", 0) == 0);
}
```

### 7.4 优先级冲突解决

1. 路径更长（更具体）的规则优先
2. 同路径：文件规则优先于目录规则
3. 同具体程度：`DENY > REDIRECT > ALLOW`

### 7.5 黑白名单判定逻辑

**BLACKLIST**：默认放行，命中 DENY → 拒绝，命中 REDIRECT → 重定向，未命中 → 放行

**WHITELIST**：默认拒绝，命中 ALLOW → 放行，命中 REDIRECT → 重定向放行，命中 DENY → 显式拒绝，未命中 → 拒绝

### 7.6 三层路径类型推断（加载期）

1. **尾部斜杠**：`DCIM/A-TEST/` → 目录
2. **`lstat()` 检查**：路径存在时，`S_ISDIR` → 目录，`S_ISREG` → 文件
3. **扩展名启发式**：`.txt/.jpg/.mp4` 等 → 文件
4. **默认兜底**：→ 目录（用户写无扩展名路径绝大多数是目录）

---

## 8. Mount 执行层设计

### 8.1 FUSE 环境优化（v5 新增）

Android 11+ 全面使用 FUSE，对 FUSE 节点大量 bind mount 可能导致 I/O 异常。**v5 策略：优先对底层 `/data/media/{userId}/...` 路径操作，规避 FUSE 层开销**：

```cpp
std::vector<std::string> resolveStoragePaths(const std::string &logicalPath, int userId) {
    std::string rel = extractRelativePath(logicalPath);
    std::string uid = std::to_string(userId);

    std::vector<std::string> result;
    // FUSE 层路径（从外到里）
    result.push_back("/sdcard/" + rel);
    result.push_back("/storage/emulated/" + uid + "/" + rel);
    result.push_back("/storage/self/primary/" + rel);
    result.push_back("/mnt/runtime/default/emulated/" + uid + "/" + rel);
    result.push_back("/mnt/runtime/read/emulated/"    + uid + "/" + rel);
    result.push_back("/mnt/runtime/write/emulated/"   + uid + "/" + rel);
    // v5 新增：FUSE 底层真实路径（优先）
    result.push_back("/data/media/" + uid + "/" + rel);

    return result;
}
```

对 `/data/media/{userId}/...` 的 bind mount 直接作用于 FUSE 底层数据，避免 FUSE VFS 层的额外开销。

### 8.2 Deny（tmpfs 覆盖）

与 v4 一致：
- RAII `MountGuard` 自动回滚
- 模块私有目录 `/data/adb/modules/path_guard/.tmp/pg_{random}`
- `getrandom()` 安全随机数
- SELinux 三级降级策略（setfscreatecon → bind template → skip）
- errno 分类处理

### 8.3 Redirect（bind mount 重定向）

```cpp
void applyRedirect(const std::string &src, const std::string &dst) {
    // 确保重定向目标存在（优先创建在 App 私有目录）
    if (mkdirs(dst.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE("redirect: dst create failed: %s (%s)", dst.c_str(), strerror(errno));
        return;
    }

    // 确保源目录存在（路径可能尚未创建）
    if (mkdirs(src.c_str(), 0755) != 0 && errno != EEXIST) {
        LOGE("redirect: src create failed: %s (%s)", src.c_str(), strerror(errno));
        return;
    }

    // bind mount dst → src
    // App 访问 src，看到的是 dst 的真实内容
    if (mount(dst.c_str(), src.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        LOGE("redirect: bind failed: %s->%s (%d: %s)",
             src.c_str(), dst.c_str(), errno, strerror(errno));
    }
}
```

### 8.4 WHITELIST 存储根覆盖

```cpp
void applyWhitelistMode(const AppPolicy &policy, int userId) {
    // 1. 覆盖整个存储根（默认拒绝）
    std::string storageRoot = "/storage/emulated/" + std::to_string(userId);
    applyDeny(storageRoot, /* uid, gid, mode from stat */);

    // 2. 对 ALLOW 规则：在 tmpfs 内创建对应目录，bind mount 恢复真实内容
    for (auto &rule : policy.rules) {
        if (rule.action == Action::ALLOW) {
            std::string inTmpfs = storageRoot + extractRelativePath(rule.path);
            mkdirs(inTmpfs.c_str(), 0755);
            mount(rule.path.c_str(), inTmpfs.c_str(), nullptr, MS_BIND | MS_REC, nullptr);
        } else if (rule.action == Action::REDIRECT) {
            applyRedirect(storageRoot + extractRelativePath(rule.path), rule.redirectTo);
        }
    }
}
```

---

## 9. Companion Daemon 设计

```cpp
static std::shared_mutex g_mutex;
static std::unordered_map<std::string, AppPolicy> g_policies;
static std::atomic<bool> g_reloading{false};

void reloadAndWriteCache() {
    bool expected = false;
    if (!g_reloading.compare_exchange_strong(expected, true)) return;

    auto newPolicies = compileRulesFile(RULES_PATH);

    // 1. 写入 tmpfs 缓存（原子替换）
    for (auto &[pkg, policy] : newPolicies) {
        std::string tmpPath = CACHE_ROOT + "/" + pkg + ".bin.tmp";
        std::string finalPath = CACHE_ROOT + "/" + pkg + ".bin";
        writeSerializedPolicy(tmpPath, policy);
        rename(tmpPath.c_str(), finalPath.c_str());  // 原子替换
    }

    // 2. 更新内存规则表
    {
        std::unique_lock lk(g_mutex);
        g_policies = std::move(newPolicies);
    }

    // 3. 更新版本号
    writeVersionFile(CACHE_ROOT + "/.version");

    g_reloading.store(false);
    LOGI("Rules compiled and cached: %zu policies", g_policies.size());
}

__attribute__((constructor)) void companionInit() {
    // 挂载 tmpfs 缓存目录
    mkdir(CACHE_DIR.c_str(), 0700);
    mount("tmpfs", CACHE_DIR.c_str(), "tmpfs",
          MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0700,uid=0,gid=0");

    mkdir(TMP_ROOT.c_str(), 0700);
    cleanupStaleTmpDirs();
    reloadAndWriteCache();
    signal(SIGUSR1, [](int) { reloadAndWriteCache(); });
}
```

---

## 10. Zygisk 模块主流程

```cpp
class PathGuard : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args->nice_name) return;

        const char *pkgC = env->GetStringUTFChars(args->nice_name, nullptr);
        std::string pkg(pkgC);
        env->ReleaseStringUTFChars(args->nice_name, pkgC);

        // 零 IPC：直接读取 tmpfs 缓存文件
        AppPolicy policy;
        if (!loadPolicyFromCache(pkg, &policy)) return;  // 无规则 = fail-open

        // 阻断 mount propagation（MS_PRIVATE | MS_REC，缺一不可）
        if (!isolateMountNamespace()) return;

        int userId = static_cast<int>(args->uid / 100000);

        if (policy.mode == Mode::WHITELIST) {
            applyWhitelistMode(policy, userId);
            return;
        }

        // BLACKLIST 模式
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
};

REGISTER_ZYGISK_MODULE(PathGuard)
REGISTER_ZYGISK_COMPANION(companionHandler)
```

---

## 11. 日志系统设计

### 11.1 Ring Buffer 异步日志

```
preAppSpecialize（热路径）
  └─ logAsync(event) → ring buffer 内存写入（< 1μs）

Companion 后台线程（每秒 or 阈值触发）
  ├─ 批量读取 ring buffer
  ├─ 采样过滤
  └─ 批量写入 /data/adb/pathguard/debug.log
```

### 11.2 采样策略

```
✅ 总是记录：DENY（被拦截）、REDIRECT（重定向）、mount 错误
📊 采样记录：ALLOW（默认放行，调试模式下 1%）
❌ 不记录：系统进程（package 为空或以 android. 开头）
```

### 11.3 Audit 模式

```ini
[global]
audit = true
```

所有应执行的 mount 操作改为只记录 `WOULD_DENY` / `WOULD_REDIRECT` 日志，不实际执行，App 正常运行，用于生产前验证规则正确性。

### 11.4 日志安全

```
路径：/data/adb/pathguard/debug.log
权限：0600（仅 root 可读，保护路径隐私）
轮转：超过 1MB → rotate，保留最近 3 个
```

---

## 12. 模块文件结构

```
path-guard/
├── module.prop
├── skip_mount
├── zygisk/
│   ├── arm64-v8a.so
│   ├── armeabi-v7a.so
│   ├── x86_64.so
│   └── x86.so
├── config/
│   └── rules.ini
├── action.sh
└── uninstall.sh       # rm -rf .tmp/、/dev/.pathguard、日志
```

```properties
# module.prop
id=path_guard
name=PathGuard - 路径访问控制
version=v5.0.0
versionCode=5
author=YourName
description=零 IPC 规则缓存 + tmpfs/bind overlay + 黑白名单 + 重定向（卸载即焚）。Zygisk + per-app mount namespace。
updateJson=https://your-server.com/path-guard/update.json
```

---

## 13. 构建与安装

```bash
# 1. 克隆官方模板（含 libcxx submodule，APP_STL=none 时需要）
git clone --recurse-submodules https://github.com/topjohnwu/zygisk-module-sample path-guard
cd path-guard

# 2. 替换 module/jni/ 源码

# 3. 编译（NDK r21+）
cd module
$ANDROID_NDK_ROOT/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk

# 4. 复制产物
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
  cp libs/$abi/libpath_guard.so ../zygisk/$abi.so
done

# 5. 打包安装
cd ..
zip -r path-guard-v5.0.zip module.prop zygisk/ config/ action.sh uninstall.sh skip_mount
adb push path-guard-v5.0.zip /sdcard/
# Magisk → 本地安装 → 重启

# 6. 热更新（无需重启）
adb shell "kill -USR1 \$(pidof magiskd)"

# 7. 调试
adb logcat -s PathGuard
adb shell cat /data/adb/pathguard/debug.log
```

---

## 14. 功能设计规划

### 14.1 Phase 1（当前）支持

- INI 分组 + `mode` + `+/-/->` + `<pkg>` 占位符
- 相对路径补全 + 中文/空格路径
- `lexicalNormalize()` 防路径穿越
- 等价路径展开（含 `/data/media/{userId}/...`）
- Trie 前缀树匹配
- 零 IPC tmpfs 规则缓存
- RAII mount 回滚
- SELinux 三级降级
- Ring buffer 异步日志
- Audit 模式
- 热更新（SIGUSR1）

### 14.2 Phase 2（syscall fallback）

- openat hook（shadowhook）过滤 `/proc/self/mountinfo`
- 运行期路径类型缓存更新

### 14.3 Phase 3（MediaStore 对抗 + Manager App）

**MediaStore 对抗**：
```
方案 A：Xposed/LSPosed Hook App 进程内 ContentResolver
方案 B：对 com.android.providers.media.module 进程做额外 mount 隔离
        参考 StorageRedirect 的媒体库重定向机制
```

**Manager App 核心功能**：

| 功能 | 说明 |
|---|---|
| 规则编辑器 | INI 可视化，分组展示 |
| **「沙盘推演」诊断器**（v5 新增） | 见下方详细设计 |
| Audit 日志 | 展示 would-block 事件 |
| mount 状态查看 | 当前 App namespace 的活跃挂载 |
| 热更新触发 | 一键 SIGUSR1 |
| 导入/导出 | 规则文件备份 |

### 14.4 「沙盘推演」规则诊断器（v5 新增）

这是降低用户调试成本最有效的功能，预计可拦截 80% 的用户 Issue 反馈。

**交互设计**：

```
┌─────────────────────────────────────────┐
│         规则沙盘推演                     │
│                                         │
│  包名：[com.tencent.mm            ▼]    │
│  路径：[DCIM/Camera/1.jpg            ]  │
│  操作：[写入                      ▼]    │
│                                         │
│  [▶ 推演]                               │
│                                         │
│  ─────────────────────────────────────  │
│  ✅ 命中第 3 条规则（REDIRECT）          │
│     DCIM/Camera -> Android/data/...     │
│                                         │
│  实际操作：                              │
│  写入将被重定向到：                      │
│  /storage/emulated/0/Android/data/      │
│  com.tencent.mm/cache/Camera/1.jpg      │
│                                         │
│  卸载 App 后此文件将自动删除 🗑          │
└─────────────────────────────────────────┘
```

**技术实现**：纯 Companion 端逻辑，根据 AppPolicy 对输入路径执行 `lexicalNormalize` + Trie 匹配 + 冲突解决，返回命中规则和最终行为，**不 fork 进程，不做实际 mount**。

---

## 15. 测试与发布清单

### 15.1 安全测试（v5 新增）

```
[ ] 路径穿越防御：DCIM/Camera/../../A-TEST → 正确命中 A-TEST 规则
[ ] 多重穿越：../../../../etc/passwd → 正确展平为根路径
[ ] 空段处理：/sdcard//DCIM → 正确规范化为 /sdcard/DCIM
[ ] 兄弟路径隔离：A-TEST 规则不误伤 A-TEST_backup
[ ] MS_REC 验证：mount propagation 不泄漏到全局 namespace
[ ] tmpfs 缓存文件权限：0600，仅 root 可读
```

### 15.2 功能测试

```
[ ] DENY：open/stat/listdir 返回空/ENOENT
[ ] ALLOW（whitelist）：目标路径正常可见
[ ] REDIRECT：访问 src 看到 dst 内容，写入实际落到 dst
[ ] <pkg> 占位符展开正确
[ ] MediaStore 写入：Phase 1 确认绕过（预期，标注为已知限制）
[ ] Audit 模式：不执行 mount，只记录 WOULD_DENY/WOULD_REDIRECT
[ ] 热更新：SIGUSR1 后 .bin 文件更新，下次启动的 App 使用新规则
[ ] 零 IPC 验证：strace preAppSpecialize 确认无 socket 调用
[ ] 卸载清理：uninstall.sh 后 .tmp/、/dev/.pathguard、日志均删除
```

### 15.3 性能测试

```
[ ] 零 IPC 场景下 preAppSpecialize 延迟 < 2ms（3条规则）
[ ] 对比 v4 Socket IPC 延迟，期望 > 10x 改善
[ ] 1000条规则：Trie 匹配延迟 < 0.1ms（对比线性 O(N) 基准）
[ ] Ring buffer 写入不阻塞 preAppSpecialize（< 1μs）
[ ] Companion 编译 1000条规则耗时 < 100ms（热更新可接受延迟）
```

### 15.4 Android 版本兼容

```
[ ] Android 8.0 (API 26) — getrandom() 基准
[ ] Android 9/10 (API 28/29) — Scoped Storage 引入
[ ] Android 11 (API 30) — sdcardfs → FUSE，/data/media 路径验证
[ ] Android 12/13/14 (API 31-34)
```

### 15.5 OEM 兼容

```
[ ] MIUI / HyperOS — SELinux 严格
[ ] EMUI / MagicOS — 存储路径定制
[ ] ColorOS        — /mnt/runtime 差异
[ ] OneUI          — Knox 额外检测
[ ] AOSP / LineageOS — 基准
```

### 15.6 Magisk 生态

```
[ ] Magisk stable / canary
[ ] KernelSU + ZygiskNext / ReZygisk
[ ] APatch + ZygiskNext
[ ] 与 LSPosed 共存
[ ] 与 Shamiko 共存
```

---

## 16. 开发路线图

| 阶段 | 目标 | 核心任务 | 工期 |
|---|---|---|---|
| **Phase 1（MVP）** | 零 IPC + 稳定隔离 | INI + `<pkg>` 占位符 · `lexicalNormalize` · AppPolicy 编译 + 序列化 · tmpfs 零 IPC 缓存 · Trie 匹配 · Deny/Redirect/Whitelist · RAII 回滚 · Ring buffer 日志 · Audit 模式 | 12-16 天 |
| **Phase 2（fallback）** | mountinfo 检测对抗 | openat hook · /proc/self/mountinfo 过滤 · 运行期路径类型缓存 | 7-10 天 |
| **Phase 3（完整生态）** | MediaStore + Manager App | ContentResolver Hook（LSPosed）· 沙盘推演诊断器 · 完整 Manager App | 14-21 天 |
| **Phase 4（发布）** | 发布 + 兼容 | ZygiskNext / ReZygisk 兼容 · 自动更新 JSON · 通配符支持 · 文档 | 5-7 天 |

---

## 17. 参考资源

| 资源 | 说明 | 地址 |
|---|---|---|
| Magisk 官方开发文档 | 模块结构、脚本、sepolicy | https://topjohnwu.github.io/Magisk/guides.html |
| Zygisk 模块模板 | 官方框架、libcxx、`APP_STL=none` | https://github.com/topjohnwu/zygisk-module-sample |
| AOSP 存储文档 | mount namespace、FUSE、多用户 | https://source.android.com/docs/core/storage |
| AOSP MediaProvider 模块 | MediaStore 架构、Scoped Storage 实现 | https://source.android.com/docs/core/media/media-provider |
| AOSP sdcardfs 废弃 | Android 11+ FUSE 实现 | https://source.android.com/docs/core/storage/sdcardfs-deprecate |
| cppreference: `lexically_normal` | 纯内存路径规范化（消灭 `../`） | https://en.cppreference.com/w/cpp/filesystem/path/lexically_normal |
| Android MediaStore 开发者文档 | ContentResolver/Scoped Storage API | https://developer.android.com/training/data-storage/shared/media |
| setfscreatecon(3) man page | SELinux fscreate context API | https://manpages.ubuntu.com/manpages/bionic/en/man3/setfscreatecon.3.html |
| StorageRedirect | bind mount + namespace 参考 | https://github.com/RikkaApps/StorageRedirect-assets |
| rvmm-zygisk-mount | preAppSpecialize mount 注入实例 | https://github.com/j-hc/rvmm-zygisk-mount |
| NoHello | preAppSpecialize umount、线程安全 | https://github.com/MhmRdd/NoHello |
| FakeXposed | syscall hook 方案（Phase 2） | https://github.com/sanfengAndroid/FakeXposed |
| ZygiskNext / ReZygisk | KernelSU/APatch 兼容层 | https://github.com/Dr-TSNG/ZygiskNext |
| shadowhook | Android inline/PLT hook 库 | https://github.com/bytedance/android-inline-hook |

---

> **法律与伦理声明**：PathGuard 涉及 root 权限下的深度系统干预。发布时请在 README 和 Manager App 中明确告知兼容性限制、稳定性风险、卸载与恢复指南，并禁止用于规避合法安全检测或侵权行为。

---

*PathGuard Technical Design v5.0 · 核心升级：零 IPC tmpfs 规则缓存 · lexicalNormalize 路径穿越防御 · MediaStore 漏洞边界标注 · `<pkg>` 占位符 · 「沙盘推演」诊断器 · FUSE 底层路径覆盖 · 重定向「卸载即焚」产品定位*
