/* users_persist.c - 用户表落盘：LBA 200 起八个扇区，小端序列化。
 * 用 drivers/ata.c 的 LBA28 读写和 drivers/ata.h 的错误码，通过 users.c 末尾追加的
 * user_table_entry() / user_table_reset_and_load() 存取内存账户表，既有用户逻辑一行没改。 */

#include "users.h"
#include "ata.h"
#include "users_persist.h"

/* 4KB 缓冲（静态 .bss，初值 0；每次落盘前整体清零）。 */
static uint8_t g_persist_buf[4096u];

/* 小端编解码助手（freestanding 无 libc，自己实现保证字节序明确） */

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void put_u64_le(uint8_t *p, uint64_t v)
{
    for (uint32_t i = 0; i < 8u; ++i) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0] |
            ((uint32_t)p[1] << 8u) |
            ((uint32_t)p[2] << 16u) |
            ((uint32_t)p[3] << 24u));
}

static uint64_t get_u64_le(const uint8_t *p)
{
    uint64_t r = 0;
    for (uint32_t i = 0; i < 8u; ++i) {
        r |= ((uint64_t)p[i] << (8u * i));
    }
    return r;
}

/* 把 src 字符串拷进 cap 字节的字段（dest 整块已清零，拷贝后自然以 0 结尾）。 */
static void field_copy(uint8_t *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (src != 0) {
        while (src[i] != '\0' && i + 1u < cap) {
            dst[i] = (uint8_t)src[i];
            ++i;
        }
    }
}

/* 单条账户记录在缓冲里的固定偏移与尺寸。 */
#define PERSIST_HDR_SIZE   16u
#define PERSIST_ENTRY_SIZE 148u   /* 32 + 4 + 4 + 64 + 32 + 8 + 1 + 3 */

int users_save(void)
{
    if (!ata_disk_present()) {
        return ATA_ENODEV;   /* 无磁盘：调用方据此打印黄字警告 */
    }

    /* 整块清零（避免上一次更长的账户表残留）。 */
    for (uint32_t i = 0; i < sizeof(g_persist_buf); ++i) {
        g_persist_buf[i] = 0u;
    }

    const uint32_t n = user_count();

    /* 头部：magic "ZHUS" + version + 用户数 N + 保留 4 字节。 */
    g_persist_buf[0] = (uint8_t)'Z';
    g_persist_buf[1] = (uint8_t)'H';
    g_persist_buf[2] = (uint8_t)'U';
    g_persist_buf[3] = (uint8_t)'S';
    put_u32_le(g_persist_buf + 4u, USERS_VERSION);
    put_u32_le(g_persist_buf + 8u, n);
    put_u32_le(g_persist_buf + 12u, 0u);

    for (uint32_t i = 0; i < n; ++i) {
        const zh_user_t *u = user_table_entry(i);
        if (u == 0) {
            continue;
        }
        const uint32_t off = PERSIST_HDR_SIZE + i * PERSIST_ENTRY_SIZE;

        field_copy(g_persist_buf + off, ZH_USER_NAME_MAX, u->name);
        put_u32_le(g_persist_buf + off + 32u, u->uid);
        put_u32_le(g_persist_buf + off + 36u, u->gid);
        field_copy(g_persist_buf + off + 40u, ZH_USER_HOME_MAX, u->home);
        field_copy(g_persist_buf + off + 104u, ZH_USER_SHELL_MAX, u->shell);
        put_u64_le(g_persist_buf + off + 136u, u->pass_hash);
        g_persist_buf[off + 144u] = u->is_admin ? 1u : 0u;
        /* off+145..147 保留字段保持为 0（已清零） */
    }

    /* 写满 8 个扇区（4096 字节），多出的部分为零，保证镜像干净。 */
    for (uint32_t s = 0; s < USERS_PERSIST_SECTORS; ++s) {
        const int rc =
            ata_write_sector(USERS_PERSIST_LBA + s,
                             g_persist_buf + s * 512u);
        if (rc != ATA_OK) {
            return rc;
        }
    }
    return ATA_OK;
}

int users_load(void)
{
    if (!ata_disk_present()) {
        return 0;   /* 无磁盘：直接用内置默认值 */
    }

    /* 读入 8 个扇区；任意一次失败都视为"没有有效持久化数据"，保留默认值。 */
    for (uint32_t s = 0; s < USERS_PERSIST_SECTORS; ++s) {
        if (ata_read_sector(USERS_PERSIST_LBA + s,
                            g_persist_buf + s * 512u) != ATA_OK) {
            return 0;
        }
    }

    /* 校验魔数与版本。 */
    if (g_persist_buf[0] != (uint8_t)'Z' ||
        g_persist_buf[1] != (uint8_t)'H' ||
        g_persist_buf[2] != (uint8_t)'U' ||
        g_persist_buf[3] != (uint8_t)'S') {
        return 0;
    }
    if (get_u32_le(g_persist_buf + 4u) != USERS_VERSION) {
        return 0;
    }

    const uint32_t n = get_u32_le(g_persist_buf + 8u);
    if (n == 0u || n > ZH_USER_MAX) {
        return 0;   /* 数量非法：不载入半张表，保留默认值 */
    }

    /* 先解析到本地临时表，全部校验通过后再覆盖内存账户表（失败也不破坏默认值）。 */
    static zh_user_t tmp[ZH_USER_MAX];
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t off = PERSIST_HDR_SIZE + i * PERSIST_ENTRY_SIZE;
        zh_user_t *u = &tmp[i];

        for (uint32_t j = 0; j < ZH_USER_NAME_MAX; ++j) {
            u->name[j] = (char)g_persist_buf[off + j];
        }
        u->name[ZH_USER_NAME_MAX - 1u] = '\0';

        u->uid = get_u32_le(g_persist_buf + off + 32u);
        u->gid = get_u32_le(g_persist_buf + off + 36u);

        for (uint32_t j = 0; j < ZH_USER_HOME_MAX; ++j) {
            u->home[j] = (char)g_persist_buf[off + 40u + j];
        }
        u->home[ZH_USER_HOME_MAX - 1u] = '\0';

        for (uint32_t j = 0; j < ZH_USER_SHELL_MAX; ++j) {
            u->shell[j] = (char)g_persist_buf[off + 104u + j];
        }
        u->shell[ZH_USER_SHELL_MAX - 1u] = '\0';

        u->pass_hash = get_u64_le(g_persist_buf + off + 136u);
        u->is_admin = (g_persist_buf[off + 144u] != 0u);
    }

    user_table_reset_and_load(tmp, n);
    return 0;
}
