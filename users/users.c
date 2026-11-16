/* users.c - 内存态账户表、FNV-1a 64 哈希、认证与权限 */

#include "users.h"

#include <stddef.h>

/*
 * 账户表的内存态实现（第 4 步）。
 *
 * TODO（本版未实现，可选任务 5）：ATA PIO 磁盘持久化，目标是让 useradd / passwd 的结果重启
 * 之后还在。drivers/ata.c 走 LBA28、主通道 0x1F0、PIO 模式：
 *      void ata_init(void);
 *      int  ata_read_sector (uint32_t lba, uint8_t *buf);     (512 字节)
 *      int  ata_write_sector(uint32_t lba, const uint8_t *buf);
 * 端口 0x1F0 数据、0x1F2 扇区数、0x1F3/1F4/1F5 是 LBA 低/中/高、0x1F6 驱动器加 LBA 高 4 位、
 * 0x1F7 状态和命令（0x20 读、0x30 写）。
 * 扇区固定用 LBA 1（避开 LBA 0，留给将来的分区表或引导记录），一个扇区 512 字节足够容纳
 * ZH_USER_MAX(16) 条账户记录。序列化格式每条一行 ASCII：
 *      name:uid:gid:home:shell:pass_hash_hex:is_admin\n
 * 末尾用空行当结束标记。读入时逐行解析并校验字段长度，任何格式错误都整体回退到内置账户表，
 * 绝不让内核拿到半张表。
 * 接口按计划写进 users.h：int users_save(void) 把 user_table 序列化后写 LBA 1，
 * int users_load(void) 读 LBA 1 重建 user_table。user_init() 先试 users_load()，失败再走现在
 * 的 user_insert() 内置账户；user_add() / user_set_password() 成功后调 users_save() 落盘。
 * user_init() 和 user_insert() 已经把载入和建表分成两层，到时候只换数据来源，认证和权限逻辑
 * 不用动。
 * 限制：从 ISO/CD 启动时介质只读，ata_write_sector() 没有可写目标或者直接写失败，持久化不会
 * 生效。得给虚拟机额外挂一块可写磁盘，比如
 *      qemu-system-x86_64 -cdrom zh_OS-1.0.iso
 *                         -drive file=disk.img,format=raw -m 128
 * 挂了可写磁盘这件事才有意义。
 * 本版不做的理由是交付约束里那两条：不新增无关依赖、不改已验收的交互行为，而且 ISO 启动场景
 * 下没法验证写盘。现在全是内存态，新加的账户、passwd 改过的密码重启都会丢，密码复位成 "zh"。
 */

#define FNV_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME        0x100000001b3ULL
#define DEFAULT_PASSWORD "zh"
#define DEFAULT_SHELL    "/bin/zhsh"

/* 全局会话指针，定义在 .bss，初值即 NULL。 */
zh_user_t *zh_current_user;

static zh_user_t user_table[ZH_USER_MAX];
static uint32_t user_table_count;
static int users_initialized;

/* 本地字符串工具（freestanding 无 libc；第 5 步可抽成独立模块复用）    */

