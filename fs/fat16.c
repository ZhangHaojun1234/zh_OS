/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    fs/fat16.c
 * Version: 1.0
 * Brief:   FAT16 只读实现：解析 BPB、定位 FAT/根目录/数据区、遍历 8.3 目录项、
 *          跟随簇链读文件。读的是 ATA 主通道「从设备」（QEMU if=ide,index=1）。
 *
 * B 组第 3 个功能，纯增量「只添加，不修改」：
 *   - 只读：不实现写、不实现 mkdir / rm。
 *   - 只解析 8.3 短文件名，不解析 LFN；名字匹配大小写不敏感。
 *   - 支持根目录与一层子目录（实现上最多 4 段路径，比任务要求更宽松）。
 *   - 复用 drivers/ata 的 ata_read_sector_slave（B2 的 ata_read_sector 一行未改）。
 *   - 本文件不触碰 ramfs，fatls / fatcat 与 ls / cat 完全隔离。
 * ============================================================================ */

#include "fat16.h"

#include <stdbool.h>

#include "../drivers/ata.h"   /* ata_read_sector_slave / ATA_OK */
#include "../drivers/vga.h"   /* vga_print / vga_putchar / enum vga_color */

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

#define FAT16_SECTOR_SIZE          512u   /* 本实现要求每扇区 512 字节 */
#define FAT16_ENTRIES_PER_SECTOR   16u    /* 512 / 32 = 16 个目录项 */
#define FAT16_DIRENT_SIZE          32u
#define FAT16_EOC_MIN              0xFFF8u /* >= 该值为簇链结束 */
#define FAT16_MAX_CLUSTER          0xFFEFu /* FAT16 有效簇号上限 */

/* ------------------------------------------------------------------ */
/* 挂载后缓存的几何信息（全部为绝对 LBA，已含隐藏扇区）              */
/* ------------------------------------------------------------------ */

static int      g_mounted = 0;
static uint32_t g_bytes_per_sector   = 0;
static uint32_t g_sectors_per_cluster = 0;
static uint32_t g_reserved_sectors   = 0;
static uint32_t g_num_fats           = 0;
static uint32_t g_root_entry_count   = 0;
static uint32_t g_fat_size           = 0;  /* 每个 FAT 的扇区数 */
static uint32_t g_total_sectors      = 0;
static uint32_t g_root_cluster       = 0;  /* 读自 0x2C，FAT16 不使用（见头文件说明） */
static uint32_t g_part_start         = 0;  /* 隐藏扇区数 */
static uint32_t g_fat_start_lba      = 0;
static uint32_t g_root_start_lba     = 0;
static uint32_t g_root_sectors       = 0;
static uint32_t g_data_start_lba     = 0;
static uint32_t g_cluster_count      = 0;

/* 单扇区暂存缓冲（静态，避免占内核栈） */
static uint8_t g_sector_buf[FAT16_SECTOR_SIZE];

/* ------------------------------------------------------------------ */
/* 小端读取与目录项判定                                               */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}

static void copy32(uint8_t *dst, const uint8_t *src)
{
    for (uint32_t i = 0; i < FAT16_DIRENT_SIZE; ++i) {
        dst[i] = src[i];
    }
}

/* 目录项首簇号：FstClusHI(0x14) << 16 | FstClusLO(0x1A)（FAT16 高 16 位恒 0） */
static uint32_t entry_cluster(const uint8_t *e)
{
    return (uint32_t)rd16(e + 0x1A) | ((uint32_t)rd16(e + 0x14) << 16u);
}

static uint32_t entry_size(const uint8_t *e)
{
    return rd32(e + 0x1C);
}

static bool entry_is_dir(const uint8_t *e)
{
    return (e[0x0B] & 0x10u) != 0u;
}

/* "." / ".." 目录项：列举时跳过，避免子目录列表出现噪音项 */
static bool is_dot_entry(const uint8_t *e)
{
    if (e[0] != '.') {
        return false;
    }
    if (e[1] != '.' && e[1] != ' ') {
        return false;
    }
    for (uint32_t i = 2u; i < 8u; ++i) {
        if (e[i] != ' ') {
            return false;
        }
    }
    for (uint32_t i = 8u; i < 11u; ++i) {
        if (e[i] != ' ') {
            return false;
        }
    }
    return true;
}

static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 32);
    }
    return c;
}

