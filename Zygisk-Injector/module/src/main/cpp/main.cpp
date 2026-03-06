#include <cstring>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cinttypes>
#include "hack.h"
#include "zygisk.hpp"
#include "game.h"
#include "log.h"
#include "dlfcn.h"

#include <vector>
#include <fstream>

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class MyModule : public zygisk::ModuleBase {
public:
    // 添加白名单容器
    std::vector<std::string> allowedPackages;    
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;        
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        auto package_name = env->GetStringUTFChars(args->nice_name, nullptr);
        auto app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);

        LOGI("preAppSpecialize %s %s %d", package_name, app_data_dir,args->runtime_flags);

        preSpecialize(package_name, app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, package_name);
        env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (enable_hack) {
            // 简化: 不需要 JavaVM,直接启动注入线程
            LOGI("✅ 启动注入线程");
            std::thread hack_thread([this]() {
                hack_start(_data_dir);
            });
            hack_thread.detach();
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack = false;
    char *_data_dir = nullptr;
    void *data= nullptr;
    size_t length = 0;

    // 加载配置文件
    bool loadConfig(const char* configPath) {
        
        system("su -c 'setenforce 0'");
        // 修改目录权限为 777
        system("su -c 'chmod 777 /data/local/tmp'");
    
        // 修改文件权限
        system("su -c 'chmod 666 /data/local/tmp/renderdoc.cfg'");
        std::ifstream configFile(configPath);
        if (!configFile.is_open()) {
            LOGW("Failed to open config file: %s", configPath);
            return false;
        }

        std::string line;
        while (std::getline(configFile, line)) {
            // 跳过注释和空行
            if (line.empty() || line[0] == '#') continue;
            
            // 清理换行符和空格
            line.erase(line.find_last_not_of(" \n\r\t") + 1);
            allowedPackages.emplace_back(line);
            // LOGD("Loaded package: %s", line.c_str());
        }
        configFile.close();
        return true;
    }

    void preSpecialize(const char *package_name, const char *app_data_dir) {        
        // 只在第一次加载配置文件（避免重复加载和权限问题）
        if (allowedPackages.empty()) {
            bool is_load = loadConfig("/data/local/tmp/renderdoc.cfg");
            if (!is_load) {
                LOGW("⚠️ Failed to load config file");
            } else {
                LOGI("📋 Config loaded, %zu packages in whitelist", allowedPackages.size());
            }
        }
        
        // 检查是否包含冒号（子进程标识）
        bool is_main_process = (strchr(package_name, ':') == nullptr);
        
        // 改为前缀匹配，支持子进程（如 com.example.app:service）
        bool is_allowed = std::any_of(allowedPackages.begin(), allowedPackages.end(),[package_name](const std::string& pkg) {
            // 检查包名是否以白名单中的包名开头
            return strncmp(package_name, pkg.c_str(), pkg.length()) == 0 &&
                   (package_name[pkg.length()] == '\0' || package_name[pkg.length()] == ':');
        });
        
        // 添加详细调试日志
        LOGD("🔍 Process: %s | Main: %s | Allowed: %s", 
             package_name, 
             is_main_process ? "YES" : "NO",
             is_allowed ? "YES" : "NO");
        
        // 🔥 修改：支持注入所有进程（包括主进程和子进程）
        if (is_allowed)  // 移除 && is_main_process 条件
        {
            LOGI("✅ 成功找到目标进程: %s (主进程: %s)", 
                 package_name, 
                 is_main_process ? "是" : "否");
            enable_hack = true;
            _data_dir = new char[strlen(app_data_dir) + 1];
            strcpy(_data_dir, app_data_dir);

#if defined(__i386__)
            auto path = "zygisk/armeabi-v7a.so";
#endif
#if defined(__x86_64__)
            auto path = "zygisk/arm64-v8a.so";
#endif
#if defined(__i386__) || defined(__x86_64__)
            // 这段代码只在 x86 模拟器上执行
            // 真机 ARM 设备不会进入这里
            int dirfd = api->getModuleDir();
            int fd = openat(dirfd, path, O_RDONLY);
            if (fd != -1) {
                struct stat sb{};
                fstat(fd, &sb);
                length = sb.st_size;
                data = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
            } else {                
                enable_hack = false;
                LOGW("enable_hack set false %s", package_name);
            }
#endif
        } else {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            LOGD("⏭️ 跳过: %s (不在白名单)", package_name);
            enable_hack = false;
        }
    }
};

REGISTER_ZYGISK_MODULE(MyModule)