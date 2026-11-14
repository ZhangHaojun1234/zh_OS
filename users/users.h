/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    users/users.h
 * Version: 0.1beta
 * Brief:   账户结构、全局会话指针与账户 API 声明。
 * ============================================================================ */

#ifndef ZH_OS_USERS_H
#define ZH_OS_USERS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 第 4 步：仿 Linux 的账户表（全部内存态）。
 *
 * 设计要点：
 *   - freestanding 环境没有 libc，也不解析 /etc/passwd，账户表是一块静态数组。
 *   - 只保存密码的 64 位 FNV-1a 哈希，绝不保存明文；FNV-1a 不是密码学哈希，
 *     这里仅作教学演示，真实系统应换成加盐的慢哈希。
 *   - zh_current_user 是全局会话指针：NULL 表示未登录，login.c 负责设置/清空。
 *   - 权限规则与 Linux 对齐：uid 0（root）可增删账户、改任何人密码；
 *     普通用户只能改自己的密码。
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
 * 校验用户名与密码：成功返回对应账户指针，失败返回 NULL。
 * 无论用户是否存在都会计算一次哈希，避免用响应时间探测账户名。
 */
zh_user_t *user_auth(const char *name, const char *password);

/*
 * 新增账户，仅 root 可用；重名返回 USER_ERR_EXISTS。
 * home / shell 传 NULL 或空串时自动生成 "/home/<name>" 与 "/bin/zhsh"。
 * password 不能为空。
 */
int user_add(const char *name, uint32_t uid, uint32_t gid,
             const char *home, const char *shell,
             const char *password, bool is_admin);

/*
 * 修改密码：name 为 NULL / 空串表示改自己。
 * 普通用户只能改自己；root 可改他人。返回 enum user_status。
 */
int user_set_password(const char *name, const char *password);

/* [ADD] 持久化桥接：供 users_persist.c 读写内存账户表（不动上面的认证/权限逻辑）。 */
zh_user_t *user_table_entry(uint32_t i);
int user_table_reset_and_load(const zh_user_t *src, uint32_t count);

#endif /* ZH_OS_USERS_H */
