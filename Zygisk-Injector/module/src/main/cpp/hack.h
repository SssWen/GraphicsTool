//
// Created by Perfare on 2020/7/4.
//

#ifndef ZYGISK_IL2CPPDUMPER_HACK_H
#define ZYGISK_IL2CPPDUMPER_HACK_H

#include <stddef.h>

void hack_prepare(const char *game_data_dir, void *data, size_t length);
void hack_start(const char *game_data_dir);

#define EARLY_INJECT  // 编译时选项：提前注入模式（适用于 PLT/GOT Hook）
#endif //ZYGISK_IL2CPPDUMPER_HACK_H
