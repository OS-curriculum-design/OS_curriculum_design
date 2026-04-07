#include "gdt.h"
#include "../include/string.h"

/*
 * 32 位保护模式下的 GDT 表项格式（8 字节）。
 *
 * 一个段描述符主要描述：
 * - base : 段基址
 * - limit: 段界限
 * - access / granularity: 段类型、权限、粒度等控制位
 */
typedef struct {
    /* 段界限低 16 位。 */
    uint16_t limit_low;
    /* 段基址低 16 位。 */
    uint16_t base_low;
    /* 段基址中间 8 位。 */
    uint8_t base_middle;
    /* 访问控制字节：段类型、是否可执行、特权级、是否存在等。 */
    uint8_t access;//
    /* 高 4 位放粒度/位宽标志，低 4 位放段界限高 4 位。 */
    uint8_t granularity;
    /* 段基址高 8 位。 */
    uint8_t base_high;
} __attribute__((packed)) GdtEntry;

/* lgdt 指令需要的“伪描述符”格式。 */
typedef struct {
    /* 整个 GDT 的总字节数减 1。 */
    uint16_t limit;
    /* GDT 在线性地址空间中的起始地址。 */
    uint32_t base;
} __attribute__((packed)) GdtPointer;

/* 在汇编里执行 lgdt，并刷新各段寄存器。 */
extern void gdt_load(const GdtPointer* ptr);
extern void tss_load(void);

typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) TssEntry;

/* 4KiB 的临时内核栈，供 CPU 从 ring3 进中断时切到 ring0 使用。 */
static uint8_t tss_kernel_stack[4096];

/* GDT：空项、内核代码、内核数据、用户代码、用户数据、TSS。 */
static GdtEntry gdt[6];
static GdtPointer gdt_ptr;
static TssEntry tss;

static void gdt_set_entry(size_t index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    /* 把 32 位 base 和 20 位 limit 拆开后写入段描述符。 */
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access = access;
    /*
     * granularity 的低 4 位来自 limit[19:16]，
     * 高 4 位是 G/D 等控制位。
     */
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);
}

void gdt_init(void) {
    /* 先准备好给 lgdt 使用的 GDT 指针。 */
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base = (uint32_t)&gdt;

    memset(&tss, 0, sizeof(tss));

    /* 第 0 项必须是空描述符，x86 约定 selector 0 无效。 */
    gdt_set_entry(0, 0, 0, 0, 0);
    /*
     * 第 1 项：内核代码段。
     * access = 0x9A:
     * - P=1   段存在
     * - DPL=0 内核态
     * - S=1   代码/数据段
     * - Type=1010b：可执行代码段，可读
     *
     * granularity = 0xCF:
     * - G=1   4 KiB 粒度
     * - D=1   32 位段
     * - limit 高 4 位 = 0xF
     */
    gdt_set_entry(1, 0, 0x000FFFFF, 0x9A, 0xCF);
    /*
     * 第 2 项：内核数据段。
     * access = 0x92:
     * - P=1   段存在
     * - DPL=0 内核态
     * - S=1   代码/数据段
     * - Type=0010b：数据段，可读写
     */
    gdt_set_entry(2, 0, 0x000FFFFF, 0x92, 0xCF);
    /*
     * 第 3 项：用户代码段。
     * access = 0xFA: P=1, DPL=3, S=1, 可执行可读代码段。
     */
    gdt_set_entry(3, 0, 0x000FFFFF, 0xFA, 0xCF);
    /*
     * 第 4 项：用户数据段。
     * access = 0xF2: P=1, DPL=3, S=1, 可读写数据段。
     */
    gdt_set_entry(4, 0, 0x000FFFFF, 0xF2, 0xCF);

    /*
     * 第 5 项：32 位可用 TSS。
     * access = 0x89: P=1, DPL=0, 系统段，类型=1001b（available 32-bit TSS）。
     * TSS 的 limit 按字节粒度设置，所以 granularity 传 0。
     */
    tss.ss0 = KERNEL_DATA_SELECTOR;
    tss.esp0 = (uint32_t)(tss_kernel_stack + sizeof(tss_kernel_stack));
    tss.iomap_base = sizeof(tss);
    gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1U, 0x89, 0x00);

    /* 把新 GDT 装入 GDTR，并在汇编里刷新 CS/DS/SS 等段寄存器。 */
    gdt_load(&gdt_ptr);
    tss_load();
}

void gdt_set_kernel_stack(uint32_t stack_top) {
    tss.esp0 = stack_top;
}
