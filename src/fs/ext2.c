#include "fs/ext2.h"
#include "drivers/blockdev.h"
#include "drivers/timer.h"
#include "memory/heap.h"
#include "core/process.h"
#include "lib/string.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declaration for block free helper used by truncate
static bool free_block_in_group(ext2_fs_t* fs, uint32_t blk);

// Seconds since boot (monotonic). Falls back to 0 if PIT not set.
static inline uint32_t now_seconds(void) {
    uint32_t f = timer_get_frequency();
    if (!f) return 0;
    return (uint32_t)(timer_get_ticks() / f);
}

// ---------------- On-disk structures ----------------
#pragma pack(push,1)
typedef struct {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    int32_t  log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    int16_t  max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    // Extended (rev >= 1):
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
    char     last_mounted[64];
    uint32_t algo_bitmap;
    // Many more fields exist; we only need the above.
} ext2_super_t;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint32_t reserved[3];
} ext2_group_desc_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15]; // 0-11 direct, 12 singly, 13 doubly, 14 triply
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t  osd2[12];
} ext2_inode_disk_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} ext2_dirent_disk_t;
#pragma pack(pop)

// ---------------- In-memory FS object ----------------
struct ext2_fs {
    block_device_t* dev;
    ext2_super_t sb;
    ext2_group_desc_t* gdt; // array
    uint32_t block_size;
    uint32_t groups;
};

static char kernel_cwd[512] = "/";

static char* current_process_cwd(void) {
    process_t* proc = process_current();
    if (proc) {
        if (!proc->cwd_initialized) {
            proc->cwd[0] = '/';
            proc->cwd[1] = '\0';
            proc->cwd_initialized = true;
        }
        return proc->cwd;
    }

    return kernel_cwd;
}

#define g_cwd (current_process_cwd())

// Forward declarations for helpers that are referenced before their
// definitions lower in the file.
bool ext2_create_empty(ext2_fs_t* fs, const char* path, uint16_t mode);

// ---------------- Helpers ----------------
static bool write_block(ext2_fs_t* fs, uint32_t blk, const void* src);
static bool alloc_block(ext2_fs_t* fs, uint32_t* out_blk);

