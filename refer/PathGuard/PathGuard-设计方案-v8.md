# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v8.0

> **核心目标**：高性能 · 占用小 · 速度快 · 隐蔽性高
>
> **参考**：ZygiskNext v1.3.0 Anonymous Memory + ZN Linker · NeoZygisk v2.x soinfo_unload / atexit evasion · Zygisk-Assistant v2.1.4 FD Reopener · ReZygisk 纯 C + 自定义链接器 · Detecting Shamiko (Medium, 2025-11) · mount_namespaces(7) peer group 传播 · v7.0 全量继承
>
> **v8 变更**：v7 的四项补丁（FD fallback、多用户路径、构建标志、selfCheck）**全部保留**。本版聚焦五项精准补丁，全部来自 2025–2026 年主流模块的真实检测向量：Mount peer group ID 泄漏、soinfo_unload SO 痕迹、`atexit` 注册泄漏、自定义链接器兼容性（ReZygisk/NeoZygisk）、APatch root 方案支持。

---

## v8 相比 v7 的变更一览

| 编号 | 类型 | 问题来源 | 问题 | 修复 |
|---|---|---|---|---|
| 8.1 | 🔴 隐蔽性 | Shamiko 检测报告 (2025-11) | mount peer group ID 可被跨进程关联，暴露隔离操作 | 首次 mount 前执行 `MS_PRIVATE\|MS_REC`，切断 peer group 传播链 |
| 8.2 | 🔴 隐蔽性 | NeoZygisk v2.x / ZygiskNext v1.3.0 | `dl_iterate_phdr` 遍历可发现注入 SO 的 load address | 核心注入完成后立即 `setOption(DLCLOSE_MODULE_LIBRARY)`，配合 soinfo 清理 |
| 8.3 | 🟠 隐蔽性 | NeoZygisk v2.x atexit evasion | `__cxa_atexit` 注册会在进程退出时暴露 SO 残留 | 仅使用 C linkage 导出函数，避免静态全局对象；显式调用 `__cxa_finalize(NULL)` |
| 8.4 | 🟠 兼容性 | ReZygisk v1.0.0 / NeoZygisk v2.x | 自定义链接器生态下，依赖系统链接器 `dlopen`/`dl_iterate_phdr` 行为的代码可能失效 | 彻底移除对系统 `dlopen`/`dlinfo` 的依赖；内部符号解析改用静态链接 |
| 8.5 | 🟢 兼容性 | APatch 用户基数增长 | PathGuard 未显式支持 APatch Zygisk 实现 | 条件编译 `PG_ROOT_APATCH`，适配 APatch uid 策略与模块路径差异 |

其余所有设计（MNT_DETACH 三步流程、零 IPC tmpfs 缓存、memfd fd 传递、FD fallback、Work Profile userId、lexicalNormalize、Trie 匹配、RAII MountGuard、SELinux 三级降级、Ring buffer、Audit 模式、INI 规则格式、黑白名单、`<pkg>` 占位符、XOR 混淆、SIGUSR1 热更新、构建标志 `--no-undefined`/`--exclude-libs,ALL`、selfCheck）**完整继承 v6/v7，不重复**。

---

## 8.1 Mount Peer Group ID 泄漏修复（隐蔽性关键）

### 背景

根据 2025 年 11 月《Detecting Shamiko & Zygisk Root Hiding》（Medium）和 Linux `mount_namespaces(7)` 官方手册，当进程在继承的 mount namespace 中创建新 mount 时，如果父挂载点处于 `shared` 状态（Android 默认），内核会为新 mount 分配一个 peer group ID，该 ID 会出现在 `/proc/self/mountinfo` 的 `optional fields` 中。

**攻击路径**：银行 App 通过 `SCM_RIGHTS` 或 Binder 读取自身 mountinfo，再请求一个可信进程（如 GooglePlay Service 的 isolated 进程）读取其 mountinfo，对比两者的 peer group ID 集合。若目标 App 的 namespace 中出现了不在系统基准 namespace 中的 peer group ID，即可判定有注入操作。

**v7 的问题**：`applyStealthOverlay` 在执行 `mount(tmpfs)` 前未做 `MS_PRIVATE|MS_REC` 隔离。虽然 `MNT_DETACH` 卸载了 source mount，但 bind 操作前的 tmpfs 本身已经产生了 peer group ID（即使短暂），且某些 kernel 版本（尤其是 GKI 2.0 之前）在 `MNT_DETACH` 后仍会在 `/proc/self/mountinfo` 短暂保留该条目。

