#!/system/bin/sh

MODDIR=${0%/*}

if [ -f "$MODDIR/run/daemon.pid" ]; then
  pid=$(cat "$MODDIR/run/daemon.pid" 2>/dev/null)
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null
  fi
fi
exit 0
