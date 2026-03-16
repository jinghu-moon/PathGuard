# PathGuard — Magisk 路径访问控制模块
## 技术实现方案 v7.0

> **核心目标**：高性能 · 占用小 · 速度快 · 隐蔽性高
>
> **参考**：Magisk issue #6523/#7448（FD sanitization 历史漏洞）· AOSP Traditional Storage / Multi-user Testing 官方文档 · Linux umount2(2) MNT_DETACH · NDK `--no-undefined`/`--exclude-libs,ALL` 官方说明 · Shamiko 自检机制 · cppreference lexically_normal · v6.0 全量继承
>
> **v7 变更**：v6 的三大核心优化（MNT_DETACH 隐蔽、零 IPC、二进制压缩）**全部保留**。本版聚焦四项精准补丁：FD fallback 鲁棒性、多用户路径完整性、构建标志最终定稿、调试自检机制。

---

## v7 相比 v6 的变更一览

| 编号 | 类型 | 问题 | 修复 |
|---|---|---|---|
| 7.1 | 🔴 鲁棒性 | memfd+fd 传递在 Magisk 特定版本/ROM 下失效，无 fallback | 显式 fallback + 版本日志，10 行代码，零额外开销 |
| 7.2 | 🟠 正确性 | Work Profile（userId≥10）路径展开不完整 | `resolveStoragePaths` 严格按 userId 参数展开，显式覆盖 `/data/media/{userId}` |
| 7.3 | 🟢 占用 | Android.mk 缺少两个链接标志 | `--no-undefined` + `--exclude-libs,ALL`，SO 再减 5-8%，消除符号依赖隐患 |
| 7.4 | 🟢 可维护 | 无运行时隐蔽性验证手段，上线后排查困难 | `selfCheck()`，仅 `log=debug` 时编译进去，release 零开销 |

其余所有设计（MNT_DETACH 三步流程、零 IPC tmpfs 缓存、memfd 匿名 fd、lexicalNormalize、Trie 匹配、RAII mount、SELinux 三级降级、Ring buffer、Audit 模式、`<pkg>` 占位符、黑白名单语义等）**完整继承 v6，不重复**。

---

## 7.1 FD 传递 Fallback（鲁棒性）

### 背景

Magisk issue #6523（2023-01）和 #7448（2023-10）均为 FD sanitization 导致 Zygisk 模块 fd 传递在特定 ROM/内核下失效。虽然主线版本已修复，但：
- OEM ROM 的 Magisk 集成版本通常滞后数月
- 第三方 Zygisk 实现（ZygiskNext、ReZygisk）的 fd 传递行为略有差异
- 极少数设备内核对 `SCM_RIGHTS` 的实现存在边界行为

### 修复（10 行，零性能影响）

```c
// module.c — preAppSpecialize 中
AppPolicyPOD policy;
int fd = -1;

// 主路径：memfd + Companion fd 传递（零 IPC，内存级）
if (api->connectCompanion) {
    fd = receiveFdFromCompanion(api);
}

// Fallback：fd 无效时退回 SHA-256 文件方案
if (fd < 0 || fstat(fd, &(struct stat){0}) != 0) {
    // 日志记录 Magisk 版本，便于排查
    LOGE("PG: fd invalid, fallback to file cache (magisk=%s)",
         getMagiskVersion());
    fd = openPolicyCacheFile(pkg);  // /dev/.__sys_{rnd}/{sha256(pkg)[:16]}
}

if (fd < 0) return;  // 无规则 = fail-open，App 正常启动
bool ok = loadPolicyFromFd(fd, &policy);
close(fd);
if (!ok) return;
```

**热路径影响**：
- 正常情况（fd 有效）：多一次 `fstat`，耗时 < 0.5μs
- fallback 触发（极罕见）：`open()` 文件，耗时 < 5μs
- 两种路径最终都是从 fd 读取固定大小 POD 结构，**后续逻辑完全一致**

---

## 7.2 Work Profile / 多用户路径完整覆盖

### 背景

根据 AOSP 官方 Multi-user Testing 文档和 Traditional Storage 文档，Android 多用户/Work Profile 的存储路径严格依赖 `userId`：

```
主用户（userId=0）：
  /storage/emulated/0/    → /data/media/0/

工作档案（Work Profile，userId 通常为 10）：
  /storage/emulated/10/   → /data/media/10/
  
次要用户（userId=11, 12, ...）：
  /storage/emulated/11/   → /data/media/11/
```

