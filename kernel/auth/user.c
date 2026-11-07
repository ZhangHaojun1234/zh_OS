#include "user.h"
#include "ata.h"
#include "console.h"
#include "printf.h"
#include "sha256.h"
#include "string.h"

/* ------------------------------------------------------------------ */
/* 磁盘布局                                                            */
/*                                                                     */
/*   LBA 4096 : 槽 A                                                   */
/*   LBA 4097 : 槽 B                                                   */
/*                                                                     */
/* 两个槽交替写入: 新数据写进"非当前"的槽, 写完刷新, 下次再看哪一槽的   */
/* 序号更大就用哪一槽。这样即使写到一半断电, 也总有一个完整的旧副本。   */
/* ------------------------------------------------------------------ */
#define USER_LBA_SLOT_A   4096u
#define USER_LBA_SLOT_B   4097u

#define USER_MAGIC        0x5A485553u     /* 'ZHUS' */
#define USER_VERSION      1u

struct PACKED user_store_header {
    u32 magic;
    u32 version;
    u32 sequence;
    u32 count;
    u32 checksum;
    u32 reserved;
};

struct PACKED user_store {
    struct user_store_header header;
    struct user_account accounts[USER_MAX];
};

static struct user_store store_a;
static struct user_store store_b;

static struct user_store *active_store;
static u32 active_lba;

static int persistent;
static const char *store_error = "not initialised";

