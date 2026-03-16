# PathGuard v11 — 完整技术设计方案

> **平台**：Android 12+，GKI kernel 5.10.220 / 5.15+  
> **核心目标**：高性能 · 占用小 · 速度快 · 隐蔽性高 · 稳定  
> **参考来源**：Zygisk 官方 API · NeoZygisk v2.x Releases（2025）· ZygiskNext · Linux fanotify(7)/fanotify_init(2) 官方手册 · torvalds/linux fanotify.h · Linux mount_namespaces(7) · umount2(2) · posix_fadvise(2) · ioprio_set(2) · AOSP Multi-user 文档 · v4–v10 全量继承

---

## 总体架构速览

```
PathGuard v11 = 两个物理独立的子系统

┌──────────────────────────────────────────────────────────┐
│  Part 1：路径访问控制引擎（Zygisk SO，注入 App 进程）        │
│  触发：preAppSpecialize（App 代码运行前 μs 级窗口）          │
│  机制：per-app mount namespace + tmpfs overlay           │
│  结束：DLCLOSE，SO 从内存彻底消失                           │
│  解决：禁止/隐藏 App 对指定路径的访问                        │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│  Part 2：双引擎重定向                                      │
│                                                          │
│  静态引擎（->）：bind mount，App 启动时一次性完成            │
│    触发：preAppSpecialize（与 Part 1 共用基础设施）          │
│    机制：两路径共享同一 inode，双向透明，零 I/O              │
│    用途：卸载即焚，隔离 App 写入                            │
│                                                          │
│  动态引擎（=>）：fanotify + root 守护进程 + 异步搬运         │
│    触发：FAN_CLOSE_WRITE（文件写入完成事件）                  │
│    机制：rename（同分区）/ sendfile（跨分区）                │
│    用途：强制提取沙盒文件，防 App 后期删除                   │
└──────────────────────────────────────────────────────────┘
```

---

# PathGuard v11 — Part 1：路径访问控制引擎

> **平台**：Android 12+，GKI kernel 5.10.220 / 5.15+  
> **目标**：高性能 · 占用小 · 速度快 · 隐蔽性高 · 稳定  
> **参考**：Zygisk 官方 API · NeoZygisk v2.x Releases（2025）· ZygiskNext · Linux mount_namespaces(7) · umount2(2) · AOSP Multi-user 文档 · v4–v10 全量继承

---

## 1. 设计定位

路径访问控制引擎解决的问题：**禁止或隐藏 App 对指定存储路径的访问。**

核心价值：
- **黑名单**：默认放行，指定路径不可见（DENY）
- **白名单**：默认拒绝，只有授权路径可见（ALLOW）

本引擎以「完成即消失」为最高设计原则——在 App 任何代码运行之前完成全部操作，然后从内存彻底消失，App 运行期间无任何可检测的注入痕迹。

---

## 2. 核心时序：preAppSpecialize 窗口

```
Zygote fork App
    │
    ├─ per-app mount namespace 创建完成
    │   (此 namespace 独立，App 私有，对全局无影响)
    │
    ├─ [Zygisk] preAppSpecialize()  ◄── PathGuard 操作窗口（μs 级）
    │    │  此时：mount namespace 独立
    │    │  此时：UID 尚未 drop，仍持有 CAP_SYS_ADMIN
    │    │  此时：App 代码未执行，无法检测任何操作
    │    │
    │    ├─ ① isolateNamespace()        MS_PRIVATE|MS_REC，切断 peer group
    │    ├─ ② loadPolicy()              memfd fd 接收规则（零 IPC 内存级）
    │    ├─ ③ applyAllMounts()          Trie 匹配 → lazy stat → RAII mount
    │    └─ ④ setOption(DLCLOSE)        SO 从内存彻底消失
    │
    ├─ UID drop + SELinux 域切换
    └─ App 代码执行  ◄── mount 已生效，SO 已消失，零可检测痕迹
```

---

## 3. 隐蔽性：五层完整防御

### 3.1 第一层：MS_PRIVATE|MS_REC — 切断 peer group 传播

**威胁**：2025 年银行 App 检测报告指出，mount namespace mismatch 是评分最高（+60）的检测信号。检测方式为对比 App 与系统进程 mountinfo 中的 shared peer group ID——若 App 存在额外的 shared peer group，即判定有注入行为。

**防御**：在任何 mount 操作之前，将整个 namespace 递归私有化，之后所有 mount 操作均不产生 shared peer group ID：

```c
static void isolateNamespace(void) {
    // 递归私有化 namespace 内全部挂载点
    // 此后 mount 操作不向任何 peer group 传播，无法被跨进程关联
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        // 部分 OEM 内核不支持此操作，记录日志但不阻断
        // 后续功能正常，仅此层隐蔽性降级
        LOGE("PG: MS_PRIVATE|MS_REC errno=%d (OEM kernel restriction)", errno);
    }
}
```

### 3.2 第二层：tmpfs + bind + MNT_DETACH — source 路径彻底消失

**威胁**：直接 bind mount 会在 mountinfo 的 source 字段暴露 `/data/adb/modules/path_guard/...`，任何能读 `/proc/self/mountinfo` 的 App 都能发现模块路径和两条 mount 条目共享的 major:minor 设备号。

**防御**：三步流程，source 挂载点在 < 1μs 内消失，mountinfo 中只剩一条纯净的 tmpfs 条目：

```c
// 依据：Linux umount2(2) — MNT_DETACH 立即将挂载点从挂载表移除，
// 但已建立的 bind mount 维持独立 vfsmount 引用计数，bind 继续有效
void applyStealthOverlay(const char *target,
                          uid_t uid, gid_t gid, mode_t mode) {
    // 临时挂载点：/dev/ 下 8 字节随机 hex，存在时间 < 1μs
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/dev/.%s", secureRandomHex(8));
    mkdir(tmp, 0700);

    char opts[128];
    snprintf(opts, sizeof(opts),
             "mode=%o,uid=%u,gid=%u", mode, uid, gid);

    // Step 1: tmpfs → 临时挂载点
    if (mount("tmpfs", tmp, "tmpfs",
              MS_NOSUID | MS_NODEV, opts) != 0) {
        rmdir(tmp); return;
    }

    // Step 2: bind 临时点 → 目标路径（建立独立 vfsmount 引用）
    if (mount(tmp, target, NULL, MS_BIND | MS_REC, NULL) != 0) {
        umount2(tmp, MNT_DETACH);
        rmdir(tmp); return;
    }

    // Step 3: 立即 MNT_DETACH 临时点
    // 临时点从 mountinfo 消失，bind 不受影响（独立引用计数）
    // 结果：mountinfo 只剩：
    // 742 739 0:91 / /storage/emulated/0/DCIM/X rw - tmpfs tmpfs rw
    // 与系统正常 tmpfs 挂载完全一致，不可溯源
    umount2(tmp, MNT_DETACH);
    rmdir(tmp);
}
```

**效果对比**：

| 检测手段 | 无保护 | v11 三步流程 |
|---|---|---|
| mountinfo source 字段 | 暴露模块路径 | 只有 `tmpfs tmpfs`，不可溯源 |
| 两条 mount 共享 major:minor | 可关联 | 只有一条，无从关联 |
| /data/adb/modules/ 目录残留 | 有 tmpPoint 残留 | rmdir 后无痕 |

