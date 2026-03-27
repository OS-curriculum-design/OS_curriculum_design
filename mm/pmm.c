#include "pmm.h"

#include "../console/console.h"
#include "../include/string.h"
#include "multiboot.h"

#define DEFAULT_TOTAL_MEMORY_BYTES (128U * 1024U * 1024U)
#define LOW_MEMORY_RESERVED_BYTES  0x00100000U

extern uint8_t __kernel_end[];

static uint8_t* pmm_bitmap = (uint8_t*)0;
static uint32_t pmm_bitmap_base = 0;
static uint32_t pmm_bitmap_bytes = 0;
static uint32_t pmm_total_memory_bytes = 0;
static uint32_t pmm_total_pages = 0;
static uint32_t pmm_free_pages = 0;
static int pmm_ready = 0;

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1U);
}

static int bitmap_test(uint32_t page_index) {
    return (pmm_bitmap[page_index / 8U] & (uint8_t)(1U << (page_index % 8U))) != 0;
}

static void bitmap_set(uint32_t page_index) {
    pmm_bitmap[page_index / 8U] |= (uint8_t)(1U << (page_index % 8U));
}

static void bitmap_clear(uint32_t page_index) {
    pmm_bitmap[page_index / 8U] &= (uint8_t)~(1U << (page_index % 8U));
}

static void reserve_page(uint32_t page_index) {
    if (page_index >= pmm_total_pages || bitmap_test(page_index)) {
        return;
    }

    bitmap_set(page_index);
    if (pmm_free_pages > 0) {
        pmm_free_pages--;
    }
}

static void free_page(uint32_t page_index) {
    if (page_index >= pmm_total_pages || !bitmap_test(page_index)) {
        return;
    }

    bitmap_clear(page_index);
    pmm_free_pages++;
}

static void reserve_range(uint32_t start, uint32_t end) {
    uint32_t page_start;
    uint32_t page_end;

    if (start >= end) {
        return;
    }

    page_start = align_down(start, PAGE_SIZE) / PAGE_SIZE;
    page_end = align_up(end, PAGE_SIZE) / PAGE_SIZE;

    if (page_end > pmm_total_pages) {
        page_end = pmm_total_pages;
    }

    for (uint32_t page = page_start; page < page_end; page++) {
        reserve_page(page);
    }
}

static void free_range(uint32_t start, uint32_t end) {
    uint32_t page_start;
    uint32_t page_end;

    if (start >= end) {
        return;
    }

    if (start < LOW_MEMORY_RESERVED_BYTES) {
        start = LOW_MEMORY_RESERVED_BYTES;
    }
    if (end > pmm_total_memory_bytes) {
        end = pmm_total_memory_bytes;
    }
    if (start >= end) {
        return;
    }

    page_start = align_up(start, PAGE_SIZE) / PAGE_SIZE;
    page_end = align_down(end, PAGE_SIZE) / PAGE_SIZE;

    if (page_end > pmm_total_pages) {
        page_end = pmm_total_pages;
    }

    for (uint32_t page = page_start; page < page_end; page++) {
        free_page(page);
    }
}

static uint32_t detect_total_memory_bytes(uint32_t multiboot_magic, const MultibootInfo* info) {
    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC && info != (const MultibootInfo*)0) {
        if ((info->flags & MULTIBOOT_INFO_MEMORY) && info->mem_upper != 0) {
            return LOW_MEMORY_RESERVED_BYTES + info->mem_upper * 1024U;
        }
    }

    return DEFAULT_TOTAL_MEMORY_BYTES;
}

static void free_memory_from_map(const MultibootInfo* info) {
    uint32_t mmap_current;
    uint32_t mmap_end;

    mmap_current = info->mmap_addr;
    mmap_end = info->mmap_addr + info->mmap_length;

    while (mmap_current < mmap_end) {
        const MultibootMmapEntry* entry = (const MultibootMmapEntry*)mmap_current;

        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE &&
            entry->addr_high == 0 &&
            entry->len_high == 0 &&
            entry->len_low != 0) {
            uint32_t start = entry->addr_low;
            uint32_t end = start + entry->len_low;

            if (end > start) {
                free_range(start, end);
            }
        }

        mmap_current += entry->size + sizeof(entry->size);
    }
}

void pmm_init(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    const MultibootInfo* info = (const MultibootInfo*)multiboot_info_addr;
    uint32_t bitmap_end;

    pmm_total_memory_bytes = detect_total_memory_bytes(multiboot_magic, info);
    pmm_total_pages = pmm_total_memory_bytes / PAGE_SIZE;
    pmm_bitmap_bytes = (pmm_total_pages + 7U) / 8U;
    pmm_bitmap_base = align_up((uint32_t)__kernel_end, PAGE_SIZE);
    pmm_bitmap = (uint8_t*)pmm_bitmap_base;

    memset(pmm_bitmap, 0xFF, pmm_bitmap_bytes);
    pmm_free_pages = 0;

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC &&
        info != (const MultibootInfo*)0 &&
        (info->flags & MULTIBOOT_INFO_MMAP) &&
        info->mmap_length != 0) {
        free_memory_from_map(info);
    } else {
        free_range(LOW_MEMORY_RESERVED_BYTES, pmm_total_memory_bytes);
    }

    bitmap_end = align_up(pmm_bitmap_base + pmm_bitmap_bytes, PAGE_SIZE);
    reserve_range(0, bitmap_end);

    pmm_ready = 1;
}

uint32_t pmm_alloc_page(void) {
    if (!pmm_ready) {
        return 0;
    }

    for (uint32_t page = 0; page < pmm_total_pages; page++) {
        if (!bitmap_test(page)) {
            reserve_page(page);
            return page * PAGE_SIZE;
        }
    }

    return 0;
}

void pmm_free_page(uint32_t phys_addr) {
    uint32_t page_index;

    if (!pmm_ready || phys_addr % PAGE_SIZE != 0) {
        return;
    }

    page_index = phys_addr / PAGE_SIZE;

    if (page_index * PAGE_SIZE < align_up(pmm_bitmap_base + pmm_bitmap_bytes, PAGE_SIZE)) {
        return;
    }

    free_page(page_index);
}

int pmm_is_ready(void) {
    return pmm_ready;
}

uint32_t pmm_get_total_memory_bytes(void) {
    return pmm_total_memory_bytes;
}

uint32_t pmm_get_total_pages(void) {
    return pmm_total_pages;
}

uint32_t pmm_get_used_pages(void) {
    return pmm_total_pages - pmm_free_pages;
}

uint32_t pmm_get_free_pages(void) {
    return pmm_free_pages;
}

uint32_t pmm_get_bitmap_base(void) {
    return pmm_bitmap_base;
}

uint32_t pmm_get_bitmap_size_bytes(void) {
    return pmm_bitmap_bytes;
}