/* ------------------------------------------------------------------ */
/* CRC32: 用来判断槽是否损坏                                           */
/* ------------------------------------------------------------------ */
static u32 crc32(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;

    for (u32 i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static u32 store_checksum(const struct user_store *store)
{
    /* 校验头之后的所有内容, 头部自己的 checksum 字段不参与 */
    const u8 *base = (const u8 *)&store->accounts;
    u32 len = (u32)(sizeof(store->accounts));
    return crc32(base, len);
}

/* ------------------------------------------------------------------ */
/* 口令派生: SHA-256(盐 || 口令) 迭代 rounds 次                          */
/* ------------------------------------------------------------------ */
static void derive_password(const char *password, const u8 *salt, u32 rounds,
                            u8 out[SHA256_DIGEST_SIZE])
{
    struct sha256_ctx ctx;
    u8 block[SHA256_DIGEST_SIZE];
    u32 len = (u32)strlen(password);

    sha256_init(&ctx);
    sha256_update(&ctx, salt, USER_SALT_SIZE);
    sha256_update(&ctx, password, len);
    sha256_final(&ctx, block);

    for (u32 i = 1; i < rounds; i++) {
        sha256_init(&ctx);
        sha256_update(&ctx, block, SHA256_DIGEST_SIZE);
        sha256_update(&ctx, salt, USER_SALT_SIZE);
        sha256_update(&ctx, password, len);
        sha256_final(&ctx, block);
    }
    memcpy(out, block, SHA256_DIGEST_SIZE);
}

static void random_salt(u8 *salt)
{
    /* 没有硬件随机源, 用 TSC + 计数器混合出一个够用的盐 */
    static u32 counter;
    u32 value;

    for (u32 i = 0; i < USER_SALT_SIZE; i += 4) {
        u32 lo, hi;

        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        value = lo ^ (hi + 0x9E3779B9u) ^ (counter++ * 2654435761u);
        value ^= value >> 13;
        value *= 0x85EBCA6Bu;
        value ^= value >> 16;

        salt[i + 0] = (u8)(value & 0xFF);
        salt[i + 1] = (u8)((value >> 8) & 0xFF);
        salt[i + 2] = (u8)((value >> 16) & 0xFF);
        salt[i + 3] = (u8)((value >> 24) & 0xFF);
    }
}

/* ------------------------------------------------------------------ */
/* 校验                                                               */
/* ------------------------------------------------------------------ */
static int store_valid(const struct user_store *store)
{
    if (store->header.magic != USER_MAGIC) {
        return 0;
    }
    if (store->header.version != USER_VERSION) {
        return 0;
    }
    if (store->header.count > USER_MAX) {
        return 0;
    }
    if (store->header.checksum != store_checksum(store)) {
        return 0;
    }
    return 1;
}

static void store_reset(struct user_store *store)
{
    memset(store, 0, sizeof(*store));
    store->header.magic = USER_MAGIC;
    store->header.version = USER_VERSION;
    store->header.sequence = 0;
    store->header.count = 0;
}

/* ------------------------------------------------------------------ */
/* 持久化                                                             */
/* ------------------------------------------------------------------ */
int user_save(void)
{
    if (!persistent || !active_store) {
        store_error = "no writable disk";
        return -1;
    }

    /* 写到另一个槽, 让当前槽保持完整 */
    u32 target_lba = (active_lba == USER_LBA_SLOT_A) ? USER_LBA_SLOT_B
                                                    : USER_LBA_SLOT_A;
    struct user_store *target = (active_lba == USER_LBA_SLOT_A) ? &store_b
                                                               : &store_a;

    memcpy(target, active_store, sizeof(*target));
    target->header.sequence = active_store->header.sequence + 1;
    target->header.checksum = store_checksum(target);

    if (ata_write_sector(target_lba, target) != 0) {
        store_error = "disk write failed";
        return -1;
    }
    if (ata_flush() != 0) {
        store_error = "disk flush failed";
        return -1;
    }

    active_store = target;
    active_lba = target_lba;
    store_error = "ok";
    return 0;
}

int user_is_persistent(void)
{
    return persistent;
}

const char *user_store_error(void)
{
    return store_error;
}

/* ------------------------------------------------------------------ */
/* 默认账户                                                           */
/* ------------------------------------------------------------------ */
static int create_default_accounts(void)
{
    store_reset(active_store);

    if (user_add("root", "root", UID_ROOT, GID_ROOT, "/root", "/bin/sh") != 0) {
        return -1;
    }
    if (user_add("guest", "guest", 1000, GID_USERS, "/home/guest",
                 "/bin/sh") != 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */
int user_init(void)
{
    persistent = 0;
    active_store = &store_a;
    active_lba = USER_LBA_SLOT_A;
    store_error = "empty";

    int have_a = 0;
    int have_b = 0;

    if (ata_present() && ata_read_sector(USER_LBA_SLOT_A, &store_a) == 0) {
        have_a = store_valid(&store_a);
    } else {
        memset(&store_a, 0, sizeof(store_a));
    }

    if (ata_present() && ata_read_sector(USER_LBA_SLOT_B, &store_b) == 0) {
        have_b = store_valid(&store_b);
    } else {
        memset(&store_b, 0, sizeof(store_b));
    }

    if (have_a && have_b) {
        /* 两边都有效, 用序号大的 (较新) */
        if (store_b.header.sequence > store_a.header.sequence) {
            memcpy(&store_a, &store_b, sizeof(store_a));
            active_lba = USER_LBA_SLOT_B;
        } else {
            active_lba = USER_LBA_SLOT_A;
        }
        active_store = &store_a;
        persistent = 1;
        store_error = "loaded";
        return 0;
    }

    if (have_a) {
        active_lba = USER_LBA_SLOT_A;
        active_store = &store_a;
        persistent = 1;
        store_error = "loaded";
        return 0;
    }

    if (have_b) {
        memcpy(&store_a, &store_b, sizeof(store_a));
        active_lba = USER_LBA_SLOT_B;
        active_store = &store_a;
        persistent = 1;
        store_error = "loaded";
        return 0;
    }

    /* 没有可用数据库: 建一个默认的 */
    if (ata_present()) {
        persistent = 1;
    }
    if (create_default_accounts() != 0) {
        persistent = 0;
        store_error = "cannot create accounts";
        return -1;
    }

    if (persistent && user_save() != 0) {
        /* 磁盘写不进去, 退化为只读内存数据库 */
        persistent = 0;
        store_error = "read-only";
        return 0;
    }
    store_error = "created";
    return 0;
}

/* ------------------------------------------------------------------ */
/* 查询                                                               */
/* ------------------------------------------------------------------ */
int user_count(void)
{
    if (!active_store) {
        return 0;
    }
    return (int)active_store->header.count;
}

struct user_account *user_at(int index)
{
    if (!active_store || index < 0 || index >= (int)active_store->header.count) {
        return NULL;
    }
    return &active_store->accounts[index];
}

struct user_account *user_find(const char *name)
{
    for (int i = 0; i < user_count(); i++) {
        if (!strcmp(active_store->accounts[i].name, name)) {
            return &active_store->accounts[i];
        }
    }
    return NULL;
}

struct user_account *user_find_uid(u32 uid)
{
    for (int i = 0; i < user_count(); i++) {
        if (active_store->accounts[i].uid == uid) {
            return &active_store->accounts[i];
        }
    }
    return NULL;
}

const char *user_group_name(u32 gid)
{
    switch (gid) {
    case GID_ROOT:
        return "root";
    case GID_USERS:
        return "users";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* 口令                                                               */
/* ------------------------------------------------------------------ */
int user_set_password(struct user_account *user, const char *password)
{
    if (!user) {
        return -1;
    }

    random_salt(user->salt);
    user->rounds = USER_PASSWORD_ROUNDS;
    derive_password(password, user->salt, user->rounds, user->password_hash);
    return 0;
}

int user_check_password(const struct user_account *user, const char *password)
{
    u8 digest[SHA256_DIGEST_SIZE];

    if (!user || !user->active) {
        return 0;
    }

    derive_password(password, user->salt, user->rounds, digest);
    return memcmp(digest, user->password_hash, SHA256_DIGEST_SIZE) == 0;
}

const char *user_password_policy(const char *name, const char *password)
{
    size_t len;
    int has_alpha = 0;
    int has_digit = 0;

    if (!password || !password[0]) {
        return "password must not be empty";
    }

    len = strlen(password);
    if (len < 4) {
        return "password must be at least 4 characters";
    }
    if (len > 64) {
        return "password must be at most 64 characters";
    }
    if (name && strlen(name) > 2 && !strcmp(name, password)) {
        return "password must not equal the user name";
    }

    for (size_t i = 0; i < len; i++) {
        char c = password[i];
        if (c >= '0' && c <= '9') {
            has_digit = 1;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            has_alpha = 1;
        }
    }
    if (!has_alpha || !has_digit) {
        return "password must contain letters and digits";
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 增删用户                                                           */
/* ------------------------------------------------------------------ */
static int uid_in_use(u32 uid, const struct user_account *ignore)
{
    for (int i = 0; i < user_count(); i++) {
        const struct user_account *u = &active_store->accounts[i];
        if (u == ignore) {
            continue;
        }
        if (u->uid == uid) {
            return 1;
        }
    }
    return 0;
}

int user_add(const char *name, const char *password, u32 uid, u32 gid,
             const char *home, const char *shell)
{
    struct user_account *slot;
    size_t namelen;

    if (!active_store) {
        return -1;
    }
    if (!name || !name[0]) {
        return -1;
    }

    namelen = strlen(name);
    if (namelen >= USER_NAME_MAX) {
        return -1;
    }
    for (size_t i = 0; i < namelen; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return -1;
        }
    }
    if (name[0] == '-' || name[0] == '.') {
        return -1;
    }
    if (user_find(name)) {
        return -1;
    }
    if (active_store->header.count >= USER_MAX) {
        return -1;
    }
    if (uid_in_use(uid, NULL)) {
        return -1;
    }

    slot = &active_store->accounts[active_store->header.count];
    memset(slot, 0, sizeof(*slot));

    strncpy(slot->name, name, USER_NAME_MAX - 1);
    slot->uid = uid;
    slot->gid = gid;
    slot->active = 1;
    strncpy(slot->home, home ? home : "/", USER_HOME_MAX - 1);
    strncpy(slot->shell, shell ? shell : "/bin/sh", USER_SHELL_MAX - 1);

    user_set_password(slot, password ? password : "");

    active_store->header.count++;
    return 0;
}

int user_remove(const char *name)
{
    int index = -1;

    if (!active_store) {
        return -1;
    }
    for (int i = 0; i < (int)active_store->header.count; i++) {
        if (!strcmp(active_store->accounts[i].name, name)) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return -1;
    }
    if (active_store->accounts[index].uid == UID_ROOT) {
        return -2;      /* 不允许删除 root */
    }

    for (int i = index; i + 1 < (int)active_store->header.count; i++) {
        active_store->accounts[i] = active_store->accounts[i + 1];
    }
    active_store->header.count--;
    memset(&active_store->accounts[active_store->header.count], 0,
           sizeof(struct user_account));
    return 0;
}

static u32 next_free_uid(void)
{
    u32 uid = 1000;

    while (uid_in_use(uid, NULL)) {
        uid++;
    }
    return uid;
}

u32 user_next_uid(void)
{
    return next_free_uid();
}

/* ------------------------------------------------------------------ */
/* 会话                                                               */
/* ------------------------------------------------------------------ */
void user_session_clear(struct user_session *session)
{
    memset(session, 0, sizeof(*session));
}

int user_login(struct user_session *session, const char *name,
               const char *password)
{
    struct user_account *user = user_find(name);

    user_session_clear(session);

    if (!user) {
        return -1;
    }
    if (!user_check_password(user, password)) {
        return -2;
    }

    session->logged_in = 1;
    session->uid = user->uid;
    session->gid = user->gid;
    strncpy(session->name, user->name, USER_NAME_MAX - 1);
    strncpy(session->home, user->home, USER_HOME_MAX - 1);
    return 0;
}

int user_is_root(const struct user_session *session)
{
    return session->logged_in && session->uid == UID_ROOT;
}
