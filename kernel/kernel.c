/*
 * 内核主入口
 * ==========
 * 这里串起整个系统的初始化顺序，并在最后进入主循环处理输入与调度。
 */
#include "../console/console.h"
#include "../drivers/ata.h"
#include "../drivers/keyboard.h"
#include "../fs/simplefs.h"
#include "../interrupt/interrupts.h"
#include "../mm/pager.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "gdt.h"
#include "process.h"
#include "../shell/shell.h"
#include "../timer/timer.h"

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    /* 关键表项还没装好前先关中断，避免外设抢先进入。 */
    interrupts_disable();
    gdt_init();

    /* 尽早准备控制台，保证后续错误信息可见。 */
    console_set_color(0x0F, 0x00);
    console_clear();

    /* 内存管理是后续大多数模块的基础。 */
    pmm_init(multiboot_magic, multiboot_info_addr);
    if (!vmm_init()) {
        console_write_line("VMM init failed.");
        while (1) {
            __asm__ __volatile__("hlt");
        }
    }

    /* 建立中断、存储、调度与交互各子系统。 */
    interrupts_init();
    ata_init();
    simplefs_init();
    pager_init();
    process_init();
    keyboard_init();
    timer_init(100);
    shell_init();
    interrupts_enable();

    shell_prompt();

    while (1) {
        uint16_t key;

        /* 先处理用户输入，保持 Shell 响应及时。 */
        while (keyboard_read_key(&key)) {
            shell_handle_key(key);
        }

        /* 再处理由时钟中断累计下来的调度事件。 */
        while (timer_take_schedule_event()) {
            if (process_auto_schedule_enabled() && process_has_ready()) {
                shell_begin_async_output();
                process_schedule_auto();
                shell_end_async_output();
            }
        }

        /*
         * 没事可做时执行 sti; hlt：
         * - sti 重新允许中断
         * - hlt 让 CPU 睡眠到下一次中断到来
         */
        interrupts_disable();
        if (!keyboard_has_key() && !timer_has_schedule_event()) {
            __asm__ __volatile__("sti\n\thlt" : : : "memory");
        } else {
            interrupts_enable();
        }
    }
}
