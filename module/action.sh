#!/system/bin/sh

MODDIR=${0%/*}

echo "== Folder Manager =="
echo "module: $(grep '^name=' "$MODDIR/module.prop" 2>/dev/null | cut -d= -f2-)"
echo "version: $(grep '^version=' "$MODDIR/module.prop" 2>/dev/null | cut -d= -f2-)"
echo
echo "== Rules =="
cat "$MODDIR/config/rules.ini" 2>/dev/null || echo "rules.ini 不存在"
echo
echo "== Zygisk Libs =="
ls "$MODDIR/zygisk" 2>/dev/null || echo "zygisk 目录不存在"
echo
echo "== Daemon =="
if [ -f "$MODDIR/run/daemon.pid" ]; then
  echo "pid: $(cat "$MODDIR/run/daemon.pid" 2>/dev/null)"
  echo "reload: $MODDIR/bin/reload.sh"
  echo "self-check: $MODDIR/bin/folder_manager_daemon --self-check"
else
  echo "pid: none"
fi
echo
echo "== Last Log =="
tail -n 20 "$MODDIR/run/service.log" 2>/dev/null || echo "暂无日志"
echo
echo "== Daemon Log =="
tail -n 20 "$MODDIR/run/daemon.log" 2>/dev/null || echo "暂无日志"
