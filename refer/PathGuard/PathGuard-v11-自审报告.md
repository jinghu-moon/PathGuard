# PathGuard v11 — 严格自审报告

> 基于：Linux fanotify(7)/fanotify_mark(2)/open_by_handle_at(2) 官方手册 · ZygiskNext Releases（Dr-TSNG archived Jan 2024，JingMatrix fork 继续至 Aug 2025）· APatch 官方文档 · Detecting Shamiko & Zygisk 2025（Medium）· mount_namespaces(7) · libfuse Issue #576

---

## 总结：发现 7 项问题（2 项严重，3 项中等，2 项轻微）

| 编号 | 严重级 | 位置 | 问题 |
|---|---|---|---|
| #1 | 🔴 严重 | 动态引擎 | `open_by_handle_at()` 未处理 `ESTALE`，文件已删除时进程 abort |
| #2 | 🔴 严重 | 动态引擎 | `mount_fd` 每次事件重新 open，高频场景 fd 泄漏风险 + 性能损耗 |
| #3 | 🟡 中等 | 动态引擎 | `is_file_still_open()` 遍历全量 `/proc/*/fd`，O(进程数×fd数)，在写入回调路径上代价过高 |
| #4 | 🟡 中等 | 守护进程 | `unlink_self_executable()` 只有调用点，无实现，且 Linux 不能 unlink 自己正在执行的文件 |
| #5 | 🟡 中等 | 生态兼容 | ZygiskNext / Shamiko 原始仓库已于 2024-01 被归档，推荐为「主力平台」描述失准 |
| #6 | ⚪ 轻微 | 动态引擎 | `CAP_DAC_READ_SEARCH` 依赖未在文档标注（root 守护进程默认持有，实际不影响，但应说明） |
| #7 | ⚪ 轻微 | 守护进程 | `memset(argv[0], ...)` 只清零 argv[0]，Linux cmdline 可见范围实际更大 |

---

## 问题 #1（🔴 严重）：ESTALE 未处理

### 问题描述

v11 的路径重建代码：

```c
// v11 当前写法（有缺陷）
int dfd = open_by_handle_at(g_media_fd, (void *)fh, O_PATH | O_RDONLY);
// ❌ 直接继续使用 dfd，未判断 errno
```

官方 Linux fanotify(7) 手册明确说明，且给出了完整示例代码：

```c
// 官方手册示例（完整错误处理）
event_fd = open_by_handle_at(mount_fd, file_handle, O_RDONLY);
if (event_fd == -1) {
    if (errno == ESTALE) {
        // 文件句柄已过期（文件在 FAN_CLOSE_WRITE 触发后、
        // open_by_handle_at 调用前被另一进程删除）
        // 正确处理：跳过此事件，继续处理下一个
        printf("File handle is no longer valid. File has been deleted\n");
        continue;
    } else {
        perror("open_by_handle_at");
        exit(EXIT_FAILURE);
    }
}
```

### 触发场景

1. App 写完文件触发 `FAN_CLOSE_WRITE`
2. App 同一时刻（或事件队列中的另一线程）立刻 `unlink()` 该文件
3. 守护进程读取事件，调用 `open_by_handle_at()` 时文件已不存在
4. v11 未处理此分支 → `dfd < 0`，后续 `readlink("/proc/self/fd/-1")` 行为未定义

### 修正后代码

```c
static bool rebuild_path_from_fid(int media_fd,
                                   const struct fanotify_event_info_fid *fi,
                                   char *out_path, size_t out_size) {
    struct file_handle *fh = (struct file_handle *)fi->handle;

    int dfd = open_by_handle_at(media_fd, fh, O_PATH | O_RDONLY);
    if (dfd < 0) {
        if (errno == ESTALE) {
            // 文件在事件触发后已被删除，跳过（正常情况，非错误）
            return false;
        }
        // EPERM/EBADF 等其他错误：记录但不 abort
        LOGW("PG: open_by_handle_at errno=%d", errno);
        return false;
    }

    char fdlink[48];
    snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", dfd);
    ssize_t n = readlink(fdlink, out_path, out_size - 1);
    close(dfd);
    if (n <= 0) return false;
    out_path[n] = '\0';

    // 拼接文件名（紧跟在 file_handle->f_handle + handle_bytes 之后）
    const char *fname = (const char *)(fh->f_handle + fh->handle_bytes);
    size_t dir_len = n;
    if (dir_len + 1 + strlen(fname) + 1 > out_size) return false;
    out_path[dir_len] = '/';
    strlcpy(out_path + dir_len + 1, fname, out_size - dir_len - 1);

    return true;
}
```

