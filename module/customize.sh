#!/system/bin/sh

SKIPUNZIP=0

ui_print "- 安装 Folder Manager 模块"
ui_print "- 架构: ${ARCH}  API: ${API}"

# 检测运行环境
if [ "$KSU" = "true" ]; then
  ui_print "- 环境: KernelSU"
  ui_print "- 警告: 本模块需要 ZygiskNext，请确保已提前安装"
else
  ui_print "- 环境: Magisk"
fi

if [ -z "$API" ]; then
  API="$(getprop ro.build.version.sdk)"
fi

if [ -z "$API" ] || [ "$API" -lt 31 ]; then
  abort "! 仅支持 Android 12 及以上 (API 31). 当前 API: ${API:-unknown}"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/customize.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
if [ -f "$MODPATH/boot-completed.sh" ]; then
  set_perm "$MODPATH/boot-completed.sh" 0 0 0755
fi
set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
set_perm "$MODPATH/bin/reload.sh" 0 0 0755
set_perm_recursive "$MODPATH/config" 0 0 0755 0644

mkdir -p "$MODPATH/run"
set_perm_recursive "$MODPATH/run" 0 0 0755 0644

DAEMON_SRC=""
case "$ARCH" in
  arm64) DAEMON_SRC="$MODPATH/bin/arm64-v8a/folder_manager_daemon" ;;
  arm) DAEMON_SRC="$MODPATH/bin/armeabi-v7a/folder_manager_daemon" ;;
  x64) DAEMON_SRC="$MODPATH/bin/x86_64/folder_manager_daemon" ;;
  x86) DAEMON_SRC="$MODPATH/bin/x86/folder_manager_daemon" ;;
esac

if [ -n "$DAEMON_SRC" ] && [ -f "$DAEMON_SRC" ]; then
  cp -f "$DAEMON_SRC" "$MODPATH/bin/folder_manager_daemon"
  set_perm "$MODPATH/bin/folder_manager_daemon" 0 0 0755
fi

ui_print "- 规则文件: /data/adb/modules/folder_manager/config/rules.ini"
ui_print "- 原生库目录: /data/adb/modules/folder_manager/zygisk"
