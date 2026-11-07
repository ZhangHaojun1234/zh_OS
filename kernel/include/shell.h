/* ---------------------------------------------------------------
 * 内核命令行 (shell)
 * --------------------------------------------------------------- */
#ifndef ZH_SHELL_H
#define ZH_SHELL_H

#include "user.h"

/* 以给定会话运行 shell。会话为 NULL 时按 root 处理 (仅用于无账户模式)。
   返回 0 表示正常结束, 2 表示用户要求登出。 */
int shell_run(struct user_session *session);

#endif
