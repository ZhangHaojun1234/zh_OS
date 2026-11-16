/* users_persist.h - 用户表落盘的接口。位置固定 LBA 200 起八个扇区（4KB），
 * 够装 ZH_USER_MAX(16) 条记录，错误码复用 ATA_*。 */

#ifndef ZH_OS_USERS_PERSIST_H
#define ZH_OS_USERS_PERSIST_H

#include <stdint.h>

/* 持久化起始扇区与占用扇区数。 */
#define USERS_PERSIST_LBA   200u
#define USERS_PERSIST_SECTORS 8u    /* 8 * 512 = 4096 字节 */

/* 序列化头部魔数与版本（全部小端存储）。 */
#define USERS_MAGIC      0x5355485Au  /* ASCII "ZHUS" 的小端 = 0x5A 'Z' 0x48 'H' 0x55 'U' 0x53 'S' */
#define USERS_MAGIC_STR  "ZHUS"
#define USERS_VERSION    1u

/*
 * 把内存账户表序列化后写入 LBA 200~207。
 * 返回 0 成功；无磁盘返回 ATA_ENODEV；写失败返回 ATA_EIO / ATA_EROFS。
 */
int users_save(void);

/*
 * 从 LBA 200 读入并覆盖内存账户表。
 * 校验 magic 与 version，任何格式错误都整体回退，静默保留内置默认值
 * （root/zh，初始密码 zh）。无磁盘时直接返回 0（用默认值）。
 * 返回 0（无论是否真的载入，都表示"当前账户表可用"）。
 */
int users_load(void);

#endif /* ZH_OS_USERS_PERSIST_H */
