package com.folder.manager.gui.data

import android.content.Context
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.io.SuFile
import com.topjohnwu.superuser.io.SuFileInputStream
import com.topjohnwu.superuser.io.SuFileOutputStream

// 模板库：从 assets/templates/ 加载内置模板，同时支持用户自定义模板持久化到设备。
object TemplateRepository {

    private const val USER_TPL_DIR = "/data/adb/modules/folder_manager/config/templates"

    data class Template(
        val name: String,
        val body: String,
        val isBuiltin: Boolean,
    )

    fun listAll(context: Context): List<Template> = loadBuiltin(context) + loadUserTemplates()

    fun loadBuiltin(context: Context): List<Template> = runCatching {
        context.assets.list("templates")?.mapNotNull { fileName ->
            if (!fileName.endsWith(".ini")) return@mapNotNull null
            val body = context.assets.open("templates/$fileName").bufferedReader().use { it.readText() }
            val name = body.lines().firstOrNull { it.startsWith("# name") }
                ?.substringAfter("=")?.trim() ?: fileName.removeSuffix(".ini")
            Template(name = name, body = body, isBuiltin = true)
        } ?: emptyList()
    }.getOrDefault(emptyList())

    fun loadUserTemplates(): List<Template> = runCatching {
        val ls = Shell.cmd("ls $USER_TPL_DIR/*.ini 2>/dev/null").exec()
        ls.out.filter { it.endsWith(".ini") }.mapNotNull { path ->
            val body = SuFileInputStream.open(SuFile(path.trim())).bufferedReader().use { it.readText() }
            val name = body.lines().firstOrNull { it.startsWith("# name") }
                ?.substringAfter("=")?.trim() ?: path.substringAfterLast("/").removeSuffix(".ini")
            Template(name = name, body = body, isBuiltin = false)
        }
    }.getOrDefault(emptyList())

    fun saveUserTemplate(name: String, body: String): Result<Unit> = runCatching {
        Shell.cmd("mkdir -p $USER_TPL_DIR").exec()
        val safeName = name.replace(Regex("[^\\w\\-]"), "_")
        val content = "# name = $name\n$body"
        SuFileOutputStream.open(SuFile("$USER_TPL_DIR/$safeName.ini"))
            .use { it.write(content.toByteArray(Charsets.UTF_8)) }
    }

    fun deleteUserTemplate(name: String): Result<Unit> = runCatching {
        val safeName = name.replace(Regex("[^\\w\\-]"), "_")
        Shell.cmd("rm -f $USER_TPL_DIR/$safeName.ini").exec()
    }
}
