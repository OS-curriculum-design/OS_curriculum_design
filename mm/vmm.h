#ifndef VMM_H
#define VMM_H

#include "../include/types.h"

#define VMM_KERNEL_BASE 0xC0000000U
#define VMM_USER_TOP    VMM_KERNEL_BASE
#define VMM_USER_CODE_BASE 0x00400000U
#define VMM_USER_STACK_TOP VMM_USER_TOP

#define VMM_PAGE_WRITABLE 0x002U
#define VMM_PAGE_USER     0x004U
#define VMM_PAGE_ACCESSED 0x020U
#define VMM_PAGE_DIRTY    0x040U

typedef struct {
    uint32_t page_directory_phys;
    uint32_t code_virt;
    uint32_t code_phys;
    uint32_t stack_top;
    uint32_t stack_page_virt;
    uint32_t stack_phys;
} VmmUserSpaceInfo;

int vmm_init(void);

int vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
int vmm_map_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
void vmm_unmap_page(uint32_t virt_addr);
void vmm_unmap_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr);
int vmm_get_mapping(uint32_t virt_addr, uint32_t* phys_addr_out);
int vmm_get_mapping_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t* phys_addr_out);
int vmm_get_page_entry(uint32_t virt_addr, uint32_t* entry_out);
int vmm_clear_page_accessed(uint32_t virt_addr);

int vmm_create_address_space(uint32_t* page_directory_phys_out);
void vmm_destroy_address_space(uint32_t page_directory_phys);
int vmm_create_user_demo_space(VmmUserSpaceInfo* info_out);
int vmm_create_current_user_demo_space(VmmUserSpaceInfo* info_out, const uint8_t* code, uint32_t code_size);

void* vmm_phys_to_virt(uint32_t phys_addr);

int vmm_is_ready(void);
int vmm_is_paging_enabled(void);
int vmm_switch_page_directory(uint32_t page_directory_phys);
uint32_t vmm_get_page_directory(void);
uint32_t vmm_get_kernel_page_directory(void);
uint32_t vmm_get_identity_mapped_bytes(void);
uint32_t vmm_get_kernel_mapped_bytes(void);
uint32_t vmm_get_mapped_pages(void);

#endif
