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
//        if (strcmp(package_name, AimPackageName) == 0){
//            args->runtime_flags=8451;
//        }
        LOGI("WEN : preAppSpecialize %s %s %d", package_name, app_data_dir,args->runtime_flags);

        preSpecialize(package_name, app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, package_name);
        env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (enable_hack) {
            std::thread hack_thread(hack_prepare, _data_dir, data, length);
            hack_thread.detach();
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack;
    char *_data_dir;
    void *data;
    size_t length;

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
            LOGD("Loaded package: %s", line.c_str());
        }
        configFile.close();
        return true;
    }

    void preSpecialize(const char *package_name, const char *app_data_dir) {
        //|| strcmp(package_name, AimPackageName1) == 0 || strcmp(package_name, AimPackageName2) == 0 || strcmp(package_name, AimPackageName3) == 0 || strcmp(package_name, AimPackageName4) == 0) 
        // if (strcmp(package_name, AimPackageName) == 0 )

        bool is_load = loadConfig("/data/local/tmp/renderdoc.cfg"); // 配置文件路径
        if (!is_load) {
            LOGI("Failed to load config file");
            return;
        }
        bool is_allowed = std::any_of(allowedPackages.begin(), allowedPackages.end(),[package_name](const std::string& pkg) {
            return pkg == package_name;
        });
        // bool is_allowedAlways = std::any_of(allowedPackages.begin(), allowedPackages.end(),[package_name](const std::string& pkg) {
        //     return pkg == AimPackageName0;
        // });
        // if (is_allowedAlways)
        // {
        //     is_allowed = true;
        //     LOGI("暴力设置: %s", package_name);
        // }        
        if (is_allowed)
        {
            LOGI("成功找到目标进程: %s", package_name);
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
            LOGW("package 不在白名单上: %s", package_name);
            enable_hack = false;
        }
    }
};

REGISTER_ZYGISK_MODULE(MyModule)