# 用户态切换辅助汇编
# ==================
# 这里封装了两类跳转：
# - usermode_enter         : 直接按入口地址 + 用户栈进入 ring3
# - usermode_enter_context : 按完整 UserContext 恢复寄存器进入 ring3
# - usermode_return_to_kernel : 从系统调用处理逻辑返回到内核 C 代码
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
    # 先把当前内核现场中的关键寄存器保存下来，方便系统调用返回时恢复。
    movl %esp, usermode_saved_esp
    movl %ebp, usermode_saved_ebp
    movl %ebx, usermode_saved_ebx
    movl %esi, usermode_saved_esi
    movl %edi, usermode_saved_edi

    movl 4(%esp), %eax
    movl 8(%esp), %edx

    # 0x23 = 用户数据段选择子（RPL=3）。
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

    # 0x1B = 用户代码段选择子（RPL=3）。
    pushl $0x1B
    pushl %eax
    iret

usermode_enter_context:
    # 与 usermode_enter 相同，但入口参数改为 UserContext*。
    movl %esp, usermode_saved_esp
    movl %ebp, usermode_saved_ebp
    movl %ebx, usermode_saved_ebx
    movl %esi, usermode_saved_esi
    movl %edi, usermode_saved_edi

    movl 4(%esp), %esi

    # 先从上下文中取出 iret 所需的关键字段。
    movl 0(%esi), %eax
    movl 4(%esi), %edx
    movl 8(%esi), %ecx
    orl $0x200, %ecx

    pushl $0x23
    pushl %edx
    pushl %ecx
    pushl $0x1B
    pushl %eax

    # 切换到用户段后，再恢复通用寄存器。
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
    # eax 作为“返回给内核的值”保留，不被下面的恢复过程覆盖。
    movl 4(%esp), %eax

    # 除 eax 之外，其余内核寄存器恢复成进入用户态前的样子。
    movl usermode_saved_ebx, %ebx
    movl usermode_saved_esi, %esi
    movl usermode_saved_edi, %edi
    movl usermode_saved_ebp, %ebp

    movw $0x10, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs

    # 最后恢复内核栈，并用普通 ret 回到调用 usermode_enter* 的 C 代码。
    movl usermode_saved_esp, %esp
    sti
    ret

.section .note.GNU-stack,"",@progbits