/* 比较 8.3 目录项与单个路径分量（大小写不敏感，分量可带一个 '.' 后缀） */
static bool name_match(const uint8_t *e, const char *comp)
{
    char want_name[8];
    char want_ext[3];
    int nl = 0;
    int el = 0;
    const char *p = comp;

    while (*p != '\0' && *p != '.') {
        if (nl >= 8) {
            return false;   /* 名字超过 8.3 规范，必然不匹配 */
        }
        want_name[nl++] = to_upper(*p);
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (*p != '\0') {
            if (el >= 3) {
                return false;
            }
            want_ext[el++] = to_upper(*p);
            ++p;
        }
    }

    /* 目录项名字段与扩展名字段均以空格补齐 */
    for (int i = 0; i < 8; ++i) {
        const char ec = (e[i] == 0x00u) ? ' ' : to_upper((char)e[i]);
        const char wc = (i < nl) ? want_name[i] : ' ';
        if (ec != wc) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const char ec = to_upper((char)e[8 + i]);
        const char wc = (i < el) ? want_ext[i] : ' ';
        if (ec != wc) {
            return false;
        }
    }
    return true;
}

/* 把 11 字节原始名整理成显示用 "NAME" 或 "NAME.EXT"（去尾部空格） */
static void build_display_name(const uint8_t *e, char *out)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < 8u; ++i) {
        if (e[i] == ' ') {
            break;
        }
        out[n++] = (char)e[i];
    }
    bool has_ext = false;
    for (uint32_t i = 0; i < 3u; ++i) {
        if (e[8u + i] != ' ') {
            has_ext = true;
            break;
        }
    }
    if (has_ext) {
        out[n++] = '.';
        for (uint32_t i = 0; i < 3u; ++i) {
            if (e[8u + i] == ' ') {
                break;
            }
            out[n++] = (char)e[8u + i];
        }
    }
    out[n] = '\0';
}

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 簇与 FAT 定位                                                      */
/* ------------------------------------------------------------------ */

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return g_data_start_lba + (cluster - 2u) * g_sectors_per_cluster;
}

/* 读取 cluster 在 FAT 表中的下一簇号 */
static int fat16_next_cluster(uint32_t cluster, uint32_t *next)
{
    const uint32_t off_bytes = cluster * 2u;   /* FAT16 每项 2 字节 */
    const uint32_t sector = g_fat_start_lba + (off_bytes / FAT16_SECTOR_SIZE);
    const uint32_t off = off_bytes % FAT16_SECTOR_SIZE;

    if (ata_read_sector_slave(sector, g_sector_buf) != ATA_OK) {
        return FAT16_ERR_IO;
    }
    *next = rd16(g_sector_buf + off);
    return FAT16_OK;
}

/* ------------------------------------------------------------------ */
/* 目录扫描：根目录（固定区域）与子目录（簇链）                       */
/* ------------------------------------------------------------------ */

/* 返回 1=找到并写入 out；0=遍历结束未找到；负=错误 */
static int scan_root(const char *want, uint8_t *out)
{
    uint32_t remaining = g_root_entry_count;

    for (uint32_t s = 0; s < g_root_sectors && remaining > 0u; ++s) {
        if (ata_read_sector_slave(g_root_start_lba + s, g_sector_buf) != ATA_OK) {
            return FAT16_ERR_IO;
        }
        for (uint32_t k = 0; k < FAT16_ENTRIES_PER_SECTOR && remaining > 0u;
             ++k, --remaining) {
            const uint8_t *e = g_sector_buf + k * FAT16_DIRENT_SIZE;
            if (e[0] == 0x00u) {
                return 0;             /* 0x00 = 目录项结束 */
            }
            if (e[0] == 0xE5u) {
                continue;             /* 0xE5 = 已删除 */
            }
            if ((e[0x0B] & 0x0Fu) == 0x0Fu) {
                continue;             /* LFN 项跳过 */
            }
            if ((e[0x0B] & 0x08u) != 0u) {
                continue;             /* 卷标跳过 */
            }
            if (name_match(e, want)) {
                copy32(out, e);
                return 1;
            }
        }
    }
    return 0;
}

