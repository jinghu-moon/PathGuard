package com.folder.manager.gui.ui

/**
 * 规则测试沙盒：给定包名 + 路径，干运行规则文本，返回命中的规则描述。
 * 纯 Kotlin 实现，不调用 Root Shell，不实际拦截。
 */
object RuleSandbox {

    data class MatchResult(
        val matched: Boolean,
        val rule: String,       // 命中的规则行
        val action: String,     // ALLOW / BLOCK / REDIRECT / NO_MATCH
        val target: String = "", // redirect 目标（如有）
        val sectionMode: String = "",
    )

    /**
     * 模拟访问：在 [rulesText] 中查找 [pkg] section，
     * 按 mode(whitelist/blacklist) 语义匹配 [path]。
     */
    fun test(rulesText: String, pkg: String, path: String): MatchResult {
        val normalizedPath = path.trimStart('/')

        // 找到对应 section（支持通配 [*]）
        val lines = rulesText.lines()
        var inSection = false
        var sectionMode = "blacklist"
        var sectionEnabled = true
        val allowRules    = mutableListOf<String>()
        val blockRules    = mutableListOf<String>()
        val redirectRules = mutableListOf<Pair<String, String>>() // src -> dst

        for (line in lines) {
            val trimmed = line.trim()
            when {
                trimmed.startsWith("[") && trimmed.endsWith("]") -> {
                    val secPkg = trimmed.removeSurrounding("[", "]")
                    inSection = secPkg == pkg || secPkg == "*"
                    if (inSection) {
                        sectionMode = "blacklist"; sectionEnabled = true
                        allowRules.clear(); blockRules.clear(); redirectRules.clear()
                    }
                }
                !inSection -> Unit
                trimmed.startsWith("#") || trimmed.isEmpty() -> Unit
                trimmed.startsWith("mode") && trimmed.contains("=") ->
                    sectionMode = trimmed.substringAfter("=").trim()
                trimmed.startsWith("enabled") && trimmed.contains("=") ->
                    sectionEnabled = trimmed.substringAfter("=").trim() != "false"
                trimmed.startsWith("+") ->
                    allowRules += trimmed.removePrefix("+").trim()
                trimmed.startsWith("-") ->
                    blockRules += trimmed.removePrefix("-").trim()
                trimmed.contains("->") -> {
                    val parts = trimmed.split("->")
                    if (parts.size >= 2)
                        redirectRules += parts[0].trim() to parts[1].trim()
                }
            }
        }

        if (!inSection) return MatchResult(false, "", "NO_MATCH", sectionMode = "(no section found)")
        if (!sectionEnabled) return MatchResult(false, "", "NO_MATCH", sectionMode = "(section disabled)")

        // 优先检查 redirect
        for ((src, dst) in redirectRules) {
            if (pathMatches(normalizedPath, src)) {
                val resolvedDst = dst.replace("<pkg>", pkg)
                return MatchResult(true, "$src -> $dst", "REDIRECT", resolvedDst, sectionMode)
            }
        }

        // 检查 block
        for (rule in blockRules) {
            if (pathMatches(normalizedPath, rule)) {
                return MatchResult(true, "- $rule", "BLOCK", sectionMode = sectionMode)
            }
        }

        // 检查 allow
        for (rule in allowRules) {
            if (pathMatches(normalizedPath, rule)) {
                return MatchResult(true, "+ $rule", "ALLOW", sectionMode = sectionMode)
            }
        }

        // 未命中任何规则，按 mode 决定默认行为
        val defaultAction = if (sectionMode == "whitelist") "BLOCK" else "ALLOW"
        return MatchResult(false, "(默认 $sectionMode)", defaultAction, sectionMode = sectionMode)
    }

    /** 路径前缀匹配（不区分大小写） */
    private fun pathMatches(path: String, rule: String): Boolean {
        val r = rule.trimStart('/')
        return path.equals(r, ignoreCase = true) ||
               path.startsWith("$r/", ignoreCase = true)
    }
}
