package com.folder.manager.gui.ui

import android.os.Handler
import android.os.Looper
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.folder.manager.gui.R
import com.folder.manager.gui.data.PresetRepository
import com.folder.manager.gui.data.RulesRepository
import com.folder.manager.gui.data.RulesRepository.HitStat
import com.folder.manager.gui.data.RulesRepository.LogType
import com.topjohnwu.superuser.Shell
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

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun MainScreen(
    executor: ExecutorService,
    onPickDir: (callback: (String) -> Unit) -> Unit,
    onImport: (callback: (String) -> Unit) -> Unit,
    onExport: (getContent: () -> String) -> Unit,
    onGetPackages: () -> List<String>,
) {
    val context = LocalContext.current

    // ---- UI state -----------------------------------------------------------
    var rulesText           by remember { mutableStateOf("") }
    var statusMsg           by remember { mutableStateOf("待机") }
    var showMoreSheet       by remember { mutableStateOf(false) }
    var showLogSheet        by remember { mutableStateOf(false) }
    var showLogContent      by remember { mutableStateOf("") }
    var showLogTitle        by remember { mutableStateOf("") }
    var showAppSheet        by remember { mutableStateOf(false) }
    var showTplSheet        by remember { mutableStateOf(false) }
    var showPresetSheet     by remember { mutableStateOf(false) }
    var showEditorSheet     by remember { mutableStateOf(false) }
    var showHitStatsSheet   by remember { mutableStateOf(false) }
    var showModuleConfSheet by remember { mutableStateOf(false) }
    var showBackupSheet     by remember { mutableStateOf(false) }
    var backupList          by remember { mutableStateOf<List<String>>(emptyList()) }
    var showOverviewSheet   by remember { mutableStateOf(false) }
    var showMsgDialog       by remember { mutableStateOf(false) }
    var msgTitle            by remember { mutableStateOf("") }
    var msgContent          by remember { mutableStateOf("") }
    var appFilterText       by remember { mutableStateOf("") }
    var mqExpanded          by remember { mutableStateOf(false) }
    var hitStatsList        by remember { mutableStateOf<List<HitStat>>(emptyList()) }
    var moduleConfText      by remember { mutableStateOf("") }
    var modulePropMap       by remember { mutableStateOf<Map<String, String>>(emptyMap()) }
    // 功能1：规则沙盒
    var showSandboxSheet    by remember { mutableStateOf(false) }
    // 功能2：分享
    // (通过 onExport + ShareCompat，无需额外状态)
    // 功能3：daemon 状态
    var showDaemonSheet     by remember { mutableStateOf(false) }
    var daemonStatus        by remember { mutableStateOf<com.folder.manager.gui.data.DaemonStatusReader.DaemonStatus?>(null) }
    // 功能4：模板库（动态加载）
    var showTplLibSheet     by remember { mutableStateOf(false) }
    // 功能6：批量操作
    var showBatchSheet      by remember { mutableStateOf(false) }
    // 功能8：Root 环境
    var rootInfo            by remember { mutableStateOf<com.folder.manager.gui.data.RootEnvironment.RootInfo?>(null) }
    // 新增文件管理功能状态
    var showAlertSheet      by remember { mutableStateOf(false) }
    var showStorageSheet    by remember { mutableStateOf(false) }
    var showDryRunSheet     by remember { mutableStateOf(false) }
    var showDiffSheet       by remember { mutableStateOf(false) }
    var showHeatmapSheet    by remember { mutableStateOf(false) }
    var storageList         by remember { mutableStateOf<List<com.folder.manager.gui.data.StorageAnalyzer.AppStorage>>(emptyList()) }
    var redirectList        by remember { mutableStateOf<List<com.folder.manager.gui.data.StorageAnalyzer.RedirectEntry>>(emptyList()) }
    var dryRunReport        by remember { mutableStateOf("") }
    var diffResult          by remember { mutableStateOf<com.folder.manager.gui.data.RulesDiff.DiffResult?>(null) }
    var savedRulesSnapshot  by remember { mutableStateOf("") }  // 用于 diff 对比的上次保存版本

    val mqOptions  = remember { listOf("auto", "true", "false") }
    var mqSelected by remember { mutableStateOf(mqOptions[0]) }
    val templates  = remember { buildTemplates() }
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

    // ---- 启动时检测 Root 环境 ------------------------------------------------
    LaunchedEffect(Unit) {
        executor.execute {
            val info = com.folder.manager.gui.data.RootEnvironment.detect()
            post {
                rootInfo = info
                if (!info.compatible) showMsg("Root 环境提示", "Root 环境兼容性未知，模块路径可能不正确\n检测到：${info.label}")
            }
        }
    }

    // ---- scaffold -----------------------------------------------------------
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.app_name)) },
                actions = {
                    IconButton(onClick = { showMoreSheet = true }) {
                        Icon(imageVector = IconMoreVert, contentDescription = "更多")
                    }
                },
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = {
                statusMsg = "保存中…"
                runAsync {
                    RulesRepository.saveAndReload(rulesText).fold(
                        onSuccess = { (_, ok) -> Pair(if (ok) "保存并重载完成" else "保存成功，但重载失败", "") },
                        onFailure = { Pair("", "保存失败：${it.message}") },
                    )
                }
            }) {
                Icon(imageVector = IconCheck, contentDescription = "保存并重载")
            }
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 12.dp),
        ) {
            Text(
                text = "状态：$statusMsg",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 4.dp),
            )
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
            ExposedDropdownMenuBox(expanded = mqExpanded, onExpandedChange = { mqExpanded = it }) {
                OutlinedTextField(
                    value = mqSelected, onValueChange = {}, readOnly = true,
                    label = { Text(stringResource(R.string.media_query_label)) },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = mqExpanded) },
                    colors = ExposedDropdownMenuDefaults.outlinedTextFieldColors(),
                    modifier = Modifier.fillMaxWidth().menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
                )
                ExposedDropdownMenu(expanded = mqExpanded, onDismissRequest = { mqExpanded = false }) {
                    mqOptions.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option) },
                            onClick = { mqSelected = option; mqExpanded = false; statusMsg = "media_query 已选择：$option" },
                        )
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = rulesText, onValueChange = { rulesText = it },
                modifier = Modifier.fillMaxWidth().weight(1f),
                label = { Text(stringResource(R.string.rules_path_label)) },
                singleLine = false,
                visualTransformation = remember { RulesSyntaxHighlighter() },
            )
        }
    }

    // ---- 更多操作 BottomSheet ------------------------------------------------
    if (showMoreSheet) {
        ModalBottomSheet(onDismissRequest = { showMoreSheet = false }) {
            Text("更多操作", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            MoreSheetItem("语法校验") {
                showMoreSheet = false
                val errors = RulesValidator.validate(rulesText)
                showMsg("语法校验", if (errors.isEmpty()) "未发现语法问题" else errors.joinToString("\n"))
            }
            MoreSheetItem("规则概览") { showMoreSheet = false; showOverviewSheet = true }
            MoreSheetItem("插入模板") { showMoreSheet = false; showTplSheet = true }
            MoreSheetItem(stringResource(R.string.action_insert_preset)) { showMoreSheet = false; showPresetSheet = true }
            MoreSheetItem(stringResource(R.string.action_rule_editor)) { showMoreSheet = false; showEditorSheet = true }
            MoreSheetItem("插入应用分组") { showMoreSheet = false; showAppSheet = true }
            MoreSheetItem("插入目录路径") {
                showMoreSheet = false
                onPickDir { path -> rulesText += "\n$path" }
            }
            MoreSheetItem(stringResource(R.string.log_daemon)) {
                showMoreSheet = false
                executor.execute {
                    val r = RulesRepository.readLog(LogType.DAEMON)
                    post { showLogTitle = "daemon.log"; showLogContent = r.getOrElse { "加载失败：${it.message}" }; showLogSheet = true }
                }
            }
            MoreSheetItem(stringResource(R.string.log_access)) {
                showMoreSheet = false
                executor.execute {
                    val r = RulesRepository.readLog(LogType.ACCESS)
                    post { showLogTitle = "access.log"; showLogContent = r.getOrElse { "加载失败：${it.message}" }; showLogSheet = true }
                }
            }
            MoreSheetItem(stringResource(R.string.log_hit_stats)) {
                showMoreSheet = false
                executor.execute {
                    val r = RulesRepository.readHitStats()
                    post { hitStatsList = r.getOrDefault(emptyList()); showHitStatsSheet = true }
                }
            }
            MoreSheetItem(stringResource(R.string.action_module_settings)) {
                showMoreSheet = false
                executor.execute {
                    val r = RulesRepository.readModuleConf()
                    val p = RulesRepository.readModuleProp().getOrDefault(emptyMap())
                    post { moduleConfText = r.getOrDefault(""); modulePropMap = p; showModuleConfSheet = true }
                }
            }
            MoreSheetItem("导入规则") {
                showMoreSheet = false
                onImport { imported ->
                    val errors = RulesValidator.validate(imported)
                    rulesText = imported
                    if (errors.isEmpty()) {
                        statusMsg = "导入成功"
                    } else {
                        showMsg("导入警告", "发现 ${errors.size} 个语法问题：\n" + errors.take(10).joinToString("\n"))
                    }
                }
            }
            MoreSheetItem("导出规则") { showMoreSheet = false; onExport { rulesText } }
            MoreSheetItem("备份历史") {
                showMoreSheet = false
                executor.execute {
                    val r = RulesRepository.listBackups()
                    post { backupList = r.getOrDefault(emptyList()); showBackupSheet = true }
                }
            }
            MoreSheetItem(stringResource(R.string.sandbox_title)) { showMoreSheet = false; showSandboxSheet = true }
            MoreSheetItem(stringResource(R.string.daemon_status_title)) {
                showMoreSheet = false
                executor.execute {
                    val r = com.folder.manager.gui.data.DaemonStatusReader.read()
                    post { daemonStatus = r.getOrNull(); showDaemonSheet = true }
                }
            }
            MoreSheetItem("模板库") { showMoreSheet = false; showTplLibSheet = true }
            MoreSheetItem("规则批量操作") { showMoreSheet = false; showBatchSheet = true }
            MoreSheetItem(stringResource(R.string.share_rules)) {
                showMoreSheet = false
                val shareIntent = android.content.Intent(android.content.Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(android.content.Intent.EXTRA_TEXT, rulesText)
                    putExtra(android.content.Intent.EXTRA_SUBJECT, "folder_manager rules.ini")
                }
                context.startActivity(android.content.Intent.createChooser(shareIntent, "分享规则"))
            }
            MoreSheetItem("路径访问热力图") { showMoreSheet = false; showHeatmapSheet = true }
            MoreSheetItem("应用存储分析") {
                showMoreSheet = false
                executor.execute {
                    val sections = RuleSectionParser.parse(rulesText)
                    val pkgs = sections.map { it.pkg }.filter { it != "*" }
                    val storage = com.folder.manager.gui.data.StorageAnalyzer.analyzeAppStorage(pkgs)
                    val redirects = com.folder.manager.gui.data.StorageAnalyzer.analyzeRedirects(rulesText)
                    post { storageList = storage.getOrDefault(emptyList()); redirectList = redirects.getOrDefault(emptyList()); showStorageSheet = true }
                }
            }
            MoreSheetItem("规则干运行报告") {
                showMoreSheet = false
                executor.execute {
                    val sections = RuleSectionParser.parse(rulesText)
                    val result = Shell.cmd("ls /data/media/0/ 2>/dev/null").exec()
                    val topPaths = result.out.take(20)
                    val sb = StringBuilder("干运行报告\n规则 section 数：${sections.size}\n\n")
                    sections.forEach { sec ->
                        sb.appendLine("[${sec.pkg}] mode=${sec.mode} enabled=${sec.enabled}")
                        sb.appendLine("  allow=${sec.allowCount} block=${sec.blockCount} redirect=${sec.redirectCount}")
                    }
                    post { dryRunReport = sb.toString(); showDryRunSheet = true }
                }
            }
            MoreSheetItem("规则变更 Diff") {
                showMoreSheet = false
                executor.execute {
                    val saved = RulesRepository.readRules().getOrDefault("")
                    post { diffResult = com.folder.manager.gui.data.RulesDiff.diff(saved, rulesText); showDiffSheet = true }
                }
            }
            MoreSheetItem("敏感路径告警设置") { showMoreSheet = false; showAlertSheet = true }
            // Root 环境信息
            rootInfo?.let { info ->
                val label = if (info.compatible) "✓ ${info.label}" else "⚠ ${info.label}"
                Text(label, style = MaterialTheme.typography.labelSmall,
                    color = if (info.compatible) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 日志 BottomSheet ---------------------------------------------------
    if (showLogSheet) {
        val isAccessLog = showLogTitle == "access.log"
        var autoRefresh by remember { mutableStateOf(isAccessLog) }
        var selectedLogLine by remember { mutableStateOf<String?>(null) }
        if (autoRefresh && isAccessLog) {
            LaunchedEffect(Unit) {
                while (isActive) {
                    delay(3_000L)
                    val r = RulesRepository.readLog(LogType.ACCESS)
                    showLogContent = r.getOrElse { "加载失败：${it.message}" }
                    // 告警检查
                    executor.execute { com.folder.manager.gui.data.AlertWatcher.checkNewEntries(context) }
                }
            }
        }
        // 白名单快速豁免 Dialog
        selectedLogLine?.let { logLine ->
            val pathRegex = Regex("path=(.+)")
            val pkgRegex  = Regex("pkg=([\\w.]+)")
            val path = pathRegex.find(logLine)?.groupValues?.get(1)?.trim() ?: ""
            val pkg  = pkgRegex.find(logLine)?.groupValues?.get(1)?.trim() ?: ""
            if (path.isNotEmpty() && pkg.isNotEmpty()) {
                AlertDialog(
                    onDismissRequest = { selectedLogLine = null },
                    title = { Text("快速添加豁免") },
                    text = { Text("为 $pkg 添加允许规则：\n+ $path") },
                    confirmButton = {
                        TextButton(onClick = {
                            selectedLogLine = null
                            // 在对应 section 下插入 + path
                            val exemption = "+ $path"
                            val sectionHeader = "[$pkg]"
                            rulesText = if (rulesText.contains(sectionHeader)) {
                                rulesText.replace(sectionHeader, "$sectionHeader\n$exemption")
                            } else {
                                rulesText + "\n[$pkg]\nmode = whitelist\nenabled = true\n$exemption\n"
                            }
                            statusMsg = "豁免规则已添加"
                        }) { Text("添加") }
                    },
                    dismissButton = { TextButton(onClick = { selectedLogLine = null }) { Text("取消") } },
                )
            } else { selectedLogLine = null }
        }
        ModalBottomSheet(onDismissRequest = { showLogSheet = false }) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text(showLogTitle, style = MaterialTheme.typography.titleMedium)
                if (isAccessLog) {
                    Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                        Text("自动刷新", style = MaterialTheme.typography.labelSmall)
                        Switch(checked = autoRefresh, onCheckedChange = { autoRefresh = it },
                            modifier = Modifier.padding(start = 4.dp))
                    }
                }
            }
            if (isAccessLog) {
                Text("长按日志行可快速添加豁免规则",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp))
                LazyColumn(
                    modifier = Modifier.fillMaxWidth().heightIn(max = 400.dp)
                        .padding(horizontal = 16.dp, vertical = 4.dp)
                ) {
                    items(showLogContent.lines()) { line ->
                        Text(
                            text = line,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            modifier = Modifier.fillMaxWidth()
                                .combinedClickable(
                                    onClick = {},
                                    onLongClick = { if (line.contains("pkg=")) selectedLogLine = line }
                                )
                                .padding(vertical = 2.dp),
                        )
                    }
                }
            } else {
                Text(
                    text = showLogContent.ifBlank { "无内容" },
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 命中统计 BottomSheet -----------------------------------------------
    if (showHitStatsSheet) {
        ModalBottomSheet(onDismissRequest = { showHitStatsSheet = false }) {
            Text(stringResource(R.string.log_hit_stats), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            if (hitStatsList.isEmpty()) {
                Text("暂无命中记录", modifier = Modifier.padding(16.dp))
            } else {
                LazyColumn(modifier = Modifier.fillMaxWidth()) {
                    items(hitStatsList) { stat ->
                        ListItem(
                            headlineContent = { Text(stat.pkg) },
                            supportingContent = { Text("action=${stat.action}  count=${stat.count}",
                                style = MaterialTheme.typography.labelSmall) },
                        )
                        HorizontalDivider()
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 模块设置 BottomSheet -----------------------------------------------
    if (showModuleConfSheet) {
        val clipboard = LocalClipboardManager.current
        ModalBottomSheet(onDismissRequest = { showModuleConfSheet = false }) {
            Text(stringResource(R.string.action_module_settings), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            // 版本信息卡片
            if (modulePropMap.isNotEmpty()) {
                Card(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
                ) {
                    Column(modifier = Modifier.padding(12.dp)) {
                        listOf(
                            "name"        to "模块名称",
                            "version"     to "版本号",
                            "versionCode" to "版本代码",
                            "author"      to "作者",
                        ).forEach { (key, label) ->
                            modulePropMap[key]?.let { v ->
                                Row(modifier = Modifier.fillMaxWidth()) {
                                    Text(label, style = MaterialTheme.typography.labelMedium,
                                        modifier = Modifier.width(80.dp))
                                    Text(v, style = MaterialTheme.typography.bodySmall)
                                }
                            }
                        }
                    }
                }
            }
            Spacer(Modifier.height(4.dp))
            OutlinedTextField(
                value = moduleConfText, onValueChange = { moduleConfText = it },
                label = { Text(stringResource(R.string.module_conf_label)) },
                singleLine = false,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            )
            Spacer(Modifier.height(8.dp))
            // 复制打包命令按钮
            val version = modulePropMap["version"] ?: "0.0.0"
            val packCmd = "cd /path/to/PathGuard && zip -r " +
                "folder_manager-$version.zip module.prop " +
                "config/ bin/ zygisk/ -x '*.bak*'"
            OutlinedButton(
                onClick = {
                    clipboard.setText(AnnotatedString(packCmd))
                    statusMsg = "打包命令已复制到剪贴板"
                },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            ) { Text("复制打包命令 (v$version)") }
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = {
                    showModuleConfSheet = false
                    runAsync {
                        RulesRepository.saveModuleConf(moduleConfText).fold(
                            onSuccess = { Pair("module.conf 已保存", "") },
                            onFailure = { Pair("", "保存失败：${it.message}") },
                        )
                    }
                },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            ) { Text(stringResource(R.string.module_conf_save)) }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 模板选择 BottomSheet -----------------------------------------------
    if (showTplSheet) {
        ModalBottomSheet(onDismissRequest = { showTplSheet = false }) {
            Text("插入模板", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            templates.forEach { (name, body) ->
                MoreSheetItem(name) { rulesText += "\n$body\n"; showTplSheet = false }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 预设规则 BottomSheet -----------------------------------------------
    if (showPresetSheet) {
        val presets = remember { PresetRepository.listPresets(context) }
        var presetFilter by remember { mutableStateOf("") }
        val filteredPresets = remember(presets, presetFilter) {
            if (presetFilter.isBlank()) presets
            else presets.filter {
                it.name.contains(presetFilter, ignoreCase = true) ||
                it.pkg.contains(presetFilter, ignoreCase = true)
            }
        }
        ModalBottomSheet(onDismissRequest = { showPresetSheet = false; presetFilter = "" }) {
            Text(stringResource(R.string.action_insert_preset), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            OutlinedTextField(
                value = presetFilter, onValueChange = { presetFilter = it },
                label = { Text("搜索预设…") }, singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
            )
            Spacer(Modifier.height(4.dp))
            if (filteredPresets.isEmpty()) {
                Text(if (presets.isEmpty()) "无可用预设" else "无匹配结果", modifier = Modifier.padding(16.dp))
            } else {
                LazyColumn(modifier = Modifier.fillMaxWidth()) {
                    items(filteredPresets) { meta ->
                        ListItem(
                            headlineContent = {
                                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                    Text(meta.name)
                                    if (meta.verified) {
                                        AssistChip(
                                            onClick = {},
                                            label = { Text(stringResource(R.string.preset_verified),
                                                style = MaterialTheme.typography.labelSmall) },
                                        )
                                    }
                                }
                            },
                            supportingContent = {
                                Column {
                                    Text(meta.pkg, style = MaterialTheme.typography.labelSmall)
                                    if (meta.description.isNotBlank())
                                        Text(meta.description, style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            },
                            modifier = Modifier.clickable {
                                executor.execute {
                                    val r = PresetRepository.loadPresetContent(context, meta.fileName)
                                    post {
                                        r.onSuccess { rulesText += "\n$it\n"; statusMsg = "已插入预设：${meta.name}" }
                                         .onFailure { statusMsg = "预设加载失败：${it.message}" }
                                        showPresetSheet = false
                                        presetFilter = ""
                                    }
                                }
                            },
                        )
                        HorizontalDivider()
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 图形化规则编辑器 ---------------------------------------------------
    if (showEditorSheet) {
        val allPackages = remember { onGetPackages() }
        RuleEditorSheet(
            packages     = allPackages,
            existingRules = rulesText,
            onPickDir    = onPickDir,
            onInsert     = { line -> rulesText += "\n$line" },
            onDismiss    = { showEditorSheet = false },
        )
    }

    // ---- 应用选择 BottomSheet -----------------------------------------------
    if (showAppSheet) {
        val allPackages = remember { onGetPackages() }
        val filtered = remember(appFilterText) {
            if (appFilterText.isBlank()) allPackages
            else allPackages.filter { it.contains(appFilterText, ignoreCase = true) }
        }
        ModalBottomSheet(onDismissRequest = { showAppSheet = false; appFilterText = "" }) {
            Text("选择应用", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            OutlinedTextField(
                value = appFilterText, onValueChange = { appFilterText = it },
                label = { Text("搜索应用…") }, singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
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
                            showAppSheet = false; appFilterText = ""
                        },
                    )
                    HorizontalDivider()
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 规则概览 BottomSheet ---------------------------------------------
    if (showOverviewSheet) {
        val sections = remember(rulesText) { RuleSectionParser.parse(rulesText) }
        val redirectFlows = remember(rulesText) { parseRedirectFlows(rulesText) }
        var expandedPkg by remember { mutableStateOf<String?>(null) }
        var showFlowTab by remember { mutableStateOf(false) }
        ModalBottomSheet(onDismissRequest = { showOverviewSheet = false }) {
            Row(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = !showFlowTab, onClick = { showFlowTab = false },
                    label = { Text("规则概览") })
                FilterChip(selected = showFlowTab, onClick = { showFlowTab = true },
                    label = { Text("重定向流图 (${redirectFlows.size})") })
            }
            if (showFlowTab) {
                if (redirectFlows.isEmpty()) {
                    Text("无重定向规则", modifier = Modifier.padding(16.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else {
                    LazyColumn(modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        items(redirectFlows) { flow ->
                            RedirectFlowCard(flow = flow)
                        }
                        item { Spacer(Modifier.height(16.dp)) }
                    }
                }
            } else {
                if (sections.isEmpty()) {
                    Text("无规则内容", modifier = Modifier.padding(16.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else {
                    LazyColumn(modifier = Modifier.fillMaxWidth()) {
                    items(sections) { sec ->
                        val isExpanded = expandedPkg == sec.pkg
                        val total = sec.allowCount + sec.blockCount + sec.redirectCount
                        Card(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 12.dp, vertical = 4.dp)
                                .clickable { expandedPkg = if (isExpanded) null else sec.pkg },
                        ) {
                            Column(modifier = Modifier.padding(12.dp)) {
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                ) {
                                    Text(
                                        text = sec.pkg,
                                        style = MaterialTheme.typography.bodyMedium,
                                        fontWeight = androidx.compose.ui.text.font.FontWeight.Bold,
                                        modifier = Modifier.weight(1f),
                                    )
                                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                        if (!sec.enabled) {
                                            AssistChip(onClick = {}, label = { Text("已禁用",
                                                style = MaterialTheme.typography.labelSmall) })
                                        }
                                        AssistChip(onClick = {}, label = { Text("$total 条",
                                            style = MaterialTheme.typography.labelSmall) })
                                    }
                                }
                                androidx.compose.animation.AnimatedVisibility(visible = isExpanded) {
                                    Column(modifier = Modifier.padding(top = 8.dp),
                                        verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                        if (sec.mode.isNotBlank())
                                            Text("mode: ${sec.mode}",
                                                style = MaterialTheme.typography.bodySmall)
                                        if (sec.allowCount > 0)
                                            Text("允许: ${sec.allowCount} 条",
                                                style = MaterialTheme.typography.bodySmall,
                                                color = androidx.compose.ui.graphics.Color(0xFFa3be8c))
                                        if (sec.blockCount > 0)
                                            Text("屏蔽: ${sec.blockCount} 条",
                                                style = MaterialTheme.typography.bodySmall,
                                                color = androidx.compose.ui.graphics.Color(0xFFbf616a))
                                        if (sec.redirectCount > 0)
                                            Text("重定向: ${sec.redirectCount} 条",
                                                style = MaterialTheme.typography.bodySmall,
                                                color = androidx.compose.ui.graphics.Color(0xFFb48ead))
                                    }
                                }
                            }
                        }
                    }
                }
                } // end overview tab
            } // end else
            Spacer(Modifier.height(16.dp))
        }
    }
    if (showBackupSheet) {
        ModalBottomSheet(onDismissRequest = { showBackupSheet = false }) {
            Text("备份历史", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            // 立即备份按钮
            OutlinedButton(
                onClick = {
                    runAsync {
                        RulesRepository.backupWithTimestamp().fold(
                            onSuccess = { path ->
                                val r = RulesRepository.listBackups()
                                post { backupList = r.getOrDefault(emptyList()) }
                                Pair("已备份：${path.substringAfterLast('/')}", "")
                            },
                            onFailure = { Pair("", "备份失败：${it.message}") },
                        )
                    }
                },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            ) { Text("立即备份") }
            Spacer(Modifier.height(4.dp))
            if (backupList.isEmpty()) {
                Text("暂无时间戳备份", modifier = Modifier.padding(16.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                LazyColumn(modifier = Modifier.fillMaxWidth()) {
                    items(backupList) { path ->
                        val name = path.substringAfterLast('/')
                        ListItem(
                            headlineContent = { Text(name, style = MaterialTheme.typography.bodyMedium) },
                            trailingContent = {
                                TextButton(onClick = {
                                    showBackupSheet = false
                                    runAsync {
                                        RulesRepository.rollbackToBackup(path).fold(
                                            onSuccess = { Pair("已回滚到：$name", "") },
                                            onFailure = { Pair("", "回滚失败：${it.message}") },
                                        )
                                    }
                                }) { Text("回滚") }
                            },
                        )
                        HorizontalDivider()
                    }
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
            confirmButton = { TextButton(onClick = { showMsgDialog = false }) { Text("确定") } },
        )
    }

    // ---- 规则测试沙盒 BottomSheet ------------------------------------------
    if (showSandboxSheet) {
        var sandboxPkg    by remember { mutableStateOf("") }
        var sandboxPath   by remember { mutableStateOf("") }
        var sandboxResult by remember { mutableStateOf<RuleSandbox.MatchResult?>(null) }
        ModalBottomSheet(onDismissRequest = { showSandboxSheet = false }) {
            Text(stringResource(R.string.sandbox_title), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            OutlinedTextField(
                value = sandboxPkg, onValueChange = { sandboxPkg = it },
                label = { Text(stringResource(R.string.sandbox_pkg_hint)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            OutlinedTextField(
                value = sandboxPath, onValueChange = { sandboxPath = it },
                label = { Text(stringResource(R.string.sandbox_path_hint)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            Button(
                onClick = { sandboxResult = RuleSandbox.test(rulesText, sandboxPkg.trim(), sandboxPath.trim()) },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                enabled = sandboxPkg.isNotBlank() && sandboxPath.isNotBlank(),
            ) { Text(stringResource(R.string.sandbox_run)) }
            sandboxResult?.let { r ->
                val (bgColor, label) = when (r.action) {
                    "BLOCK"    -> MaterialTheme.colorScheme.errorContainer to "BLOCK"
                    "ALLOW"    -> MaterialTheme.colorScheme.primaryContainer to "ALLOW"
                    "REDIRECT" -> MaterialTheme.colorScheme.secondaryContainer to "REDIRECT"
                    else       -> MaterialTheme.colorScheme.surfaceVariant to "NO_MATCH"
                }
                Card(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                    colors = CardDefaults.cardColors(containerColor = bgColor),
                ) {
                    Column(modifier = Modifier.padding(12.dp)) {
                        Text("结果：$label", style = MaterialTheme.typography.titleSmall)
                        if (r.matched) Text("命中规则：${r.rule}", style = MaterialTheme.typography.bodySmall)
                        Text("mode：${r.sectionMode}", style = MaterialTheme.typography.bodySmall)
                        if (r.target.isNotEmpty()) Text("重定向至：${r.target}", style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- Daemon 状态 BottomSheet ------------------------------------------
    if (showDaemonSheet) {
        ModalBottomSheet(onDismissRequest = { showDaemonSheet = false }) {
            Text(stringResource(R.string.daemon_status_title), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            val status = daemonStatus
            if (status == null) {
                Text("加载失败", modifier = Modifier.padding(16.dp))
            } else {
                val runningColor = if (status.running) MaterialTheme.colorScheme.primaryContainer
                                   else MaterialTheme.colorScheme.errorContainer
                val runningLabel = if (status.running) stringResource(R.string.daemon_running)
                                   else stringResource(R.string.daemon_stopped)
                Card(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                    colors = CardDefaults.cardColors(containerColor = runningColor),
                ) {
                    Column(modifier = Modifier.padding(12.dp)) {
                        Text(runningLabel, style = MaterialTheme.typography.titleSmall)
                        status.pid?.let { Text("PID: $it", style = MaterialTheme.typography.bodySmall) }
                        Text("fanotify 挂载点：${status.mountCount}", style = MaterialTheme.typography.bodySmall)
                        Text("错误数：${status.errorCount}", style = MaterialTheme.typography.bodySmall)
                        if (status.startTime.isNotEmpty())
                            Text("启动：${status.startTime}", style = MaterialTheme.typography.bodySmall)
                    }
                }
                if (status.lastError.isNotEmpty()) {
                    Card(
                        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                    ) {
                        Text("最近错误：${status.lastError}",
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.padding(12.dp))
                    }
                }
                Spacer(Modifier.height(4.dp))
                Text("最近日志", style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.padding(horizontal = 16.dp))
                LazyColumn(
                    modifier = Modifier.fillMaxWidth().heightIn(max = 200.dp).padding(horizontal = 16.dp)
                ) {
                    items(status.recentLines) { line ->
                        Text(line, style = MaterialTheme.typography.bodySmall,
                            fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace)
                    }
                }
                // 热重载按钮（功能7）
                Spacer(Modifier.height(8.dp))
                OutlinedButton(
                    onClick = {
                        runAsync {
                            RulesRepository.reload().fold(
                                onSuccess = { Pair("Daemon 已重载规则", "") },
                                onFailure = { Pair("", "重载失败：${it.message}") },
                            )
                        }
                    },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                ) { Text("热重载规则") }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 模板库 BottomSheet （功能4）--------------------------------------
    if (showTplLibSheet) {
        val allTpls = remember { com.folder.manager.gui.data.TemplateRepository.listAll(context) }
        var saveDialogName by remember { mutableStateOf("") }
        var showSaveDialog by remember { mutableStateOf(false) }
        ModalBottomSheet(onDismissRequest = { showTplLibSheet = false }) {
            Text("模板库", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            OutlinedButton(
                onClick = { showSaveDialog = true },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            ) { Text(stringResource(R.string.tpl_save_user)) }
            LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 360.dp)) {
                items(allTpls) { tpl ->
                    ListItem(
                        headlineContent = { Text(tpl.name) },
                        supportingContent = {
                            Text(
                                if (tpl.isBuiltin) stringResource(R.string.tpl_builtin_badge)
                                else stringResource(R.string.tpl_user_badge),
                                style = MaterialTheme.typography.labelSmall,
                            )
                        },
                        modifier = Modifier.clickable {
                            rulesText += "\n${tpl.body}\n"
                            showTplLibSheet = false
                        },
                    )
                    HorizontalDivider()
                }
            }
            Spacer(Modifier.height(16.dp))
        }
        if (showSaveDialog) {
            AlertDialog(
                onDismissRequest = { showSaveDialog = false },
                title = { Text(stringResource(R.string.tpl_save_user)) },
                text = {
                    OutlinedTextField(
                        value = saveDialogName, onValueChange = { saveDialogName = it },
                        label = { Text("模板名称") }, singleLine = true,
                    )
                },
                confirmButton = {
                    TextButton(onClick = {
                        showSaveDialog = false
                        runAsync {
                            com.folder.manager.gui.data.TemplateRepository.saveUserTemplate(saveDialogName, rulesText).fold(
                                onSuccess = { Pair("模板已保存：$saveDialogName", "") as Pair<String, String> },
                                onFailure = { Pair("", "保存失败：${it.message}") },
                            )
                        }
                    }, enabled = saveDialogName.isNotBlank()) { Text("保存") }
                },
                dismissButton = { TextButton(onClick = { showSaveDialog = false }) { Text("取消") } },
            )
        }
    }

    // ---- 批量操作 BottomSheet （功能6）------------------------------------
    if (showBatchSheet) {
        val sections = remember(rulesText) { RuleSectionParser.parse(rulesText) }
        val selected = remember { mutableStateOf(setOf<String>()) }
        ModalBottomSheet(onDismissRequest = { showBatchSheet = false }) {
            Text("规则批量操作", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            Row(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = {
                    // 批量启用：移除 enabled=false
                    var text = rulesText
                    selected.value.forEach { pkg ->
                        text = text.replace(Regex("(\\[$pkg\\][\\s\\S]*?)enabled\\s*=\\s*false")) {
                            it.value.replace(Regex("enabled\\s*=\\s*false"), "enabled = true")
                        }
                    }
                    rulesText = text; showBatchSheet = false
                }, modifier = Modifier.weight(1f), enabled = selected.value.isNotEmpty()) {
                    Text(stringResource(R.string.batch_enable))
                }
                OutlinedButton(onClick = {
                    // 批量禁用：添加/替换 enabled=false
                    var text = rulesText
                    selected.value.forEach { pkg ->
                        text = if (text.contains(Regex("\\[$pkg\\][\\s\\S]*?enabled\\s*="))) {
                            text.replace(Regex("(\\[$pkg\\][\\s\\S]*?)enabled\\s*=\\s*\\w+")) {
                                it.value.replace(Regex("enabled\\s*=\\s*\\w+"), "enabled = false")
                            }
                        } else {
                            text.replace("[$pkg]", "[$pkg]\nenabled = false")
                        }
                    }
                    rulesText = text; showBatchSheet = false
                }, modifier = Modifier.weight(1f), enabled = selected.value.isNotEmpty()) {
                    Text(stringResource(R.string.batch_disable))
                }
            }
            LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 360.dp)) {
                items(sections) { sec ->
                    val checked = sec.pkg in selected.value
                    ListItem(
                        headlineContent = { Text(sec.pkg) },
                        supportingContent = { Text("mode=${sec.mode} allow=${sec.allowCount} block=${sec.blockCount}",
                            style = MaterialTheme.typography.bodySmall) },
                        leadingContent = {
                            Checkbox(checked = checked, onCheckedChange = { c ->
                                selected.value = if (c) selected.value + sec.pkg else selected.value - sec.pkg
                            })
                        },
                        modifier = Modifier.clickable {
                            selected.value = if (checked) selected.value - sec.pkg else selected.value + sec.pkg
                        },
                    )
                    HorizontalDivider()
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 路径访问热力图 BottomSheet -----------------------------------------
    if (showHeatmapSheet) {
        ModalBottomSheet(onDismissRequest = { showHeatmapSheet = false }) {
            Text("路径访问热力图", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            if (hitStatsList.isEmpty()) {
                OutlinedButton(
                    onClick = {
                        executor.execute {
                            val r = RulesRepository.readHitStats()
                            post { hitStatsList = r.getOrDefault(emptyList()) }
                        }
                    },
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp)
                ) { Text("加载统计数据") }
            } else {
                HitStatsBarChart(
                    stats = hitStatsList,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp)
                )
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 应用存储分析 BottomSheet -------------------------------------------
    if (showStorageSheet) {
        ModalBottomSheet(onDismissRequest = { showStorageSheet = false }) {
            Text("应用存储分析", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            if (storageList.isNotEmpty()) {
                Text("Android/data 占用", style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 240.dp)) {
                    items(storageList) { app ->
                        ListItem(
                            headlineContent = { Text(app.pkg.substringAfterLast('.')) },
                            supportingContent = { Text(app.pkg, style = MaterialTheme.typography.bodySmall) },
                            trailingContent = { Text(app.dataSize, style = MaterialTheme.typography.labelLarge,
                                color = MaterialTheme.colorScheme.primary) },
                        )
                        HorizontalDivider()
                    }
                }
            }
            if (redirectList.isNotEmpty()) {
                Text("重定向目标目录", style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 240.dp)) {
                    items(redirectList) { entry ->
                        ListItem(
                            headlineContent = { Text(entry.src) },
                            supportingContent = { Text("-> ${entry.dst}", style = MaterialTheme.typography.bodySmall) },
                            trailingContent = {
                                Column(horizontalAlignment = Alignment.End) {
                                    Text(entry.size, style = MaterialTheme.typography.labelLarge,
                                        color = MaterialTheme.colorScheme.secondary)
                                    TextButton(onClick = {
                                        runAsync {
                                            com.folder.manager.gui.data.StorageAnalyzer.clearRedirectTarget(entry.dst).fold(
                                                onSuccess = { Pair("已清空：${entry.dst}", "") as Pair<String,String> },
                                                onFailure = { Pair("", "清空失败：${it.message}") },
                                            )
                                        }
                                    }) { Text("清空", style = MaterialTheme.typography.labelSmall) }
                                }
                            },
                        )
                        HorizontalDivider()
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 规则干运行报告 BottomSheet -----------------------------------------
    if (showDryRunSheet) {
        ModalBottomSheet(onDismissRequest = { showDryRunSheet = false }) {
            Text("规则干运行报告", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            Text(
                text = dryRunReport.ifBlank { "无数据" },
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp, vertical = 8.dp)
                    .heightIn(max = 400.dp),
            )
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 规则变更 Diff BottomSheet ------------------------------------------
    if (showDiffSheet) {
        val dr = diffResult
        ModalBottomSheet(onDismissRequest = { showDiffSheet = false }) {
            Text("规则变更 Diff", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            if (dr == null) {
                Text("无差异数据", modifier = Modifier.padding(16.dp))
            } else {
                Text("+${dr.added} 行  -${dr.removed} 行",
                    style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 420.dp)
                    .padding(horizontal = 8.dp)) {
                    items(dr.lines.filter { it.type != com.folder.manager.gui.data.RulesDiff.DiffType.UNCHANGED }) { dl ->
                        val (bg, prefix) = when (dl.type) {
                            com.folder.manager.gui.data.RulesDiff.DiffType.ADDED   ->
                                MaterialTheme.colorScheme.primaryContainer to "+"
                            com.folder.manager.gui.data.RulesDiff.DiffType.REMOVED ->
                                MaterialTheme.colorScheme.errorContainer to "-"
                            else -> MaterialTheme.colorScheme.surface to " "
                        }
                        Text(
                            text = "$prefix ${dl.lineNo.toString().padStart(3)} ${dl.line}",
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = if (dl.type == com.folder.manager.gui.data.RulesDiff.DiffType.ADDED)
                                MaterialTheme.colorScheme.onPrimaryContainer
                            else MaterialTheme.colorScheme.onErrorContainer,
                            modifier = Modifier.fillMaxWidth().padding(2.dp),
                        )
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    // ---- 敏感路径告警设置 BottomSheet ---------------------------------------
    if (showAlertSheet) {
        var alertEnabled by remember { mutableStateOf(com.folder.manager.gui.data.AlertWatcher.isEnabled(context)) }
        var pathsText    by remember {
            mutableStateOf(com.folder.manager.gui.data.AlertWatcher.getSensitivePaths(context).joinToString("\n"))
        }
        ModalBottomSheet(onDismissRequest = { showAlertSheet = false }) {
            Text("敏感路径告警设置", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("启用告警通知", style = MaterialTheme.typography.bodyMedium)
                Switch(checked = alertEnabled, onCheckedChange = {
                    alertEnabled = it
                    com.folder.manager.gui.data.AlertWatcher.setEnabled(context, it)
                })
            }
            Text("敏感路径关键词（每行一个）", style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
            OutlinedTextField(
                value = pathsText, onValueChange = { pathsText = it },
                singleLine = false,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp).heightIn(min = 80.dp, max = 160.dp),
                placeholder = { Text("Documents\nDownload\nDCIM") },
            )
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = {
                    val paths = pathsText.lines().map { it.trim() }.filter { it.isNotEmpty() }.toSet()
                    com.folder.manager.gui.data.AlertWatcher.setSensitivePaths(context, paths)
                    showAlertSheet = false
                    statusMsg = "告警设置已保存（${paths.size} 个路径）"
                },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            ) { Text("保存") }
            Spacer(Modifier.height(16.dp))
        }
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

        // ---- 阶段1：逐行语法检查 ----------------------------------------
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

        // ---- 阶段2：冲突检测 --------------------------------------------
        data class SectionData(
            val lineNo: Int,
            val allows: MutableList<String> = mutableListOf(),
            val blocks: MutableList<String> = mutableListOf(),
            val redirectSrcs: MutableList<String> = mutableListOf(),
        )
        val sections = mutableMapOf<String, SectionData>()
        var curPkg: String? = null
        var curLine = 0

        text.lines().forEachIndexed { i, raw ->
            val line = raw.trim()
            when {
                line.startsWith("[") && line.endsWith("]") -> {
                    val pkg = line.removeSurrounding("[", "]")
                    if (sections.containsKey(pkg)) {
                        errors.add("第 ${i + 1} 行：重复 section [$pkg]（首次出现在第 ${sections[pkg]!!.lineNo + 1} 行）")
                    } else {
                        sections[pkg] = SectionData(lineNo = i)
                    }
                    curPkg = pkg; curLine = i
                }
                line.startsWith("+") -> {
                    curPkg?.let { sections[it]?.allows?.add(line.removePrefix("+").trim()) }
                }
                line.startsWith("-") && !line.contains("->") -> {
                    curPkg?.let { sections[it]?.blocks?.add(line.removePrefix("-").trim()) }
                }
                line.contains("->") && !line.startsWith("#") -> {
                    val src = line.substringBefore("->").trim()
                    curPkg?.let { sections[it]?.redirectSrcs?.add(src) }
                }
            }
        }

        // 检测 allow/block 前缀冲突
        for ((pkg, data) in sections) {
            for (allow in data.allows) {
                for (block in data.blocks) {
                    if (allow.startsWith(block) || block.startsWith(allow)) {
                        errors.add("[$pkg] allow '$allow' 与 block '$block' 路径前缀冲突")
                    }
                }
            }
            // 检测 redirect 源路径与 block 冲突
            for (src in data.redirectSrcs) {
                for (block in data.blocks) {
                    if (src.startsWith(block) || block.startsWith(src)) {
                        errors.add("[$pkg] redirect 源 '$src' 与 block '$block' 路径前缀冲突")
                    }
                }
            }
            // 检测空 section（无任何规则）
            if (data.allows.isEmpty() && data.blocks.isEmpty() && data.redirectSrcs.isEmpty()) {
                errors.add("[$pkg] 为空 section，未包含任何规则行")
            }
        }

        return errors
    }
}