### 修复

**原则**：在任何 PathGuard 发起 mount 操作之前，先将整个 mount namespace 标记为私有，切断与系统 namespace 的 peer group 传播链。

```c
// mount.c — applyStealthOverlay 第一步（新增，原有逻辑前插入）

// ① 将当前 namespace 中所有挂载点标记为 private
//    这确保后续的 tmpfs/bind 不会产生任何 shared peer group ID
//    也不会把我们的操作传播到父 namespace
static int isolateNamespace(void) {
    // MS_PRIVATE | MS_REC: 递归私有化
    // 参数 NULL 表示当前 namespace 的根，"/" 为锚点
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        // 失败时记录 errno，但不阻断注入（部分 OEM 内核限制此操作）
        LOGE("PG: MS_PRIVATE|MS_REC failed: %s (skipping, may leak peer groups)", strerror(errno));
        return -1;
    }
    return 0;
}

// 调用时机：preAppSpecialize 中，第一次 applyStealthOverlay 之前调用一次
// 之后所有 mount 操作均在私有 namespace 中进行，peer group ID 不外泄
```

**完整 applyStealthOverlay 调用序列（v8 更新）**：

```
preAppSpecialize:
  1. isolateNamespace()          ← v8 新增：切断 peer group 传播
  2. for each rule:
       applyStealthOverlay()     ← v6 原有：tmpfs → bind → MNT_DETACH
  3. setOption(DLCLOSE_MODULE_LIBRARY)  ← v8 8.2 新增
```

**验证**：

```bash
# 在目标 App 进程中检查 mountinfo，期望无任何 shared:N 标签
adb shell "cat /proc/$(pidof com.target.app)/mountinfo | grep shared"
# 期望：无输出（全部为 private mount）
```

---

## 8.2 SO 内存痕迹消除（`DLCLOSE_MODULE_LIBRARY`）

### 背景

NeoZygisk v2.x 和 ZygiskNext v1.3.0 均引入了对 `dl_iterate_phdr` 遍历的防护。根源在于：**Zygisk 模块 SO 加载后，在 `/proc/self/maps` 中永久可见，除非主动请求卸载**。

Appdome 等检测框架（2025 年）通过以下方式发现注入 SO：
- 调用 `dl_iterate_phdr` 枚举所有已加载的共享库
- 扫描 `/proc/self/maps` 中标注为 `r-xp` 的匿名或命名内存段
- 对比系统基准库列表，发现未知路径的 SO

PathGuard SO 一旦在 maps 中可见，其路径（即使被混淆）也可能被 `readlink("/proc/self/fd/N")` 或 `maps` 解析出来。

**ZygiskNext 的解决方案**：`setOption(DLCLOSE_MODULE_LIBRARY)` 通知 Zygisk 运行时在合适时机通过 `dlclose` + `soinfo_unload` 从内存中完全卸载该 SO，使其从 `maps` 和 `dl_iterate_phdr` 中消失。

### 修复

PathGuard 的工作全部在 `preAppSpecialize` 阶段同步完成，无需在 App 运行期间保持 SO 在内存中。因此可以安全地请求卸载。

```c
// module.c — REGISTER_ZYGISK_MODULE 中
static void preAppSpecialize(ZygiskApi *api, JNIEnv *env,
                             AppSpecializeArgs *args) {
    // ... 所有规则加载、路径覆盖、mount 操作 ...

    // v8 新增：核心工作完成后，请求 Zygisk 卸载本 SO
    // 效果：SO 从 /proc/self/maps 和 dl_iterate_phdr 中消失
    // 前提：所有功能必须在此调用前全部完成（mount 已生效，fd 已关闭）
    api->setOption(ZYGISK_DLCLOSE_MODULE_LIBRARY);
}

static void postAppSpecialize(ZygiskApi *api, JNIEnv *env,
                              const AppSpecializeArgs *args) {
    // postAppSpecialize 中不做任何操作
    // SO 已被卸载，此回调不会被调用（模块已不在内存中）
    (void)api; (void)env; (void)args;
}
```

