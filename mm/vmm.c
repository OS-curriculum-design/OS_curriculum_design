#include "vmm.h"

#include "../include/string.h"
#include "pmm.h"

#define PAGE_PRESENT 0x001U
#define PAGE_ADDR_MASK 0xFFFFF000U
#define PAGE_TABLE_ENTRIES 1024U
#define CR0_PAGING 0x80000000U
#define KERNEL_DIRECTORY_START (VMM_KERNEL_BASE >> 22)

static uint32_t* current_page_directory = (uint32_t*)0;
static uint32_t current_page_directory_phys = 0;
static uint32_t identity_mapped_bytes = 0;
static uint32_t kernel_mapped_bytes = 0;
static uint32_t mapped_pages = 0;
static int vmm_ready = 0;
static int paging_enabled = 0;

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

static void invalidate_page(uint32_t virt_addr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

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
    page_flags = (flags & (VMM_PAGE_WRITABLE | VMM_PAGE_USER)) | PAGE_PRESENT;
    page_directory = (uint32_t*)phys_to_virt(page_directory_phys);

    if (!(page_directory[directory_index] & PAGE_PRESENT)) {
        uint32_t page_table_phys = pmm_alloc_page();

        if (page_table_phys == 0) {
            return 0;
        }

        memset(phys_to_virt(page_table_phys), 0, PAGE_SIZE);
        page_directory[directory_index] =
            page_table_phys | PAGE_PRESENT | VMM_PAGE_WRITABLE | (flags & VMM_PAGE_USER);
    } else if (flags & VMM_PAGE_USER) {
        page_directory[directory_index] |= VMM_PAGE_USER;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(page_directory[directory_index]));

    if (count_mapping && !(page_table[table_index] & PAGE_PRESENT)) {
        mapped_pages++;
    }

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

void vmm_unmap_page(uint32_t virt_addr) {
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t* page_table;

    if (current_page_directory == (uint32_t*)0 || (virt_addr % PAGE_SIZE) != 0) {
        return;
    }

    directory_index = page_directory_index(virt_addr);
    table_index = page_table_index(virt_addr);

    if (!(current_page_directory[directory_index] & PAGE_PRESENT)) {
        return;
    }

    page_table = (uint32_t*)phys_to_virt((uint32_t)page_table_from_directory(current_page_directory[directory_index]));
    if (page_table[table_index] & PAGE_PRESENT) {
        page_table[table_index] = 0;
        if (mapped_pages > 0) {
            mapped_pages--;
        }
        if (paging_enabled) {
            invalidate_page(virt_addr);
        }
    }
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
    memset(current_page_directory, 0, PAGE_SIZE);

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

    if (!vmm_ready || page_directory_phys_out == (uint32_t*)0) {
        return 0;
    }

    new_page_directory_phys = pmm_alloc_page();
    if (new_page_directory_phys == 0) {
        return 0;
    }

    new_page_directory = (uint32_t*)phys_to_virt(new_page_directory_phys);
    memset(new_page_directory, 0, PAGE_SIZE);

    /*
     * 用户空间页目录项保持为空；内核空间页目录项直接共享当前内核映射。
     */
    for (uint32_t i = KERNEL_DIRECTORY_START; i < PAGE_TABLE_ENTRIES; i++) {
        new_page_directory[i] = current_page_directory[i];
    }

    *page_directory_phys_out = new_page_directory_phys;
    return 1;
}

static int alloc_and_map_user_page(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t flags, uint32_t* phys_out) {
    uint32_t phys_addr;

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

uint32_t vmm_get_page_directory(void) {
    return current_page_directory_phys;
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
