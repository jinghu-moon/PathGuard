package com.folder.manager.gui.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.folder.manager.gui.data.RulesRepository.HitStat

/**
 * 基于 Canvas 绘制命中统计柱状图（按包名，不引入第三方库）。
 */
@Composable
fun HitStatsBarChart(
    stats: List<HitStat>,
    modifier: Modifier = Modifier,
) {
    if (stats.isEmpty()) {
        Box(modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) {
            Text("暂无统计数据", style = MaterialTheme.typography.bodyMedium)
        }
        return
    }

    // 按包名聚合 block+allow+redirect
    data class PkgStat(val pkg: String, val block: Int, val allow: Int, val redirect: Int) {
        val total get() = block + allow + redirect
    }
    val grouped = stats.groupBy { it.pkg }.map { (pkg, list) ->
        PkgStat(
            pkg      = pkg.substringAfterLast('.'),  // 只显示末段包名
            block    = list.filter { it.action == "block" }.sumOf { it.count },
            allow    = list.filter { it.action == "allow" }.sumOf { it.count },
            redirect = list.filter { it.action == "redirect" }.sumOf { it.count },
        )
    }.sortedByDescending { it.total }.take(12)

    val maxVal = grouped.maxOf { it.total }.coerceAtLeast(1).toFloat()
    val barWidth = 36.dp
    val chartHeight = 160.dp
    val labelHeight = 48.dp
    val totalWidth = (barWidth + 8.dp) * grouped.size + 16.dp

    val blockColor   = Color(0xFFbf616a)   // Nord11
    val allowColor   = Color(0xFFa3be8c)   // Nord14
    val redirectColor = Color(0xFFb48ead)  // Nord15

    val textMeasurer = rememberTextMeasurer()
    val labelStyle = TextStyle(fontSize = 9.sp)

    Column(modifier = modifier) {
        // 图例
        Row(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            LegendDot(blockColor,    "block")
            LegendDot(allowColor,    "allow")
            LegendDot(redirectColor, "redirect")
        }
        Box(modifier = Modifier.horizontalScroll(rememberScrollState())) {
            Canvas(
                modifier = Modifier
                    .width(totalWidth)
                    .height(chartHeight + labelHeight)
            ) {
                val chartH = chartHeight.toPx()
                val bw = barWidth.toPx()
                val gap = 8.dp.toPx()
                val paddingLeft = 8.dp.toPx()

                grouped.forEachIndexed { i, stat ->
                    val x = paddingLeft + i * (bw + gap)
                    val totalH = (stat.total / maxVal) * chartH
                    val blockH = (stat.block / maxVal) * chartH
                    val allowH = (stat.allow / maxVal) * chartH
                    val redirH = (stat.redirect / maxVal) * chartH

                    // 堆叠柱：block 在底，allow 在中，redirect 在上
                    var yOffset = chartH
                    if (blockH > 0) {
                        yOffset -= blockH
                        drawRect(blockColor, Offset(x, yOffset), Size(bw, blockH))
                    }
                    if (allowH > 0) {
                        yOffset -= allowH
                        drawRect(allowColor, Offset(x, yOffset), Size(bw, allowH))
                    }
                    if (redirH > 0) {
                        yOffset -= redirH
                        drawRect(redirectColor, Offset(x, yOffset), Size(bw, redirH))
                    }

                    // 包名标签（旋转 -45° 近似：竖排截断）
                    val measured = textMeasurer.measure(stat.pkg, labelStyle)
                    drawText(
                        textMeasurer = textMeasurer,
                        text = stat.pkg.take(8),
                        style = labelStyle,
                        topLeft = Offset(x, chartH + 4.dp.toPx()),
                    )

                    // 总数标签
                    if (stat.total > 0) {
                        drawText(
                            textMeasurer = textMeasurer,
                            text = "${stat.total}",
                            style = labelStyle,
                            topLeft = Offset(x, chartH - totalH - 14.dp.toPx()),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun LegendDot(color: Color, label: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        Canvas(modifier = Modifier.size(10.dp)) { drawCircle(color) }
        Text(label, style = MaterialTheme.typography.labelSmall)
    }
}