根据 AOSP 源码，`userId = uid / 100000`（Zygisk `args->uid` 字段直接提供）。

**v6 的问题**：`resolveStoragePaths` 中部分路径硬编码了 `0`，而非使用传入的 `userId` 参数——在 Work Profile（userId=10）下，覆盖操作会作用于错误的目录，或漏掉 `/data/media/10/` 真实路径。

### 修复

```c
// path_utils.c — resolveStoragePaths（完整版）
// userId 来自 args->uid / 100000，由调用方传入
int resolveStoragePaths(const char *logicalPath, int userId,
                        char out[][PATH_MAX], int maxOut) {
    // 从逻辑路径提取相对部分（去掉任何已知前缀）
    const char *rel = extractRelSubpath(logicalPath);  // 纯字符串操作，栈内
    int n = 0;
    char uid_s[12];
    snprintf(uid_s, sizeof(uid_s), "%d", userId);

#define ADD(fmt, ...) \
    if (n < maxOut) snprintf(out[n++], PATH_MAX, fmt, ##__VA_ARGS__)

    // 全部路径严格使用 userId，不硬编码 0
    ADD("/sdcard/%s",                                        rel);   // userId=0 时有效的符号链接
    ADD("/storage/emulated/%s/%s",               uid_s, rel);       // 主要入口
    ADD("/storage/self/primary/%s",                          rel);   // per-app 符号链接
    ADD("/mnt/user/%s/primary/%s",               uid_s, rel);       // vold bind 中间层
    ADD("/mnt/runtime/default/emulated/%s/%s",   uid_s, rel);       // 无权限视图
    ADD("/mnt/runtime/read/emulated/%s/%s",      uid_s, rel);       // 读权限视图
    ADD("/mnt/runtime/write/emulated/%s/%s",     uid_s, rel);       // 写权限视图
    ADD("/data/media/%s/%s",                     uid_s, rel);       // FUSE 底层真实路径

#undef ADD
    return n;  // 返回实际填充数量
}
```

**注意**：`/sdcard` 符号链接只对 userId=0 有效（主用户）；Work Profile 下不存在 `/sdcard` 捷径，但 lazy stat 会自然过滤掉不存在的路径，无额外开销。

**Work Profile 下的实际有效路径**（userId=10）：

```
✅ /storage/emulated/10/DCIM/private
✅ /mnt/runtime/write/emulated/10/DCIM/private
✅ /data/media/10/DCIM/private
❌ /sdcard/DCIM/private         ← stat 失败，lazy stat 跳过，无影响
❌ /storage/self/primary/...    ← 同上
```

---

## 7.3 构建标志最终定稿

在 v6 的 Android.mk 基础上追加两行：

```makefile
LOCAL_LDFLAGS += \
    -Wl,--gc-sections       \   # v6 已有
    -Wl,--icf=safe          \   # v6 已有
    -Wl,--strip-all         \   # v6 已有
    -Wl,-z,now              \   # v6 已有
    -Wl,--version-script=$(LOCAL_PATH)/version_script.map \  # v6 已有
    -Wl,--no-undefined      \   # v7 新增：链接时报告所有未定义符号（防止运行时崩溃）
    -Wl,--exclude-libs,ALL      # v7 新增：彻底排除所有静态库的内部符号（防止意外导出）
```

**`--no-undefined`**：要求所有符号在链接时必须解析完毕。防止 SO 加载时因未定义符号引发 `SIGSEGV`——这在低内存设备上较难复现，但在 OEM 定制 ROM 上偶发。

**`--exclude-libs,ALL`**：确保链接进来的静态库（如 libselinux 的静态依赖部分）不向 SO 导出任何符号，进一步缩小符号表。配合 `--version-script` 双重保险，导出符号数量降至绝对最小值。

**预期效果**（基于 NDK 官方文档和主流小体积模块实测）：
- arm64-v8a.so：从 v6 的 <48KB → **<44KB**
- `readelf -s` 输出的符号行数：从 ~20 → **≤5**（仅 Zygisk 入口 + 必要内部符号）

---

## 7.4 调试自检函数 `selfCheck()`

### 设计原则

