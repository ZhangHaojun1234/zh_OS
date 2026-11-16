/* cmd_task.h - tasks / taskrun 的声明 */

#ifndef ZH_OS_CMD_TASK_H
#define ZH_OS_CMD_TASK_H

/* `tasks`：打印 TCB 表（状态、累计 tick、换入次数、退出/被杀原因）。 */
int cmd_tasks(int argc, char **argv);

/* `taskrun`：跑两个 ring3 任务；`taskrun fault` 让 B 故意 #PF 被单独杀掉。 */
int cmd_taskrun(int argc, char **argv);

#endif /* ZH_OS_CMD_TASK_H */
