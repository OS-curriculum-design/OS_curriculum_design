#include "timer.h"
#include "../drivers/io.h"
#include "../interrupt/interrupts.h"

/* PIT（8253/8254）相关端口。 */
#define PIT_COMMAND_PORT 0x43
#define PIT_CHANNEL0_PORT 0x40

/*
 * PIT 的输入时钟基频约为 1.193182 MHz。
 * 想得到目标频率，就用 divisor = 1193182 / frequency_hz。
 */
#define PIT_BASE_FREQUENCY 1193182

/* 这些变量会被中断处理程序和主循环共同访问，所以用 volatile。 */
static volatile uint32_t timer_ticks = 0;
static volatile uint32_t timer_frequency = 100;
static volatile uint32_t timeslice_ticks = 10;
static volatile uint32_t slice_remaining = 10;
static volatile uint32_t schedule_event_pending = 0;

static uint32_t interrupt_save_and_disable(void) {
    uint32_t flags;
    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

static void interrupt_restore(uint32_t flags) {
    __asm__ __volatile__(
        "pushl %0\n\t"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc");
}

static void timer_irq_handler(InterruptFrame* frame) {
    (void)frame;

    /* 每进一次 IRQ0，就说明系统时钟走了一拍。 */
    timer_ticks++;

    if (slice_remaining > 0) {
        slice_remaining--;
    }

    /*
     * 时间片耗尽时，不直接切换任务，而是先立一个“该调度了”的标志。
     * 这样后续你做 RR 调度时，可以把真正的切换逻辑接在这里后面。
     */
    if (slice_remaining == 0) {
        schedule_event_pending++;
        slice_remaining = timeslice_ticks;
    }
}

void timer_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    }

    timer_frequency = frequency_hz;
    timer_ticks = 0;
    schedule_event_pending = 0;
    slice_remaining = timeslice_ticks;

    /* IRQ0 就是 PIT 时钟中断。 */
    irq_register_handler(0, timer_irq_handler);

    /*
     * 0x36 = 0011 0110b，对应 PIT 控制字：
     * - bits 7~6 = 00: 选择通道 0
     * - bits 5~4 = 11: 先写低字节，再写高字节
     * - bits 3~1 = 011: 工作模式 3（方波发生器）
     * - bit 0   = 0 : 16 位二进制计数
     */
    outb(PIT_COMMAND_PORT, 0x36);
    outb(PIT_CHANNEL0_PORT, divisor & 0xFF);
    outb(PIT_CHANNEL0_PORT, (divisor >> 8) & 0xFF);
}

uint32_t timer_get_ticks(void) {
    return timer_ticks;
}

uint32_t timer_get_frequency(void) {
    return timer_frequency;
}

void timer_set_timeslice(uint32_t ticks) {
    /* 时间片不能是 0；这里最小强制为 1 tick。 */
    if (ticks == 0) {
        ticks = 1;
    }

    timeslice_ticks = ticks;
    slice_remaining = ticks;
    schedule_event_pending = 0;
}

uint32_t timer_get_timeslice(void) {
    return timeslice_ticks;
}

int timer_take_schedule_event(void) {
    /*
     * 这里要把“检查 + 消费”放进同一个临界区里，
     * 否则可能在两步之间被时钟中断打断，导致丢掉一次调度事件。
     */
    uint32_t flags = interrupt_save_and_disable();

    if (schedule_event_pending == 0) {
        interrupt_restore(flags);
        return 0;
    }

    schedule_event_pending--;
    interrupt_restore(flags);
    return 1;
}
