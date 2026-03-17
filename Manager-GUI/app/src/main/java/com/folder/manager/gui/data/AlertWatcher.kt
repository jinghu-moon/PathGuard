package com.folder.manager.gui.data

import android.content.Context
import com.topjohnwu.superuser.Shell

/**
 * 监控 access.log，当命中「敏感路径」时触发通知。
 * 采用轮询（tail -f 模拟）：记录上次读取的行数，每次只处理新增行。
 */
object AlertWatcher {

    private const val ACCESS_LOG = "/data/adb/modules/folder_manager/run/access.log"

    // 用户配置的敏感路径关键词列表（持久化到 SharedPreferences）
    private const val PREFS_NAME    = "pathguard_alert"
    private const val KEY_PATHS     = "sensitive_paths"
    private const val KEY_ENABLED   = "alert_enabled"

    private var lastLineCount = 0

    fun isEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENABLED, false)

    fun setEnabled(context: Context, enabled: Boolean) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_ENABLED, enabled).apply()

    fun getSensitivePaths(context: Context): Set<String> =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getStringSet(KEY_PATHS, setOf("Documents", "Download", "DCIM")) ?: setOf()

    fun setSensitivePaths(context: Context, paths: Set<String>) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putStringSet(KEY_PATHS, paths).apply()

    /**
     * 检查新增日志行，对命中敏感路径的记录发送通知。
     * 应在后台线程调用（每 3 秒一次，与 access.log 轮询共用）。
     */
    fun checkNewEntries(context: Context) {
        if (!isEnabled(context)) return
        val sensitivePaths = getSensitivePaths(context)
        if (sensitivePaths.isEmpty()) return

        val result = Shell.cmd("wc -l < $ACCESS_LOG 2>/dev/null").exec()
        val currentCount = result.out.firstOrNull()?.trim()?.toIntOrNull() ?: return
        if (currentCount <= lastLineCount) { lastLineCount = currentCount; return }

        val newLines = currentCount - lastLineCount
        lastLineCount = currentCount

        val tailResult = Shell.cmd("tail -$newLines $ACCESS_LOG 2>/dev/null").exec()
        val logRegex = Regex("pkg=([\\w.]+).*?action=(\\w+).*?path=(.+)")

        tailResult.out.forEach { line ->
            val m = logRegex.find(line) ?: return@forEach
            val pkg    = m.groupValues[1]
            val action = m.groupValues[2]
            val path   = m.groupValues[3].trim()
            if (sensitivePaths.any { path.contains(it, ignoreCase = true) }) {
                NotificationHelper.sendAlert(context, pkg, path, action)
            }
        }
    }

    fun resetLineCount() { lastLineCount = 0 }
}