### 3.3 第三层：DLCLOSE — SO 从内存彻底消失

**威胁**：未卸载的 SO 永久留在 `/proc/self/maps` 和 `dl_iterate_phdr` 中，是最直接的注入指示器。

**依据**：NeoZygisk v2.x（2025 Release）核心特性之一是 soinfo_unload——使用 soinfo_unload 函数优雅地从内存移除所有 library 痕迹；同时 atexit 注册被主动清零，消除模块指纹。ZygiskNext ZN Linker 同样提供匿名内存加载支持。

```c
// 执行顺序（preAppSpecialize 末尾，所有 mount 完成之后）：

// ① 清零 atexit 注册（防止 SO 卸载后 atexit 链表残留指纹）
// 代码规范：禁止所有带析构函数的全局对象（static std::string 等）
// 主动 finalize 兜底：
extern void __cxa_finalize(void *dso);
extern void *__dso_handle;
__cxa_finalize(__dso_handle);

// ② 请求 Zygisk 运行时卸载本 SO
// NeoZygisk/ZygiskNext 的 soinfo_unload 会处理链表修复
api->setOption(ZYGISK_DLCLOSE_MODULE_LIBRARY);

// 此后：
//   /proc/self/maps 中无 path_guard SO
//   dl_iterate_phdr 不再枚举到本 SO
//   atexit 链表无本 SO 的注册项
//   soinfo 链表由 NeoZygisk/ZygiskNext 修复（无 gap）
```

### 3.4 第四层：memfd jit-cache — 规则缓存 fd 伪装

Zygisk 自身使用 `memfd_create("jit-cache", ...)` 加载模块 SO，使其在 `/proc/self/fd` 中显示为 `/memfd:jit-cache (deleted)`。PathGuard 的规则缓存 fd 采用同名，与 Zygote 真实 jit-cache 完全混同，无法区分：

```c
// Companion 端：创建规则缓存 fd
// "jit-cache" = Zygisk 加载模块时使用的 memfd 名称
// /proc/{pid}/fd/{n} 显示为 /memfd:jit-cache (deleted)
int fd = memfd_create("jit-cache", MFD_CLOEXEC);
write(fd, &policy, sizeof(policy));
lseek(fd, 0, SEEK_SET);
// 通过 SCM_RIGHTS 传递给 Zygisk 模块端（零文件系统路径）

// FD fallback（v7 鲁棒性补丁）：
// memfd/fd 传递失败时，退回文件缓存方案：
// /dev/.__sys_{random8hex}/{sha256(pkg)[:16]}
// 路径无规律，无法反推模块身份
```

### 3.5 第五层：SO 二进制硬化

```makefile
# 构建标志（完整版）
LOCAL_CFLAGS += \
    -Os                      \  # 代码大小最优化
    -flto                    \  # 链接时优化，消除死代码
    -ffunction-sections      \  # 按函数粒度 DCE
    -fdata-sections          \  # 按数据粒度 DCE
    -fvisibility=hidden      \  # 所有符号默认隐藏
    -fno-exceptions          \  # 消除异常处理表（同时减少 atexit 注册）
    -fno-rtti                \  # 消除 RTTI 信息
    -fno-plt                 \  # 消除 PLT，配合 -z,now 彻底消除延迟绑定
    -fomit-frame-pointer     \  # 减少寄存器压力
    -fstack-protector-strong \  # 栈保护（安全基线）
    -DNDEBUG                    # 禁用 assert

LOCAL_LDFLAGS += \
    -Wl,--gc-sections        \  # 删除未使用段
    -Wl,--icf=safe           \  # 消除相同函数体
    -Wl,--strip-all          \  # 剥离所有调试信息
    -Wl,-z,now               \  # 立即绑定，无 lazy 重定位
    -Wl,--no-undefined       \  # 链接时报所有未定义符号（v7 补丁）
    -Wl,--exclude-libs,ALL   \  # 静态库符号不泄漏到导出表（v7 补丁）
    -Wl,--version-script=version_script.map

LOCAL_LDLIBS := -llog -lselinux   # 无 -ldl：移除所有 dlopen 运行时依赖

# version_script.map：只导出 2 个必要符号
# {
#   global: zygisk_module_entry; zygisk_companion_entry;
#   local: *;
# };
```

**字符串混淆**（XOR，所有路径和标识符）：

```c
// 编译期 XOR 编码，运行时解码到栈，strings 扫描不可见
#define XOR_KEY 0x5A
#define OBFSTR(enc) _xor_decode(enc, sizeof(enc)-1)
static inline char *_xor_decode(const char *e, size_t n) {
    static char b[256];
    for (size_t i = 0; i < n; i++) b[i] = e[i] ^ XOR_KEY;
    b[n] = '\0';
    return b;
}
// 使用：const char *p = OBFSTR("\x57\x2f\x3e\x2f"); // "/dev/" XOR 0x5A
```

**指标目标**：

| 指标 | 目标 |
|---|---|
| arm64 SO 大小 | < 44 KB |
| 导出符号数 | ≤ 2 |
| 未定义符号 | 0 |
| atexit 注册 | 0 |
| 敏感字符串（strings 扫描）| 0 |

---

## 4. 存储路径完整展开

Android 存储系统有多层等价路径，必须全部覆盖，否则 App 可通过别名路径绕过规则。

```c
// 严格按 userId 参数展开（Work Profile: userId = uid / 100000）
// userId=0：主用户；userId=10：工作资料；userId=11,12,...：次要用户
int resolveStoragePaths(const char *logical, int userId,
                        char out[][PATH_MAX], int maxOut) {
    const char *rel = extractRelSubpath(logical);  // 提取相对子路径
    char uid_s[12];
    snprintf(uid_s, sizeof(uid_s), "%d", userId);
    int n = 0;

#define ADD(fmt, ...) \
    if (n < maxOut) snprintf(out[n++], PATH_MAX, fmt, ##__VA_ARGS__)

    ADD("/sdcard/%s",                                       rel);
    ADD("/storage/emulated/%s/%s",              uid_s,     rel);
    ADD("/storage/self/primary/%s",                         rel);
    ADD("/mnt/user/%s/primary/%s",              uid_s,     rel);
    ADD("/mnt/runtime/default/emulated/%s/%s",  uid_s,     rel);
    ADD("/mnt/runtime/read/emulated/%s/%s",     uid_s,     rel);
    ADD("/mnt/runtime/write/emulated/%s/%s",    uid_s,     rel);
    ADD("/data/media/%s/%s",                    uid_s,     rel); // FUSE 底层
#undef ADD
    return n;
}

// lazy stat：只对 lstat() 成功的路径执行 mount
// 跳过不存在的等价路径，消除无效 mount 调用（v6 核心性能优化）
```

---

## 5. 规则引擎

### 5.1 Trie 前缀树匹配

时间复杂度 O(path_length)，与规则总数无关：

