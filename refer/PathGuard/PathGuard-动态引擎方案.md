# PathGuard — 动态引擎设计方案
## fanotify 文件监控 + 异步搬运

> **适用范围**：Android 12+，GKI 内核（kernel 5.10/5.15）  
> **核心目标**：高性能 · 占用小 · 速度快 · 隐蔽性高 · 不被 App 检测  
> **参考来源**：Linux fanotify(7) / fanotify_init(2) / fanotify_mark(2) 官方手册 · Linux posix_fadvise(2) · torvalds/linux fanotify.h 源码 · v9 全量继承

---

## 一、整体定位

动态引擎解决的问题与静态引擎（bind mount）**完全不同**：

| | 静态引擎（`->` bind mount）| 动态引擎（`=>` fanotify）|
|---|---|---|
| **用户心智** | 任意门：两个路径指向同一份数据 | 搬运工：文件写完就抢走 |
| **文件移动** | ❌ 从不移动数据 | ✅ 写入完成后真实 rename/copy |
| **典型场景** | 欺骗 App / 卸载即焚 | 强制提取沙盒文件到外部目录 |
| **竞态风险** | 无（内核透明）| 有，需要设计保护窗口 |
| **I/O 开销** | 零 | 有（跨分区时）|

规则语法区分：

```ini
# 静态引擎：bind mount，双向透明，零 I/O
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera

# 动态引擎：fanotify 监控 + 异步 rename，单向搬运
DCIM/WeiXin => Pictures/WeiXin_Archive
```

---

## 二、fanotify 核心 API 设计

### 2.1 初始化参数选择

```c
// daemon/watcher.c

int pg_fanotify_init(void) {
    /*
     * 初始化标志说明：
     *
     * FAN_CLASS_NOTIF
     *   纯通知模式（非权限模式）。
     *   我们只需要知道「文件写完了」，不需要阻断 App，
     *   所以不用 FAN_CLASS_CONTENT / FAN_CLASS_PRE_CONTENT。
     *   好处：事件处理不阻塞 App 进程，零延迟影响。
     *
     * FAN_REPORT_DFID_NAME
     *   = FAN_REPORT_DIR_FID | FAN_REPORT_NAME（Linux 5.9+）
     *   每个事件附带：父目录的 file handle + 文件名字符串。
     *   这是获取「完整文件路径」最高效的方式：
     *     父目录 fhandle → open_by_handle_at() → 得到目录 fd
     *     目录 fd + 文件名 → 完整路径，无需遍历 /proc/pid/fd
     *
     * FAN_REPORT_PIDFD（Linux 5.15+，Android 12 GKI 支持）
     *   事件中附带触发进程的 pidfd（而不是普通 pid）。
     *   pidfd 是稳定的进程引用：pid 可能被复用，pidfd 不会。
     *   通过 pidfd_getfd() 可安全获取进程信息。
     *
     * FAN_CLOEXEC | FAN_NONBLOCK
     *   fanotify fd 本身的属性：exec 时自动关闭，read 不阻塞。
     */
    int fan_fd = fanotify_init(
        FAN_CLASS_NOTIF |
        FAN_REPORT_DFID_NAME |   // 目录 fid + 文件名（路径重建必需）
        FAN_REPORT_PIDFD |       // 进程 pidfd（稳定进程标识）
        FAN_CLOEXEC |
        FAN_NONBLOCK,
        O_RDONLY | O_LARGEFILE   // 事件中 fd 的打开标志（NOTIF 模式下 fd=-1，此参数实际不用）
    );

    if (fan_fd < 0) {
        // Android 12 GKI 保证支持 fanotify，失败说明非 GKI 内核
        // 降级策略：记录日志，动态引擎不启动，静态引擎不受影响
        LOGE("PG: fanotify_init failed errno=%d (non-GKI kernel?)", errno);
        return -1;
    }
    return fan_fd;
}
```

### 2.2 挂载点级别监控（一次覆盖全部）