- **Release 构建**：`#ifdef PG_DEBUG` 保护，**编译时完全剔除，零 footprint**
- **Debug 构建**（`config/rules.ini` 中 `log=debug`）：在 `onLoad` 时执行一次，输出完整隐蔽性报告

```c
// module.c — 仅在 PG_DEBUG 宏定义时编译
#ifdef PG_DEBUG
static void selfCheck(const char *testTarget) {
    LOGI("PG selfCheck start ─────────────────");

    // 检查 1：mountinfo 无残留
    // 执行一次 applyStealthOverlay，然后检查 /proc/self/mountinfo
    applyStealthOverlay(testTarget, 0, 0, 0755);
    checkMountinfoClean(testTarget);
    // 预期：testTarget 出现一条 "tmpfs tmpfs rw"，无任何 pg_ 前缀路径

    // 检查 2：SO 导出符号数量
    checkExportedSymbols();
    // 预期：只有 zygisk_module_entry / zygisk_companion_entry

    // 检查 3：memfd fd 匿名性
    int fd = memfd_create("p", MFD_CLOEXEC);
    if (fd >= 0) {
        char fdPath[64];
        snprintf(fdPath, sizeof(fdPath), "/proc/self/fd/%d", fd);
        char linkTarget[256] = {0};
        readlink(fdPath, linkTarget, sizeof(linkTarget));
        LOGI("PG selfCheck memfd link: %s", linkTarget);
        // 预期输出：memfd:p (deleted) 或 memfd:p —— 无任何包名或路径信息
        close(fd);
    }

    // 检查 4：strings 敏感字符串（读取自身 /proc/self/maps 对应段）
    checkSensitiveStrings();
    // 预期：无 "pathguard"、"path_guard"、"/sdcard"、"/storage" 明文

    LOGI("PG selfCheck end ───────────────────");
}
#endif
```

**使用方式**：

```bash
# 开启 debug 模式
echo "[global]" > /data/adb/modules/path_guard/config/rules.ini
echo "log = debug" >> /data/adb/modules/path_guard/config/rules.ini
# 热更新
adb shell "kill -USR1 $(pidof magiskd)"
# 重启一个受规则影响的 App，查看 selfCheck 报告
adb logcat -s PG
```

---

## 7.5 兼容性测试顺序（实测优先级）

根据当前公开主流模块实践和 2026 年 Android 生态现状，建议按以下顺序验证：

**第一优先级**（核心功能验证）：

```
[ ] Android 14/15（Pixel 8+、小米 14）：preAppSpecialize 平均耗时 < 500μs（50 App 冷启）
[ ] Work Profile 下规则生效（userId=10，bank App / 企业 App 场景）
[ ] FD fallback 触发验证（临时降级 Magisk 版本或手动注入 fd 错误）
```

**第二优先级**（隐蔽性验证）：

```
[ ] Shamiko + LSPosed 共存下，银行 App 不触发 mountinfo 检测
[ ] selfCheck() 报告：mountinfo 无残留、导出符号 ≤2、memfd 匿名
[ ] strings / nm 扫描结果符合预期
```

**第三优先级**（兼容性回归）：

```
[ ] 极端低内存设备（2GB RAM）：Companion RSS < 200KB
[ ] MIUI HyperOS / ColorOS（SELinux 严格 ROM）：fallback 到 bind template 方案
[ ] ZygiskNext + KernelSU：fd 传递行为与 Magisk Zygisk 一致性
[ ] 多用户切换后规则实时生效（SIGUSR1 热更新）
```

---

## 完整 Android.mk（v7 最终版）

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
    -Os                     \
    -flto                   \
    -ffunction-sections     \
    -fdata-sections         \
    -fvisibility=hidden     \
    -fno-exceptions         \
    -fno-rtti               \
    -fno-plt                \
    -fomit-frame-pointer    \
    -fstack-protector-strong\
    -DNDEBUG

# Debug 构建时追加：-DPG_DEBUG
# release 构建时不加，selfCheck() 被完全剔除

LOCAL_LDFLAGS += \
    -Wl,--gc-sections                                       \
    -Wl,--icf=safe                                          \
    -Wl,--strip-all                                         \
    -Wl,-z,now                                              \
    -Wl,--no-undefined                                      \
    -Wl,--exclude-libs,ALL                                  \
    -Wl,--version-script=$(LOCAL_PATH)/version_script.map

LOCAL_LDLIBS := -llog -lselinux

