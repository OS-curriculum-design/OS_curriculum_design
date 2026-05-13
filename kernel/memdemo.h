#ifndef MEMDEMO_H
#define MEMDEMO_H

#include "../include/types.h"

#define MEMDEMO_OP_ALLOC 1U
#define MEMDEMO_OP_FREE  2U
#define MEMDEMO_OP_TOUCH 3U

#define MEMDEMO_EVENT_COUNT 8U

void memdemo_reset(void);
int memdemo_apply_op(uint32_t op, uint32_t page);
int memdemo_report_event(uint32_t sequence);
void memdemo_release_for_directory(uint32_t page_directory_phys);
const char* memdemo_log_name(void);

#endif
