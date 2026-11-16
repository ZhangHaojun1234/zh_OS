/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    fs/vfs.c
 * Version: 0.2beta
 * Brief:   挂载表 + 最长前缀路径解析 + cwd 的实现。
 *
 * 对应 Linux 的两件事：
 *   - vfs_resolve() ≈ path lookup 里"找到所属 vfsmount"那一步（这里用最长前缀匹配
 *     代替 dentry 树的向上回溯，因为我们没有真正的 dentry 缓存）；
 *   - vfs_list()/vfs_read() ≈ 通过 file_operations 间接调用具体文件系统，
 *     这里用 enum + switch 代替函数指针表（freestanding、没有 C++/trampoline 的讲究）。
 *
 * 关键约定：**挂载表空着的时候，一切行为与 M19 之前完全一致**。
 * 没有匹配到挂载点的路径，vfs_list/vfs_read 直接返回 VFS_ERR_NOENT，
 * 命令层看到这个码就回落到 ramfs —— 这才是纯增量。
 * ============================================================================ */

#include "vfs.h"

#include <stdbool.h>

#include "ext2.h"
#include "fat16.h"

static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static char        g_cwd[VFS_PATH_MAX] = "/";
static int         g_cwd_in_mount = 0;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static bool str_eq(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (a[i] != '\0' || b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        ++i;
    }
    return true;
}

/* 把 src 拷进 dst（最多 cap-1 字节），返回长度 */
static uint32_t str_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0u;
    if (cap == 0u) {
        return 0u;
    }
    while (src[i] != '\0' && i + 1u < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    return i;
}

/* 去掉末尾的 '/'（"/mnt/" -> "/mnt"），"根" 除外 */
static void strip_trailing_slash(char *p)
{
    uint32_t n = 0u;
    while (p[n] != '\0') {
        ++n;
    }
    while (n > 1u && p[n - 1u] == '/') {
        p[n - 1u] = '\0';
        --n;
    }
}

/* ------------------------------------------------------------------ */
/* 挂载表                                                             */
/* ------------------------------------------------------------------ */

void vfs_reset(void)
{
    uint32_t i;
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        g_mounts[i].used = 0u;
        g_mounts[i].prefix[0] = '\0';
        g_mounts[i].fs = VFS_FS_RAMFS;
    }
    str_copy(g_cwd, "/", sizeof(g_cwd));
    g_cwd_in_mount = 0;
}

int vfs_mount(const char *prefix, vfs_fs_t fs)
{
    uint32_t i;
    char p[VFS_PREFIX_MAX];

    if (prefix == (const char *)0 || prefix[0] != '/') {
        return VFS_ERR_INVAL;             /* 必须是绝对路径 */
    }
    str_copy(p, prefix, sizeof(p));
    strip_trailing_slash(p);
    if (p[1] == '\0') {
        return VFS_ERR_INVAL;             /* 不允许挂到 "/" 上（root 是 ramfs） */
    }
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        if (g_mounts[i].used && str_eq(g_mounts[i].prefix, p)) {
            return VFS_ERR_EXIST;
        }
    }
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        if (g_mounts[i].used == 0u) {
            str_copy(g_mounts[i].prefix, p, sizeof(g_mounts[i].prefix));
            g_mounts[i].fs = fs;
            g_mounts[i].used = 1u;
            return VFS_OK;
        }
    }
    return VFS_ERR_FULL;
}

int vfs_umount(const char *prefix)
{
    uint32_t i;
    char p[VFS_PREFIX_MAX];

    if (prefix == (const char *)0) {
        return VFS_ERR_INVAL;
    }
    str_copy(p, prefix, sizeof(p));
    strip_trailing_slash(p);
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        if (g_mounts[i].used && str_eq(g_mounts[i].prefix, p)) {
            g_mounts[i].used = 0u;
            g_mounts[i].prefix[0] = '\0';
            if (g_cwd_in_mount) {         /* cwd 正好在被卸载的地方就退回 "/" */
                uint32_t k = 0u;
                while (p[k] != '\0' && g_cwd[k] == p[k]) {
                    ++k;
                }
                if (p[k] == '\0' && (g_cwd[k] == '\0' || g_cwd[k] == '/')) {
                    str_copy(g_cwd, "/", sizeof(g_cwd));
                    g_cwd_in_mount = 0;
                }
            }
            return VFS_OK;
        }
    }
    return VFS_ERR_NOENT;
}

const vfs_mount_t *vfs_mount_at(uint32_t index)
{
    if (index >= VFS_MAX_MOUNTS || g_mounts[index].used == 0u) {
        return (const vfs_mount_t *)0;
    }
    return &g_mounts[index];
}

