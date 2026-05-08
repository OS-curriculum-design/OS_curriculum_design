/*
 * 虚拟内存管理器实现
 * ==================
 * 本模块负责：
 * - 创建并维护页目录 / 页表
 * - 打开分页机制
 * - 在当前页目录或指定页目录中建立 / 删除映射
 * - 构造带有共享内核空间的用户地址空间
 */
#include "vmm.h"

#include "../include/string.h"
#include "pmm.h"

/* x86 分页相关常量。 */
#define PAGE_PRESENT 0x001U
#define PAGE_ADDR_MASK 0xFFFFF000U
#define PAGE_TABLE_ENTRIES 1024U
#define CR0_PAGING 0x80000000U
#define KERNEL_DIRECTORY_START (VMM_KERNEL_BASE >> 22)
#define KERNEL_LOW_SHARED_DIRECTORIES 1U

/* 当前 CPU 正在使用的页目录（内核可见虚拟地址）。 */
static uint32_t* current_page_directory = (uint32_t*)0;
/* 当前页目录的物理地址。 */
static uint32_t current_page_directory_phys = 0;
/* 内核主页目录的物理地址，供进程切回内核时使用。 */
static uint32_t kernel_page_directory_phys = 0;
/* 恒等映射的低地址字节数。 */
static uint32_t identity_mapped_bytes = 0;
/* 高地址内核镜像映射字节数。 */
static uint32_t kernel_mapped_bytes = 0;
/* 统计当前已建立的用户可见/内核可见页映射数量。 */
static uint32_t mapped_pages = 0;
static int vmm_ready = 0;
static int paging_enabled = 0;

/* 读取 / 写入控制寄存器是分页初始化中最关键的硬件步骤。 */
static uint32_t read_cr0(void) {
    uint32_t value;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint32_t value) {
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(value) : "memory");
}

static void load_cr3(uint32_t phys_addr) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(phys_addr) : "memory");
}

/* 单页失效，避免整张 TLB 被粗暴刷新。 */
static void invalidate_page(uint32_t virt_addr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

/* 线性地址高 10 位是页目录索引，中间 10 位是页表索引。 */
static uint32_t page_directory_index(uint32_t virt_addr) {
    return virt_addr >> 22;
}

static uint32_t page_table_index(uint32_t virt_addr) {
    return (virt_addr >> 12) & 0x3FFU;
}

static uint32_t* page_table_from_directory(uint32_t directory_entry) {
    return (uint32_t*)(directory_entry & PAGE_ADDR_MASK);
}

static void* phys_to_virt(uint32_t phys_addr) {
    /*
     * 分页未开启前，内核仍工作在“物理地址即线性地址”的阶段；
     * 打开分页后，则通过高地址内核映射访问同一片物理内存。
     */
    if (!paging_enabled) {
        return (void*)phys_addr;
    }

    return (void*)(VMM_KERNEL_BASE + phys_addr);
}

void* vmm_phys_to_virt(uint32_t phys_addr) {
    return phys_to_virt(phys_addr);
}

static int map_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags, int count_mapping) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_directory;
    uint32_t* page_table;
    uint32_t page_flags;

    if (page_directory_phys == 0) {
        return 0;
    }

    if ((virt_addr % PAGE_SIZE) != 0 || (phys_addr % PAGE_SIZE) != 0) {
        return 0;
    }

    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);
    /* 对外只允许传入少数几个抽象标志，真正写入硬件位时补上 P 位。 */
    page_flags = (flags & (VMM_PAGE_WRITABLE | VMM_PAGE_USER)) | PAGE_PRESENT;
    page_directory = (uint32_t*)phys_to_virt(page_directory_phys);

    if (!(page_directory[directory_index] & PAGE_PRESENT)) {
        uint32_t page_table_phys = pmm_alloc_page();

        if (page_table_phys == 0) {
            return 0;
        }

        /* 新页表必须先清零，否则会残留旧映射脏数据。 */
        memset(phys_to_virt(page_table_phys), 0, PAGE_SIZE);
        page_directory[directory_index] =
            page_table_phys | PAGE_PRESENT | VMM_PAGE_WRITABLE | (flags & VMM_PAGE_USER);
    } else if (flags & VMM_PAGE_USER) {
        /* 同一页目录项下一旦出现用户页，页目录项自身也必须带 USER 位。 */
        page_directory[directory_index] |= VMM_PAGE_USER;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(page_directory[directory_index]));

    if (count_mapping && !(page_table[table_index] & PAGE_PRESENT)) {
        mapped_pages++;
    }

    /* 直接覆盖目标页表项；当前实现不区分“新建映射”和“修改权限”。 */
    page_table[table_index] = (phys_addr & PAGE_ADDR_MASK) | page_flags;

    if (paging_enabled && page_directory_phys == current_page_directory_phys) {
        invalidate_page(virt_addr);
    }

    return 1;
}

int vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    return map_page_in_directory(current_page_directory_phys, virt_addr, phys_addr, flags, 1);
}

int vmm_map_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags) {
    return map_page_in_directory(page_directory_phys, virt_addr, phys_addr, flags, 0);
}

static void unmap_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, int count_mapping) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_directory;
    uint32_t* page_table;

    if (page_directory_phys == 0 || (virt_addr % PAGE_SIZE) != 0) {
        return;
    }

    page_directory = (uint32_t*)phys_to_virt(page_directory_phys);
    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);

    if (!(page_directory[directory_index] & PAGE_PRESENT)) {
        return;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(page_directory[directory_index]));
    if (page_table[table_index] & PAGE_PRESENT) {
        page_table[table_index] = 0;
        if (count_mapping && mapped_pages > 0) {
            mapped_pages--;
        }
        if (paging_enabled && page_directory_phys == current_page_directory_phys) {
            invalidate_page(virt_addr);
        }
    }
}

void vmm_unmap_page(uint32_t virt_addr) {
    unmap_page_in_directory(current_page_directory_phys, virt_addr, 1);
}

void vmm_unmap_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr) {
    unmap_page_in_directory(page_directory_phys, virt_addr, 0);
}

int vmm_get_mapping(uint32_t virt_addr, uint32_t* phys_addr_out) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_table;
    uint32_t entry;

    if (current_page_directory == (uint32_t*)0 || phys_addr_out == (uint32_t*)0) {
        return 0;
    }

    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);

    if (!(current_page_directory[directory_index] & PAGE_PRESENT)) {
        return 0;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(current_page_directory[directory_index]));
    entry = page_table[table_index];

    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }

    /* 低 12 位页内偏移直接沿用原虚拟地址的偏移。 */
    *phys_addr_out = (entry & PAGE_ADDR_MASK) | (virt_addr & ~PAGE_ADDR_MASK);
    return 1;
}

int vmm_get_page_entry(uint32_t virt_addr, uint32_t* entry_out) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_table;

    if (current_page_directory == (uint32_t*)0 || entry_out == (uint32_t*)0) {
        return 0;
    }

    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);

    if (!(current_page_directory[directory_index] & PAGE_PRESENT)) {
        return 0;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(current_page_directory[directory_index]));
    *entry_out = page_table[table_index];
    return 1;
}

int vmm_clear_page_accessed(uint32_t virt_addr) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_table;

    if (current_page_directory == (uint32_t*)0) {
        return 0;
    }

    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);

    if (!(current_page_directory[directory_index] & PAGE_PRESENT)) {
        return 0;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(current_page_directory[directory_index]));
    if (!(page_table[table_index] & PAGE_PRESENT)) {
        return 0;
    }

    /* accessed 位由 CPU 自动置位，这里清掉供时钟算法判断“最近是否被访问过”。 */
    page_table[table_index] &= ~VMM_PAGE_ACCESSED;
    invalidate_page(virt_addr);
    return 1;
}

int vmm_get_mapping_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t* phys_addr_out) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_directory;
    uint32_t* page_table;
    uint32_t entry;

    if (page_directory_phys == 0 || phys_addr_out == (uint32_t*)0) {
        return 0;
    }

    page_directory = (uint32_t*)phys_to_virt(page_directory_phys);
    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);

    if (!(page_directory[directory_index] & PAGE_PRESENT)) {
        return 0;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(page_directory[directory_index]));
    entry = page_table[table_index];

    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }

    *phys_addr_out = (entry & PAGE_ADDR_MASK) | (virt_addr & ~PAGE_ADDR_MASK);
    return 1;
}

