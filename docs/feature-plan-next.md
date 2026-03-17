# PathGuard 下一阶段功能实施方案

> 文档日期：2026-03-17
> 基于 Android 官方文档调研 + 现有代码架构分析

---

## 功能 1：规则语法高亮

### 目标
将纯文本规则编辑器升级为带颜色标注的富文本编辑器，无需引入第三方依赖。

### 技术方案
**核心 API：** `BasicTextField` + `VisualTransformation` + `buildAnnotatedString`

官方文档确认：`VisualTransformation` 可在不改变底层 `String` 状态的前提下，将显示文本替换为带 `SpanStyle` 的 `AnnotatedString`。这是实现语法高亮最轻量的方式。

**颜色方案（Material3 配色）：**
| 元素 | 样式 | 示例 |
|------|------|------|
| Section 头 `[pkg]` | `Primary` + `Bold` | `[com.tencent.mm]` |
| 允许规则 `+` | `Green` / `Tertiary` | `+ DCIM/Camera` |
| 屏蔽规则 `-` | `Error` / `Red` | `- DCIM/Secret` |
| 重定向 `->` | `Secondary` | `DCIM -> Android/data/...` |
| 注释 `#` | `onSurfaceVariant` + `Italic` | `# 注释` |
| 键值对 `key = value` | key 用 `Tertiary` | `mode = whitelist` |

**实现要点：**
- 用 `OutlinedTextField` 的 `visualTransformation` 参数（M3 支持）
- 按行解析，正则匹配各类型，`buildAnnotatedString` 逐段 append
- 光标偏移映射：`OffsetMapping.Identity`（字符数不变，1:1 映射）
- 不引入任何新依赖

**涉及文件：**
- 新建：`ui/RulesSyntaxHighlighter.kt`（`VisualTransformation` 实现）
- 修改：`ui/MainScreen.kt`（`OutlinedTextField` 加 `visualTransformation` 参数）

**代码骨架：**
```kotlin
class RulesSyntaxTransformation(private val colors: ColorScheme) : VisualTransformation {
    override fun filter(text: AnnotatedString): TransformedText {
        val annotated = buildAnnotatedString {
            text.text.lines().forEachIndexed { i, line ->
                if (i > 0) append("\n")
                when {
                    line.trimStart().startsWith("#")  -> withStyle(SpanStyle(color = colors.onSurfaceVariant, fontStyle = FontStyle.Italic)) { append(line) }
                    line.matches(Regex("\\s*\\[.*]\\s*")) -> withStyle(SpanStyle(color = colors.primary, fontWeight = FontWeight.Bold)) { append(line) }
                    line.trimStart().startsWith("+")  -> withStyle(SpanStyle(color = colors.tertiary)) { append(line) }
                    line.trimStart().startsWith("-")  -> withStyle(SpanStyle(color = colors.error)) { append(line) }
                    line.contains("->")               -> withStyle(SpanStyle(color = colors.secondary)) { append(line) }
                    else -> append(line)
                }
            }
        }
        return TransformedText(annotated, OffsetMapping.Identity)
    }
}
```

---

## 功能 2：导入验证

### 目标
导入外部文件后自动校验语法，有错误时弹窗提示，防止用户导入损坏的规则文件。

### 技术方案
**复用现有：** `RulesValidator.validate()` 已实现，无需新代码。

**修改点（仅 MainScreen.kt）：**
```kotlin
onImport { imported ->
    val errors = RulesValidator.validate(imported)
    if (errors.isEmpty()) {
        rulesText = imported
        statusMsg = "导入成功"
    } else {
        rulesText = imported  // 仍然导入，但弹窗提示
        showMsg("导入警告", "发现 ${errors.size} 个语法问题：\n" + errors.take(10).joinToString("\n"))
    }
}
```

**涉及文件：**
- 修改：`ui/MainScreen.kt`（`onImport` 回调加验证逻辑，约 8 行）

---

## 功能 3：预设搜索框

### 目标
预设列表增加实时搜索过滤，复用现有 appFilterText 模式。

### 技术方案
**无新 API，完全复用现有模式。**