**兼容性注意**：
- Magisk 官方 Zygisk：`ZYGISK_DLCLOSE_MODULE_LIBRARY` 自 v24.2+ 支持
- ZygiskNext v1.3.0+：匿名内存模式下额外隐藏 loader 本身
- ReZygisk v1.0.0+：同样支持，且使用自定义链接器，卸载更彻底
- NeoZygisk v2.x：通过 `soinfo_unload` 实现，效果等价

**热路径影响**：零。`setOption` 是一个标志位设置，不产生系统调用。SO 卸载发生在 preAppSpecialize 返回之后，由 Zygisk 运行时异步处理。

---

## 8.3 `atexit` / `__cxa_atexit` 注册泄漏消除

### 背景

NeoZygisk v2.x 的 changelog 明确提到"atexit evasion"作为独立检测向量：

> "You no longer need to handle atexit evasion. NeoZygisk now completely neutralizes this detection vector for all loaded modules automatically."

**检测机制**：当 SO 中存在静态全局对象（C++ 全局变量、函数静态变量、全局 `std::string`/`std::vector` 等）时，链接器会在 SO 加载时调用 `__cxa_atexit` 注册析构函数。这些注册条目在系统的 atexit 链表中可见，即使 SO 被 `dlclose`，条目本身（包含函数指针）仍可能残留，暴露 SO 曾加载过的痕迹。

**v7 的潜在问题**：`companion.cpp` 中使用了 C++ STL（`std::string` 等），可能产生静态全局对象的 atexit 注册。

### 修复

**策略一：消灭静态全局对象（根本方案）**

```c
// 规则：PathGuard 的所有 C++ 代码中，禁止出现：
// ❌ static std::string g_str = "hello";       // 产生 atexit 注册
// ❌ static std::vector<int> g_vec;             // 产生 atexit 注册
// ❌ static MyClass g_obj;                      // 产生 atexit 注册（若有析构函数）

// ✅ 改用 POD 或延迟初始化
static const char G_PREFIX[] = "pg_";            // 字符串字面量，无 atexit
static AppPolicyPOD *g_policy_ptr = NULL;        // 指针，无 atexit

// ✅ 函数内 static 但无析构函数
static int g_count = 0;                          // trivially destructible，无 atexit
```

**策略二：`-fno-exceptions` + `-fno-rtti`（v7 已有，此处确认其副作用）**

`-fno-exceptions` 消除了异常处理表中的 atexit 注册。v7 的 `LOCAL_CFLAGS` 已包含此标志，继续保留。

**策略三：显式调用 `__cxa_finalize`（保险层）**

```c
// module.c — preAppSpecialize 末尾，setOption(DLCLOSE) 之前
// 清空当前 SO 注册的所有 atexit 回调
extern void __cxa_finalize(void *dso_handle);
// __dso_handle 是链接器自动定义的符号，指向当前 SO
extern void *__dso_handle;

// 在 SO 卸载前主动清理，避免任何残留
__cxa_finalize(__dso_handle);
```

**构建验证**：

```bash
# 检查是否有意外的 atexit 注册
nm zygisk/arm64-v8a.so | grep -E "cxa_atexit|__dso_handle"
# 期望：只有 __dso_handle（链接器必需），无其他 cxa_atexit 引用
```

---

## 8.4 自定义链接器兼容性（ReZygisk / NeoZygisk）

### 背景

根据 2026 年 Android 社区实测，以下 Zygisk 实现均已转向**自定义链接器**：

- **ReZygisk v1.0.0**（PerformanC）：纯 C 重写，使用自定义链接器，"不在正常情况下使用系统链接器"，彻底规避链接器特征检测
- **NeoZygisk v2.x**（JingMatrix）：自定义 `soinfo_unload`，绕过系统 `dlclose` 的限制
- **ZygiskNext v1.3.0+**（Dr-TSNG）：内置 ZN Linker，模块加载后 `dl_iterate_phdr` 中的信息由 ZN 内部实现管理，而非系统链接器

**兼容性风险**：PathGuard 若内部使用 `dlopen` / `dladdr` / `dl_iterate_phdr` 进行自检或符号查找，在上述自定义链接器环境下可能返回错误结果或直接失败。

### 修复

**原则：PathGuard 不依赖系统链接器的任何运行时 API。**

