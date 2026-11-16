/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    fs/ext2.c
 * Version: 0.2beta
 * Brief:   ext2 只读实现：超级块 -> 块组描述符 -> inode 表 -> inode -> 目录项
 *          -> 数据块（直接块 / 一级间接 / 二级间接）。读 ATA 主通道「主设备」
 *          （QEMU -drive if=ide,index=0）。
 *
 * M19，纯增量「只添加，不修改」：
 *   - 只读：不写盘、不建目录、不删文件（写 ext2 是 M20 的事）。
 *   - 只解析 rev0/rev1 的静态 inode，不支持 extent（那是 ext4 的东西）。
 *   - 复用 drivers/ata 的 ata_read_sector（B2 的 PIO 接口一行未改）。
 *   - 不触碰 ramfs / FAT16，ext2ls / ext2cat 与 ls / cat / fatls 完全隔离。
 *
 * freestanding：不能调 memcpy/memset，所有拷贝都是本文件里的小循环。
 * ============================================================================ */

#include "ext2.h"

#include <stdbool.h>

#include "../drivers/ata.h"   /* ata_read_sector */
#include "../drivers/vga.h"   /* vga_print / vga_putchar */

/* ------------------------------------------------------------------ */
/* 常量：ext2 的字段偏移全部来自 Linux 的 struct ext2_super_block 等  */
/* ------------------------------------------------------------------ */

#define EXT2_SUPER_MAGIC      0xEF53u
#define EXT2_SECTOR_SIZE      512u
#define EXT2_ROOT_INO         2u
#define EXT2_INODE_TABLE_MAX  4096u

/* 超级块字段偏移（相对超级块起点，超级块在绝对偏移 1024） */
#define SB_INODES_COUNT       0x00u
#define SB_BLOCKS_COUNT       0x04u
#define SB_FIRST_DATA_BLOCK   0x14u
#define SB_LOG_BLOCK_SIZE     0x18u
#define SB_BLOCKS_PER_GROUP   0x20u
#define SB_INODES_PER_GROUP   0x28u
#define SB_MAGIC              0x38u
#define SB_REV_LEVEL          0x4Cu
#define SB_FIRST_INO          0x54u
#define SB_INODE_SIZE         0x58u
#define SB_FEATURE_INCOMPAT   0x60u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u

/* inode 字段偏移 */
#define IN_MODE               0x00u
#define IN_SIZE_LO            0x04u
#define IN_SIZE_HI            0x6Cu   /* rev>0 时这里才是 size 的高 32 位 */
#define IN_BLOCKS             0x28u   /* i_block[15]，15 个 u32 */

#define MODE_FMT_MASK         0xF000u
#define MODE_FMT_DIR          0x4000u
#define MODE_FMT_REG          0x8000u

/* ------------------------------------------------------------------ */
/* 挂载后缓存的几何信息                                               */
/* ------------------------------------------------------------------ */

static int      g_mounted = 0;
static uint32_t g_block_size = 0;
static uint32_t g_blocks_count = 0;
static uint32_t g_inodes_count = 0;
static uint32_t g_first_data_block = 0;
static uint32_t g_blocks_per_group = 0;
static uint32_t g_inodes_per_group = 0;
static uint32_t g_inode_size = 0;
static uint32_t g_first_ino = 0;
static uint16_t g_rev_level = 0;
static uint32_t g_gdt_block = 0;      /* 块组描述符表第一个块 */
static uint32_t g_inode_table = 0;    /* 第 0 组 inode 表第一个块 */
static uint32_t g_name_off = 8u;      /* 目录项里 name 的偏移：有 filetype 是 8，没有是 7 */
static int      g_have_filetype = 1;  /* EXT2_FEATURE_INCOMPAT_FILETYPE */

/* 一个块的暂存。ext2 的块最大 4096 字节（s_log_block_size<=2）。 */
static uint8_t  g_block_buf[EXT2_INODE_TABLE_MAX];
static uint8_t  g_block_buf2[EXT2_INODE_TABLE_MAX];
static uint8_t  g_ptr_buf[EXT2_INODE_TABLE_MAX];   /* 间接块的"指针数组"专用 */
static uint8_t  g_dir_buf[EXT2_INODE_TABLE_MAX];   /* ext2ls 遍历目录时的专用块缓冲 */
static uint8_t  g_sb_buf[1024];

/* 解析出来的 inode（只保留本实现用得到的字段） */
typedef struct {
    uint32_t ino;
    uint16_t mode;
    uint32_t size;
    uint32_t block[15];
} inode_t;

/* ------------------------------------------------------------------ */
/* 小端读取                                                           */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* freestanding 的 memcpy：从 src 拷 n 字节到 dst */
static void copy_bytes(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        d[i] = s[i];
    }
}

/* ------------------------------------------------------------------ */
/* 块读取：一个 ext2 块 = block_size/512 个扇区                       */
/* ------------------------------------------------------------------ */

static int read_block_to(uint32_t blk, uint8_t *buf)
{
    uint32_t spb = g_block_size / EXT2_SECTOR_SIZE;
    uint32_t i;
    int rc;

    if (spb == 0u) {
        return EXT2_ERR_SB;
    }
    for (i = 0u; i < spb; ++i) {
        /* 不管块多大，都是一扇区一扇区地要：ATA PIO 一次最多 256 个扇区，
         * 但这里故意拆到最小粒度，读失败时能指出是哪一扇区。 */
        rc = ata_read_sector(blk * spb + i, buf + i * EXT2_SECTOR_SIZE);
        if (rc != ATA_OK) {
            return EXT2_ERR_IO;
        }
    }
    return EXT2_OK;
}

/* ------------------------------------------------------------------ */
/* inode 读取                                                         */
/* ------------------------------------------------------------------ */

/* 读一个 inode：组号 = (ino-1)/inodes_per_group，组内序号 = (ino-1)%...，
 * inode 表里第 index 项，偏移 index*inode_size —— 这是 ext2 的定位公式，
 * 和 Linux ext2_iget() 里的 ext2_get_inode() 一模一样。 */
