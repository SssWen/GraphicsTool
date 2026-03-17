# RenderDocTools

## 修改记录 (RenderDoc 定制)

| 文件 | 修改内容 | 原值 | 新值 |
|------|----------|------|------|
| `core/core.cpp` | 禁用日志文件保存，仅输出到 logcat | `RDCLOGFILE(m_LoggingFilename.c_str())` | 已注释 |
| `common/globalconfig.h` | Target Control 端口 | 38920 | 41682 |
| `common/globalconfig.h` | Remote Server 端口 | 39920 | 42719 |
| `common/globalconfig.h` | Forward Port Base | 38950 | 41395 |
| `android/android.cpp` | JDWP 端口基址 | 39500 | 46528 |

> 端口修改用于降低被检测软件识别的风险。

## 启动塔防游戏
手机连接电脑后，双击 `launch_tower_defense.bat` 或执行：
```
adb shell am start -n com.run.tower.defense/com.unity3d.player.MyMainPlayerActivity
```

## 编译libvkEGL.so
cmake -DBUILD_ANDROID=1 -DANDROID_ABI=arm64-v8a -DANDROID_NATIVE_API_LEVEL=28 -DCMAKE_BUILD_TYPE=Release -DSTRIP_ANDROID_LIBRARY=On 

ninja

## push 命令
adb push G:\aHook4\GraphicsTool\renderdoc-1.37\build_android64\lib\libvkEGL.so /data/local/tmp/

## 编译zygisk 模块

设置临时java环境

set "JAVA_HOME=C:\Program Files\Java\jdk-11"

set "PATH=%JAVA_HOME%\bin;%PATH%"

gradlew :module:assembleRelease

## push 命令
adb push "G:\aHook4\GraphicsTool\Zygisk-Injector\out\Zygisk-Injector-v1.1-release.zip" /sdcard/

## 查看当前运行apk包名
adb shell dumpsys window | findstr mCurrentFocus