```c
// ❌ 禁止在模块运行时使用（v8 明确排除）：
void *handle = dlopen("libselinux.so", RTLD_LAZY);   // 系统链接器
void *sym = dlsym(handle, "setcon");                  // 系统链接器
int r = dl_iterate_phdr(callback, NULL);              // 系统链接器，自定义环境下行为未定义

// ✅ 改用直接静态链接或 NDK 保证的 syscall：
// libselinux 已在 Android.mk 中 LOCAL_LDLIBS := -lselinux
// 直接调用 setcon()，无需 dlopen
extern int setcon(const char *context);
setcon("u:r:zygote:s0");
```

**selfCheck 中的 `dl_iterate_phdr` 调用**（v7 8.4 中有间接依赖）：

```c
#ifdef PG_DEBUG
// v7 selfCheck 中检查导出符号的代码需要改写
// ❌ 旧：通过 dl_iterate_phdr 枚举（在自定义链接器下可能漏报）
// ✅ 新：直接读 /proc/self/maps 进行文本匹配，更可靠
static void checkExportedSymbols(void) {
    // 读 /proc/self/maps，查找本 SO 的路径条目
    // 再用 readelf 离线验证（selfCheck 只在 debug 构建中执行）
    char maps[4096];
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, maps, sizeof(maps) - 1);
    close(fd);
    if (n > 0) {
        maps[n] = '\0';
        LOGI("PG selfCheck maps (partial): %.512s", maps);
    }
}
#endif
```

**Application.mk 确认（不引入 STL 链接器依赖）**：

```makefile
APP_STL := none   # 继承 v6/v7，确保不链接 libc++ 或 libstdc++
```

---

## 8.5 APatch 兼容支持

### 背景

APatch（2024–2026 年增长迅速的第三类 root 方案）使用与 Magisk/KernelSU 不同的：
- 模块路径：`/data/adb/apatch/modules/` vs Magisk 的 `/data/adb/modules/`
- uid 策略：部分版本不使用 Magisk 的 `magisk_manager` uid 分配机制
- Zygisk 实现：通过 ZygiskNext 或 ReZygisk 加载，API 层面兼容，但 Companion 路径约定略有差异

### 修复

**条件编译策略**：不引入运行时分支，仅在构建时通过宏区分路径常量。

```c
// utils/compat.h — 新增兼容层头文件
#pragma once

// 构建时通过 -DPG_ROOT_MAGISK / -DPG_ROOT_KSU / -DPG_ROOT_APATCH 选择
// 默认为 Magisk（与 v6/v7 保持向后兼容）
#if defined(PG_ROOT_APATCH)
    #define PG_MODULE_BASE   "/data/adb/apatch/modules/path_guard"
    #define PG_RULES_PATH    PG_MODULE_BASE "/config/rules.ini"
    #define PG_CACHE_BASE    "/dev/.__sys"   // 与 Magisk 路径相同（tmpfs，无差异）
#elif defined(PG_ROOT_KSU)
    #define PG_MODULE_BASE   "/data/adb/modules/path_guard"   // KSU 与 Magisk 相同
    #define PG_RULES_PATH    PG_MODULE_BASE "/config/rules.ini"
    #define PG_CACHE_BASE    "/dev/.__sys"
#else
    // 默认：Magisk（向后兼容 v6/v7）
    #define PG_MODULE_BASE   "/data/adb/modules/path_guard"
    #define PG_RULES_PATH    PG_MODULE_BASE "/config/rules.ini"
    #define PG_CACHE_BASE    "/dev/.__sys"
#endif
```

**Android.mk 扩展**（多目标构建）：

```makefile
# 构建三个变体，安装包中包含三个 SO，由 customize.sh 在安装时选择正确的一个
# 注意：三个变体 API 完全相同，仅路径常量不同，无性能差异

# 在 customize.sh 中：
# if [ "$ROOT_SOL" = "apatch" ]; then cp zygisk_apatch/arm64-v8a.so zygisk/arm64-v8a.so; fi
```

**热路径影响**：零。路径常量在编译时确定，运行时无条件分支。

---

## 8.6 v8 隐蔽性综合评估

根据 2025 年《Detecting Shamiko》报告中列出的检测维度，v8 完整覆盖情况：