在预设 BottomSheet 顶部加 `OutlinedTextField`，`remember(presetFilter)` 过滤列表：
```kotlin
var presetFilter by remember { mutableStateOf("") }
val filteredPresets = remember(presets, presetFilter) {
    if (presetFilter.isBlank()) presets
    else presets.filter {
        it.name.contains(presetFilter, ignoreCase = true) ||
        it.pkg.contains(presetFilter, ignoreCase = true)
    }
}
```

**涉及文件：**
- 修改：`ui/MainScreen.kt`（预设 BottomSheet 区域，约 15 行）

---

## 功能 4：备份历史（最近 5 份）

### 目标
保留按时间戳命名的最近 5 份备份，GUI 展示列表供选择回滚。

### 技术方案
**备份文件命名：** `rules.ini.bak.YYYYMMDD_HHmmss`

**Kotlin 实现（RulesRepository）：**
```kotlin
fun backupWithTimestamp(): Result<String> = runCatching {
    val ts = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
        .format(java.util.Date())
    val dst = "$BACKUP_DIR/rules.ini.bak.$ts"
    Shell.cmd("cp ${shellQuote(RULES_PATH)} ${shellQuote(dst)}").exec()
    // 只保留最近 5 份，删除多余的
    val result = Shell.cmd("ls -t ${shellQuote(BACKUP_DIR)}/rules.ini.bak.* 2>/dev/null").exec()
    result.out.drop(5).forEach { old ->
        Shell.cmd("rm -f ${shellQuote(old.trim())}").exec()
    }
    dst
}

fun listBackups(): Result<List<String>> = runCatching {
    val result = Shell.cmd("ls -t /data/adb/modules/folder_manager/config/rules.ini.bak.* 2>/dev/null").exec()
    result.out.map { it.trim() }.filter { it.isNotBlank() }
}

fun rollbackToBackup(backupPath: String): Result<Unit> = runCatching {
    Shell.cmd("cp ${shellQuote(backupPath)} ${shellQuote(RULES_PATH)}").exec()
    reload().getOrThrow()
}
```

**GUI：** 在更多操作 BottomSheet 中「回滚备份」改为打开 BackupHistorySheet，展示时间戳列表，点击条目执行回滚。

**涉及文件：**
- 修改：`data/RulesRepository.kt`（新增 3 个方法）
- 修改：`ui/MainScreen.kt`（备份历史 BottomSheet）

---

## 功能 5：access.log 实时刷新

### 目标
日志查看页面自动轮询刷新，无需手动重新打开。

### 技术方案
**核心 API：** `LaunchedEffect` + `while(isActive) { delay(3000) }` —— 官方文档确认此为标准轮询模式。

```kotlin
// 在 access.log BottomSheet 内
var autoRefresh by remember { mutableStateOf(true) }

if (autoRefresh) {
    LaunchedEffect(Unit) {
        while (isActive) {
            delay(3_000L)
            val r = RulesRepository.readLog(LogType.ACCESS)
            showLogContent = r.getOrElse { "加载失败：${it.message}" }
        }
    }
}
```

增加「自动刷新」开关（`Switch`），用户可关闭。

**涉及文件：**
- 修改：`ui/MainScreen.kt`（access.log BottomSheet 区域，约 20 行）
- 无新依赖，无新文件

---

## 功能 6：规则概览（分组折叠卡片）

### 目标
新增「规则概览」BottomSheet，解析 rulesText 显示每个 `[pkg]` section 的摘要，支持折叠展开。

### 技术方案
**核心 API：** `AnimatedVisibility` + `derivedStateOf` —— 官方文档确认标准方案。

**解析逻辑（纯 Kotlin，无新依赖）：**
```kotlin
data class RuleSection(
    val pkg: String,
    val mode: String,
    val enabled: Boolean,
    val allowCount: Int,
    val blockCount: Int,
    val redirectCount: Int,
)

fun parseRuleSections(text: String): List<RuleSection> {
    // 按 [pkg] 分割，逐段统计 +/-/-> 行数
}
```

**UI 骨架：**
```kotlin
var expandedPkg by remember { mutableStateOf<String?>(null) }
LazyColumn {
    items(sections) { section ->
        Card(modifier = Modifier.clickable {
            expandedPkg = if (expandedPkg == section.pkg) null else section.pkg
        }) {
            Row { Text(section.pkg); Badge { Text("${section.allowCount+section.blockCount}") } }
            AnimatedVisibility(visible = expandedPkg == section.pkg) {
                Column {
                    Text("允许: ${section.allowCount}  屏蔽: ${section.blockCount}  重定向: ${section.redirectCount}")
                    Text("mode=${section.mode}  enabled=${section.enabled}")
                }
            }
        }
    }
}
```

