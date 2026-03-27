#ifndef PMM_H
#define PMM_H

#include "../include/types.h"

/* 物理页大小：4KiB。 */
#define PAGE_SIZE 4096U

/*
 * 初始化物理内存管理器。
 * 参数来自 multiboot：
 * - multiboot_magic：协议校验值
 * - multiboot_info_addr：MultibootInfo 结构体物理地址
 */
void pmm_init(uint32_t multiboot_magic, uint32_t multiboot_info_addr);

/* 分配 1 个物理页，成功返回页起始地址，失败返回 0。 */
uint32_t pmm_alloc_page(void);
/* 释放 1 个物理页（参数必须是 4KiB 对齐的物理地址）。 */
void pmm_free_page(uint32_t phys_addr);

/* 查询 PMM 是否完成初始化。 */
int pmm_is_ready(void);
/* 查询 PMM 管理的总内存字节数。 */
uint32_t pmm_get_total_memory_bytes(void);
/* 查询总页数。 */
uint32_t pmm_get_total_pages(void);
/* 查询已使用页数。 */
uint32_t pmm_get_used_pages(void);
/* 查询空闲页数。 */
uint32_t pmm_get_free_pages(void);
/* 查询位图物理基地址。 */
uint32_t pmm_get_bitmap_base(void);
/* 查询位图总字节数。 */
uint32_t pmm_get_bitmap_size_bytes(void);

#endif
