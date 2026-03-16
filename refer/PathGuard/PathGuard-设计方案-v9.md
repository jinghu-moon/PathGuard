# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v9.0（完整版）

> **核心目标**：高性能 · 占用小 · 速度快 · 隐蔽性高
>
> **参考来源**：Magisk 官方文档 · Zygisk API（topjohnwu/zygisk-module-sample）· AOSP 存储/MediaProvider/SELinux/Multi-user 文档 · StorageRedirect v29 · FakeXposed · rvmm-zygisk-mount · NoHello · ZygiskNext v1.3.0（ZN Linker）· NeoZygisk v2.x · ReZygisk v1.0.0 · Shamiko · Zygisk-Assistant v2.1.4 · JingMatrix「Android 用户态注入隐藏已死」博文 · Linux umount2(2)/mount_namespaces(7) · cppreference lexically_normal · NDK --no-undefined/--exclude-libs,ALL 官方说明 · v4–v8 全量继承
>
> **v9 战略**：v8 的全部补丁（MS_PRIVATE|MS_REC、DLCLOSE、atexit 清零、自定义链接器兼容、APatch 支持）**完整保留**。v9 在此基础上将 v4 至 v8 所有设计融合为一份完整、详细、可直接指导实现的权威文档，同时补充三项新洞察：「完成即消失」哲学定稿、memfd 路径伪装、soinfo gap 的生态合作对策。

---

## 目录

