/*
 * 链接脚本说明
 * ============
 * 这个文件决定内核在最终 ELF 中的段布局：
 * - 保留一小段 1MiB 低地址 bootstrap，供 GRUB 入口和临时分页使用
 * - 正式内核链接到 0xC0000000 以上的高虚拟地址
 * - 保留 multiboot 头，确保 GRUB 能识别
 * - 按页对齐 text / rodata / data / bss，方便分页管理
 * - 导出 __kernel_start / __kernel_end 供内存管理模块使用
 */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

KERNEL_PHYS_BASE = 0x00100000;
KERNEL_VIRT_BASE = 0xC0000000;

SECTIONS
{
    . = KERNEL_PHYS_BASE;
    __kernel_phys_start = .;

    .multiboot ALIGN(4) : {
        KEEP(*(.multiboot))
    }

    .bootstrap ALIGN(4K) : {
        *(.bootstrap)
    }

    .bootstrap_data ALIGN(4K) : {
        *(.bootstrap_data)
    }

    .bootstrap_bss ALIGN(4K) (NOLOAD) : {
        *(.bootstrap_bss)
    }

    . = ALIGN(4K);
    __kernel_high_phys_start = .;
    . += KERNEL_VIRT_BASE;
    __kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
        *(.text*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
        *(.rodata*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
        *(.data*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4K);
    __kernel_end = .;
    __kernel_phys_end = . - KERNEL_VIRT_BASE;
}