```c
// daemon/watcher.c

int pg_fanotify_watch(int fan_fd) {
    /*
     * FAN_MARK_ADD | FAN_MARK_MOUNT：
     *   标记整个 /data/media 挂载点（ext4 底层，绕过 FUSE）。
     *   之后该挂载点下任意深度的文件写入事件都会上报，
     *   无需像 inotify 那样递归注册每个子目录。
     *   这是 fanotify 相对 inotify 最大的架构优势。
     *
     * 监听的事件：
     *   FAN_CLOSE_WRITE：文件最后一个可写 fd 关闭时触发。
     *     这是「写入真正完成」的最可靠信号，比 IN_MODIFY 准确。
     *     App 写完 close() 之后才触发，不会在写入中途抢走文件。
     *
     *   FAN_CREATE | FAN_ONDIR：
     *     新文件/目录创建时触发（Linux 5.1+，FAN_REPORT_FID 必需）。
     *     用于检测新建子目录，更新路径过滤缓存。
     *     FAN_ONDIR 确保目录事件也被上报。
     *
     * 为什么监控 /data/media 而不是 /sdcard：
     *   /sdcard → FUSE → /data/media
     *   FUSE 层在 Android 11+ 有已知的 fsnotify 集成问题。
     *   直接监控 /data/media（ext4 真实文件系统）事件稳定可靠。
     *   需要 root 权限才能 open("/data/media", ...)。
     */
    int ret = fanotify_mark(
        fan_fd,
        FAN_MARK_ADD | FAN_MARK_MOUNT,        // 挂载点级别，一次覆盖全部
        FAN_CLOSE_WRITE |                      // 写入完成事件
        FAN_CREATE | FAN_ONDIR,               // 新建文件/目录事件
        AT_FDCWD,
        "/data/media"                          // ext4 底层路径，绕过 FUSE
    );

    if (ret < 0) {
        LOGE("PG: fanotify_mark failed errno=%d", errno);
        return -1;
    }
    return 0;
}
```

### 2.3 事件读取与路径重建

```c
// daemon/watcher.c

// 事件缓冲区：一次 read() 尽可能多读，减少系统调用次数
// 4096 * 4 = 16KB，可容纳约 30-50 个事件
#define EVENT_BUF_SIZE (4096 * 4)

// fanotify 事件完整结构（FAN_REPORT_DFID_NAME 模式）：
// [fanotify_event_metadata]          // 固定头部：mask, pid, fd=-1（NOTIF 模式）
// [fanotify_event_info_pidfd]        // pidfd 信息（FAN_REPORT_PIDFD）
// [fanotify_event_info_fid]          // 父目录 file handle（FAN_REPORT_DIR_FID）
//   + 文件名字符串（FAN_REPORT_NAME），紧跟在 fid 结构体之后

typedef struct {
    char  path[PATH_MAX];   // 重建后的完整路径（底层路径，/data/media/0/...）
    pid_t writer_pid;       // 触发写入的进程 PID
    int   writer_pidfd;     // 触发写入的进程 pidfd（稳定引用）
} PgFileEvent;

// 从 fanotify 事件重建完整文件路径
// 使用 open_by_handle_at() 打开父目录，再拼接文件名
static bool rebuild_path(const struct fanotify_event_info_fid *fid_info,
                         char *out_path, size_t out_size) {
    // 1. 获取父目录的 file_handle
    struct file_handle *fh = (struct file_handle *)fid_info->handle;

    // 2. open_by_handle_at() 将 file handle 转换为目录 fd
    //    AT_FDCWD + /proc/self/fd/... 方式打开根目录作为 mount_fd
    int mount_fd = open("/data/media", O_PATH | O_RDONLY);
    if (mount_fd < 0) return false;

    int dir_fd = open_by_handle_at(mount_fd, fh, O_PATH | O_RDONLY);
    close(mount_fd);
    if (dir_fd < 0) return false;

    // 3. 通过 /proc/self/fd/{dir_fd} 读取目录的绝对路径
    char dir_path[PATH_MAX];
    char fd_link[32];
    snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", dir_fd);
    ssize_t len = readlink(fd_link, dir_path, sizeof(dir_path) - 1);
    close(dir_fd);
    if (len <= 0) return false;
    dir_path[len] = '\0';

    // 4. 提取文件名（紧跟在 file_handle 结构体之后的字符串）
    const char *filename = (const char *)fh + sizeof(*fh) + fh->handle_bytes;

    // 5. 拼接完整路径
    snprintf(out_path, out_size, "%s/%s", dir_path, filename);
    return true;
}

void pg_fanotify_loop(int fan_fd, EventQueue *queue) {
    char buf[EVENT_BUF_SIZE]
        __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));

    while (1) {
        // epoll_wait + 非阻塞 read 组合：无事件时 epoll 休眠（零 CPU 占用）
        // 有事件时一次性读取多个，批量处理
        ssize_t n = read(fan_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN) continue;  // 非阻塞 fd，无事件
            if (errno == EINTR)  continue;  // 被信号打断，重试
            break;
        }

        const struct fanotify_event_metadata *meta =
            (const struct fanotify_event_metadata *)buf;

        while (FAN_EVENT_OK(meta, n)) {
            if (meta->vers != FANOTIFY_METADATA_VERSION) {
                meta = FAN_EVENT_NEXT(meta, n);
                continue;
            }

            // 只处理写入完成事件
            if (meta->mask & FAN_CLOSE_WRITE) {
                PgFileEvent evt = {0};
                evt.writer_pid    = meta->pid;
                evt.writer_pidfd  = -1;

                // 从附加信息记录中提取 pidfd 和路径
                const struct fanotify_event_info_header *info =
                    (const void *)meta + meta->metadata_len;
                const struct fanotify_event_info_header *end_info =
                    (const void *)meta + meta->event_len;

                while (info < end_info) {
                    if (info->info_type == FAN_EVENT_INFO_TYPE_PIDFD) {
                        const struct fanotify_event_info_pidfd *pidfd_info =
                            (const void *)info;
                        evt.writer_pidfd = pidfd_info->pidfd;
                    } else if (info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                        const struct fanotify_event_info_fid *fid_info =
                            (const void *)info;
                        rebuild_path(fid_info, evt.path, sizeof(evt.path));
                    }
                    info = (const void *)info + info->len;
                }

                if (evt.path[0] != '\0') {
                    // 丢入任务队列，由搬运线程异步处理
                    event_queue_push(queue, &evt);
                }
            }

            meta = FAN_EVENT_NEXT(meta, n);
        }
    }
}
```

