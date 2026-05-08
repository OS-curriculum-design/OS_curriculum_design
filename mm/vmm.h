#ifndef VMM_H
#define VMM_H

#include "../include/types.h"

/*
 * 虚拟内存管理接口
 * ================
 * 这里封装页目录/页表操作，并提供内核页表初始化、用户地址空间创建、
 * 页映射查询以及页目录切换等能力。
 */

#define VMM_KERNEL_BASE 0xC0000000U
#define VMM_USER_TOP    VMM_KERNEL_BASE
#define VMM_USER_CODE_BASE 0x00400000U
#define VMM_USER_STACK_TOP VMM_USER_TOP

#define VMM_PAGE_WRITABLE 0x002U
#define VMM_PAGE_USER     0x004U
#define VMM_PAGE_ACCESSED 0x020U
#define VMM_PAGE_DIRTY    0x040U

/* 用于向 Shell 或用户态演示代码返回一组关键虚拟内存布局信息。 */
typedef struct {
    uint32_t page_directory_phys;
    uint32_t code_virt;
    uint32_t code_phys;
    uint32_t stack_top;
    uint32_t stack_page_virt;
    uint32_t stack_phys;
} VmmUserSpaceInfo;

/* 初始化内核页目录并打开分页。 */
int vmm_init(void);

/* 在当前页目录中建立一个 4KiB 映射。 */
int vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
/* 在指定页目录中建立一个 4KiB 映射。 */
int vmm_map_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
/* 从当前页目录中取消某个虚拟页映射。 */
void vmm_unmap_page(uint32_t virt_addr);
/* 从指定页目录中取消某个虚拟页映射。 */
void vmm_unmap_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr);
/* 查询当前页目录中的虚拟地址映射结果。 */
int vmm_get_mapping(uint32_t virt_addr, uint32_t* phys_addr_out);
/* 查询指定页目录中的虚拟地址映射结果。 */
int vmm_get_mapping_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t* phys_addr_out);
/* 读取当前页目录下某个虚拟页对应的页表项原始值。 */
int vmm_get_page_entry(uint32_t virt_addr, uint32_t* entry_out);
/* 清除某个虚拟页的 accessed 位，供时钟算法使用。 */
int vmm_clear_page_accessed(uint32_t virt_addr);

/* 创建仅复制内核高地址映射的新页目录。 */
int vmm_create_address_space(uint32_t* page_directory_phys_out);
/* 销毁一个由 vmm_create_address_space 创建的页目录。 */
void vmm_destroy_address_space(uint32_t page_directory_phys);
/* 构造一个简单的用户态演示地址空间。 */
int vmm_create_user_demo_space(VmmUserSpaceInfo* info_out);
/* 在当前内核上下文中创建可立即进入的用户态演示空间。 */
int vmm_create_current_user_demo_space(VmmUserSpaceInfo* info_out, const uint8_t* code, uint32_t code_size);

/* 在当前映射规则下，把物理地址转换成内核可访问的虚拟地址。 */
void* vmm_phys_to_virt(uint32_t phys_addr);

/* 查询 VMM 是否已初始化。 */
int vmm_is_ready(void);
/* 查询 CR0.PG 是否已经置位。 */
int vmm_is_paging_enabled(void);
/* 切换当前正在使用的页目录。 */
int vmm_switch_page_directory(uint32_t page_directory_phys);
/* 返回当前页目录物理地址。 */
uint32_t vmm_get_page_directory(void);
/* 返回内核主页目录物理地址。 */
uint32_t vmm_get_kernel_page_directory(void);
/* 返回低端恒等映射总字节数。 */
uint32_t vmm_get_identity_mapped_bytes(void);
/* 返回高地址内核映射总字节数。 */
uint32_t vmm_get_kernel_mapped_bytes(void);
/* 返回累计映射页数统计。 */
uint32_t vmm_get_mapped_pages(void);

#endif
