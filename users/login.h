/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    users/login.h
 * Version: 0.2beta
 * Brief:   登录界面接口声明。
 * ============================================================================ */

#ifndef ZH_OS_LOGIN_H
#define ZH_OS_LOGIN_H

/*
 * 第 4 步：登录界面。
 *
 * login_screen() 会清屏、绘制彩色横幅并进入登录循环；登录成功后设置
 * 全局 zh_current_user 然后返回给调用者（kernel_main 的临时会话循环）。
 * logout() 清空当前会话并重新走一遍 login_screen()，供第 5 步 Shell 的
 * logout 命令直接复用。
 */

void login_screen(void);

void logout(void);

#endif /* ZH_OS_LOGIN_H */