```c
// 目录规则安全匹配（防止 A-TEST 误伤 A-TEST_backup）：
//   path == rule_path              → 命中（目录本身）
//   path 以 rule_path + "/" 开头   → 命中（目录内后代）
//
// 文件规则：
//   path == rule_path              → 命中（精确匹配）
```

### 5.2 lexicalNormalize — 路径穿越防御

纯内存操作，零磁盘 I/O，对应 C++17 `path::lexically_normal()` 语义：

```c
// 消灭 ../ 绕过攻击，不依赖 realpath()（realpath 路径不存在时失败）
void lexicalNormalize(const char *in, char *out, size_t size) {
    // 按 '/' 分段，. 跳过，.. 弹出上一段，绝对路径 .. 不超根
    // 结果写入 out（栈分配，零动态内存）
}

// 攻击验证：
// DCIM/Camera/../../A-TEST/hide.txt
//   → /storage/emulated/0/DCIM/Camera/../../A-TEST/hide.txt
//   → lexical: /storage/emulated/0/A-TEST/hide.txt
//   → Trie 命中 "- DCIM/A-TEST" 规则 → 正确拦截 ✅
//
// /storage/emulated/0/DCIM/A-TEST_backup
//   → 不以 A-TEST/ 开头，不命中 ✅
```

### 5.3 冲突解决优先级

1. **路径更长**的规则优先（更具体覆盖更泛）
2. 同路径下**文件规则**优先于目录规则
3. 动作优先级：`-`（DENY）> 重定向（`->` / `=>`）> `+`（ALLOW）

### 5.4 SELinux 三级降级

```c
int mountWithSELinux(const char *src, const char *tgt,
                     const char *type, unsigned long flags,
                     const char *opts) {
    char label[512];
    // 第一级：继承目标路径现有 label（最精确，MIUI/HyperOS 首选）
    if (getfilecon(tgt, &label) >= 0) {
        setfscreatecon(label);
        if (mount(src, tgt, type, flags, opts) == 0) {
            setfscreatecon(NULL); return 0;
        }
    }
    // 第二级：通用 sdcard label
    setfscreatecon("u:object_r:sdcard_posix:s0");
    if (mount(src, tgt, type, flags, opts) == 0) {
        setfscreatecon(NULL); return 0;
    }
    setfscreatecon(NULL);
    // 第三级：不设置 context，直接挂载（兜底）
    return mount(src, tgt, type, flags, opts);
}
```

---

## 6. 主流程（完整版）

```c
void preAppSpecialize(AppSpecializeArgs *args) {
    const char *pkg = env->GetStringUTFChars(args->nice_name, nullptr);

    // 快速路径：绝大多数 App 无规则（__builtin_expect 优化分支预测）
    if (__builtin_expect(!hasPolicyFor(pkg), 1)) goto dlclose;

    // ① namespace 私有化
    isolateNamespace();

    // ② 加载规则（memfd 主路径 + file fallback，50ms 超时 fail-open）
    {
        int pfd = acquirePolicyFd(api, pkg);  // 含超时逻辑
        if (pfd < 0) goto dlclose;  // fail-open：无规则，App 正常启动

        AppPolicyPOD policy;  // 固定大小 POD，栈分配，零动态内存
        if (!loadPolicyFromFd(pfd, &policy)) { close(pfd); goto dlclose; }
        close(pfd);

        // ③ 路径展开 + lazy stat + 按路径深度降序排列
        int userId = args->uid / 100000;
        MountGuard guard = {};  // RAII：作用域结束或异常时自动 rollback
        applyPolicy(&policy, userId, &guard);
        // 注意：guard 在此作用域内，任何 panic 自动 umount 已挂载路径
    }

dlclose:
    // ④ atexit 清零 + SO 卸载（完成即消失）
    __cxa_finalize(__dso_handle);
    api->setOption(ZYGISK_DLCLOSE_MODULE_LIBRARY);
    env->ReleaseStringUTFChars(args->nice_name, pkg);
}
```

---

## 7. RAII MountGuard（异常安全）

```c
// 任何错误路径自动逆序 umount 已挂载的全部路径
typedef struct {
    char   paths[MAX_MOUNTS_PER_APP][PATH_MAX];
    int    count;
} MountGuard;

static void mountGuard_add(MountGuard *g, const char *path) {
    if (g->count < MAX_MOUNTS_PER_APP)
        strlcpy(g->paths[g->count++], path, PATH_MAX);
}

static void mountGuard_rollback(MountGuard *g) {
    for (int i = g->count - 1; i >= 0; i--)
        umount2(g->paths[i], MNT_DETACH);
    g->count = 0;
}
```

---

## 8. Companion Daemon

**职责**：解析 rules.ini → 编译 AppPolicy → 序列化为 AppPolicyPOD → 写入 memfd → 通过 SCM_RIGHTS 传递给 Zygisk 端

**热更新**：监听 SIGUSR1 信号，原子 reload 规则（shared_mutex 写锁）

```c
// 热更新流程：
// 1. 解析新规则到临时内存
// 2. 获取写锁，原子替换规则集
// 3. 下次 preAppSpecialize 时生效（当前运行中的 App 不受影响）
static volatile sig_atomic_t g_reload = 0;
void sigusr1_handler(int sig) { g_reload = 1; }
```

---

## 9. 性能目标

| 场景 | 耗时目标 | 保障手段 |
|---|---|---|
| 无规则 App（绝大多数） | < 50 μs | `__builtin_expect` 快速返回 |
| 有规则，3 条 | < 300 μs | 零 malloc，栈分配，lazy stat |
| 有规则，50 条 | < 550 μs | Trie O(len)，批量 lazy stat |
| isolateNamespace 额外开销 | < 50 μs | 单次 mount 系统调用 |
| Companion RSS | < 200 KB | 无 STL，纯 C |

---

## 10. 生态兼容性

| Zygisk 实现 | 支持状态 | 关键说明 |
|---|---|---|
| Magisk 内置 Zygisk（v27+） | ✅ | 主要平台 |
| ZygiskNext（KSU/APatch）| ✅ | ZN Linker 提供匿名内存 + soinfo 修复 |
| NeoZygisk v2.x（KSU/APatch）| ✅ 推荐 | soinfo_unload，atexit 自动清零，Zygote 栈清理，Android 16 已验证稳定 |
| ReZygisk（KSU/APatch）| ✅ | 自定义链接器，无 dlopen 依赖问题 |
| APatch 条件编译 | ✅ | -DPG_ROOT_APATCH，customize.sh 自动选择 |

**推荐搭配**：PathGuard + NeoZygisk（soinfo 修复）+ Shamiko（根隐藏）= 用户态检测覆盖率接近上限
-e 
---

# PathGuard v11 — Part 2：静态 / 动态双引擎重定向

> **平台**：Android 12+，GKI kernel 5.10.220 / 5.15+（动态引擎强制要求）  
> **参考**：Linux fanotify(7)/fanotify_init(2) 官方手册 · torvalds/linux fanotify.h 源码 · posix_fadvise(2) · ioprio_set(2) · Linux umount2(2)

---

## 1. 两种重定向的本质区别

先从用户心智模型说清楚，再讲实现：

