package com.folder.manager.gui.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * 图形化单条规则添加面板。
 * 支持 BLOCK / ALLOW / REDIRECT / EXPORT / DELETE 五种规则类型。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RuleEditorSheet(
    packages: List<String>,
    existingRules: String = "",
    onPickDir: (callback: (String) -> Unit) -> Unit,
    onInsert: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var pkgFilter    by remember { mutableStateOf("") }
    var pkgExpanded  by remember { mutableStateOf(false) }
    var selectedPkg  by remember { mutableStateOf("") }
    var ruleType     by remember { mutableStateOf(RuleType.BLOCK) }
    var srcPath      by remember { mutableStateOf("") }
    var dstPath      by remember { mutableStateOf("") }
    var isDirectory  by remember { mutableStateOf(true) }
    var extensions      by remember { mutableStateOf("") }
    var trashOnRedirect by remember { mutableStateOf(false) }
    var exportRuleId    by remember { mutableStateOf("") }
    var mediaScan       by remember { mutableStateOf(false) }
    var addToDownloads  by remember { mutableStateOf(false) }
    var allowChild      by remember { mutableStateOf(false) }
    var deleteExisting  by remember { mutableStateOf(false) }
    var trashEnabled    by remember { mutableStateOf(false) }
    var minAgeDays      by remember { mutableStateOf("") }
    var minSizeMb       by remember { mutableStateOf("") }

    val filteredPkgs = remember(pkgFilter, packages) {
        if (pkgFilter.isBlank()) packages
        else packages.filter { it.contains(pkgFilter, ignoreCase = true) }
    }
    val historyDstPaths = remember(existingRules) {
        Regex("->\\s*(.+)").findAll(existingRules)
            .map { it.groupValues[1].trim() }.distinct().take(10).toList()
    }
    var dstExpanded by remember { mutableStateOf(false) }
    val conflictWarning = remember(selectedPkg, ruleType, srcPath, existingRules) {
        detectConflict(selectedPkg, ruleType, srcPath, existingRules)
    }
    val preview = remember(selectedPkg, ruleType, srcPath, dstPath, isDirectory,
        extensions, trashOnRedirect, exportRuleId, mediaScan, addToDownloads, allowChild,
        deleteExisting, trashEnabled, minAgeDays, minSizeMb) {
        buildPreview(selectedPkg, ruleType, srcPath, dstPath, isDirectory,
            extensions, trashOnRedirect, exportRuleId, mediaScan, addToDownloads, allowChild,
            deleteExisting, trashEnabled, minAgeDays, minSizeMb)
    }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp).padding(bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("图形化添加规则", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(vertical = 4.dp))

            ExposedDropdownMenuBox(expanded = pkgExpanded, onExpandedChange = { pkgExpanded = it }) {
                OutlinedTextField(
                    value = pkgFilter, onValueChange = { pkgFilter = it; selectedPkg = it },
                    label = { Text("应用包名") }, singleLine = true,
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = pkgExpanded) },
                    modifier = Modifier.fillMaxWidth().menuAnchor(ExposedDropdownMenuAnchorType.PrimaryEditable),
                )
                if (filteredPkgs.isNotEmpty()) {
                    ExposedDropdownMenu(expanded = pkgExpanded, onDismissRequest = { pkgExpanded = false }) {
                        filteredPkgs.take(20).forEach { entry ->
                            DropdownMenuItem(
                                text = { Text(entry, style = MaterialTheme.typography.bodySmall) },
                                onClick = {
                                    val pkg = extractPkg(entry) ?: entry
                                    selectedPkg = pkg; pkgFilter = pkg; pkgExpanded = false
                                },
                            )
                        }
                    }
                }
            }

            Text("规则类型", style = MaterialTheme.typography.labelMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
                RuleType.entries.forEach { type ->
                    FilterChip(selected = ruleType == type, onClick = { ruleType = type },
                        label = { Text(type.label, style = MaterialTheme.typography.labelSmall) })
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = srcPath, onValueChange = { srcPath = it },
                    label = { Text(if (ruleType == RuleType.EXPORT) "源路径（App私有目录）" else "源路径") },
                    singleLine = true, modifier = Modifier.weight(1f),
                )
                OutlinedButton(onClick = { onPickDir { srcPath = it } }) { Text("浏览") }
            }

            AnimatedVisibility(ruleType == RuleType.REDIRECT || ruleType == RuleType.EXPORT) {
                ExposedDropdownMenuBox(expanded = dstExpanded, onExpandedChange = { dstExpanded = it }) {
                    OutlinedTextField(
                        value = dstPath, onValueChange = { dstPath = it },
                        label = { Text(if (ruleType == RuleType.EXPORT) "目标路径（公共目录）" else "目标路径") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth().menuAnchor(ExposedDropdownMenuAnchorType.PrimaryEditable),
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(dstExpanded) },
                    )
                    if (historyDstPaths.isNotEmpty()) {
                        ExposedDropdownMenu(expanded = dstExpanded, onDismissRequest = { dstExpanded = false }) {
                            historyDstPaths.forEach { path ->
                                DropdownMenuItem(
                                    text = { Text(path, style = MaterialTheme.typography.bodySmall) },
                                    onClick = { dstPath = path; dstExpanded = false },
                                )
                            }
                        }
                    }
                }
            }

            AnimatedVisibility(ruleType == RuleType.REDIRECT) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = extensions, onValueChange = { extensions = it },
                        label = { Text("扩展名过滤（逗号分隔，空=全部）") },
                        placeholder = { Text("jpg,png,mp4") },
                        singleLine = true, modifier = Modifier.fillMaxWidth(),
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = trashOnRedirect, onCheckedChange = { trashOnRedirect = it })
                        Text("移动时保留副本到回收站")
                    }
                }
            }

            AnimatedVisibility(ruleType == RuleType.EXPORT) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    OutlinedTextField(
                        value = exportRuleId, onValueChange = { exportRuleId = it },
                        label = { Text("规则 ID（如 share_photos）") },
                        singleLine = true, modifier = Modifier.fillMaxWidth(),
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = mediaScan, onCheckedChange = { mediaScan = it })
                        Text("触发媒体扫描 (media_scan)")
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = addToDownloads, onCheckedChange = { addToDownloads = it })
                        Text("添加到下载列表 (add_to_downloads)")
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = allowChild, onCheckedChange = { allowChild = it })
                        Text("允许子目录访问 (allow_child)")
                    }
                }
            }

            AnimatedVisibility(ruleType == RuleType.DELETE) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = deleteExisting, onCheckedChange = { deleteExisting = it })
                        Text("立即删除已存在文件 (delete_existing)")
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = trashEnabled, onCheckedChange = { trashEnabled = it })
                        Text("放入回收站而非直接删除 (trash_enabled)")
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = minAgeDays, onValueChange = { minAgeDays = it },
                            label = { Text("最小年龄（天）") }, placeholder = { Text("0") },
                            singleLine = true, modifier = Modifier.weight(1f),
                        )
                        OutlinedTextField(
                            value = minSizeMb, onValueChange = { minSizeMb = it },
                            label = { Text("最小大小（MB）") }, placeholder = { Text("0") },
                            singleLine = true, modifier = Modifier.weight(1f),
                        )
                    }
                }
            }

            AnimatedVisibility(ruleType != RuleType.EXPORT) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = isDirectory, onCheckedChange = { isDirectory = it })
                    Text("目录匹配（末尾自动补 /）")
                }
            }
            // 冲突实时提示
            AnimatedVisibility(conflictWarning.isNotEmpty()) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(conflictWarning, style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        modifier = Modifier.padding(8.dp))
                }
            }

            // 预览
            if (preview.isNotBlank()) {
                Surface(color = MaterialTheme.colorScheme.surfaceVariant,
                    shape = MaterialTheme.shapes.small, modifier = Modifier.fillMaxWidth()) {
                    Text(text = preview, style = MaterialTheme.typography.bodySmall,
                        fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                        modifier = Modifier.padding(8.dp))
                }
            }

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = onDismiss, modifier = Modifier.weight(1f)) { Text("取消") }
                Button(
                    onClick = { if (preview.isNotBlank()) { onInsert(preview); onDismiss() } },
                    enabled = preview.isNotBlank(),
                    modifier = Modifier.weight(1f),
                ) { Text("插入") }
            }
        }
    }
}

