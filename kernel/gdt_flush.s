.section .text
.code32
.global gdt_load
.global tss_load

gdt_load:
    movl 4(%esp), %eax
    lgdt (%eax)

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    ljmp $0x08, $1f
1:
    ret

tss_load:
    movw $0x28, %ax
    ltr %ax
    ret

.section .note.GNU-stack,"",@progbits
