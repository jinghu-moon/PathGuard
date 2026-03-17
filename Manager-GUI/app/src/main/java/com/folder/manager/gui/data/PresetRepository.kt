package com.folder.manager.gui.data

import android.content.Context

/**
 * 预设规则包仓库，从 assets/presets/ 读取 INI 片段和元数据注释。
 */
object PresetRepository {

    data class PresetMeta(
        val name: String,
        val pkg: String,
        val verified: Boolean,
        val description: String,
        val fileName: String,
    )

    /**
     * 列出所有可用预设的元数据（仅解析头部注释，不读取规则体）。
     */
    fun listPresets(context: Context): List<PresetMeta> {
        val files = context.assets.list("presets") ?: return emptyList()
        return files
            .filter { it.endsWith(".ini") }
            .mapNotNull { fileName ->
                runCatching {
                    context.assets.open("presets/$fileName").bufferedReader().use { reader ->
                        parseMeta(reader.readText(), fileName)
                    }
                }.getOrNull()
            }
            .sortedWith(compareByDescending<PresetMeta> { it.verified }.thenBy { it.name })
    }

    /**
     * 读取指定预设的完整 INI 内容（跳过头部注释块）。
     */
    fun loadPresetContent(context: Context, fileName: String): Result<String> = runCatching {
        context.assets.open("presets/$fileName").bufferedReader().use { reader ->
            reader.readText().lines()
                .dropWhile { it.startsWith("#") || it.isBlank() }
                .joinToString("\n")
        }
    }

    // -------------------------------------------------------------------------
    // 内部
    // -------------------------------------------------------------------------

    private fun parseMeta(content: String, fileName: String): PresetMeta? {
        val meta = mutableMapOf<String, String>()
        for (line in content.lines()) {
            if (!line.startsWith("#")) break
            val stripped = line.removePrefix("#").trim()
            val colonIdx = stripped.indexOf(':')
            if (colonIdx < 0) continue
            val key = stripped.substring(0, colonIdx).trim()
            val value = stripped.substring(colonIdx + 1).trim()
            meta[key] = value
        }
        val name = meta["name"] ?: return null
        val pkg  = meta["package"] ?: return null
        return PresetMeta(
            name        = name,
            pkg         = pkg,
            verified    = meta["verified"]?.equals("true", ignoreCase = true) == true,
            description = meta["description"] ?: "",
            fileName    = fileName,
        )
    }
}
