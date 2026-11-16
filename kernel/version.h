/* version.h - 版本号只在这里定义，横幅、uname、登录页、GRUB 菜单都从这几个宏拼出来 */

#ifndef ZH_OS_VERSION_H
#define ZH_OS_VERSION_H

#define ZH_OS_NAME          "zh_OS"
#define ZH_OS_VERSION       "1.0"
#define ZH_OS_ARCH          "x86_64"

/* 对外统一呈现的版本号字符串，必须与 build.sh 的 VERSION_TEXT 完全一致。 */
#define ZH_OS_VERSION_TEXT  "zh_OS 1.0 (x86_64)"

#endif /* ZH_OS_VERSION_H */
