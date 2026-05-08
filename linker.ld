/*
 * 链接脚本说明
 * ============
 * 这个文件决定内核在最终 ELF 中的段布局：
 * - 从 1MiB 线性地址开始放置内核
 * - 保留 multiboot 头，确保 GRUB 能识别
 * - 按页对齐 text / rodata / data / bss，方便分页管理
 * - 导出 __kernel_start / __kernel_end 供内存管理模块使用
 */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .multiboot ALIGN(4) : {
        KEEP(*(.multiboot))
    }

    .text ALIGN(4K) : {
        *(.text*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data*)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4K);
    __kernel_end = .;
}
