#ifndef GDT_H
#define GDT_H

#include "../include/types.h"

/*
 * GDT/TSS 接口
 * ============
 * 这里定义了内核与用户态使用的段选择子常量，并暴露 GDT 初始化入口。
 */

#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define USER_CODE_SELECTOR   0x1B
#define USER_DATA_SELECTOR   0x23
#define TSS_SELECTOR         0x28

/* 建立 GDT、TSS，并装入 GDTR/TR。 */
void gdt_init(void);
/* 更新 TSS.esp0，供用户态陷入内核时切换到新的内核栈。 */
void gdt_set_kernel_stack(uint32_t stack_top);

#endif