| 检测维度 | 检测手段 | v7 状态 | v8 状态 |
|---|---|---|---|
| Mount namespace mismatch | 对比 App 与系统进程 mountinfo | ⚠️ peer group ID 可能泄漏 | ✅ MS_PRIVATE\|MS_REC 切断 |
| OverlayFS / bind mount 异常 | 扫描 mountinfo 中非标准条目 | ✅ MNT_DETACH 卸载 source | ✅ 继承，无变化 |
| `/proc/self/maps` SO 痕迹 | dl_iterate_phdr / maps 扫描 | ⚠️ SO 永久可见 | ✅ DLCLOSE_MODULE_LIBRARY |
| atexit 注册残留 | atexit 链表检查 | ⚠️ C++ 全局对象可能注册 | ✅ 无全局对象 + cxa_finalize |
| 导出符号 / nm 扫描 | nm -D / readelf -s | ✅ ≤2 个导出符号 | ✅ 继承，无变化 |
| 敏感字符串 | strings 扫描 | ✅ XOR 混淆 | ✅ 继承，无变化 |
| PID namespace 不一致 | /proc/PID/ns/mnt inode 对比 | ✅ Zygisk 原生 namespace | ✅ isolateNamespace 后 inode 仍一致 |
| 系统链接器特征 | dl_iterate_phdr / linker maps | ⚠️ 依赖系统链接器 | ✅ 移除所有 dlopen 依赖 |

---

## 8.7 兼容性测试顺序（v8 更新）

**第一优先级**（v8 新增验证项）：

```
[ ] isolateNamespace() 后 mountinfo 无 shared:N 条目（Pixel 8 / Android 15）
[ ] DLCLOSE 后 /proc/self/maps 中无 path_guard SO 路径（任意测试 App）
[ ] nm/strings 扫描：无 __cxa_atexit 符号引用
[ ] ReZygisk v1.0.0 + KernelSU 下模块正常加载（自定义链接器兼容）
[ ] APatch 安装包自动选择正确 SO 变体
```

**第二优先级**（继承 v7 验证项）：

```
[ ] Android 14/15 冷启耗时 < 500μs（50 App，新增 isolateNamespace 不超过 50μs）
[ ] Work Profile userId=10 规则生效
[ ] FD fallback 触发验证
[ ] Shamiko 共存下银行 App 通过检测
[ ] selfCheck() 报告完整
```

**第三优先级**（回归）：

```
[ ] 极端低内存设备 2GB RAM：Companion RSS < 200KB
[ ] MIUI HyperOS / ColorOS SELinux 严格 ROM
[ ] ZygiskNext + NeoZygisk + ReZygisk 三种 Zygisk 实现下均正常
[ ] 多用户切换后 SIGUSR1 热更新
```

---

## 完整 Android.mk（v8 最终版）

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
    utils/selinux.c     \
    utils/compat.h      # 头文件，不参与编译，仅供包含

LOCAL_CFLAGS += \
    -Os                         \
    -flto                       \
    -ffunction-sections         \
    -fdata-sections             \
    -fvisibility=hidden         \
    -fno-exceptions             \
    -fno-rtti                   \
    -fno-plt                    \
    -fomit-frame-pointer        \
    -fstack-protector-strong    \
    -DNDEBUG                    \
    -DPG_ROOT_MAGISK            # 默认 Magisk；APatch 构建时改为 -DPG_ROOT_APATCH

# Debug 构建时追加：-DPG_DEBUG
# Release 构建时不加，selfCheck() + 调试代码完全剔除

LOCAL_LDFLAGS += \
    -Wl,--gc-sections                                           \
    -Wl,--icf=safe                                              \
    -Wl,--strip-all                                             \
    -Wl,-z,now                                                  \
    -Wl,--no-undefined                                          \
    -Wl,--exclude-libs,ALL                                      \
    -Wl,--version-script=$(LOCAL_PATH)/version_script.map

LOCAL_LDLIBS := -llog -lselinux
# 注意：不链接 -ldl（移除所有 dlopen 依赖，见 8.4）

include $(BUILD_SHARED_LIBRARY)
```

```makefile
# Application.mk（v8 最终版，与 v7 相同）
APP_ABI      := arm64-v8a armeabi-v7a x86_64 x86
APP_PLATFORM := android-26
APP_STL      := none
APP_OPTIM    := release
APP_CFLAGS   := -std=c17
```

---

## 构建后验证脚本（v8 扩展版）

```bash
#!/bin/bash
# verify_build.sh — v8 一键验证
SO="zygisk/arm64-v8a.so"

echo "=== 1. SO 大小 ==="
ls -lh "$SO"
# 期望：< 44KB（与 v7 相同，新增代码均为小函数）

