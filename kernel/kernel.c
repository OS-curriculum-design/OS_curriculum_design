#include "../console/console.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../shell/shell.h"
#include "../process/process.h"

static void draw_status_bar(void) {
    MouseState ms = mouse_get_state();
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

    console_write_at(ms.enabled ? " ON " : " OFF", 0, 72, bar_color);
}

void kernel_main(void) {
    console_set_color(0x0F, 0x00);
    console_clear();

    keyboard_init();
    mouse_init();
    process_init();
    shell_init();

    //draw_status_bar();

    console_write_line("========================================");
    console_write_line("        Welcome to MyOS Kernel");
    console_write_line("========================================");
    console_write_line("[OK] Booted by GRUB");
    console_write_line("[OK] Entered 32-bit kernel_main()");
    console_write_line("[OK] Keyboard initialized");
    console_write_line("[OK] Mouse initialized");
    console_write_line("[OK] Process manager initialized");
    console_write_line("[OK] Command line ready");
    console_write_line("");
    console_write_line("Type 'help' to see commands.");
    shell_prompt();

    while (1) {
        char c;

        mouse_poll();
        //draw_status_bar();

        if (keyboard_read_char(&c)) {
            shell_handle_char(c);
        }
    }
}