/* 在子目录的簇链中查找，返回码同上 */
static int scan_cluster_dir(uint32_t first, const char *want, uint8_t *out)
{
    uint32_t cluster = first;
    uint32_t guard = 0;

    while (cluster >= 2u && cluster <= FAT16_MAX_CLUSTER) {
        if (++guard > 65536u) {
            break;                    /* 防御环链 */
        }
        const uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_sectors_per_cluster; ++s) {
            if (ata_read_sector_slave(lba + s, g_sector_buf) != ATA_OK) {
                return FAT16_ERR_IO;
            }
            for (uint32_t k = 0; k < FAT16_ENTRIES_PER_SECTOR; ++k) {
                const uint8_t *e = g_sector_buf + k * FAT16_DIRENT_SIZE;
                if (e[0] == 0x00u) {
                    return 0;
                }
                if (e[0] == 0xE5u) {
                    continue;
                }
                if ((e[0x0B] & 0x0Fu) == 0x0Fu) {
                    continue;
                }
                if ((e[0x0B] & 0x08u) != 0u) {
                    continue;
                }
                if (name_match(e, want)) {
                    copy32(out, e);
                    return 1;
                }
            }
        }
        uint32_t next = 0;
        const int rc = fat16_next_cluster(cluster, &next);
        if (rc != FAT16_OK) {
            return rc;
        }
        if (next > FAT16_MAX_CLUSTER) {
            break;
        }
        cluster = next;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 路径解析                                                           */
/* ------------------------------------------------------------------ */

/* 把 path 按 '/' 切段（跳过前导/重复 '/'），最多 4 段、每段最长 15 字符。
 * 返回 0 成功（*n 为段数，*n==0 表示根目录），-1 非法。 */
static int split_path(const char *path, char comps[4][16], int *n)
{
    int count = 0;
    const char *p = path;

    if (p == 0) {
        return -1;
    }
    while (*p == '/') {
        ++p;
    }
    if (*p == '\0') {
        *n = 0;
        return 0;
    }
    while (*p != '\0') {
        if (count >= 4) {
            return -1;
        }
        int j = 0;
        while (*p != '\0' && *p != '/') {
            if (j >= 15) {
                return -1;
            }
            comps[count][j++] = *p++;
        }
        comps[count][j] = '\0';
        ++count;
        while (*p == '/') {
            ++p;
        }
    }
    *n = count;
    return 0;
}

/* 解析路径到最终目录项，写入 out[32]。根目录本身返回 FAT16_ERR_ISDIR。 */
static int resolve_entry(const char *path, uint8_t *out)
{
    char comps[4][16];
    int n = 0;

    if (!g_mounted) {
        return FAT16_ERR_NOTMOUNTED;
    }
    if (split_path(path, comps, &n) != 0) {
        return FAT16_ERR_NOENT;
    }
    if (n == 0) {
        return FAT16_ERR_ISDIR;
    }

    uint32_t cur = 0;
    bool is_root = true;
    for (int i = 0; i < n; ++i) {
        uint8_t e[FAT16_DIRENT_SIZE];
        const int found = is_root ? scan_root(comps[i], e)
                                  : scan_cluster_dir(cur, comps[i], e);
        if (found < 0) {
            return found;
        }
        if (found == 0) {
            return FAT16_ERR_NOENT;
        }
        if (i == n - 1) {
            copy32(out, e);
            return FAT16_OK;
        }
        if (!entry_is_dir(e)) {
            return FAT16_ERR_NOTDIR;
        }
        cur = entry_cluster(e);
        is_root = false;
    }
    return FAT16_ERR_NOENT;
}

/* ------------------------------------------------------------------ */
/* 目录列举：4 列、目录亮蓝带 '/'、文件浅灰                           */
/* ------------------------------------------------------------------ */

static void list_emit(const uint8_t *e, uint32_t *count)
{
    char name[16];
    build_display_name(e, name);
    const bool isdir = entry_is_dir(e);
    const enum vga_color col = isdir ? VGA_LIGHT_BLUE : VGA_LIGHT_GREY;

    uint32_t n = str_len(name);
    if (isdir) {
        name[n++] = '/';
        name[n] = '\0';
    }

    vga_print(name, col, VGA_BLACK);
    uint32_t pad = n;
    while (pad < 20u) {                 /* 每行 4 列，列宽 20 */
        vga_print(" ", col, VGA_BLACK);
        ++pad;
    }
    ++(*count);
    if (((*count) % 4u) == 0u) {
        vga_putchar('\n', col, VGA_BLACK);
    }
}

static int list_root(uint32_t *count)
{
    uint32_t remaining = g_root_entry_count;

    for (uint32_t s = 0; s < g_root_sectors && remaining > 0u; ++s) {
        if (ata_read_sector_slave(g_root_start_lba + s, g_sector_buf) != ATA_OK) {
            return FAT16_ERR_IO;
        }
        for (uint32_t k = 0; k < FAT16_ENTRIES_PER_SECTOR && remaining > 0u;
             ++k, --remaining) {
            const uint8_t *e = g_sector_buf + k * FAT16_DIRENT_SIZE;
            if (e[0] == 0x00u) {
                return FAT16_OK;
            }
            if (e[0] == 0xE5u) {
                continue;
            }
            if ((e[0x0B] & 0x0Fu) == 0x0Fu) {
                continue;
            }
            if ((e[0x0B] & 0x08u) != 0u) {
                continue;
            }
            if (is_dot_entry(e)) {
                continue;
            }
            list_emit(e, count);
        }
    }
    return FAT16_OK;
}

static int list_cluster_dir(uint32_t first, uint32_t *count)
{
    uint32_t cluster = first;
    uint32_t guard = 0;

    while (cluster >= 2u && cluster <= FAT16_MAX_CLUSTER) {
        if (++guard > 65536u) {
            break;
        }
        const uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_sectors_per_cluster; ++s) {
            if (ata_read_sector_slave(lba + s, g_sector_buf) != ATA_OK) {
                return FAT16_ERR_IO;
            }
            for (uint32_t k = 0; k < FAT16_ENTRIES_PER_SECTOR; ++k) {
                const uint8_t *e = g_sector_buf + k * FAT16_DIRENT_SIZE;
                if (e[0] == 0x00u) {
                    return FAT16_OK;
                }
                if (e[0] == 0xE5u) {
                    continue;
                }
                if ((e[0x0B] & 0x0Fu) == 0x0Fu) {
                    continue;
                }
                if ((e[0x0B] & 0x08u) != 0u) {
                    continue;
                }
                if (is_dot_entry(e)) {
                    continue;
                }
                list_emit(e, count);
            }
        }
        uint32_t next = 0;
        const int rc = fat16_next_cluster(cluster, &next);
        if (rc != FAT16_OK) {
            return rc;
        }
        if (next > FAT16_MAX_CLUSTER) {
            break;
        }
        cluster = next;
    }
    return FAT16_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

int fat16_mount(void)
{
    g_mounted = 0;

    /* 从设备是否存在 / 引导扇区能否读出。 */
    if (ata_read_sector_slave(0, g_sector_buf) != ATA_OK) {
        return FAT16_ERR_NODEV;
    }

    const uint8_t *b = g_sector_buf;

    /* ---- 解析 BPB（偏移见 fat16.h 顶部字段表） ---- */
    const uint32_t bps       = rd16(b + 0x0B);   /* 每扇区字节数 */
    const uint32_t spc       = (uint32_t)b[0x0D];/* 每簇扇区数 */
    const uint32_t rsvd      = rd16(b + 0x0E);   /* 保留扇区数 */
    const uint32_t nfats     = (uint32_t)b[0x10];/* FAT 个数 */
    const uint32_t root_ents = rd16(b + 0x11);   /* 根目录项数 */
    const uint32_t tot16     = rd16(b + 0x13);   /* 总扇区数（16 位） */
    const uint32_t fatsz     = rd16(b + 0x16);   /* 每个 FAT 扇区数 */
    const uint32_t hidd      = rd32(b + 0x1C);   /* 隐藏扇区数 */
    const uint32_t tot32     = rd32(b + 0x20);   /* 总扇区数（32 位） */
    const uint32_t rootclus  = rd32(b + 0x2C);   /* 根簇号（仅 FAT32 有意义） */

    /* ---- BPB 合法性 ---- */
    if (bps != FAT16_SECTOR_SIZE) {
        return FAT16_ERR_BPB;    /* 本实现只支持 512 字节扇区 */
    }
    if (spc == 0u || (spc & (spc - 1u)) != 0u) {
        return FAT16_ERR_BPB;    /* 每簇扇区数须为 2 的幂 */
    }
    if (rsvd == 0u || nfats == 0u || fatsz == 0u || root_ents == 0u) {
        return FAT16_ERR_BPB;
    }

    const uint32_t total = (tot16 != 0u) ? tot16 : tot32;
    if (total == 0u) {
        return FAT16_ERR_BPB;
    }

    /* ---- 推导各区域起始 LBA（绝对，含隐藏扇区） ---- */
    const uint32_t part_start   = hidd;
    const uint32_t fat_start    = part_start + rsvd;
    const uint32_t root_start   = fat_start + nfats * fatsz;
    const uint32_t root_sectors = ((root_ents * FAT16_DIRENT_SIZE) + bps - 1u) / bps;
    const uint32_t data_start   = root_start + root_sectors;

    /* 数据区簇数（相对分区起点），用于按簇数推断 FAT 类型 */
    uint32_t clusters = 0;
    const uint32_t meta = data_start - part_start;   /* 保留+FAT+根目录 */
    if (total > meta) {
        clusters = (total - meta) / spc;
    }

    /* ---- 类型校验：0x36 处 "FAT16   " 或簇数落在 FAT16 区间 ---- */
    const bool want_str = (b[0x36] == 'F' && b[0x37] == 'A' && b[0x38] == 'T' &&
                           b[0x39] == '1' && b[0x3A] == '6' &&
                           b[0x3B] == ' ' && b[0x3C] == ' ' && b[0x3D] == ' ');
    const bool want_cnt = (clusters >= 4085u && clusters < 65525u);
    if (!want_str && !want_cnt) {
        return FAT16_ERR_NOTFAT;
    }

    /* ---- 缓存几何信息 ---- */
    g_bytes_per_sector    = bps;
    g_sectors_per_cluster = spc;
    g_reserved_sectors    = rsvd;
    g_num_fats            = nfats;
    g_root_entry_count    = root_ents;
    g_fat_size            = fatsz;
    g_total_sectors       = total;
    g_root_cluster        = rootclus;
    g_part_start          = part_start;
    g_fat_start_lba       = fat_start;
    g_root_start_lba      = root_start;
    g_root_sectors        = root_sectors;
    g_data_start_lba      = data_start;
    g_cluster_count       = clusters;

    g_mounted = 1;
    return FAT16_OK;
}

int fat16_list(const char *path)
{
    char comps[4][16];
    int n = 0;

    if (!g_mounted) {
        return FAT16_ERR_NOTMOUNTED;
    }
    if (split_path(path, comps, &n) != 0) {
        return FAT16_ERR_NOENT;
    }

    /* 逐段下行到目标目录 */
    uint32_t dir_cluster = 0;
    bool is_root = true;
    for (int i = 0; i < n; ++i) {
        uint8_t e[FAT16_DIRENT_SIZE];
        const int found = is_root ? scan_root(comps[i], e)
                                  : scan_cluster_dir(dir_cluster, comps[i], e);
        if (found < 0) {
            return found;
        }
        if (found == 0) {
            return FAT16_ERR_NOENT;
        }
        if (!entry_is_dir(e)) {
            return FAT16_ERR_NOTDIR;
        }
        dir_cluster = entry_cluster(e);
        is_root = false;
    }

    uint32_t count = 0;
    const int rc = is_root ? list_root(&count) : list_cluster_dir(dir_cluster, &count);
    if (rc != FAT16_OK) {
        return rc;
    }

    if (count == 0u) {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    } else if ((count % 4u) != 0u) {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    }
    return FAT16_OK;
}

int fat16_size(const char *path, uint64_t *size)
{
    uint8_t e[FAT16_DIRENT_SIZE];
    const int rc = resolve_entry(path, e);
    if (rc != FAT16_OK) {
        return rc;
    }
    if (entry_is_dir(e)) {
        return FAT16_ERR_ISDIR;
    }
    *size = (uint64_t)entry_size(e);
    return FAT16_OK;
}

int fat16_read(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen)
{
    uint8_t e[FAT16_DIRENT_SIZE];

    if (buf == 0 || outlen == 0) {
        return FAT16_ERR_IO;
    }
    *outlen = 0;

    const int rc = resolve_entry(path, e);
    if (rc != FAT16_OK) {
        return rc;
    }
    if (entry_is_dir(e)) {
        return FAT16_ERR_ISDIR;
    }

    const uint64_t size = (uint64_t)entry_size(e);
    const uint64_t want = (size < maxlen) ? size : maxlen;
    uint32_t cluster = entry_cluster(e);
    uint64_t copied = 0;
    uint32_t guard = 0;

    while (cluster >= 2u && cluster <= FAT16_MAX_CLUSTER && copied < want) {
        if (++guard > 65536u) {
            break;
        }
        const uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_sectors_per_cluster && copied < want; ++s) {
            if (ata_read_sector_slave(lba + s, g_sector_buf) != ATA_OK) {
                return FAT16_ERR_IO;
            }
            uint32_t chunk = FAT16_SECTOR_SIZE;
            if ((uint64_t)chunk > (want - copied)) {
                chunk = (uint32_t)(want - copied);
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                buf[copied + i] = g_sector_buf[i];
            }
            copied += chunk;
        }
        if (copied >= want) {
            break;
        }
        uint32_t next = 0;
        const int nrc = fat16_next_cluster(cluster, &next);
        if (nrc != FAT16_OK) {
            return nrc;
        }
        if (next > FAT16_MAX_CLUSTER) {
            break;
        }
        cluster = next;
    }

    *outlen = copied;
    return FAT16_OK;
}
