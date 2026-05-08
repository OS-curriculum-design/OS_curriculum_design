# 启动入口说明
# ============
# GRUB 通过 multiboot 协议把控制权交给这里。
# 这段汇编只做三件事：
# 1. 提供 multiboot 头，让 GRUB 识别本内核
# 2. 建立一段临时内核栈
# 3. 把 multiboot 参数压栈后调用 kernel_main
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
    # 使用 .bss 中预留的 16KiB 栈作为早期内核栈。
    mov $stack_top, %esp
    # GRUB 约定：EAX=magic，EBX=multiboot info 指针。
    # 这里按 C 调用约定把它们压栈，供 kernel_main(uint32_t, uint32_t) 使用。
    push %ebx
    push %eax
    call kernel_main

halt:
    # 如果内核主函数意外返回，就停机自旋，避免继续执行未知区域。
    cli
    hlt
    jmp halt

.section .bss,"aw",@nobits
.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .note.GNU-stack,"",@progbits