```
规则语法：
  DCIM/Camera   ->  Android/data/com.tencent.mm/cache/Camera  （静态，->）
  DCIM/WeiXin   =>  Pictures/WeiXin_Archive                   （动态，=>）

静态重定向（->）= 任意门
  两个路径共享同一份底层数据，完全双向透明，零 I/O 开销。
  App 以为自己在写 DCIM/Camera，数据实际落在 Android/data/... 下。
  卸载 App，系统自动清理 Android/data/<pkg>/ 下所有数据。（「卸载即焚」）

动态重定向（=>）= 搬运工
  守护进程持续监控源路径，文件写入完成后异步搬走。
  App 原路径上的文件随后消失，数据转移到用户控制的目录。
  典型用途：强制从 App 沙盒提取重要文件，防止 App 后期擅自删除。
```

**关键区别**：静态引擎在 App 启动瞬间由 Zygisk SO 完成，共享 Part 1 全部基础设施；动态引擎是独立的 root 守护进程，与 Zygisk SO 物理隔离，持续异步运行。

---

## 2. 静态引擎（`->`）：bind mount 重定向

### 2.1 实现原理

静态引擎与访问控制（Part 1）共用 namespace、RAII MountGuard、SELinux 降级全套基础设施，仅将「挂载 tmpfs 空目录」改为「bind 挂载目标路径」：

```c
// 把 dst 的内容绑定到 src 位置
// App 访问 src → 读写 dst 的内容，完全双向透明
// App 以为在写 src，数据实际落在 dst
static void applyBindRedirect(const char *src, const char *dst,
                               const struct stat *src_stat) {
    // 目标路径不存在时自动创建（继承 src 的 mode/uid/gid）
    ensurePathExists(dst, src_stat);

    // bind mount：dst → src
    // 注意：bind 的 source（dst）是用户数据目录，非模块私有路径
    // mountinfo 中 source 显示为 dst 路径，属于正常预期行为
    mount(dst, src, NULL, MS_BIND | MS_REC, NULL);
}
```

**mountinfo 最终状态**（与系统正常 bind mount 无法区分）：

```
743 739 8:1 /data/media/0/Android/data/com.tencent.mm/cache/Camera
            /storage/emulated/0/DCIM/Camera rw,relatime
            master:1 - ext4 /dev/block/sda5 rw
```

### 2.2 「卸载即焚」产品特性

将重定向目标设置为 `Android/data/<pkg>/` 子目录：

```ini
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
```

当用户卸载 App，Android 系统自动清理 `Android/data/com.tencent.mm/`，所有数据随 App 消失，用户存储目录零残留。

### 2.3 静态引擎执行时序

完全嵌入 preAppSpecialize，与访问控制操作串行执行，无额外耗时：

```
applyPolicy() 内部：
  for each rule in AppPolicy (按路径深度降序):
      expanded_paths = resolveStoragePaths(rule.src, userId)
      for each path in expanded_paths:
          if lstat(path) 失败: continue  （lazy stat 跳过）
          if rule.action == DENY:
              applyStealthOverlay(path, ...)  // tmpfs 覆盖
          if rule.action == REDIRECT_STATIC:
              applyBindRedirect(path, rule.dst, ...)  // bind 重定向
          mountGuard_add(&guard, path)
```

---

## 3. 动态引擎（`=>`）：fanotify + 异步搬运

### 3.1 为什么用 fanotify 而不是 inotify

| 能力维度 | inotify | fanotify |
|---|---|---|
| 挂载点级别监控（一次覆盖全部）| ❌ 需逐目录注册，新建子目录有窗口期 | ✅ FAN_MARK_MOUNT 一次搞定 |
| 触发进程 PID 识别 | ❌ 无进程信息 | ✅ FAN_REPORT_PIDFD，稳定不复用 |
| 文件名直接获取 | ❌ 需自行管理 watch descriptor 映射 | ✅ FAN_REPORT_DFID_NAME，父目录 fh + 文件名 |
| FUSE 层稳定性 | ❌ Android 11+ FUSE 集成有已知问题 | ✅ 直接监控 /data/media（ext4）稳定 |
| 内核要求 | 2.6.13+，无特殊要求 | 5.10.220 / 5.15+（Android 12 GKI 满足）|

**结论**：fanotify 在 Android 12+ GKI 设备上全面优于 inotify。

### 3.2 fanotify 初始化

```c
// daemon/watcher.c

int pg_fanotify_init(void) {
    /*
     * 初始化标志选择依据（均来自 Linux fanotify_init(2) 官方手册）：
     *
     * FAN_CLASS_NOTIF
     *   纯通知模式，不阻断 App 进程。事件处理完全异步，
     *   App 感知不到任何延迟。
     *   （对比 FAN_CLASS_CONTENT/PRE_CONTENT 会挂起 App 等待决策）
     *
     * FAN_REPORT_DFID_NAME
     *   = FAN_REPORT_DIR_FID | FAN_REPORT_NAME（Linux 5.9+）
     *   每个事件附带：父目录 file_handle + 紧跟的文件名字符串。
     *   依据 fanotify(7) 官方手册：
     *   "if info_type == FAN_EVENT_INFO_TYPE_DFID_NAME,
     *    the file handle is followed by a null terminated string
     *    that identifies the name of a directory entry in that
     *    directory"
     *   通过 open_by_handle_at(父目录 fh) → readlink → 拼文件名
     *   = 完整底层路径，无需遍历 /proc/pid/fd，路径重建效率最高。
     *
     * FAN_REPORT_PIDFD
     *   依据 fanotify_init(2) 官方手册：
     *   "since Linux 5.15 and 5.10.220"
     *   注：5.10.220 意味着 Android 12 的 LTS 5.10 内核回合了此补丁，
     *   比通常认知的 5.15 更早可用。
     *   事件附带 pidfd（稳定进程引用）：
     *   - pid 在进程退出后可被新进程复用
     *   - pidfd 不会被复用，可靠标识触发进程
     *   - 进程退出后读取事件时返回 FAN_NOPIDFD（需处理此错误值）
     *   - 使用完毕后必须手动 close(pidfd)
     *
     * FAN_CLOEXEC | FAN_NONBLOCK
     *   exec 时自动关闭；read 非阻塞，配合 epoll 使用。
     */
    int fan_fd = fanotify_init(
        FAN_CLASS_NOTIF      |
        FAN_REPORT_DFID_NAME |  // Linux 5.9+，路径重建必需
        FAN_REPORT_PIDFD     |  // Linux 5.10.220/5.15+，进程识别必需
        FAN_CLOEXEC          |
        FAN_NONBLOCK,
        O_RDONLY | O_LARGEFILE
    );

    if (fan_fd < 0) {
        // errno=EINVAL：内核不支持指定 flags 组合（非 GKI 老内核）
        // errno=EPERM ：缺少 CAP_SYS_ADMIN（不应发生，守护进程以 root 运行）
        // 两种情况均静默退出，动态引擎不启动，其他功能不受影响
        LOGE("PG: fanotify_init failed errno=%d (non-GKI kernel)", errno);
        return -1;
    }

    LOGI("PG: fanotify initialized, dynamic engine active");
    return fan_fd;
}
```

### 3.3 挂载点级别监控

