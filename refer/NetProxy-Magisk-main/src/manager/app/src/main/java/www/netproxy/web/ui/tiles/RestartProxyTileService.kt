package www.netproxy.web.ui.tiles

import android.service.quicksettings.TileService
import com.topjohnwu.superuser.Shell

/**
 * 重启代理快捷设置磁贴
 * 
 * 点击时先停止再启动代理服务
 */
class RestartProxyTileService : TileService() {
    
    override fun onClick() {
        super.onClick()
        // 先停止再启动代理 (异步执行避免阻塞主线程)
        Shell.cmd(
            "/data/adb/modules/netproxy/scripts/core/stop.sh",
            "/data/adb/modules/netproxy/scripts/core/start.sh"
        ).submit()
    }
}
