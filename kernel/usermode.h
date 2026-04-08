#ifndef USERMODE_H
#define USERMODE_H

#include "../include/types.h"
#include "../interrupt/interrupts.h"

#define SYS_WRITE 1U
#define SYS_EXIT  2U
#define SYS_YIELD 3U

#define USERMODE_RETURN_YIELD 0xFFFFFFFEU

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

uint32_t usermode_enter(uint32_t entry, uint32_t user_esp);
uint32_t usermode_enter_context(UserContext* context);
uint32_t usermode_run_demo(void);
void usermode_handle_syscall(InterruptFrame* frame);
void usermode_return_to_kernel(uint32_t value) __attribute__((noreturn));

#endif
