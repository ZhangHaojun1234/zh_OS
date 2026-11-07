/* ---------------------------------------------------------------
 * 登录流程
 * --------------------------------------------------------------- */
#ifndef ZH_LOGIN_H
#define ZH_LOGIN_H

#include "user.h"

#define LOGIN_MAX_ATTEMPTS 3

/* 显示 login: 提示并验证。成功返回 0 并填充 session, 失败返回 -1。 */
int login_prompt(struct user_session *session);

/* 重新验证口令 (passwd / su 用), 成功返回 0 */
int login_authenticate(const char *name, const char *prompt);

#endif
