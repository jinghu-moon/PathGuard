# Folder Manager Magisk Module

这是一个基于 `Magisk + Zygisk + C++` 的原生模块工程。
仅支持 **Android 12 及以上（API 31）**，安装时会做版本校验。

当前阶段目标（人话版）：

1. 按应用拦截指定路径访问
2. 对指定路径执行文件或目录重定向
3. 保持模块结构简单，优先保证可编译、可调试、可迭代

## 当前实现状态（概览）

- 模块外壳已经就绪
- 阶段二已切换为 `C++` 原生方案
- 已加入首版 `Zygisk` 骨架
- 已加入规则解析与路径决策逻辑
- 已加入 `ndk-build` 构建脚本与打包流程

## 目录结构

```
PathGuard/
├── README.md
├── module/                      # Magisk 模块运行时文件
│   ├── module.prop
│   ├── customize.sh
│   ├── post-fs-data.sh
│   ├── service.sh
│   ├── boot-completed.sh
│   ├── action.sh
│   ├── uninstall.sh
│   ├── skip_mount
│   ├── config/rules.ini         # 规则配置文件
│   ├── bin/{abi}/               # 守护进程二进制
│   └── zygisk/{abi}.so          # Zygisk 模块库
│
├── native/                      # C++ 源码
│   ├── Android.mk
│   ├── Application.mk
│   ├── CMakeLists.txt
│   ├── folder_manager.cpp       # Zygisk 模块与 Hook 主逻辑
│   ├── folder_manager_daemon.cpp
│   ├── rule_engine.cpp/h        # 规则匹配引擎
│   ├── rule_config.cpp/h        # 规则解析
│   ├── path_mapper.cpp/h        # 路径重定向
│   ├── path_kind_cache.cpp/h
│   ├── daemon_utils.cpp/h
│   ├── runtime_context.h
│   └── zygisk.hpp               # 官方 Zygisk API
│
├── tests/                       # 单元测试
├── benchmarks/                  # 性能基准
├── tools/rule_cli.cpp           # 规则 CLI 调试工具
│
├── Manager-GUI/                 # Android 管理 App
├── scripts/                     # 构建脚本
│   ├── build.ps1                # 完整模块打包
│   └── build-native.ps1         # 原生库构建
│
├── docs/                        # 设计文档
└── dist/                        # 构建产物（gitignore）
```

## 规则格式（最常用写法）

规则文件位置：`config/rules.ini`

```ini
[com.example.app]
mode = blacklist
enabled = true

- Download/secret
DCIM/Camera -> Android/data/<pkg>/cache/Camera
DCIM/WeChat => Pictures/WeChat_Archive
```

## 当前原生拦截范围（已覆盖）

当前首版骨架已覆盖以下常见文件访问入口：

- `open`
- `open64`
- `__open_2`
- `openat`
- `__openat_2`
- `fopen`
- `fopen64`
- `access`
- `faccessat`
- `stat`
- `lstat`
- `stat64`
- `lstat64`
- `fstatat`
- `fstatat64`
- `statx`
- `opendir`
- `scandir`
- `readlink`
- `readlinkat`
- `realpath`

另外，当前版本会在目标应用进程内对 `android.os.BinderProxy.transactNative` 做 JNI Hook，
对发往 `MediaStore` 的查询追加排除条件，尽量把命中 `block` 规则的目录从列表层隐藏。

## 设计取舍（为什么这么做）

### 为什么直接用 C++

- `Zygisk` 官方 API 是 `C++` 头文件接口
- 官方 sample 使用 `ndk-build + C++`
- 直接做 Hook、ABI 适配、符号处理更自然
- 调试链路更短，问题定位更直接

### 为什么当前不引入 Rust

- 当前目标是先把原生拦截链路跑通
- 如果先上 Rust，会额外引入 FFI、panic、链接与 ABI 复杂度
- 对当前阶段属于明显的非必要复杂化