enum class RuleType(val label: String) {
    BLOCK("block"), ALLOW("allow"), REDIRECT("redirect→"), EXPORT("export↗"), DELETE("delete🗑"),
}

private fun buildPreview(
    pkg: String, type: RuleType, src: String, dst: String, isDir: Boolean,
    extensions: String, trashOnRedirect: Boolean,
    exportRuleId: String, mediaScan: Boolean, addToDownloads: Boolean, allowChild: Boolean,
    deleteExisting: Boolean, trashEnabled: Boolean, minAgeDays: String, minSizeMb: String,
): String {
    if (pkg.isBlank() || src.isBlank()) return ""
    val normalSrc = src.trimEnd('/')
    val dirSuffix = if (isDir) "/" else ""
    return when (type) {
        RuleType.BLOCK  -> "- $normalSrc$dirSuffix"
        RuleType.ALLOW  -> "+ $normalSrc$dirSuffix"
        RuleType.REDIRECT -> {
            val normalDst = dst.trimEnd('/')
            if (normalDst.isBlank()) return ""
            val extLine = if (extensions.isNotBlank()) "\nextensions = ${extensions.replace(" ", "")}" else ""
            val trashLine = if (trashOnRedirect) "\ntrash_on_redirect = true" else ""
            "$normalSrc -> $normalDst$extLine$trashLine"
        }
        RuleType.EXPORT -> {
            val normalDst = dst.trimEnd('/')
            if (normalDst.isBlank() || exportRuleId.isBlank()) return ""
            val id = exportRuleId.replace(" ", "_")
            buildString {
                appendLine("[$pkg.export.$id]")
                appendLine("source = $normalSrc")
                appendLine("target = $normalDst")
                if (mediaScan)      appendLine("media_scan = true")
                if (addToDownloads) appendLine("add_to_downloads = true")
                if (allowChild)     appendLine("allow_child = true")
            }.trimEnd()
        }
        RuleType.DELETE -> {
            buildString {
                if (deleteExisting) appendLine("delete_existing = true")
                if (trashEnabled)   appendLine("trash_enabled = true")
                val days = minAgeDays.trim().toIntOrNull() ?: 0
                val mb   = minSizeMb.trim().toIntOrNull() ?: 0
                if (days > 0) appendLine("min_age_days = $days")
                if (mb > 0)   appendLine("min_size_mb = $mb")
                append("! $normalSrc${dirSuffix}")
            }
        }
    }
}