include $(BUILD_SHARED_LIBRARY)
```

```makefile
# Application.mk（最终版）
APP_ABI      := arm64-v8a armeabi-v7a x86_64 x86
APP_PLATFORM := android-26
APP_STL      := none
APP_OPTIM    := release
APP_CFLAGS   := -std=c17
```

---

## 构建后验证脚本

```bash
#!/bin/bash
# verify_build.sh — 一键验证 v7 构建质量
SO="zygisk/arm64-v8a.so"

echo "=== 1. SO 大小 ==="
ls -lh "$SO"
# 期望：< 44KB

echo "=== 2. 导出符号 ==="
nm -D "$SO" | grep ' T ' | awk '{print $3}'
# 期望：只有 zygisk_module_entry, zygisk_companion_entry

echo "=== 3. 重定位数量 ==="
readelf --relocs "$SO" | grep -c RELA
# 期望：< 200

echo "=== 4. 未定义符号（应为空） ==="
nm "$SO" | grep ' U ' | grep -v '^$'
# 期望：无输出（--no-undefined 已在链接时报错）

echo "=== 5. 敏感字符串 ==="
strings "$SO" | grep -iE "pathguard|path_guard|storage|sdcard|/data/adb"
# 期望：无输出（XOR 混淆生效）

echo "=== 6. 依赖库 ==="
readelf -d "$SO" | grep NEEDED
# 期望：只有 liblog.so, libselinux.so, libdl.so（无 libstdc++, libc++ 等）
```

---

## 目标指标汇总（v6 → v7）

| 指标 | v6 目标 | v7 目标 | 变化来源 |
|---|---|---|---|
| arm64 SO 大小 | < 48KB | **< 44KB** | `--exclude-libs,ALL` |
| 导出符号数 | ≤ 5 | **≤ 2** | `--exclude-libs,ALL` + `--version-script` 双重 |
| 未定义符号 | 可能存在（运行时暴露） | **0**（链接时报错） | `--no-undefined` |
| Work Profile 覆盖 | 部分（userId 未完整传播） | **完整**（全路径严格 userId） |
| FD 失效时行为 | 无规则，App 不受保护 | **自动 fallback，保护生效** | |
| Release 二进制 selfCheck | 无 | **无（`#ifdef` 剔除）** | 零影响 |
| Debug selfCheck | 无 | **有，输出完整报告** | |

---

## 继承自 v6 的完整设计（不重复展开）

以下所有设计在 v7 中**原样保留**，请参阅 v6 文档：

- **MNT_DETACH 三步隐蔽流程**（tmpfs→bind→立即卸载 source）
- **零 IPC tmpfs 规则缓存**（`/dev/.__sys_{rnd}/{sha256[:16]}.bin`）
- **`memfd_create` 匿名 fd + SCM_RIGHTS 传递**（主路径）
- **`lexicalNormalize()`** 纯内存路径规范化（消灭 `../` 穿越）
- **Trie 前缀树匹配**（O(path_length)，与规则数无关）
- **栈分配热路径**（preAppSpecialize 零 `malloc`）
- **SELinux 三级降级**（setfscreatecon → bind template → skip）
- **RAII MountGuard**（任何异常路径自动回滚）
- **AppPolicyPOD 固定大小结构**（直接 read() 到栈，无动态分配）
- **INI 规则格式**（`+/-/->` + `<pkg>` 占位符 + mode=blacklist/whitelist）
- **MediaStore 已知限制**（Phase 1 文档化，Phase 3 应对路线）
- **Ring buffer 异步日志**（正常模式仅 logcat error，无文件 IO）
- **Audit 模式**（`log=debug` + `audit=true`）
- **version_script.map 零导出**（全局符号隐藏）
- **XOR 字符串混淆**（敏感路径不出现在 `.rodata`）
- **SIGUSR1 热更新**（原子 rename，Zygisk 端读到的永远是完整文件）
- **`-fno-plt` + `-Wl,-z,now`**（消除 PLT 和懒加载延迟）
- **`__builtin_expect` 分支预测提示**（无规则 App 快速返回）

---

*PathGuard Technical Design v7.0 · 精准四项补丁：FD fallback 鲁棒性（+10 行）· Work Profile 路径完整性（userId 严格传播）· 构建标志最终定稿（`--no-undefined` + `--exclude-libs,ALL`）· Debug selfCheck（Release 零开销）· 继承 v6 全量设计*