---

## 三、进程过滤：按 App 精确识别

这是 fanotify 相对 inotify 的关键优势之一：事件中有 PID，可以精确知道是哪个 App 写的。

```c
// daemon/pid_filter.c

// 从 pidfd 获取包名（稳定，不受 pid 复用影响）
static bool get_package_name(int pidfd, char *out_pkg, size_t out_size) {
    // 通过 /proc/self/fdinfo/{pidfd} 获取 pid
    char fdinfo_path[64];
    snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/self/fdinfo/%d", pidfd);

    FILE *f = fopen(fdinfo_path, "r");
    if (!f) return false;

    pid_t pid = -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Pid: %d", &pid) == 1) break;
    }
    fclose(f);
    if (pid <= 0) return false;

    // 读 /proc/{pid}/cmdline 获取包名
    char cmdline_path[64];
    snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
    f = fopen(cmdline_path, "r");
    if (!f) return false;

    size_t n = fread(out_pkg, 1, out_size - 1, f);
    fclose(f);
    if (n == 0) return false;

    // cmdline 以 \0 分隔，第一段就是包名（进程主名）
    out_pkg[n] = '\0';
    // 截断到第一个 \0（如果有参数的话）
    char *colon = strchr(out_pkg, ':');
    if (colon) *colon = '\0';   // 去掉 :processName 后缀

    return out_pkg[0] != '\0';
}

// 判断事件是否匹配某条动态规则
bool pg_event_matches_rule(const PgFileEvent *evt,
                           const DynamicRule *rule) {
    // 1. 路径过滤：监控路径是否是规则 src 的前缀
    //    /data/media/0/DCIM/WeiXin → 映射回 /storage/emulated/0/DCIM/WeiXin
    char mapped_path[PATH_MAX];
    data_media_to_storage(evt->path, mapped_path, sizeof(mapped_path));

    if (!path_has_prefix(mapped_path, rule->src_path)) return false;

    // 2. 如果规则指定了包名，验证写入进程是否匹配
    if (rule->pkg[0] != '\0') {
        char writer_pkg[256];
        if (!get_package_name(evt->writer_pidfd, writer_pkg, sizeof(writer_pkg))) {
            return false;  // 无法确认来源，保守跳过
        }
        if (strcmp(writer_pkg, rule->pkg) != 0) return false;
    }

    return true;
}
```

