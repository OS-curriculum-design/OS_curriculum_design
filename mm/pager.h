#ifndef PAGER_H
#define PAGER_H

#include "../include/types.h"

#define PAGER_MAX_PAGES 128U
#define PAGER_FRAME_LIMIT 16U

int pager_init(void);
int pager_is_ready(void);
int pager_register_page(uint32_t virt_addr, uint32_t flags);
int pager_handle_page_fault(uint32_t fault_addr, uint32_t err_code);

void pager_print_stats(void);

#endif
