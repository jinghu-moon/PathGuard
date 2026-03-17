# PathGuard

[![License: AGPL-3.0-or-later](https://img.shields.io/badge/License-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Android](https://img.shields.io/badge/Android-12%2B-green.svg)](https://developer.android.com)
[![NDK](https://img.shields.io/badge/NDK-r27d-orange.svg)](https://developer.android.com/ndk)

基于 **Magisk + Zygisk + C++** 的 Android 原生文件路径管控模块，配套 Kotlin/Compose 管理 App。

支持按应用拦截指定路径访问，对指定路径执行文件重定向（移动）、删除、导出，并通过 `fanotify` 实现任意来源触发的目录监控。

**要求：Android 12+（API 31），需 Root（Magisk / KernelSU / APatch）**

---

## 功能概览

### Native 守护进程（C++）

| 功能 | 说明 |
|---|---|
| 路径拦截（Block） | Zygisk Hook `open/stat/opendir` 等系统调用，阻止应用访问指定路径 |
| 文件重定向（Redirect） | `fanotify` 监控 `CLOSE_WRITE` 事件，写入完成后自动移动文件 |
| 文件删除（Delete） | 写入触发后自动删除，支持软删除（移入 Trash 目录）|
| 文件导出（Export） | 复制到目标路径，支持注册 MediaStore / Downloads |
| 通配来源重定向 `[*]` | 不绑定包名，任意应用写入均触发重定向 |
| 规则热重载 | 向守护进程发送 `SIGUSR1`，无需重启应用或设备 |
| 审计日志 | 结构化写入 `access.log`，记录 `pkg/pid/op/action/path` |
| MediaStore 过滤 | JNI Hook `BinderProxy.transactNative`，对 MediaStore 查询追加排除条件 |

### 管理 App（Kotlin + Compose）

| 功能 | 说明 |
|---|---|
| 规则编辑 | 内置语法高亮编辑器（Nord 配色），支持图形化规则插入 |
| 规则概览 | 按包名折叠卡片，展示 allow/block/redirect 统计 |
| 规则沙盒 | 纯 Kotlin 干运行，预览规则命中结果 |
| 规则 Diff | LCS 算法对比变更，新增/删除行高亮 |
| 社区预设 | 10 个热门 App 预设规则包（微信、QQ、抖音等） |
| 模板库 | 内置 + 用户自定义模板，持久化存储 |
| 冲突检测 | 导入时自动检测重复 section / allow-block 冲突 |
| 备份与回滚 | 时间戳备份，保留最近 5 份，支持一键回滚 |
| 导入 / 导出 | 文件导入（含验证）/ `ShareCompat` 分享导出 |
| access.log 查看 | 实时轮询刷新，长按日志行一键生成豁免规则 |
| 命中热力图 | Canvas 堆叠柱状图，路径访问频次可视化 |
| 重定向流图 | 规则源路径 → 目标路径可视化 |
| 存储占用分析 | `du` 查询各重定向目录占用，支持清空 / 迁移 |
| 敏感路径告警 | 轮询检测，触发推送通知 |
| 批量操作 | 多选 section 批量启用 / 禁用 |
| daemon 状态监控 | Health Check 面板 + 热重载按钮 |
| Root 环境检测 | 自动识别 Magisk / KernelSU / APatch |
| 多语言 | 中文 / 英文（`values-en/`）|

---

## 规则格式

规则文件位置（设备）：`/data/adb/modules/folder_manager/config/rules.ini`

### 基本结构

```ini
[com.example.app]
mode = blacklist        # blacklist（默认）或 whitelist
enabled = true

- Download/secret       # 拦截：禁止访问
+ DCIM/Camera           # 放行（白名单模式下使用）
DCIM/Camera -> Android/data/<pkg>/cache/Camera   # 重定向
```

### 通配来源重定向（任意 App 触发）

```ini
[*]
/storage/emulated/0/Download/ -> /storage/emulated/0/Documents/Auto/
/storage/emulated/0/DCIM/ -> /storage/emulated/0/Pictures/Sorted/ @types=jpg,png,heic
```

### 完整选项

```ini
[com.example.app]
mode = blacklist
enabled = true
trash_on_redirect = true          # 重定向时将原文件移入 Trash 而非覆盖
trash_dir = /sdcard/.MyTrash      # 自定义 Trash 目录
delete_existing = true            # 启动时清理已存在的命中文件
media_query = auto                # auto / true / false，控制 MediaStore 过滤
min_age_days = 7                  # 仅处理超过 N 天的文件（delete 规则）

- Download/secret
DCIM/Camera -> Android/data/<pkg>/cache/Camera
Download/*.pdf -> Documents/PDF/ @types=pdf
```

### 子 section 类型

```ini
; 跨应用路径授权
[com.example.app.accessible.rule1]
from = com.other.app
path = /storage/emulated/0/SharedData/

; 文件导出（注册到 MediaStore/Downloads）
[com.example.app.export.photos]
source = Android/data/com.example.app/cache/output/
target = Pictures/ExportedPhotos/
media_scan = true
add_to_downloads = true
```

---

## 目录结构

```
PathGuard/
├── native/                          # C++ 源码
│   ├── folder_manager.cpp           # Zygisk 模块 + PLT Hook 主逻辑
│   ├── folder_manager_daemon.cpp    # fanotify 守护进程
│   ├── rule_engine.cpp/h            # INI 解析 + 规则匹配引擎
│   ├── path_mapper.cpp/h            # 路径重定向映射
│   ├── path_kind_cache.cpp/h        # 路径类型缓存
│   ├── daemon_utils.cpp/h           # 审计日志 / DownloadManager 工具
│   ├── runtime_context.h            # 运行时上下文
│   ├── Android.mk / Application.mk  # ndk-build 配置
│   └── zygisk.hpp                   # 官方 Zygisk API
│
├── module/                          # Magisk 模块运行时文件
│   ├── module.prop
│   ├── customize.sh / service.sh
│   ├── config/
│   │   ├── rules.ini                # 规则配置
│   │   └── module.conf              # 全局开关（enabled = true/false）
│   ├── bin/{abi}/folder_manager_daemon
│   └── zygisk/{abi}.so
│
├── Manager-GUI/                     # Android 管理 App（Kotlin + Compose）
│   └── app/src/main/
│       ├── assets/
│       │   ├── presets/             # 10 个社区预设 .ini
│       │   └── templates/           # 规则模板
│       └── java/com/folder/manager/gui/
│           ├── ui/                  # Compose UI 组件
│           └── data/               # 数据层（Repository / Watcher）
│
├── scripts/
│   ├── build.ps1                    # 完整模块打包（输出 dist/*.zip）
│   └── build-native.ps1             # 仅构建 Native 库
│
├── tools/rule_cli.cpp               # 规则路径可视化 CLI 调试工具
├── tests/                           # 单元测试
├── benchmarks/                      # 性能基准
└── docs/                            # 设计文档
```

---

## 构建

### 依赖

- Android NDK r27d（`ndk-build`）
- Android SDK（`compileSdk 35`）
- JDK 17+

### 构建 Native 库

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-native.ps1 -Abi arm64-v8a
```

产物复制到：
- `module/zygisk/arm64-v8a.so`
- `module/bin/arm64-v8a/folder_manager_daemon`

### 构建管理 App

```powershell
cd Manager-GUI
.\gradlew assembleDebug -x lintVitalAnalyzeRelease -x lintVitalRelease -x lintVitalReportRelease
```

> 首次构建需联网下载依赖，需确保代理可访问 Google Maven。

### 打包完整 Magisk 模块

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
```

输出：`dist/folder-manager-magisk-v{version}-{abi}.zip`

---

## 运行时路径（设备）

| 用途 | 路径 |
|---|---|
| 规则配置 | `/data/adb/modules/folder_manager/config/rules.ini` |
| 全局开关 | `/data/adb/modules/folder_manager/config/module.conf` |
| 访问审计日志 | `/data/adb/modules/folder_manager/run/access.log` |
| 守护进程日志 | `/data/adb/modules/folder_manager/run/daemon.log` |
| Trash 目录 | `/storage/emulated/0/.FolderManagerTrash`（默认）|

---

## 规则热重载

修改 `rules.ini` 后，无需重启设备或应用，执行：

```bash
kill -USR1 $(cat /data/adb/modules/folder_manager/run/daemon.pid)
```

或通过管理 App「daemon 状态」面板点击「热重载规则」。

---

## 日志观察

```bash
adb logcat -s FolderManager
```

关键日志格式：

```
hit symbol=open action=block total=12 block=10 redirect=2 rules=[Download/secret]
media query filtered uri=content://media/... blocked_rules=1
```

---

## 规则调试（CLI）

```bash
fm_rule_cli --process com.tencent.mm --path /storage/emulated/0/DCIM/A-TEST --op open
```

输出：决策结果、匹配规则行号、动作类型、重定向路径。

---

## 性能参考

- 规则匹配热路径：**~6.7M 次/秒**
- 守护进程常驻内存：**~3 MB 峰值**
- fanotify 事件处理：异步队列（上限 256），独立 worker 线程执行移动

---

## 已知限制

- PLT Hook 仅对进程启动时已映射的 ELF 生效，运行时动态加载的 so 需额外处理
- `fanotify` 触发条件为 `CLOSE_WRITE`，不含 `rename`/`mv` 操作（即 `mv` 到监控目录不触发）
- MediaStore 查询过滤不保证覆盖所有 ROM 定制分支
- 通配 `[*]` 规则仅作为 fallback，包名精确规则优先匹配

---

## License

[AGPL-3.0-or-later](LICENSE)
