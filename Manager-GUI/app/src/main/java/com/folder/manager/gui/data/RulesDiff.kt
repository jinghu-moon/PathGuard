package com.folder.manager.gui.data

/**
 * 规则文本 diff：比较两个版本，返回新增/删除行列表。
 */
object RulesDiff {

    enum class DiffType { ADDED, REMOVED, UNCHANGED }

    data class DiffLine(
        val type: DiffType,
        val line: String,
        val lineNo: Int,
    )

    data class DiffResult(
        val added: Int,
        val removed: Int,
        val lines: List<DiffLine>,
    )

    fun diff(oldText: String, newText: String): DiffResult {
        val oldLines = oldText.lines()
        val newLines = newText.lines()
        val result = mutableListOf<DiffLine>()

        // 简单 LCS-based diff（Myers 简化版）
        val lcs = lcs(oldLines, newLines)
        var oi = 0; var ni = 0; var li = 0

        while (oi < oldLines.size || ni < newLines.size) {
            when {
                li < lcs.size && oi < oldLines.size && oldLines[oi] == lcs[li] &&
                ni < newLines.size && newLines[ni] == lcs[li] -> {
                    result += DiffLine(DiffType.UNCHANGED, oldLines[oi], oi + 1)
                    oi++; ni++; li++
                }
                oi < oldLines.size && (li >= lcs.size || oldLines[oi] != lcs[li]) -> {
                    result += DiffLine(DiffType.REMOVED, oldLines[oi], oi + 1)
                    oi++
                }
                ni < newLines.size -> {
                    result += DiffLine(DiffType.ADDED, newLines[ni], ni + 1)
                    ni++
                }
                else -> break
            }
        }

        return DiffResult(
            added   = result.count { it.type == DiffType.ADDED },
            removed = result.count { it.type == DiffType.REMOVED },
            lines   = result,
        )
    }

    private fun lcs(a: List<String>, b: List<String>): List<String> {
        val m = a.size; val n = b.size
        val dp = Array(m + 1) { IntArray(n + 1) }
        for (i in 1..m) for (j in 1..n) {
            dp[i][j] = if (a[i-1] == b[j-1]) dp[i-1][j-1] + 1
                       else maxOf(dp[i-1][j], dp[i][j-1])
        }
        val result = mutableListOf<String>()
        var i = m; var j = n
        while (i > 0 && j > 0) {
            when {
                a[i-1] == b[j-1] -> { result.add(0, a[i-1]); i--; j-- }
                dp[i-1][j] >= dp[i][j-1] -> i--
                else -> j--
            }
        }
        return result
    }
}
