/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    fs/ramfs.h
 * Version: 0.1beta
 * Brief:   ramfs 内存态文件系统接口（节点结构、路径解析、权限判断与命令原型）。
 *
 * 设计要点（B 组第 1 个功能，纯增量「只添加，不修改」）：
 *   - 全部内存态：节点与文件内容都用 kmalloc 分配，重启即复位，不落盘。
 *   - 路径支持绝对 /a/b、相对 a/b，以及 ./、..、~（展开为当前用户 home，
 *     若 home 不存在回退 /）。
 *   - 权限：root（或 init 阶段 user_current()==NULL）可在任意位置创建/删除；
 *     普通用户只能在自己 home 及其子树内操作。命令层负责把错误码打印成红字。
 *   - 本头文件同时声明基于 ramfs 的 7 个 Shell 命令（ls/cd/pwd/cat/mkdir/
 *     touch/rm），供 commands.c 的命令表注册使用。
 * ============================================================================ */

#ifndef ZH_OS_FS_RAMFS_H
#define ZH_OS_FS_RAMFS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* 节点与错误码                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    RAMFS_FILE,
    RAMFS_DIR
} ramfs_type_t;

typedef struct ramfs_node {
    char name[64];                 /* 节点名（不含路径分隔符） */
    ramfs_type_t type;             /* 文件 / 目录 */
    uint64_t size;                 /* 文件字节数；目录恒为 0 */
    uint8_t *data;                 /* 文件内容，kmalloc 分配；目录为 NULL */
    struct ramfs_node *parent;     /* 父目录；根节点的 parent 为 NULL */
    struct ramfs_node *children;   /* 目录子节点单向链表（头插） */
    struct ramfs_node *next;       /* 同级下一个 */
} ramfs_node_t;

/* 返回码：0 成功，负数为错误（不使用 errno，freestanding 无 libc） */
#define RAMFS_OK          0
#define RAMFS_ERR_NOENT   -1   /* 路径不存在 */
#define RAMFS_ERR_EXIST   -2   /* 已存在同名节点 */
#define RAMFS_ERR_NOTDIR  -3   /* 路径中段不是目录 */
#define RAMFS_ERR_PERM    -4   /* 权限不足（普通用户越出 home 子树） */
#define RAMFS_ERR_NOTEMPTY -5  /* 目录非空，不能删除 */
#define RAMFS_ERR_ISDIR   -6   /* 期望文件却遇到目录 */
#define RAMFS_ERR_INVAL   -7   /* 参数非法（如路径为空、组件过多） */
#define RAMFS_ERR_NOMEM   -8   /* kmalloc 失败 */

/* ------------------------------------------------------------------ */
/* 初始化与路径解析                                                   */
/* ------------------------------------------------------------------ */

/* 创建根 "/" 与 /root、/home、/home/zh 三个目录，并把 cwd 设为根。
 * 必须在 heap_init() 之后、任何 Shell 启动之前调用一次。 */
void ramfs_init(void);

/* 把显示用的 cwd 字符串注册给 Shell 提示符：提示符会改用真实路径而非 "~"。 */
void shell_set_cwd_fn(const char *(*fn)(void));

/* ------------------------------------------------------------------ */
/* 对外接口（全部 kmalloc 分配，不链接 libc）                         */
/* ------------------------------------------------------------------ */

/* 创建目录：0 成功，负数错误码。parent 不在 home 子树内（普通用户）返回 PERM。 */
int ramfs_mkdir(const char *path);

/* 创建空文件（size=0, data=NULL）：0 成功，负数错误码。 */
int ramfs_touch(const char *path);

/* 删除节点：只删空文件或空目录（目录有子节点返回 NOTEMPTY）。 */
int ramfs_rm(const char *path);

/* 查找节点：成功返回节点指针，不存在返回 NULL。不做权限裁剪（底层查找）。 */
ramfs_node_t *ramfs_lookup(const char *path);

/* 写文件：不存在则创建；已存在且为目录返回 ISDIR；自动 (k)malloc 内容缓冲。 */
int ramfs_write_file(const char *path, const uint8_t *buf, uint64_t len);

/* 读文件：写入 buf 最多 maxlen 字节，*outlen 返回实际读取数；目录返回 ISDIR。 */
int ramfs_read_file(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen);

/* 切换当前目录：目标须为目录且（普通用户）须在 home 子树内。 */
int ramfs_chdir(const char *path);

/* 返回显示用 cwd 路径（静态缓冲，下次调用覆盖）：
 * 非 root 且位于自己 home 时返回 "~" 或 "~/xxx"；root 与越出 home 时返回绝对路径。 */
const char *ramfs_get_cwd(void);

/* 把错误码映射为可读英文串（供命令层拼红字）。 */
const char *ramfs_strerror(int rc);

/* ------------------------------------------------------------------ */
/* 基于 ramfs 的 Shell 命令（命令表末尾追加注册）                     */
/* ------------------------------------------------------------------ */

int cmd_ls(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_rm(int argc, char **argv);

#endif /* ZH_OS_FS_RAMFS_H */
