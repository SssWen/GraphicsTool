#include "hack.h"
#include "log.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

// 将已 mmap 到内存的 SO 写入 game_data_dir/files/ 并 dlopen
// 在 postAppSpecialize 中同步调用，无需线程和 sleep 重试
void inject_from_memory(const void *so_data, size_t so_length, const char *game_data_dir) {
    if (!so_data || so_length == 0 || !game_data_dir) {
        LOGE("inject_from_memory: invalid arguments");
        return;
    }

    // 1. 确保 files/ 目录存在
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/files", game_data_dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        LOGE("mkdir %s failed: %s", dir, strerror(errno));
        return;
    }

    // 2. 写入 SO 文件
    char dest[256];
    snprintf(dest, sizeof(dest), "%s/libvkEGL.so", dir);
    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOGE("open dest %s failed: %s", dest, strerror(errno));
        return;
    }
    const uint8_t *ptr = static_cast<const uint8_t *>(so_data);
    size_t remaining = so_length;
    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        if (written < 0) {
            LOGE("write to %s failed: %s", dest, strerror(errno));
            close(fd);
            return;
        }
        ptr       += written;
        remaining -= written;
    }
    close(fd);

    // 3. 设置可执行权限
    if (chmod(dest, 0755) != 0) {
        LOGE("chmod %s failed: %s", dest, strerror(errno));
        return;
    }

    // 4. 单次 dlopen（文件是自己刚写的，无需重试）
    LOGI("inject_from_memory: dlopen %s", dest);
    void *handle = dlopen(dest, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        LOGE("dlopen %s failed: %s", dest, dlerror());
    } else {
        LOGI("inject_from_memory: SUCCESS %s handle=%p", dest, handle);
    }
}
