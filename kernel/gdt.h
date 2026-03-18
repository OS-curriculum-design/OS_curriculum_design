#ifndef GDT_H
#define GDT_H

#include "../include/types.h"

#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10

void gdt_init(void);

#endif