static int read_inode(uint32_t ino, inode_t *out)
{
    uint32_t group, index, desc_off, desc_sector, desc_in_block;
    uint32_t inode_off, blk, off_in_blk;
    const uint8_t *desc;
    const uint8_t *raw;
    uint16_t mode;
    uint32_t size;
    uint32_t i;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (ino == 0u || ino > g_inodes_count) {
        return EXT2_ERR_CORRUPT;
    }

    group = (ino - 1u) / g_inodes_per_group;
    index = (ino - 1u) % g_inodes_per_group;

    /* 1) 组描述符：块组描述符表里第 group 项，每项 32 字节 */
    desc_off = group * 32u;
    desc_sector = desc_off / g_block_size;
    desc_in_block = desc_off % g_block_size;
    if (read_block_to(g_gdt_block + desc_sector, g_block_buf2) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    desc = g_block_buf2 + desc_in_block;
    /* bg_inode_table 在组描述符的第 8 字节 */
    g_inode_table = rd32(desc + 8u);
    if (g_inode_table == 0u || g_inode_table >= g_blocks_count) {
        return EXT2_ERR_CORRUPT;
    }

    /* 2) inode 在 inode 表里的位置 */
    inode_off = index * g_inode_size;
    blk = g_inode_table + inode_off / g_block_size;
    off_in_blk = inode_off % g_block_size;

    if (read_block_to(blk, g_block_buf2) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (off_in_blk + g_inode_size > g_block_size) {
        /* 跨块：先把本块尾部拷出来，再读下一块，从两块拼出这一个 inode */
        uint32_t first = g_block_size - off_in_blk;
        copy_bytes(g_block_buf, g_block_buf2 + off_in_blk, first);
        if (read_block_to(blk + 1u, g_block_buf2) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        copy_bytes(g_block_buf + first, g_block_buf2,
                   g_inode_size - first);
        raw = g_block_buf;
    } else {
        raw = g_block_buf2 + off_in_blk;
    }

    mode = rd16(raw + IN_MODE);
    size = rd32(raw + IN_SIZE_LO);
    if (g_rev_level > 0u) {
        uint32_t hi = rd32(raw + IN_SIZE_HI);
        if (g_block_size == 1024u) {
            /* 4GB 以内够用；这里只做"高位非零就当作不支持"的判断 */
            if (hi != 0u) {
                return EXT2_ERR_CORRUPT;
            }
        }
    }

    out->ino = ino;
    out->mode = mode;
    out->size = size;
    for (i = 0u; i < 15u; ++i) {
        out->block[i] = rd32(raw + IN_BLOCKS + i * 4u);
    }
    return EXT2_OK;
}

/* ------------------------------------------------------------------ */
/* 路径解析                                                           */
/* ------------------------------------------------------------------ */

#define PATH_MAX_COMPS 6u
#define NAME_MAX_LEN   32u

typedef struct {
    char name[NAME_MAX_LEN];
    uint32_t ino;
    uint8_t type;      /* 1=普通文件 2=目录（file_type 不一定存在，按 inode 再判） */
} dentry_t;

/* 把 "/docs/note.txt" 拆成 {"docs","note.txt"} */
static int split_path(const char *path, char comps[PATH_MAX_COMPS][NAME_MAX_LEN],
                      uint32_t *count)
{
    uint32_t n = 0u;
    const char *p = path;

    if (path == (const char *)0) {
        return EXT2_ERR_NOENT;
    }
    while (*p == '/') {
        ++p;
    }
    if (*p == '\0') {
        *count = 0u;              /* "/" -> 根目录 */
        return EXT2_OK;
    }
    while (*p != '\0') {
        uint32_t k = 0u;
        while (*p != '\0' && *p != '/') {
            if (k + 1u >= NAME_MAX_LEN || n >= PATH_MAX_COMPS) {
                return EXT2_ERR_NOENT;
            }
            comps[n][k] = *p;
            ++k;
            ++p;
        }
        comps[n][k] = '\0';
        ++n;
        while (*p == '/') {
            ++p;
        }
    }
    *count = n;
    return EXT2_OK;
}

/* 在目录 inode 里按名字找一项（ext2 目录项：inode, rec_len, name_len, type,
 * name —— 每项长度是 rec_len，最后一项 rec_len 拉到块尾）。 */
static int dir_lookup(const inode_t *dir, const char *name, dentry_t *out)
{
    uint32_t b;
    uint32_t seen = 0u;

    if ((dir->mode & MODE_FMT_MASK) != MODE_FMT_DIR) {
        return EXT2_ERR_NOTDIR;
    }

    for (b = 0u; b < 12u; ++b) {
        uint32_t off = 0u;

        if (dir->block[b] == 0u) {
            break;
        }
        if (read_block_to(dir->block[b], g_block_buf) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        while (off + 8u <= g_block_size) {
            const uint8_t *e = g_block_buf + off;
            uint16_t rec_len = rd16(e + 4u);
            uint8_t name_len = e[6];
            uint32_t i;
            bool hit = true;

            if (rec_len == 0u || off + rec_len > g_block_size ||
                (uint32_t)name_len + g_name_off > (uint32_t)rec_len) {
                return EXT2_ERR_CORRUPT;
            }
            if (name_len > 0u) {
                for (i = 0u; i < (uint32_t)name_len; ++i) {
                    if (i >= NAME_MAX_LEN - 1u || name[i] != e[g_name_off + i]) {
                        hit = false;
                        break;
                    }
                }
                if (hit && name[name_len] != '\0') {
                    hit = false;      /* 名字更长的项，不算命中 */
                }
                if (hit) {
                    uint32_t k;
                    out->ino = rd32(e);
                    out->type = g_have_filetype ? e[7] : 0u;
                    for (k = 0u; k < (uint32_t)name_len && k < NAME_MAX_LEN - 1u; ++k) {
                        out->name[k] = e[g_name_off + k];
                    }
                    out->name[k] = '\0';
                    return EXT2_OK;
                }
            }
            ++seen;
            off += rec_len;
        }
    }
    return (seen == 0u) ? EXT2_ERR_CORRUPT : EXT2_ERR_NOENT;
}

/* 解析路径到 inode：从根 inode 2 开始逐段 dir_lookup() */
static int resolve(const char *path, inode_t *out)
{
    char comps[PATH_MAX_COMPS][NAME_MAX_LEN];
    uint32_t n = 0u;
    uint32_t i;
    inode_t cur;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (split_path(path, comps, &n) != EXT2_OK) {
        return EXT2_ERR_NOENT;
    }
    if (read_inode(EXT2_ROOT_INO, &cur) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    for (i = 0u; i < n; ++i) {
        dentry_t de;
        int rc = dir_lookup(&cur, comps[i], &de);

        if (rc != EXT2_OK) {
            return rc;
        }
        if (read_inode(de.ino, &cur) != EXT2_OK) {
            return EXT2_ERR_CORRUPT;
        }
    }
    *out = cur;
    return EXT2_OK;
}

/* ------------------------------------------------------------------ */
/* 挂载                                                               */
/* ------------------------------------------------------------------ */

int ext2_mount(void)
{
    uint32_t log_bs;
    uint32_t i;
    int rc;

    g_mounted = 0;

    if (ata_disk_present() == 0) {
        return EXT2_ERR_NODEV;
    }

    /* 超级块在绝对偏移 1024：读扇区 2 和 3 拼出前 1024 字节 */
    for (i = 0u; i < 2u; ++i) {
        rc = ata_read_sector(2u + i, g_sb_buf + i * EXT2_SECTOR_SIZE);
        if (rc != ATA_OK) {
            return EXT2_ERR_NODEV;
        }
    }
    if (rd16(g_sb_buf + SB_MAGIC) != EXT2_SUPER_MAGIC) {
        return EXT2_ERR_NOTEXT2;
    }

    log_bs = rd32(g_sb_buf + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) {
        return EXT2_ERR_SB;                   /* 只支持 1024/2048/4096 */
    }
    g_block_size = 1024u << log_bs;
    g_blocks_count = rd32(g_sb_buf + SB_BLOCKS_COUNT);
    g_inodes_count = rd32(g_sb_buf + SB_INODES_COUNT);
    g_first_data_block = rd32(g_sb_buf + SB_FIRST_DATA_BLOCK);
    g_blocks_per_group = rd32(g_sb_buf + SB_BLOCKS_PER_GROUP);
    g_inodes_per_group = rd32(g_sb_buf + SB_INODES_PER_GROUP);
    g_rev_level = rd16(g_sb_buf + SB_REV_LEVEL);
    {
        uint32_t feat = rd32(g_sb_buf + SB_FEATURE_INCOMPAT);
        g_have_filetype = ((feat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u) ? 1 : 0;
        g_name_off = g_have_filetype ? 8u : 7u;
    }
    g_first_ino = rd32(g_sb_buf + SB_FIRST_INO);
    if (g_rev_level == 0u) {
        g_inode_size = 128u;
    } else {
        g_inode_size = rd16(g_sb_buf + SB_INODE_SIZE);
        if (g_inode_size < 128u || g_inode_size > 1024u) {
            return EXT2_ERR_SB;
        }
    }
    if (g_block_size == 0u || g_blocks_count == 0u || g_inodes_count == 0u ||
        g_inodes_per_group == 0u) {
        return EXT2_ERR_SB;
    }

    /* 块组描述符表紧跟在第一个数据块之后（bs=1024 时 first_data_block=1） */
    g_gdt_block = g_first_data_block + 1u;
    if (read_block_to(g_gdt_block, g_block_buf2) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    g_inode_table = rd32(g_block_buf2 + 8u);
    if (g_inode_table == 0u || g_inode_table >= g_blocks_count) {
        return EXT2_ERR_SB;
    }

    /* 探一次根 inode：读不出来就当挂载失败，不要留一个半死不活的状态。
     * 注意 read_inode() 自己会检查 g_mounted，所以这里要先"临时挂上"。 */
    {
        inode_t root;
        g_mounted = 1;
        if (read_inode(EXT2_ROOT_INO, &root) != EXT2_OK) {
            g_mounted = 0;
            return EXT2_ERR_CORRUPT;
        }
    }

    g_mounted = 1;
    return EXT2_OK;
}

int ext2_mounted(void)
{
    return g_mounted;
}

int ext2_geometry(ext2_geom_t *out)
{
    if (g_mounted == 0 || out == (ext2_geom_t *)0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    out->block_size = g_block_size;
    out->blocks_count = g_blocks_count;
    out->inodes_count = g_inodes_count;
    out->blocks_per_group = g_blocks_per_group;
    out->inodes_per_group = g_inodes_per_group;
    out->inode_size = g_inode_size;
    out->first_ino = g_first_ino;
    out->gdt_block = g_gdt_block;
    out->inode_table = g_inode_table;
    out->magic = EXT2_SUPER_MAGIC;
    out->rev_level = g_rev_level;
    return EXT2_OK;
}

/* ------------------------------------------------------------------ */
/* ext2ls                                                             */
/* ------------------------------------------------------------------ */

int ext2_list(const char *path)
{
    inode_t dir;
    uint32_t b;
    uint32_t count = 0u;
    int rc;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    rc = resolve((path == (const char *)0) ? "/" : path, &dir);
    if (rc != EXT2_OK) {
        return rc;
    }
    if ((dir.mode & MODE_FMT_MASK) != MODE_FMT_DIR) {
        return EXT2_ERR_NOTDIR;
    }

    for (b = 0u; b < 12u; ++b) {
        uint32_t off = 0u;

        if (dir.block[b] == 0u) {
            break;
        }
        if (read_block_to(dir.block[b], g_dir_buf) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        while (off + 8u <= g_block_size) {
            const uint8_t *e = g_dir_buf + off;
            uint16_t rec_len = rd16(e + 4u);
            uint8_t name_len = e[6];
            uint32_t ino = rd32(e);
            uint32_t k;
            char name[NAME_MAX_LEN];

            if (rec_len == 0u || off + rec_len > g_block_size) {
                return EXT2_ERR_CORRUPT;
            }
            off += rec_len;
            if (ino == 0u || name_len == 0u) {
                continue;                       /* 空闲项 / 空洞 */
            }
            if (name_len > NAME_MAX_LEN - 1u) {
                continue;
            }
            if (name_len == 1u && e[g_name_off] == '.') {
                continue;                       /* Linux 也跳过 "." */
            }
            if (name_len == 2u && e[g_name_off] == '.' && e[g_name_off + 1] == '.') {
                continue;                       /* 以及 ".." */
            }
            for (k = 0u; k < (uint32_t)name_len; ++k) {
                name[k] = e[g_name_off + k];
            }
            name[name_len] = '\0';

            /* 目录亮蓝带 '/'，文件浅灰 —— 与 fatls 的视觉语言一致 */
            {
                inode_t child;
                const bool is_dir =
                    (g_have_filetype && e[7] == 2u) ||
                    (read_inode(ino, &child) == EXT2_OK &&
                     (child.mode & MODE_FMT_MASK) == MODE_FMT_DIR);

                if (is_dir) {
                    vga_print(name, VGA_LIGHT_BLUE, VGA_BLACK);
                    vga_putchar('/', VGA_LIGHT_BLUE, VGA_BLACK);
                } else {
                    vga_print(name, VGA_LIGHT_GREY, VGA_BLACK);
                }
            }
            ++count;
            if ((count % 4u) == 0u) {
                vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
            } else {
                vga_print("  ", VGA_LIGHT_GREY, VGA_BLACK);
            }
        }
    }
    if ((count % 4u) != 0u) {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    }
    return EXT2_OK;
}

/* ------------------------------------------------------------------ */
/* ext2cat：直接块 + 一级间接 + 二级间接                              */
/* ------------------------------------------------------------------ */

/* 从"指针块"里逐项读数据块。level=1 表示这些指针直接指向数据（一级间接），
 * level=2 表示它们指向下一层指针块（二级间接）—— 就是 ext2 的间接块树。 */
static uint32_t copy_from_pointer_block(uint32_t ptr_block, uint32_t level,
                                        uint8_t *buf, uint32_t cap,
                                        uint32_t written)
{
    uint32_t n = g_block_size / 4u;
    uint32_t i;

    if (ptr_block == 0u) {
        return written;
    }
    if (read_block_to(ptr_block, g_ptr_buf) != EXT2_OK) {
        return written;
    }
    for (i = 0u; i < n; ++i) {
        uint32_t child = rd32(g_ptr_buf + i * 4u);
        uint32_t take;

        if (child == 0u) {
            break;
        }
        if (written >= cap) {
            break;
        }
        if (level == 2u) {
            written = copy_from_pointer_block(child, 1u, buf, cap, written);
            continue;
        }
        if (read_block_to(child, g_block_buf2) != EXT2_OK) {
            break;
        }
        take = g_block_size;
        if (take > cap - written) {
            take = cap - written;
        }
        copy_bytes(buf + written, g_block_buf2, take);
        written += take;
    }
    return written;
}

int ext2_read(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen)
{
    inode_t f;
    uint32_t want;
    uint32_t written = 0u;
    uint32_t b;
    int rc;

    if (outlen != (uint64_t *)0) {
        *outlen = 0u;
    }
    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    rc = resolve(path, &f);
    if (rc != EXT2_OK) {
        return rc;
    }
    if ((f.mode & MODE_FMT_MASK) != MODE_FMT_REG) {
        return EXT2_ERR_ISDIR;
    }

    want = f.size;
    if ((uint64_t)want > maxlen) {
        want = (uint32_t)maxlen;
    }

    /* 1) 12 个直接块 */
    for (b = 0u; b < 12u && written < want; ++b) {
        uint32_t take = g_block_size;

        if (f.block[b] == 0u) {
            break;
        }
        if (read_block_to(f.block[b], g_block_buf2) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        if (take > want - written) {
            take = want - written;
        }
        copy_bytes(buf + written, g_block_buf2, take);
        written += take;
    }

    /* 2) 一级间接（i_block[12]） */
    if (written < want) {
        written = copy_from_pointer_block(f.block[12], 1u, buf, want, written);
    }
    /* 3) 二级间接（i_block[13]） */
    if (written < want) {
        written = copy_from_pointer_block(f.block[13], 2u, buf, want, written);
    }

    if (outlen != (uint64_t *)0) {
        *outlen = (uint64_t)written;
    }
    return EXT2_OK;
}

int ext2_size(const char *path, uint64_t *size)
{
    inode_t f;
    int rc;

    if (size == (uint64_t *)0) {
        return EXT2_ERR_CORRUPT;
    }
    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    rc = resolve(path, &f);
    if (rc != EXT2_OK) {
        return rc;
    }
    if ((f.mode & MODE_FMT_MASK) != MODE_FMT_REG) {
        return EXT2_ERR_ISDIR;
    }
    *size = (uint64_t)f.size;
    return EXT2_OK;
}

/* ==================================================================== */
/* [ADD] M21：写路径 —— 位图分配、建文件、写数据、目录项插入          */
/*                                                                    */
/* 对应 Linux 的四个函数：                                             */
/*   ext2_new_block()  -> 块位图里找第一个 0 位，置 1                  */
/*   ext2_new_inode()  -> inode 位图里找第一个 0 位，置 1              */
/*   ext2_get_block()  -> 给文件第 n 个逻辑块挑一个物理块（这里只做   */
/*                        12 个直接块，间接块留给下一步）              */
/*   ext2_add_entry()  -> 在父目录里拆 rec_len，塞进一个新目录项       */
/* 所有写都是 write-through：一块 = block_size/512 个扇区，逐扇区调    */
/* ata_write_sector()，不做页缓存，所以没有 fsync 这一步。             */
/* ==================================================================== */

#define SB_FREE_BLOCKS   0x0Cu
#define SB_FREE_INODES   0x10u
#define SB_WTIME         0x30u
#define GD_BLOCK_BITMAP  0x00u
#define GD_INODE_BITMAP  0x04u
#define GD_INODE_TABLE   0x08u
#define GD_FREE_BLOCKS   0x0Cu
#define GD_FREE_INODES   0x0Eu
#define GD_USED_DIRS     0x10u
#define SECTOR_512       512u
#define IN_LINKS_COUNT   0x1Au
#define IN_MTIME         0x10u
#define IN_ATIME         0x08u
#define IN_DTIME         0x14u   /* [ADD] M23：删除时刻，非 0 才算"已删除" */
#define IN_CTIME         0x0Cu

/* 写路径专用的三块缓冲（与只读路径的 g_block_buf/g_dir_buf 分开，互不覆盖） */
static uint8_t g_wbuf[EXT2_INODE_TABLE_MAX];   /* 读-改-写位图/组描述符用 */
static uint8_t g_wdata[EXT2_INODE_TABLE_MAX];  /* 写数据块 / 目录块用 */
static uint8_t g_winode[EXT2_INODE_TABLE_MAX]; /* inode 表块用 */

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t round_up4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

/* ------------------------------------------------------------------ */
/* 写一个块：read_block_to 的镜像版本                                 */
/* ------------------------------------------------------------------ */

int ext2_write_block(uint32_t blk, const uint8_t *buf)
{
    uint32_t spb = g_block_size / SECTOR_512;
    uint32_t i;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (spb == 0u || blk >= g_blocks_count) {
        return EXT2_ERR_CORRUPT;
    }
    for (i = 0u; i < spb; ++i) {
        if (ata_write_sector(blk * spb + i,
                             (uint8_t *)(buf + i * SECTOR_512)) != ATA_OK) {
            return EXT2_ERR_IO;
        }
    }
    return EXT2_OK;
}

int ext2_read_block(uint32_t blk, uint8_t *buf)
{
    return read_block_to(blk, buf);
}

/* 几何查询（shell 层不必再依赖 ata / 内部静态变量） */
uint32_t ext2_block_size(void)      { return g_block_size; }
uint32_t ext2_blocks_count(void)    { return g_blocks_count; }
uint32_t ext2_inodes_count(void)    { return g_inodes_count; }
uint32_t ext2_blocks_per_group(void){ return g_blocks_per_group; }
uint32_t ext2_inodes_per_group(void){ return g_inodes_per_group; }
uint32_t ext2_first_data_block(void){ return g_first_data_block; }
uint32_t ext2_inode_bytes(void)     { return g_inode_size; }

/* ------------------------------------------------------------------ */
/* 组描述符                                                           */
/* ------------------------------------------------------------------ */

static uint32_t group_count(void)
{
    uint32_t n = (g_blocks_count - g_first_data_block + g_blocks_per_group - 1u)
                 / g_blocks_per_group;
    uint32_t m = (g_inodes_count + g_inodes_per_group - 1u) / g_inodes_per_group;
    return (n > m) ? n : m;
}

static int gdt_read(uint32_t group, uint8_t *out32)
{
    uint32_t off = group * 32u;
    uint32_t blk = g_gdt_block + off / g_block_size;
    uint32_t inb = off % g_block_size;

    if (read_block_to(blk, g_wbuf) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    copy_bytes(out32, g_wbuf + inb, 32u);
    return EXT2_OK;
}

static int gdt_write(uint32_t group, const uint8_t *in32)
{
    uint32_t off = group * 32u;
    uint32_t blk = g_gdt_block + off / g_block_size;
    uint32_t inb = off % g_block_size;

    if (read_block_to(blk, g_wbuf) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    copy_bytes(g_wbuf + inb, in32, 32u);
    return ext2_write_block(blk, g_wbuf);
}

/* ------------------------------------------------------------------ */
/* 超级块：空闲计数（s_free_blocks_count @0x0C / s_free_inodes @0x10） */
/* ------------------------------------------------------------------ */

static int sb_read(uint8_t *sb)
{
    if (ata_read_sector(2u, sb) != ATA_OK) {
        return EXT2_ERR_IO;
    }
    if (ata_read_sector(3u, sb + 512) != ATA_OK) {
        return EXT2_ERR_IO;
    }
    return EXT2_OK;
}

static int sb_bump(int32_t dblocks, int32_t dinodes)
{
    uint8_t sb[1024];

    if (sb_read(sb) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    wr32(sb + SB_FREE_BLOCKS,
         (uint32_t)((int32_t)rd32(sb + SB_FREE_BLOCKS) + dblocks));
    wr32(sb + SB_FREE_INODES,
         (uint32_t)((int32_t)rd32(sb + SB_FREE_INODES) + dinodes));
    wr32(sb + SB_WTIME, 1700000000u);     /* 固定时间戳：这一版不读 RTC */
    if (ata_write_sector(2u, sb) != ATA_OK) {
        return EXT2_ERR_IO;
    }
    if (ata_write_sector(3u, sb + 512) != ATA_OK) {
        return EXT2_ERR_IO;
    }
    copy_bytes(g_sb_buf, sb, 1024u);
    return EXT2_OK;
}

uint32_t ext2_free_blocks(void)
{
    uint8_t sb[1024];
    if (g_mounted == 0 || sb_read(sb) != EXT2_OK) {
        return 0u;
    }
    return rd32(sb + SB_FREE_BLOCKS);
}

uint32_t ext2_free_inodes(void)
{
    uint8_t sb[1024];
    if (g_mounted == 0 || sb_read(sb) != EXT2_OK) {
        return 0u;
    }
    return rd32(sb + SB_FREE_INODES);
}

/* ------------------------------------------------------------------ */
/* 位图分配                                                           */
/* ------------------------------------------------------------------ */

/* 在 limit 位范围内找第一个 0 位 */
static int bitmap_first_zero(const uint8_t *bm, uint32_t from, uint32_t limit,
                             uint32_t *out)
{
    uint32_t i;

    for (i = from; i < limit; ++i) {
        uint32_t byte_i = i / 8u;
        if (byte_i >= (EXT2_INODE_TABLE_MAX / 8u)) {
            break;
        }
        if ((bm[byte_i] & (uint8_t)(1u << (i % 8u))) == 0u) {
            *out = i;
            return EXT2_OK;
        }
    }
    return EXT2_ERR_NOSPC;
}

/* 分配一个块：优先 goal_group，满了就下一组 */
static int alloc_block(uint32_t goal_group, uint32_t *out_blk)
{
    uint32_t groups = group_count();
    uint32_t pass;

    for (pass = 0u; pass < groups; ++pass) {
        uint32_t g = (goal_group + pass) % groups;
        uint8_t  gd[32];
        uint32_t bm_blk, bit, blk, limit, taken;

        if (gdt_read(g, gd) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        bm_blk = rd32(gd + GD_BLOCK_BITMAP);
        if (bm_blk == 0u) {
            continue;
        }
        /* 本组第 0 位对应块号 first_data_block + g*blocks_per_group */
        taken = g * g_blocks_per_group;
        limit = g_blocks_per_group;
        if (taken + limit > g_blocks_count - g_first_data_block) {
            limit = g_blocks_count - g_first_data_block - taken;
        }
        if (read_block_to(bm_blk, g_wbuf) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        if (bitmap_first_zero(g_wbuf, 0u, limit, &bit) != EXT2_OK) {
            continue;
        }
        blk = g_first_data_block + taken + bit;
        if (blk >= g_blocks_count) {
            continue;
        }
        g_wbuf[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        if (ext2_write_block(bm_blk, g_wbuf) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        /* 组描述符 + 超级块两处空闲计数一起减（Linux 也是这么做的） */
        wr16(gd + GD_FREE_BLOCKS, (uint16_t)(rd16(gd + GD_FREE_BLOCKS) - 1u));
        if (gdt_write(g, gd) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        if (sb_bump(-1, 0) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        *out_blk = blk;
        return EXT2_OK;
    }
    return EXT2_ERR_NOSPC;
}

/* 分配一个 inode：组 0 要跳过 1 号与 s_first_ino 之前的保留项 */
static int alloc_inode(uint32_t goal_group, uint32_t *out_ino, int is_dir)
{
    uint32_t groups = group_count();
    uint32_t pass;

    for (pass = 0u; pass < groups; ++pass) {
        uint32_t g = (goal_group + pass) % groups;
        uint8_t  gd[32];
        uint32_t bm_blk, bit, first, limit, taken;

        if (gdt_read(g, gd) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        bm_blk = rd32(gd + GD_INODE_BITMAP);
        if (bm_blk == 0u) {
            continue;
        }
        taken = g * g_inodes_per_group;
        limit = g_inodes_per_group;
        if (taken + limit > g_inodes_count) {
            limit = g_inodes_count - taken;
        }
        first = 0u;
        if (g == 0u) {
            first = (g_first_ino > 1u) ? (g_first_ino - 1u) : 0u;
        }
        if (read_block_to(bm_blk, g_wbuf) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        if (bitmap_first_zero(g_wbuf, first, limit, &bit) != EXT2_OK) {
            continue;
        }
        g_wbuf[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        if (ext2_write_block(bm_blk, g_wbuf) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        wr16(gd + GD_FREE_INODES, (uint16_t)(rd16(gd + GD_FREE_INODES) - 1u));
        if (is_dir) {
            wr16(gd + GD_USED_DIRS, (uint16_t)(rd16(gd + GD_USED_DIRS) + 1u));
        }
        if (gdt_write(g, gd) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        if (sb_bump(0, -1) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        *out_ino = taken + bit + 1u;
        return EXT2_OK;
    }
    return EXT2_ERR_NOSPC;
}

/* ------------------------------------------------------------------ */
/* inode 的整块读-改-写                                               */
/* ------------------------------------------------------------------ */

static int inode_block_off(uint32_t ino, uint32_t *blk, uint32_t *off)
{
    uint32_t group = (ino - 1u) / g_inodes_per_group;
    uint32_t index = (ino - 1u) % g_inodes_per_group;
    uint8_t  gd[32];

    if (gdt_read(group, gd) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    g_inode_table = rd32(gd + GD_INODE_TABLE);
    *blk = g_inode_table + (index * g_inode_size) / g_block_size;
    *off = (index * g_inode_size) % g_block_size;
    return EXT2_OK;
}

int ext2_read_inode_raw(uint32_t ino, uint8_t *raw)
{
    uint32_t blk, off;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (inode_block_off(ino, &blk, &off) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (off + g_inode_size > g_block_size) {
        return EXT2_ERR_CORRUPT;   /* bs=1024 / inode_size=256，本例不会跨块 */
    }
    if (read_block_to(blk, g_winode) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    copy_bytes(raw, g_winode + off, g_inode_size);
    return EXT2_OK;
}

int ext2_write_inode_raw(uint32_t ino, const uint8_t *raw)
{
    uint32_t blk, off;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (inode_block_off(ino, &blk, &off) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (off + g_inode_size > g_block_size) {
        return EXT2_ERR_CORRUPT;
    }
    if (read_block_to(blk, g_winode) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    copy_bytes(g_winode + off, raw, g_inode_size);
    return ext2_write_block(blk, g_winode);
}

/* ------------------------------------------------------------------ */
/* 目录项插入：拆 rec_len                                             */
/* ------------------------------------------------------------------ */

/* 在 dir 里加一项。type: 1=普通文件 2=目录。
 * 思路就是 Linux ext2_add_entry()：从块头扫，找一项"自己实际用的长度 + 新项长度
 * <= 它的 rec_len"的，把 rec_len 砍成自己实际用的长度，剩余空间正好放新项。
 * 目录连一个块都没有时（block[0]==0）先分配一个整块。 */
static int add_dir_entry(uint32_t dir_ino, inode_t *dir, const char *name,
                         uint32_t child_ino, uint8_t type, uint32_t *new_block)
{
    uint32_t name_len = 0u;
    uint32_t b;

    while (name[name_len] != '\0' && name_len + 1u < NAME_MAX_LEN) {
        ++name_len;
    }
    if (name_len == 0u) {
        return EXT2_ERR_NOENT;
    }
    if (new_block != (uint32_t *)0) {
        *new_block = 0u;
    }

    for (b = 0u; b < 12u; ++b) {
        uint32_t off = 0u;
        uint32_t blkno = dir->block[b];

        if (blkno == 0u) {
            uint32_t nb = 0u;
            uint32_t i;
            if (alloc_block((dir_ino - 1u) / g_inodes_per_group, &nb) != EXT2_OK) {
                return EXT2_ERR_NOSPC;
            }
            for (i = 0u; i < g_block_size; ++i) {
                g_wdata[i] = 0u;
            }
            wr32(g_wdata + 0u, child_ino);
            wr16(g_wdata + 4u, (uint16_t)g_block_size);
            g_wdata[6] = (uint8_t)name_len;
            if (g_have_filetype) {
                g_wdata[7] = type;
            }
            for (i = 0u; i < name_len; ++i) {
                g_wdata[g_name_off + i] = name[i];
            }
            if (ext2_write_block(nb, g_wdata) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
            dir->block[b] = nb;
            if (new_block != (uint32_t *)0) {
                *new_block = nb;
            }
            return EXT2_OK;
        }

        if (read_block_to(blkno, g_wdata) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        while (off + 8u <= g_block_size) {
            uint16_t rec = rd16(g_wdata + off + 4u);
            uint8_t  nlen = g_wdata[off + 6u];
            uint32_t used = round_up4(g_name_off + nlen);
            uint32_t need = round_up4(g_name_off + name_len);

            if (rec == 0u || off + rec > g_block_size) {
                break;
            }
            if (used > rec) {
                used = rec;
            }
            if (rec - used >= need) {
                uint8_t *neu = g_wdata + off + used;
                uint32_t i;

                wr16(g_wdata + off + 4u, (uint16_t)used);
                wr32(neu + 0u, child_ino);
                wr16(neu + 4u, (uint16_t)(rec - used));
                neu[6] = (uint8_t)name_len;
                if (g_have_filetype) {
                    neu[7] = type;
                }
                for (i = 0u; i < name_len; ++i) {
                    neu[g_name_off + i] = name[i];
                }
                if (ext2_write_block(blkno, g_wdata) != EXT2_OK) {
                    return EXT2_ERR_IO;
                }
                return EXT2_OK;
            }
            off += rec;
        }
    }
    return EXT2_ERR_NOSPC;      /* 12 个直接块都塞满了 */
}

/* ------------------------------------------------------------------ */
/* ext2_create：建一个普通文件                                         */
/* ------------------------------------------------------------------ */

int ext2_create(const char *path, const uint8_t *data, uint64_t len,
                uint32_t *out_ino)
{
    char comps[PATH_MAX_COMPS][NAME_MAX_LEN];
    char parent_path[96];
    char name[NAME_MAX_LEN];
    uint32_t ncomp = 0u;
    inode_t parent;
    dentry_t existing;
    uint32_t pino, ino = 0u;
    uint32_t blk[15];
    uint32_t blocks = 0u, data_off = 0u, rest;
    uint8_t  raw[EXT2_INODE_TABLE_MAX];
    uint32_t i, k;
    int rc;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (len > (uint64_t)g_block_size * 12u) {
        return EXT2_ERR_NOSPC;             /* 这一版只写 12 个直接块 */
    }
    if (split_path(path, comps, &ncomp) != EXT2_OK || ncomp == 0u) {
        return EXT2_ERR_NOENT;
    }

    /* 父目录路径 = 前 ncomp-1 段 */
    parent_path[0] = '/';
    parent_path[1] = '\0';
    if (ncomp > 1u) {
        uint32_t n = 0u;
        for (i = 0u; i + 1u < ncomp; ++i) {
            parent_path[n++] = '/';
            k = 0u;
            while (comps[i][k] != '\0' && n + 1u < sizeof(parent_path)) {
                parent_path[n++] = comps[i][k++];
            }
        }
        parent_path[n] = '\0';
    }
    k = 0u;
    while (comps[ncomp - 1u][k] != '\0' && k + 1u < sizeof(name)) {
        name[k] = comps[ncomp - 1u][k];
        ++k;
    }
    name[k] = '\0';

    rc = resolve(parent_path, &parent);
    if (rc != EXT2_OK) {
        return rc;
    }
    if ((parent.mode & MODE_FMT_MASK) != MODE_FMT_DIR) {
        return EXT2_ERR_NOTDIR;
    }
    pino = parent.ino;
    if (dir_lookup(&parent, name, &existing) == EXT2_OK) {
        return EXT2_ERR_EXIST;
    }

    /* 1) 分配 inode */
    rc = alloc_inode((pino - 1u) / g_inodes_per_group, &ino, 0);
    if (rc != EXT2_OK) {
        return rc;
    }

    /* 2) 每个逻辑块分配一个物理块并写内容 */
    for (i = 0u; i < 15u; ++i) {
        blk[i] = 0u;
    }
    rest = (uint32_t)len;
    while (blocks < 12u && data_off < (uint32_t)len) {
        uint32_t phys = 0u;
        uint32_t take = (rest > g_block_size) ? g_block_size : rest;
        uint32_t j;

        rc = alloc_block(0u, &phys);
        if (rc != EXT2_OK) {
            return rc;
        }
        for (j = 0u; j < g_block_size; ++j) {
            g_wdata[j] = 0u;               /* 尾部补 0：不留磁盘上的旧垃圾 */
        }
        for (j = 0u; j < take; ++j) {
            g_wdata[j] = data[data_off + j];
        }
        if (ext2_write_block(phys, g_wdata) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        blk[blocks] = phys;
        ++blocks;
        data_off += take;
        rest -= take;
    }

    /* 3) 写 inode：满 256 字节先清零，再只填需要的字段 */
    for (i = 0u; i < g_inode_size; ++i) {
        raw[i] = 0u;
    }
    wr16(raw + IN_MODE, (uint16_t)(MODE_FMT_REG | 0x01A4u));    /* 0100644 */
    wr32(raw + IN_SIZE_LO, (uint32_t)len);
    wr16(raw + IN_LINKS_COUNT, 1u);
    wr32(raw + 0x1Cu, blocks * (g_block_size / SECTOR_512));
    wr32(raw + 0x08u, 1700000000u);       /* atime */
    wr32(raw + IN_CTIME, 1700000000u);
    wr32(raw + IN_MTIME, 1700000000u);
    for (i = 0u; i < 15u; ++i) {
        wr32(raw + IN_BLOCKS + i * 4u, blk[i]);
    }
    if (ext2_write_inode_raw(ino, raw) != EXT2_OK) {
        return EXT2_ERR_IO;
    }

    /* 4) 父目录里加一项；若目录扩了块，父 inode 的 size/block[] 一起更新 */
    {
        uint32_t nb = 0u;
        rc = add_dir_entry(pino, &parent, name, ino, 1u, &nb);
        if (rc != EXT2_OK) {
            return rc;
        }
        if (nb != 0u) {
            uint8_t pbuf[EXT2_INODE_TABLE_MAX];
            uint32_t j;
            if (ext2_read_inode_raw(pino, pbuf) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
            wr32(pbuf + IN_SIZE_LO, parent.size + g_block_size);
            wr32(pbuf + IN_MTIME, 1700000000u);
            for (j = 0u; j < 15u; ++j) {
                wr32(pbuf + IN_BLOCKS + j * 4u, parent.block[j]);
            }
            if (ext2_write_inode_raw(pino, pbuf) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
        }
    }

    if (out_ino != (uint32_t *)0) {
        *out_ino = ino;
    }
    return EXT2_OK;
}

/* ==================================================================== */
/* [ADD] M22：间接块写 —— 让内核能写出 > 12KB 的文件                   */
/*                                                                    */
/* M19 的读路径已经会走 i_block[12]（一级间接），M21 的写路径只填了 12  */
/* 个直接块。这里补上写的对称面：                                     */
/*   1) 先给文件分配一个"指针块"，i_block[12] 指向它                  */
/*   2) 指针块里 每个 u32 = 一个数据块号（bs=1024 时有 256 个槽）      */
/*   3) 数据块还是逐块分配、逐块写                                    */
/* 这正是 Linux ext2_get_block() 里 add_branch 的那一步。              */
/* ==================================================================== */

#define PTR_PER_BLOCK_DEF 256u   /* g_block_size/4，bs=1024 时的默认值 */

static uint8_t g_wptr[EXT2_INODE_TABLE_MAX];   /* 指针块的内容（写之前攒在内存里） */

/* 建一个大文件：先写满 12 个直接块，剩下的走一级间接块。
 * 返回 EXT2_OK / EXT2_ERR_NOSPC（超过 12 + 256 块）等。 */
int ext2_create_big(const char *path, const uint8_t *data, uint64_t len,
                    uint32_t *out_ino, uint32_t *out_blocks, uint32_t *out_indirect)
{
    char comps[PATH_MAX_COMPS][NAME_MAX_LEN];
    char parent_path[96];
    char name[NAME_MAX_LEN];
    uint32_t ncomp = 0u;
    inode_t parent;
    dentry_t existing;
    uint32_t pino, ino = 0u;
    uint32_t blk[15];
    uint32_t ptr_slots;
    uint32_t data_off = 0u;
    uint32_t nblocks = 0u;
    uint32_t indirect_blk = 0u;
    uint32_t slot = 0u;
    uint8_t  raw[EXT2_INODE_TABLE_MAX];
    uint32_t i, k, j;
    int rc;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    ptr_slots = g_block_size / 4u;
    if (len > (uint64_t)g_block_size * (12u + ptr_slots)) {
        return EXT2_ERR_NOSPC;
    }
    if (split_path(path, comps, &ncomp) != EXT2_OK || ncomp == 0u) {
        return EXT2_ERR_NOENT;
    }

    parent_path[0] = '/';
    parent_path[1] = '\0';
    if (ncomp > 1u) {
        uint32_t n = 0u;
        for (i = 0u; i + 1u < ncomp; ++i) {
            parent_path[n++] = '/';
            k = 0u;
            while (comps[i][k] != '\0' && n + 1u < sizeof(parent_path)) {
                parent_path[n++] = comps[i][k++];
            }
        }
        parent_path[n] = '\0';
    }
    k = 0u;
    while (comps[ncomp - 1u][k] != '\0' && k + 1u < sizeof(name)) {
        name[k] = comps[ncomp - 1u][k];
        ++k;
    }
    name[k] = '\0';

    rc = resolve(parent_path, &parent);
    if (rc != EXT2_OK) {
        return rc;
    }
    if ((parent.mode & MODE_FMT_MASK) != MODE_FMT_DIR) {
        return EXT2_ERR_NOTDIR;
    }
    pino = parent.ino;
    if (dir_lookup(&parent, name, &existing) == EXT2_OK) {
        return EXT2_ERR_EXIST;
    }

    rc = alloc_inode((pino - 1u) / g_inodes_per_group, &ino, 0);
    if (rc != EXT2_OK) {
        return rc;
    }
    for (i = 0u; i < 15u; ++i) {
        blk[i] = 0u;
    }

    /* 1) 12 个直接块 */
    while (nblocks < 12u && data_off < (uint32_t)len) {
        uint32_t phys = 0u;
        uint32_t take = ((uint32_t)len - data_off > g_block_size)
                        ? g_block_size : ((uint32_t)len - data_off);

        rc = alloc_block(0u, &phys);
        if (rc != EXT2_OK) {
            return rc;
        }
        for (j = 0u; j < g_block_size; ++j) {
            g_wdata[j] = 0u;
        }
        for (j = 0u; j < take; ++j) {
            g_wdata[j] = data[data_off + j];
        }
        if (ext2_write_block(phys, g_wdata) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        blk[nblocks] = phys;
        ++nblocks;
        data_off += take;
    }

    /* 2) 一级间接：先分配指针块并清零，边分配数据块边往槽里填 */
    if (data_off < (uint32_t)len) {
        rc = alloc_block(0u, &indirect_blk);
        if (rc != EXT2_OK) {
            return rc;
        }
        for (j = 0u; j < g_block_size; ++j) {
            g_wptr[j] = 0u;
        }
        while (slot < ptr_slots && data_off < (uint32_t)len) {
            uint32_t phys = 0u;
            uint32_t take = ((uint32_t)len - data_off > g_block_size)
                            ? g_block_size : ((uint32_t)len - data_off);

            rc = alloc_block(0u, &phys);
            if (rc != EXT2_OK) {
                return rc;
            }
            for (j = 0u; j < g_block_size; ++j) {
                g_wdata[j] = 0u;
            }
            for (j = 0u; j < take; ++j) {
                g_wdata[j] = data[data_off + j];
            }
            if (ext2_write_block(phys, g_wdata) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
            wr32(g_wptr + slot * 4u, phys);
            ++slot;
            ++nblocks;
            data_off += take;
        }
        if (ext2_write_block(indirect_blk, g_wptr) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        blk[12] = indirect_blk;
    }

    /* 3) inode：i_blocks 按 512 字节扇区计数（含指针块本身） */
    for (i = 0u; i < g_inode_size; ++i) {
        raw[i] = 0u;
    }
    wr16(raw + IN_MODE, (uint16_t)(MODE_FMT_REG | 0x01A4u));
    wr32(raw + IN_SIZE_LO, (uint32_t)len);
    wr16(raw + IN_LINKS_COUNT, 1u);
    wr32(raw + 0x1Cu, (nblocks + (indirect_blk ? 1u : 0u)) * (g_block_size / SECTOR_512));
    wr32(raw + 0x08u, 1700000000u);
    wr32(raw + IN_CTIME, 1700000000u);
    wr32(raw + IN_MTIME, 1700000000u);
    for (i = 0u; i < 15u; ++i) {
        wr32(raw + IN_BLOCKS + i * 4u, blk[i]);
    }
    if (ext2_write_inode_raw(ino, raw) != EXT2_OK) {
        return EXT2_ERR_IO;
    }

    /* 4) 父目录加一项 */
    {
        uint32_t nb = 0u;
        rc = add_dir_entry(pino, &parent, name, ino, 1u, &nb);
        if (rc != EXT2_OK) {
            return rc;
        }
        if (nb != 0u) {
            uint8_t pbuf[EXT2_INODE_TABLE_MAX];
            if (ext2_read_inode_raw(pino, pbuf) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
            wr32(pbuf + IN_SIZE_LO, parent.size + g_block_size);
            wr32(pbuf + IN_MTIME, 1700000000u);
            for (j = 0u; j < 15u; ++j) {
                wr32(pbuf + IN_BLOCKS + j * 4u, parent.block[j]);
            }
            if (ext2_write_inode_raw(pino, pbuf) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
        }
    }

    if (out_ino != (uint32_t *)0) {
        *out_ino = ino;
    }
    if (out_blocks != (uint32_t *)0) {
        *out_blocks = nblocks;
    }
    if (out_indirect != (uint32_t *)0) {
        *out_indirect = indirect_blk;
    }
    return EXT2_OK;
}

/* ==================================================================== */
/* [ADD] M23：释放 —— 把盘上的东西删掉                                 */
/*                                                                    */
/* M21/M22 只会让盘变胖。这里补另一半：                                */
/*   free_block()  -> 位图清位 + 组描述符 -1 + 超级块 +1               */
/*   free_inode()  -> 位图清位 + 组描述符 -1（目录还要 used_dirs -1）  */
/*   ext2_truncate() -> 遍历 i_block[0..14]，间接块里的指针逐个释放，   */
/*                      再释放指针块本身（顺序反了就会漏块）            */
/*   ext2_unlink()  -> 目录项 inode 置 0 + rec_len 并给前一项，        */
/*                      i_links_count 减到 0 才真释放 inode             */
/* ==================================================================== */

static uint8_t g_freebuf[EXT2_INODE_TABLE_MAX];   /* 释放时读"指针块"专用 */
static uint8_t g_freebm[EXT2_INODE_TABLE_MAX];    /* 释放时读"位图"专用（不能和 g_freebuf 共用） */

/* 释放一个数据块：位图清位 + 组描述符 + 超级块一起加回去 */
static int free_block(uint32_t blk)
{
    uint32_t group, bit;
    uint32_t bm_blk;
    uint8_t  gd[32];

    if (blk == 0u || blk >= g_blocks_count) {
        return EXT2_ERR_NOENT;
    }
    if (g_first_data_block > blk) {
        return EXT2_ERR_NOENT;
    }
    group = (blk - g_first_data_block) / g_blocks_per_group;
    bit   = (blk - g_first_data_block) % g_blocks_per_group;
    if (group >= group_count()) {
        return EXT2_ERR_CORRUPT;
    }
    if (gdt_read(group, gd) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    bm_blk = rd32(gd + GD_BLOCK_BITMAP);
    if (bm_blk == 0u) {
        return EXT2_ERR_CORRUPT;
    }
    if (read_block_to(bm_blk, g_freebm) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if ((g_freebm[bit / 8u] & (uint8_t)(1u << (bit % 8u))) == 0u) {
        return EXT2_ERR_CORRUPT;      /* 重复释放：宁可报错也不要偷偷放过 */
    }
    g_freebm[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
    if (ext2_write_block(bm_blk, g_freebm) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    wr16(gd + GD_FREE_BLOCKS, (uint16_t)(rd16(gd + GD_FREE_BLOCKS) + 1u));
    if (gdt_write(group, gd) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (sb_bump(1, 0) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    return EXT2_OK;
}

/* 释放一个 inode：位图清位 + 组描述符（目录还要 used_dirs -1）+ 超级块 */
static int free_inode(uint32_t ino, int was_dir)
{
    uint32_t group, bit, bm_blk;
    uint8_t  gd[32];

    if (ino < 1u || ino > g_inodes_count) {
        return EXT2_ERR_NOENT;
    }
    group = (ino - 1u) / g_inodes_per_group;
    bit   = (ino - 1u) % g_inodes_per_group;
    if (gdt_read(group, gd) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    bm_blk = rd32(gd + GD_INODE_BITMAP);
    if (bm_blk == 0u) {
        return EXT2_ERR_CORRUPT;
    }
    if (read_block_to(bm_blk, g_freebm) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if ((g_freebm[bit / 8u] & (uint8_t)(1u << (bit % 8u))) == 0u) {
        return EXT2_ERR_CORRUPT;
    }
    g_freebm[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
    if (ext2_write_block(bm_blk, g_freebm) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    wr16(gd + GD_FREE_INODES, (uint16_t)(rd16(gd + GD_FREE_INODES) + 1u));
    if (was_dir) {
        wr16(gd + GD_USED_DIRS, (uint16_t)(rd16(gd + GD_USED_DIRS) - 1u));
    }
    if (gdt_write(group, gd) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (sb_bump(0, 1) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    return EXT2_OK;
}

/* 递归释放一棵"指针树"：level=1 是一级间接（叶子是数据块），level=2 是二级 */
static int free_ptr_tree(uint32_t ptr_blk, uint32_t level, uint32_t *count)
{
    uint32_t slots = g_block_size / 4u;
    uint32_t i;

    if (ptr_blk == 0u) {
        return EXT2_OK;
    }
    if (read_block_to(ptr_blk, g_freebuf) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    for (i = 0u; i < slots; ++i) {
        uint32_t child = rd32(g_freebuf + i * 4u);
        if (child == 0u) {
            continue;
        }
        if (level > 1u) {
            int rc = free_ptr_tree(child, level - 1u, count);
            if (rc != EXT2_OK) {
                return rc;
            }
        } else {
            if (free_block(child) != EXT2_OK) {
                return EXT2_ERR_IO;
            }
            ++(*count);
        }
    }
    if (free_block(ptr_blk) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    ++(*count);
    return EXT2_OK;
}

/* 把一个 inode 的数据块全放掉；返回释放了多少块（指针块也算一块） */
static int ext2_truncate(uint8_t *raw, uint32_t *out_freed)
{
    uint32_t freed = 0u;
    uint32_t i;
    uint32_t ptr[3];

    ptr[0] = rd32(raw + IN_BLOCKS + 12u * 4u);
    ptr[1] = rd32(raw + IN_BLOCKS + 13u * 4u);
    ptr[2] = rd32(raw + IN_BLOCKS + 14u * 4u);

    for (i = 0u; i < 12u; ++i) {
        uint32_t blk = rd32(raw + IN_BLOCKS + i * 4u);
        if (blk == 0u) {
            continue;
        }
        if (free_block(blk) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        ++freed;
        wr32(raw + IN_BLOCKS + i * 4u, 0u);
    }
    if (free_ptr_tree(ptr[0], 1u, &freed) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (free_ptr_tree(ptr[1], 2u, &freed) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    /* 三级间接：本内核从不分配，但读路径认得它，这里一并说明不予处理 */
    if (ptr[2] != 0u) {
        return EXT2_ERR_CORRUPT;
    }
    wr32(raw + IN_BLOCKS + 12u * 4u, 0u);
    wr32(raw + IN_BLOCKS + 13u * 4u, 0u);
    wr32(raw + IN_SIZE_LO, 0u);
    wr32(raw + 0x1Cu, 0u);                       /* i_blocks = 0 */
    if (out_freed != (uint32_t *)0) {
        *out_freed = freed;
    }
    return EXT2_OK;
}

/* 从父目录里删掉一项：inode 置 0，rec_len 并给前一项（Linux ext2_unlink 的收尾） */
static int remove_dir_entry(uint32_t dir_ino, inode_t *dir, const char *name,
                            uint32_t *out_ino, uint32_t *out_type)
{
    uint32_t name_len = 0u;
    uint32_t b;

    (void)dir_ino;
    while (name[name_len] != '\0' && name_len + 1u < NAME_MAX_LEN) {
        ++name_len;
    }
    for (b = 0u; b < 12u; ++b) {
        uint32_t off = 0u;
        uint32_t prev_off = 0u;
        uint32_t blkno = dir->block[b];
        int      have_prev = 0;

        if (blkno == 0u) {
            continue;
        }
        if (read_block_to(blkno, g_wdata) != EXT2_OK) {
            return EXT2_ERR_IO;
        }
        while (off + 8u <= g_block_size) {
            uint16_t rec = rd16(g_wdata + off + 4u);
            uint8_t  nlen = g_wdata[off + 6u];
            uint32_t eino = rd32(g_wdata + off + 0u);

            if (rec == 0u || off + rec > g_block_size) {
                break;
            }
            if (nlen == name_len && eino != 0u) {
                uint32_t i;
                int hit = 1;
                for (i = 0u; i < name_len; ++i) {
                    if (g_wdata[off + g_name_off + i] != name[i]) {
                        hit = 0;
                        break;
                    }
                }
                if (hit) {
                    uint8_t  type = 1u;
                    if (g_have_filetype) {
                        type = g_wdata[off + 7u];
                    }
                    if (have_prev) {
                        uint16_t prev_rec = rd16(g_wdata + prev_off + 4u);
                        wr16(g_wdata + prev_off + 4u,
                             (uint16_t)(prev_rec + rec));
                        if (ext2_write_block(blkno, g_wdata) != EXT2_OK) {
                            return EXT2_ERR_IO;
                        }
                        if (out_ino != (uint32_t *)0) {
                            *out_ino = eino;
                        }
                        if (out_type != (uint32_t *)0) {
                            *out_type = type;
                        }
                        return EXT2_OK;
                    }
                    /* 第一项：没有前驱可并，只能把 inode 置 0 */
                    wr32(g_wdata + off + 0u, 0u);
                    if (ext2_write_block(blkno, g_wdata) != EXT2_OK) {
                        return EXT2_ERR_IO;
                    }
                    if (out_ino != (uint32_t *)0) {
                        *out_ino = eino;
                    }
                    if (out_type != (uint32_t *)0) {
                        *out_type = type;
                    }
                    return EXT2_OK;
                }
            }
            prev_off = off;
            have_prev = 1;
            off += rec;
        }
    }
    return EXT2_ERR_NOENT;
}

/* 删一个普通文件（暂不支持删目录，留给下一步） */
int ext2_unlink(const char *path, uint32_t *out_ino, uint32_t *out_freed)
{
    char comps[PATH_MAX_COMPS][NAME_MAX_LEN];
    char parent_path[96];
    char name[NAME_MAX_LEN];
    uint32_t ncomp = 0u;
    inode_t parent;
    dentry_t de;
    uint32_t pino, ino = 0u, type = 0u, freed = 0u;
    uint8_t  raw[EXT2_INODE_TABLE_MAX];
    uint32_t i, k;
    int rc;

    if (g_mounted == 0) {
        return EXT2_ERR_NOTMOUNTED;
    }
    if (split_path(path, comps, &ncomp) != EXT2_OK || ncomp == 0u) {
        return EXT2_ERR_NOENT;
    }
    parent_path[0] = '/';
    parent_path[1] = '\0';
    if (ncomp > 1u) {
        uint32_t n = 0u;
        for (i = 0u; i + 1u < ncomp; ++i) {
            parent_path[n++] = '/';
            k = 0u;
            while (comps[i][k] != '\0' && n + 1u < sizeof(parent_path)) {
                parent_path[n++] = comps[i][k++];
            }
        }
        parent_path[n] = '\0';
    }
    k = 0u;
    while (comps[ncomp - 1u][k] != '\0' && k + 1u < sizeof(name)) {
        name[k] = comps[ncomp - 1u][k];
        ++k;
    }
    name[k] = '\0';

    rc = resolve(parent_path, &parent);
    if (rc != EXT2_OK) {
        return rc;
    }
    pino = parent.ino;
    if (dir_lookup(&parent, name, &de) != EXT2_OK) {
        return EXT2_ERR_NOENT;
    }
    {
        inode_t ti;
        if (read_inode(de.ino, &ti) == EXT2_OK &&
            (ti.mode & MODE_FMT_MASK) == MODE_FMT_DIR) {
            return EXT2_ERR_ISDIR;     /* 目录先不删，避免搞坏 . 和 .. 的 nlink */
        }
    }

    if (remove_dir_entry(pino, &parent, name, &ino, &type) != EXT2_OK) {
        return EXT2_ERR_NOENT;
    }
    if (ext2_read_inode_raw(ino, raw) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (ext2_truncate(raw, &freed) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    wr16(raw + IN_LINKS_COUNT, 0u);
    wr32(raw + IN_DTIME, 1700000000u);           /* i_dtime：删除时刻，e2fsck 要求非 0 */
    wr32(raw + IN_CTIME, 1700000000u);
    wr32(raw + IN_MTIME, 1700000000u);
    wr32(raw + 0x1Cu, 0u);                       /* i_blocks 归零 */
    {
        uint32_t j;
        for (j = 0u; j < 15u; ++j) {
            wr32(raw + IN_BLOCKS + j * 4u, 0u);
        }
    }
    if (ext2_write_inode_raw(ino, raw) != EXT2_OK) {
        return EXT2_ERR_IO;
    }
    if (free_inode(ino, 0) != EXT2_OK) {
        return EXT2_ERR_IO;
    }

    /* 父目录 mtime 更新（Linux ext2_unlink 也会 touch 父目录） */
    {
        uint8_t pbuf[EXT2_INODE_TABLE_MAX];
        if (ext2_read_inode_raw(pino, pbuf) == EXT2_OK) {
            wr32(pbuf + IN_MTIME, 1700000000u);
            (void)ext2_write_inode_raw(pino, pbuf);
        }
    }

    if (out_ino != (uint32_t *)0) {
        *out_ino = ino;
    }
    if (out_freed != (uint32_t *)0) {
        *out_freed = freed;
    }
    return EXT2_OK;
}
