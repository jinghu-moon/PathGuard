# LSPosed GUI 构建说明

GUI 工程位置：`lsposed-gui/`

## 1. 构建方式

推荐直接用 **Android Studio** 打开 `lsposed-gui/` 目录：

1. 打开后等待 Gradle 同步
2. 选择 `app` 模块
3. 运行或生成 APK（Debug 即可）

## 2. 安装与使用

1. 安装 APK
2. 打开 **LSPosed 管理器**，启用模块（仅用于入口）
3. 打开 GUI，编辑规则并点击“保存并重载”

GUI 内部通过 `su` 写入：
`/data/adb/modules/folder_manager/config/rules.ini`
并调用：
`/data/adb/modules/folder_manager/bin/reload.sh`

## 3. 依赖前提

- 设备已安装 Magisk（可用 `su`）
- 模块已安装并运行守护进程
- 仅支持 Android 12 及以上（API 31）
