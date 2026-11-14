/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/version.h
 * Version: 0.1beta
 * Brief:   全项目唯一的版本号来源：启动横幅、uname、登录横幅、GRUB 菜单
 *          都由这里的宏拼接，避免多处硬编码后出现版本不一致。
 * ============================================================================ */

#ifndef ZH_OS_VERSION_H
#define ZH_OS_VERSION_H

#define ZH_OS_NAME          "zh_OS"
#define ZH_OS_VERSION       "0.1beta"
#define ZH_OS_ARCH          "x86_64"

/* 对外统一呈现的版本号字符串，必须与 build.sh 的 VERSION_TEXT 完全一致。 */
#define ZH_OS_VERSION_TEXT  "zh_OS 0.1beta (x86_64)"

#endif /* ZH_OS_VERSION_H */
