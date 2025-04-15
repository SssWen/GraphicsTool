# [Zygisk-MyInjector](https://github.com/jiqiu2022/Zygisk-MyInjector)


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
```


   
