#include "interrupts.h"
#include "../console/console.h"
#include "../drivers/io.h"
#include "../kernel/usermode.h"
#include "../mm/pager.h"

/*
 * x86 IDT 一共有 256 个中断向量槽位，编号 0~255。
 *
 * 典型划分：
 * - 0~31: CPU 异常/保留向量
 * - 32~47: 我们通常放 PIC 重映射后的 16 条硬件 IRQ
 * - 其余: 可以留给系统调用、自定义软件中断等
 */
#define IDT_ENTRIES 256 // interrupt descriptor table

/* 8259A PIC 的 I/O 端口。 */
#define PIC1_COMMAND 0x20//主 PIC 命令寄存器  pic for programmable interrupt controller
#define PIC1_DATA 0x21//主 PIC 数据寄存器
#define PIC2_COMMAND 0xA0//从 PIC 命令寄存器
#define PIC2_DATA 0xA1//从 PIC 数据寄存器
//PIC是中断控制器，主PIC负责0-7,从PIC负责8-15
/*
 * 向 PIC 命令端口写 0x20 表示 EOI（End Of Interrupt）：
 * “这次中断处理完了，你可以继续发下一次了”。
 */
#define PIC_EOI 0x20

/*
 * 我们把硬件 IRQ 重映射到 32 开始。
 * 原因：IRQ 默认是 0~15，但这和 CPU 异常 0~31 冲突。
 * 所以常见做法是把主从 PIC 改到 32~47。
 */
#define IRQ_BASE 32

typedef struct {
    /*
     * 这是 32 位模式下的 IDT 表项格式（8 字节）。
     *
     * offset_low  + offset_high:
     *   组成中断处理函数的 32 位入口地址
     *
     * selector:
     *   代码段选择子，告诉 CPU 跳到哪个代码段去执行处理函数
     *
     * zero:
     *   这一字节在中断门/陷阱门里必须为 0
     *
     * type_attr:
     *   各 bit 含义如下：
     *   - bit 7: P（Present），1 表示该门有效
     *   - bit 6~5: DPL（门描述符特权级）
     *   - bit 4: 对中断门/陷阱门固定为 0
     *   - bit 3~0: Gate Type
     *       1110b = 32 位中断门（interrupt gate）
     *       1111b = 32 位陷阱门（trap gate）
     *
     * 本项目里常用的 0x8E = 1000 1110b：
     * - P=1
     * - DPL=00（只有内核态可直接调用）
     * - Type=1110（32 位中断门）
     */
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed)) IdtEntry;//禁止编译器对齐结构体

typedef struct {
    /*
     * lidt 指令需要的“伪描述符”格式：
     * - limit: IDT 总字节数 - 1
     * - base : IDT 在线性地址空间中的起始地址
     */
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) IdtPointer;

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
//对应CPU异常或特权异常（如除0/越界），一般汇编编写
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);
extern void isr128(void);
//硬件中断请求（如键盘输入）
static IdtEntry idt[IDT_ENTRIES];
static IdtPointer idt_ptr;
/* IRQ0~IRQ15 各自对应一条 C 处理函数。 */
static irq_handler_t irq_handlers[16];
static uint16_t kernel_code_selector = 0;

/* CPU 异常号 0~31 的人类可读名字，方便 panic 时输出。 */
static const char* exception_messages[32] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

static void idt_set_gate(uint8_t vector, uint32_t handler, uint16_t selector, uint8_t type_attr) {
    /* 把 32 位入口地址拆成低 16 位和高 16 位写进 IDT 表项。 */
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = selector;
    idt[vector].zero = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_high = (handler >> 16) & 0xFFFF;
}

static void idt_load(void) {
    /* 
    lidt 把 idt_ptr 指向的描述符装进 CPU 的 IDTR 寄存器。
    后注：asm表示汇编内联，volatile表示不要优化，
    括号内是一条汇编指令：
    lidt表示load idt，把idt加载进内存中。
    %0代表这里填入的是输入中的第一个操作数，也就是后面的"m"(idt_ptr)。
    asm的格式应当是asm(模板 : 输出 : 输入 : clobber)，
    clobber代表什么可能被修改，比如说__asm__("mov $1, %eax");
    如果没有clobber，编译器就不知道eax被修改了，所以一定要写成__asm__("mov $1, %%eax" : : : "eax");
    这里我们的输入是"m"(idt_ptr)，m是一个页数，代表后面的东西是一个变量，要从内存中去读取
    */
    __asm__ __volatile__("lidt %0" : : "m"(idt_ptr)); 
}

