#include "gdt.h"

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

/* 当前内核只准备了 3 个 GDT 表项：空项、代码段、数据段。 */
static GdtEntry gdt[3];
static GdtPointer gdt_ptr;

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

    /* 把新 GDT 装入 GDTR，并在汇编里刷新 CS/DS/SS 等段寄存器。 */
    gdt_load(&gdt_ptr);
}