---

## 问题 #2（🔴 严重）：mount_fd 每次事件重新 open

### 问题描述

v11 在每个 `FAN_CLOSE_WRITE` 事件处理时：

```c
// v11 当前写法（有缺陷）
int mfd = open("/data/media", O_PATH | O_RDONLY);  // 每次都 open！
int dfd = open_by_handle_at(mfd, (void *)fh, O_PATH | O_RDONLY);
close(mfd);
```

`open_by_handle_at()` 的第一个参数只需是目标 filesystem 上的**任意一个 fd**，用于内核定位挂载点。它不需要每次重新打开。

### 危害

1. **fd 泄漏风险**：若 `open_by_handle_at()` 之前发生异常（或代码路径跳转到 `goto next_info`），`mfd` 不会被关闭
2. **性能损耗**：高频写入（如视频录制每帧写入）场景中，每个事件额外一次 `open()` 系统调用
3. **重复打开无意义**：`/data/media` 的 inode 不变，每次 open 拿到的是同一个文件系统上的 fd

### 修正

```c
// daemon/watcher.c - 全局变量，初始化时打开一次，全生命周期复用
static int g_media_fd = -1;

int pg_fanotify_init(void) {
    // ...
    // 打开一次，持有整个守护进程生命周期
    g_media_fd = open("/data/media", O_PATH | O_RDONLY);
    if (g_media_fd < 0) {
        LOGE("PG: cannot open /data/media: errno=%d", errno);
        return -1;
    }
    // ...
}

// 所有事件处理直接复用 g_media_fd：
static bool rebuild_path_from_fid(const struct fanotify_event_info_fid *fi,
                                   char *out, size_t size) {
    struct file_handle *fh = (struct file_handle *)fi->handle;
    int dfd = open_by_handle_at(g_media_fd, fh, O_PATH | O_RDONLY);
    if (dfd < 0) {
        if (errno == ESTALE) return false;
        LOGW("PG: open_by_handle_at errno=%d", errno);
        return false;
    }
    // ...（后续路径重建）
    close(dfd);
    return true;
}
```

---

## 问题 #3（🟡 中等）：is_file_still_open() 遍历全量 /proc

### 问题描述

v11 的 `is_file_still_open()` 通过遍历 `/proc/*/fd/*` 来检查文件是否仍被持有：

```c
// v11 当前写法（代价过高）
DIR *proc = opendir("/proc");
while ((pd = readdir(proc)) != NULL) {       // 遍历所有进程
    DIR *fdd = opendir(fd_dir);
    while ((fd = readdir(fdd)) != NULL) {    // 遍历每个进程的所有 fd
        stat(link, &lst);
        if (lst.st_ino == st.st_ino) ...     // inode 对比
    }
}
```

**代价**：典型 Android 设备有 200-400 个进程，每个进程有数十个 fd，单次调用需要 `200 × 50 = 10,000` 次 `stat()` 系统调用，耗时约 5-20ms，远超可接受范围。

### 修正：只检查已知写入进程

fanotify 事件本身携带了写入进程的 PID（`meta->pid` 或 pidfd）。只需检查**已知写入方**是否仍持有该 fd，而不是全量遍历：

```c
// 只检查触发写入的进程，代价 O(该进程的 fd 数)
static bool is_file_open_by_pid(const char *path, pid_t writer_pid) {
    struct stat st;
    if (stat(path, &st) != 0) return false;

    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", writer_pid);

    DIR *fdd = opendir(fd_dir);
    if (!fdd) return false;  // 进程已退出 → 文件必定已关闭

    bool found = false;
    struct dirent *fd;
    while ((fd = readdir(fdd)) != NULL) {
        char link[128];
        snprintf(link, sizeof(link), "%s/%s", fd_dir, fd->d_name);
        struct stat lst;
        if (stat(link, &lst) == 0 && lst.st_ino == st.st_ino) {
            found = true;
            break;
        }
    }
    closedir(fdd);
    return found;
}
```

