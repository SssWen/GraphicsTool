//
// Created by Perfare on 2020/7/4.
//

#ifndef ZYGISK_IL2CPPDUMPER_HACK_H
#define ZYGISK_IL2CPPDUMPER_HACK_H

#include <stddef.h>

void inject_from_memory(const void *so_data, size_t so_length, const char *game_data_dir);
#endif //ZYGISK_IL2CPPDUMPER_HACK_H