int vmm_init(void) {
    uint32_t cr0;

    if (!pmm_is_ready()) {
        return 0;
    }

    current_page_directory_phys = pmm_alloc_page();
    if (current_page_directory_phys == 0) {
        return 0;
    }

    current_page_directory = (uint32_t*)phys_to_virt(current_page_directory_phys);
    kernel_page_directory_phys = current_page_directory_phys;
    memset(current_page_directory, 0, PAGE_SIZE);

    /*
     * 初始化阶段先为“全部物理内存”建立两套映射：
     * - 低地址恒等映射，保证启动早期代码和设备地址仍可直接访问
     * - 0xC0000000 以上高地址映射，作为后续内核长期使用的虚拟基址
     */
    identity_mapped_bytes = pmm_get_total_memory_bytes() & PAGE_ADDR_MASK;
    kernel_mapped_bytes = identity_mapped_bytes;

    for (uint32_t addr = 0; addr < identity_mapped_bytes; addr += PAGE_SIZE) {
        if (!vmm_map_page(addr, addr, VMM_PAGE_WRITABLE)) {
            return 0;
        }
    }

    for (uint32_t addr = 0; addr < kernel_mapped_bytes; addr += PAGE_SIZE) {
        if (!vmm_map_page(VMM_KERNEL_BASE + addr, addr, VMM_PAGE_WRITABLE)) {
            return 0;
        }
    }

    /* 先装入页目录，再置位 CR0.PG 打开分页。 */
    load_cr3(current_page_directory_phys);

    cr0 = read_cr0();
    write_cr0(cr0 | CR0_PAGING);

    paging_enabled = 1;
    current_page_directory = (uint32_t*)phys_to_virt(current_page_directory_phys);
    vmm_ready = 1;
    return 1;
}

int vmm_create_address_space(uint32_t* page_directory_phys_out) {
    uint32_t new_page_directory_phys;
    uint32_t* new_page_directory;
    uint32_t* kernel_page_directory;

    if (!vmm_ready || page_directory_phys_out == (uint32_t*)0) {
        return 0;
    }

    new_page_directory_phys = pmm_alloc_page();
    if (new_page_directory_phys == 0) {
        return 0;
    }

    new_page_directory = (uint32_t*)phys_to_virt(new_page_directory_phys);
    kernel_page_directory = (uint32_t*)phys_to_virt(kernel_page_directory_phys);
    memset(new_page_directory, 0, PAGE_SIZE);

    /*
     * 地址空间共享策略：
     * - 低 4MiB：保留给启动代码、VGA、早期内核结构等公共低端映射
     * - 0xC0000000 以上：共享整个内核高地址空间
     * - 中间用户区：先留空，由各进程自己映射代码和栈
     */
    for (uint32_t i = 0; i < KERNEL_LOW_SHARED_DIRECTORIES; i++) {
        new_page_directory[i] = kernel_page_directory[i];
    }

    for (uint32_t i = KERNEL_DIRECTORY_START; i < PAGE_TABLE_ENTRIES; i++) {
        new_page_directory[i] = kernel_page_directory[i];
    }

    *page_directory_phys_out = new_page_directory_phys;
    return 1;
}

void vmm_destroy_address_space(uint32_t page_directory_phys) {
    uint32_t* page_directory;

    if (!vmm_ready || page_directory_phys == 0 || page_directory_phys == kernel_page_directory_phys) {
        return;
    }

    page_directory = (uint32_t*)phys_to_virt(page_directory_phys);

    /*
     * 只释放该进程私有的“中间用户空间”页表；
     * 低端共享页表和高地址内核页表都由内核统一维护，不应在这里释放。
     */
    for (uint32_t i = KERNEL_LOW_SHARED_DIRECTORIES; i < KERNEL_DIRECTORY_START; i++) {
        if (page_directory[i] & PAGE_PRESENT) {
            pmm_free_page(page_directory[i] & PAGE_ADDR_MASK);
            page_directory[i] = 0;
        }
    }

    pmm_free_page(page_directory_phys);
}

static int alloc_and_map_user_page(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t flags, uint32_t* phys_out) {
    uint32_t phys_addr;

    /* 给用户页分配物理页，并在页目录中映射为 USER 可访问。 */
    phys_addr = pmm_alloc_page();
    if (phys_addr == 0) {
        return 0;
    }

    memset(phys_to_virt(phys_addr), 0, PAGE_SIZE);

    if (!vmm_map_page_in_directory(page_directory_phys, virt_addr, phys_addr, flags | VMM_PAGE_USER)) {
        pmm_free_page(phys_addr);
        return 0;
    }

    if (phys_out != (uint32_t*)0) {
        *phys_out = phys_addr;
    }

    return 1;
}

