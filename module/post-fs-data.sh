#!/system/bin/sh

MODDIR=${0%/*}

# 当前阶段仅保留生命周期入口，避免过早引入复杂逻辑。
# 如果后续切换到更早期的 namespace 或策略初始化，可在此扩展。
mkdir -p "$MODDIR/run"

