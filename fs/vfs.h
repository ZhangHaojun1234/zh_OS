/* vfs.h - 最小可用的 VFS：挂载表加最长前缀解析加 cwd，把三套文件系统塞进同一个命名空间。
 * Linux 那边是 super_block / mount / vfsmount / dentry 一堆，这里只留真正起作用的两样：
 * 挂载表 { 挂载点前缀, 文件系统种类, 该 FS 的 list/read }；
 * 路径解析：/mnt/docs/note.txt 先最长前缀匹配到 /mnt，交给 ext2，剩下 /docs/note.txt。
 * 没匹配上的原样交回 ramfs，所以 mount 表空着时 ls / cd / cat 走的还是 M1-M18 那条老路。 */

#ifndef ZH_OS_FS_VFS_H
#define ZH_OS_FS_VFS_H

#include <stdint.h>

#define VFS_MAX_MOUNTS   4u
#define VFS_PREFIX_MAX   32u
#define VFS_PATH_MAX     192u

/* 返回码 */
#define VFS_OK           0
#define VFS_ERR_FULL     (-1)   /* 挂载表满了 */
#define VFS_ERR_NOENT    (-2)   /* 没有这个挂载点 */
#define VFS_ERR_EXIST    (-3)   /* 这个挂载点已经挂上了 */
#define VFS_ERR_NOTDIR   (-4)   /* 不是目录 */
#define VFS_ERR_INVAL    (-5)   /* 参数非法 */
#define VFS_ERR_IO       (-6)   /* 底层文件系统报错（透传） */
#define VFS_ERR_ISDIR    (-7)   /* [ADD] M24：是个目录 */
#define VFS_ERR_NOSPC    (-8)   /* [ADD] M24：没有空间（位图分配失败） */
#define VFS_ERR_RO       (-9)   /* [ADD] M24：该文件系统只读（FAT16） */

/* 文件系统种类：决定 list/read 调哪套实现 */
typedef enum {
    VFS_FS_RAMFS = 0,      /* 根文件系统（内存态，走 ramfs.c） */
    VFS_FS_EXT2  = 1,
    VFS_FS_FAT16 = 2
} vfs_fs_t;

typedef struct {
    char     prefix[VFS_PREFIX_MAX];   /* 挂载点，形如 "/mnt"（无尾斜杠） */
    vfs_fs_t fs;
    uint32_t used;
} vfs_mount_t;

/* 挂载：prefix 必须形如 "/mnt"（绝对路径、无尾斜杠）。
 * 只登记，不负责把底层 FS 挂上来（ext2mount / fatmount 已经做过了）。 */
int vfs_mount(const char *prefix, vfs_fs_t fs);

/* 卸载某个挂载点 */
int vfs_umount(const char *prefix);

/* 清空挂载表（开机时由 ramfs_init 之后调一次；这里不自动调用） */
void vfs_reset(void);

/* 枚举挂载表：index 从 0 到 vfs_mount_count()-1，取出一项（未用的返回 NULL）。 */
const vfs_mount_t *vfs_mount_at(uint32_t index);
uint32_t vfs_mount_count(void);

/* 最长前缀匹配：把 path 拆成 {挂载项, 该 FS 内的路径}。
 * 返回 1 = 匹配到某个挂载点（*fs 与 out_path 有效），0 = 没有挂载点匹配。
 * out_path 以 '/' 开头，挂载点本身对应 "/"。 */
int vfs_resolve(const char *path, vfs_fs_t *fs, char *out_path, uint32_t cap);

/* 列目录 / 读文件：先 resolve，匹配到挂载点就调对应 FS 的实现，
 * 没匹配上则返回 VFS_ERR_NOENT（调用方应当回落到 ramfs）。 */
int vfs_list(const char *path);
int vfs_read(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen);

/* 判断 path 是否落在某个挂载点里（cd 用） */
int vfs_under_mount(const char *path);

/* cwd：绝对路径字符串。非挂载路径时它只是 ramfs cwd 的镜像（由 cmd_cd 同步），
 * 落在挂载点里时由 VFS 自己持有（ramfs 那边没有 /mnt 这个目录）。 */
void vfs_set_cwd(const char *path);
const char *vfs_get_cwd(void);
int vfs_in_mount_cwd(void);      /* 1 = 当前 cwd 在某个挂载点里 */

/* 把 "cwd" 和相对路径 rel 拼成绝对路径（处理 "./" 与 ".."），写进 out。 */
void vfs_join_cwd(char *out, uint32_t cap, const char *rel);

/* Shell 命令：mount / umount / mounts                               */
int cmd_mount(int argc, char **argv);
int cmd_umount(int argc, char **argv);
int cmd_mounts(int argc, char **argv);


/* [ADD] M24：写路径。没匹配到挂载点返回 VFS_ERR_NOENT（调用方回落 ramfs） */
int vfs_create(const char *path, const uint8_t *data, uint64_t len,
               uint32_t *out_ino);
int vfs_unlink(const char *path, uint32_t *out_freed);
int vfs_write_target_is_mount(const char *path, vfs_fs_t *out_fs);
#endif /* ZH_OS_FS_VFS_H */