---

## 四、异步搬运引擎：生产者-消费者模型

### 4.1 任务队列设计

```c
// daemon/queue.c

// 无锁环形队列（单生产者单消费者，fanotify 读取线程 → 搬运线程）
// 容量 256，每个元素 = 一个文件路径 + 目标路径
#define QUEUE_CAPACITY 256

typedef struct {
    char src[PATH_MAX];    // 源文件路径（/data/media/0/... 底层路径）
    char dst_dir[PATH_MAX]; // 目标目录路径
    char pkg[256];          // 触发 App 包名（用于日志）
} MoveTask;

typedef struct {
    MoveTask   tasks[QUEUE_CAPACITY];
    atomic_int head;       // 消费者读取位置
    atomic_int tail;       // 生产者写入位置
} TaskQueue;

static TaskQueue g_queue;

// 生产者：fanotify 事件循环调用（非阻塞）
bool queue_push(const MoveTask *task) {
    int tail = atomic_load(&g_queue.tail);
    int next = (tail + 1) % QUEUE_CAPACITY;
    if (next == atomic_load(&g_queue.head)) {
        // 队列已满：丢弃最旧的任务（避免 OOM，保持低内存占用）
        LOGW("PG: task queue full, dropping oldest task");
        atomic_store(&g_queue.head,
                     (atomic_load(&g_queue.head) + 1) % QUEUE_CAPACITY);
    }
    g_queue.tasks[tail] = *task;
    atomic_store(&g_queue.tail, next);
    return true;
}

// 消费者：搬运线程调用（阻塞等待）
bool queue_pop(MoveTask *out) {
    int head = atomic_load(&g_queue.head);
    if (head == atomic_load(&g_queue.tail)) return false;  // 空队列
    *out = g_queue.tasks[head];
    atomic_store(&g_queue.head, (head + 1) % QUEUE_CAPACITY);
    return true;
}
```

### 4.2 搬运线程实现

