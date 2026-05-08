#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "../include/types.h"

/*
 * 中断模块公共接口
 * ================
 * 这里定义了中断栈帧结构、IRQ 处理函数类型以及 IDT/PIC 初始化接口。
 */

/*
 * 这个结构体描述的是“中断发生后，汇编入口帮我们整理好的栈帧”。
 *
 * 顺序必须和 interrupt_stubs.s 里的压栈顺序严格一致，否则 C 代码按这个
 * 结构体解释时就会读错字段，轻则打印错信息，重则直接崩溃。
 *
 * 各字段来源：
 * - gs/fs/es/ds: 我们在汇编入口里手动 push 的段寄存器
 * - edi...eax: pusha 自动保存的通用寄存器
 * - int_no: 我们手动压入的“中断向量号”
 * - err_code: 某些异常由 CPU 自动压入错误码；没有错误码的情况我们补 0
 * - eip/cs/eflags: CPU 进入中断时自动压栈的返回现场
 * - useresp/ss: 如果发生了特权级切换，CPU 还会额外压入这两个值
 */
typedef struct interrupt_frame {
    /* 进入 C 处理函数前保存下来的段寄存器。 */
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    /* pusha 保存下来的通用寄存器。 */
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    /* 汇编入口补齐的向量号与错误码。 */
    uint32_t int_no;
    uint32_t err_code;
    /* CPU 自动压栈的返回现场。 */
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;
    uint32_t ss;
} InterruptFrame;

/* 每条 IRQ 在线 C 层对应的处理函数签名。 */
typedef void (*irq_handler_t)(InterruptFrame* frame);

/* 初始化 IDT、重映射 PIC、设置中断门。 */
void interrupts_init(void);

/* 打开/关闭 IF（Interrupt Flag），即允许/禁止可屏蔽中断。 */
void interrupts_enable(void);
void interrupts_disable(void);

/* 给 IRQ0~IRQ15 注册 C 级处理函数。 */
void irq_register_handler(uint8_t irq, irq_handler_t handler);

/* 汇编入口最终会调用这里，把中断分发到对应的 C 处理逻辑。 */
void isr_dispatch(InterruptFrame* frame);

#endif
