#ifndef USERMODE_H
#define USERMODE_H

#include "../include/types.h"
#include "../interrupt/interrupts.h"

/*
 * 用户态/系统调用接口
 * ===================
 * 这个模块负责：
 * - 保存和恢复用户态执行上下文
 * - 通过 int 0x80 提供最小系统调用接口
 * - 为进程模块提供进入 ring3 的统一入口
 */

#define SYS_WRITE 1U
#define SYS_EXIT  2U
#define SYS_YIELD 3U

#define USERMODE_RETURN_YIELD 0xFFFFFFFEU

/* 可由 usermode_enter_context 直接恢复的最小用户态寄存器集合。 */
typedef struct {
    uint32_t eip;
    uint32_t user_esp;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
} UserContext;

/* 直接跳到指定用户入口和用户栈，返回值由系统调用带回。 */
uint32_t usermode_enter(uint32_t entry, uint32_t user_esp);
/* 从完整上下文恢复执行，用于调度器切换到某个进程。 */
uint32_t usermode_enter_context(UserContext* context);
/* 运行一个固定的 ring3 演示程序。 */
uint32_t usermode_run_demo(void);
/* 处理 int 0x80 进入的系统调用。 */
void usermode_handle_syscall(InterruptFrame* frame);
/* 结束当前用户态执行并返回内核调用点。 */
void usermode_return_to_kernel(uint32_t value) __attribute__((noreturn));

#endif
