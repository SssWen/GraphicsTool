# [Zygisk-MyInjector](https://github.com/jiqiu2022/Zygisk-MyInjector)



最新开发进度：**多模块注入完成，maps隐藏注入完成**（并且修复了riru内存泄漏问题）     现在仍然面临soinfo链表遍历无法隐藏的情况

我会初步进行框架层面的hook进行隐藏，进一步使用自定义linker来伪装成系统库的加载。

原项目https://github.com/Perfare/Zygisk-Il2CppDumper

本项目在原项目基础上做局部更改，请支持原项目作者劳动成果

1. 安装[Magisk](https://github.com/topjohnwu/Magisk) v24以上版本并开启Zygisk

2. 生成模块
   - GitHub Actions
     1. Fork这个项目
     2. 在你fork的项目中选择**Actions**选项卡
     3. 在左边的侧边栏中，单击**Build**
     4. 选择**Run workflow**
     5. 输入游戏包名并点击**Run workflow**
     6. 等待操作完成并下载
   - Android Studio
     1. 下载源码
     2. 编辑`game.h`, 修改`GamePackageName`为游戏包名
     3. 使用Android Studio运行gradle任务`:module:assembleRelease`编译，zip包会生成在`out`文件夹下

3. 在Magisk里安装模块

4. 将要注入的so放入到/data/local/tmp下修改为test.so

   (部分手机第一次注入不会成功，请重启，再之后的注入会成功)

多模块注入已经开发完成：

```
void hack_start(const char *game_data_dir,JavaVM *vm) {
    load_so(game_data_dir,vm,"test");
    //如果要注入多个so，那么就在这里不断的添加load_so函数即可
}
```


   