**涉及文件：**
- 新建：`ui/RuleSectionParser.kt`（解析逻辑）
- 修改：`ui/MainScreen.kt`（规则概览入口 + BottomSheet）

---

## 功能 7：规则冲突检测

### 目标
扩展 `RulesValidator`，检测同一 pkg 下路径重叠、重复 section、allow/block 矛盾等冲突。

### 技术方案
**扩展 RulesValidator.validate()，新增冲突检测阶段：**

```kotlin
// 阶段2：冲突检测（在现有语法检测基础上追加）
data class SectionData(val line: Int, val allows: MutableList<String>, val blocks: MutableList<String>)
val sections = mutableMapOf<String, SectionData>()

// 检测项：
// 1. 重复 section：同一 pkg 出现两次
// 2. allow/block 路径前缀冲突：+ DCIM 且 - DCIM/Camera（子路径矛盾）
// 3. 空 section：有 [pkg] 但无任何规则行
// 4. redirect 源路径与 block 路径冲突
```

**涉及文件：**
- 修改：`ui/MainScreen.kt`（`RulesValidator` 扩展，约 40 行）

---

## 功能 8：多 ABI 打包辅助

### 目标
模块设置 BottomSheet 展示版本号，提供「复制打包命令」按钮（调 PC 剪贴板），不内嵌 PowerShell。

### 技术方案
```kotlin
// RulesRepository 新增
fun readModuleProp(): Result<Map<String, String>> = runCatching {
    val raw = SuFileInputStream.open(
        SuFile("/data/adb/modules/folder_manager/module.prop")
    ).bufferedReader().use { it.readText() }
    raw.lines()
        .filter { it.contains("=") && !it.startsWith("#") }
        .associate { line ->
            val idx = line.indexOf('=')
            line.substring(0, idx).trim() to line.substring(idx + 1).trim()
        }
}

// GUI：剪贴板复制打包命令
val clip = LocalClipboardManager.current
Button(onClick = { clip.setText(AnnotatedString("powershell -File scripts/build.ps1")) }) {
    Text("复制打包命令")
}
```

**涉及文件：**
- 修改：`data/RulesRepository.kt`（新增 `readModuleProp`）
- 修改：`ui/MainScreen.kt`（模块设置 BottomSheet 扩展）

---

## 实施优先级与工作量估算

| # | 功能 | 优先级 | 工作量 | 新文件 | 依赖变更 |
|---|------|--------|--------|--------|----------|
| 1 | 语法高亮 | 高 | M（~80行）| 1个 | 无 |
| 2 | 导入验证 | 高 | XS（~8行）| 无 | 无 |
| 3 | 预设搜索 | 中 | XS（~15行）| 无 | 无 |
| 5 | access.log 实时刷新 | 中 | XS（~20行）| 无 | 无 |
| 4 | 备份历史 | 中 | M（~60行）| 无 | 无 |
| 6 | 规则概览折叠卡片 | 中 | M（~80行）| 1个 | 无 |
| 7 | 规则冲突检测 | 低 | S（~40行）| 无 | 无 |
| 8 | 打包辅助 | 低 | S（~30行）| 无 | 无 |

**推荐实施顺序：** 2 → 3 → 5 → 1 → 4 → 6 → 7 → 8

---

## 关键技术决策

### 语法高亮
- **VisualTransformation**（选用）：不改变 state，光标 1:1 映射，零依赖
- BasicTextField + TextFieldValue AnnotatedString：需手动管理光标，复杂度高
- 第三方库：需引入 WebView，不适合简单 INI 格式

### 轮询刷新
- **LaunchedEffect + while(isActive) { delay() }**（选用）：官方推荐，自动随 Composition 生命周期取消
- Handler.postDelayed：View 系统方案，Compose 不推荐
- Flow + collectAsState：适合响应式流，此场景过重

### 备份历史
- **Shell ls -t + 保留前 5 条**（选用）：复用现有 libsu Shell，零新依赖
- Room 数据库：过重，备份文件本身即记录
- SharedPreferences 记录路径：冗余，直接扫描目录更可靠
