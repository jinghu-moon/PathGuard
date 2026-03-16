package www.netproxy.web.ui.tiles

import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import com.topjohnwu.superuser.Shell
import www.netproxy.web.ui.R

/**
 * 代理状态快捷设置磁贴
 * 
 * 智能显示代理运行状态，点击可切换开关：
 * - 运行中 → 点击停止
 * - 已停止 → 点击启动
 */
class ProxyStatusTileService : TileService() {
    
    companion object {
        /** Xray 可执行文件路径 */
        private const val XRAY_BIN = "/data/adb/modules/netproxy/bin/xray"
        /** 服务管理脚本路径 */
        private const val SERVICE_SCRIPT = "/data/adb/modules/netproxy/scripts/core/service.sh"
    }
    
    override fun onStartListening() {
        super.onStartListening()
        updateTileState()
    }
    
    override fun onClick() {
        super.onClick()
        
        val isRunning = getProxyStatus()
        val newState = !isRunning
        
        // 立即更新 UI 状态 (乐观更新)
        updateTileUi(newState)
        
        if (isRunning) {
            // 当前运行中，执行停止
            Shell.cmd("$SERVICE_SCRIPT stop").submit { 
                // 脚本执行完成后再次检查状态以确保一致性
                updateTileState()
            }
        } else {
            // 当前已停止，执行启动
            Shell.cmd("$SERVICE_SCRIPT start").submit {
                updateTileState()
            }
        }
    }
    
    private fun getProxyStatus(): Boolean {
        // 使用 pidof 检查 xray 进程是否运行
        val result = Shell.cmd("pidof -s $XRAY_BIN").exec()
        return result.isSuccess && result.out.isNotEmpty() && result.out[0].isNotBlank()
    }
    
    private fun updateTileState() {
        val isRunning = getProxyStatus()
        updateTileUi(isRunning)
    }

    private fun updateTileUi(isRunning: Boolean) {
        val tile = qsTile ?: return
        
        // 主标签保持为应用名称
        tile.label = getString(R.string.app_name)
        
        if (isRunning) {
            tile.state = Tile.STATE_ACTIVE
            val stateLabel = getString(R.string.tile_on)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
                tile.subtitle = stateLabel
            }
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                tile.stateDescription = stateLabel
            }
            tile.contentDescription = "${getString(R.string.app_name)} $stateLabel"
        } else {
            tile.state = Tile.STATE_INACTIVE
            val stateLabel = getString(R.string.tile_off)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
                tile.subtitle = stateLabel
            }
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                tile.stateDescription = stateLabel
            }
            tile.contentDescription = "${getString(R.string.app_name)} $stateLabel"
        }
        
        tile.updateTile()
    }
}
