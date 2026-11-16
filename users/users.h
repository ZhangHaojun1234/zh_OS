/* users.h - 账户结构、全局会话指针、账户 API */

#ifndef ZH_OS_USERS_H
#define ZH_OS_USERS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 第 4 步：仿 Linux 的账户表，全是内存态。freestanding 环境没有 libc，也不解析 /etc/passwd，
 * 账户表就是一块静态数组。只存密码的 64 位 FNV-1a 哈希，不存明文；FNV-1a 不是密码学哈希，
 * 这里只作教学演示，真实系统得换成加盐的慢哈希。zh_current_user 是全局会话指针，NULL 表示
 * 未登录，login.c 负责设置和清空。权限规则跟 Linux 对齐：uid 0（root）能增删账户、改任何人
 * 的密码，普通用户只能改自己的。
 */

#define ZH_USER_NAME_MAX   32u
#define ZH_USER_HOME_MAX   64u
#define ZH_USER_SHELL_MAX  32u
#define ZH_PASSWORD_MAX    64u   /* 输入缓冲上限，含结尾 '\0' */
#define ZH_USER_MAX        16u   /* 静态账户表容量 */

typedef struct {
    char name[32];
    uint32_t uid;
    uint32_t gid;
    char home[64];
    char shell[32];
    uint64_t pass_hash;   /* 64 位下用 64 位哈希 */
    bool is_admin;
} zh_user_t;

/* 返回码：0 成功，负数为错误（不使用 errno，freestanding 无 libc） */
enum user_status {
    USER_OK = 0,
    USER_ERR_INVALID = -1,    /* 空名 / 空密码 / 超长字段 */
    USER_ERR_PERMISSION = -2, /* 非 root 越权，或没有登录会话 */
    USER_ERR_NOT_FOUND = -3,  /* 账户不存在 */
    USER_ERR_EXISTS = -4,     /* 重名 */
    USER_ERR_FULL = -5        /* 账户表已满 */
};

/* 全局当前会话；NULL 表示未登录。login.c 直接读写，Shell 通过 user_current() 只读访问。 */
extern zh_user_t *zh_current_user;

/* 初始化内置账户表并清空会话；幂等，可在 kernel_main 中调用一次。 */
void user_init(void);

/* 按用户名查找，找不到返回 NULL。 */
zh_user_t *user_find(const char *name);

/* 当前登录用户（未登录返回 NULL），以及是否为 root（uid 0 或 is_admin）。 */
zh_user_t *user_current(void);
bool user_is_root(void);

/* 已加载的账户数量，供登录横幅 / 未来的 who 命令使用。 */
uint32_t user_count(void);

/*
 * 校验用户名和密码，成功返回对应账户指针，失败返回 NULL。不管用户存不存在都算一次哈希，
 * 免得用响应时间探出账户名有没有。
 */
zh_user_t *user_auth(const char *name, const char *password);

/*
 * 新增账户，只有 root 能用，重名返回 USER_ERR_EXISTS。home / shell 传 NULL 或空串时自动给
 * "/home/<name>" 和 "/bin/zhsh"。password 不能为空。
 */
int user_add(const char *name, uint32_t uid, uint32_t gid,
             const char *home, const char *shell,
             const char *password, bool is_admin);

/*
 * 改密码，name 为 NULL 或空串表示改自己。普通用户只能改自己，root 能改别人。返回
 * enum user_status。
 */
int user_set_password(const char *name, const char *password);

/* [ADD] 持久化桥接：供 users_persist.c 读写内存账户表（不动上面的认证/权限逻辑）。 */
zh_user_t *user_table_entry(uint32_t i);
int user_table_reset_and_load(const zh_user_t *src, uint32_t count);

#endif /* ZH_OS_USERS_H */
