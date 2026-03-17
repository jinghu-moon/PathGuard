package com.folder.manager.gui.ui

/**
 * 解析规则文本，提取每个 [pkg] section 的摘要信息。
 */
object RuleSectionParser {

    data class RuleSection(
        val pkg: String,
        val mode: String,
        val enabled: Boolean,
        val allowCount: Int,
        val blockCount: Int,
        val redirectCount: Int,
        val lineStart: Int,
    )

    fun parse(text: String): List<RuleSection> {
        val sections = mutableListOf<RuleSection>()
        var currentPkg: String? = null
        var currentMode = ""
        var currentEnabled = true
        var allowCount = 0
        var blockCount = 0
        var redirectCount = 0
        var lineStart = 0

        fun flush() {
            val pkg = currentPkg ?: return
            sections += RuleSection(
                pkg          = pkg,
                mode         = currentMode,
                enabled      = currentEnabled,
                allowCount   = allowCount,
                blockCount   = blockCount,
                redirectCount = redirectCount,
                lineStart    = lineStart,
            )
        }

        text.lines().forEachIndexed { i, raw ->
            val line = raw.trim()
            when {
                line.startsWith("[") && line.endsWith("]") -> {
                    flush()
                    currentPkg     = line.removeSurrounding("[", "]")
                    currentMode    = ""
                    currentEnabled = true
                    allowCount     = 0
                    blockCount     = 0
                    redirectCount  = 0
                    lineStart      = i
                }
                line.startsWith("mode") && line.contains("=") ->
                    currentMode = line.substringAfter("=").trim()
                line.startsWith("enabled") && line.contains("=") ->
                    currentEnabled = line.substringAfter("=").trim() != "false"
                line.startsWith("+") -> allowCount++
                line.startsWith("-") -> blockCount++
                line.contains("->") && !line.startsWith("#") -> redirectCount++
            }
        }
        flush()
        return sections
    }
}