```c
int pg_fanotify_watch(int fan_fd) {
    /*
     * FAN_MARK_ADD | FAN_MARK_MOUNT
     *   标记 /data/media 整个挂载点（ext4 底层，绕过 FUSE 层）。
     *   之后该挂载点下任意深度的文件写入事件均上报。
     *   无需像 inotify 那样逐目录注册，新建子目录无窗口期漏报。
     *
     * 为什么监控 /data/media 而非 /sdcard：
     *   /sdcard → FUSE → /data/media（ext4 真实文件系统）
     *   Android 11+ 中 FUSE 层与 fsnotify 的集成存在已知稳定性问题。
     *   直接监控 /data/media 绕过 FUSE，事件稳定可靠。
     *   代价：需要 root 权限才能访问 /data/media。
     *
     * FAN_CLOSE_WRITE
     *   文件最后一个可写 fd 关闭时触发。
     *   这是「写入真正完成」的最可靠信号：
     *   App 调用 close() 之后才触发，不会在写入过程中触发，
     *   确保搬运的是完整文件，不是写到一半的半成品。
     */
    int ret = fanotify_mark(
        fan_fd,
        FAN_MARK_ADD | FAN_MARK_MOUNT,
        FAN_CLOSE_WRITE,
        AT_FDCWD,
        "/data/media"   // ext4 底层，绕过 FUSE
    );

    if (ret < 0) {
        LOGE("PG: fanotify_mark failed errno=%d", errno);
    }
    return ret;
}
```

### 3.4 事件读取与路径重建

```c
// 事件缓冲区：16 KB，一次 read() 批量读取 ~30-50 个事件
// 减少系统调用次数，提升吞吐量
#define EVENT_BUF_LEN (16 * 1024)

void pg_event_loop(int fan_fd, TaskQueue *queue) {
    char buf[EVENT_BUF_LEN]
        __attribute__((aligned(
            __alignof__(struct fanotify_event_metadata))));

    // epoll：无事件时休眠（0% CPU），有事件时立即唤醒
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fan_fd};
    epoll_ctl(epfd, EPOLL_CTL_ADD, fan_fd, &ev);

    while (g_running) {
        struct epoll_event fired;
        if (epoll_wait(epfd, &fired, 1, -1) < 1) continue;

        // 批量读取所有待处理事件
        ssize_t len = read(fan_fd, buf, sizeof(buf));
        if (len <= 0) continue;

        const struct fanotify_event_metadata *meta =
            (void *)buf;

        while (FAN_EVENT_OK(meta, len)) {
            if (meta->mask & FAN_CLOSE_WRITE)
                process_one_event(meta, queue);
            meta = FAN_EVENT_NEXT(meta, len);
        }
    }
}

static void process_one_event(
        const struct fanotify_event_metadata *meta,
        TaskQueue *queue) {

    char    path[PATH_MAX] = {};
    int     pidfd = -1;
    pid_t   pid   = meta->pid;  // 备用（pidfd 优先）

    // 遍历附加信息记录（info records）
    // 依据 fanotify(7)：info records 的顺序不保证，必须按 info_type 判断
    const struct fanotify_event_info_header *info =
        (void *)meta + meta->metadata_len;
    const struct fanotify_event_info_header *end =
        (void *)meta + meta->event_len;

    while ((char *)info < (char *)end) {

        if (info->info_type == FAN_EVENT_INFO_TYPE_PIDFD) {
            // pidfd 信息记录
            const struct fanotify_event_info_pidfd *pi = (void *)info;
            if (pi->pidfd >= 0) {
                // 正常 pidfd：复制一份以供后续使用
                pidfd = dup(pi->pidfd);
            }
            // FAN_NOPIDFD：进程在读取事件前已退出
            // FAN_EPIDFD：pidfd 创建失败（罕见）
            // 两种情况均跳过包名过滤，回退到 meta->pid
            close(pi->pidfd);  // 原始 pidfd 使用后必须关闭（官方手册要求）

        } else if (info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
            // 父目录 file_handle + 文件名
            const struct fanotify_event_info_fid *fi = (void *)info;
            const struct file_handle *fh =
                (void *)&fi->handle;

            // 1. file_handle → 目录 fd（open_by_handle_at）
            int mfd = open("/data/media", O_PATH | O_RDONLY);
            int dfd = open_by_handle_at(mfd, (void *)fh,
                                         O_PATH | O_RDONLY);
            close(mfd);
            if (dfd < 0) goto next_info;

            // 2. /proc/self/fd/{dfd} → 目录绝对路径
            char dir_path[PATH_MAX];
            char fdlnk[48];
            snprintf(fdlnk, sizeof(fdlnk),
                     "/proc/self/fd/%d", dfd);
            ssize_t n = readlink(fdlnk, dir_path,
                                  sizeof(dir_path) - 1);
            close(dfd);
            if (n <= 0) goto next_info;
            dir_path[n] = '\0';

            // 3. 文件名：紧跟在 file_handle 数据之后的 null 终止字符串
            // 依据 fanotify(7) 官方手册：
            // "the file handle is followed by a null terminated
            //  string that identifies the name of a directory entry"
            const char *fname =
                (char *)fh->f_handle + fh->handle_bytes;

            // 4. 拼接完整底层路径
            snprintf(path, sizeof(path),
                     "%s/%s", dir_path, fname);
        }

    next_info:
        info = (void *)((char *)info + info->len);
    }

    if (path[0] == '\0') return;  // 路径重建失败，跳过

    // 包名识别 + 规则匹配
    char pkg[256] = {};
    bool have_pkg = false;

    if (pidfd >= 0) {
        have_pkg = pidfd_to_package(pidfd, pkg, sizeof(pkg));
        close(pidfd);
    }
    if (!have_pkg && pid > 0) {
        // 回退：直接用 meta->pid（注意 pid 可能已被复用）
        have_pkg = pid_to_package(pid, pkg, sizeof(pkg));
    }

    MoveTask task;
    if (have_pkg && rules_match_dynamic(path, pkg, &task)) {
        queue_push(queue, &task);
    }
}
```

### 3.5 包名识别（pidfd 优先路径）

```c
// 通过 pidfd 稳定获取 App 包名
// pidfd 不会因进程退出后被新进程复用，避免竞态
bool pidfd_to_package(int pidfd, char *out, size_t size) {
    // /proc/self/fdinfo/{pidfd} 包含「Pid:\t{actual_pid}」
    char fdinfo[64];
    snprintf(fdinfo, sizeof(fdinfo),
             "/proc/self/fdinfo/%d", pidfd);
    FILE *f = fopen(fdinfo, "r");
    if (!f) return false;

    pid_t pid = -1;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Pid:\t%d", &pid) == 1) break;
    }
    fclose(f);
    if (pid <= 0) return false;

    return pid_to_package(pid, out, size);
}

bool pid_to_package(pid_t pid, char *out, size_t size) {
    char cmdline_path[64];
    snprintf(cmdline_path, sizeof(cmdline_path),
             "/proc/%d/cmdline", pid);
    FILE *f = fopen(cmdline_path, "r");
    if (!f) return false;

    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    if (n == 0) return false;
    out[n] = '\0';

    // 去掉 :processName 后缀（如 com.tencent.mm:push）
    char *colon = strchr(out, ':');
    if (colon) *colon = '\0';

    return out[0] != '\0';
}
```

