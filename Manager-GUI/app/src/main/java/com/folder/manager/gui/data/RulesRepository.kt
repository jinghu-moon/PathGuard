package com.folder.manager.gui.data

import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream
import com.topjohnwu.superuser.io.SuFileOutputStream
import java.io.IOException

/**
 * 规则文件读写仓库，所有 root I/O 操作集中于此。
 * 使用 libsu SuFile 替代手动拼接 su -c 命令。
 */
object RulesRepository {

    private const val RULES_PATH  = "/data/adb/modules/folder_manager/config/rules.ini"
    private const val BACKUP_PATH = "/data/adb/modules/folder_manager/config/rules.ini.bak"
    private const val RELOAD_SCRIPT = "/data/adb/modules/folder_manager/bin/reload.sh"
    private const val DAEMON_LOG  = "/data/adb/modules/folder_manager/run/daemon.log"
    private const val SERVICE_LOG = "/data/adb/modules/folder_manager/run/service.log"

    // -------------------------------------------------------------------------
    // 读
    // -------------------------------------------------------------------------

    fun readRules(): Result<String> = runCatching {
        SuFileInputStream.open(SuFile(RULES_PATH)).bufferedReader().use { it.readText() }
    }

    fun readLog(type: LogType): Result<String> = runCatching {
        when (type) {
            LogType.DAEMON  -> SuFileInputStream.open(SuFile(DAEMON_LOG)).bufferedReader().use { it.readText() }
            LogType.SERVICE -> SuFileInputStream.open(SuFile(SERVICE_LOG)).bufferedReader().use { it.readText() }
            LogType.LOGCAT  -> {
                val result = Shell.cmd("logcat -d -s FolderManager").exec()
                result.out.joinToString("\n")
            }
        }
    }

    // -------------------------------------------------------------------------
    // 写
    // -------------------------------------------------------------------------

    /**
     * 备份 → 写入新内容 → 热重载。
     * @return Pair(saveOk, reloadOk)
     */
    fun saveAndReload(content: String): Result<Pair<Boolean, Boolean>> = runCatching {
        backup() // best-effort，失败不阻断
        writeRules(content).getOrThrow()
        val reloadOk = reload().isSuccess
        Pair(true, reloadOk)
    }

    fun reload(): Result<Unit> = runCatching {
        val result = Shell.cmd("sh ${shellQuote(RELOAD_SCRIPT)}").exec()
        if (!result.isSuccess) error(result.err.joinToString("\n").ifBlank { "reload 失败" })
    }

    fun backup(): Result<Unit> = runCatching {
        val result = Shell.cmd("cp ${shellQuote(RULES_PATH)} ${shellQuote(BACKUP_PATH)}").exec()
        if (!result.isSuccess) error(result.err.joinToString("\n").ifBlank { "备份失败" })
    }

    fun rollback(): Result<Unit> = runCatching {
        val cp = Shell.cmd("cp ${shellQuote(BACKUP_PATH)} ${shellQuote(RULES_PATH)}").exec()
        if (!cp.isSuccess) error(cp.err.joinToString("\n").ifBlank { "回滚复制失败" })
        reload().getOrThrow()
    }

    fun hitStats(): Result<String> = runCatching {
        val result = Shell.cmd("logcat -d -s FolderManager").exec()
        val output = result.out.joinToString("\n")
        val actionRegex = Regex("action=(block|redirect)")
        val totalRegex  = Regex("total=(\\d+).*block=(\\d+).*redirect=(\\d+)")
        var blockCount = 0; var redirectCount = 0
        var lastLine: String? = null
        output.lines().forEach { line ->
            when (actionRegex.find(line)?.groupValues?.get(1)) {
                "block"    -> blockCount++
                "redirect" -> redirectCount++
            }
            if (totalRegex.containsMatchIn(line)) lastLine = line
        }
        buildString {
            appendLine("logcat 统计（近似）")
            appendLine("block 行数：$blockCount")
            appendLine("redirect 行数：$redirectCount")
            lastLine?.let { appendLine("最新：$it") }
        }.trim()
    }

    // -------------------------------------------------------------------------
    // 内部
    // -------------------------------------------------------------------------

    private fun writeRules(content: String): Result<Unit> = runCatching {
        SuFileOutputStream.open(SuFile(RULES_PATH)).use { out ->
            out.write(content.toByteArray(Charsets.UTF_8))
        }
    }

    private fun shellQuote(v: String) = "'" + v.replace("'", "'\\''" ) + "'"

    enum class LogType { DAEMON, SERVICE, LOGCAT }
}
