# Folder Manager 设计说明

## 目标

本模块聚焦两个能力：

1. 禁止指定应用访问指定目录或文件
2. 将指定应用访问的目标路径重定向到新路径
3. 动态导出规则（写入完成后异步搬运）

## 适配范围

- 仅支持 Android 12 及以上（API 31）。
- 安装阶段会进行 API 版本校验，低版本直接中止安装。

## 技术路线

当前采用：`Magisk 模块外壳 + Zygisk C++ 原生模块`

选择理由：

- `Zygisk` 官方 API 本身就是 `C++` 头文件接口
- Hook libc 类文件访问符号时，`C++` 与 NDK 生态更直接
- 相比 `Rust + C++ bridge`，首版方案更简单、可控、可调试

## 处理流程

### 1. preAppSpecialize

- 读取当前进程名
- 判断是否有匹配该进程的规则
- 通过 `getModuleDir()` 读取 `config/rules.ini`
- 将命中的规则加载到当前进程内存

### 1.5 动态重定向守护进程（=>）

- `service.sh` 启动 `folder_manager_daemon`
- 使用 `fanotify` 监控 `/data/media`
- 命中 `=>` 规则后搬运文件（异步）
- 命中 `export_folders` 规则后复制文件（可选触发 media scan）

### 2. postAppSpecialize

- 扫描 `/proc/self/maps`
- 提取当前已映射 ELF 的 `dev + inode`
- 对目标符号注册 `PLT Hook`
- 提交 Hook

### 3. 运行时路径决策

- `block`：返回 `ENOENT`
- `redirect`：将原始路径改写为新路径，再调用原始函数

### 3.5 MediaStore 过滤策略

- 当规则为白名单或存在 `block` 时，启用 `MediaStore` 查询参数改写，尽量让列表层与访问层一致。
- 过滤同时使用 `_data` 与 `relative_path` 以兼容旧接口与新接口。
- 可通过 `media_query = true/false/auto` 显式控制启用策略。

## 当前实现边界

### 已实现

- 包名匹配
- 规则解析
- 路径前缀匹配
- 目录级阻断
- 目录级重定向
- 常见 libc 文件访问入口 Hook 骨架

### 尚未实现

- 动态加载新 ELF 后的二次 Hook
- 更多变体函数覆盖
- 路径规范化
- 复杂符号链接场景处理
- 规则热更新

## 参考设计（取长补短）

- Android Scoped Storage 的权限和访问模型，避免对系统行为做超范围假设。
- Storage Isolation 的“按应用隔离/导出”思路，用于指导可视化与规则分组设计。
- Storage Redirect 的“规则驱动重定向 + 监控写入”思路，用于约束守护进程的职责边界。

## 原则说明

### KISS

- 首版只做必要 Hook 点与必要规则模型
- 不引入 Rust、Gradle、JNI UI 等额外复杂度

### YAGNI

- GUI 只做轻量配置入口，不做重型前端
- 暂不做规则数据库
- 暂不做多层抽象引擎

### DRY

- 所有路径决策统一走 `rule_config`
- Hook 逻辑只负责转发，不重复实现规则判断

### SOLID

- `folder_manager.cpp`：负责 Zygisk 生命周期与 Hook 注册
- `rule_config.cpp`：负责规则解析和路径决策
