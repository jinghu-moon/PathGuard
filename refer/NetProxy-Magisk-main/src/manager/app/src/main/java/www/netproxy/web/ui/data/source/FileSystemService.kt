package www.netproxy.web.ui.data.source

import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import androidx.annotation.MainThread
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ipc.RootService
import com.topjohnwu.superuser.nio.FileSystemManager
import www.netproxy.web.ui.App
import java.util.concurrent.CopyOnWriteArraySet

/**
 * 文件系统服务
 * 
 * 以 Root 权限运行，提供对设备文件系统的访问能力
 * 用于读取模块目录下的 WebUI 资源文件
 */
class FileSystemService : RootService() {
    override fun onBind(intent: Intent): IBinder {
        return FileSystemManager.getService()
    }

    /**
     * 服务状态监听器接口
     */
    interface Listener {
        /** 服务可用时回调 */
        fun onServiceAvailable(fs: FileSystemManager)
        /** 启动失败时回调（无 Root 权限） */
        fun onLaunchFailed()
    }

    companion object {
        private sealed class Status {
            data object Uninitialized : Status()
            data object CheckRoot : Status()
            data class ServiceAvailable(val fs: FileSystemManager) : Status()
        }

        private var status: Status = Status.Uninitialized
        private val connection = object : ServiceConnection {
            override fun onServiceConnected(p0: ComponentName, p1: IBinder) {
                val fs = FileSystemManager.getRemote(p1)
                status = Status.ServiceAvailable(fs)
                pendingListeners.forEach { l ->
                    l.onServiceAvailable(fs)
                    pendingListeners.remove(l)
                }
            }

            override fun onServiceDisconnected(p0: ComponentName) {
                status = Status.Uninitialized
            }

        }
        private val pendingListeners = CopyOnWriteArraySet<Listener>()

        @MainThread
        fun start(listener: Listener) {
            (status as? Status.ServiceAvailable)?.let {
                listener.onServiceAvailable(it.fs)
                return
            }
            pendingListeners.add(listener)
            if (status == Status.Uninitialized) {
                checkRoot()
            }
        }

        private fun checkRoot() {
            status = Status.CheckRoot
            App.executor.submit {
                val isRoot = Shell.Builder.create().setFlags(Shell.FLAG_MOUNT_MASTER).build().use {
                    it.isRoot
                }
                App.handler.post {
                    if (isRoot) {
                        launchService()
                    } else {
                        status = Status.Uninitialized
                        pendingListeners.forEach { l ->
                            l.onLaunchFailed()
                            pendingListeners.remove(l)
                        }
                    }
                }
            }
        }

        private fun launchService() {
            bind(Intent(App.instance, FileSystemService::class.java), connection)
        }

        fun removeListener(listener: Listener) {
            pendingListeners.remove(listener)
        }
    }
}