echo "=== 2. 导出符号 ==="
nm -D "$SO" | grep ' T ' | awk '{print $3}'
# 期望：zygisk_module_entry, zygisk_companion_entry（仅 2 个）

echo "=== 3. atexit 相关符号（应为空或仅 __dso_handle）==="
nm "$SO" | grep -E "cxa_atexit|cxa_finalize" | grep -v "__dso_handle"
# 期望：无输出（无 __cxa_atexit 引用）

echo "=== 4. dlopen/dl_iterate_phdr 依赖（应为空）==="
nm "$SO" | grep -E "dlopen|dlsym|dl_iterate_phdr|dladdr"
# 期望：无输出（8.4 移除所有系统链接器 API 依赖）

echo "=== 5. 未定义符号（应为空）==="
nm "$SO" | grep ' U ' | grep -v "^$"
# 期望：无输出（--no-undefined 已确保）

echo "=== 6. 敏感字符串 ==="
strings "$SO" | grep -iE "pathguard|path_guard|storage|sdcard|/data/adb"
# 期望：无输出（XOR 混淆）

echo "=== 7. 依赖库（不应有 libdl）==="
readelf -d "$SO" | grep NEEDED
# 期望：liblog.so, libselinux.so（无 libdl.so，无 libc++）

echo "=== 8. 重定位数量 ==="
readelf --relocs "$SO" | grep -c RELA
# 期望：< 200
```

---

## 目标指标汇总（v7 → v8）

| 指标 | v7 目标 | v8 目标 | 变化来源 |
|---|---|---|---|
| arm64 SO 大小 | < 44KB | **< 44KB**（持平）| 新增代码 <200 行，LTO 抵消 |
| 导出符号数 | ≤ 2 | **≤ 2**（持平）| 无变化 |
| peer group ID 泄漏 | 可能存在 | **0**（MS_PRIVATE\|MS_REC）| 8.1 |
| SO 在 maps 中可见 | 永久可见 | **卸载后不可见** | 8.2 DLCLOSE |
| atexit 注册残留 | 可能存在 | **0** | 8.3 |
| 系统链接器 API 依赖 | 有（dlopen 内部） | **0** | 8.4 |
| APatch 支持 | 无 | **有（条件编译）** | 8.5 |
| 检测维度覆盖率 | 6/8 | **8/8** | 8.1–8.4 |
| preAppSpecialize 耗时 | < 500μs | **< 550μs**（isolateNamespace 增加约 30–50μs）| 8.1 单次 mount 系统调用 |

---

## 继承自 v6/v7 的完整设计（不重复展开）

以下所有设计在 v8 中**原样保留**：

- MNT_DETACH 三步隐蔽流程（tmpfs→bind→立即卸载 source）
- 零 IPC tmpfs 规则缓存（`/dev/.__sys_{rnd}/{sha256[:16]}.bin`）
- `memfd_create` 匿名 fd + SCM_RIGHTS 传递（主路径）+ file fallback（v7 8.1）
- `lexicalNormalize()` 纯内存路径规范化
- Trie 前缀树匹配（O(path_length)）
- 栈分配热路径（preAppSpecialize 零 `malloc`）
- SELinux 三级降级（setfscreatecon → bind template → skip）
- RAII MountGuard（任何异常路径自动回滚）
- AppPolicyPOD 固定大小结构（直接 read() 到栈）
- INI 规则格式（`+/-/->` + `<pkg>` 占位符 + mode=blacklist/whitelist）
- Work Profile 完整路径覆盖（userId 严格传播，v7 8.2）
- Ring buffer 异步日志
- Audit 模式（`log=debug` + `audit=true`）
- version_script.map 零导出
- XOR 字符串混淆
- SIGUSR1 热更新
- `-fno-plt` + `-Wl,-z,now`
- `__builtin_expect` 分支预测提示
- `--no-undefined` + `--exclude-libs,ALL`（v7 8.3）
- Debug selfCheck()（v7 8.4，Release 零开销）

---

*PathGuard Technical Design v8.0 · 五项精准补丁：Mount peer group ID 隔离（MS_PRIVATE\|MS_REC）· SO 内存痕迹消除（DLCLOSE_MODULE_LIBRARY）· atexit 注册清零（无全局对象 + cxa_finalize）· 自定义链接器兼容（移除 dlopen 依赖）· APatch 条件编译支持 · 继承 v6/v7 全量设计 · 检测维度覆盖率 8/8*
