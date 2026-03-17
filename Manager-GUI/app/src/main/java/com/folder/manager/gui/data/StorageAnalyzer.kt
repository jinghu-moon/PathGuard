package com.folder.manager.gui.data

import com.topjohnwu.superuser.Shell

/**
 * 分析应用存储占用 + 重定向目标目录管理。
 */
object StorageAnalyzer {

    private const val ANDROID_DATA = "/data/media/0/Android/data"
    private const val REDIRECT_BASE = "/data/adb/modules/folder_manager"

    data class AppStorage(
        val pkg: String,
        val dataSize: String,   // human-readable, e.g. "12M"
        val dataSizeBytes: Long,
    )

    data class RedirectEntry(
        val src: String,
        val dst: String,
        val pkg: String,
        val size: String,
        val sizeBytes: Long,
    )

    /** 统计各 App 的 Android/data/<pkg>/ 占用 */
    fun analyzeAppStorage(pkgs: List<String>): Result<List<AppStorage>> = runCatching {
        pkgs.mapNotNull { pkg ->
            val r = Shell.cmd("du -sb $ANDROID_DATA/$pkg 2>/dev/null | awk '{print \$1}'").exec()
            val bytes = r.out.firstOrNull()?.trim()?.toLongOrNull() ?: return@mapNotNull null
            AppStorage(pkg = pkg, dataSize = formatBytes(bytes), dataSizeBytes = bytes)
        }.sortedByDescending { it.dataSizeBytes }
    }

    /** 从规则文本提取所有重定向条目，查询其目标目录大小 */
    fun analyzeRedirects(rulesText: String): Result<List<RedirectEntry>> = runCatching {
        val redirectRegex = Regex("^([^#\\[].+?)\\s*->\\s*(.+)$", RegexOption.MULTILINE)
        var currentPkg = ""
        val entries = mutableListOf<RedirectEntry>()

        rulesText.lines().forEach { line ->
            val trimmed = line.trim()
            if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                currentPkg = trimmed.removeSurrounding("[", "]")
            } else {
                val m = redirectRegex.find(trimmed) ?: return@forEach
                val src = m.groupValues[1].trim()
                val dstTemplate = m.groupValues[2].trim()
                val dst = dstTemplate.replace("<pkg>", currentPkg)
                val fullDst = "/data/media/0/$dst"
                val r = Shell.cmd("du -sb $fullDst 2>/dev/null | awk '{print \$1}'").exec()
                val bytes = r.out.firstOrNull()?.trim()?.toLongOrNull() ?: 0L
                entries += RedirectEntry(src, dst, currentPkg, formatBytes(bytes), bytes)
            }
        }
        entries
    }

    /** 清空重定向目标目录内容（保留目录本身） */
    fun clearRedirectTarget(dst: String): Result<Unit> = runCatching {
        val fullDst = if (dst.startsWith("/")) dst else "/data/media/0/$dst"
        Shell.cmd("rm -rf ${shellQuote(fullDst)}/* 2>/dev/null").exec()
    }

    /** 将重定向数据迁移回原路径 */
    fun migrateBackToSource(src: String, dst: String, pkg: String): Result<Unit> = runCatching {
        val fullSrc = "/data/media/0/$src"
        val fullDst = if (dst.startsWith("/")) dst else "/data/media/0/$dst"
        Shell.cmd("mkdir -p ${shellQuote(fullSrc)} && cp -a ${shellQuote(fullDst)}/. ${shellQuote(fullSrc)}/ 2>/dev/null").exec()
    }

    private fun formatBytes(bytes: Long): String = when {
        bytes >= 1_073_741_824L -> "%.1fG".format(bytes / 1_073_741_824.0)
        bytes >= 1_048_576L     -> "%.1fM".format(bytes / 1_048_576.0)
        bytes >= 1_024L         -> "%.1fK".format(bytes / 1_024.0)
        else                    -> "${bytes}B"
    }

    private fun shellQuote(v: String) = "'" + v.replace("'", "'\\''" ) + "'"
}
