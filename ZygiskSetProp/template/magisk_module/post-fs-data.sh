#!/system/bin/sh
# Please don't hardcode /magisk/modname/... ; instead, please use $MODDIR/...
# This will make your scripts compatible even if Magisk change its mount point in the future
MODDIR=${0%/*}

# This script will be executed in post-fs-data mode
# More info in the main Magisk thread

# 尽早设置 SELinux 为 permissive 模式
setenforce 0

# Set properties (requires Magisk)
magisk resetprop ro.debuggable 1
magisk resetprop ro.secure 0

# 设置 /data/local/tmp 目录权限
chmod 777 /data/local/tmp

# 设置 renderdoc.cfg 配置文件权限(如果存在)
chmod 666 /data/local/tmp/renderdoc.cfg 2>/dev/null

# 记录日志
log -t GraphicsTool "SELinux set to permissive mode (post-fs-data)"
log -t GraphicsTool "resetprop ro.debuggable=1 ro.secure=0 (post-fs-data)"
log -t GraphicsTool "Permissions set for /data/local/tmp"
