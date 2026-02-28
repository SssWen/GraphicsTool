#include "hack.h"
#include "log.h"
#include "xdl.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <linux/unistd.h>
#include <array>
#include <sys/stat.h>
//#include <asm-generic/fcntl.h>
#include <fcntl.h>
#include "newriruhide.h"
void load_so(const char *game_data_dir, JavaVM *vm, const char *soname) {
    bool load = false;
    LOGI("hack_start %s", game_data_dir);

    // 构建新文件路径，使用传入的 soname 参数
    char new_so_path[256];
    snprintf(new_so_path, sizeof(new_so_path), "%s/files/%s.so", game_data_dir, soname);
    LOGI("加载so路径 %s", new_so_path);

    // 构建源文件路径
    char src_path[256];
    snprintf(src_path, sizeof(src_path), "/data/local/tmp/%s.so", soname);

    // 打开源文件
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        LOGE("Failed to open %s: %s (errno: %d)", src_path, strerror(errno), errno);
        return;
    }

    // 打开目标文件
    int dest_fd = open(new_so_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    LOGI("应用 so 地址  %s", new_so_path);


    // 复制文件内容,将 /data/local/tmp/%s.so 复制到 %s/files/%s.so
    // 将 /data/local/tmp/libvkEGL.so 复制到  /data/user/0/com.gzgroup.lproject3/files/libvkEGL.so
    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(src_fd, buffer, sizeof(buffer))) > 0) {
        if (write(dest_fd, buffer, bytes) != bytes) {            
            LOGE("复制so到应用失败 %s", new_so_path);
            close(src_fd);
            close(dest_fd);
            return;
        }
    }
    LOGI("复制so到应用成功 %s", new_so_path);

    // 关闭文件描述符
    close(src_fd);
    close(dest_fd);

    // 修改文件权限
    if (chmod(new_so_path, 0755) != 0) {
        LOGE("Failed to change permissions on %s: %s (errno: %d)", new_so_path, strerror(errno), errno);
        return;
    } else {
        LOGI("Successfully changed permissions to 755 on %s", new_so_path);
    }

    LOGI("Attempting to load %s using dlopen", new_so_path);
    void * handle;
    for (int i = 0; i < 10; i++) {
        handle = dlopen(new_so_path, RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            LOGI("Successfully loaded %s using dlopen", new_so_path);
            load = true;                 
            break;
        } else {
            LOGE("Failed to load %s: %s", new_so_path, dlerror());
            sleep(1);
        }
    }
}
void hack_start(const char *game_data_dir,JavaVM *vm) {    
    load_so(game_data_dir,vm,"libvkEGL");//libvkEGL.so
    //如果要注入多个so，那么就在这里不断的添加load_so函数即可
}

std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    vms->AttachCurrentThread(&env, nullptr);
    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz,
                                                              currentApplicationId);
            jclass application_clazz = env->GetObjectClass(application);
            if (application_clazz) {
                jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                  "getApplicationInfo",
                                                                  "()Landroid/content/pm/ApplicationInfo;");
                if (get_application_info) {
                    jobject application_info = env->CallObjectMethod(application,
                                                                     get_application_info);
                    jfieldID native_library_dir_id = env->GetFieldID(
                            env->GetObjectClass(application_info), "nativeLibraryDir",
                            "Ljava/lang/String;");
                    if (native_library_dir_id) {
                        auto native_library_dir_jstring = (jstring) env->GetObjectField(
                                application_info, native_library_dir_id);
                        auto path = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                        LOGI("lib dir %s", path);
                        std::string lib_dir(path);
                        env->ReleaseStringUTFChars(native_library_dir_jstring, path);
                        return lib_dir;
                    } else {
                        LOGE("nativeLibraryDir not found");
                    }
                } else {
                    LOGE("getApplicationInfo not found");
                }
            } else {
                LOGE("application class not found");
            }
        } else {
            LOGE("currentApplication not found");
        }
    } else {
        LOGE("ActivityThread not found");
    }
    return {};
}

static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;

    void *(*loadLibrary)(const char *libpath, int flag);

    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);

    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;

    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};


void hack_prepare(const char *_data_dir, void *data, size_t length) {
    LOGI("hack thread: %d", gettid());
    int api_level = android_get_device_api_level();
    LOGI("api level: %d", api_level);
    // usleep(500000*6);  // 500ms，而不是 2 秒
    
    LOGI("✅ Process stabilized, injecting now...");
    hack_start(_data_dir, nullptr);
}

#if defined(__arm__) || defined(__aarch64__)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    std::thread hack_thread(hack_start, game_data_dir,vm);
    hack_thread.detach();
    return JNI_VERSION_1_6;
}

#endif