```c
// daemon/mover.c

// 搬运策略：
//   同分区（/data/media → /data/media）：rename()，原子操作，零拷贝，< 1ms
//   跨分区：sendfile() + 删除源文件，有实际 I/O 开销
static bool move_file(const char *src, const char *dst_dir) {
    // 提取文件名
    const char *filename = strrchr(src, '/');
    if (!filename) return false;
    filename++;

    // 构造目标路径
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, filename);

    // 确保目标目录存在
    mkdir_p(dst_dir, 0755);

    // 尝试同分区 rename（最优路径，原子操作，零 I/O）
    if (rename(src, dst) == 0) {
        LOGI("PG: moved (rename) %s → %s", src, dst);
        return true;
    }

    // rename 跨分区失败（errno=EXDEV），退回 sendfile + unlink
    if (errno != EXDEV) {
        LOGE("PG: rename failed errno=%d src=%s", errno, src);
        return false;
    }

    return sendfile_move(src, dst);
}

// sendfile 跨分区搬运（带 posix_fadvise 页面缓存控制，防止 OOM）
static bool sendfile_move(const char *src, const char *dst) {
    int src_fd = open(src, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) return false;

    struct stat st;
    fstat(src_fd, &st);

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dst_fd < 0) { close(src_fd); return false; }

    /*
     * posix_fadvise 页面缓存控制策略（来自 Linux 官方手册最佳实践）：
     *
     * 背景：sendfile 大文件（如 100MB 视频）时，数据会进入页面缓存，
     *       守护进程是后台低优先级进程，大文件缓存会挤出其他 App 的工作集，
     *       导致系统 UI 卡顿和内存压力（Thrashing）。
     *
     * 解决方案：分块 sendfile + 每块完成后 POSIX_FADV_DONTNEED
     *   1. POSIX_FADV_SEQUENTIAL：告知内核顺序读取，优化预读策略
     *   2. 每 CHUNK_SIZE 字节发送完后，立即 DONTNEED 释放已处理的页面缓存
     *   3. 最终对 dst_fd 执行 DONTNEED，确保目标文件也不污染缓存
     *
     * 注意：DONTNEED 必须在 fdatasync 之后调用，否则脏页不会被释放
     *       （来自 Linux posix_fadvise(2) 手册的明确说明）
     */
    const off_t CHUNK_SIZE = 4 * 1024 * 1024;  // 4MB 分块，平衡性能与内存压力

    posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);  // 顺序读取提示

    off_t offset = 0;
    off_t total  = st.st_size;
    bool  ok     = true;

    while (offset < total) {
        off_t chunk = (total - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total - offset);
        ssize_t sent = sendfile(dst_fd, src_fd, &offset, chunk);
        if (sent <= 0) { ok = false; break; }

        // 每块发送完立即释放源文件页面缓存（防止大文件挤压系统内存）
        posix_fadvise(src_fd, offset - sent, sent, POSIX_FADV_DONTNEED);
    }

    if (ok) {
        // 确保数据落盘后再释放目标文件缓存
        fdatasync(dst_fd);
        posix_fadvise(dst_fd, 0, 0, POSIX_FADV_DONTNEED);
    }

    close(src_fd);
    close(dst_fd);

    if (ok) {
        unlink(src);  // 源文件删除
        LOGI("PG: moved (sendfile) %s → %s", src, dst);
    } else {
        unlink(dst);  // 失败时清理不完整的目标文件
        LOGE("PG: sendfile failed src=%s", src);
    }

    return ok;
}

// 搬运线程主循环（单线程，串行处理，避免并发 I/O 竞争）
void *mover_thread(void *arg) {
    // 线程名伪装（防止被 App 枚举进程线程时发现）
    prctl(PR_SET_NAME, "kworker/u:0", 0, 0, 0);

    MoveTask task;
    while (1) {
        // 等待任务（使用 eventfd + epoll，零 CPU 占用）
        eventfd_wait(g_task_eventfd);

        while (queue_pop(&task)) {
            // 小延迟：等待 App 写完后可能的紧跟读取操作完成
            // 避免「App 写完立刻读，文件已被搬走」的竞态
            usleep(50 * 1000);  // 50ms 保护窗口

            // 再次确认文件已关闭（通过 lsof 等价检查）
            if (file_still_open(task.src)) {
                // 文件仍被持有，推迟搬运（重新入队，延迟 500ms）
                usleep(500 * 1000);
                queue_push(&task);
                continue;
            }

            move_file(task.src, task.dst_dir);
        }
    }
    return NULL;
}

// 检查文件是否仍被其他进程持有
// 通过扫描 /proc/*/fd 检查是否有进程持有该文件的 fd
static bool file_still_open(const char *path) {
    // 获取目标文件的 inode
    struct stat st;
    if (stat(path, &st) != 0) return false;

    // 遍历 /proc/*/fd/，找到 inode 相同的条目
    // 注意：这个操作本身有开销，只在 50ms 窗口后触发，不在热路径
    DIR *proc = opendir("/proc");
    if (!proc) return false;

    bool found = false;
    struct dirent *pd;
    while ((pd = readdir(proc)) != NULL) {
        if (pd->d_type != DT_DIR) continue;
        if (!isdigit(pd->d_name[0])) continue;

        char fd_dir[64];
        snprintf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", pd->d_name);
        DIR *fdd = opendir(fd_dir);
        if (!fdd) continue;

        struct dirent *fd;
        while ((fd = readdir(fdd)) != NULL) {
            char link[64], target[PATH_MAX];
            snprintf(link, sizeof(link), "%s/%s", fd_dir, fd->d_name);
            struct stat lst;
            if (stat(link, &lst) == 0 && lst.st_ino == st.st_ino) {
                found = true;
                break;
            }
        }
        closedir(fdd);
        if (found) break;
    }
    closedir(proc);
    return found;
}
```

---

## 五、守护进程隐蔽设计

守护进程的存在本身是最大的可检测点。

### 5.1 进程伪装

