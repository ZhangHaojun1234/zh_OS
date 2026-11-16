/* cmd_user.h - ring3 命令的声明 */

#ifndef ZH_OS_CMD_USER_H
#define ZH_OS_CMD_USER_H

/* ring3：把 CPU 送进用户态跑一段自带机器码，用 #GP 证明特权级生效。 */
int cmd_ring3(int argc, char **argv);

#endif /* ZH_OS_CMD_USER_H */
