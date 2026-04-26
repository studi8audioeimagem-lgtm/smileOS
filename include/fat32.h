#ifndef FAT32_H
#define FAT32_H

#include "types.h"

typedef struct {
    char name[32];
    u32 first_cluster;
    u32 size;
    bool is_dir;
} fat32_dirent_t;

bool fat32_mount_primary(void);
usize fat32_list_root(fat32_dirent_t *out, usize cap);
bool fat32_read_file(u32 first_cluster, u32 size, void *out, u32 out_cap, u32 *out_len);

#endif