### 3.6 路径映射：底层 → 逻辑路径

fanotify 监控 `/data/media/0/...`，规则配置的是 `/storage/emulated/0/...`：

```c
// /data/media/0/DCIM/WeiXin/img.jpg
//   → /storage/emulated/0/DCIM/WeiXin/img.jpg
void data_media_to_storage(const char *bottom, int userId,
                            char *out, size_t size) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix),
             "/data/media/%d/", userId);

    if (strncmp(bottom, prefix, strlen(prefix)) == 0) {
        snprintf(out, size, "/storage/emulated/%d/%s",
                 userId, bottom + strlen(prefix));
    } else {
        strlcpy(out, bottom, size);
    }
}
```

---

## 4. 搬运引擎：四层并发控制

### 4.1 第一层：IOPRIO_CLASS_IDLE（内核级背压）

最重要，一行代码解决 80% 的系统性能干扰问题：

```c
// 搬运线程启动时设置一次，由内核调度器自动处理后续
// IOPRIO_CLASS_IDLE（=3）：最低 I/O 调度类
//   - 系统繁忙时：完全让路给其他进程，用户无感知
//   - 系统空闲时：全速推进，利用空闲磁盘带宽
//   无需任何主动限速逻辑，内核天然处理
static void set_io_idle(void) {
    syscall(__NR_ioprio_set,
            1 /* IOPRIO_WHO_PROCESS */,
            0 /* current thread */,
            3 << 13 /* IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT */);
}
```

### 4.2 第二层：信号量分级并发控制

UFS 存储的 WriteBooster（伪 SLC 缓存）容量有限，并发多个大文件写入会耗尽缓冲，触发从 ~1200 MB/s 骤降到 ~100 MB/s 的速度悬崖：

```c
// 按文件大小分三级，控制最大并发数
// 小文件 (<1 MB)：I/O 开销极小，允许 4 并发
// 中等文件 (1–50 MB)：限 2 并发，避免打满 WriteBooster 缓冲
// 大文件 (>50 MB)：强制串行，1 个 at a time
static sem_t g_sem_small;   // 初始值 4
static sem_t g_sem_medium;  // 初始值 2
static sem_t g_sem_large;   // 初始值 1

void mover_init_semaphores(void) {
    sem_init(&g_sem_small,  0, 4);
    sem_init(&g_sem_medium, 0, 2);
    sem_init(&g_sem_large,  0, 1);
}

static sem_t *pick_sem(off_t size) {
    if (size <  1 * 1024 * 1024) return &g_sem_small;
    if (size < 50 * 1024 * 1024) return &g_sem_medium;
    return &g_sem_large;
}
```

### 4.3 第三层：分块 sendfile + POSIX_FADV_DONTNEED

大文件全量 sendfile 会把整个文件写入页面缓存，挤走其他 App 的工作集，导致系统 UI 卡顿。分块处理，每块完成后立即释放：

```c
static bool sendfile_move_chunked(const char *src, const char *dst,
                                   off_t total) {
    const off_t CHUNK = 4 * 1024 * 1024;  // 4 MB/块

    int src_fd = open(src, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) return false;

    struct stat st;
    fstat(src_fd, &st);

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC,
                       st.st_mode & 0777);
    if (dst_fd < 0) { close(src_fd); return false; }

    // 顺序读取提示，优化内核预读策略
    posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    off_t done = 0;
    bool  ok   = true;

    while (done < total) {
        // 第四层：写速监控（见下一节）
        adaptive_io_throttle(done);

        off_t chunk = (total - done > CHUNK)
                      ? CHUNK : (total - done);
        ssize_t sent = sendfile(dst_fd, src_fd, &done, chunk);
        if (sent <= 0) { ok = false; break; }

        // 每块完成后立即释放源文件页面缓存
        // 防止大文件长期占用系统内存，挤压其他 App 工作集
        posix_fadvise(src_fd, done - sent, sent,
                      POSIX_FADV_DONTNEED);
    }

    if (ok) {
        // 关键顺序：必须先 fdatasync，再 POSIX_FADV_DONTNEED
        // 原因：DONTNEED 对脏页无效（内核忽略），必须先落盘再释放缓存
        fdatasync(dst_fd);
        posix_fadvise(dst_fd, 0, 0, POSIX_FADV_DONTNEED);
    }

    close(src_fd);
    close(dst_fd);

    if (ok) {
        unlink(src);   // 源文件删除
    } else {
        unlink(dst);   // 失败时清理不完整目标文件
    }
    return ok;
}
```

### 4.4 第四层：/proc/diskstats 写速监控（WriteBooster 耗尽检测）

```c
// 每 50 MB 采样一次实时写入速率
// 速率 < 10 MB/s 说明 WriteBooster SLC 缓冲耗尽，进入 TLC 裸速
// 主动暂停 2 秒，等待后台 GC 恢复缓冲
static void adaptive_io_throttle(off_t bytes_done) {
    const off_t SAMPLE_INTERVAL = 50 * 1024 * 1024;  // 每 50 MB 采样
    if (bytes_done == 0 || bytes_done % SAMPLE_INTERVAL != 0) return;

    static uint64_t last_sectors  = 0;
    static struct timespec last_ts = {};

    // 读 /proc/diskstats 第 10 列（写入扇区数）
    uint64_t cur_sectors = read_write_sectors_from_diskstats();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (last_sectors > 0) {
        double elapsed = (now.tv_sec - last_ts.tv_sec) +
                         (now.tv_nsec - last_ts.tv_nsec) * 1e-9;
        double rate_mbps = (double)(cur_sectors - last_sectors)
                           * 512.0 / 1024.0 / 1024.0 / elapsed;

        if (rate_mbps < 10.0) {
            LOGW("PG: WriteBooster exhausted (%.1f MB/s), pausing 2s",
                 rate_mbps);
            sleep(2);  // 等待后台 GC 恢复 SLC 缓冲
        }
    }

    last_sectors = cur_sectors;
    last_ts      = now;
}
```

---

## 5. 搬运主循环：竞态保护 + 串行执行