private fun detectConflict(pkg: String, type: RuleType, src: String, rules: String): String {
    if (pkg.isBlank() || src.isBlank()) return ""
    val normalSrc = src.trimEnd('/')
    var inPkg = false
    for (line in rules.lines()) {
        val t = line.trim()
        if (t.startsWith("[") && t.endsWith("]")) {
            inPkg = t.removeSurrounding("[", "]") == pkg
        }
        if (!inPkg) continue
        when (type) {
            RuleType.BLOCK -> {
                if (t.startsWith("+ ") && pathOverlaps(t.removePrefix("+ ").trim(), normalSrc))
                    return "⚠ 与已有 allow 规则冲突：$t"
            }
            RuleType.ALLOW -> {
                if (t.startsWith("- ") && pathOverlaps(t.removePrefix("- ").trim(), normalSrc))
                    return "⚠ 与已有 block 规则冲突：$t"
            }
            RuleType.REDIRECT -> {
                if (t.startsWith("- ") && pathOverlaps(t.removePrefix("- ").trim(), normalSrc))
                    return "⚠ 与已有 block 规则路径重叠：$t"
            }
            else -> {}
        }
    }
    return ""
}

private fun pathOverlaps(a: String, b: String): Boolean {
    val a2 = a.trimEnd('/')
    val b2 = b.trimEnd('/')
    return a2 == b2 || a2.startsWith("$b2/") || b2.startsWith("$a2/")
}

private fun extractPkg(text: String) =
    Regex("""\(([^)]+)\)""").find(text)?.groupValues?.getOrNull(1)?.trim()
