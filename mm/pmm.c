#include "pmm.h"

#include "../console/console.h"
#include "../include/string.h"
#include "multiboot.h"

/*
 * PMM（Physical Memory Manager，物理内存管理器）说明
 * ==================================================
 * 这份代码负责“物理页”的分配与回收，页大小固定为 4KiB。
 *
 * 一、位图模型
 * - 每个 bit 对应一个物理页：
 *   - 1：该页已占用（或保留）
 *   - 0：该页空闲（可分配）
 *
 * 二、初始化思路
 * 1) 先把所有页都设为“占用”（最安全的起点）
 * 2) 根据 multiboot 提供的内存信息，释放真正可用的区域
 * 3) 再把关键区域保留回来（低端内存、内核、位图自身）
 *
 * 三、对外接口
 * - pmm_alloc_page()：分配一个物理页，返回页起始物理地址
 * - pmm_free_page() ：释放一个物理页
 *
 * 四、实现取舍
 * - 目前采用 first-fit 线性扫描，简单、容易教学和调试
 * - 后续可优化为 next-fit 或分层位图以提升性能
 */

/* 当引导信息不可靠时，临时采用的总内存兜底值（128MiB）。 */
#define DEFAULT_TOTAL_MEMORY_BYTES (128U * 1024U * 1024U)
/* 1MiB 以下通常保留给历史兼容区域，不作为通用可分配内存。 */
#define LOW_MEMORY_RESERVED_BYTES  0x00100000U

/* 链接脚本导出的内核结束地址（物理地址）。 */
extern uint8_t __kernel_end[];

/* 位图起始指针（映射到 pmm_bitmap_base）。 */
static uint8_t* pmm_bitmap = (uint8_t*)0;
/* 位图在物理内存中的起始地址。 */
static uint32_t pmm_bitmap_base = 0;
/* 位图总字节数。 */
static uint32_t pmm_bitmap_bytes = 0;

/* PMM 管理的总内存（字节）。 */
static uint32_t pmm_total_memory_bytes = 0;
/* 总页数 = pmm_total_memory_bytes / PAGE_SIZE。 */
static uint32_t pmm_total_pages = 0;
/* 当前空闲页计数。 */
static uint32_t pmm_free_pages = 0;

/* 初始化完成标记：1 表示可分配/可释放。 */
static int pmm_ready = 0;

/*
 * 向上对齐：
 * 把 value 调整到 alignment 的整数倍，且结果 >= value。
 */
static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

/*
 * 向下对齐：
 * 把 value 调整到 alignment 的整数倍，且结果 <= value。
 */
static uint32_t align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1U);
}

/*
 * 查询某个页索引对应的 bit 是否为 1。
 * 返回非 0：该页已占用；返回 0：该页空闲。
 */
static int bitmap_test(uint32_t page_index) {
    return (pmm_bitmap[page_index / 8U] & (uint8_t)(1U << (page_index % 8U))) != 0;
}

/* 将某页置为“占用”。 */
static void bitmap_set(uint32_t page_index) {
    pmm_bitmap[page_index / 8U] |= (uint8_t)(1U << (page_index % 8U));
}

/* 将某页置为“空闲”。 */
static void bitmap_clear(uint32_t page_index) {
    pmm_bitmap[page_index / 8U] &= (uint8_t)~(1U << (page_index % 8U));
}

/*
 * 保留单页：
 * - 如果 page_index 越界，忽略；
 * - 如果该页已经是占用状态，忽略（避免重复扣减 free 计数）。
 */
static void reserve_page(uint32_t page_index) {
    if (page_index >= pmm_total_pages || bitmap_test(page_index)) {
        return;
    }

    bitmap_set(page_index);
    if (pmm_free_pages > 0) {
        pmm_free_pages--;
    }
}

/*
 * 释放单页：
 * - 如果 page_index 越界，忽略；
 * - 如果该页已经空闲，忽略（避免重复增加 free 计数）。
 */
static void free_page(uint32_t page_index) {
    if (page_index >= pmm_total_pages || !bitmap_test(page_index)) {
        return;
    }

    bitmap_clear(page_index);
    pmm_free_pages++;
}

/*
 * 保留地址区间 [start, end) 的全部页（左闭右开）。
 *
 * 为什么 start 向下对齐、end 向上对齐？
 * - 因为“保留”要确保完整覆盖，只要页有一部分在区间内就必须保留。
 */
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

/*
 * 释放地址区间 [start, end) 的页（左闭右开）。
 *
 * 安全限制：
 * - 低于 1MiB 的内存不释放；
 * - 高于总内存上限的部分不释放。
 *
 * 为什么 start 向上对齐、end 向下对齐？
 * - 因为“释放”要保守，只有完整落在区间内的页才释放。
 */
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