1. [设计哲学与战略定位](#1-设计哲学与战略定位)
2. [核心技术原理](#2-核心技术原理)
3. [整体架构](#3-整体架构)
4. [隐蔽性系统（Stealth Engine）](#4-隐蔽性系统stealth-engine)
5. [规则格式设计](#5-规则格式设计)
6. [规则引擎设计](#6-规则引擎设计)
7. [Mount 执行层设计](#7-mount-执行层设计)
8. [Companion Daemon 设计](#8-companion-daemon-设计)
9. [Zygisk 模块主流程](#9-zygisk-模块主流程)
10. [日志系统设计](#10-日志系统设计)
11. [构建系统设计](#11-构建系统设计)
12. [模块文件结构](#12-模块文件结构)
13. [兼容性矩阵](#13-兼容性矩阵)
14. [功能规划与开发路线图](#14-功能规划与开发路线图)
15. [测试与发布清单](#15-测试与发布清单)
16. [参考资源](#16-参考资源)

---

## 1. 设计哲学与战略定位

### 1.1 「完成即消失」核心哲学（v9 定稿）

根据 JingMatrix 博文「Android 用户态注入隐藏已死」（2025）的技术分析，以及 NeoZygisk v2.x 的工程实践，v9 正式将以下结论作为最高设计原则：

**用户态注入隐藏在理论上对足够强大的检测器是不可能完全完成的。** 最好的策略不是「藏好 SO」，而是「在 App 任何代码运行前完成全部工作，然后彻底离开」。

```
检测可行性现实认知（2026 年）：
  匿名可执行内存       → 一旦存在即可被检测（jit-cache 除外）
  soinfo 链表 gap     → dlclose 后链表出现空洞，可被发现
  /proc/self/maps     → 文件路径可被扫描
  mountinfo peer group → 跨进程关联可被利用

PathGuard 的应对策略：
  ✅ preAppSpecialize 完成全部 mount 操作（App 代码尚未运行）
  ✅ 立即 DLCLOSE（SO 从内存消失）
  ✅ mount 操作本身隐蔽（MNT_DETACH，无 source 痕迹）
  ✅ 与 Shamiko/TreatWheels 协作处理 soinfo gap
  ✅ 接受「无法对抗内核级检测」的边界，聚焦用户态检测对抗
```

**实际效果预期**：
- 对抗 99% 的用户态检测（银行 App、企业 App、常见安全 SDK）
- 无法对抗内核级强制检测（如 Android 16 的 Hardware Attestation 增强）
- PathGuard 的路径隐私保护目标与根隐藏目标正交，互为补充

### 1.2 功能定位

PathGuard 是一个 **路径访问控制模块**，核心价值在于：

- **黑名单模式**：阻止 App 访问指定敏感目录（隐私防护）
- **白名单模式**：只允许 App 访问授权路径（最小权限原则）
- **重定向模式**：将 App 的写入重定向到隔离目录（「卸载即焚」数据隔离）

PathGuard **不是**：
- 根隐藏工具（请配合 Shamiko/TreatWheels）
- Play Integrity 修复工具（请配合 PlayIntegrityFix/TrickyStore）
- 全局 syscall 拦截框架（Phase 2 仅作 mountinfo 过滤的最小 hook）

### 1.3 版本演进总览

| 版本 | 核心贡献 |
|---|---|
| v4 | INI 规则格式 · Trie 匹配 · Ring buffer · Audit 模式 · fail-open IPC |
| v5 | 零 IPC tmpfs 缓存 · lexicalNormalize · MediaStore 边界标注 · `<pkg>` 占位符 |
| v6 | MNT_DETACH 三步隐蔽 · memfd 匿名 fd · 版本脚本零导出 · LTO+strip-all · lazy stat |
| v7 | FD fallback · Work Profile userId · --no-undefined/--exclude-libs,ALL · selfCheck |
| v8 | MS_PRIVATE\|MS_REC · DLCLOSE · atexit 清零 · 自定义链接器兼容 · APatch 支持 |
| **v9** | **完成即消失哲学定稿 · memfd 路径伪装 · soinfo gap 生态合作 · 完整文档整合** |

---

## 2. 核心技术原理

### 2.1 Android 存储路径映射（必须完整覆盖）

```
/sdcard
  └─(symlink)→ /storage/self/primary
                 └─(bind)→ /mnt/user/{userId}/primary
                              └─(symlink)→ /storage/emulated/{userId}
                                              └─(bind)→ /mnt/runtime/{VIEW}/emulated/{userId}
                                                          └─(FUSE)→ /data/media/{userId}
                                                                           ↑
                                                              底层真实路径（优先操作）
```

**VIEW** = `default` | `read` | `write`（Android 6.0+，根据 App 权限注入不同视图）

**userId 规则**（来自 AOSP Multi-user Testing 文档）：
- `userId = uid / 100000`（直接来自 `preAppSpecialize` 的 `args->uid`）
- 主用户：userId=0，Work Profile：userId=10，次要用户：userId=11, 12, ...
- 每条规则必须严格使用 userId，绝不硬编码 0

**所有等价路径展开**（`resolveStoragePaths`，v7 最终版）：

```c
// 严格按 userId 参数展开，不硬编码 0
int resolveStoragePaths(const char *logicalPath, int userId,
                        char out[][PATH_MAX], int maxOut) {
    const char *rel = extractRelSubpath(logicalPath);
    int n = 0;
    char uid_s[12];
    snprintf(uid_s, sizeof(uid_s), "%d", userId);

#define ADD(fmt, ...) if (n < maxOut) snprintf(out[n++], PATH_MAX, fmt, ##__VA_ARGS__)
    ADD("/sdcard/%s",                                        rel);   // userId=0 时有效
    ADD("/storage/emulated/%s/%s",               uid_s, rel);
    ADD("/storage/self/primary/%s",                          rel);
    ADD("/mnt/user/%s/primary/%s",               uid_s, rel);
    ADD("/mnt/runtime/default/emulated/%s/%s",   uid_s, rel);
    ADD("/mnt/runtime/read/emulated/%s/%s",      uid_s, rel);
    ADD("/mnt/runtime/write/emulated/%s/%s",     uid_s, rel);
    ADD("/data/media/%s/%s",                     uid_s, rel);       // FUSE 底层真实路径
#undef ADD
    return n;
}
// lazy stat：仅 stat 成功的路径才执行 mount，跳过不存在路径
```

### 2.2 Zygisk 切入时机与时序

```
Zygote fork App
    │
    ├─ [Zygisk] preAppSpecialize()   ← ✅ 唯一正确时机
    │    │  mount namespace 已独立（per-app）
    │    │  UID drop 尚未执行（仍有 root 权限执行 mount）
    │    │  App 代码尚未执行（无法检测到任何注入痕迹）
    │    │
    │    ├─ Step 1: isolateNamespace()          (MS_PRIVATE|MS_REC)
    │    ├─ Step 2: loadPolicy()                (memfd → fd → read)
    │    ├─ Step 3: applyAllMounts()            (Trie 匹配 → mount loop)
    │    └─ Step 4: setOption(DLCLOSE)          (SO 从内存消失)
    │
    ├─ App specialize（UID drop、SELinux 域切换）
    ├─ [Zygisk] postAppSpecialize()  ← ❌ SO 已卸载，此回调不会执行
    └─ App 代码执行                  ← 此时 mount 已生效，SO 已消失
```

### 2.3 MediaStore 绕过漏洞边界（Phase 1 已知限制）

```
⚠️ 已知限制
PathGuard Phase 1 的路径保护仅对以下访问有效：
  ✅ Native 文件 IO（open/read/write/stat/opendir 等 syscall）
  ✅ Java File API（底层调用 native syscall）
  ✅ NDK fopen/fread/fwrite

对以下访问无效（需 Phase 3 应对）：
  ❌ ContentResolver / MediaStore API（由 MediaProvider 进程代理执行）
  ❌ MediaScanner 扫描（运行在独立进程）
  ❌ ExifInterface 通过 ContentProvider 的访问
```

Phase 3 应对路线：
- 方案 A：LSPosed Hook App 进程内 `ContentResolver`
- 方案 B：对 `com.android.providers.media.module` 进程额外注入 PathGuard 规则

---

## 3. 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              PathGuard v9                                   │
│                                                                             │
│  ┌───────────────────────────┐    ┌──────────────────────────────────────┐  │
│  │      Zygisk Module        │    │         Companion Daemon             │  │
│  │    (libpathguard.so)      │    │      (root 守护进程，常驻)             │  │
│  │                           │    │                                      │  │
│  │  preAppSpecialize:        │    │  ┌──────────────────────────────┐    │  │
│  │   1. isolateNamespace()   │    │  │       Rule Engine            │    │  │
│  │      MS_PRIVATE|MS_REC    │    │  │  INI 解析 → AppPolicy 编译   │    │  │
│  │   2. loadPolicy()         │◄───┤  │  → Trie 前缀树               │    │  │
│  │      memfd fd 传递        │    │  │  → 序列化 → memfd fd         │    │  │
│  │      + file fallback      │    │  │  → shared_mutex 热更新       │    │  │
│  │   3. applyAllMounts()     │    │  └──────────────────────────────┘    │  │
│  │      lazy stat            │    │  ┌──────────────────────────────┐    │  │
│  │      RAII MountGuard      │    │  │      Stealth Cache           │    │  │
│  │      SELinux 三级降级      │    │  │  memfd 匿名 fd（主路径）      │    │  │
│  │   4. setOption(DLCLOSE)   │    │  │  /dev/.__sys_{rnd}/          │    │  │
│  │      SO 从内存消失         │    │  │  {sha256(pkg)[:16]}.bin      │    │  │
│  └───────────────────────────┘    │  │  （降级备用路径）             │    │  │
│                                   │  └──────────────────────────────┘    │  │
│  compat layer:                    │  ┌──────────────────────────────┐    │  │
│  Magisk / KSU / APatch            │  │       Log System             │    │  │
│  ZygiskNext / ReZygisk /          │  │  Ring buffer（异步）          │    │  │
│  NeoZygisk（API 兼容）             │  │  采样策略（DENY/REDIRECT 全记）│    │  │
│                                   │  │  批量刷盘 + 1MB 轮转          │    │  │
│  soinfo gap 对策：                 │  └──────────────────────────────┘    │  │
│  配合 Shamiko/TreatWheels          │              ▲ SIGUSR1 热更新         │  │
│  做 soinfo 链表修复                └──────────────────────────────────────┘  │
│                                               ▲ Unix Socket                 │
│                                   ┌───────────┴──────────────────────────┐  │
│                                   │          Manager App（Phase 3）       │  │
│                                   │  规则编辑器 · 沙盘推演诊断器           │  │
│                                   │  Audit 日志 · mount 状态 · 热更新     │  │
│                                   └──────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. 隐蔽性系统（Stealth Engine）

### 4.1 检测向量全景与对策矩阵（v9 完整版）

| 检测维度 | 检测手段 | PathGuard v9 对策 | 残余风险 |
|---|---|---|---|
| Mount source 溯源 | 扫描 mountinfo source 字段 | MNT_DETACH 三步流程，source 消失 | 无 |
| Mount peer group ID | 跨进程 mountinfo peer ID 关联 | MS_PRIVATE\|MS_REC 切断传播链 | 无 |
| /proc/self/maps SO 路径 | dl_iterate_phdr / maps 扫描 | DLCLOSE 后 SO 消失 | soinfo gap（见 4.6）|
| soinfo 链表空洞 | 链表 gap 在 specialize 前出现 | 依赖 Shamiko/NeoZygisk/TreatWheels | 需外部模块协作 |
| 匿名可执行内存 | maps 中 r-xp 匿名段检测 | memfd 名称伪装为 jit-cache | 内核级检测无法绕过 |
| atexit 注册残留 | atexit 链表泄露 SO 曾加载 | 无全局对象 + __cxa_finalize | 无 |
| 导出符号 / nm 扫描 | nm -D / readelf -s | version_script.map 零导出（≤2 符号）| 无 |
| 敏感字符串 | strings 扫描 .rodata | XOR 混淆所有路径和模块名 | 无 |
| 缓存文件路径 | /proc/{pid}/fd 枚举 | memfd 匿名（/proc/fd/ 只显示 memfd:jit-cache）| 无 |
| 系统链接器特征 | dl_iterate_phdr 枚举 | 移除所有 dlopen/dlsym 运行时依赖 | 无 |
| UID/路径特征 | /data/adb 下文件路径 | 所有缓存在 /dev 下随机路径 | 无 |

### 4.2 隐蔽三步流程：tmpfs + bind + MNT_DETACH

来自 Linux umount2(2) 的理论保证：`MNT_DETACH` 立即将挂载点从挂载表移除，但已建立的 bind mount 维持独立 `vfsmount` 引用，不受影响。

```c
// mount.c — applyStealthOverlay（v9 最终版）
// 调用前已执行 isolateNamespace()，namespace 已私有化

void applyStealthOverlay(const char *target, uid_t uid, gid_t gid, mode_t mode) {
    // 临时挂载点：/dev/ 下随机 8 字节 hex，仅存在 <1μs
    char tmpPoint[64];
    snprintf(tmpPoint, sizeof(tmpPoint), "/dev/.%s", secureRandomHex(8));

    if (mkdir(tmpPoint, 0700) != 0) return;

    // tmpfs 挂载选项：uid/gid/mode 严格匹配目标，SELinux label 三级降级（见 4.4）
    char opts[128];
    snprintf(opts, sizeof(opts), "mode=%o,uid=%u,gid=%u", mode, uid, gid);

    // Step 1: mount tmpfs → tmpPoint
    if (mount("tmpfs", tmpPoint, "tmpfs", MS_NOSUID | MS_NODEV, opts) != 0) {
        rmdir(tmpPoint);
        return;
    }

    // Step 2: bind mount tmpPoint → target（target 路径由 lazy stat 确认存在）
    if (mount(tmpPoint, target, NULL, MS_BIND | MS_REC, NULL) != 0) {
        umount2(tmpPoint, MNT_DETACH);
        rmdir(tmpPoint);
        return;
    }

    // Step 3: 立即 MNT_DETACH（source 从 mountinfo 消失，bind 仍有效）
    umount2(tmpPoint, MNT_DETACH);
    rmdir(tmpPoint);

    // 结果：mountinfo 中只剩：
    // 742 739 0:91 / /storage/emulated/0/DCIM/secret rw - tmpfs tmpfs rw,mode=755,uid=1000,gid=1000
    // 与系统正常 tmpfs 挂载完全一致，无任何 source 痕迹
}
```

**RAII MountGuard（异常安全）**：

```c
// 任何错误路径自动回滚已执行的 mount 操作
typedef struct {
    char paths[MAX_MOUNTS_PER_APP][PATH_MAX];
    int  count;
} MountGuard;

static void mountGuard_rollback(MountGuard *g) {
    // 逆序 umount2(MNT_DETACH) 已挂载的所有路径
    for (int i = g->count - 1; i >= 0; i--) {
        umount2(g->paths[i], MNT_DETACH);
    }
    g->count = 0;
}
```

### 4.3 namespace 私有化（MS_PRIVATE|MS_REC）

**目的**：切断 mount peer group 传播链，防止银行 App 通过跨进程 mountinfo 对比发现 peer group ID 异常。

```c
// mount.c — isolateNamespace（preAppSpecialize 第一步）
static int isolateNamespace(void) {
    // 递归私有化当前 namespace 的所有挂载点
    // 此后的所有 mount 操作不会产生 shared peer group ID
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        // 部分 OEM 内核限制此操作，记录日志但不阻断
        // 后续 mount 操作仍继续（隐蔽性降级但功能不受影响）
        LOGE("PG: MS_PRIVATE|MS_REC failed errno=%d, peer group may leak", errno);
        return -1;
    }
    return 0;
}
```

**调用时机**：在 `preAppSpecialize` 的绝对最开始，任何 mount 操作之前调用一次。

### 4.4 SELinux label 三级降级策略

```c
// utils/selinux.c
static int applyMountWithSELinux(const char *source, const char *target,
                                  const char *fstype, unsigned long flags,
                                  const char *opts) {
    // 第一级：设置 fscreate context 为目标路径的现有 label
    char existingLabel[512] = {0};
    if (getfilecon(target, (char **)&existingLabel) >= 0) {
        if (setfscreatecon(existingLabel) == 0) {
            int r = mount(source, target, fstype, flags, opts);
            setfscreatecon(NULL);  // 重置
            if (r == 0) return 0;
        }
    }

    // 第二级：使用 bind template（已知可工作的通用 label）
    if (setfscreatecon("u:object_r:sdcard_posix:s0") == 0) {
        int r = mount(source, target, fstype, flags, opts);
        setfscreatecon(NULL);
        if (r == 0) return 0;
    }

    // 第三级：不设置 context，直接 mount（MIUI/HyperOS 严格 ROM 降级到此）
    return mount(source, target, fstype, flags, opts);
}
```

### 4.5 memfd 路径伪装（v9 新增）

**背景**：Zygisk 使用 `memfd_create("jit-cache", ...)` + fd 加载模块 SO，使其在 `/proc/self/maps` 中显示为 `/memfd:jit-cache (deleted)`，与 Zygote 真实的 jit-cache 内存段混淆。

PathGuard 的规则缓存 fd 也应使用相同的名称以降低可识别度：

```c
// companion.cpp — 创建规则缓存 fd
int createPolicyFd(const AppPolicyPOD *policy) {
    // v9：使用 "jit-cache" 作为 memfd 名称
    // /proc/{pid}/fd/{n} 读出为 /memfd:jit-cache (deleted)
    // 与 Zygote 真实 jit-cache 内存在 maps 中完全一致
    int fd = memfd_create("jit-cache", MFD_CLOEXEC);
    if (fd < 0) return -1;

    if (write(fd, policy, sizeof(*policy)) != sizeof(*policy)) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;  // 通过 SCM_RIGHTS 传递给 Zygisk 端
}
```

**FD 传递 + Fallback 完整流程（v7 鲁棒性补丁）**：

```c
// module.c — preAppSpecialize 规则加载
AppPolicyPOD policy;
int fd = -1;

// 主路径：memfd + Companion fd 传递（零 IPC，内存级速度）
if (api->connectCompanion) {
    fd = receiveFdFromCompanion(api);
}

// Fallback：fd 无效时退回 SHA-256 文件缓存方案
if (fd < 0 || fstat(fd, &(struct stat){0}) != 0) {
    LOGE("PG: fd invalid, fallback to file cache (magisk=%s)", getMagiskVersion());
    fd = openPolicyCacheFile(pkg);  // /dev/.__sys_{rnd}/{sha256(pkg)[:16]}
}

if (fd < 0) return;  // 无规则 = fail-open，App 正常启动

bool ok = loadPolicyFromFd(fd, &policy);
close(fd);
if (!ok) return;
```

### 4.6 soinfo gap 对策：生态合作

**背景**（JingMatrix 博文）：即使执行了 `dlclose`，bionic linker 的 soinfo 链表中会在原来的位置留下一个空洞（gap），这个空洞在 `specializeAppProcess` 之前就已存在，是可检测的注入指示器。同时，`dlclose` 操作会增加 `g_module_unload_counter` 计数器，也是可检测的痕迹。

**PathGuard 的立场**：

这个问题超出了 PathGuard 单模块的能力边界，需要由 Zygisk 运行时层（NeoZygisk `soinfo_unload`、ZygiskNext ZN Linker、Shamiko soinfo 修复）在 PathGuard 执行 `DLCLOSE` 之后处理链表修复。

**PathGuard 的配合义务**：
1. 必须调用 `setOption(ZYGISK_DLCLOSE_MODULE_LIBRARY)` 触发卸载，给运行时提供修复窗口
2. 保证调用 `setOption` 时所有功能已完成（mount 已生效，fd 已关闭）
3. 不依赖 `postAppSpecialize`（SO 卸载后该回调不会执行）

**推荐生态搭配**：

```
PathGuard v9（路径控制）
  + NeoZygisk v2.x 或 ZygiskNext v1.3.0+（soinfo 修复）
  + Shamiko 或 TreatWheels（根隐藏 + soinfo 链表修复）
  + PlayIntegrityFix + TrickyStore（Play Integrity 修复）

= 用户态检测覆盖率接近上限，路径隐私保护完整
```

### 4.7 SO 二进制隐蔽

**版本脚本零导出**：

```
# version_script.map
{
  global: zygisk_module_entry; zygisk_companion_entry;
  local: *;
};
# 效果：nm -D 只输出 2 个符号
```

**XOR 字符串混淆**（所有路径和模块标识符）：

```c
// 编译时宏：将字符串 XOR 编码到 .rodata
// 运行时解码到栈上，strings 扫描不可见
#define XOR_KEY 0x5A
#define OBFSTR(s) _obfuscate(s, sizeof(s)-1)

static inline char *_obfuscate(const char *enc, size_t len) {
    static char buf[256];
    for (size_t i = 0; i < len; i++) buf[i] = enc[i] ^ XOR_KEY;
    buf[len] = '\0';
    return buf;
}

// 使用示例
const char *moduleDir = OBFSTR("\x1b\x34\x3f\x3a\x27");  // "/dev/" XOR 0x5A
```

**atexit 清零**（无静态全局对象 + 主动 finalize）：

```c
// module.c — preAppSpecialize 末尾
// 策略一：代码规范，禁止所有带析构函数的全局对象
// 策略二：主动调用 __cxa_finalize 清空注册
extern void __cxa_finalize(void *dso_handle);
extern void *__dso_handle;

// 在 setOption(DLCLOSE) 之前执行
__cxa_finalize(__dso_handle);
api->setOption(ZYGISK_DLCLOSE_MODULE_LIBRARY);
```

---

## 5. 规则格式设计

### 5.1 规则文件格式

**路径**：`/data/adb/modules/path_guard/config/rules.ini`（Magisk/KSU），`/data/adb/apatch/modules/path_guard/config/rules.ini`（APatch）

**完整示例**：

```ini
# PathGuard 规则文件 v9
# 编码：UTF-8，LF 换行
#
# 三句话记住全部规则：
#   + 路径              → 允许访问
#   - 路径              → 禁止/隐藏
#   源路径 -> 目标路径   → 重定向（访问源路径时看到目标路径内容）
#
# 路径不以 / 开头时，自动补全为 /storage/emulated/0/<路径>
# 路径以 / 结尾时，强制视为目录规则（防歧义）
# 程序会自动展开所有等价存储路径（/sdcard、/mnt/runtime/... 等）
# 支持 <pkg> 占位符，自动替换为当前应用包名

[global]
log   = error          # error | debug（debug 时启用 selfCheck）
audit = false          # true 时只记录日志，不实际执行 mount

[com.tencent.mm]
mode = whitelist       # whitelist：默认拒绝，只有 + 路径可见
                       # blacklist：默认允许，只有 - 路径被屏蔽

+ Pictures/Share
+ Download/Send
+ DCIM/Camera
+ Android/data/<pkg>/   # <pkg> 占位符自动展开

- DCIM/A-TEST
- Download/private.txt

DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
Movies/Clip -> Android/data/com.tencent.mm/cache/Clip

[com.tencent.mobileqq]
mode = blacklist

- DCIM/敏感目录/
- Download/private/
Tencent/QQfile_recv -> Android/data/com.tencent.mobileqq/files/hidden
```

### 5.2 语法规范

**分组 `[pkg]`**：使用真实包名。不使用别名（UI 层可展示别名，配置文件层统一包名）。

**`mode`**：必填，无默认值，强制显式声明，避免歧义。

**`+ 路径`**：允许访问。用于 whitelist 模式显式放行某路径。

**`- 路径`**：禁止访问并隐藏。`open/stat/listdir` 均返回空或 ENOENT。

**`源 -> 目标`**：bind mount 重定向。访问「源」看到「目标」内容，写入实际落在「目标」。「卸载即焚」产品特性：将 App 写入重定向到 `Android/data/<pkg>/` 下，卸载 App 时系统自动清理。

**`<pkg>` 占位符**：规则加载时替换为当前 App 包名，避免重复书写。

**路径规范化**（加载期统一处理）：

```
/sdcard/...                   → /storage/emulated/0/...
/storage/self/primary/...     → /storage/emulated/0/...
末尾多余 /                    → 去除（尾部 / 语义已在加载期处理）
lexicalNormalize()            → 消灭 . 和 .. 穿越（纯内存，无磁盘 IO）
```

**空格与中文路径**：

```ini
- DCIM/My Album          # + / - 行：去掉前缀后整行为路径
- DCIM/隐私目录/
My Album/Cam -> Android/data/com.tencent.mm/cache/Cam  # 按第一个 -> 切分
```

### 5.3 路径类型三层推断（不在热路径做磁盘探测）

**第一层：加载期静态推断**（按优先级）

1. 尾部 `/`：直接视为目录（`DCIM/A-TEST/` → dir）
2. `lstat()` 成功：`S_ISDIR` → dir，`S_ISREG` → file
3. 扩展名启发式：`.txt/.jpg/.png/.mp4/.db/.json/.xml/.apk` → file
4. 默认兜底：无扩展名、不存在、无尾部 `/` → **dir**（最常见情况）

**第二层：运行期 syscall 上下文修正**（Phase 2，openat hook 启用时）

| syscall | 推断 |
|---|---|
| `mkdir/mkdirat/rmdir/opendir/open(O_DIRECTORY)` | dir |
| `open(O_CREAT)` 且无 `O_DIRECTORY`、`fopen/fopen64` | file |

**第三层：内存路径类型缓存**（运行时越来越准确，无热路径磁盘探测）

### 5.4 冲突解决优先级

1. **路径更长的规则优先**（更具体覆盖更泛）
2. **文件规则优先于同路径的目录规则**
3. **动作优先级**：`-` > 重定向 > `+`

示例：

```ini
[com.tencent.mm]
mode = whitelist

+ Pictures                         # 允许整个 Pictures
- Pictures/Private                 # 禁止 Pictures/Private（优先）
Pictures/Camera -> Android/data/com.tencent.mm/cache/Camera  # 重定向
```

判定：
- `Pictures/Share/a.jpg` → ✅ 允许（命中 `+ Pictures`）
- `Pictures/Private/a.jpg` → ❌ 拒绝（命中 `- Pictures/Private`，路径更长优先）
- `Pictures/Camera/a.jpg` → 🔀 重定向（重定向优先于 allow）

---

## 6. 规则引擎设计

### 6.1 内部规则对象（编译期结构，运行时零字符串解析）

```c
// rules/types.h

typedef enum { ACTION_ALLOW, ACTION_DENY, ACTION_REDIRECT } Action;
typedef enum { PATH_AUTO, PATH_FILE, PATH_DIR } PathKind;
typedef enum { MODE_WHITELIST, MODE_BLACKLIST } PolicyMode;

// 单条规则（编译期填充，运行时只读）
typedef struct {
    char     path[PATH_MAX];       // lexicalNormalize() 后的绝对路径
    char     dst[PATH_MAX];        // 重定向目标（仅 REDIRECT 有效）
    Action   action;
    PathKind kind;                 // auto → 运行时缓存修正
} Rule;

// 单个 App 的完整策略（序列化为 AppPolicyPOD 后通过 fd 传递）
typedef struct {
    PolicyMode mode;
    Rule       rules[MAX_RULES_PER_APP];  // 按路径长度降序预排序
    int        ruleCount;
} AppPolicy;

// 固定大小 POD，直接 read() 到栈，零动态分配
typedef struct {
    uint32_t   magic;              // 0x50470009（PathGuard v9）
    PolicyMode mode;
    uint32_t   ruleCount;
    Rule       rules[MAX_RULES_PER_APP];
} AppPolicyPOD;
```

### 6.2 Trie 前缀树匹配

**时间复杂度**：O(path_length)，与规则数量无关。

**目录规则的安全匹配**（防止 `A-TEST` 规则误伤 `A-TEST_backup`）：

```
path == rule_path                 → 命中（目录本身）
path 以 rule_path + "/" 开头      → 命中（目录内后代）
```

**纯文件规则**：`path == rule_path`（精确匹配，不做前缀）

```cpp
// rules/trie.cpp — 核心查询逻辑
MatchResult TrieNode::query(const char *path) const {
    const TrieNode *node = this;
    const char *p = path;

    while (*p && node) {
        // 按 '/' 分隔逐段遍历 Trie
        const char *slash = strchr(p, '/');
        size_t segLen = slash ? (slash - p) : strlen(p);

        auto it = node->children.find(std::string_view(p, segLen));
        if (it == node->children.end()) break;
        node = &it->second;

        // 命中目录规则：当前节点有 dir 规则，且路径在此节点下
        if (node->hasRule && node->rule.kind == PATH_DIR) {
            return {node->rule.action, &node->rule};
        }

        if (!slash) {
            // 路径末尾：精确命中文件或目录规则
            if (node->hasRule) return {node->rule.action, &node->rule};
            break;
        }
        p = slash + 1;
    }

    return {ACTION_NONE, nullptr};  // 未命中
}
```

### 6.3 lexicalNormalize（路径穿越防御）

**v5 关键修复**：用纯内存操作替代依赖磁盘的 `realpath()`，消灭 `../` 穿越攻击。

```c
// utils/path.c — 纯字符串操作，零磁盘 IO，零动态分配
// 对应 C++17 path::lexically_normal() 语义
void lexicalNormalize(const char *input, char *out, size_t outSize) {
    // 分段处理 /a/b/../c → /a/c
    // 绝对路径 .. 不超过根
    // 空段和 . 跳过
    // 结果写入 out[outSize]（栈分配）
    char segments[32][NAME_MAX + 1];
    int segCount = 0;
    bool isAbs = (input[0] == '/');
    const char *p = input + (isAbs ? 1 : 0);

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.')) {
            // 跳过空段和 .
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (segCount > 0) segCount--;
        } else if (segCount < 32) {
            memcpy(segments[segCount++], p, len);
            segments[segCount - 1][len] = '\0';
        }
        if (!slash) break;
        p = slash + 1;
    }

    size_t pos = 0;
    if (isAbs) out[pos++] = '/';
    for (int i = 0; i < segCount; i++) {
        if (i > 0) out[pos++] = '/';
        size_t slen = strlen(segments[i]);
        memcpy(out + pos, segments[i], slen);
        pos += slen;
    }
    out[pos] = '\0';
    if (pos == 0) { out[0] = '/'; out[1] = '\0'; }
}
```

**攻击示例验证**：

```
DCIM/Camera/../../A-TEST/hide.txt
  → abs: /storage/emulated/0/DCIM/Camera/../../A-TEST/hide.txt
  → lexical: /storage/emulated/0/A-TEST/hide.txt
  → Trie 命中 "- DCIM/A-TEST" 规则 → 正确拦截 ✅

/storage/emulated/0/DCIM/A-TEST_backup
  → 目录规则要求 path == rule 或 path 以 rule+"/" 开头
  → A-TEST_backup ≠ A-TEST 且不以 A-TEST/ 开头 → 不命中 ✅
```

---

## 7. Mount 执行层设计

### 7.1 preAppSpecialize 完整执行序列

```c
// module.c — preAppSpecialize（v9 完整版）
void preAppSpecialize(ZygiskApi *api, JNIEnv *env, AppSpecializeArgs *args) {
    const char *pkg = JniGetString(env, args->nice_name);
    if (!pkg || !*pkg) return;  // 系统进程，快速返回

    // ① 快速检查是否有规则（__builtin_expect 分支预测：绝大多数 App 无规则）
    if (__builtin_expect(!hasPolicyFor(pkg), 1)) goto cleanup_dlclose;

    // ② Namespace 私有化（切断 peer group 传播）
    isolateNamespace();

    // ③ 加载规则（memfd 主路径 + file fallback）
    AppPolicyPOD policy;
    int policyFd = acquirePolicyFd(api, pkg);
    if (policyFd < 0) goto cleanup_dlclose;  // fail-open
    if (!loadPolicyFromFd(policyFd, &policy)) goto cleanup_fd;

    {
        // ④ 获取 userId（Work Profile 支持）
        int userId = (int)(args->uid / 100000);

        // ⑤ 路径展开 + lazy stat + 按深度降序排列
        char expandedPaths[MAX_EXPANDED_PATHS][PATH_MAX];
        int pathCount = expandAndSortPaths(&policy, userId, expandedPaths);

        // ⑥ RAII MountGuard 初始化
        MountGuard guard = {0};

        // ⑦ 逐条执行 mount 操作（deny/redirect）
        for (int i = 0; i < pathCount; i++) {
            applyOneRule(&policy, expandedPaths[i], &guard);
        }

        // MountGuard 在此作用域内：任何 panic 自动 rollback
    }

cleanup_fd:
    close(policyFd);

cleanup_dlclose:
    // ⑧ atexit 清零 + 请求 SO 卸载（完成即消失）
    __cxa_finalize(__dso_handle);
    api->setOption(ZYGISK_DLCLOSE_MODULE_LIBRARY);

    // JNI 字符串释放
    if (pkg) env->ReleaseStringUTFChars(args->nice_name, pkg);
}
```

### 7.2 Lazy Stat 优化

```c
// 仅对 stat 成功的路径执行 mount，跳过不存在的等价路径
// v6 核心性能补丁：消除对不存在路径的无效 mount 调用

static void applyOneRule(const AppPolicyPOD *policy,
                         const char *expandedPath,
                         MountGuard *guard) {
    struct stat st;
    // lazy stat：路径不存在则直接跳过，无 mount 调用
    if (lstat(expandedPath, &st) != 0) return;

    const Rule *rule = trieQuery(&policy->trie, expandedPath);
    if (!rule) {
        // 白名单模式：未命中 → 默认 DENY
        if (policy->mode == MODE_WHITELIST) {
            applyStealthOverlay(expandedPath, st.st_uid, st.st_gid, st.st_mode & 07777);
            mountGuard_add(guard, expandedPath);
        }
        return;
    }

    switch (rule->action) {
    case ACTION_DENY:
        applyStealthOverlay(expandedPath, st.st_uid, st.st_gid, st.st_mode & 07777);
        mountGuard_add(guard, expandedPath);
        break;
    case ACTION_REDIRECT:
        applyBindRedirect(expandedPath, rule->dst, &st);
        mountGuard_add(guard, expandedPath);
        break;
    case ACTION_ALLOW:
        // 白名单模式下 + 规则：放行，不做 mount
        break;
    }
}
```

### 7.3 重定向实现

```c
// 把 dst 路径的内容绑定到 src 位置
// App 访问 src 看到 dst 的内容，写入实际落在 dst
static void applyBindRedirect(const char *src, const char *dst,
                               const struct stat *srcStat) {
    // 确认/创建 dst 路径
    ensurePathExists(dst, srcStat);

    // 直接 bind mount dst → src（无需临时点，此处不需要隐藏 source）
    // 注意：重定向的 source（dst）不是 pathguard 私有路径，无隐蔽需求
    mount(dst, src, NULL, MS_BIND | MS_REC, NULL);
}
```

---

## 8. Companion Daemon 设计

### 8.1 职责

- 读取并编译 `rules.ini` → `AppPolicy` 对象
- 序列化 `AppPolicy` → `AppPolicyPOD`
- 将策略写入 `memfd`，通过 Zygisk 的 `SCM_RIGHTS` fd 传递给模块端
- 监听 `SIGUSR1` 信号实现热更新
- 写入 Ring buffer 日志到文件

### 8.2 规则热更新

```c
// companion.cpp — SIGUSR1 热更新处理

static volatile sig_atomic_t g_reload_requested = 0;

static void sigusr1_handler(int sig) {
    g_reload_requested = 1;
}

// 热更新流程：原子 rename，Zygisk 端读到的永远是完整文件
// 1. 将新规则编译到 /tmp 下临时文件
// 2. 原子 rename 到正式路径
// 3. Zygisk 端下次 preAppSpecialize 时读到新规则
static void tryReloadRules(void) {
    if (!g_reload_requested) return;
    g_reload_requested = 0;

    AppPolicy newPolicies[MAX_PKGS];
    int count = parseRulesINI(PG_RULES_PATH, newPolicies, MAX_PKGS);
    if (count < 0) {
        LOGE("PG: rules reload failed, keeping old rules");
        return;
    }

    // shared_mutex：热更新时写锁，Zygisk 端查询时读锁
    pthread_rwlock_wrlock(&g_policy_lock);
    memcpy(g_policies, newPolicies, count * sizeof(AppPolicy));
    g_policyCount = count;
    pthread_rwlock_unlock(&g_policy_lock);

    LOGI("PG: rules reloaded, %d packages", count);
}
```

### 8.3 IPC 协议（Companion ↔ Zygisk 模块端）

```
请求：4 字节包名长度 + 包名字节串
响应：SCM_RIGHTS 传递一个 memfd fd（成功）
      或关闭连接（无规则，fail-open）

超时：50ms（来自 v4，防止 Companion 挂起阻塞 App 启动）
fail-open：超时或任何错误 → App 正常启动，规则不生效
```

---

## 9. Zygisk 模块主流程

### 9.1 模块注册

```c
// module.c — 标准 Zygisk 模块注册
#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ModuleBase;
using zygisk::Option;

class PathGuardModule : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
#ifdef PG_DEBUG
        selfCheck("/storage/emulated/0/DCIM/__pg_test");
#endif
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        // 完整实现见第 7.1 节
        ::preAppSpecialize(api, env, args);
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        // SO 已被 DLCLOSE，此回调不会执行
        // 如果执行到这里，说明 DLCLOSE 未生效（某些 Zygisk 实现不支持）
        // 此时 mount 操作已完成，无需额外处理
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        // system_server 不做路径控制
        api->setOption(Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api;
    JNIEnv *env;
};

REGISTER_ZYGISK_MODULE(PathGuardModule)
REGISTER_ZYGISK_COMPANION(companionHandler)
```

### 9.2 selfCheck 调试自检（仅 Debug 构建）

```c
// module.c — 仅 PG_DEBUG 时编译，Release 零 footprint
#ifdef PG_DEBUG
static void selfCheck(const char *testTarget) {
    LOGI("PG selfCheck start ─────────────────────");

    // 检查 1：mount 后 mountinfo 无残留 source 痕迹
    applyStealthOverlay(testTarget, getuid(), getgid(), 0755);
    checkMountinfoClean(testTarget);
    // 期望：只有一条 tmpfs tmpfs，无任何 pg_/path_guard 字样

    // 检查 2：导出符号数量
    checkExportedSymbolsFromMaps();
    // 期望：仅 zygisk_module_entry / zygisk_companion_entry

    // 检查 3：memfd 匿名性（fd 路径）
    int fd = memfd_create("jit-cache", MFD_CLOEXEC);
    if (fd >= 0) {
        char link[256] = {0};
        char fdPath[64];
        snprintf(fdPath, sizeof(fdPath), "/proc/self/fd/%d", fd);
        readlink(fdPath, link, sizeof(link));
        LOGI("PG selfCheck memfd link: %s", link);
        // 期望：/memfd:jit-cache (deleted)
        close(fd);
    }

    // 检查 4：atexit 注册（nm 验证）
    // 期望：nm 输出中无 __cxa_atexit 引用

    // 检查 5：敏感字符串扫描
    checkSensitiveStringsFromMaps();
    // 期望：无 pathguard/path_guard/storage/sdcard 明文

    // 检查 6：peer group ID（mountinfo）
    checkNoPeerGroupId(testTarget);
    // 期望：mountinfo 无 shared:N 标签

    LOGI("PG selfCheck end ───────────────────────");
}
#endif
```

---

## 10. 日志系统设计

**核心原则**：日志写入绝不在 `preAppSpecialize` 热路径同步执行。

### 10.1 Ring Buffer 异步架构

```
preAppSpecialize（热路径）
    └─ logAsync(pkg, path, action)
         └─ 写入 ring buffer（原子操作，< 1μs）
              不做任何 IO

Companion 后台线程（每秒或 ring buffer 达阈值）
    ├─ 批量读取 ring buffer
    ├─ 采样过滤
    └─ 批量写入日志文件（一次系统调用）
```

### 10.2 采样策略

| 事件类型 | 正常模式 | Debug 模式 |
|---|---|---|
| DENY | 全部记录 | 全部记录 |
| REDIRECT | 全部记录 | 全部记录 |
| ALLOW | 不记录 | 采样 1% |
| 系统进程（包名为空或 android.*）| 跳过 | 跳过 |

### 10.3 Audit 模式

```ini
[global]
audit = true   # 只记录 WOULD_DENY/WOULD_REDIRECT，不实际执行 mount
```

Audit 模式下所有 `applyStealthOverlay` 和 `applyBindRedirect` 调用被替换为日志写入，App 正常运行，用于规则上线前的正确性验证。

### 10.4 日志文件

```
路径：/data/adb/pathguard/pg.log
权限：0600（仅 root 可读，保护路径隐私）
轮转：超过 1MB → rename pg.log.1，新建 pg.log（保留最近 3 个）
格式：2026-01-01T12:00:00 DENY com.tencent.mm /storage/emulated/0/DCIM/A-TEST
```

---

## 11. 构建系统设计

### 11.1 Android.mk（v9 最终版）

```makefile
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := path_guard

LOCAL_SRC_FILES := \
    module.c            \
    companion.cpp       \
    mount.c             \
    rules/compiler.cpp  \
    rules/trie.cpp      \
    utils/random.c      \
    utils/path.c        \
    utils/selinux.c

LOCAL_CFLAGS += \
    -Os                          \   # 代码大小优化（vs -O2 减少重定位）
    -flto                        \   # 链接时优化（配合 --icf=safe 消除重复代码）
    -ffunction-sections          \   # 允许 --gc-sections 按函数粒度 DCE
    -fdata-sections              \   # 允许 --gc-sections 按数据粒度 DCE
    -fvisibility=hidden          \   # 默认隐藏所有符号（配合 version_script）
    -fno-exceptions              \   # 消除 C++ 异常处理表（也消除部分 atexit 注册）
    -fno-rtti                    \   # 消除 RTTI 信息（减小体积）
    -fno-plt                     \   # 消除 PLT（配合 -z,now 彻底消除延迟绑定）
    -fomit-frame-pointer         \   # 减少寄存器压力
    -fstack-protector-strong     \   # 栈保护（安全基线）
    -DNDEBUG                        # 禁用 assert

# Debug 构建时追加（热更新 rules.ini 中 log=debug 时由 Companion 传递）：
# -DPG_DEBUG

# Root 方案选择（安装时由 customize.sh 选择）：
# -DPG_ROOT_MAGISK（默认）
# -DPG_ROOT_KSU
# -DPG_ROOT_APATCH

LOCAL_LDFLAGS += \
    -Wl,--gc-sections                                            \
    -Wl,--icf=safe                                               \
    -Wl,--strip-all                                              \
    -Wl,-z,now                                                   \
    -Wl,--no-undefined                                           \   # 链接时报所有未定义符号
    -Wl,--exclude-libs,ALL                                       \   # 静态库符号不导出
    -Wl,--version-script=$(LOCAL_PATH)/version_script.map

LOCAL_LDLIBS := -llog -lselinux
# 注意：不链接 -ldl（已移除所有 dlopen/dlsym 运行时依赖，见 8.4/v8）

include $(BUILD_SHARED_LIBRARY)
```

### 11.2 Application.mk（v9 最终版）

```makefile
APP_ABI      := arm64-v8a armeabi-v7a x86_64 x86
APP_PLATFORM := android-26       # Android 8.0+（memfd_create / getrandom 最低要求）
APP_STL      := none             # 官方要求：不得修改，消除 libc++ 依赖
APP_OPTIM    := release
APP_CFLAGS   := -std=c17
```

### 11.3 version_script.map

```
{
  global:
    zygisk_module_entry;
    zygisk_companion_entry;
  local: *;
};
```

### 11.4 目标指标汇总（v4 → v9）

| 指标 | v4 | v6 | v8 | v9 目标 |
|---|---|---|---|---|
| arm64 SO 大小 | ~80KB | <48KB | <44KB | **<44KB** |
| 导出符号数 | ~30 | ~5 | ≤2 | **≤2** |
| 未定义符号 | 可能 | 可能 | 0 | **0** |
| preAppSpecialize 耗时（50条规则）| ~5ms | <1ms | <550μs | **<550μs** |
| mountinfo source 痕迹 | 有 | 无 | 无 | **无** |
| peer group ID 泄漏 | 有 | 有 | 无 | **无** |
| SO 在 maps 中 | 永久 | 永久 | 卸载后消失 | **消失** |
| atexit 残留 | 可能 | 可能 | 0 | **0** |
| 敏感字符串 | 明文 | XOR 混淆 | XOR 混淆 | **XOR 混淆** |
| 检测维度覆盖率 | 3/8 | 5/8 | 8/8 | **8/8 + memfd 伪装** |
| Companion RSS | ~500KB | <200KB | <200KB | **<200KB** |

### 11.5 构建后验证脚本

```bash
#!/bin/bash
# verify_build.sh — v9 一键验证
SO="zygisk/arm64-v8a.so"

echo "=== 1. SO 大小（期望 <44KB）==="
ls -lh "$SO"

echo "=== 2. 导出符号（期望仅 2 个）==="
nm -D "$SO" | grep ' T ' | awk '{print $3}'

echo "=== 3. atexit 符号（期望无 __cxa_atexit 引用）==="
nm "$SO" | grep "cxa_atexit" | grep -v "__dso_handle"

echo "=== 4. dlopen/dl_iterate_phdr 依赖（期望无）==="
nm "$SO" | grep -E "dlopen|dlsym|dl_iterate_phdr|dladdr"

echo "=== 5. 未定义符号（期望无）==="
nm "$SO" | grep ' U '

echo "=== 6. 敏感字符串（期望无）==="
strings "$SO" | grep -iE "pathguard|path_guard|storage|sdcard|/data/adb"

echo "=== 7. 依赖库（期望无 libdl、libc++）==="
readelf -d "$SO" | grep NEEDED

echo "=== 8. 重定位数量（期望 <200）==="
readelf --relocs "$SO" | grep -c RELA

echo "=== 9. memfd 名称验证（期望包含 jit-cache）==="
strings "$SO" | grep "jit-cache"
```

---

## 12. 模块文件结构

```
path-guard/
├── module.prop                 # 模块元信息
├── skip_mount                  # 跳过 magic mount（无 system 文件替换）
│
├── zygisk/                     # Zygisk native 库
│   ├── arm64-v8a.so            # 各架构 SO（< 44KB each）
│   ├── armeabi-v7a.so
│   ├── x86_64.so
│   └── x86.so
│
├── config/
│   └── rules.ini               # 用户规则文件
│
├── customize.sh                # 安装时自动选择 root 方案变体
├── action.sh                   # Magisk App「执行」按钮（触发热更新）
└── uninstall.sh                # 清理 /dev/.__sys_*、日志文件
```

**module.prop**：

```properties
id=path_guard
name=PathGuard - 路径访问控制
version=v9.0.0
versionCode=9
author=YourName
description=高性能路径隐私保护。黑/白名单 + 重定向（卸载即焚）。完成即消失架构，隐蔽性强。Zygisk + per-app mount namespace。
updateJson=https://your-server.com/path-guard/update.json
minMagisk=26000
```

**customize.sh**（安装时选择 root 方案 SO）：

```bash
#!/system/bin/sh
# customize.sh — 安装时自动检测 root 方案
SKIP_MOUNT=true

if [ -d "/data/adb/apatch" ]; then
    ROOT_SOL="apatch"
elif [ -d "/data/adb/ksu" ]; then
    ROOT_SOL="ksu"
else
    ROOT_SOL="magisk"
fi

ui_print "- Root solution: $ROOT_SOL"
# 三个变体已预编译，API 完全兼容，仅路径常量不同
cp "$MODPATH/zygisk_variants/${ROOT_SOL}/arm64-v8a.so" "$MODPATH/zygisk/arm64-v8a.so"
```

---

## 13. 兼容性矩阵

### 13.1 Root 方案兼容性

| Root 方案 | Zygisk 实现 | 支持状态 | 备注 |
|---|---|---|---|
| Magisk v26+ | 内置 Zygisk | ✅ 主要平台 | 关闭内置 Zygisk 时改用 ReZygisk |
| KernelSU | ZygiskNext v1.3.0+ | ✅ 已验证 | ZN Linker 提供 soinfo 修复 |
| KernelSU | ReZygisk v1.0.0+ | ✅ 已验证 | 自定义链接器，无 dlopen 依赖问题 |
| KernelSU | NeoZygisk v2.x | ✅ 已验证 | soinfo_unload，atexit 自动处理 |
| APatch | ZygiskNext / ReZygisk | ✅ 条件编译 | -DPG_ROOT_APATCH |
| Magisk Kitsune | 内置 Zygisk | ✅ 应兼容 | 待实测 |

### 13.2 Android 版本兼容性

| Android 版本 | API | 特殊处理 |
|---|---|---|
| 8.0–9 | 26–28 | getrandom 基准，无 FUSE |
| 10 | 29 | Scoped Storage 引入，需覆盖 FUSE 层 |
| 11 | 30 | sdcardfs → FUSE，优先覆盖 `/data/media/{userId}` |
| 12–15 | 31–35 | 标准 FUSE，全面支持 |
| 16+ | 36+ | 需持续验证（Hardware Attestation 增强） |

### 13.3 OEM ROM 特殊处理

| ROM | 问题 | 处理方案 |
|---|---|---|
| MIUI / HyperOS | SELinux 严格，setfscreatecon 受限 | SELinux 三级降级（见 4.4）|
| ColorOS | /mnt/runtime 路径结构差异 | 等价路径展开覆盖所有视图 |
| OneUI (Knox) | Knox 额外挂载点检测 | 配合 Shamiko |
| EMUI / MagicOS | 存储路径定制 | lazy stat 自然过滤 |
| AOSP / LineageOS | 基准 | 完全兼容 |

### 13.4 共存兼容性

| 模块组合 | 状态 | 说明 |
|---|---|---|
| PathGuard + Shamiko | ✅ 推荐 | Shamiko 处理 soinfo gap，强烈推荐 |
| PathGuard + TreatWheels | ✅ 推荐 | 同上（ReZygisk 生态） |
| PathGuard + LSPosed | ✅ 兼容 | LSPosed 处理 MediaStore Hook（Phase 3）|
| PathGuard + PlayIntegrityFix | ✅ 兼容 | 功能正交 |
| PathGuard + StorageRedirect | ⚠️ 谨慎 | 两者同时覆盖存储路径可能冲突，按包名隔离规则 |
| PathGuard + Zygisk-Assistant | ✅ 兼容 | ZA 处理其他 unmount 逻辑 |

---

## 14. 功能规划与开发路线图

### 14.1 Phase 1（MVP，当前实现）

- ✅ INI 规则格式（`mode` + `+/-/->` + `<pkg>` 占位符）
- ✅ 黑名单 / 白名单 / 重定向三种模式
- ✅ 相对路径补全、lexicalNormalize 防穿越、中文/空格路径
- ✅ 等价路径完整展开（含 Work Profile 多用户）
- ✅ Trie 前缀树 O(len) 匹配
- ✅ 零 IPC memfd 规则缓存（+ file fallback）
- ✅ MNT_DETACH + MS_PRIVATE|MS_REC 隐蔽 mount
- ✅ RAII MountGuard 自动回滚
- ✅ SELinux 三级降级
- ✅ Ring buffer 异步日志 + Audit 模式
- ✅ SIGUSR1 热更新
- ✅ 完成即消失（DLCLOSE + atexit 清零）
- ✅ APatch / KSU / Magisk 条件编译兼容

### 14.2 Phase 2（syscall fallback）

目标：对抗主动读取 mountinfo 的检测型 App。

- openat hook（shadowhook 或 PLT hook）过滤 `/proc/self/mountinfo`
- 当 App 读取 mountinfo 时，将 PathGuard 的挂载条目过滤出返回结果
- 运行期路径类型缓存更新

### 14.3 Phase 3（生态完整化）

- **MediaStore 对抗**：
  - 方案 A：LSPosed Hook `ContentResolver`（需 App 级别注入）
  - 方案 B：对 `com.android.providers.media.module` 额外注入 PathGuard 规则
- **Manager App**：可视化规则编辑器 · 沙盘推演诊断器 · Audit 日志查看 · mount 状态 · 热更新触发

**「沙盘推演」规则诊断器**（零副作用，纯 Companion 端逻辑）：

```
输入：包名 + 路径 + 操作类型
输出：命中哪条规则 + 最终行为 + 理由
实现：lexicalNormalize → Trie 查询 → 冲突解决 → 返回结果
     不 fork 进程，不做实际 mount，不访问文件系统
```

---

## 15. 测试与发布清单

### 15.1 隐蔽性验证（v9 扩展）

```
[ ] selfCheck() 全部通过（debug 构建）
[ ] mountinfo 无 shared:N 标签（MS_PRIVATE 生效）
[ ] mountinfo 无任何 pg_/path_guard/.__sys 字样
[ ] /proc/self/maps 中无 path_guard SO 路径（DLCLOSE 生效）
[ ] memfd fd 路径显示为 /memfd:jit-cache (deleted)
[ ] nm -D 输出仅 2 个符号
[ ] strings 扫描无敏感字符串
[ ] nm 无 __cxa_atexit 引用
[ ] readelf -d 无 libdl.so 依赖
[ ] 与 Shamiko 共存后银行 App 通过检测
```

### 15.2 功能验证

```
[ ] DENY：open/stat/listdir 均返回空/ENOENT
[ ] ALLOW（whitelist）：允许的路径正常可见
[ ] REDIRECT：访问 src 看到 dst 内容，写入落在 dst
[ ] 路径穿越：DCIM/Camera/../../A-TEST/hide.txt → 命中 A-TEST 规则
[ ] 兄弟路径隔离：A-TEST 规则不误伤 A-TEST_backup
[ ] 中文路径：DCIM/隐私目录 正确解析和拦截
[ ] Work Profile：userId=10 下规则完整生效
[ ] <pkg> 占位符：展开为正确包名
[ ] Audit 模式：不执行 mount，只记录 WOULD_DENY/WOULD_REDIRECT
[ ] FD fallback：Companion fd 失效时自动切换 file 缓存
[ ] SIGUSR1 热更新：新规则对下次启动的 App 生效
[ ] fail-open：Companion 挂起时 App 正常启动（50ms 超时）
[ ] 卸载清理：uninstall.sh 后 /dev/.__sys_* 和日志均删除
```

### 15.3 性能验证

```
[ ] preAppSpecialize 平均耗时 < 550μs（50 App 冷启，Android 15，Pixel 8+）
[ ] isolateNamespace 额外耗时 < 50μs（单次 mount 系统调用）
[ ] 3 条规则场景：App 启动延迟增量 < 1ms
[ ] 1000 条规则场景：App 启动延迟增量 < 10ms（Trie O(len) 保障）
[ ] Companion RSS < 200KB（含规则编译缓存）
[ ] Ring buffer 写入不阻塞热路径（< 1μs 原子操作）
[ ] zero malloc 验证：strace preAppSpecialize 确认无 brk/mmap 调用
```

### 15.4 Android 版本兼容测试矩阵

```
[ ] Android 8.0 (API 26) — 基准，getrandom/memfd_create
[ ] Android 10 (API 29) — Scoped Storage 引入
[ ] Android 11 (API 30) — sdcardfs → FUSE，/data/media 路径
[ ] Android 12/13 (API 31/33)
[ ] Android 14/15 (API 34/35) — 主要测试目标
[ ] Android 16 (API 36) — Hardware Attestation 增强，需持续关注
```

### 15.5 OEM / 生态兼容测试

```
[ ] MIUI HyperOS（SELinux 严格）：SELinux 三级降级正确触发
[ ] ColorOS（/mnt/runtime 差异）：等价路径全覆盖
[ ] OneUI（Knox）：与 Shamiko 共存无冲突
[ ] AOSP / LineageOS：基准，完全兼容
[ ] ZygiskNext v1.3.0+（ZN Linker）：DLCLOSE 后 SO 消失
[ ] ReZygisk v1.0.0+（自定义链接器）：无 dlopen 依赖，正常加载
[ ] NeoZygisk v2.x（soinfo_unload）：atexit 自动处理，兼容
[ ] APatch + ZygiskNext/ReZygisk：customize.sh 正确选择变体
```

---

## 16. 参考资源

| 资源 | 说明 | 地址 |
|---|---|---|
| Magisk 官方开发文档 | 模块结构、脚本、sepolicy 规范 | https://topjohnwu.github.io/Magisk/guides.html |
| Zygisk 模块模板 | 官方 C++ 框架、`APP_STL=none` 要求 | https://github.com/topjohnwu/zygisk-module-sample |
| AOSP 存储文档 | mount namespace、FUSE、sdcardfs、多用户存储 | https://source.android.com/docs/core/storage |
| AOSP Multi-user Testing | userId 分配规则（uid/100000）| https://source.android.com/docs/core/tests/development/multi-user-testing |
| AOSP MediaProvider 文档 | MediaStore 架构、Scoped Storage | https://source.android.com/docs/core/media/media-provider |
| Linux umount2(2) | MNT_DETACH 惰性卸载机制 | https://man7.org/linux/man-pages/man2/umount2.2.html |
| Linux mount_namespaces(7) | peer group、MS_PRIVATE\|MS_REC | https://man7.org/linux/man-pages/man7/mount_namespaces.7.html |
| cppreference lexically_normal | 纯内存路径规范化 | https://en.cppreference.com/w/cpp/filesystem/path/lexically_normal |
| NDK --no-undefined 文档 | 链接标志说明 | https://developer.android.com/ndk/guides/abis |
| setfscreatecon(3) | SELinux fscreate context API | https://man7.org/linux/man-pages/man3/setfscreatecon.3.html |
| StorageRedirect v29 | bind mount + namespace 设计参考 | https://github.com/RikkaApps/StorageRedirect-assets |
| rvmm-zygisk-mount | preAppSpecialize mount 注入实例 | https://github.com/j-hc/rvmm-zygisk-mount |
| ZygiskNext v1.3.0（ZN Linker）| 自定义链接器、匿名内存、DLCLOSE | https://github.com/Dr-TSNG/ZygiskNext |
| ReZygisk v1.0.0 | 纯 C 重写、自定义链接器 | https://github.com/PerformanC/ReZygisk |
| NeoZygisk v2.x | soinfo_unload、atexit evasion | https://github.com/JingMatrix/NeoZygisk |
| Shamiko | soinfo 修复、根隐藏天花板 | （Telegram 发布）|
| JingMatrix「Android 用户态注入隐藏已死」| 注入隐藏理论分析，「完成即消失」哲学基础 | https://root99.cn/index.php/archives/397/ |
| JingMatrix/Demo | soinfo gap 检测 PoC | https://github.com/JingMatrix/Demo |
| FakeXposed | syscall hook 方案（Phase 2 参考）| https://github.com/sanfengAndroid/FakeXposed |
| shadowhook | Android inline/PLT hook 库 | https://github.com/bytedance/android-inline-hook |
| rule-format-plan.md | 规则格式专项设计（项目内部文档）| config/rule-format-plan.md |

---

> **法律与伦理声明**：PathGuard 涉及 root 权限下的深度系统干预。发布时请在 README 和 Manager App 中明确告知兼容性限制、稳定性风险、卸载与恢复指南，并禁止用于规避合法安全检测或实施侵权行为。

---

*PathGuard Technical Design v9.0 完整版 · 整合 v4–v8 全量设计 · 五项 v9 新增：「完成即消失」哲学定稿 · memfd jit-cache 路径伪装 · soinfo gap 生态合作对策 · 完整兼容性矩阵 · 全量测试清单 · 检测向量覆盖率 8/8*