static uint16_t read_cs(void) {
    uint16_t cs;
    /*
     * 读取当前代码段选择子。
     * 这里不能硬编码 0x08，因为当前引导环境（比如 GRUB 设置的 GDT）
     * 不一定使用那组段号。之前就是这里写死导致中断一开就 #GP。
     * 后注：将cs寄存器中的值传入第零个操作数，即后面的cs变量，=代表是一个输出，r代表需要一个寄存器来接受输出
     */
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static void pic_remap(void) {
    /* 先保存原屏蔽位，避免重映射时把原本的使能状态弄丢。 */
    // 通过PICX_DATA这个端口去访问IMR(interrupt mask register)，读取中断屏蔽寄存器
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    //初始化的时候发四个ICW，第一个发到命令端口，后三个发到数据端口
    /*
     * 0x11 = ICW1:
     * - bit4=1: 表示开始初始化 PIC
     * - bit0=1: 后面还会继续发送 ICW4
     */
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    /* ICW2：设置中断向量起始号。主片从 32 开始，从片从 40 开始。 */
    outb(PIC1_DATA, IRQ_BASE);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);
    io_wait();

    /*
     * ICW3：说明主从 PIC 的级联关系。
     * - 主 PIC 写 0x04 = 0000 0100b，表示 IRQ2 上接了一片从 PIC
     * - 从 PIC 写 0x02，表示自己挂在主 PIC 的 IRQ2 上
     */
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    /*
     * ICW4：0x01 表示使用 8086/88 模式。
     * 这是现代 PC 上最常见、也是我们现在需要的模式。
     */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* 最后把原来的 mask 恢复回去。 */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

static void pic_set_masks(uint8_t master_mask, uint8_t slave_mask) {
    /*
     * PIC 的 mask 位语义：
     * - 某一位 = 1：屏蔽该 IRQ（不让它进来）
     * - 某一位 = 0：允许该 IRQ
     */
    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

static void pic_send_eoi(uint8_t irq) { 
    /*
     * 如果是从 PIC（IRQ8~15）发来的中断，需要先通知从片，
     * 再通知主片。否则 PIC 会认为这条中断还没处理完。
     */
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

static uint32_t read_cr2(void) {
    uint32_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static void print_page_fault_error(uint32_t err_code) {
    console_write("PF reason:");
    if (!(err_code & 0x01)) {
        console_write(" not-present");
    } else {
        console_write(" protection");
    }
    if (err_code & 0x02) {
        console_write(" write");
    } else {
        console_write(" read");
    }
    if (err_code & 0x04) {
        console_write(" user");
    } else {
        console_write(" kernel");
    }
    if (err_code & 0x08) {
        console_write(" reserved-bit");
    }
    if (err_code & 0x10) {
        console_write(" instruction-fetch");
    }
    console_put_char('\n');
}

static void kernel_panic(InterruptFrame* frame) { // 发生无法处理的内核错误，打印错误信息并停机
    console_set_color(0x0F, 0x04);
    console_write_line("");
    console_write_line("KERNEL PANIC");
    console_write("Exception: ");
    console_write_line(exception_messages[frame->int_no]);
    console_write("Vector: ");
    console_write_dec((int)frame->int_no);
    console_put_char('\n');
    console_write("Error code: ");
    console_write_hex(frame->err_code);
    console_put_char('\n');
    console_write("EIP: ");
    console_write_hex(frame->eip);
    console_put_char('\n');
    console_write("CS: ");
    console_write_hex(frame->cs);
    console_put_char('\n');
    console_write("EFLAGS: ");
    console_write_hex(frame->eflags);
    console_put_char('\n');

    if (frame->int_no == 14) {
        console_write("CR2: ");
        console_write_hex(read_cr2());
        console_put_char('\n');
        print_page_fault_error(frame->err_code);
    }

    interrupts_disable();
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

void interrupts_init(void) {
    kernel_code_selector = read_cs();

    /* 先把整个 IDT 清空，未使用的向量默认都无效。 */
    for (size_t i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    /* 0~31 是 CPU 异常入口。 */
    idt_set_gate(0, (uint32_t)isr0, kernel_code_selector, 0x8E);
    idt_set_gate(1, (uint32_t)isr1, kernel_code_selector, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, kernel_code_selector, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, kernel_code_selector, 0x8E);
    idt_set_gate(4, (uint32_t)isr4, kernel_code_selector, 0x8E);
    idt_set_gate(5, (uint32_t)isr5, kernel_code_selector, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, kernel_code_selector, 0x8E);
    idt_set_gate(7, (uint32_t)isr7, kernel_code_selector, 0x8E);
    idt_set_gate(8, (uint32_t)isr8, kernel_code_selector, 0x8E);
    idt_set_gate(9, (uint32_t)isr9, kernel_code_selector, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, kernel_code_selector, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, kernel_code_selector, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, kernel_code_selector, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, kernel_code_selector, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, kernel_code_selector, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, kernel_code_selector, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, kernel_code_selector, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, kernel_code_selector, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, kernel_code_selector, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, kernel_code_selector, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, kernel_code_selector, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, kernel_code_selector, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, kernel_code_selector, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, kernel_code_selector, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, kernel_code_selector, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, kernel_code_selector, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, kernel_code_selector, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, kernel_code_selector, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, kernel_code_selector, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, kernel_code_selector, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, kernel_code_selector, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, kernel_code_selector, 0x8E);

    /* 把原始 IRQ0~15 挪到 32~47，避免和异常向量冲突。 */
    pic_remap();

    /* 32~47 是重映射后的硬件中断入口。 */
    idt_set_gate(IRQ_BASE + 0, (uint32_t)irq0, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 1, (uint32_t)irq1, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 2, (uint32_t)irq2, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 3, (uint32_t)irq3, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 4, (uint32_t)irq4, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 5, (uint32_t)irq5, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 6, (uint32_t)irq6, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 7, (uint32_t)irq7, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 8, (uint32_t)irq8, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 9, (uint32_t)irq9, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 10, (uint32_t)irq10, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 11, (uint32_t)irq11, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 12, (uint32_t)irq12, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 13, (uint32_t)irq13, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 14, (uint32_t)irq14, kernel_code_selector, 0x8E);
    idt_set_gate(IRQ_BASE + 15, (uint32_t)irq15, kernel_code_selector, 0x8E);

    /*
     * 0x80：演示用系统调用入口。
     * type_attr = 0xEE:
     * - P=1
     * - DPL=3，允许用户态通过 int 0x80 主动进入
     * - 32 位中断门
     */
    idt_set_gate(0x80, (uint32_t)isr128, kernel_code_selector, 0xEE);

    /* 初始时所有 IRQ 都还没有注册 C 处理函数。 */
    for (size_t irq = 0; irq < 16; irq++) {
        irq_handlers[irq] = 0;
    }

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base = (uint32_t)&idt;

    idt_load();

    /*
     * 主 PIC mask = 0xF8 = 1111 1000b
     * - IRQ0(时钟)、IRQ1(键盘)、IRQ2(级联到从 PIC) 允许
     * - 其余 IRQ3~7 暂时屏蔽
     *
     * 从 PIC mask = 0xFF = 1111 1111b
     * - IRQ8~15 暂时全部屏蔽
     */
    pic_set_masks(0xF8, 0xFF);
}

void interrupts_enable(void) {
    /* sti：把 EFLAGS.IF 置 1，允许可屏蔽中断进来。 */
    __asm__ __volatile__("sti");
}

void interrupts_disable(void) {
    /* cli：把 EFLAGS.IF 清 0，禁止可屏蔽中断。 */
    __asm__ __volatile__("cli");
}

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void isr_dispatch(InterruptFrame* frame) {
    if (frame->int_no == 0x80) {
        usermode_handle_syscall(frame);
        return;
    }

    if (frame->int_no == 14) {
        if (pager_handle_page_fault(read_cr2(), frame->err_code)) {
            return;
        }
    }

    /* 小于 32 的向量号表示 CPU 异常。 */
    if (frame->int_no < IRQ_BASE) {
        kernel_panic(frame);//控制台报错，系统停止
        return;
    }

    /* 32~47 是我们当前支持的硬件 IRQ。 */
    if (frame->int_no < IRQ_BASE + 16) {
        uint8_t irq = (uint8_t)(frame->int_no - IRQ_BASE);
        irq_handler_t handler = irq_handlers[irq];

        if (handler) {
            handler(frame);
        }

        pic_send_eoi(irq);
    }
}