static uint32_t str_length(const char *s)
{
    uint32_t n = 0;
    if (s == 0) {
        return 0;
    }
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static int str_equal(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* 带容量上限的拷贝，保证 '\0' 结尾；cap 为数组总字节数。 */
static void str_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    if (src == 0 || cap == 0) {
        if (cap != 0) {
            dst[0] = '\0';
        }
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void str_append(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    uint32_t j = 0;

    if (src == 0 || cap == 0) {
        return;
    }
    while (dst[i] != '\0') {
        ++i;
    }
    while (src[j] != '\0' && i + 1 < cap) {
        dst[i++] = src[j++];
    }
    dst[i] = '\0';
}

/* FNV-1a 64 位：先异或字节再乘素数，与 32 位版本的偏移基/素数不同。 */
static uint64_t fnv1a64(const char *s)
{
    uint64_t hash = FNV_OFFSET_BASIS;

    if (s == 0) {
        return hash;
    }
    while (*s != '\0') {
        hash ^= (uint64_t)(unsigned char)*s++;
        hash *= FNV_PRIME;
    }
    return hash;
}

/* 账户表                                                             */

/*
 * 内部插入函数，不做权限检查，给 user_init() 载入内置账户用。公开的 user_add() 在它外面加
 * 一层 root 权限校验。
 */
static int user_insert(const char *name, uint32_t uid, uint32_t gid,
                       const char *home, const char *shell,
                       const char *password, bool is_admin)
{
    if (name == 0 || name[0] == '\0' ||
        str_length(name) >= ZH_USER_NAME_MAX) {
        return USER_ERR_INVALID;
    }
    if (password == 0 || password[0] == '\0' ||
        str_length(password) >= ZH_PASSWORD_MAX) {
        return USER_ERR_INVALID;
    }
    if (user_find(name) != 0) {
        return USER_ERR_EXISTS;
    }
    if (user_table_count >= ZH_USER_MAX) {
        return USER_ERR_FULL;
    }

    zh_user_t *u = &user_table[user_table_count++];
    str_copy(u->name, name, ZH_USER_NAME_MAX);
    u->uid = uid;
    u->gid = gid;

    if (home != 0 && home[0] != '\0') {
        str_copy(u->home, home, ZH_USER_HOME_MAX);
    } else {
        char default_home[ZH_USER_HOME_MAX];
        str_copy(default_home, "/home/", ZH_USER_HOME_MAX);
        str_append(default_home, name, ZH_USER_HOME_MAX);
        str_copy(u->home, default_home, ZH_USER_HOME_MAX);
    }

    if (shell != 0 && shell[0] != '\0') {
        str_copy(u->shell, shell, ZH_USER_SHELL_MAX);
    } else {
        str_copy(u->shell, DEFAULT_SHELL, ZH_USER_SHELL_MAX);
    }

    u->pass_hash = fnv1a64(password);
    u->is_admin = is_admin;
    return USER_OK;
}

void user_init(void)
{
    if (users_initialized) {
        return;
    }
    users_initialized = 1;
    user_table_count = 0;
    zh_current_user = 0;

    /* 内置账户：两个账户的初始密码都是 "zh"，登录后可临时用 passwd 改。 */
    (void)user_insert("root", 0u, 0u, "/root", DEFAULT_SHELL,
                      DEFAULT_PASSWORD, true);
    (void)user_insert("zh", 1000u, 1000u, "/home/zh", DEFAULT_SHELL,
                      DEFAULT_PASSWORD, false);
}

zh_user_t *user_find(const char *name)
{
    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    for (uint32_t i = 0; i < user_table_count; ++i) {
        if (str_equal(user_table[i].name, name)) {
            return &user_table[i];
        }
    }
    return 0;
}

zh_user_t *user_current(void)
{
    return zh_current_user;
}

bool user_is_root(void)
{
    return zh_current_user != 0 &&
           (zh_current_user->uid == 0u || zh_current_user->is_admin);
}

uint32_t user_count(void)
{
    return user_table_count;
}

zh_user_t *user_auth(const char *name, const char *password)
{
    /* 先算哈希再查账户，用户不存在时也走完同样的计算路径。 */
    const uint64_t hash = fnv1a64(password);
    zh_user_t *u = user_find(name);

    if (u == 0 || password == 0 || password[0] == '\0') {
        return 0;
    }
    if (u->pass_hash != hash) {
        return 0;
    }
    return u;
}

int user_add(const char *name, uint32_t uid, uint32_t gid,
             const char *home, const char *shell,
             const char *password, bool is_admin)
{
    /* 仅 root 可以新增账户；未登录时 zh_current_user 为 NULL，同样拒绝。 */
    if (!user_is_root()) {
        return USER_ERR_PERMISSION;
    }
    return user_insert(name, uid, gid, home, shell, password, is_admin);
}

int user_set_password(const char *name, const char *password)
{
    if (zh_current_user == 0) {
        return USER_ERR_PERMISSION;
    }

    const char *target_name =
        (name == 0 || name[0] == '\0') ? zh_current_user->name : name;
    zh_user_t *target = user_find(target_name);
    if (target == 0) {
        return USER_ERR_NOT_FOUND;
    }

    /* 普通用户只能改自己；root（uid 0 / is_admin）可以改任何人。 */
    if (!user_is_root() && target != zh_current_user) {
        return USER_ERR_PERMISSION;
    }

    if (password == 0 || password[0] == '\0' ||
        str_length(password) >= ZH_PASSWORD_MAX) {
        return USER_ERR_INVALID;
    }

    target->pass_hash = fnv1a64(password);
    return USER_OK;
}

/* [ADD] 持久化桥接：供 users_persist.c 读写内存账户表（不动上面任何逻辑）。 */

/* 取第 i 个账户（i >= 当前数量返回 NULL），供 users_save() 序列化。 */
zh_user_t *user_table_entry(uint32_t i)
{
    if (i >= user_table_count) {
        return 0;
    }
    return &user_table[i];
}

/*
 * 用外部（磁盘载入）的账户数组整体替换内存账户表。清空计数和当前会话，逐条拷贝，包括预计算
 * 好的 pass_hash，不再重算哈希。校验失败要不要回退由调用方决定，本函数只在参数合法时覆盖。
 */
int user_table_reset_and_load(const zh_user_t *src, uint32_t count)
{
    if (src == 0 || count == 0u || count > ZH_USER_MAX) {
        return USER_ERR_INVALID;
    }
    user_table_count = 0;
    zh_current_user = 0;
    for (uint32_t i = 0; i < count; ++i) {
        zh_user_t *u = &user_table[user_table_count];
        str_copy(u->name, src[i].name, ZH_USER_NAME_MAX);
        u->uid = src[i].uid;
        u->gid = src[i].gid;
        str_copy(u->home, src[i].home, ZH_USER_HOME_MAX);
        str_copy(u->shell, src[i].shell, ZH_USER_SHELL_MAX);
        u->pass_hash = src[i].pass_hash;
        u->is_admin = src[i].is_admin;
        ++user_table_count;
    }
    return USER_OK;
}
