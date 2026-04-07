.section .data
.code32
.global usermode_saved_esp
usermode_saved_esp:
    .long 0
usermode_saved_ebp:
    .long 0
usermode_saved_ebx:
    .long 0
usermode_saved_esi:
    .long 0
usermode_saved_edi:
    .long 0

.section .text
.code32
.global usermode_enter
.global usermode_return_to_kernel

usermode_enter:
    movl %esp, usermode_saved_esp
    movl %ebp, usermode_saved_ebp
    movl %ebx, usermode_saved_ebx
    movl %esi, usermode_saved_esi
    movl %edi, usermode_saved_edi

    movl 4(%esp), %eax
    movl 8(%esp), %edx

    movw $0x23, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs

    pushl $0x23
    pushl %edx

    pushfl
    popl %ecx
    orl $0x200, %ecx
    pushl %ecx

    pushl $0x1B
    pushl %eax
    iret

usermode_return_to_kernel:
    movl 4(%esp), %eax

    movl usermode_saved_ebx, %ebx
    movl usermode_saved_esi, %esi
    movl usermode_saved_edi, %edi
    movl usermode_saved_ebp, %ebp

    movw $0x10, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs

    movl usermode_saved_esp, %esp
    sti
    ret

.section .note.GNU-stack,"",@progbits
