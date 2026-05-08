# GDT/TSS 装载辅助汇编
# =====================
# gdt_load : 执行 lgdt，并刷新各段寄存器与 CS
# tss_load : 把 TSS 选择子装入 TR，使 ring3 -> ring0 切换可用
.section .text
.code32
.global gdt_load
.global tss_load

gdt_load:
    # 参数 1：GdtPointer*，位于栈顶返回地址之后。
    movl 4(%esp), %eax
    lgdt (%eax)

    # 刷新数据段寄存器，让它们指向新的内核数据段。
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # 远跳转强制刷新 CS。
    ljmp $0x08, $1f
1:
    ret

tss_load:
    # TSS 描述符在 GDT 中的选择子由 C 端固定为 0x28。
    movw $0x28, %ax
    ltr %ax
    ret

.section .note.GNU-stack,"",@progbits
