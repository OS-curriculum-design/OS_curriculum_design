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
.global usermode_enter_context
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

usermode_enter_context:
    movl %esp, usermode_saved_esp
    movl %ebp, usermode_saved_ebp
    movl %ebx, usermode_saved_ebx
    movl %esi, usermode_saved_esi
    movl %edi, usermode_saved_edi

    movl 4(%esp), %esi

    movl 0(%esi), %eax
    movl 4(%esi), %edx
    movl 8(%esi), %ecx
    orl $0x200, %ecx

    pushl $0x23
    pushl %edx
    pushl %ecx
    pushl $0x1B
    pushl %eax

    movw $0x23, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs

    movl 36(%esi), %ebp
    movl 16(%esi), %ebx
    movl 20(%esi), %ecx
    movl 24(%esi), %edx
    movl 32(%esi), %edi
    movl 12(%esi), %eax
    movl 28(%esi), %esi
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
