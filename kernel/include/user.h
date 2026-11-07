/* ---------------------------------------------------------------
 * 用户账户系统
 *
 * 参考 Linux 的 /etc/passwd + /etc/shadow 模型:
 *   - 每个用户有 uid / gid、家目录、登录 shell
 *   - 口令不保存明文, 只保存 "盐 + 迭代 SHA-256" 的摘要
 *   - 数据库落在磁盘固定扇区, 双槽交替写入, 掉电不会写坏旧数据
 * --------------------------------------------------------------- */
#ifndef ZH_USER_H
#define ZH_USER_H

#include "types.h"
#include "sha256.h"

#define USER_NAME_MAX        32
#define USER_HOME_MAX        32
#define USER_SHELL_MAX       16
#define USER_SALT_SIZE       16
#define USER_MAX             16

#define UID_ROOT             0
#define GID_ROOT             0
#define GID_USERS            100

/* 口令派生强度: 迭代次数越多越慢。1000 次在 QEMU 上约 0.1 秒。 */
#define USER_PASSWORD_ROUNDS 1000

struct user_account {
    char name[USER_NAME_MAX];
    u32  uid;
    u32  gid;
    u8   password_hash[SHA256_DIGEST_SIZE];
    u8   salt[USER_SALT_SIZE];
    u32  rounds;
    u32  active;
    char home[USER_HOME_MAX];
    char shell[USER_SHELL_MAX];
} PACKED;

struct user_session {
    int  logged_in;
    u32  uid;
    u32  gid;
    char name[USER_NAME_MAX];
    char home[USER_HOME_MAX];
};

/* 初始化: 从磁盘加载, 没有有效数据库时创建默认账户 */
int  user_init(void);
int  user_save(void);
int  user_is_persistent(void);
const char *user_store_error(void);

int  user_count(void);
struct user_account *user_at(int index);
struct user_account *user_find(const char *name);
struct user_account *user_find_uid(u32 uid);

int  user_add(const char *name, const char *password, u32 uid, u32 gid,
              const char *home, const char *shell);
int  user_remove(const char *name);
u32  user_next_uid(void);
int  user_set_password(struct user_account *user, const char *password);
int  user_check_password(const struct user_account *user, const char *password);

/* 会话 */
void user_session_clear(struct user_session *session);
int  user_login(struct user_session *session, const char *name,
                const char *password);
int  user_is_root(const struct user_session *session);
const char *user_group_name(u32 gid);

/* 口令强度检查: 返回 NULL 表示通过, 否则返回原因 */
const char *user_password_policy(const char *name, const char *password);

#endif
