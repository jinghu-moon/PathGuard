# Folder Manager 真机调试

## 前提

1. 设备已安装支持 `Zygisk` 的 `Magisk`
2. 已在 `Magisk` 中开启 `Zygisk`
3. 已通过 `build.ps1` 生成安装包

## 安装

1. 在 `Magisk App` 中选择“从本地安装”
2. 选择 `dist/folder-manager-magisk-v0.2.0.zip`
3. 安装后重启设备
4. 如果你刚从旧版本升级，务必确认当前安装的是 `0.3.0`

## 示例规则

编辑 `/data/adb/modules/folder_manager/config/rules.ini`

```ini
[com.example.app]
mode = blacklist

- Download/secret
DCIM/Camera -> Android/data/<pkg>/cache/Camera
Movies/demo.mp4 -> Android/data/<pkg>/cache/demo.mp4
DCIM/WeChat => Pictures/WeChat_Archive
```

说明：

- 第一条阻断目录访问
- 第二条重定向目录访问
- 第三条重定向单文件访问
- 第四条动态搬运（写完后转存）

修改规则后，建议强制停止目标应用并重新启动。

## 日志查看

### 过滤原生日志

```bash
adb logcat -s FolderManager
```

### 观察媒体查询过滤日志

```bash
adb logcat -s FolderManager | grep 'media query filtered'
```

### 只看命中日志

```bash
adb logcat -s FolderManager | grep 'hit symbol='
```

### 观察累计统计

- `total`：累计命中次数
- `block`：累计阻断次数
- `redirect`：累计重定向次数
- `rules`：当前进程加载到的规则数量
- `media query filtered`：说明模块已经开始改写 `MediaStore` 查询条件

### 查看模块 service 占位日志

```bash
adb shell su -c 'tail -f /data/adb/modules/folder_manager/run/service.log'
```

### 查看动态引擎日志

```bash
adb shell su -c 'tail -f /data/adb/modules/folder_manager/run/daemon.log'
```

### 动态规则热更新

```bash
adb shell su -c '/data/adb/modules/folder_manager/bin/reload.sh'
```

### 动态守护进程自检

```bash
adb shell su -c '/data/adb/modules/folder_manager/bin/folder_manager_daemon --self-check'
```

### 查看规则文件

```bash
adb shell su -c 'cat /data/adb/modules/folder_manager/config/rules.ini'
```

## 验证步骤

### 验证 block

1. 先确保目标目录真实存在
2. 启动目标应用并触发对该目录的读取
3. 观察应用是否表现为“文件不存在”或目录为空
4. 观察 `logcat` 中是否出现 `matched_rules` 与 `PLT Hook` 日志
5. 观察是否出现 `hit symbol=... action=block path=...`
6. 观察是否出现 `media query filtered uri=...`

### 验证 redirect

1. 准备目标路径和替代路径
2. 替代路径放入可识别的测试内容
3. 启动目标应用访问原路径
4. 确认应用实际读到的是替代路径内容
5. 观察是否出现 `hit symbol=... action=redirect from=... to=...`

### 验证 export + add_to_downloads

1. 在 `export_folders` 中开启 `add_to_downloads = true`
2. 触发目标应用写入 `source` 目录
3. 查看是否已复制到 `target`
4. 打开系统“下载”应用，确认列表出现该文件
5. 观察日志是否出现 `add_to_downloads ok: ...`
6. 可用以下命令确认写入记录：

```bash
adb shell su -c 'content query --uri content://downloads/my_downloads --projection _id,title,_data | head -n 5'
```

## 常见问题

### 没有任何日志

- 确认 `Magisk` 已开启 `Zygisk`
- 确认目标应用进程名与规则中的包名匹配
- 确认模块目录下存在对应 ABI 的 `zygisk/*.so`

### 规则不生效

- 确认访问的是直接文件系统路径，不是 `SAF` 或 `ContentProvider`
- 确认目标库在进程启动时已经映射，当前实现只对已映射 ELF 做 `PLT Hook`
- 尝试强制停止应用后重新打开

### 只对部分访问生效

- 说明目标应用可能走了尚未覆盖的 libc 入口
- 下一步优先补 `openat64`、`getdents64`、`scandirat`

### 列表仍可见但发送失败

- 说明真实文件读取已经被拦下
- 但列表层可能来自 `MediaStore` 缓存、缩略图缓存或应用内部索引
- 当前版本已追加 `MediaStore` 查询过滤；重装模块并重启后再验证一次