```c
// daemon/main.c

int main(int argc, char *argv[]) {
    // ① argv[0] 清空（/proc/self/cmdline 变为空）
    size_t argv0_len = strlen(argv[0]);
    memset(argv[0], 0, argv0_len);

    // ② 进程名伪装为内核工作线程（prctl PR_SET_NAME）
    //    内核工作线程名格式：kworker/u{N}:{M}
    //    普通 App 无法区分真假（没有 root 权限）
    prctl(PR_SET_NAME, "kworker/u8:2", 0, 0, 0);

    // ③ fork 后父进程退出，子进程成为孤儿进程
    //    由 init(PID=1) 收养，不出现在 Magisk 守护进程的子进程树中
    pid_t pid = fork();
    if (pid > 0) exit(0);       // 父进程退出
    if (pid < 0) return -1;
    setsid();                    // 子进程创建新会话，彻底脱离父进程组

    // ④ 二进制文件路径混淆（由 customize.sh 安装时生成随机路径）
    //    /dev/.__sys_{random8hex}/k（文件名单字母）
    //    安装完成后，install 脚本立即 unlink 自身可执行文件
    //    运行中的守护进程在 /proc/{pid}/exe 显示为「(deleted)」

    // ⑤ 关闭不需要的 fd（stdin/stdout/stderr 重定向到 /dev/null）
    int null_fd = open("/dev/null", O_RDWR);
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);

    // ⑥ 启动 fanotify 监控
    return daemon_main();
}
```

### 5.2 守护进程可检测性分析

| 检测手段 | 具体操作 | 防御措施 | 残余风险 |
|---|---|---|---|
| 枚举 `/proc/` 进程 | 遍历所有 PID，读 cmdline | cmdline 清空，进程名伪装 | 极低 |
| `/proc/{pid}/exe` | 读取可执行文件路径 | unlink 后 exe 显示 `(deleted)` | 无 |
| 进程树分析 | 检查父进程 PID | 成为 init 孤儿进程 | 无 |
| `/proc/{pid}/maps` | 扫描内存映射 | 二进制小，无异常段 | 低 |
| CPU/内存监控 | 无事件时 CPU 使用率 | epoll 休眠，零 CPU | 无 |
| fanotify fd 扫描 | 读 `/proc/{pid}/fdinfo` | fd 在 root 进程，App 无读权限 | 无 |
| 文件搬运操作本身 | 监控 rename/unlink syscall | App 无法监控其他进程的 syscall | 无 |

---

## 六、规则格式扩展

在 v9 的 INI 格式基础上，新增 `=>` 动态规则：

```ini
# PathGuard 规则文件 v10（动态引擎扩展）

[com.tencent.mm]
mode = blacklist

# 静态引擎（bind mount，双向透明，零 I/O）
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera

# 动态引擎（fanotify 监控，单向搬运）
# 语法：监控路径 => 目标目录
# 含义：com.tencent.mm 在 DCIM/WeiXin 下写入的文件，写完后自动搬到 Pictures/WeiXin_Archive
DCIM/WeiXin => Pictures/WeiXin_Archive

[com.tencent.mobileqq]
mode = blacklist

# 动态规则：QQ 在 Tencent/QQ_Images 写的图片，搬到公共相册
Tencent/QQ_Images => Pictures/QQ_Archive
```

**`=>` 规则语义**：
- 监控源目录下所有新写入的文件（`FAN_CLOSE_WRITE`）
- 过滤：只处理规则指定 App（包名匹配）写入的文件
- 写入完成后 50ms 保护窗口，再执行 rename/sendfile
- 目标目录不存在时自动创建（`mkdir -p`）
- 同分区：`rename()`，原子，< 1ms
- 跨分区：`sendfile()` + `POSIX_FADV_DONTNEED`，有 I/O 开销

---

## 七、完整守护进程架构

```
pg_daemon（root 孤儿进程，进程名：kworker/u8:2）
    │
    ├── 主线程
    │    ├── epoll 事件循环（fanotify fd + signal fd + eventfd）
    │    ├── fanotify 事件读取（批量 read，EVENT_BUF_SIZE=16KB）
    │    ├── 路径重建（open_by_handle_at + readlink）
    │    ├── 包名查找（pidfd → /proc/pid/cmdline）
    │    ├── 规则匹配（路径前缀 + 包名过滤）
    │    └── 任务入队（无锁环形队列）
    │
    ├── 搬运线程（单线程，串行）
    │    ├── eventfd 等待（零 CPU 占用）
    │    ├── 50ms 保护窗口
    │    ├── file_still_open 检查
    │    ├── rename()（同分区，原子，< 1ms）
    │    └── sendfile() + POSIX_FADV_DONTNEED（跨分区，分块 4MB）
    │
    └── 信号处理线程
         ├── SIGUSR1：热更新动态规则
         └── SIGTERM/SIGINT：优雅退出（等待搬运线程完成当前任务）
```

