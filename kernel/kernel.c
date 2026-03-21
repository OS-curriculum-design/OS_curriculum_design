#include "../console/console.h"
#include "../drivers/keyboard.h"
#include "../interrupt/interrupts.h"
#include "gdt.h"
#include "../shell/shell.h"
#include "../timer/timer.h"

void kernel_main(void) {
    interrupts_disable();
    gdt_init();

    console_set_color(0x0F, 0x00);
    console_clear();

    interrupts_init();
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

        if (timer_take_schedule_event()) {
            /* Scheduler hook for future RR task switching. */
        }

        __asm__ __volatile__("hlt");
    }
}