uint32_t vfs_mount_count(void)
{
    uint32_t i;
    uint32_t n = 0u;
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        if (g_mounts[i].used) {
            ++n;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 最长前缀匹配                                                       */
/* ------------------------------------------------------------------ */

/* 判断 path 是否以 prefix 开头，且前缀之后正好是 '\0' 或 '/'（避免 "/mntx" 命中 "/mnt"） */
static bool prefix_match(const char *path, const char *prefix)
{
    uint32_t i = 0u;
    while (prefix[i] != '\0') {
        if (path[i] != prefix[i]) {
            return false;
        }
        ++i;
    }
    return (path[i] == '\0' || path[i] == '/');
}

/* 就地规范化绝对路径：折叠 "//"、去掉 "./"、处理 ".."（不触碰真实 FS） */
static void normalize_abs(char *p)
{
    char out[VFS_PATH_MAX];
    uint32_t n = 0u;
    uint32_t k = 0u;
    uint32_t t;

    out[n++] = '/';
    while (p[k] != '\0') {
        while (p[k] == '/') {
            ++k;
        }
        if (p[k] == '\0') {
            break;
        }
        char comp[64];
        uint32_t cl = 0u;
        while (p[k] != '\0' && p[k] != '/' && cl + 1u < sizeof(comp)) {
            comp[cl++] = p[k++];
        }
        comp[cl] = '\0';
        if (str_eq(comp, ".")) {
            continue;
        }
        if (str_eq(comp, "..")) {
            uint32_t last = 1u;
            for (t = 1u; t < n; ++t) {
                if (out[t] == '/') {
                    last = t;
                }
            }
            n = last;
            continue;
        }
        if (n > 1u && n + 1u < sizeof(out)) {
            out[n++] = '/';
        }
        for (t = 0u; t < cl && n + 1u < sizeof(out); ++t) {
            out[n++] = comp[t];
        }
    }
    out[n] = '\0';
    str_copy(p, out, VFS_PATH_MAX);
}

int vfs_resolve(const char *path, vfs_fs_t *fs, char *out_path, uint32_t cap)
{
    uint32_t i;
    uint32_t best = VFS_MAX_MOUNTS;
    uint32_t best_len = 0u;
    char p[VFS_PATH_MAX];

    if (path == (const char *)0) {
        return 0;
    }
    /* 相对路径先拼上 cwd：Linux 也是这么做的（nd->path 从 cwd 开始） */
    if (path[0] != '/') {
        uint32_t n = 0u;
        uint32_t cl = 0u;
        const char *cwd = g_cwd;
        while (cwd[cl] != '\0' && n + 1u < sizeof(p)) {
            p[n++] = cwd[cl++];
        }
        if (n > 0u && p[n - 1u] != '/' && n + 1u < sizeof(p)) {
            p[n++] = '/';
        }
        uint32_t k = 0u;
        while (path[k] != '\0' && n + 1u < sizeof(p)) {
            p[n++] = path[k++];
        }
        p[n] = '\0';
        if (str_eq(p, "/.") || (p[0] == '\0')) {
            str_copy(p, "/", sizeof(p));
        }
    } else {
        str_copy(p, path, sizeof(p));
    }
    strip_trailing_slash(p);
    normalize_abs(p);

    /* 最长前缀匹配：/mnt/docs 比 /mnt 更长就优先，但我们只允许挂载点本身做前缀，
     * 所以就是"在几个挂载点里挑最长的那个能匹配的" */
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        uint32_t len;
        if (g_mounts[i].used == 0u) {
            continue;
        }
        if (!prefix_match(p, g_mounts[i].prefix)) {
            continue;
        }
        len = 0u;
        while (g_mounts[i].prefix[len] != '\0') {
            ++len;
        }
        if (len > best_len) {
            best_len = len;
            best = i;
        }
    }
    if (best == VFS_MAX_MOUNTS) {
        return 0;                        /* 不在任何挂载点里 -> 交给 ramfs */
    }
    if (fs != (vfs_fs_t *)0) {
        *fs = g_mounts[best].fs;
    }
    if (out_path != (char *)0 && cap > 0u) {
        const char *rest = p + best_len;
        if (rest[0] == '\0') {
            str_copy(out_path, "/", cap);
        } else {
            str_copy(out_path, rest, cap);   /* rest 已以 '/' 开头 */
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 分发到具体文件系统                                                 */
/* ------------------------------------------------------------------ */

int vfs_list(const char *path)
{
    vfs_fs_t fs = VFS_FS_RAMFS;
    char sub[VFS_PATH_MAX];

    if (!vfs_resolve(path, &fs, sub, sizeof(sub))) {
        return VFS_ERR_NOENT;
    }
    switch (fs) {
    case VFS_FS_EXT2: {
        const int rc = ext2_list(sub);
        return (rc == EXT2_OK) ? VFS_OK : VFS_ERR_IO;
    }
    case VFS_FS_FAT16: {
        const int rc = fat16_list(sub);
        return (rc == FAT16_OK) ? VFS_OK : VFS_ERR_IO;
    }
    case VFS_FS_RAMFS:
    default:
        return VFS_ERR_NOENT;             /* 根 FS 由命令层直接交给 ramfs */
    }
}

int vfs_read(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen)
{
    vfs_fs_t fs = VFS_FS_RAMFS;
    char sub[VFS_PATH_MAX];

    if (!vfs_resolve(path, &fs, sub, sizeof(sub))) {
        return VFS_ERR_NOENT;
    }
    switch (fs) {
    case VFS_FS_EXT2: {
        uint64_t n = 0u;
        const int rc = ext2_read(sub, buf, maxlen, &n);
        if (rc != EXT2_OK) {
            return VFS_ERR_IO;
        }
        if (outlen != (uint64_t *)0) {
            *outlen = n;
        }
        return VFS_OK;
    }
    case VFS_FS_FAT16: {
        uint64_t n = 0u;
        const int rc = fat16_read(sub, buf, maxlen, &n);
        if (rc != FAT16_OK) {
            return VFS_ERR_IO;
        }
        if (outlen != (uint64_t *)0) {
            *outlen = n;
        }
        return VFS_OK;
    }
    case VFS_FS_RAMFS:
    default:
        return VFS_ERR_NOENT;
    }
}

int vfs_under_mount(const char *path)
{
    vfs_fs_t fs = VFS_FS_RAMFS;
    char sub[VFS_PATH_MAX];
    return vfs_resolve(path, &fs, sub, sizeof(sub));
}

/* ------------------------------------------------------------------ */
/* cwd                                                                */
/* ------------------------------------------------------------------ */

void vfs_set_cwd(const char *path)
{
    if (path == (const char *)0) {
        return;
    }
    str_copy(g_cwd, path, sizeof(g_cwd));
    strip_trailing_slash(g_cwd);
    if (g_cwd[0] == '\0') {
        str_copy(g_cwd, "/", sizeof(g_cwd));
    }
    g_cwd_in_mount = vfs_under_mount(g_cwd);
}

const char *vfs_get_cwd(void)
{
    return g_cwd;
}

int vfs_in_mount_cwd(void)
{
    return g_cwd_in_mount;
}

/* 把 cwd 与相对路径拼起来：跳过 "./"，".." 就砍掉 cwd 的最后一段 */
void vfs_join_cwd(char *out, uint32_t cap, const char *rel)
{
    uint32_t n = 0u;
    uint32_t i;
    uint32_t t;

    if (out == (char *)0 || cap == 0u) {
        return;
    }
    for (i = 0u; g_cwd[i] != '\0' && n + 1u < cap; ++i) {
        out[n++] = g_cwd[i];
    }
    uint32_t k = 0u;
    while (rel[k] != '\0') {
        /* 取出一个路径分量 */
        while (rel[k] == '/') {
            ++k;
        }
        if (rel[k] == '\0') {
            break;
        }
        char comp[64];
        uint32_t cl = 0u;
        while (rel[k] != '\0' && rel[k] != '/' && cl + 1u < sizeof(comp)) {
            comp[cl++] = rel[k++];
        }
        comp[cl] = '\0';
        if (cl == 0u || str_eq(comp, ".")) {
            continue;
        }
        if (str_eq(comp, "..")) {
            uint32_t last = 0u;
            for (t = 0u; t < n; ++t) {
                if (out[t] == '/') {
                    last = t;
                }
            }
            n = last;                     /* 砍到上一个 '/' */
            if (n == 0u) {
                continue;
            }
            continue;
        }
        if (n > 0u && out[n - 1u] != '/' && n + 1u < cap) {
            out[n++] = '/';
        }
        for (t = 0u; t < cl && n + 1u < cap; ++t) {
            out[n++] = comp[t];
        }
    }
    if (n == 0u) {
        out[n++] = '/';
    }
    out[n] = '\0';
}

/* ==================================================================== */
/* [ADD] M24：VFS 的写路径 —— touch / rm 落到挂载的文件系统             */
/*                                                                    */
/* M20 的 VFS 只会 list / read。写请求到不了 ext2，所以 `touch /mnt/x`  */
/* 一直在 ramfs 里造一个同名节点（而 ramfs 根本没有 /mnt 这一层）。      */
/* 这里补两个入口，形状照着 Linux 的 inode_operations：                */
/*      vfs_create(path, data, len)   ≈ ->create()                     */
/*      vfs_unlink(path)              ≈ ->unlink()                     */
/* 没匹配到挂载点就返回 VFS_ERR_NOENT，命令层照旧回落 ramfs —— 这条   */
/* 回落是 M20 定下的规矩，绝不让根文件系统的行为被改动。                */
/* ==================================================================== */

/* ext2 的错误码 -> VFS 错误码（命令层只需要知道"大概哪一类错"） */
static int ext2_to_vfs(int rc)
{
    switch (rc) {
    case EXT2_OK:          return VFS_OK;
    case EXT2_ERR_NOENT:   return VFS_ERR_NOENT;
    case EXT2_ERR_EXIST:   return VFS_ERR_EXIST;
    case EXT2_ERR_NOTDIR:  return VFS_ERR_NOTDIR;
    case EXT2_ERR_ISDIR:   return VFS_ERR_ISDIR;
    case EXT2_ERR_NOSPC:   return VFS_ERR_NOSPC;
    case EXT2_ERR_NOTMOUNTED: return VFS_ERR_NOENT;
    default:               return VFS_ERR_IO;
    }
}

/* 在挂载的文件系统上建一个文件。返回 VFS_OK / VFS_ERR_* */
int vfs_create(const char *path, const uint8_t *data, uint64_t len,
               uint32_t *out_ino)
{
    vfs_fs_t fs = VFS_FS_RAMFS;
    char sub[VFS_PATH_MAX];

    if (path == (const char *)0) {
        return VFS_ERR_INVAL;
    }
    if (!vfs_resolve(path, &fs, sub, sizeof(sub))) {
        return VFS_ERR_NOENT;         /* 不在挂载点里：交给 ramfs */
    }
    switch (fs) {
    case VFS_FS_EXT2: {
        uint32_t ino = 0u;
        int rc;
        /* 小文件走 ext2_create（12 个直接块就够），大的走间接块那条路。
         * 判断阈值用的是底层真实的 block_size，而不是写死的 12KB。 */
        if (len > (uint64_t)ext2_block_size() * 12u) {
            rc = ext2_create_big(sub, data, len, &ino, (uint32_t *)0,
                                 (uint32_t *)0);
        } else {
            rc = ext2_create(sub, data, len, &ino);
        }
        if (rc == EXT2_OK && out_ino != (uint32_t *)0) {
            *out_ino = ino;
        }
        return ext2_to_vfs(rc);
    }
    case VFS_FS_FAT16:
        /* FAT16 这条链路是只读的（M8 起就只做读），写请求直接拒绝 */
        return VFS_ERR_RO;
    case VFS_FS_RAMFS:
    default:
        return VFS_ERR_NOENT;
    }
}

/* 删掉挂载的文件系统上的一个文件。返回 VFS_OK / VFS_ERR_* */
int vfs_unlink(const char *path, uint32_t *out_freed)
{
    vfs_fs_t fs = VFS_FS_RAMFS;
    char sub[VFS_PATH_MAX];

    if (path == (const char *)0) {
        return VFS_ERR_INVAL;
    }
    if (!vfs_resolve(path, &fs, sub, sizeof(sub))) {
        return VFS_ERR_NOENT;
    }
    switch (fs) {
    case VFS_FS_EXT2: {
        uint32_t freed = 0u;
        const int rc = ext2_unlink(sub, (uint32_t *)0, &freed);
        if (rc == EXT2_OK && out_freed != (uint32_t *)0) {
            *out_freed = freed;
        }
        return ext2_to_vfs(rc);
    }
    case VFS_FS_FAT16:
        return VFS_ERR_RO;
    case VFS_FS_RAMFS:
    default:
        return VFS_ERR_NOENT;
    }
}

/* 判断一个路径"是不是落在挂载点里"，写路径用（避免重复 resolve） */
int vfs_write_target_is_mount(const char *path, vfs_fs_t *out_fs)
{
    vfs_fs_t fs = VFS_FS_RAMFS;
    char sub[VFS_PATH_MAX];

    if (!vfs_resolve(path, &fs, sub, sizeof(sub))) {
        return 0;
    }
    if (out_fs != (vfs_fs_t *)0) {
        *out_fs = fs;
    }
    return 1;
}
