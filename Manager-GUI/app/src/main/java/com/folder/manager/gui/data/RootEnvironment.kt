package com.folder.manager.gui.data

import com.topjohnwu.superuser.Shell

/**
 * 检测当前 Root 环境（Magisk / KernelSU / APatch），
 * 并返回对应的模块基础路径。
 */
object RootEnvironment {

    enum class RootType { MAGISK, KERNELSU, APATCH, UNKNOWN }

    data class RootInfo(
        val type: RootType,
        val modulesBase: String,
        val label: String,
        val compatible: Boolean,
    )

    private const val MAGISK_MODULES  = "/data/adb/modules"
    private const val KSU_MODULES     = "/data/adb/modules"   // KSU 与 Magisk 路径相同
    private const val APATCH_MODULES  = "/data/adb/modules"   // APatch 路径相同

    /** 同步检测，应在后台线程调用 */
    fun detect(): RootInfo {
        // 检测 KernelSU
        val ksuCheck = Shell.cmd("ksud --version 2>/dev/null || [ -f /data/adb/ksud ] && echo ksud").exec()
        if (ksuCheck.isSuccess || ksuCheck.out.any { it.contains("ksud", ignoreCase = true) }) {
            return RootInfo(RootType.KERNELSU, KSU_MODULES, "KernelSU", true)
        }

        // 检测 APatch
        val apatchCheck = Shell.cmd("apd --version 2>/dev/null || [ -f /data/adb/apd ] && echo apd").exec()
        if (apatchCheck.isSuccess || apatchCheck.out.any { it.contains("apd", ignoreCase = true) }) {
            return RootInfo(RootType.APATCH, APATCH_MODULES, "APatch", true)
        }

        // 检测 Magisk
        val magiskCheck = Shell.cmd("magisk --version 2>/dev/null").exec()
        if (magiskCheck.isSuccess && magiskCheck.out.isNotEmpty()) {
            val ver = magiskCheck.out.firstOrNull() ?: ""
            return RootInfo(RootType.MAGISK, MAGISK_MODULES, "Magisk $ver", true)
        }

        // 有 Root 但未知环境
        return RootInfo(RootType.UNKNOWN, MAGISK_MODULES, "Unknown Root", false)
    }

    /** 解析模块路径（根据检测结果） */
    fun moduleBase(info: RootInfo) = "${info.modulesBase}/folder_manager"
}
