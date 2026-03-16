#!/system/bin/sh

MODDIR=${0%/*}
mkdir -p "$MODDIR/run"
exec >>"$MODDIR/run/service.log" 2>&1

echo "[$(/system/bin/date '+%F %T' 2>/dev/null || date '+%F %T')] service.sh started"

DAEMON="$MODDIR/bin/folder_manager_daemon"
PID_FILE="$MODDIR/run/daemon.pid"
LOG_FILE="$MODDIR/run/daemon.log"

if [ -x "$DAEMON" ]; then
  if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
    echo "动态引擎已在运行: pid=$(cat "$PID_FILE" 2>/dev/null)"
  else
    rm -f "$PID_FILE"
    "$DAEMON" --config "$MODDIR/config/rules.ini" >>"$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    echo "动态引擎已启动: pid=$(cat "$PID_FILE" 2>/dev/null)"
  fi
else
  echo "未找到动态引擎可执行文件: $DAEMON"
fi
