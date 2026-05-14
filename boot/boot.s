# 启动入口说明
# ============
# GRUB 通过 multiboot 协议把控制权交给这里。
# 这段汇编只做四件事：
# 1. 提供 multiboot 头，让 GRUB 识别本内核
# 2. 建立一套临时页表，开启分页
# 3. 跳到高地址内核映射
# 4. 把 multiboot 参数压栈后调用 kernel_main
.section .multiboot,"a"
.align 4
.long 0x1BADB002
.long 0x00000003
.long -(0x1BADB002 + 0x00000003)

.section .bootstrap,"ax"
.code32
.global _start
.extern kernel_main

.set KERNEL_BASE, 0xC0000000
.set PAGE_SIZE, 4096
.set PAGE_PRESENT, 0x001
.set PAGE_WRITABLE, 0x002
.set CR0_PAGING, 0x80000000
.set KERNEL_PDE_INDEX, 768

_start:
    cli
    # 分页开启前只能可靠使用低地址临时栈。
    mov $bootstrap_stack_top, %esp

    # GRUB 约定：EAX=magic，EBX=multiboot info 物理地址。
    mov %eax, %ebp
    mov %ebx, %esi

    # 清空临时页目录和页表。
    mov $bootstrap_page_directory, %edi
    xor %eax, %eax
    mov $3072, %ecx
    rep stosl

    # 临时映射低 4MiB，同时把同一段物理内存映射到 0xC0000000。
    mov $bootstrap_low_page_table, %edi
    mov $bootstrap_high_page_table, %edx
    xor %eax, %eax
    mov $1024, %ecx
1:
    mov %eax, %ebx
    or $(PAGE_PRESENT | PAGE_WRITABLE), %ebx
    mov %ebx, (%edi)
    mov %ebx, (%edx)
    add $PAGE_SIZE, %eax
    add $4, %edi
    add $4, %edx
    loop 1b

    mov $bootstrap_low_page_table, %eax
    or $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov %eax, bootstrap_page_directory

    mov $bootstrap_high_page_table, %eax
    or $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov %eax, bootstrap_page_directory + KERNEL_PDE_INDEX * 4

    mov $bootstrap_page_directory, %eax
    mov %eax, %cr3

    mov %cr0, %eax
    or $CR0_PAGING, %eax
    mov %eax, %cr0

    # 当前指令流还在低地址别名上，立刻跳到高地址别名继续。
    mov $(higher_half_entry + KERNEL_BASE), %eax
    jmp *%eax

higher_half_entry:
    # 使用高地址 .bss 中预留的正式早期内核栈。
    mov $stack_top, %esp
    # GRUB 约定：EAX=magic，EBX=multiboot info 指针。
    # 这里按 C 调用约定把它们压栈，供 kernel_main(uint32_t, uint32_t) 使用。
    add $KERNEL_BASE, %esi
    push %esi
    push %ebp
    mov $kernel_main, %eax
    call *%eax

halt:
    # 如果内核主函数意外返回，就停机自旋，避免继续执行未知区域。
    cli
    hlt
    jmp halt

.section .bootstrap_bss,"aw",@nobits
.align 4096
bootstrap_page_directory:
    .skip 4096
bootstrap_low_page_table:
    .skip 4096
bootstrap_high_page_table:
    .skip 4096
.align 16
bootstrap_stack_bottom:
    .skip 4096
bootstrap_stack_top:

.section .bss,"aw",@nobits
.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .note.GNU-stack,"",@progbits