**代价降低**：典型 App 进程有 20-50 个 fd，单次检查约 20-50 次 `stat()`，耗时 < 1ms。

---

## 问题 #4（🟡 中等）：unlink_self_executable() 误解与缺失实现

### 问题描述

v11 守护进程 `main()` 中有：

```c
unlink_self_executable();  // ❌ 函数未实现
```

且有一个根本性误解：**Linux 不能 unlink 正在执行的文件**。`unlink()` 只会删除目录项（dentry），使文件的 link count 降为 0，但只要进程还在运行，内核持有的 inode 引用计数就不为零，文件数据不会被释放。`/proc/{pid}/exe` 会显示路径加上 `(deleted)` 后缀。

这本身就是期望行为，v11 描述中的「删除后 exe 显示 deleted，无法反查路径」是正确的最终状态，但实现方式有误。

### 正确实现

```c
static void unlink_self_binary(void) {
    // 读取当前可执行文件的绝对路径
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) return;
    exe_path[len] = '\0';

    // 去掉可能已有的 " (deleted)" 后缀（首次运行时无，重启后可能有）
    char *del = strstr(exe_path, " (deleted)");
    if (del) *del = '\0';

    // unlink：删除目录项（dentry）
    // 效果：/proc/self/exe 变为 "/dev/.__sys_xxx/k (deleted)"
    // 其他进程无法通过文件系统路径找到这个二进制文件
    // 当前运行中的进程不受影响（内核持有 inode 引用）
    if (unlink(exe_path) != 0 && errno != ENOENT) {
        LOGW("PG: unlink self binary errno=%d", errno);
    }
}
```

**注意**：`unlink_self_binary()` 应在 daemon 完成初始化（fanotify 已建立）之后再调用，确保不影响重新加载路径。

---

## 问题 #5（🟡 中等）：生态状态描述失准

### 问题描述

v11 生态兼容性表：

| KSU + ZygiskNext | ✅ |
| KSU/APatch + NeoZygisk v2.x | ✅ 推荐 |

并在多处将 ZygiskNext 和 Shamiko 列为主力推荐。

### 实际状态（基于搜索结果）

- **ZygiskNext（Dr-TSNG/ZygiskNext）**：原始仓库于 2024 年 1 月被归档，原团队停止维护。JingMatrix fork（JingMatrix/LSPosed 的子项目）在 2025 年 8 月仍有更新（v1.2.9.1），但为社区 fork 而非原始项目
- **Shamiko（LSPosed/Shamiko）**：同 2024 年 1 月归档，JingMatrix fork 继续维护至 v1.2.5
- **NeoZygisk**：活跃开发中，仅提供最小化 Zygisk API，直接对标 Magisk 内置 Zygisk 设计
- **ReZygisk**：活跃，自由实现，仍在早期开发阶段
- **Zygisk-Assistant（snake-4）**：活跃，开源 root 隐藏模块，Shamiko 的开源替代

### 修正后的兼容性描述

| Zygisk 实现 | 状态 | 备注 |
|---|---|---|
| Magisk 内置 Zygisk（v27+）| ✅ 主力 | 官方维护，最稳定 |
| ZygiskNext（JingMatrix fork）| ✅ 社区 fork | 原始 Dr-TSNG 仓库已归档；fork 活跃 |
| NeoZygisk | ✅ 推荐 | 最小化 API，活跃维护，与 Magisk Zygisk 设计最接近 |
| ReZygisk | ✅ | 活跃，仍在早期开发 |
| 根隐藏搭配 | Zygisk-Assistant（开源）或 Shamiko fork | Shamiko 原仓库已归档 |

---

## 问题 #6（⚪ 轻微）：CAP_DAC_READ_SEARCH 依赖未说明

### 问题描述

`open_by_handle_at()` 需要调用进程持有 `CAP_DAC_READ_SEARCH` 能力（参考 libfuse issue #576，Linux capabilities(7)）。

v11 的守护进程以 root 运行，默认持有全部 capabilities（包括 `CAP_DAC_READ_SEARCH`），实际不影响功能。但方案文档未说明这一依赖，若未来考虑降权（privilege dropping）则需注意。

### 建议补充