这符合 `KISS` 与 `YAGNI`。

## 构建

### 1. 构建原生库

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native.ps1
```

构建完成后产物复制到：

- `module/zygisk/{abi}.so`
- `module/bin/{abi}/folder_manager_daemon`

### 2. 打包完整模块

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

输出 zip 位于 `dist/`。

## Manager GUI

- 工程位置：`Manager-GUI/`
- 版本：0.2.0（versionCode 2）
- 功能：编辑 `rules.ini`、触发热重载、查看日志、导入导出规则、语法校验
- 技术栈：Kotlin + AndroidX Compose + Material3（Geist 配色）
- 最低支持：Android 12（API 31）
- 构建产物：`Manager-GUI/app/build/outputs/apk/release/`（4 个 ABI 分包，每个约 2.5 MB）

### 构建 GUI

```powershell
cd Manager-GUI
.\gradlew assembleRelease -x lintVitalAnalyzeRelease -x lintVitalReportRelease -x lintVitalRelease
```

> 首次构建需要联网下载依赖，需确保代理可访问 Google Maven。

## 当前限制（务实说明）

- 这是首版 C++ 骨架，不是最终完成版
- 当前使用 `PLT Hook`，只对当前时刻已映射的 ELF 生效
- 后续若应用在运行时动态加载新的 native 库，仍需继续增强
- 路径匹配当前按字符串前缀规则工作，尚未做更复杂的规范化
- 仍未处理 `openat64`、`scandirat`、`getdents64` 等更多入口
- `MediaStore` 查询过滤属于最佳可行版，不保证覆盖所有 ROM 和所有微信选择器分支
- 当规则为白名单或包含 `block` 时，默认自动启用 `MediaStore` 查询过滤（可用 `media_query = true/false/auto` 覆盖）
- 如果微信列表来自应用自身缓存，仍可能出现“缩略图可见但文件不可发送”

## 日志观察（排查问题用）

- 规则命中时会输出 `hit symbol=... action=... total=... block=... redirect=... rules=...`
- 媒体查询被改写时会输出 `media query filtered uri=... blocked_rules=...`
- 可用 `adb logcat -s FolderManager` 直接观察
- 每条规则的命中计数会在内存中累计，便于后续 GUI 查询

## 规则热加载

- 修改 `config/rules.ini` 后，需要手动触发重载
- 执行：`/data/adb/modules/folder_manager/bin/reload.sh`
- 本质是向守护进程发送 `SIGUSR1`，不需要重启应用或手机

## 规则调试工具（CLI）

构建后可使用 `fm_rule_cli` 做路径可视化测试：

```bash
fm_rule_cli --process com.tencent.mm --path /storage/emulated/0/DCIM/A-TEST --op open
```

输出包含：决策结果、匹配规则行号、动作类型、重定向路径。

## 下一步建议（推荐顺序）

优先顺序建议如下：

1. 在真机上编译并安装首版模块
2. 验证目标应用上的 `block` 与 `redirect` 行为
3. 根据实际命中情况补充更多 Hook 点
4. 再决定是否加入更严格的路径规范化与规则缓存

## 真机调试

详细步骤见 `docs/device-test.md`

## 性能与内存说明（说人话）

**结论先说：**
- 热路径性能已经很快：批量匹配约 **6.7M/s**。
- `mem_peak` 看起来很大，是因为**全量基准会构造大量临时数据**，不代表模块常驻占用。
- 更真实的常驻内存（只跑匹配基准的私有内存）约 **3MB 峰值**。

**为什么会出现 100MB+ 的峰值？**
- 全量基准会构造大量规则文本、10k 路径、并做内存估算；
- 这些都是“基准测试”的额外开销，不是模块运行时常驻。

**想看真实占用，用这个：**
- `--benchmark_filter=BM_Match`（只测匹配热路径）

详细对比见：`docs/perf-report.md`
