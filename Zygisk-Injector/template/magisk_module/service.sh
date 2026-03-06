#!/system/bin/sh
# Please don't hardcode /magisk/modname/... ; instead, please use $MODDIR/...
# This will make your scripts compatible even if Magisk change its mount point in the future
MODDIR=${0%/*}

# This script will be executed in late_start service mode
# More info in the main Magisk thread

# 等待系统启动完成
sleep 10

# 设置 SELinux 为 permissive 模式
setenforce 0

# 设置 /data/local/tmp 目录权限
chmod 777 /data/local/tmp

# 设置 renderdoc.cfg 配置文件权限
chmod 666 /data/local/tmp/renderdoc.cfg

# 记录日志
log -t GraphicsTool "SELinux set to permissive mode"
log -t GraphicsTool "Permissions set for /data/local/tmp and renderdoc.cfg"
