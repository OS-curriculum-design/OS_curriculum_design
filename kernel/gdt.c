#include "gdt.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) GdtEntry;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) GdtPointer;

extern void gdt_load(const GdtPointer* ptr);

static GdtEntry gdt[3];
static GdtPointer gdt_ptr;

static void gdt_set_entry(size_t index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access = access;
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base = (uint32_t)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0x000FFFFF, 0x9A, 0xCF);
    gdt_set_entry(2, 0, 0x000FFFFF, 0x92, 0xCF);

    gdt_load(&gdt_ptr);
}
