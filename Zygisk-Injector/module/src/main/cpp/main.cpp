#include <cstring>
#include <cerrno>
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
        if (enable_hack && so_data) {
            // postAppSpecialize 阶段进程已完成 fork + specialize，
            // Dobby hook 只需挂钩函数指针，不依赖进程名，可直接同步注入。
            inject_from_memory(so_data, so_length, _data_dir);
            munmap(so_data, so_length);
            so_data   = nullptr;
            so_length = 0;
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack = false;
    char *_data_dir = nullptr;
    void  *so_data   = nullptr;
    size_t so_length = 0;

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
        
        // 精确匹配白名单包名
        bool is_allowed = std::any_of(allowedPackages.begin(), allowedPackages.end(),
            [package_name](const std::string& pkg) {
                return pkg == package_name;
            });
        
        LOGD("🔍 Process: %s | Allowed: %s", 
             package_name, 
             is_allowed ? "YES" : "NO");
        
        if (is_allowed) {
            LOGI("✅ 成功找到目标进程: %s", package_name);
            enable_hack = true;
            _data_dir = new char[strlen(app_data_dir) + 1];
            strcpy(_data_dir, app_data_dir);

            // 在 zygote 进程（有 root 权限）里把 SO 映射到内存
            // fork 后子进程继承此映射，postAppSpecialize 直接写入无需再读文件
            {
                int fd = open("/data/local/tmp/libvkEGL.so", O_RDONLY);
                if (fd != -1) {
                    struct stat sb{};
                    fstat(fd, &sb);
                    so_length = sb.st_size;
                    so_data   = mmap(nullptr, so_length, PROT_READ, MAP_PRIVATE, fd, 0);
                    close(fd);
                    if (so_data == MAP_FAILED) {
                        LOGE("mmap failed for libvkEGL.so: %s", strerror(errno));
                        so_data   = nullptr;
                        so_length = 0;
                        enable_hack = false;
                    } else {
                        LOGI("mmap libvkEGL.so success: %p size=%zu", so_data, so_length);
                    }
                } else {
                    LOGE("open /data/local/tmp/libvkEGL.so failed: %s", strerror(errno));
                    enable_hack = false;
                }
            }
        } else {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            LOGD("⏭️ 跳过: %s (不在白名单)", package_name);
            enable_hack = false;
        }
    }
};

REGISTER_ZYGISK_MODULE(MyModule)