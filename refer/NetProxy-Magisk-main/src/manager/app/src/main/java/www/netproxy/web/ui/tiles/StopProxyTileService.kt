package www.netproxy.web.ui.tiles

import android.service.quicksettings.TileService
import com.topjohnwu.superuser.Shell

/**
 * 停止代理快捷设置磁贴
 * 
 * 点击时执行停止脚本停止代理服务
 */
class StopProxyTileService : TileService() {
    
    override fun onClick() {
        super.onClick()
        // 执行关闭代理命令
        Shell.cmd("/data/adb/modules/netproxy/scripts/core/stop.sh").submit()
    }
}