```c
static bool move_file(const char *src, const char *dst_dir) {
    struct stat st;
    if (lstat(src, &st) != 0) return false;

    // 构造目标路径，确保目标目录存在
    const char *fname = strrchr(src, '/');
    if (!fname) return false;
    fname++;

    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, fname);
    mkdir_p(dst_dir, 0755);

    // 获取并发令牌（阻塞直到有空闲槽位）
    sem_t *sem = pick_sem(st.st_size);
    sem_wait(sem);
    bool ok = false;

    // 优先路径：同分区 rename()
    // 原子操作，零 I/O，< 1 ms，无需任何并发控制
    if (rename(src, dst) == 0) {
        ok = true;
        goto done;
    }

    // 跨分区（errno=EXDEV）：sendfile + 四层并发控制
    if (errno == EXDEV) {
        ok = sendfile_move_chunked(src, dst, st.st_size);
    }

done:
    sem_post(sem);
    return ok;
}

// 搬运线程主循环（单线程串行，避免并发 I/O 竞争）
void *mover_thread(void *arg) {
    prctl(PR_SET_NAME, "kworker/u8:2");  // 进程名伪装
    set_io_idle();                        // 设置 IDLE I/O 优先级

    MoveTask task;
    while (g_running) {
        // eventfd 等待：无任务时 0% CPU
        uint64_t val;
        read(g_task_efd, &val, sizeof(val));

        while (queue_pop(&task)) {
            // ① 竞态保护窗口：50 ms
            // 防止「App close() 后立刻 open() 读取，文件已被搬走」崩溃
            usleep(50 * 1000);

            // ② 确认文件已被所有进程关闭
            // 通过 /proc/*/fd 扫描检查 inode 是否仍被持有
            if (is_file_still_open(task.src)) {
                // 仍被持有：500 ms 后重新排队
                usleep(500 * 1000);
                queue_push(&task);
                continue;
            }

            // ③ 执行搬运
            if (!move_file(task.src, task.dst_dir)) {
                LOGE("PG: move failed %s → %s",
                     task.src, task.dst_dir);
            }
        }
    }
    return NULL;
}
```

---

## 6. 守护进程隐蔽设计

守护进程本身是动态引擎最大的可检测点。

```c
int main(int argc, char *argv[]) {
    // ① cmdline 清零（/proc/self/cmdline 变为空）
    memset(argv[0], 0, strlen(argv[0]));

    // ② 进程名伪装为内核工作线程
    //    格式 kworker/u{N}:{M} 是 Linux 内核线程标准命名
    //    普通 App 无法区分真实内核线程与伪装进程
    prctl(PR_SET_NAME, "kworker/u8:2");

    // ③ 双重 fork：成为 init(PID=1) 孤儿进程
    //    不出现在 Magisk/service.sh 进程树中
    pid_t pid = fork();
    if (pid > 0) exit(0);   // 父进程退出
    if (pid < 0) return -1;
    setsid();               // 新会话，脱离终端
    pid = fork();
    if (pid > 0) exit(0);   // 中间进程退出
    if (pid < 0) return -1;
    // 此时守护进程由 init 收养，无父进程痕迹

    // ④ 关闭标准 I/O
    int nfd = open("/dev/null", O_RDWR);
    dup2(nfd, 0); dup2(nfd, 1); dup2(nfd, 2);
    close(nfd);

    // ⑤ 二进制文件随机路径（customize.sh 安装时生成）
    //    /dev/.__sys_{random8hex}/k（单字母文件名）
    //    守护进程启动后 unlink 自身可执行文件
    //    /proc/{pid}/exe 显示为「(deleted)」，无法反查路径
    unlink_self_executable();

    return daemon_run();
}
```

**可检测性矩阵**：

| 检测手段 | 防御措施 | 残余风险 |
|---|---|---|
| `/proc/{pid}/cmdline` 枚举 | 清零 + 进程名伪装为 kworker | 极低 |
| `/proc/{pid}/exe` | unlink 后显示 `(deleted)` | 无 |
| 进程父子树分析 | 双重 fork，init 孤儿 | 无 |
| fanotify fd 扫描 | fd 在 root 进程，App 无读权限 | 无 |
| CPU 占用监控 | epoll 休眠，0% CPU | 无 |
| rename/unlink syscall | App 无权监控其他进程 syscall | 无 |
| 内存占用异常 | RSS < 2 MB，无动态分配热路径 | 无 |

---

## 7. 任务队列（无锁环形队列）

```c
// 单生产者单消费者（事件循环 → 搬运线程）
// 无锁设计：原子操作，零互斥开销
#define QUEUE_CAP 256

typedef struct {
    char src[PATH_MAX];
    char dst_dir[PATH_MAX];
    char pkg[256];
} MoveTask;

typedef struct {
    MoveTask    tasks[QUEUE_CAP];
    atomic_int  head;  // 消费者位置
    atomic_int  tail;  // 生产者位置
    int         efd;   // eventfd（通知搬运线程）
} TaskQueue;

bool queue_push(TaskQueue *q, const MoveTask *t) {
    int tail = atomic_load(&q->tail);
    int next = (tail + 1) % QUEUE_CAP;

    if (next == atomic_load(&q->head)) {
        // 队列满：丢弃最旧任务（保持低内存占用，避免 OOM）
        LOGW("PG: queue full, dropping oldest task");
        atomic_store(&q->head,
            (atomic_load(&q->head) + 1) % QUEUE_CAP);
    }

    q->tasks[tail] = *t;
    atomic_store(&q->tail, next);

    // 唤醒搬运线程
    uint64_t v = 1;
    write(q->efd, &v, sizeof(v));
    return true;
}

bool queue_pop(TaskQueue *q, MoveTask *out) {
    int head = atomic_load(&q->head);
    if (head == atomic_load(&q->tail)) return false;
    *out = q->tasks[head];
    atomic_store(&q->head, (head + 1) % QUEUE_CAP);
    return true;
}
```

---

## 8. 守护进程整体架构

```
pg_daemon（root 孤儿进程，伪装为 kworker/u8:2）
    │
    ├── 主线程（fanotify 事件循环）
    │    ├── epoll 监听 fan_fd（无事件时 0% CPU）
    │    ├── 批量 read()，16 KB 缓冲，~30-50 事件/次
    │    ├── 遍历 info records：
    │    │    ├── FAN_EVENT_INFO_TYPE_DFID_NAME
    │    │    │    open_by_handle_at() → readlink() → 拼接文件名 → 完整路径
    │    │    └── FAN_EVENT_INFO_TYPE_PIDFD
    │    │         dup(pidfd) → close(原始pidfd) → fdinfo → pid → cmdline → 包名
    │    ├── data_media_to_storage() 路径映射
    │    ├── rules_match_dynamic() 规则匹配（路径前缀 + 包名）
    │    └── queue_push() 入队
    │
    ├── 搬运线程（单线程串行）
    │    ├── eventfd read 等待（0% CPU）
    │    ├── 50 ms 竞态保护窗口
    │    ├── is_file_still_open() 确认文件已完全关闭
    │    ├── rename()：同分区，< 1 ms，原子，零 I/O
    │    └── sendfile_move_chunked()：跨分区
    │         ├── IOPRIO_CLASS_IDLE（内核背压，系统繁忙时让路）
    │         ├── 信号量并发限制（按文件大小：4/2/1）
    │         ├── 4 MB 分块 + POSIX_FADV_DONTNEED（页面缓存控制）
    │         └── diskstats 写速监控（SLC 耗尽 → sleep 2s）
    │
    └── 信号线程
         ├── SIGUSR1：热更新动态规则（重新解析 rules.ini）
         └── SIGTERM：优雅退出（等待当前搬运任务完成）
```

---

## 9. 规则格式（完整版）

