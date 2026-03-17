package com.folder.manager.gui.data

import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream

/**
 * 读取 daemon.log 并解析运行状态。
 */
object DaemonStatusReader {

    private const val DAEMON_LOG = "/data/adb/modules/folder_manager/run/daemon.log"
    private const val DAEMON_PID = "/data/adb/modules/folder_manager/run/daemon.pid"
    private const val TAIL_LINES = 60

    data class DaemonStatus(
        val running: Boolean,
        val pid: Int?,
        val mountCount: Int,       // fanotify 挂载点数量
        val errorCount: Int,       // 最近日志中的 ERROR 行数
        val lastError: String,     // 最新一条 ERROR
        val startTime: String,     // 启动时间戳（首行）
        val recentLines: List<String>, // 最近 N 行日志
    )

    fun read(): Result<DaemonStatus> = runCatching {
        // 检查 PID 文件
        val pidStr = runCatching {
            SuFileInputStream.open(SuFile(DAEMON_PID)).bufferedReader().use { it.readText().trim() }
        }.getOrNull()
        val pid = pidStr?.toIntOrNull()

        // 检查进程是否存活
        val running = if (pid != null) {
            Shell.cmd("kill -0 $pid 2>/dev/null").exec().isSuccess
        } else {
            Shell.cmd("pgrep -x folder_manager_daemon 2>/dev/null").exec().isSuccess
        }

        // 读取日志尾部
        val logResult = Shell.cmd("tail -$TAIL_LINES ${shellQuote(DAEMON_LOG)} 2>/dev/null").exec()
        val lines = logResult.out.filter { it.isNotBlank() }

        // 解析挂载点数量（日志中形如 "fanotify mark: /sdcard" 或 "mount N paths"）
        val mountCount = lines.count {
            it.contains("fanotify", ignoreCase = true) && it.contains("mark", ignoreCase = true)
        }.coerceAtLeast(
            lines.firstOrNull { it.contains("mount", ignoreCase = true) }
                ?.let { Regex("(\\d+)").find(it)?.value?.toIntOrNull() } ?: 0
        )

        // 解析错误
        val errorLines = lines.filter { it.contains("ERROR", ignoreCase = true) || it.contains("error", ignoreCase = true) }
        val lastError = errorLines.lastOrNull() ?: ""

        // 启动时间（首行或含 start/init 的行）
        val startTime = lines.firstOrNull { it.contains("start", ignoreCase = true) || it.contains("init", ignoreCase = true) }
            ?: lines.firstOrNull() ?: ""

        DaemonStatus(
            running     = running,
            pid         = pid,
            mountCount  = mountCount,
            errorCount  = errorLines.size,
            lastError   = lastError,
            startTime   = startTime,
            recentLines = lines.takeLast(20),
        )
    }

    private fun shellQuote(v: String) = "'" + v.replace("'", "'\\''" ) + "'"
}
