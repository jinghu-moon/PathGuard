package com.folder.manager.gui.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.OffsetMapping
import androidx.compose.ui.text.input.TransformedText
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.withStyle

/**
 * Nord 配色 INI 规则语法高亮。
 *
 * 配色映射（Nord 调色板）：
 *   [section]  nord8  #88c0d0  Bold    — 经典 Nord 蓝，关键字
 *   + allow    nord14 #a3be8c          — 绿，成功/允许
 *   - block    nord11 #bf616a          — 红，错误/屏蔽
 *   ->redirect nord15 #b48ead          — 紫，特殊/重定向
 *   # comment  nord3  #4c566a  Italic  — 注释文字
 *   key =      nord9  #81a1c1          — 深蓝灰，键名
 *   = value    nord13 #ebcb8b          — 金黄，值
 */
object NordColors {
    val comment  = Color(0xFF4c566a) // nord3
    val section  = Color(0xFF88c0d0) // nord8
    val allow    = Color(0xFFa3be8c) // nord14
    val block    = Color(0xFFbf616a) // nord11
    val redirect = Color(0xFFb48ead) // nord15
    val key      = Color(0xFF81a1c1) // nord9
    val value    = Color(0xFFebcb8b) // nord13
}

class RulesSyntaxHighlighter : VisualTransformation {

    override fun filter(text: AnnotatedString): TransformedText {
        val annotated = buildAnnotatedString {
            val lines = text.text.split("\n")
            lines.forEachIndexed { idx, line ->
                if (idx > 0) append("\n")
                highlightLine(line)
            }
        }
        return TransformedText(annotated, OffsetMapping.Identity)
    }

    private fun AnnotatedString.Builder.highlightLine(line: String) {
        val trimmed = line.trimStart()
        when {
            // 注释行：# ...
            trimmed.startsWith("#") -> {
                withStyle(SpanStyle(color = NordColors.comment, fontStyle = FontStyle.Italic)) {
                    append(line)
                }
            }
            // section 头：[pkg.name]
            trimmed.startsWith("[") && trimmed.contains("]") -> {
                withStyle(SpanStyle(color = NordColors.section, fontWeight = FontWeight.Bold)) {
                    append(line)
                }
            }
            // 允许规则：+ path
            trimmed.startsWith("+") -> {
                withStyle(SpanStyle(color = NordColors.allow)) {
                    append(line)
                }
            }
            // 屏蔽规则：- path
            trimmed.startsWith("-") -> {
                withStyle(SpanStyle(color = NordColors.block)) {
                    append(line)
                }
            }
            // 重定向：src -> dst
            trimmed.contains("->") -> {
                val arrowIdx = line.indexOf("->")
                // src 部分用 block 色，-> 及 dst 用 redirect 色
                withStyle(SpanStyle(color = NordColors.block)) {
                    append(line.substring(0, arrowIdx))
                }
                withStyle(SpanStyle(color = NordColors.redirect)) {
                    append(line.substring(arrowIdx))
                }
            }
            // 键值对：key = value
            trimmed.contains("=") -> {
                val eqIdx = line.indexOf("=")
                withStyle(SpanStyle(color = NordColors.key)) {
                    append(line.substring(0, eqIdx + 1))
                }
                withStyle(SpanStyle(color = NordColors.value)) {
                    append(line.substring(eqIdx + 1))
                }
            }
            // 其他行：默认颜色
            else -> append(line)
        }
    }
}
