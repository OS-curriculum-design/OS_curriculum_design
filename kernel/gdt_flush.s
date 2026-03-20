.section .text
.code32
.global gdt_load

gdt_load:
    movl 4(%esp), %eax//将参数传入eax
    lgdt (%eax)//将eax中的值装入gdtr

    movw $0x10, %ax//内核数据段
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    ljmp $0x08, $1f//内核指令段
1:
    ret

.section .note.GNU-stack,"",@progbits
