#include "../console/console.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../interrupt/interrupts.h"
#include "../memory/memory.h"
#include "../shell/shell.h"
#include "../timer/timer.h"

static void write_dec_at(uint32_t value, size_t row, size_t col, uint8_t color) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        console_put_char_at('0', row, col, color);
        return;
    }

    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i--) {
        console_put_char_at(buf[i], row, col++, color);
    }
}

static void draw_status_bar(void) {
    MouseState ms = mouse_get_state();
    uint32_t ticks = timer_get_ticks();
    uint8_t bar_color = 0x1F;

    console_clear_line(0, bar_color);
    console_write_at(" MyOS Status ", 0, 0, bar_color);
    console_write_at("Mouse:", 0, 20, bar_color);

    int pos = 27;

    console_write_at("X=", 0, pos, bar_color); pos += 2;
    if (ms.x >= 10) console_put_char_at('0' + (ms.x / 10), 0, pos++, bar_color);
    console_put_char_at('0' + (ms.x % 10), 0, pos++, bar_color);

    console_write_at(" Y=", 0, pos, bar_color); pos += 3;
    if (ms.y >= 10) console_put_char_at('0' + (ms.y / 10), 0, pos++, bar_color);
    console_put_char_at('0' + (ms.y % 10), 0, pos++, bar_color);

    console_write_at(" L=", 0, pos, bar_color); pos += 3;
    console_put_char_at(ms.left ? '1' : '0', 0, pos++, bar_color);

    console_write_at(" R=", 0, pos, bar_color); pos += 3;
    console_put_char_at(ms.right ? '1' : '0', 0, pos++, bar_color);

    console_write_at(" M=", 0, pos, bar_color); pos += 3;
    console_put_char_at(ms.middle ? '1' : '0', 0, pos++, bar_color);

    console_write_at(" T=", 0, 58, bar_color);
    write_dec_at(ticks, 0, 61, bar_color);
    console_write_at(ms.enabled ? " ON " : " OFF", 0, 72, bar_color);
}

static int should_refresh_status(uint32_t* last_tick) {
    uint32_t current_tick = timer_get_ticks();
    if (current_tick - *last_tick >= 10) {
        *last_tick = current_tick;
        return 1;
    }
    return 0;
}

void kernel_main(void) {
    console_set_color(0x0F, 0x00);
    console_clear();

    interrupts_disable();
    interrupts_init();
    keyboard_init();
    mouse_init();
    timer_init(100);
    memory_manager_init();
    shell_init();
    interrupts_enable();

    draw_status_bar();

    console_write_line("========================================");
    console_write_line("        Welcome to MyOS Kernel");
    console_write_line("========================================");
    console_write_line("[OK] Booted by GRUB");
    console_write_line("[OK] Entered 32-bit kernel_main()");
    console_write_line("[OK] Keyboard initialized");
    console_write_line("[OK] Mouse initialized");
    console_write_line("[OK] PIT timer initialized at 100 Hz");
    console_write_line("[OK] Memory manager initialized");
    console_write_line("[OK] Interrupts enabled");
    console_write_line("[OK] Command line ready");
    console_write_line("");
    console_write_line("Type 'help' to see commands.");
    shell_prompt();

    uint32_t last_tick = timer_get_ticks();

    while (1) {
        char c;

        while (keyboard_read_char(&c)) {
            shell_handle_char(c);
        }

        if (mouse_consume_update() || should_refresh_status(&last_tick)) {
            draw_status_bar();
        }

        if (timer_take_schedule_event()) {
            memory_manager_tick();
        }

        __asm__ __volatile__("hlt");
    }
}
