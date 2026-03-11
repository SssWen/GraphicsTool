# RenderDocTools

编译libvkEGL.so
cmake -DBUILD_ANDROID=1 -DANDROID_ABI=arm64-v8a -DANDROID_NATIVE_API_LEVEL=28 -DCMAKE_BUILD_TYPE=Release -DSTRIP_ANDROID_LIBRARY=On 

ninja

push 命令
adb push G:\aHook4\GraphicsTool\renderdoc-1.37\build_android64\lib\libvkEGL.so /data/local/tmp/

编译zygisk 模块

设置临时java环境

set "JAVA_HOME=C:\Program Files\Java\jdk-11"

set "PATH=%JAVA_HOME%\bin;%PATH%"

gradlew :module:assembleRelease

push 命令
adb push "G:\aHook4\GraphicsTool\Zygisk-Injector\out\Zygisk-Injector-v1.1-release.zip" /sdcard/