---

## 八、性能与资源占用目标

| 指标 | 目标值 | 实现手段 |
|---|---|---|
| 守护进程 RSS | < 2MB | 无 STL，无动态内存分配热路径 |
| 事件循环 CPU（空闲）| 0% | epoll 休眠 |
| 事件循环 CPU（有事件）| < 0.1% | 批量 read，无频繁 syscall |
| 同分区搬运延迟 | < 5ms（含 50ms 窗口）| rename() 原子操作 |
| 跨分区搬运吞吐 | ~500MB/s（UFS 3.1）| sendfile + 4MB 分块 |
| 页面缓存污染 | 极低 | POSIX_FADV_DONTNEED 分块清理 |
| fanotify 事件积压 | 无（队列满丢弃最旧）| 256 槽位无锁环形队列 |

---

## 九、构建集成

动态引擎作为独立二进制（`pg_daemon`），不是 Zygisk SO 的一部分：

```makefile
# Android.mk（守护进程独立编译）
LOCAL_MODULE    := pg_daemon
LOCAL_SRC_FILES := \
    daemon/main.c       \
    daemon/watcher.c    \
    daemon/mover.c      \
    daemon/queue.c      \
    daemon/pid_filter.c \
    utils/path.c        \
    utils/log.c

LOCAL_CFLAGS  += -Os -flto -ffunction-sections -fdata-sections \
                 -fvisibility=hidden -fomit-frame-pointer \
                 -fstack-protector-strong -DNDEBUG
LOCAL_LDFLAGS += -Wl,--gc-sections -Wl,--strip-all -Wl,-z,now \
                 -static-libgcc    # 静态链接 libgcc，减少运行时依赖
LOCAL_LDLIBS  := -llog

include $(BUILD_EXECUTABLE)
```

```makefile
# Application.mk
APP_ABI      := arm64-v8a armeabi-v7a
APP_PLATFORM := android-31   # Android 12+，fanotify FAN_REPORT_PIDFD 最低要求
APP_STL      := none
APP_OPTIM    := release
APP_CFLAGS   := -std=c17
```

**安装集成**（`customize.sh`）：

```bash
# 安装时将 pg_daemon 放到随机路径
DAEMON_DIR="/dev/.__sys_$(cat /proc/sys/kernel/random/uuid | tr -d '-' | head -c 8)"
mkdir -p "$DAEMON_DIR"
cp "$MODPATH/bin/pg_daemon" "$DAEMON_DIR/k"
chmod 700 "$DAEMON_DIR/k"

# 通过 service.sh 在开机时启动守护进程
echo "$DAEMON_DIR/k &" >> "$MODPATH/service.sh"
```

---

## 十、兼容性边界

| 条件 | 状态 | 说明 |
|---|---|---|
| Android 12+，GKI 5.10/5.15 | ✅ 完全支持 | fanotify 全特性可用 |
| Android 12+，非 GKI（升级机）| ⚠️ 降级 | fanotify_init 失败，动态引擎不启动，静态引擎正常 |
| Android 11 及以下 | ❌ 不支持 | 动态引擎不启动 |
| 与静态引擎（bind mount）共存 | ✅ 完全兼容 | 两者独立运行，互不干扰 |
| MIUI/HyperOS | ✅ 可用 | ext4 /data/media 不受 SELinux 影响（root 守护进程） |
| OneUI/Knox | ⚠️ 待验证 | Knox 可能有额外限制 |

---

*PathGuard Dynamic Engine Design · fanotify FAN_CLASS_NOTIF + FAN_REPORT_DFID_NAME + FAN_REPORT_PIDFD · 单线程串行搬运 · posix_fadvise DONTNEED 页面缓存控制 · 进程伪装为内核工作线程 · Android 12+ GKI*
