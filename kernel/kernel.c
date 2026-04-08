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
    interrupts_disable();
    gdt_init();

    console_set_color(0x0F, 0x00);
    console_clear();

    pmm_init(multiboot_magic, multiboot_info_addr);
    if (!vmm_init()) {
        console_write_line("VMM init failed.");
        while (1) {
            __asm__ __volatile__("hlt");
        }
    }

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
        char c;

        while (keyboard_read_char(&c)) {
            shell_handle_char(c);
        }

        while (timer_take_schedule_event()) {
            if (process_auto_schedule_enabled() && process_has_ready()) {
                shell_begin_async_output();
                process_schedule_auto();
                shell_end_async_output();
            }
        }

        interrupts_disable();
        if (!keyboard_has_char() && !timer_has_schedule_event()) {
            __asm__ __volatile__("sti\n\thlt" : : : "memory");
        } else {
            interrupts_enable();
        }
    }
}
