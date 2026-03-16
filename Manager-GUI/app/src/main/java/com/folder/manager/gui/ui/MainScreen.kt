package com.folder.manager.gui.ui

import android.os.Handler
import android.os.Looper
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.folder.manager.gui.R
import com.folder.manager.gui.data.RulesRepository
import com.folder.manager.gui.data.RulesRepository.LogType
import java.util.concurrent.ExecutorService

// 内联图标，避免引入 material-icons-extended 依赖
private val IconCheck: ImageVector get() = ImageVector.Builder(
    defaultWidth = 24.dp, defaultHeight = 24.dp,
    viewportWidth = 24f, viewportHeight = 24f,
).path { moveTo(9f, 16.17f); lineTo(4.83f, 12f); lineTo(3.41f, 13.41f); lineTo(9f, 19f); lineTo(21f, 7f); lineTo(19.59f, 5.59f); close() }.build()

private val IconMoreVert: ImageVector get() = ImageVector.Builder(
    defaultWidth = 24.dp, defaultHeight = 24.dp,
    viewportWidth = 24f, viewportHeight = 24f,
).path { moveTo(12f, 8f); moveToRelative(-1f, 0f); arcToRelative(1f, 1f, 0f, true, false, 2f, 0f); arcToRelative(1f, 1f, 0f, true, false, -2f, 0f) }
 .path { moveTo(12f, 12f); moveToRelative(-1f, 0f); arcToRelative(1f, 1f, 0f, true, false, 2f, 0f); arcToRelative(1f, 1f, 0f, true, false, -2f, 0f) }
 .path { moveTo(12f, 16f); moveToRelative(-1f, 0f); arcToRelative(1f, 1f, 0f, true, false, 2f, 0f); arcToRelative(1f, 1f, 0f, true, false, -2f, 0f) }
 .build()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
    executor: ExecutorService,
    onPickDir: (callback: (String) -> Unit) -> Unit,
    onImport: (callback: (String) -> Unit) -> Unit,
    onExport: (getContent: () -> String) -> Unit,
    onGetPackages: () -> List<String>,
) {
    // ---- UI state -----------------------------------------------------------
    var rulesText      by remember { mutableStateOf("") }
    var statusMsg      by remember { mutableStateOf("待机") }
    var showMoreSheet  by remember { mutableStateOf(false) }
    var showLogSheet   by remember { mutableStateOf(false) }
    var showLogContent by remember { mutableStateOf("") }
    var showLogTitle   by remember { mutableStateOf("") }
    var showAppSheet   by remember { mutableStateOf(false) }
    var showTplSheet   by remember { mutableStateOf(false) }
    var showMsgDialog  by remember { mutableStateOf(false) }
    var msgTitle       by remember { mutableStateOf("") }
    var msgContent     by remember { mutableStateOf("") }
    var appFilterText  by remember { mutableStateOf("") }
    var mqExpanded     by remember { mutableStateOf(false) }

    val mqOptions = remember { listOf("auto", "true", "false") }
    var mqSelected by remember { mutableStateOf(mqOptions[0]) }
    val templates = remember { buildTemplates() }
    val mainHandler = remember { Handler(Looper.getMainLooper()) }

    // ---- helpers ------------------------------------------------------------
    fun post(block: () -> Unit) = mainHandler.post(block)

    fun runAsync(block: () -> Pair<String, String>) {
        executor.execute {
            val (ok, err) = block()
            post { statusMsg = if (err.isEmpty()) ok else err }
        }
    }

    fun showMsg(title: String, content: String) {
        msgTitle = title; msgContent = content; showMsgDialog = true
    }

    // ---- scaffold -----------------------------------------------------------
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.app_name)) },
                actions = {
                    IconButton(onClick = { showMoreSheet = true }) {
                        Icon(
                            imageVector = IconMoreVert,
                            contentDescription = "更多",
                        )
                    }
                },
            )
        },
        floatingActionButton = {
            FloatingActionButton(
                onClick = {
                    statusMsg = "保存中…"
                    runAsync {
                        RulesRepository.saveAndReload(rulesText).fold(
                            onSuccess = { (_, ok) -> Pair(if (ok) "保存并重载完成" else "保存成功，但重载失败", "") },
                            onFailure = { Pair("", "保存失败：${it.message}") },
                        )
                    }
                },
            ) {
                Icon(
                    imageVector = IconCheck,
                    contentDescription = "保存并重载",
                )
            }
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 12.dp),
        ) {
            // 状态提示
            Text(
                text = "状态：$statusMsg",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 4.dp),
            )

            // 加载 / 重载 按钮行
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Button(
                    onClick = {
                        statusMsg = "读取中…"
                        executor.execute {
                            val r = RulesRepository.readRules()
                            post {
                                r.onSuccess { rulesText = it; statusMsg = "读取完成" }
                                 .onFailure { statusMsg = "读取失败：${it.message}" }
                            }
                        }
                    },
                    modifier = Modifier.weight(1f),
                ) { Text(stringResource(R.string.action_load)) }

                Button(
                    onClick = {
                        statusMsg = "重载中…"
                        runAsync {
                            RulesRepository.reload().fold(
                                onSuccess = { Pair("重载完成", "") },
                                onFailure = { Pair("", "重载失败：${it.message}") },
                            )
                        }
                    },
                    modifier = Modifier.weight(1f),
                ) { Text(stringResource(R.string.action_reload)) }
            }

            Spacer(Modifier.height(8.dp))

            // media_query 下拉选择器
            ExposedDropdownMenuBox(
                expanded = mqExpanded,
                onExpandedChange = { mqExpanded = it },
            ) {
                OutlinedTextField(
                    value = mqSelected,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text(stringResource(R.string.media_query_label)) },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = mqExpanded) },
                    colors = ExposedDropdownMenuDefaults.outlinedTextFieldColors(),
                    modifier = Modifier
                        .fillMaxWidth()
                        .menuAnchor(MenuAnchorType.PrimaryNotEditable),
                )
                ExposedDropdownMenu(
                    expanded = mqExpanded,
                    onDismissRequest = { mqExpanded = false },
                ) {
                    mqOptions.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option) },
                            onClick = {
                                mqSelected = option
                                mqExpanded = false
                                statusMsg = "media_query 已选择：$option"
                            },
                        )
                    }
                }
            }

            Spacer(Modifier.height(8.dp))

            // 规则编辑器
            OutlinedTextField(
                value = rulesText,
                onValueChange = { rulesText = it },
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f),
                label = { Text(stringResource(R.string.rules_path_label)) },
                singleLine = false,
            )
        }
    }

    // ---- 更多操作 BottomSheet ------------------------------------------------
    if (showMoreSheet) {
        ModalBottomSheet(onDismissRequest = { showMoreSheet = false }) {
            Text(
                text = "更多操作",
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            MoreSheetItem("语法校验") {
                showMoreSheet = false
                val errors = RulesValidator.validate(rulesText)
                showMsg("语法校验", if (errors.isEmpty()) "未发现语法问题" else errors.joinToString("\n"))
            }
            MoreSheetItem("插入模板") { showMoreSheet = false; showTplSheet = true }
            MoreSheetItem("插入应用分组") { showMoreSheet = false; showAppSheet = true }
            MoreSheetItem("插入目录路径") {
                showMoreSheet = false
                onPickDir { path -> rulesText += "\n$path" }
            }
            MoreSheetItem("查看日志") {
                showMoreSheet = false
                executor.execute {
                    val r = RulesRepository.readLog(LogType.DAEMON)
                    post {
                        showLogTitle = "daemon.log"
                        showLogContent = r.getOrElse { "加载失败：${it.message}" }
                        showLogSheet = true
                    }
                }
            }
            MoreSheetItem("导入规则") { showMoreSheet = false; onImport { rulesText = it } }
            MoreSheetItem("导出规则") { showMoreSheet = false; onExport { rulesText } }
            MoreSheetItem("手动备份") {
                showMoreSheet = false
                runAsync {
                    RulesRepository.backup().fold(
                        onSuccess = { Pair("备份完成", "") },
                        onFailure = { Pair("", "备份失败：${it.message}") },
                    )
                }
            }
            MoreSheetItem("回滚备份") {
                showMoreSheet = false
                runAsync {
                    RulesRepository.rollback().fold(
                        onSuccess = { Pair("回滚完成", "") },
                        onFailure = { Pair("", "回滚失败：${it.message}") },
                    )
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 日志 BottomSheet ---------------------------------------------------
    if (showLogSheet) {
        ModalBottomSheet(onDismissRequest = { showLogSheet = false }) {
            Text(
                text = showLogTitle,
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            Text(
                text = showLogContent.ifBlank { "无内容" },
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp, vertical = 8.dp),
            )
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 模板选择 BottomSheet -----------------------------------------------
    if (showTplSheet) {
        ModalBottomSheet(onDismissRequest = { showTplSheet = false }) {
            Text(
                text = "插入模板",
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            templates.forEach { (name, body) ->
                MoreSheetItem(name) {
                    rulesText += "\n$body\n"
                    showTplSheet = false
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 应用选择 BottomSheet -----------------------------------------------
    if (showAppSheet) {
        val allPackages = remember { onGetPackages() }
        val filtered = remember(appFilterText) {
            if (appFilterText.isBlank()) allPackages
            else allPackages.filter { it.contains(appFilterText, ignoreCase = true) }
        }
        ModalBottomSheet(onDismissRequest = { showAppSheet = false; appFilterText = "" }) {
            Text(
                text = "选择应用",
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            OutlinedTextField(
                value = appFilterText,
                onValueChange = { appFilterText = it },
                label = { Text("搜索应用…") },
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp),
            )
            Spacer(Modifier.height(4.dp))
            Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                filtered.forEach { entry ->
                    val pkg = extractPackageName(entry) ?: return@forEach
                    ListItem(
                        headlineContent = { Text(entry.substringBefore(" (")) },
                        supportingContent = { Text(pkg, style = MaterialTheme.typography.labelSmall) },
                        modifier = Modifier.clickable {
                            rulesText += "\n[$pkg]\nmode = whitelist\nenabled = true\n"
                            showAppSheet = false
                            appFilterText = ""
                        },
                    )
                    HorizontalDivider()
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 消息 Dialog --------------------------------------------------------
    if (showMsgDialog) {
        AlertDialog(
            onDismissRequest = { showMsgDialog = false },
            title = { Text(msgTitle) },
            text = { Text(msgContent) },
            confirmButton = {
                TextButton(onClick = { showMsgDialog = false }) { Text("确定") }
            },
        )
    }
}

// ---------------------------------------------------------------------------
// 复用：BottomSheet 列表项
// ---------------------------------------------------------------------------

@Composable
private fun MoreSheetItem(title: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        modifier = Modifier.clickable(onClick = onClick),
    )
    HorizontalDivider()
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

private fun extractPackageName(text: String) =
    Regex("\\(([^)]+)\\)").find(text)?.groupValues?.getOrNull(1)?.trim()

private fun buildTemplates() = linkedMapOf(
    "微信白名单" to "[com.tencent.mm]\nmode = whitelist\nenabled = true\nmedia_query = auto\n\n+ DCIM/Camera\n+ Pictures/Share\n- DCIM/A-TEST\nDCIM/Camera -> Android/data/<pkg>/cache/Camera",
    "QQ 白名单" to "[com.tencent.mobileqq]\nmode = whitelist\nenabled = true\nmedia_query = auto\n\n+ DCIM/Camera\n+ Pictures/QQ\n- DCIM/敏感\nTencent/QQfile_recv -> Android/data/<pkg>/Tencent/QQfile_recv_hidden",
    "图片重定向" to "[com.example.app]\nmode = blacklist\nmedia_query = auto\n\nDCIM/Camera -> Android/data/<pkg>/cache/Camera",
    "全局黑名单" to "[*]\nmode = blacklist\nenabled = true\n\n- Android/data\n- Android/obb",
)

// ---------------------------------------------------------------------------
// 语法校验器（基础规则格式检查）
// ---------------------------------------------------------------------------

internal object RulesValidator {
    fun validate(text: String): List<String> {
        val errors = mutableListOf<String>()
        var inSection = false
        text.lines().forEachIndexed { i, raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) return@forEachIndexed
            when {
                line.startsWith("[") -> {
                    if (!line.endsWith("]")) errors.add("第 ${i + 1} 行：section 标题未闭合：$line")
                    inSection = true
                }
                line.contains("=") -> {
                    val key = line.substringBefore("=").trim()
                    if (key.isEmpty()) errors.add("第 ${i + 1} 行：键名为空：$line")
                }
                line.startsWith("+") || line.startsWith("-") -> {
                    if (!inSection) errors.add("第 ${i + 1} 行：规则在 section 外：$line")
                }
                line.contains("->") -> {
                    val parts = line.split("->")
                    if (parts.size != 2 || parts.any { it.trim().isEmpty() })
                        errors.add("第 ${i + 1} 行：重定向格式错误：$line")
                }
                else -> { /* 未知行，忽略 */ }
            }
        }
        return errors
    }
}