```ini
# PathGuard v11 规则文件
# 位置：config/rules.ini   编码：UTF-8 + LF

[global]
log   = error    # error（生产）| debug（启用 selfCheck，仅 debug 构建）
audit = false    # true = 只记录，不执行任何 mount 或搬运操作

# ═══════════════ 访问控制 ════════════════════════════════════════

[com.tencent.mm]
mode = whitelist          # 默认拒绝，只有 + 路径可见

+ DCIM/Camera             # 允许访问
+ Pictures/WeiXin
+ Android/data/<pkg>/     # <pkg> = 当前 App 包名（占位符自动展开）

- DCIM/Private/           # 明确拒绝（/ 结尾强制目录规则，防止歧义）

# ═══════════════ 静态重定向（->）= bind mount，双向透明，零 I/O ══

# App 写入 DCIM/Camera 实际落在私有沙盒，卸载 App 自动清理
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera

# ═══════════════ 动态重定向（=>）= fanotify + 异步搬运 ════════════

# 微信保存的图片，写完自动搬到用户相册（防止微信后期清理）
DCIM/WeiXin => Pictures/WeiXin_Archive

[com.tencent.mobileqq]
mode = blacklist          # 默认允许，只有 - 路径被屏蔽

- DCIM/私密/              # 屏蔽目录（DENY）
- Download/私密.zip       # 屏蔽特定文件

# QQ 接收的文件，写完搬到公共下载目录
Tencent/QQfile_recv => Download/QQ_Files
```

**语法速查**：

| 语法 | 引擎 | 含义 |
|---|---|---|
| `mode = whitelist/blacklist` | 访问控制 | 默认拒绝/默认允许 |
| `+ 路径` | 访问控制 | 白名单放行 |
| `- 路径` | 访问控制 | 拒绝并隐藏 |
| `src -> dst` | 静态引擎 | bind mount，双向透明，零 I/O |
| `src => dst` | 动态引擎 | fanotify 监控，写完异步搬运 |
| `<pkg>` | 通用 | 运行时替换为 App 包名 |
| 路径尾部 `/` | 通用 | 强制目录规则（推荐用于目录）|
| 相对路径 | 通用 | 自动补全为 `/storage/emulated/0/<路径>` |

---

## 10. 构建配置

### Zygisk SO（访问控制 + 静态引擎）

```makefile
LOCAL_MODULE    := path_guard
LOCAL_SRC_FILES := module.c companion.cpp mount.c \
                   rules/trie.cpp rules/compiler.cpp \
                   utils/path.c utils/selinux.c utils/random.c
LOCAL_CFLAGS    += -Os -flto -ffunction-sections -fdata-sections \
                   -fvisibility=hidden -fno-exceptions -fno-rtti \
                   -fno-plt -fomit-frame-pointer \
                   -fstack-protector-strong -DNDEBUG
LOCAL_LDFLAGS   += -Wl,--gc-sections -Wl,--icf=safe \
                   -Wl,--strip-all -Wl,-z,now \
                   -Wl,--no-undefined -Wl,--exclude-libs,ALL \
                   -Wl,--version-script=$(LOCAL_PATH)/version_script.map
LOCAL_LDLIBS    := -llog -lselinux   # 无 -ldl
include $(BUILD_SHARED_LIBRARY)
```

### 守护进程（动态引擎）

```makefile
LOCAL_MODULE    := pg_daemon
LOCAL_SRC_FILES := daemon/main.c daemon/watcher.c daemon/mover.c \
                   daemon/queue.c daemon/pid_filter.c \
                   utils/path.c utils/log.c
LOCAL_CFLAGS    += -Os -flto -ffunction-sections -fdata-sections \
                   -fvisibility=hidden -fomit-frame-pointer \
                   -fstack-protector-strong -DNDEBUG
LOCAL_LDFLAGS   += -Wl,--gc-sections -Wl,--strip-all \
                   -Wl,-z,now -static-libgcc
LOCAL_LDLIBS    := -llog
include $(BUILD_EXECUTABLE)
```

```makefile
# Application.mk（两个目标共用）
APP_ABI      := arm64-v8a armeabi-v7a
APP_PLATFORM := android-31   # Android 12+，fanotify FAN_REPORT_PIDFD 最低要求
APP_STL      := none
APP_OPTIM    := release
APP_CFLAGS   := -std=c17
```

---

## 11. 性能与资源目标

| 指标 | 目标 | 保障手段 |
|---|---|---|
| 守护进程 RSS | < 2 MB | 无动态内存分配热路径，纯 C |
| 空闲 CPU | 0% | epoll + eventfd 休眠 |
| fanotify 事件到入队延迟 | < 1 ms | epoll 立即唤醒 + 批量 read |
| 同分区搬运（含 50 ms 窗口）| < 60 ms | rename() 原子，< 1 ms |
| 跨分区搬运 100 MB | ~200 ms @ 500 MB/s | sendfile 4 MB 分块 |
| 页面缓存污染 | 极低 | 逐块 POSIX_FADV_DONTNEED |
| 守护进程二进制大小 | < 50 KB | 纯 C，-Os，strip-all |

---

## 12. 兼容性矩阵

| 条件 | 访问控制 | 静态引擎（`->`）| 动态引擎（`=>`）|
|---|---|---|---|
| Android 12+，GKI 5.10.220 / 5.15 | ✅ | ✅ | ✅ |
| Android 12+，非 GKI（升级机）| ✅ | ✅ | ❌ 静默不启动 |
| Work Profile（userId ≥ 10）| ✅ userId 展开 | ✅ | ✅ |
| Magisk + 内置 Zygisk | ✅ | ✅ | ✅ |
| KSU + ZygiskNext | ✅ | ✅ | ✅ |
| KSU/APatch + NeoZygisk v2.x | ✅ 推荐 | ✅ | ✅ |
| APatch + ReZygisk | ✅ | ✅ | ✅ |
| MIUI/HyperOS（严格 SELinux）| ✅ 三级降级 | ✅ | ✅ |
| OneUI/Knox | ✅（配合 Shamiko）| ✅ | ⚠️ 待验证 |
| 同分区搬运（/data/media）| — | — | ✅ rename 原子 |
| 跨分区搬运（外置 SD 卡）| — | — | ✅ sendfile |

---

## 13. 两引擎协作说明

同一 src 路径同时配置 `->` 和 `=>` 时的行为：

```
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera  （静态）
DCIM/Camera => Pictures/Camera_Archive                   （动态）

结果：
  静态引擎在 preAppSpecialize 把 DCIM/Camera bind 到私有沙盒。
  App 的所有写入实际落在 Android/data/.../cache/Camera（非 DCIM/Camera 底层）。
  fanotify 监控 /data/media/0/DCIM/Camera，由于静态引擎重定向，
  该路径下实际不会有新文件写入，动态引擎不触发。

建议：同一 src 路径只配一种重定向规则，避免歧义。
      -> 适合「双向透明隔离」；=> 适合「强制提取，防止 App 删除」。
```

---

*PathGuard v11 Part 2 · 静态引擎：bind mount + RAII + SELinux 三级降级 · 动态引擎：fanotify FAN_CLASS_NOTIF + FAN_REPORT_DFID_NAME + FAN_REPORT_PIDFD（5.10.220/5.15+）+ 四层并发控制（IOPRIO_CLASS_IDLE + 信号量分级 + POSIX_FADV_DONTNEED + diskstats 监控）+ 守护进程伪装 · Android 12+ GKI*