int vmm_create_user_demo_space(VmmUserSpaceInfo* info_out) {
    uint32_t page_directory_phys;
    uint32_t code_phys;
    uint32_t stack_phys;
    uint32_t stack_page_virt;

    if (!vmm_ready || info_out == (VmmUserSpaceInfo*)0) {
        return 0;
    }

    if (!vmm_create_address_space(&page_directory_phys)) {
        return 0;
    }

    /* 代码页默认只读即可；当前没有实现执行权限区分。 */
    if (!alloc_and_map_user_page(page_directory_phys, VMM_USER_CODE_BASE, 0, &code_phys)) {
        pmm_free_page(page_directory_phys);
        return 0;
    }

    /*
     * 用户栈向低地址增长。
     * 初始 ESP 可以放在 VMM_USER_STACK_TOP，第一次 push 会落到下面这页内。
     */
    stack_page_virt = VMM_USER_STACK_TOP - PAGE_SIZE;
    if (!alloc_and_map_user_page(page_directory_phys, stack_page_virt, VMM_PAGE_WRITABLE, &stack_phys)) {
        pmm_free_page(code_phys);
        pmm_free_page(page_directory_phys);
        return 0;
    }

    info_out->page_directory_phys = page_directory_phys;
    info_out->code_virt = VMM_USER_CODE_BASE;
    info_out->code_phys = code_phys;
    info_out->stack_top = VMM_USER_STACK_TOP;
    info_out->stack_page_virt = stack_page_virt;
    info_out->stack_phys = stack_phys;
    return 1;
}

int vmm_create_current_user_demo_space(VmmUserSpaceInfo* info_out, const uint8_t* code, uint32_t code_size) {
    uint32_t code_phys;
    uint32_t stack_phys;
    uint32_t stack_page_virt;
    uint8_t* code_dst;

    if (!vmm_ready || info_out == (VmmUserSpaceInfo*)0 || code == (const uint8_t*)0 || code_size > PAGE_SIZE) {
        return 0;
    }

    /*
     * 这个版本直接在当前页目录中创建用户页，
     * 适合 shell 里的 ring3 demo，不走完整的进程地址空间切换。
     */
    if (!alloc_and_map_user_page(current_page_directory_phys, VMM_USER_CODE_BASE, 0, &code_phys)) {
        return 0;
    }

    code_dst = (uint8_t*)phys_to_virt(code_phys);
    for (uint32_t i = 0; i < code_size; i++) {
        code_dst[i] = code[i];
    }

    stack_page_virt = VMM_USER_STACK_TOP - PAGE_SIZE;
    if (!alloc_and_map_user_page(current_page_directory_phys, stack_page_virt, VMM_PAGE_WRITABLE, &stack_phys)) {
        pmm_free_page(code_phys);
        return 0;
    }

    info_out->page_directory_phys = current_page_directory_phys;
    info_out->code_virt = VMM_USER_CODE_BASE;
    info_out->code_phys = code_phys;
    info_out->stack_top = VMM_USER_STACK_TOP;
    info_out->stack_page_virt = stack_page_virt;
    info_out->stack_phys = stack_phys;
    return 1;
}

int vmm_is_ready(void) {
    return vmm_ready;
}

int vmm_is_paging_enabled(void) {
    return paging_enabled;
}

int vmm_switch_page_directory(uint32_t page_directory_phys) {
    if (!vmm_ready || page_directory_phys == 0) {
        return 0;
    }

    /* CR3 切换后，CPU 的地址翻译上下文也随之切换。 */
    load_cr3(page_directory_phys);
    current_page_directory_phys = page_directory_phys;
    current_page_directory = (uint32_t*)phys_to_virt(page_directory_phys);
    return 1;
}

uint32_t vmm_get_page_directory(void) {
    return current_page_directory_phys;
}

uint32_t vmm_get_kernel_page_directory(void) {
    return kernel_page_directory_phys;
}

uint32_t vmm_get_identity_mapped_bytes(void) {
    return identity_mapped_bytes;
}

uint32_t vmm_get_kernel_mapped_bytes(void) {
    return kernel_mapped_bytes;
}

uint32_t vmm_get_mapped_pages(void) {
    return mapped_pages;
}
