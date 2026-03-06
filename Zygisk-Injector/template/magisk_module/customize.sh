#!/system/bin/sh

# 这个脚本在模块安装时执行
# 用于设置文件权限

# 设置脚本文件为可执行
set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/service.sh 0 0 0755
set_perm $MODPATH/post-fs-data.sh 0 0 0755

ui_print "- 设置脚本权限完成"
ui_print "- SELinux 将在重启后自动设置为 permissive 模式"
