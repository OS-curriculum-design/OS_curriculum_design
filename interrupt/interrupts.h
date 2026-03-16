#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "../include/types.h"

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
    //数据段寄存器和附加段寄存器，存储段selector。告诉CPU数据和代码存储的位置。四个寄存器用于不同的段
    //在中断处理时，操作系统可能会修改这些寄存器来访问特定的内存区域。不是很重要
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    //通用寄存器
    uint32_t edi;//用于数据的处理和存储（通常作为源和目的操作数的寄存器）
    uint32_t esi;//同上
    uint32_t ebp;//基指针寄存器，通常用于存储当前栈帧的基地址
    uint32_t esp;//栈指针寄存器，指向当前栈顶。它指向中断处理前的栈位置
    uint32_t ebx;//通用寄存器
    uint32_t edx;//同上
    uint32_t ecx;//同上
    uint32_t eax;//同上
    uint32_t int_no;//中断号。中断发生时中断号会存入这里
    uint32_t err_code;//不是所有中断都会有错误码。在特定的异常中断（如页面错误（Page Fault）或一般保护错误（General Protection Fault））时被设置
    uint32_t eip;//程序指针（EIP）是 当前正在执行的指令地址。发生中断时将当前程序地址存入这里以便返回
    uint32_t cs;//当前执行的代码段的选择子，指示当前正在执行的代码所在的段。
    uint32_t eflags;//标志寄存器（EFLAGS）用于保存CPU的状态标志
    uint32_t useresp;//用户栈指针。如果中断发生在用户模式，这个字段保存 用户栈指针
    uint32_t ss;//堆栈段选择子。它表示堆栈所在的段
} InterruptFrame;

typedef void (*irq_handler_t)(InterruptFrame* frame);//函数指针

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
