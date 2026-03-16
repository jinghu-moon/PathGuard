package www.netproxy.web.ui

import android.app.Application
import android.os.Handler
import android.os.Looper
import com.topjohnwu.superuser.Shell
import java.util.concurrent.Executors

/**
 * 应用程序入口类
 * 
 * 负责初始化 Shell 配置和提供全局单例访问
 */
class App : Application() {
    companion object {
        /** 应用实例单例 */
        lateinit var instance: App
            private set
        
        /** 后台线程池，用于执行异步任务 */
        val executor by lazy { Executors.newCachedThreadPool() }
        
        /** 主线程 Handler，用于在主线程执行任务 */
        val handler = Handler(Looper.getMainLooper())
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        // 配置 Shell，启用 MOUNT_MASTER 标志以访问所有挂载点
        Shell.setDefaultBuilder(Shell.Builder.create().setFlags(Shell.FLAG_MOUNT_MASTER))
    }
}

