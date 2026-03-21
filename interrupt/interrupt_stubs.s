.extern isr_dispatch

.section .text
.code32

.macro ISR_NOERR num//没有错误码的异常
.global isr\num
isr\num:
    pushl $0//压入一个0,与CPU自动压入错误码的对齐布局
    pushl $\num
    jmp isr_common_stub
.endm

.macro ISR_ERR num//CPU会自动压入错误码的异常
.global isr\num
isr\num:
    pushl $\num
    jmp isr_common_stub
.endm

.macro IRQ num vector//硬件中断
.global irq\num
irq\num:
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

isr_common_stub:
    pusha//保存通用寄存器
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs

    pushl %esp//InterruptFrame*
    call isr_dispatch
    addl $4, %esp

    popl %gs
    popl %fs
    popl %es
    popl %ds
    popa
    addl $8, %esp//前面压入的错误码和num
    iret

.section .note.GNU-stack,"",@progbits
