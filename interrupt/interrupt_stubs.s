# 中断汇编入口说明
# =================
# 这个文件为每个异常/IRQ 提供一个统一的汇编入口，职责是：
# 1. 补齐错误码和中断向量号
# 2. 保存通用寄存器与段寄存器
# 3. 切到内核数据段
# 4. 把整理好的 InterruptFrame 交给 isr_dispatch
.extern isr_dispatch

.section .text
.code32

.macro ISR_NOERR num
.global isr\num
isr\num:
    # 对“CPU 不会自动压错误码”的异常，手动补 0，保证栈帧统一。
    pushl $0
    pushl $\num
    jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    # 对“CPU 已自动压错误码”的异常，只需要再压入向量号。
    pushl $\num
    jmp isr_common_stub
.endm

.macro IRQ num vector
.global irq\num
irq\num:
    # 硬件 IRQ 没有错误码，同样补 0。
    pushl $0
    pushl $\vector
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

ISR_NOERR 128

isr_common_stub:
    # pusha 会依次保存 eax, ecx, edx, ebx, esp, ebp, esi, edi。
    pusha
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs

    # 所有后续 C 代码都按内核数据段访问内存。
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # 传入当前栈顶，等价于 InterruptFrame*。
    pushl %esp
    call isr_dispatch
    addl $4, %esp

    # 按与压栈相反的顺序恢复寄存器，再由 iret 恢复 CPU 自动保存的现场。
    popl %gs
    popl %fs
    popl %es
    popl %ds
    popa
    addl $8, %esp
    iret

.section .note.GNU-stack,"",@progbits
