package com.folder.manager.gui.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private val NordBlue   = Color(0xFF81A1C1)
private val NordGreen  = Color(0xFFA3BE8C)
private val NordOrange = Color(0xFFD08770)
private val NordPurple = Color(0xFFB48EAD)

data class RedirectFlow(
    val pkg: String,
    val src: String,
    val dst: String,
    val type: FlowType = FlowType.REDIRECT,
)

enum class FlowType { REDIRECT, EXPORT, DELETE }

fun parseRedirectFlows(rulesText: String): List<RedirectFlow> {
    val flows = mutableListOf<RedirectFlow>()
    var currentPkg = ""
    rulesText.lines().forEach { raw ->
        val line = raw.trim()
        when {
            line.startsWith("[") && line.endsWith("]") -> {
                val name = line.removeSurrounding("[", "]")
                currentPkg = when {
                    name.contains(".export.") -> name.substringBefore(".export.")
                    else -> name
                }
            }
            line.contains("->") && currentPkg.isNotEmpty() -> {
                val parts = line.split("->")
                if (parts.size == 2)
                    flows += RedirectFlow(currentPkg, parts[0].trim(), parts[1].trim(), FlowType.REDIRECT)
            }
            line.startsWith("delete_existing") && currentPkg.isNotEmpty() -> {
                flows += RedirectFlow(currentPkg, "(当前目录)", "(删除)", FlowType.DELETE)
            }
        }
    }
    return flows
}
@Composable
fun RedirectFlowCard(flow: RedirectFlow, modifier: Modifier = Modifier) {
    val typeColor = when (flow.type) {
        FlowType.REDIRECT -> NordBlue
        FlowType.EXPORT   -> NordGreen
        FlowType.DELETE   -> NordOrange
    }
    val typeLabel = when (flow.type) {
        FlowType.REDIRECT -> "重定向"
        FlowType.EXPORT   -> "导出"
        FlowType.DELETE   -> "删除"
    }
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.small,
        modifier = modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(flow.pkg, style = MaterialTheme.typography.labelSmall,
                color = NordPurple, fontWeight = FontWeight.Bold)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Surface(color = MaterialTheme.colorScheme.surface, shape = MaterialTheme.shapes.extraSmall) {
                    Text(flow.src, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp))
                }
                Canvas(modifier = Modifier.weight(1f).height(20.dp)) {
                    val y = size.height / 2f
                    drawLine(typeColor, Offset(0f, y), Offset(size.width - 12f, y), strokeWidth = 2f)
                    val arrowPath = Path().apply {
                        moveTo(size.width, y)
                        lineTo(size.width - 12f, y - 6f)
                        lineTo(size.width - 12f, y + 6f)
                        close()
                    }
                    drawPath(arrowPath, typeColor)
                }
                Surface(color = typeColor.copy(alpha = 0.15f), shape = MaterialTheme.shapes.extraSmall) {
                    Text(flow.dst, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                        color = typeColor, modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp))
                }
            }
            Text(typeLabel, style = MaterialTheme.typography.labelSmall, color = typeColor)
        }
    }
}