/*
 * 估算总物理内存大小（字节）。
 *
 * 当前策略：
 * - 若 multiboot 的 mem_upper 可用，则按它计算；
 * - 否则回退到 DEFAULT_TOTAL_MEMORY_BYTES。
 *
 * 说明：
 * - 这是“尽量可用”的实现；
 * - 更严谨的方式是优先由 mmap 推导内存上界。
 */
static uint32_t detect_total_memory_bytes(uint32_t multiboot_magic, const MultibootInfo* info) {
    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC && info != (const MultibootInfo*)0) {
        if ((info->flags & MULTIBOOT_INFO_MEMORY) && info->mem_upper != 0) {
            return LOW_MEMORY_RESERVED_BYTES + info->mem_upper * 1024U;
        }
    }

    return DEFAULT_TOTAL_MEMORY_BYTES;
}

/*
 * 按 multiboot mmap 表释放可用区间。
 *
 * 目前仅处理 32 位地址空间（addr_high/len_high 必须为 0）。
 * 条件满足时，把 AVAILABLE 区间交给 free_range()。
 */
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

            /* 防止 uint32_t 溢出导致 end 回绕。 */
            if (end > start) {
                free_range(start, end);
            }
        }

        /*
         * multiboot mmap 的下一项地址：
         * 当前条目头 size 字段 + size 指定的数据体。
         */
        mmap_current += entry->size + sizeof(entry->size);
    }
}

/*
 * 初始化 PMM。
 * 输入参数由 boot.s 从 GRUB 传到 kernel_main 再传到这里。
 */
void pmm_init(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    const MultibootInfo* info = (const MultibootInfo*)multiboot_info_addr;
    uint32_t bitmap_end;

    /* 1) 计算总页数与位图布局。 */
    pmm_total_memory_bytes = detect_total_memory_bytes(multiboot_magic, info);
    pmm_total_pages = pmm_total_memory_bytes / PAGE_SIZE;
    pmm_bitmap_bytes = (pmm_total_pages + 7U) / 8U;
    pmm_bitmap_base = align_up((uint32_t)__kernel_end, PAGE_SIZE);
    pmm_bitmap = (uint8_t*)pmm_bitmap_base;

    /* 2) 所有页默认标为“占用”。 */
    memset(pmm_bitmap, 0xFF, pmm_bitmap_bytes);
    pmm_free_pages = 0;

    /* 3) 根据引导信息释放可用页。 */
    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC &&
        info != (const MultibootInfo*)0 &&
        (info->flags & MULTIBOOT_INFO_MMAP) &&
        info->mmap_length != 0) {
        free_memory_from_map(info);
    } else {
        /*
         * 无 mmap 时的保守兜底：
         * 仅释放 [1MiB, total_memory)。
         */
        free_range(LOW_MEMORY_RESERVED_BYTES, pmm_total_memory_bytes);
    }

    /*
     * 4) 重新保留关键区域：
     * [0, bitmap_end) 覆盖低端内存、内核镜像、位图本身。
     */
    bitmap_end = align_up(pmm_bitmap_base + pmm_bitmap_bytes, PAGE_SIZE);
    reserve_range(0, bitmap_end);

    /* 5) 标记就绪。 */
    pmm_ready = 1;
}

/*
 * 分配 1 个物理页。
 * 返回值：
 * - 成功：页起始物理地址（4KiB 对齐）
 * - 失败：0
 */
uint32_t pmm_alloc_page(void) {
    if (!pmm_ready) {
        return 0;
    }

    /*
     * first-fit：从低地址到高地址线性找第一个空闲页。
     * 优点：逻辑简单；
     * 缺点：在大内存场景中可能慢。
     */
    for (uint32_t page = 0; page < pmm_total_pages; page++) {
        if (!bitmap_test(page)) {
            reserve_page(page);
            return page * PAGE_SIZE;
        }
    }

    return 0;
}

/*
 * 释放 1 个物理页。
 * 保护逻辑：
 * - 未初始化时忽略；
 * - 地址不是页对齐时忽略；
 * - 落在内核/位图保留区时忽略。
 */
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

/* PMM 是否已初始化。 */
int pmm_is_ready(void) {
    return pmm_ready;
}

/* 总内存字节数。 */
uint32_t pmm_get_total_memory_bytes(void) {
    return pmm_total_memory_bytes;
}

/* 总页数。 */
uint32_t pmm_get_total_pages(void) {
    return pmm_total_pages;
}

/* 已使用页数。 */
uint32_t pmm_get_used_pages(void) {
    return pmm_total_pages - pmm_free_pages;
}

/* 空闲页数。 */
uint32_t pmm_get_free_pages(void) {
    return pmm_free_pages;
}

/* 位图基地址（物理地址）。 */
uint32_t pmm_get_bitmap_base(void) {
    return pmm_bitmap_base;
}

/* 位图总字节数。 */
uint32_t pmm_get_bitmap_size_bytes(void) {
    return pmm_bitmap_bytes;
}
