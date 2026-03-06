# [Zygisk-Injector](https://github.com/jiqiu2022/Zygisk-MyInjector)


我会初步进行框架层面的hook进行隐藏，进一步使用自定义linker来伪装成系统库的加载。

原项目https://github.com/Perfare/Zygisk-Il2CppDumper

本项目在原项目基础上做局部更改，请支持原项目作者劳动成果

1. 安装[Magisk](https://github.com/topjohnwu/Magisk) v24以上版本并开启Zygisk

2. 生成模块
   gradlew :module:assembleRelease 编译，zip包会生成在`out`文件夹下
3. 在Magisk里安装模块

4. 将要注入的so放入到/data/local/tmp 下，eg: libvkEGL.so


多模块注入已经开发完成：

```
void hack_start(const char *game_data_dir,JavaVM *vm) {
    load_so(game_data_dir,vm,"test");
    //如果要注入多个so，那么就在这里不断的添加load_so函数即可
}
````

# ZygiskSetProp

本项目是一个 Magisk/Zygisk 模块，用于在开机阶段执行一些系统属性设置（`magisk resetprop`）与权限/SELinux 相关命令。

## 功能

- 设置系统属性（示例）：
  - `magisk resetprop ro.debuggable 1`
  - `magisk resetprop ro.secure 0`
- 可选：在脚本中设置 SELinux/permissive、chmod 指定目录等

> 注意：此项目**不包含 SO 注入功能**，Zygisk 的 native 模块仅做最小化加载并立即 `DLCLOSE`。

## 配置位置

- `template/magisk_module/post-fs-data.sh`：更早执行，适合尽早生效的属性设置。
- `template/magisk_module/service.sh`：late_start service 阶段执行，适合需要等待系统启动后再执行的命令。

请在上述脚本中按需增删你的命令。

## 编译

- 生成 Magisk 模块 zip：
  - `gradlew :module:assembleRelease`

输出 zip 在 `out/` 目录。



