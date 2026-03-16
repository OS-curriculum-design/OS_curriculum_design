#include "shell.h"
#include "../console/console.h"
#include "../drivers/mouse.h"
#include "../include/string.h"
#include "../memory/memory.h"
#include "../timer/timer.h"

#define INPUT_MAX 128

static char input_buffer[INPUT_MAX];
static int input_len = 0;

static int parse_uint(const char* str, uint32_t* out) {
    uint32_t value = 0;

    if (!str[0]) return 0;

    for (size_t i = 0; str[i]; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;
        value = value * 10 + (uint32_t)(str[i] - '0');
    }

    *out = value;
    return 1;
}

static void run_command(const char* cmd) {
    if (strcmp(cmd, "") == 0) return;

    if (strcmp(cmd, "help") == 0) {
        console_write_line("Commands:");
        console_write_line("  help");
        console_write_line("  clear");
        console_write_line("  version");
        console_write_line("  about");
        console_write_line("  echo <text>");
        console_write_line("  mouse");
        console_write_line("  mouse on");
        console_write_line("  mouse off");
        console_write_line("  ticks");
        console_write_line("  slice");
        console_write_line("  slice <ticks>");
        console_write_line("  mem");
        console_write_line("  mem help");
        return;
    }

    if (strcmp(cmd, "clear") == 0) {
        console_clear();
        return;
    }

    if (strcmp(cmd, "version") == 0) {
        console_write_line("MyOS version 0.3");
        return;
    }

    if (strcmp(cmd, "about") == 0) {
        console_write_line("MyOS: simple educational operating system.");
        console_write_line("Stage 3: memory management simulator.");
        return;
    }

    if (strncmp(cmd, "echo ", 5) == 0) {
        console_write_line(cmd + 5);
        return;
    }

    if (strcmp(cmd, "mouse") == 0) {
        MouseState ms = mouse_get_state();
        console_write("Mouse: x=");
        console_write_dec(ms.x);
        console_write(" y=");
        console_write_dec(ms.y);
        console_write(" left=");
        console_write_dec(ms.left);
        console_write(" right=");
        console_write_dec(ms.right);
        console_write(" middle=");
        console_write_dec(ms.middle);
        console_put_char('\n');
        return;
    }

    if (strcmp(cmd, "mouse on") == 0) {
        mouse_set_enabled(1);
        console_write_line("Mouse enabled.");
        return;
    }

    if (strcmp(cmd, "mouse off") == 0) {
        mouse_set_enabled(0);
        console_write_line("Mouse disabled.");
        return;
    }

    if (strcmp(cmd, "ticks") == 0) {
        console_write("Timer ticks: ");
        console_write_dec((int)timer_get_ticks());
        console_put_char('\n');
        return;
    }

    if (strcmp(cmd, "slice") == 0) {
        console_write("Round-robin time slice: ");
        console_write_dec((int)timer_get_timeslice());
        console_write_line(" ticks");
        return;
    }

    if (strncmp(cmd, "slice ", 6) == 0) {
        uint32_t ticks = 0;
        if (!parse_uint(cmd + 6, &ticks)) {
            console_write_line("Usage: slice <ticks>");
            return;
        }

        timer_set_timeslice(ticks);
        console_write("Time slice updated to ");
        console_write_dec((int)timer_get_timeslice());
        console_write_line(" ticks");
        return;
    }

    if (strcmp(cmd, "mem") == 0) {
        memory_manager_print_status();
        return;
    }

    if (strcmp(cmd, "mem help") == 0) {
        memory_manager_print_help();
        return;
    }

    if (strcmp(cmd, "mem start") == 0) {
        memory_manager_start();
        console_write_line("Memory simulation started.");
        return;
    }

    if (strcmp(cmd, "mem stop") == 0) {
        memory_manager_stop();
        console_write_line("Memory simulation stopped.");
        return;
    }

    if (strcmp(cmd, "mem reset") == 0) {
        memory_manager_reset();
        console_write_line("Memory simulation reset.");
        return;
    }

    if (strcmp(cmd, "mem log") == 0) {
        memory_manager_print_log();
        return;
    }

    if (strcmp(cmd, "mem compare") == 0) {
        memory_manager_run_compare();
        return;
    }

    if (strncmp(cmd, "mem mode ", 9) == 0) {
        if (!memory_manager_set_mode(cmd + 9)) {
            console_write_line("Usage: mem mode bestfit|segpage|vm");
            return;
        }

        console_write("Memory mode set to ");
        console_write_line(memory_manager_mode_name());
        return;
    }

    if (strncmp(cmd, "mem step ", 9) == 0) {
        uint32_t cycles = 0;
        if (!parse_uint(cmd + 9, &cycles)) {
            console_write_line("Usage: mem step <cycles>");
            return;
        }

        memory_manager_step(cycles);
        console_write("Memory simulation advanced by ");
        console_write_dec((int)cycles);
        console_write_line(" cycles");
        return;
    }

    if (strncmp(cmd, "mem pages ", 10) == 0) {
        uint32_t pages = 0;
        if (!parse_uint(cmd + 10, &pages) || !memory_manager_set_user_pages(pages)) {
            console_write_line("Usage: mem pages <4-32>");
            return;
        }

        console_write("User page count set to ");
        console_write_dec((int)pages);
        console_put_char('\n');
        return;
    }

    if (strncmp(cmd, "mem frames ", 11) == 0) {
        uint32_t frames = 0;
        if (!parse_uint(cmd + 11, &frames) || !memory_manager_set_physical_frames(frames)) {
            console_write_line("Usage: mem frames <4-32>");
            return;
        }

        console_write("Physical frame count set to ");
        console_write_dec((int)frames);
        console_put_char('\n');
        return;
    }

    console_write("Unknown command: ");
    console_write_line(cmd);
}

void shell_init(void) {
    input_len = 0;
    memset(input_buffer, 0, INPUT_MAX);
}

void shell_prompt(void) {
    console_write("MyOS> ");
}

void shell_handle_char(char c) {
    if (c == '\n') {
        console_put_char('\n');
        input_buffer[input_len] = '\0';
        run_command(input_buffer);
        input_len = 0;
        memset(input_buffer, 0, INPUT_MAX);
        shell_prompt();
        return;
    }

    if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            input_buffer[input_len] = '\0';
            console_put_char('\b');
        }
        return;
    }

    if (c < 32 || c > 126) return;
    if (input_len >= INPUT_MAX - 1) return;

    input_buffer[input_len++] = c;
    console_put_char(c);
}
