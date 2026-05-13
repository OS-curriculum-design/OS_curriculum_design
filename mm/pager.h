#ifndef PAGER_H
#define PAGER_H

#include "../include/types.h"

/*
 * 简化分页器接口
 * ===============
 * 这个模块在固定数量物理页框之上，实现了一个最小换页器：
 * - register_page 只登记“逻辑上存在”的页
 * - page fault 时再把页从交换区换入
 * - 页框不够时按当前页面置换算法换出
 */

#define PAGER_MAX_PAGES 128U
#define PAGER_FRAME_LIMIT 16U

typedef enum {
    PAGER_ALGORITHM_CLOCK = 0,
    PAGER_ALGORITHM_LRU = 1
} PagerAlgorithm;

/* 初始化分页器内部表项和交换槽。 */
int pager_init(void);
/* 查询分页器是否可用。 */
int pager_is_ready(void);
/* 查询/设置当前页面置换算法。 */
PagerAlgorithm pager_get_algorithm(void);
int pager_set_algorithm(PagerAlgorithm algorithm);
const char* pager_algorithm_name(PagerAlgorithm algorithm);
/* 采样 accessed 位，主要用于演示近似 LRU 的最近访问记录。 */
void pager_sample_usage(void);
/* 清空最近牺牲页记录，便于单次演示观察。 */
void pager_clear_victim_trace(void);
/* 注册一个按需换入的虚拟页。 */
int pager_register_page(uint32_t virt_addr, uint32_t flags);
/* 在指定页目录中注册一个按需换入的虚拟页。 */
int pager_register_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t flags);
/* 在指定页目录中注册带初始内容的按需页。 */
int pager_register_page_data(uint32_t page_directory_phys, uint32_t virt_addr, uint32_t flags, const uint8_t* data, uint32_t data_size);
/* 标记某个 pager 页是否禁止被页面置换选作牺牲页。 */
int pager_pin_page_in_directory(uint32_t page_directory_phys, uint32_t virt_addr, int pinned);
/* 注销一个按需页，并释放其当前驻留的物理页。 */
void pager_unregister_page(uint32_t page_directory_phys, uint32_t virt_addr);
/* 处理 not-present 页故障；返回 1 表示已成功补页。 */
int pager_handle_page_fault(uint32_t fault_addr, uint32_t err_code);

/* 打印分页器统计信息。 */
void pager_print_stats(void);

#endif