```c
// daemon/watcher.c 启动时校验：
static bool check_required_caps(void) {
    // fanotify_init() 需要 CAP_SYS_ADMIN
    // open_by_handle_at() 需要 CAP_DAC_READ_SEARCH
    // 以 root (UID=0) 运行时两者均默认持有
    // 若未来考虑 setuid/capability-only 模式，此处需显式检查
    if (geteuid() != 0) {
        LOGE("PG daemon: must run as root (UID=0)");
        return false;
    }
    return true;
}
```

---

## 问题 #7（⚪ 轻微）：cmdline 清零不完整

### 问题描述

v11 使用：

```c
memset(argv[0], 0, strlen(argv[0]));
```

只清零了 `argv[0]` 字符串本身。

Linux 进程的 `/proc/self/cmdline` 是 `argv[0]` 到 `argv[argc-1]` 之间的连续内存区域，以 `\0` 分隔各参数。上述写法只清空了第一个参数，若进程以 `pg_daemon --config=/path/to/config` 启动，`--config=...` 部分仍可见。

### 修正

```c
// 计算整个 cmdline 区域长度并全部清零
static void wipe_cmdline(int argc, char *argv[]) {
    if (argc < 1 || !argv[0]) return;

    // 找到 argv[argc-1] 的末尾
    char *end = argv[argc - 1] + strlen(argv[argc - 1]);
    size_t total = (size_t)(end - argv[0]);
    if (total > 0) {
        memset(argv[0], 0, total);
    }
}
// 调用：wipe_cmdline(argc, argv);
```

注意：部分内核要求 `/proc/self/cmdline` 至少保留一个字节可见；全清零后其他进程读取 cmdline 得到空字符串，配合 `prctl(PR_SET_NAME, ...)` 已足够。

---

## 不存在的问题（经验证为正确）

以下在初次审查中曾产生疑虑，验证后确认 v11 设计正确：

### ✅ `fanotify_event_info_fid->handle` 访问方式

v11 使用 `(struct file_handle *)fi->handle`，官方 man 手册示例代码使用相同模式，正确。
`handle` 是 flexible array member，decay 为 `unsigned char *`，cast 到 `struct file_handle *` 后按 `handle_bytes` + `f_handle` 访问，语义清晰。

### ✅ FAN_CLASS_NOTIF + FAN_REPORT_DFID_NAME + FAN_MARK_MOUNT 组合

fanotify_mark(2) 中受到 `FAN_MARK_MOUNT` 限制的是 **需要 file handle 才能触发** 的事件（如 `FAN_CREATE`, `FAN_ATTRIB`, `FAN_DELETE_SELF`）。`FAN_CLOSE_WRITE` 不在此列，可与 `FAN_MARK_MOUNT` 共用，同时搭配 `FAN_REPORT_DFID_NAME` 提供辅助路径信息，合法。

### ✅ 双重 fork + setsid 守护进程化

经典 POSIX 双重 fork 模式，孙进程成为 init 孤儿进程，正确。

### ✅ pidfd 原始 fd 需手动 close

v11 事件循环中正确 `close(pi->pidfd)` 后再使用 `dup()` 的副本，符合 fanotify(7) 规定（FAN_REPORT_PIDFD 的 pidfd 由内核在每个事件中创建，使用完毕必须 close）。

### ✅ MS_PRIVATE | MS_REC 的必要性

mount_namespaces(7) 确认：MS_PRIVATE 切断 propagation，使后续 bind mount 不产生 shared peer group ID，匹配 2025 年检测报告中 mount namespace mismatch (+60) 的防御需求。

---

## 修订优先级

| 优先级 | 问题 | 影响 |
|---|---|---|
| 立即修复 | #1 ESTALE 未处理 | 高频写+删场景下进程异常 |
| 立即修复 | #2 mount_fd 重复 open | fd 泄漏 + 系统调用浪费 |
| 下次迭代 | #3 is_file_open 遍历过重 | 高并发写入时 CPU spike |
| 下次迭代 | #4 unlink 实现错误 | 隐蔽性功能残缺 |
| 文档更新 | #5 生态描述失准 | 不影响实现，影响文档准确性 |
| 可选 | #6 CAP 依赖未标注 | 仅文档缺失 |
| 可选 | #7 cmdline 清零不完整 | 隐蔽性轻微提升 |

---

*PathGuard v11 自审报告 · 7 项问题（2 严重 / 3 中等 / 2 轻微）· 基于官方手册和同类开源项目交叉验证*
