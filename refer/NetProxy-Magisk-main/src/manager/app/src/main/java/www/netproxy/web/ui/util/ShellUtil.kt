package www.netproxy.web.ui.util

import com.topjohnwu.superuser.Shell
import www.netproxy.web.ui.BuildConfig

/**
 * 创建新的 Root Shell 并执行代码块
 * 
 * @param globalMnt 是否启用全局挂载访问
 * @param block 在 Shell 上下文中执行的代码块
 * @return 代码块的返回值
 */
inline fun <T> withNewRootShell(
    globalMnt: Boolean = false,
    block: Shell.() -> T
): T {
    return createRootShell(globalMnt).use(block)
}

/**
 * 创建 Root Shell 实例
 * 
 * @param globalMnt 是否启用 MOUNT_MASTER 标志
 * @return Shell 实例
 */
fun createRootShell(globalMnt: Boolean = false): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
    if (globalMnt) {
        builder.setFlags(Shell.FLAG_MOUNT_MASTER)
    }
    return builder.build()
}

