#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "../include/types.h"

/* GRUB 传入 eax 时用于校验 multiboot 协议的魔数。 */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* MultibootInfo.flags 中的位定义。 */
#define MULTIBOOT_INFO_MEMORY 0x00000001
#define MULTIBOOT_INFO_MMAP   0x00000040

/* mmap 条目类型：1 表示该区间可用。 */
#define MULTIBOOT_MEMORY_AVAILABLE 1

/*
 * Multiboot 信息头（仅包含当前 PMM 用到的字段范围）。
 * 关键字段：
 * - mem_upper：1MiB 以上可用内存大小（单位 KiB）
 * - mmap_length + mmap_addr：内存映射表位置与长度
 */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint8_t syms[16];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) MultibootInfo;

/*
 * 内存映射表条目格式（mmap entry）。
 * - size：不包含自身 size 字段长度
 * - addr_low/high：起始物理地址（64 位拆成高低 32 位）
 * - len_low/high：区间长度（64 位拆成高低 32 位）
 * - type：区间类型（1 表示可用）
 */
typedef struct {
    uint32_t size;
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t len_low;
    uint32_t len_high;
    uint32_t type;
} __attribute__((packed)) MultibootMmapEntry;

#endif
