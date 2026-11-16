/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_task.c
 * Version: 0.2beta
 * Brief:   M4 进程与调度：tasks / taskrun 命令实现。
 * ============================================================================ */

#include "cmd_task.h"

#include "shell.h"
#include "../kernel/task.h"

/* 只比较两个走 argv 的短字符串（freestanding，无 strcmp）。 */
static int arg_equals(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    task_dump();
    return SHELL_CONTINUE;
}

int cmd_taskrun(int argc, char **argv)
{
    int mode = 0;

    if (argc >= 2 && arg_equals(argv[1], "fault")) {
        mode = 1;
    } else if (argc >= 2 && arg_equals(argv[1], "dry")) {
        mode = 2;   /* [M5] 只建进程、只打印页表，不进 ring3 */
    }

    (void)task_demo(mode);
    return SHELL_CONTINUE;
}
