#include <cstring>
#include <thread>
#include <vector>
#include <fstream>
#include "hack.h"
#include "zygisk.hpp"
#include "game.h"
#include "log.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class ImGuiModMenu : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        auto package_name = env->GetStringUTFChars(args->nice_name, nullptr);
        auto app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        preSpecialize(package_name, app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, package_name);
        env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (enable_hack) {
            prehook_eglgetdisplay();
            std::thread hack_thread(hack_prepare, game_data_dir);
            hack_thread.detach();
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack;
    char *game_data_dir;
    std::vector<std::string> allowedPackages;
    bool configLoaded = false;

    bool loadConfig(const char* configPath) {
        std::ifstream configFile(configPath);
        if (!configFile.is_open()) {            
            return false;
        }
        std::string line;
        while (std::getline(configFile, line)) {
            if (line.empty() || line[0] == '#') continue;
            line.erase(line.find_last_not_of(" \n\r\t") + 1);
            allowedPackages.emplace_back(line);
        }
        configFile.close();
        return true;
    }

    void preSpecialize(const char *package_name, const char *app_data_dir) {
        if (!configLoaded) {
            loadConfig("/data/local/tmp/renderdoc.cfg");
            configLoaded = true;
        }
        bool is_allowed = std::any_of(allowedPackages.begin(), allowedPackages.end(),
            [package_name](const std::string& pkg) {
                return pkg == package_name;
            });
        if (is_allowed) {            
            enable_hack = true;
            game_data_dir = new char[strlen(app_data_dir) + 1];
            strcpy(game_data_dir, app_data_dir);
        } else {
            enable_hack = false;
        }
    }
};

REGISTER_ZYGISK_MODULE(ImGuiModMenu)