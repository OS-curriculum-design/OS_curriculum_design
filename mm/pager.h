#ifndef PAGER_H
#define PAGER_H

#include "../include/types.h"

/*
 * 简化分页器接口
 * ===============
 * 这个模块在固定数量物理页框之上，实现了一个最小换页器：
 * - register_page 只登记“逻辑上存在”的页
 * - page fault 时再把页从交换区换入
 * - 页框不够时用时钟算法换出
 */

#define PAGER_MAX_PAGES 128U
#define PAGER_FRAME_LIMIT 16U

/* 初始化分页器内部表项和交换槽。 */
int pager_init(void);
/* 查询分页器是否可用。 */
int pager_is_ready(void);
/* 注册一个按需换入的虚拟页。 */
int pager_register_page(uint32_t virt_addr, uint32_t flags);
/* 处理 not-present 页故障；返回 1 表示已成功补页。 */
int pager_handle_page_fault(uint32_t fault_addr, uint32_t err_code);

/* 打印分页器统计信息。 */
void pager_print_stats(void);

#endif
