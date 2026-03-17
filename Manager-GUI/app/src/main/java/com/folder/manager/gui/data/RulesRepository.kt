package com.folder.manager.gui.data

import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream
import com.topjohnwu.superuser.io.SuFileOutputStream

/**
 * 规则文件读写仓库，所有 root I/O 操作集中于此。
 * 使用 libsu SuFile 替代手动拼接 su -c 命令。
 */
object RulesRepository {

    private const val RULES_PATH    = "/data/adb/modules/folder_manager/config/rules.ini"
    private const val BACKUP_PATH   = "/data/adb/modules/folder_manager/config/rules.ini.bak"
    private const val BACKUP_DIR    = "/data/adb/modules/folder_manager/config"
    private const val MODULE_CONF   = "/data/adb/modules/folder_manager/config/module.conf"
    private const val MODULE_PROP   = "/data/adb/modules/folder_manager/module.prop"
    private const val RELOAD_SCRIPT = "/data/adb/modules/folder_manager/bin/reload.sh"
    private const val DAEMON_LOG    = "/data/adb/modules/folder_manager/run/daemon.log"
    private const val SERVICE_LOG   = "/data/adb/modules/folder_manager/run/service.log"
    private const val ACCESS_LOG    = "/data/adb/modules/folder_manager/run/access.log"

    // -------------------------------------------------------------------------
    // 数据类
    // -------------------------------------------------------------------------

    data class HitStat(val pkg: String, val action: String, val count: Int)

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
            LogType.ACCESS  -> SuFileInputStream.open(SuFile(ACCESS_LOG)).bufferedReader().use { it.readText() }
            LogType.LOGCAT  -> Shell.cmd("logcat -d -s FolderManager").exec().out.joinToString("\n")
        }
    }

    /**
     * 解析 access.log，按 pkg+action 聚合命中次数。
     * 日志格式：timestamp pid=N pkg=X op=Y action=Z path=P
     */
    fun readHitStats(): Result<List<HitStat>> = runCatching {
        val raw = SuFileInputStream.open(SuFile(ACCESS_LOG)).bufferedReader().use { it.readText() }
        val regex = Regex("pkg=([\\w.]+).*?action=(\\w+)")
        val counts = mutableMapOf<Pair<String, String>, Int>()
        raw.lines().forEach { line ->
            val m = regex.find(line) ?: return@forEach
            val key = Pair(m.groupValues[1], m.groupValues[2])
            counts[key] = (counts[key] ?: 0) + 1
        }
        counts.map { (k, v) -> HitStat(k.first, k.second, v) }
            .sortedByDescending { it.count }
    }

    // -------------------------------------------------------------------------
    // module.conf
    // -------------------------------------------------------------------------

    /**
     * 读取 module.prop，解析为 key→value map。
     */
    fun readModuleProp(): Result<Map<String, String>> = runCatching {
        SuFileInputStream.open(SuFile(MODULE_PROP)).bufferedReader().use { it.readText() }
            .lines()
            .filter { it.contains("=") && !it.trimStart().startsWith("#") }
            .associate { line ->
                val idx = line.indexOf('=')
                line.substring(0, idx).trim() to line.substring(idx + 1).trim()
            }
    }

    fun readModuleConf(): Result<String> = runCatching {
        SuFileInputStream.open(SuFile(MODULE_CONF)).bufferedReader().use { it.readText() }
    }

    fun saveModuleConf(content: String): Result<Unit> = runCatching {
        SuFileOutputStream.open(SuFile(MODULE_CONF)).use { out ->
            out.write(content.toByteArray(Charsets.UTF_8))
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

    /**
     * 带时间戳备份，保留最近 5 份，多余的自动删除。
     */
    fun backupWithTimestamp(): Result<String> = runCatching {
        val ts = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
            .format(java.util.Date())
        val dst = "$BACKUP_DIR/rules.ini.bak.$ts"
        val cp = Shell.cmd("cp ${shellQuote(RULES_PATH)} ${shellQuote(dst)}").exec()
        if (!cp.isSuccess) error(cp.err.joinToString("\n").ifBlank { "时间戳备份失败" })
        // 只保留最近 5 份
        val ls = Shell.cmd("ls -t ${shellQuote(BACKUP_DIR)}/rules.ini.bak.* 2>/dev/null").exec()
        ls.out.drop(5).forEach { old ->
            Shell.cmd("rm -f ${shellQuote(old.trim())}").exec()
        }
        dst
    }

    /** 列出所有时间戳备份，按时间倒序。 */
    fun listBackups(): Result<List<String>> = runCatching {
        val ls = Shell.cmd("ls -t ${shellQuote(BACKUP_DIR)}/rules.ini.bak.* 2>/dev/null").exec()
        ls.out.map { it.trim() }.filter { it.isNotBlank() }
    }

    /** 回滚到指定备份文件。 */
    fun rollbackToBackup(backupPath: String): Result<Unit> = runCatching {
        val cp = Shell.cmd("cp ${shellQuote(backupPath)} ${shellQuote(RULES_PATH)}").exec()
        if (!cp.isSuccess) error(cp.err.joinToString("\n").ifBlank { "回滚复制失败" })
        reload().getOrThrow()
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

    enum class LogType { DAEMON, SERVICE, ACCESS, LOGCAT }
}
