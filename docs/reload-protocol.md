# 规则热重载协议（Magisk 侧）

目标：让 GUI 或脚本在写入 `rules.ini` 后，**稳定触发守护进程重载**，不依赖重启应用或手机。

## 1. 固定路径

- 规则文件：`/data/adb/modules/folder_manager/config/rules.ini`
- 守护进程 PID：`/data/adb/modules/folder_manager/run/daemon.pid`
- 重载脚本：`/data/adb/modules/folder_manager/bin/reload.sh`

## 2. 标准重载命令（推荐）

GUI 在保存规则后，执行：

```bash
su -c '/data/adb/modules/folder_manager/bin/reload.sh'
```

该脚本内部会读取 PID 并发送 `SIGUSR1`。

## 3. 直接信号方式（等价）

```bash
su -c 'kill -USR1 $(cat /data/adb/modules/folder_manager/run/daemon.pid)'
```

## 4. 建议的写入流程（避免半写入）

1. 写入临时文件  
2. 原子替换  
3. 触发重载  

示例：

```bash
su -c 'cp /sdcard/rules.ini /data/adb/modules/folder_manager/config/rules.ini'
su -c '/data/adb/modules/folder_manager/bin/reload.sh'
```

## 5. 常见失败原因

- `daemon.pid` 不存在：模块未启动或尚未写入 pid
- `reload.sh` 返回 `daemon not running`：检查 `service.sh` 是否已运行
- 规则解析错误：查看 `/data/adb/modules/folder_manager/run/daemon.log`

## 6. 适配 LSPosed GUI

LSPosed 模块的设置页保存规则后，直接调用第 2 条命令即可完成热重载。
