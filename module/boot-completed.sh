#!/system/bin/sh

# KernelSU 独有生命周期：系统 boot 完成后执行
# 当前版本守护进程由 service.sh 负责启动，此处仅做状态确认

MODDIR=${0%/*}
LOG="$MODDIR/run/service.log"

exec >>"$LOG" 2>&1
echo "[$(/system/bin/date '+%F %T' 2>/dev/null || date '+%F %T')] boot-completed.sh: boot finished"

DAEMON="$MODDIR/bin/folder_manager_daemon"
PID_FILE="$MODDIR/run/daemon.pid"

# 若守护进程意外退出，尝试补拉起
if [ -x "$DAEMON" ]; then
  if ! ( [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null ); then
    rm -f "$PID_FILE"
    "$DAEMON" --config "$MODDIR/config/rules.ini" >>"$MODDIR/run/daemon.log" 2>&1 &
    echo $! > "$PID_FILE"
    echo "boot-completed.sh: 守护进程已补拉起 pid=$(cat "$PID_FILE" 2>/dev/null)"
  else
    echo "boot-completed.sh: 守护进程运行中 pid=$(cat "$PID_FILE" 2>/dev/null)"
  fi
fi
