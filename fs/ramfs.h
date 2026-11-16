/* ramfs.h - 内存文件系统的接口，顺带声明七个命令（ls/cd/pwd/cat/mkdir/touch/rm）。
 * 全在内存里，kmalloc 分配，重启就复位。路径认绝对 /a/b、相对 a/b，以及 ./、..、~
 * （展开成当前用户 home，没有 home 就回退 /）。
 * 权限：root 或还没登录（user_current() 为 NULL）时哪儿都能建能删，普通用户只在自己 home 子树里。
 * 错误码由命令层负责打印成红字。 */

#ifndef ZH_OS_FS_RAMFS_H
#define ZH_OS_FS_RAMFS_H

#include <stdint.h>

/* 节点与错误码                                                       */

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

/* 初始化与路径解析                                                   */

/* 创建根 "/" 与 /root、/home、/home/zh 三个目录，并把 cwd 设为根。
 * 必须在 heap_init() 之后、任何 Shell 启动之前调用一次。 */
void ramfs_init(void);

/* 把显示用的 cwd 字符串注册给 Shell 提示符：提示符会改用真实路径而非 "~"。 */
void shell_set_cwd_fn(const char *(*fn)(void));

/* 对外接口（全部 kmalloc 分配，不链接 libc）                         */

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
/* [ADD] M9：fd 表需要的"打开检查 / 取大小 / 按偏移读"三件套。
 * 与 ramfs_read_file 的区别：这三个不复制整份文件，读偏移由调用方（fd 表）持有。 */
int ramfs_open_check(const char *path, ramfs_node_t **out);
uint64_t ramfs_node_size(const ramfs_node_t *node);
int ramfs_node_read_at(ramfs_node_t *node, uint64_t off, uint8_t *buf,
                       uint64_t maxlen, uint64_t *outlen);

/* [ADD] M10：可写文件 ， creat（截断/新建）与追加写。 */
int ramfs_create_file(const char *path, ramfs_node_t **out);
int ramfs_node_append(ramfs_node_t *node, const uint8_t *buf, uint64_t len);

/* [ADD] M14：按序号枚举目录（getdents 的雏形），给用户态 ls 用。 */
int ramfs_readdir(const char *path, uint32_t index, char *name, uint32_t cap,
                  ramfs_type_t *type);

int ramfs_chdir(const char *path);

/* 返回显示用 cwd 路径（静态缓冲，下次调用覆盖）：
 * 非 root 且位于自己 home 时返回 "~" 或 "~/xxx"；root 与越出 home 时返回绝对路径。 */
const char *ramfs_get_cwd(void);

/* 把错误码映射为可读英文串（供命令层拼红字）。 */
const char *ramfs_strerror(int rc);

/* 基于 ramfs 的 Shell 命令（命令表末尾追加注册）                     */

int cmd_ls(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_rm(int argc, char **argv);

#endif /* ZH_OS_FS_RAMFS_H */
