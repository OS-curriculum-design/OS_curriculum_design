.section .multiboot,"a"
.align 4
.long 0x1BADB002
.long 0x00000003
.long -(0x1BADB002 + 0x00000003)

.section .text
.code32
.global _start
.extern kernel_main

_start:
    mov $stack_top, %esp
    push %ebx
    push %eax
    call kernel_main

halt:
    cli
    hlt
    jmp halt

.section .bss,"aw",@nobits
.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .note.GNU-stack,"",@progbits
