## 版本 6.0.0（2026-02-27）

### 核心更新
* **全新原生管理界面**: 采用 **Miuix** 设计语言重构，带来更精致的视觉体验、更流畅的操作反馈以及显著的性能提升。详见 [Telegram 公告](https://t.me/NetProxy_Magisk/9971)。

### 主要变更
> [!IMPORTANT]
> 由于涉及大量文件结构变更，建议先完全卸载旧版本模块后再进行重新安装。

1. **模式切换逻辑重构**:
   - 废弃 WebUI 实时生成路由的逻辑，改为使用预设的静态配置文件（`rule.json`, `global.json`, `direct.json`）。
   - 彻底解决重启后无法自动恢复“全局/直连”模式的历史遗留问题。
2. **直连模式配置保护**:
   - 将直连模式依赖的 freedom 出站配置从 `outbounds/` 移至内部受保护路径 `confdir/routing/internal/proxy_freedom.json`。
   - 优化 `switch-mode.sh`，确保即使出站节点被清空，直连模式依然稳固可用。
3. **配置目录结构优化**:
   - **路由规则统一化**: 新增 `confdir/routing/` 文件夹，集中管理所有路由 JSON 及 `routing_rules.json` 规则库。
   - **TProxy 配置独立**: `tproxy.conf` 移入 `config/tproxy/` 文件夹，结构更清晰。
   - **文件排序优化**: 重新排列 `confdir` 下的文件前缀，提升系统加载逻辑的可读性。
4. **默认分组重构**:
   - 将原 `outbounds/` 根目录下的节点配置迁移至 `outbounds/default/` 子目录。
   - 自动同步更新 `module.conf` 中的 `CURRENT_CONFIG` 路径。

---
