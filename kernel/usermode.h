#ifndef USERMODE_H
#define USERMODE_H

#include "../include/types.h"
#include "../interrupt/interrupts.h"

uint32_t usermode_run_demo(void);
void usermode_handle_syscall(InterruptFrame* frame);
void usermode_return_to_kernel(uint32_t value) __attribute__((noreturn));

#endif
