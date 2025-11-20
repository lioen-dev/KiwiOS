#ifndef FS_EXT2_H
#define FS_EXT2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "drivers/blockdev.h"

typedef struct ext2_fs ext2_fs_t;
typedef struct ext2_inode ext2_inode_t;

typedef struct {
    uint32_t mode;
    uint32_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
} ext2_stat_t;

typedef struct {
    uint32_t ino;
    char name[256];
    uint8_t file_type; // 1=reg,2=dir, others ignored
} ext2_dirent_t;

ext2_fs_t* ext2_mount(block_device_t* dev);
void       ext2_unmount(ext2_fs_t* fs);

bool ext2_stat(ext2_fs_t* fs, const char* path, ext2_stat_t* st);
bool ext2_is_dir(ext2_fs_t* fs, const char* path);
bool ext2_is_file(ext2_fs_t* fs, const char* path);
bool ext2_create_empty(ext2_fs_t* fs, const char* path, uint16_t mode);
bool ext2_append(ext2_fs_t* fs, const char* path, const void* data, uint32_t len);
bool ext2_truncate(ext2_fs_t* fs, const char* path, uint32_t new_size);
bool ext2_replace(ext2_fs_t* fs, const char* path, const void* data, uint32_t len);

// Read a whole file into a newly kallocated buffer. Caller must kfree().
void* ext2_read_entire_file(ext2_fs_t* fs, const char* path, size_t* out_size);

// Directory helpers
bool ext2_listdir(ext2_fs_t* fs, const char* path, void (*cb)(const ext2_dirent_t*, void*), void* user);

// For shell
const char* ext2_get_cwd(void);
bool ext2_chdir(ext2_fs_t* fs, const char* path);

#endif // FS_EXT2_H