static uint32_t div_ceil_u32(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static bool read_bytes(block_device_t* dev, uint64_t offset, uint32_t n, void* out) {
    // offset in bytes from start of partition
    uint64_t first_lba = offset / SECTOR_SIZE;
    uint32_t byte_off  = (uint32_t)(offset % SECTOR_SIZE);
    uint8_t* dst = (uint8_t*)out;
    uint32_t remaining = n;

    uint8_t tmp[SECTOR_SIZE];
    if (byte_off) {
        if (!block_read(dev, first_lba, 1, tmp)) return false;
        uint32_t take = SECTOR_SIZE - byte_off;
        if (take > remaining) take = remaining;
        memcpy(dst, tmp + byte_off, take);
        dst += take; remaining -= take; first_lba++;
    }
    while (remaining >= SECTOR_SIZE) {
        uint32_t secs = remaining / SECTOR_SIZE;
        if (secs > 128) secs = 128; // chunk
        if (!block_read(dev, first_lba, secs, dst)) return false;
        first_lba += secs;
        dst += secs * SECTOR_SIZE;
        remaining -= secs * SECTOR_SIZE;
    }
    if (remaining) {
        if (!block_read(dev, first_lba, 1, tmp)) return false;
        memcpy(dst, tmp, remaining);
    }
    return true;
}

static bool read_block(ext2_fs_t* fs, uint32_t blk, void* out) {
    uint64_t off = (uint64_t)blk * fs->block_size;
    return read_bytes(fs->dev, off, fs->block_size, out);
}

static bool read_inode(ext2_fs_t* fs, uint32_t ino, ext2_inode_disk_t* out) {
    if (ino == 0 || ino > fs->sb.inodes_count) return false;
    uint32_t group = (ino - 1) / fs->sb.inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.inodes_per_group;
    ext2_group_desc_t* gd = &fs->gdt[group];

    uint32_t inodes_per_block = fs->block_size / ((fs->sb.rev_level >= 1 && fs->sb.inode_size) ? fs->sb.inode_size : 128);
    uint32_t block_ofs = index / inodes_per_block;
    uint32_t offset    = index % inodes_per_block;

    uint32_t table_block = gd->inode_table + block_ofs;
    uint8_t* buf = (uint8_t*)kmalloc(fs->block_size);
    if (!buf) return false;
    bool ok = read_block(fs, table_block, buf);
    if (!ok) { kfree(buf); return false; }

    uint32_t isz = (fs->sb.rev_level >= 1 && fs->sb.inode_size) ? fs->sb.inode_size : 128;
    memcpy(out, buf + offset * isz, sizeof(*out));
    kfree(buf);
    return true;
}

static bool write_inode(ext2_fs_t* fs, uint32_t ino, const ext2_inode_disk_t* in) {
    if (ino == 0 || ino > fs->sb.inodes_count) return false;
    uint32_t group = (ino - 1) / fs->sb.inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.inodes_per_group;
    ext2_group_desc_t* gd = &fs->gdt[group];

    uint32_t isz = (fs->sb.rev_level >= 1 && fs->sb.inode_size) ? fs->sb.inode_size : 128;
    uint32_t inodes_per_block = fs->block_size / isz;
    uint32_t block_ofs = index / inodes_per_block;
    uint32_t slot_ofs  = index % inodes_per_block;

    uint32_t table_block = gd->inode_table + block_ofs;
    uint8_t* buf = (uint8_t*)kmalloc(fs->block_size);
    if (!buf) return false;
    if (!read_block(fs, table_block, buf)) { kfree(buf); return false; }

    memcpy(buf + slot_ofs * isz, in, sizeof(*in));
    bool ok = write_block(fs, table_block, buf);
    kfree(buf);
    return ok;
}


static bool append_block_to_inode(ext2_fs_t* fs, ext2_inode_disk_t* ino, uint32_t* out_blk) {
    // Direct blocks first
    uint32_t idx = 0; while (idx < 12 && ino->block[idx]) idx++;
    if (idx < 12) {
        uint32_t nb; if (!alloc_block(fs, &nb)) return false;
        ino->block[idx] = nb; if (out_blk) *out_blk = nb; return true;
    }

    uint32_t per = fs->block_size / 4;

    // Single indirect
    if (!ino->block[12]) { uint32_t ind; if (!alloc_block(fs,&ind)) return false; ino->block[12]=ind; }
    uint32_t* l1 = (uint32_t*)kmalloc(fs->block_size); if (!l1) return false;
    if (!read_block(fs, ino->block[12], l1)) { kfree(l1); return false; }
    for (uint32_t i=0;i<per;i++) if (!l1[i]) {
        uint32_t nb; if (!alloc_block(fs,&nb)) { kfree(l1); return false; }
        l1[i]=nb; bool ok=write_block(fs, ino->block[12], l1); kfree(l1);
        if (!ok) return false;
            if (out_blk) *out_blk = nb;
            return true;
    }
    kfree(l1);

    // Double indirect (minimal)
    if (!ino->block[13]) { uint32_t dbl; if (!alloc_block(fs,&dbl)) return false; ino->block[13]=dbl; }
    uint32_t* l2 = (uint32_t*)kmalloc(fs->block_size); if (!l2) return false;
    if (!read_block(fs, ino->block[13], l2)) { kfree(l2); return false; }
    for (uint32_t i=0;i<per;i++) {
        if (!l2[i]) { uint32_t l1b; if(!alloc_block(fs,&l1b)){kfree(l2);return false;} l2[i]=l1b; if(!write_block(fs,ino->block[13],l2)){kfree(l2);return false;} }
        uint32_t* l1b=(uint32_t*)kmalloc(fs->block_size); if(!l1b){kfree(l2);return false;}
        if(!read_block(fs,l2[i],l1b)){kfree(l1b);kfree(l2);return false;}
        for (uint32_t j=0;j<per;j++) if (!l1b[j]) {
            uint32_t nb; if(!alloc_block(fs,&nb)){kfree(l1b);kfree(l2);return false;}
            l1b[j]=nb; bool ok=write_block(fs,l2[i],l1b); kfree(l1b); kfree(l2);
            if (!ok) return false;
            if (out_blk) *out_blk = nb;
            return true;
        }
        kfree(l1b);
    }
    kfree(l2);
    return false; // triple-indirect not implemented
}
static uint32_t get_block_from_inode(ext2_fs_t* fs, const ext2_inode_disk_t* ino, uint32_t file_block_index) {
    // direct
    if (file_block_index < 12) return ino->block[file_block_index];

    // singly indirect
    file_block_index -= 12;
    if (file_block_index < (fs->block_size / 4)) {
        if (ino->block[12] == 0) return 0;
        uint32_t* ibuf = (uint32_t*)kmalloc(fs->block_size);
        if (!ibuf) return 0;
        if (!read_block(fs, ino->block[12], ibuf)) { kfree(ibuf); return 0; }
        uint32_t b = ibuf[file_block_index];
        kfree(ibuf);
        return b;
    }

    // doubly indirect (optional - basic support)
    file_block_index -= (fs->block_size / 4);
    uint32_t per = fs->block_size / 4;
    if (file_block_index < per * per && ino->block[13]) {
        uint32_t* l1 = (uint32_t*)kmalloc(fs->block_size);
        uint32_t* l2 = (uint32_t*)kmalloc(fs->block_size);
        if (!l1 || !l2) { if(l1)kfree(l1); if(l2)kfree(l2); return 0; }
        if (!read_block(fs, ino->block[13], l1)) { kfree(l1); kfree(l2); return 0; }
        uint32_t i1 = file_block_index / per;
        uint32_t i2 = file_block_index % per;
        uint32_t blk1 = l1[i1];
        if (!blk1) { kfree(l1); kfree(l2); return 0; }
        if (!read_block(fs, blk1, l2)) { kfree(l1); kfree(l2); return 0; }
        uint32_t b = l2[i2];
        kfree(l1); kfree(l2);
        return b;
    }

    // triply indirect not implemented
    return 0;
}

static bool read_file(ext2_fs_t* fs, const ext2_inode_disk_t* ino, uint32_t pos, uint32_t len, void* out) {
    if (len == 0) return true;
    uint8_t* dst = (uint8_t*)out;
    uint32_t remaining = len;
    uint32_t block_size = fs->block_size;

    while (remaining) {
        uint32_t fb = pos / block_size;
        uint32_t off = pos % block_size;
        uint32_t blk = get_block_from_inode(fs, ino, fb);
        if (blk == 0) return false;

        uint8_t* buf = (uint8_t*)kmalloc(block_size);
        if (!buf) return false;
        if (!read_block(fs, blk, buf)) { kfree(buf); return false; }

        uint32_t tocopy = block_size - off;
        if (tocopy > remaining) tocopy = remaining;
        memcpy(dst, buf + off, tocopy);

        kfree(buf);
        dst += tocopy;
        remaining -= tocopy;
        pos += tocopy;
    }
    return true;
}


// Return parent inode for a directory by reading its ".." entry.
static uint32_t find_parent_inode(ext2_fs_t* fs, uint32_t dir_ino) {
    // Root's parent is itself.
    if (dir_ino == 2) return 2;

    ext2_inode_disk_t dino;
    if (!read_inode(fs, dir_ino, &dino)) return 2;
    if ((dino.mode & 0xF000) != 0x4000) return 2; // not a directory

    // Look only at the first block; "." and ".." are guaranteed to be there.
    uint32_t blk = get_block_from_inode(fs, &dino, 0);
    if (blk == 0) return 2;

    uint8_t* buf = (uint8_t*)kmalloc(fs->block_size);
    if (!buf) return 2;
    if (!read_block(fs, blk, buf)) { kfree(buf); return 2; }

    uint32_t off = 0;
    while (off + 8 <= fs->block_size) { // minimal sane rec_len
        ext2_dirent_disk_t* de = (ext2_dirent_disk_t*)(buf + off);
        if (de->rec_len < 8) break;
        if (de->rec_len > fs->block_size - off) break;
        if (de->inode && de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
            uint32_t p = de->inode;
            kfree(buf);
            return p ? p : 2;
        }
        off += de->rec_len;
    }
    kfree(buf);
    return 2;
}
static uint32_t path_to_inode(ext2_fs_t* fs, const char* path) {
    // empty or root -> inode 2
    if (!path || !*path || (path[0]=='/' && path[1]==0)) return 2;
    ext2_inode_disk_t ino2;
    if (!read_inode(fs, 2, &ino2)) return 0;

    // Choose start directory (absolute vs relative)
    const char* p = path;
    if (*p == '/') {
        while (*p == '/') ++p;
    } else {
        // relative to cwd: resolve g_cwd first, then append p
        char temp[512];
        // ensure cwd ends without trailing slash except it is "/"
        size_t l = strlen(g_cwd);
        size_t i = 0;
        if (l == 1 && g_cwd[0] == '/') {
            temp[0] = 0;
        } else {
            for (i=0;i<l;i++) temp[i] = g_cwd[i];
            temp[i++] = '/';
            temp[i] = 0;
        }
        // append p
        size_t j=0; while (p[j] && i+1<sizeof(temp)) temp[i++] = p[j++];
        temp[i]=0;
        p = temp;
        if (*p == '/') { while (*p=='/') ++p; }
    }

    uint32_t cur = 2;
    if (!read_inode(fs, cur, &ino2)) return 0;

    char component[256];
    while (*p) {
        // extract next component
        size_t k=0;
        while (*p && *p!='/') { if (k<sizeof(component)-1) component[k++]=*p; ++p; }
        component[k]=0;
        while (*p=='/') ++p; // skip slashes

        if (k==0) break; // trailing slash
        if (k==1 && component[0]=='.') continue;
        if (k==2 && component[0]=='.' && component[1]=='.') { cur = find_parent_inode(fs, cur); continue; }

        // iterate directory entries of 'cur'
        ext2_inode_disk_t dino;
        if (!read_inode(fs, cur, &dino)) return 0;
        if (!((dino.mode & 0xF000) == 0x4000)) return 0; // not a dir

        uint32_t size = dino.size_lo;
        uint32_t pos = 0;
        bool found=false;
        while (pos < size) {
            uint8_t* buf = (uint8_t*)kmalloc(fs->block_size);
            if (!buf) return 0;
            // read file block (pos / block_size)
            uint32_t fb = pos / fs->block_size;
            uint32_t blk = get_block_from_inode(fs, &dino, fb);
            if (blk == 0) { kfree(buf); return 0; }
            if (!read_block(fs, blk, buf)) { kfree(buf); return 0; }

            uint32_t inner = pos % fs->block_size;
            while (inner < fs->block_size && pos < size) {
                ext2_dirent_disk_t* de = (ext2_dirent_disk_t*)(buf + inner);
                if (de->inode && de->name_len) {
                    char name[256];
                    uint32_t nl = de->name_len;
                    if (nl >= sizeof(name)) nl = sizeof(name)-1;
                    memcpy(name, de->name, nl); name[nl]=0;

                    if (strcmp(name, component) == 0) {
                        cur = de->inode;
                        found = true;
                        break;
                    }
                }
                if (de->rec_len == 0) { break; }
                pos += de->rec_len;
                inner += de->rec_len;
            }
            kfree(buf);
            if (found) break;
        }
        if (!found) return 0;
    }
    return cur;
}

// ---------------- Public API ----------------
ext2_fs_t* ext2_mount(block_device_t* dev) {
    if (!dev) return 0;

    ext2_super_t sb;
    if (!read_bytes(dev, 1024, sizeof(sb), &sb)) return 0;
    if (sb.magic != 0xEF53) return 0;

    ext2_fs_t* fs = (ext2_fs_t*)kmalloc(sizeof(ext2_fs_t));
    if (!fs) return 0;
    memset(fs, 0, sizeof(*fs));

    fs->dev = dev;
    fs->sb = sb;
    fs->block_size = 1024u << sb.log_block_size;
    uint32_t data_blocks = (sb.blocks_count > sb.first_data_block)
        ? (sb.blocks_count - sb.first_data_block) : sb.blocks_count;
    fs->groups = div_ceil_u32(data_blocks, sb.blocks_per_group);

    // Read GDT (immediately after superblock)
    uint32_t gdt_start_block = (fs->block_size == 1024) ? 2 : 1;
    uint32_t gdt_bytes = fs->groups * sizeof(ext2_group_desc_t);
    uint32_t gdt_blocks = div_ceil_u32(gdt_bytes, fs->block_size);
    fs->gdt = (ext2_group_desc_t*)kmalloc(gdt_blocks * fs->block_size);
    if (!fs->gdt) { kfree(fs); return 0; }

    for (uint32_t i=0; i<gdt_blocks; ++i) {
        if (!read_block(fs, gdt_start_block + i, (uint8_t*)fs->gdt + i*fs->block_size)) {
            kfree(fs->gdt); kfree(fs); return 0;
        }
    }

    // Initialize CWD
    g_cwd[0] = '/'; g_cwd[1] = 0;
    return fs;
}

void ext2_unmount(ext2_fs_t* fs) {
    if (!fs) return;
    if (fs->gdt) kfree(fs->gdt);
    kfree(fs);
}

bool ext2_stat(ext2_fs_t* fs, const char* path, ext2_stat_t* st) {
    if (!fs) return false;
    uint32_t ino = path_to_inode(fs, path);
    if (ino == 0) return false;
    ext2_inode_disk_t id;
    if (!read_inode(fs, ino, &id)) return false;
    if (st) {
        st->mode = id.mode;
        st->uid = id.uid;
        st->size = id.size_lo;
        st->atime = id.atime;
        st->ctime = id.ctime;
        st->mtime = id.mtime;
        st->dtime = id.dtime;
        st->gid = id.gid;
        st->links_count = id.links_count;
        st->blocks = id.blocks;
        st->flags = id.flags;
    }
    return true;
}

bool ext2_is_dir(ext2_fs_t* fs, const char* path) {
    ext2_stat_t st; if (!ext2_stat(fs, path, &st)) return false;
    return (st.mode & 0xF000) == 0x4000;
}
bool ext2_is_file(ext2_fs_t* fs, const char* path) {
    ext2_stat_t st; if (!ext2_stat(fs, path, &st)) return false;
    return (st.mode & 0xF000) == 0x8000;
}

void* ext2_read_entire_file(ext2_fs_t* fs, const char* path, size_t* out_size) {
    if (!fs) return 0;
    uint32_t ino = path_to_inode(fs, path);
    if (ino == 0) return 0;
    ext2_inode_disk_t id;
    if (!read_inode(fs, ino, &id)) return 0;
    if (!((id.mode & 0xF000) == 0x8000)) return 0; // not regular

    size_t size = id.size_lo;
    void* buf = kmalloc(size ? size : 1);
    if (!buf) return 0;
    if (size) {
        if (!read_file(fs, &id, 0, (uint32_t)size, buf)) { kfree(buf); return 0; }
    }
    if (out_size) *out_size = size;
    return buf;
}

bool ext2_listdir(ext2_fs_t* fs, const char* path, void (*cb)(const ext2_dirent_t*, void*), void* user) {
    if (!fs) return false;
    uint32_t ino = path_to_inode(fs, path);
    if (ino == 0) return false;

    ext2_inode_disk_t id;
    if (!read_inode(fs, ino, &id)) return false;
    if (!((id.mode & 0xF000) == 0x4000)) return false; // dir

    uint32_t size = id.size_lo;
    uint32_t pos = 0;
    while (pos < size) {
        uint8_t* buf = (uint8_t*)kmalloc(fs->block_size);
        if (!buf) return false;
        uint32_t fb = pos / fs->block_size;
        uint32_t blk = get_block_from_inode(fs, &id, fb);
        if (blk == 0) { kfree(buf); return false; }
        if (!read_block(fs, blk, buf)) { kfree(buf); return false; }

        uint32_t inner = pos % fs->block_size;
        while (inner < fs->block_size && pos < size) {
            ext2_dirent_disk_t* de = (ext2_dirent_disk_t*)(buf + inner);
            if (de->inode && de->name_len) {
                ext2_dirent_t e; e.ino = de->inode; e.file_type = de->file_type;
                // Some ext2 volumes don't populate file_type in dirents unless the
                // FEATURE_INCOMPAT_FILETYPE flag is set. Fall back to inspecting
                // the inode's mode bits so callers (e.g., ls) can still tell
                // directories from regular files for coloring.
                if (e.file_type == 0) {
                    ext2_inode_disk_t dir_inode;
                    if (read_inode(fs, e.ino, &dir_inode)) {
                        uint16_t mode_hi = dir_inode.mode;
                        if ((mode_hi & 0xF000) == 0x4000) {
                            e.file_type = 2; // directory
                        } else {
                            e.file_type = 1; // treat everything else as a file
                        }
                    }
                }

                uint32_t nl = de->name_len; if (nl >= sizeof(e.name)) nl = sizeof(e.name)-1;
                memcpy(e.name, de->name, nl); e.name[nl] = 0;
                if (cb) cb(&e, user);
            }
            if (de->rec_len == 0) break;
            pos += de->rec_len;
            inner += de->rec_len;
        }
        kfree(buf);
    }
    return true;
}

const char* ext2_get_cwd(void) { return g_cwd; }



bool ext2_chdir(ext2_fs_t* fs, const char* path) {
    if (!fs || !path) return false;

    // Normalize into absolute path first (pure lexical '.'/'..' handling)
    char norm[512];
    size_t n = 0;
    const char* p = path;

    // Seed with cwd if relative
    if (*p != '/') {
        size_t l = strlen(g_cwd);
        if (l >= sizeof(norm)) l = sizeof(norm) - 1;
        memcpy(norm, g_cwd, l);
        n = l;
    } else {
        norm[n++] = '/';
        while (*p == '/') ++p;
    }

    while (*p) {
        while (*p == '/') ++p;
        // extract component
        char comp[256]; size_t k = 0;
        while (p[k] && p[k] != '/' && k < sizeof(comp) - 1) { comp[k] = p[k]; k++; }
        comp[k] = 0;
        p += k;

        if (k == 0) break;
        if (k == 1 && comp[0] == '.') {
            // no-op
        } else if (k == 2 && comp[0] == '.' && comp[1] == '.') {
            // back up one component
            if (n > 1) {
                if (norm[n-1] == '/' && n > 1) n--;
                while (n > 1 && norm[n-1] != '/') n--;
            }
        } else {
            if (!(n == 1 && norm[0] == '/')) norm[n++] = '/';
            for (size_t i = 0; i < k && n < sizeof(norm) - 1; i++) norm[n++] = comp[i];
        }
        while (*p == '/') ++p;
    }
    if (n == 0) norm[n++] = '/';
    norm[n] = 0;
    // remove trailing slashes (except root)
    while (n > 1 && norm[n-1] == '/') norm[--n] = 0;

    // Resolve inode of the normalized absolute path
    uint32_t ino = path_to_inode(fs, norm);
    if (ino == 0) return false;
    ext2_inode_disk_t ino2; if (!read_inode(fs, ino, &ino2)) return false;
    if ((ino2.mode & 0xF000) != 0x4000) return false;

    // Commit new cwd
    size_t L = n; if (L >= sizeof(g_cwd)) L = sizeof(g_cwd) - 1;
    memcpy(g_cwd, norm, L);
    g_cwd[L] = 0;
    return true;
}


// --- tiny write helpers
// --- tiny write helpers (mirror your read_* helpers) ---
static bool write_bytes(block_device_t* dev, uint64_t offset, uint32_t n, const void* src) {
    if (!dev || !dev->write) return false;
    uint64_t first_lba = offset / SECTOR_SIZE;
    uint32_t ofs_in_first = (uint32_t)(offset % SECTOR_SIZE);

    const uint8_t* s = (const uint8_t*)src;
    uint32_t remaining = n;

    uint8_t tmp[SECTOR_SIZE];

    // Unaligned first sector?
    if (ofs_in_first) {
        if (!block_read(dev, first_lba, 1, tmp)) return false;
        uint32_t tocopy = SECTOR_SIZE - ofs_in_first;
        if (tocopy > remaining) tocopy = remaining;
        memcpy(tmp + ofs_in_first, s, tocopy);
        if (!block_write(dev, first_lba, 1, tmp)) return false;
        s += tocopy; remaining -= tocopy; first_lba++;
    }

    // Whole sectors
    if (remaining >= SECTOR_SIZE) {
        uint32_t secs = remaining / SECTOR_SIZE;
        if (!block_write(dev, first_lba, secs, s)) return false;
        first_lba += secs;
        s += secs * SECTOR_SIZE;
        remaining -= secs * SECTOR_SIZE;
    }

    // Tail
    if (remaining) {
        if (!block_read(dev, first_lba, 1, tmp)) return false;
        memcpy(tmp, s, remaining);
        if (!block_write(dev, first_lba, 1, tmp)) return false;
    }
    return true;
}

static bool write_block(ext2_fs_t* fs, uint32_t blk, const void* src) {
    uint64_t off = (uint64_t)blk * fs->block_size;
    return write_bytes(fs->dev, off, fs->block_size, src);
}

bool ext2_append(ext2_fs_t* fs, const char* path, const void* data, uint32_t len) {
    if (!fs || !path || !*path || !data || !len) return false;

    uint32_t ino_nr = path_to_inode(fs, path);

    // If the target does not exist yet, create an empty regular file first so
    // that shell commands like "append" work on new paths as expected.
    if (ino_nr == 0) {
        if (!ext2_create_empty(fs, path, 0644)) return false;
        ino_nr = path_to_inode(fs, path);
        if (ino_nr == 0) return false;
    }

    ext2_inode_disk_t ino;
    if (!read_inode(fs, ino_nr, &ino)) return false;
    if ((ino.mode & 0xF000) != 0x8000) return false; // only regular for now

    uint32_t pos = ino.size_lo;   // append
    const uint8_t* src = (const uint8_t*)data;
    uint32_t remaining = len;

    while (remaining) {
        uint32_t block_off = pos % fs->block_size;
        uint32_t fb = pos / fs->block_size;

        uint32_t blk = get_block_from_inode(fs, &ino, fb);
        if (blk == 0) {
            // need a new block
            if (!append_block_to_inode(fs, &ino, &blk)) return false;
            // persist inodeâ€™s new block[] (so get_block_from_inode sees it on next loop)
            if (!write_inode(fs, ino_nr, &ino)) return false;
        }

        uint8_t* buf = (uint8_t*)kmalloc(fs->block_size);
        if (!buf) return false;
        if (!read_block(fs, blk, buf)) { kfree(buf); return false; }

        uint32_t room = fs->block_size - block_off;
        uint32_t tocopy = (remaining < room) ? remaining : room;
        memcpy(buf + block_off, src, tocopy);

        if (!write_block(fs, blk, buf)) { kfree(buf); return false; }
        kfree(buf);

        src += tocopy;
        remaining -= tocopy;
        pos += tocopy;

        // Grow size if we extended
        if (pos > ino.size_lo) ino.size_lo = pos;
    }

        uint32_t nowt = now_seconds();
    ino.mtime = nowt; ino.ctime = nowt;

    // Write final inode back
    if (!write_inode(fs, ino_nr, &ino)) return false;
    return true;
}


bool ext2_truncate(ext2_fs_t* fs, const char* path, uint32_t new_size) {
    if (!fs || !path) return false;
    uint32_t ino_nr = path_to_inode(fs, path);
    if (ino_nr == 0) {
        // Create the file if it doesn't exist so truncate behaves like POSIX.
        if (!ext2_create_empty(fs, path, 0644)) return false;
        ino_nr = path_to_inode(fs, path);
        if (ino_nr == 0) return false;
    }

    ext2_inode_disk_t ino;
    if (!read_inode(fs, ino_nr, &ino)) return false;

    // regular file check
    if ((ino.mode & 0xF000) != 0x8000) return false;

    uint32_t old_size = ino.size_lo;
    if (new_size == old_size) return true;

    if (new_size < old_size) {
        uint32_t block_size = fs->block_size;
        uint32_t off = new_size % block_size;
        if (off) {
            uint32_t fb = new_size / block_size;
            uint32_t blk = get_block_from_inode(fs, &ino, fb);
            if (blk) {
                uint8_t* buf = (uint8_t*)kmalloc(block_size);
                if (!buf) return false;
                if (!read_block(fs, blk, buf)) { kfree(buf); return false; }
                for (uint32_t i = off; i < block_size; i++) buf[i] = 0;
                bool ok = write_block(fs, blk, buf);
                kfree(buf);
                if (!ok) return false;
            }
        }
        // Free full blocks beyond new end
        uint32_t old_blocks = (old_size + block_size - 1) / block_size;
        uint32_t new_blocks = (new_size + block_size - 1) / block_size;
        if (old_blocks > new_blocks) {
            uint32_t per = block_size / 4;
            for (uint32_t fb = old_blocks; fb-- > new_blocks; ) {
                if (fb < 12) {
                    if (ino.block[fb]) { free_block_in_group(fs, ino.block[fb]); ino.block[fb]=0; }
                } else if (fb < 12 + per) {
                    if (ino.block[12]) {
                        uint32_t* l1=(uint32_t*)kmalloc(block_size); if(!l1) return false;
                        if(!read_block(fs,ino.block[12],l1)){kfree(l1);return false;}
                        uint32_t i = fb - 12;
                        if (l1[i]) { free_block_in_group(fs,l1[i]); l1[i]=0; write_block(fs,ino.block[12],l1); }
                        bool any=false; for(uint32_t t=0;t<per;t++) if(l1[t]){any=true;break;}
                        kfree(l1); if(!any){ free_block_in_group(fs,ino.block[12]); ino.block[12]=0; }
                    }
                } else {
                    uint32_t rem = fb - 12 - per;
                    uint32_t i = rem / per, j = rem % per;
                    if (ino.block[13]) {
                        uint32_t* l2=(uint32_t*)kmalloc(block_size); if(!l2) return false;
                        if(!read_block(fs,ino.block[13],l2)){kfree(l2);return false;}
                        if (l2[i]) {
                            uint32_t* l1=(uint32_t*)kmalloc(block_size); if(!l1){kfree(l2);return false;}
                            if(!read_block(fs,l2[i],l1)){kfree(l1);kfree(l2);return false;}
                            if (l1[j]) { free_block_in_group(fs,l1[j]); l1[j]=0; write_block(fs,l2[i],l1); }
                            bool any=false; for(uint32_t t=0;t<per;t++) if(l1[t]){any=true;break;}
                            kfree(l1); if(!any){ free_block_in_group(fs,l2[i]); l2[i]=0; write_block(fs,ino.block[13],l2); }
                        }
                        kfree(l2);
                    }
                }
            }
            if (ino.block[13]) {
                uint32_t* l2=(uint32_t*)kmalloc(block_size); if(!l2) return false;
                if(!read_block(fs,ino.block[13],l2)){kfree(l2);return false;}
                bool any=false; for(uint32_t t=0;t<block_size/4;t++) if(l2[t]){any=true;break;}
                kfree(l2); if(!any){ free_block_in_group(fs,ino.block[13]); ino.block[13]=0; }
            }
        }
    }

    ino.size_lo = new_size;
    uint32_t now = now_seconds(); ino.mtime = now; ino.ctime = now;
    return write_inode(fs, ino_nr, &ino);
}


bool ext2_replace(ext2_fs_t* fs, const char* path, const void* data, uint32_t len) {
    if (!ext2_truncate(fs, path, 0)) return false;
    return (len == 0) ? true : ext2_append(fs, path, data, len);
}

// --- minimal helpers used by create ---
static inline uint32_t inode_size_bytes(ext2_fs_t* fs) {
    // ext2 rev>=1 stores inode size; default 128 otherwise
    return (fs->sb.rev_level >= 1 && fs->sb.inode_size) ? fs->sb.inode_size : 128;
}

// ceil-div for bits in N blocks of bitmap
static inline uint32_t bitmap_blocks_for_inodes(ext2_fs_t* fs) {
    uint32_t bits_per_block = fs->block_size * 8u;
    return (fs->sb.inodes_per_group + bits_per_block - 1) / bits_per_block;
}

static inline uint16_t rec_len_min(uint8_t name_len) {
    // ext2 dirent min size is 8 + name_len, rounded to 4
    uint16_t s = (uint16_t)(8 + name_len);
    return (s + 3) & ~3u;
}

// Split "path" into parent + leaf using cwd semantics of your FS
// Returns true on success, filling parent_out and name_out.
// parent_out/name_out buffers must be provided by caller.
// NOTE: normalizes "." and "..".
static bool split_parent_leaf(ext2_fs_t* fs, const char* path, char* parent_out, size_t parent_cap,
                              char* name_out, size_t name_cap) {
    (void)fs;
    
    if (!path || !*path) return false;

    char full[512];
    // Normalize '.' and '..' and build absolute path
    char norm[512]; size_t n=0; const char* pcur = path;
    if (*pcur=='/') { norm[n++]='/'; while (*pcur=='/') ++pcur; }
    else {
        // start with cwd
        size_t l = strlen(g_cwd); if (l >= sizeof(norm)) l = sizeof(norm)-1;
        memcpy(norm, g_cwd, l); n = l;
    }
    while (*pcur) {
        while (*pcur=='/') ++pcur;
        char comp[256]; size_t k=0;
        while (pcur[k] && pcur[k]!='/' && k<sizeof(comp)-1){ comp[k]=pcur[k]; k++; }
        comp[k]=0; pcur+=k;
        if (k==0) break;
        if (k==1 && comp[0]=='.') { /*skip*/ }
        else if (k==2 && comp[0]=='.' && comp[1]=='.') {
            if (n>1) { if (norm[n-1]=='/'&&n>1)n--; while(n>1 && norm[n-1]!='/') n--; }
        } else {
            if (!(n==1 && norm[0]=='/')) norm[n++]='/';
            for (size_t i=0;i<k && n<sizeof(norm)-1;i++) norm[n++]=comp[i];
        }
        while (*pcur=='/') ++pcur;
    }
    if (n == 0) norm[n++] = '/';
    norm[n] = 0;
     size_t L = strlen(norm);
    if (L >= sizeof(full)) L = sizeof(full)-1;
    memcpy(full, norm, L); full[L]=0;

    // Find last '/'
    const char* slash = 0;
    for (size_t i=0;i<L;i++) if (full[i]=='/') slash = &full[i];

    if (!slash || (slash==full && full[1]==0)) {
        // parent is "/", name is full (without leading '/')
        parent_out[0] = '/'; parent_out[1]=0;
        const char* nm = (full[0]=='/') ? full+1 : full;
        size_t nl = strlen(nm); if (nl >= name_cap) nl = name_cap-1;
        memcpy(name_out, nm, nl); name_out[nl]=0;
        return name_out[0] != 0;
    }

    // parent = [0..slash-1] (but keep at least "/")
    size_t plen = (size_t)(slash - full);
    if (plen==0) plen=1; // root
    if (plen >= parent_cap) plen = parent_cap-1;
    memcpy(parent_out, full, plen); parent_out[plen]=0;

    // name = after slash
    const char* nm = slash+1;
    size_t nl = strlen(nm); if (nl >= name_cap) nl = name_cap-1;
    memcpy(name_out, nm, nl); name_out[nl]=0;

    return name_out[0] != 0;
}

// --- main: create an empty regular file ---
bool ext2_create_empty(ext2_fs_t* fs, const char* path, uint16_t mode) {
    if (!fs || !path || !*path) return false;

    // If it already exists, succeed (touch semantics; no mtime update here)
    uint32_t existing = path_to_inode(fs, path);
    if (existing != 0) {
        ext2_inode_disk_t ex; if (read_inode(fs, existing, &ex)) { uint32_t now = now_seconds(); ex.mtime = now; ex.ctime = now; write_inode(fs, existing, &ex); }
        return true;
    }

    // Resolve parent + name
    char parent[512], name[256];
    if (!split_parent_leaf(fs, path, parent, sizeof(parent), name, sizeof(name))) return false;

    uint32_t parent_ino = path_to_inode(fs, parent);
    if (parent_ino == 0) return false;

    // Load parent inode and sanity check it's a directory
    ext2_inode_disk_t pino;
    if (!read_inode(fs, parent_ino, &pino)) return false;
    if (((pino.mode & 0xF000) != 0x4000)) return false; // not a dir

    // 1) Find a free inode in some group
    uint32_t new_ino = 0;
    uint32_t group_idx = 0;
    for (uint32_t g = 0; g < fs->groups; ++g) {
        if (fs->gdt[g].free_inodes_count == 0) continue;

        uint32_t blocks = bitmap_blocks_for_inodes(fs);
        uint32_t found_global_index = UINT32_MAX;

        for (uint32_t bi = 0; bi < blocks; ++bi) {
            uint32_t bmp_block = fs->gdt[g].inode_bitmap + bi;
            uint8_t* bmp = (uint8_t*)kmalloc(fs->block_size);
            if (!bmp) return false;
            if (!read_block(fs, bmp_block, bmp)) { kfree(bmp); return false; }

            uint32_t bits_in_this = fs->block_size * 8u;
            uint32_t base = bi * bits_in_this;
            uint32_t limit = fs->sb.inodes_per_group - base;
            if ((int32_t)limit <= 0) { kfree(bmp); break; }
            if (limit > bits_in_this) limit = bits_in_this;

            bool done = false;
            for (uint32_t i = 0; i < limit; ++i) {
                uint32_t byte = i >> 3;
                uint8_t  mask = (uint8_t)(1u << (i & 7));
                if ((bmp[byte] & mask) == 0) {
                    // free inode -> mark used
                    bmp[byte] |= mask;
                    if (!write_block(fs, bmp_block, bmp)) { kfree(bmp); return false; }
                    found_global_index = base + i; // 0-based within group
                    done = true;
                    break;
                }
            }
            kfree(bmp);
            if (done) {
                new_ino = g * fs->sb.inodes_per_group + found_global_index + 1; // global ino
                group_idx = g;
                goto have_ino;
            }
        }
    }
    // no inode available
    return false;

have_ino: ;
    // 2) Initialize the inode on disk
    uint32_t isz = inode_size_bytes(fs);
    uint32_t inodes_per_block = fs->block_size / isz;
uint32_t index_within_group = (new_ino - 1) % fs->sb.inodes_per_group;
    uint32_t table_block_ofs = index_within_group / inodes_per_block;
    uint32_t table_slot_ofs  = index_within_group % inodes_per_block;

    uint32_t table_block = fs->gdt[group_idx].inode_table + table_block_ofs;
    uint8_t* ibuf = (uint8_t*)kmalloc(fs->block_size);
    if (!ibuf) return false;
    if (!read_block(fs, table_block, ibuf)) { kfree(ibuf); return false; }

    ext2_inode_disk_t ino_init;
    memset(&ino_init, 0, sizeof(ino_init));
    ino_init.mode = (uint16_t)(0x8000 | (mode & 0x0FFF)); // regular file + perms
    ino_init.uid = 0;
    ino_init.size_lo = 0;
    uint32_t now = now_seconds();
    ino_init.atime = now; ino_init.ctime = now; ino_init.mtime = now;
    ino_init.dtime = 0;
    ino_init.gid = 0;
    ino_init.links_count = 1;
    ino_init.blocks = 0;
    ino_init.flags = 0;
    // block[] already zeroed (no data blocks)

    // Write inode into its slot
    memcpy(ibuf + table_slot_ofs * isz, &ino_init, sizeof(ino_init));
    if (!write_block(fs, table_block, ibuf)) { kfree(ibuf); return false; }
    kfree(ibuf);

    // 3) Insert a directory entry into the parent dir (use slack in an existing block)
    uint8_t name_len = (uint8_t)strlen(name);
    uint16_t need = rec_len_min(name_len);

    uint32_t psize = pino.size_lo;
    uint32_t pos = 0;
    bool inserted = false;

    while (pos < psize && !inserted) {
        uint8_t* db = (uint8_t*)kmalloc(fs->block_size);
        if (!db) return false;

        uint32_t fb = pos / fs->block_size;
        uint32_t blk = 0;
        // Reuse your existing helper to fetch data block number for a file block index
        // get_block_from_inode(fs, &pino, fb) is already present in the file.
        blk = get_block_from_inode(fs, &pino, fb);
        if (blk == 0) { kfree(db); return false; }
        if (!read_block(fs, blk, db)) { kfree(db); return false; }

        uint32_t inner = 0;
        uint32_t last_off = 0;
        ext2_dirent_disk_t* last = NULL;

        // Walk dirents to find the last one in this block
        while (inner < fs->block_size) {
            ext2_dirent_disk_t* de = (ext2_dirent_disk_t*)(db + inner);
            if (de->rec_len == 0) break;
            last = de; last_off = inner;
            inner += de->rec_len;
        }

        if (last) {
            // Compute minimal size of the last entry and check for slack
            uint16_t last_min = rec_len_min(last->name_len);
            uint16_t slack = (uint16_t)(last->rec_len - last_min);
            if (slack >= need) {
                // Shrink last to min and append new entry
                last->rec_len = last_min;

                ext2_dirent_disk_t* nde = (ext2_dirent_disk_t*)(db + last_off + last_min);
                nde->inode = new_ino;
                nde->name_len = name_len;
                nde->file_type = 1; // regular file
                nde->rec_len = slack;
                memcpy(nde->name, name, name_len);
                // zero pad the name tail (not strictly necessary)
                uint32_t pad = nde->rec_len - (8 + name_len);
                if (pad && (8 + name_len) < nde->rec_len) {
                    memset(((uint8_t*)nde) + 8 + name_len, 0, pad);
                }

                if (!write_block(fs, blk, db)) { kfree(db); return false; }
                inserted = true;
            }
        }
        kfree(db);
        pos += fs->block_size;
    }

    if (!inserted) {
        // Roll back the inode-bit we set

uint32_t index_within_group = (new_ino - 1) % fs->sb.inodes_per_group;
        uint32_t bits = fs->block_size * 8u;
        uint32_t bi = index_within_group / bits;
        uint32_t bit = index_within_group % bits;
        uint32_t bmp_block = fs->gdt[group_idx].inode_bitmap + bi;
        uint8_t* bmp=(uint8_t*)kmalloc(fs->block_size);
        if (bmp && read_block(fs, bmp_block, bmp)) { bmp[bit>>3] &= (uint8_t)~(1u << (bit & 7)); write_block(fs, bmp_block, bmp); }
        if (bmp) kfree(bmp);
        return false;
    }

    // 4) Update counters and parent dir times
    if (fs->sb.free_inodes_count) fs->sb.free_inodes_count--;
    if (fs->gdt[group_idx].free_inodes_count) fs->gdt[group_idx].free_inodes_count--;
    pino.mtime = now_seconds(); pino.ctime = pino.mtime; write_inode(fs, parent_ino, &pino);

    // Write superblock (at byte offset 1024)
    if (!write_bytes(fs->dev, 1024, sizeof(fs->sb), &fs->sb)) return false;

    // Write back the whole GDT region (same way you read it in ext2_mount)
    uint32_t gdt_start_block = (fs->block_size == 1024) ? 2 : 1;
    uint32_t gdt_bytes = fs->groups * sizeof(ext2_group_desc_t);
    uint32_t gdt_blocks = (gdt_bytes + fs->block_size - 1) / fs->block_size;
    for (uint32_t i = 0; i < gdt_blocks; ++i) {
        if (!write_block(fs, gdt_start_block + i, (uint8_t*)fs->gdt + i*fs->block_size)) return false;
    }

    return true;
}

static bool zero_block(ext2_fs_t* fs, uint32_t blk) {
    uint8_t* z = (uint8_t*)kmalloc(fs->block_size);
    if (!z) return false;
    memset(z, 0, fs->block_size);
    bool ok = write_block(fs, blk, z);
    kfree(z);
    return ok;
}

// Scan a block bitmap in group g for a free block, mark it used, update counters.
static bool alloc_block_in_group(ext2_fs_t* fs, uint32_t g, uint32_t* out_blk) {
    if (fs->gdt[g].free_blocks_count == 0) return false;

    uint32_t bits = fs->block_size * 8u;
    uint32_t blocks_in_group = fs->sb.blocks_per_group;

    // Number of bitmap blocks needed to cover the group's blocks
    uint32_t bmp_blocks = (blocks_in_group + bits - 1) / bits;

    for (uint32_t bi = 0; bi < bmp_blocks; ++bi) {
        uint32_t bmp_block = fs->gdt[g].block_bitmap + bi;
        uint8_t* bmp = (uint8_t*)kmalloc(fs->block_size);
        if (!bmp) return false;
        if (!read_block(fs, bmp_block, bmp)) { kfree(bmp); return false; }

        uint32_t base = bi * bits;
        uint32_t limit = blocks_in_group - base;
        if ((int32_t)limit <= 0) { kfree(bmp); break; }
        if (limit > bits) limit = bits;

        for (uint32_t i = 0; i < limit; ++i) {
            uint32_t byte = i >> 3;
            uint8_t mask  = (uint8_t)(1u << (i & 7));
            if ((bmp[byte] & mask) == 0) {
                // Mark used
                bmp[byte] |= mask;
                if (!write_block(fs, bmp_block, bmp)) { kfree(bmp); return false; }

                uint32_t global_block =
                    g * fs->sb.blocks_per_group + base + i;
                // ext2 counts blocks starting at sb.first_data_block; your code
                // already treats on-disk "block numbers" as absolute indexes used
                // by read_block/write_block, so use this directly.
                *out_blk = global_block;

                // Update counters in memory
                if (fs->sb.free_blocks_count) fs->sb.free_blocks_count--;
                if (fs->gdt[g].free_blocks_count) fs->gdt[g].free_blocks_count--;

                kfree(bmp);
                // Zero new data block to be safe
                if (!zero_block(fs, *out_blk)) return false;

                // Persist SB + GDT (same way as ext2_create_empty)
                if (!write_bytes(fs->dev, 1024, sizeof(fs->sb), &fs->sb)) return false;
                uint32_t gdt_start_block = (fs->block_size == 1024) ? 2 : 1;
                uint32_t gdt_bytes = fs->groups * sizeof(ext2_group_desc_t);
                uint32_t gdt_blocks = (gdt_bytes + fs->block_size - 1) / fs->block_size;
                for (uint32_t ii = 0; ii < gdt_blocks; ++ii) {
                    if (!write_block(fs, gdt_start_block + ii,
                                     (uint8_t*)fs->gdt + ii*fs->block_size)) return false;
                }
                return true;
            }
        }
        kfree(bmp);
    }
    return false;
}

// Allocate a data block anywhere (first group with free blocks).
static bool alloc_block(ext2_fs_t* fs, uint32_t* out_blk) {
    for (uint32_t g = 0; g < fs->groups; ++g) {
        if (alloc_block_in_group(fs, g, out_blk)) return true;
    }
    return false;
}

// Free a data block and update SB/GDT/bitmap (mirror of allocation path)
static bool free_block_in_group(ext2_fs_t* fs, uint32_t blk) {
    if (blk < fs->sb.first_data_block) return false;
    uint32_t rel = blk - fs->sb.first_data_block;
    uint32_t g = rel / fs->sb.blocks_per_group;
    uint32_t idx = rel % fs->sb.blocks_per_group;
    if (g >= fs->groups) return false;

    uint32_t bits_per_block = fs->block_size * 8u;
    uint32_t bmp_block = fs->gdt[g].block_bitmap + (idx / bits_per_block);
    uint32_t bit_in_block = idx % bits_per_block;
    uint32_t byte = bit_in_block >> 3;
    uint8_t mask = (uint8_t)(1u << (bit_in_block & 7));

    uint8_t* bmp = (uint8_t*)kmalloc(fs->block_size);
    if (!bmp) return false;
    if (!read_block(fs, bmp_block, bmp)) { kfree(bmp); return false; }

    if (!(bmp[byte] & mask)) { kfree(bmp); return false; } // already free?
    bmp[byte] &= (uint8_t)~mask;
    if (!write_block(fs, bmp_block, bmp)) { kfree(bmp); return false; }
    kfree(bmp);

    // Update counters and flush SB/GDT
    if (fs->sb.free_blocks_count) fs->sb.free_blocks_count++;
    if (fs->gdt[g].free_blocks_count) fs->gdt[g].free_blocks_count++;

    if (!write_bytes(fs->dev, 1024, sizeof(fs->sb), &fs->sb)) return false;

    uint32_t gdt_start = (fs->block_size == 1024) ? 2 : 1;
    uint32_t gdt_bytes = fs->groups * sizeof(ext2_group_desc_t);
    uint32_t gdt_blocks = (gdt_bytes + fs->block_size - 1) / fs->block_size;
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        if (!write_block(fs, gdt_start + i, (uint8_t*)fs->gdt + i * fs->block_size)) return false;
    }
    return true;
}