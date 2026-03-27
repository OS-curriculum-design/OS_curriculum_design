#ifndef PMM_H
#define PMM_H

#include "../include/types.h"

#define PAGE_SIZE 4096U

void pmm_init(uint32_t multiboot_magic, uint32_t multiboot_info_addr);

uint32_t pmm_alloc_page(void);
void pmm_free_page(uint32_t phys_addr);

int pmm_is_ready(void);
uint32_t pmm_get_total_memory_bytes(void);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_bitmap_base(void);
uint32_t pmm_get_bitmap_size_bytes(void);

#endif
