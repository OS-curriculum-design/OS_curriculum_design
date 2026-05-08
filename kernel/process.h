#ifndef PROCESS_H
#define PROCESS_H

#include "../include/types.h"
#include "../interrupt/interrupts.h"

/*
 * 进程调度接口
 * ============
 * 本项目中的“进程”是教学用的最小用户态任务模型：
 * - 每个进程有独立页目录、代码页和用户栈页
 * - 调度采用简单轮转
 * - 通过 int 0x80 实现 write / exit / yield 三个系统调用
 */

typedef enum {
    PROCESS_UNUSED = 0,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_ZOMBIE
} ProcessState;

/* 初始化进程表与调度器状态。 */
void process_init(void);
/* 由内建样例程序名创建一个进程。 */
int process_spawn_builtin(const char* name);
/* 由内存中的应用镜像创建一个进程。 */
int process_spawn_from_buffer(const char* name, const uint8_t* image, uint32_t image_size);
/* 把 hello / counter / busy 构造成可执行镜像。 */
int process_build_builtin_image(const char* name, uint8_t* image, uint32_t image_capacity, uint32_t* image_size_out);
/* 调度一个 READY 进程运行。 */
int process_schedule(void);
/* 自动调度入口，供时钟中断或主循环调用。 */
int process_schedule_auto(void);
/* 创建并立即运行一个内建程序。 */
int process_run_builtin(const char* name);
/* 是否存在 READY 状态的进程。 */
int process_has_ready(void);
/* 查询自动调度开关。 */
int process_auto_schedule_enabled(void);
/* 打开或关闭自动调度。 */
void process_set_auto_schedule(int enabled);
/* 回收所有僵尸进程，返回回收数量。 */
int process_reap_zombies(void);
/* 打印进程表。 */
void process_print_table(void);
/* 在 SYS_YIELD 时保存用户寄存器现场。 */
void process_save_yield_frame(InterruptFrame* frame);
/* 在时钟中断尾部检查是否需要抢占当前进程。 */
int process_preempt_if_needed(InterruptFrame* frame);

#endif
