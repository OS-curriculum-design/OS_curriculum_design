#include "shell.h"
#include "../console/console.h"
#include "../drivers/mouse.h"
#include "../include/string.h"
#include "../process/process.h"

#define INPUT_MAX 128
#define MAX_ARGS  8

static char input_buffer[INPUT_MAX];
static int input_len = 0;

static int tokenize(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* p = line;

    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;

        argv[argc++] = p;

        while (*p && *p != ' ') p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

static void show_help(void) {
    console_write_line("Commands:");
    console_write_line("  help");
    console_write_line("  clear");
    console_write_line("  version");
    console_write_line("  about");
    console_write_line("  echo <text>");
    console_write_line("  mouse");
    console_write_line("  mouse on");
    console_write_line("  mouse off");
    console_write_line("  ps");
    console_write_line("  exec <name> <burst> <priority>");
    console_write_line("  kill <pid>");
    console_write_line("  block <pid>");
    console_write_line("  wakeup <pid>");
    console_write_line("  sched fcfs|sjf|rr|prio");
    console_write_line("  quantum <n>");
    console_write_line("  run");
}

static void run_command(const char* cmdline_const) {
    char cmdline[INPUT_MAX];
    char* argv[MAX_ARGS];
    int argc;

    strncpy(cmdline, cmdline_const, INPUT_MAX - 1);
    cmdline[INPUT_MAX - 1] = '\0';

    argc = tokenize(cmdline, argv, MAX_ARGS);
    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0) {
        show_help();
        return;
    }

    if (strcmp(argv[0], "clear") == 0) {
        console_clear();
        return;
    }

    if (strcmp(argv[0], "version") == 0) {
        console_write_line("MyOS version 0.4");
        console_write_line("Priority scheduling added.");
        return;
    }

    if (strcmp(argv[0], "about") == 0) {
        console_write_line("MyOS: simple educational operating system.");
        console_write_line("Current stage: CLI + mouse + process scheduling.");
        return;
    }

    if (strcmp(argv[0], "echo") == 0) {
        if (argc >= 2) {
            for (int i = 1; i < argc; i++) {
                console_write(argv[i]);
                if (i != argc - 1) console_put_char(' ');
            }
            console_put_char('\n');
        }
        return;
    }

    if (strcmp(argv[0], "mouse") == 0) {
        if (argc == 1) {
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

        if (argc == 2 && strcmp(argv[1], "on") == 0) {
            mouse_set_enabled(1);
            console_write_line("Mouse enabled.");
            return;
        }

        if (argc == 2 && strcmp(argv[1], "off") == 0) {
            mouse_set_enabled(0);
            console_write_line("Mouse disabled.");
            return;
        }

        console_write_line("Usage: mouse | mouse on | mouse off");
        return;
    }

    if (strcmp(argv[0], "ps") == 0) {
        process_list();
        return;
    }

    if (strcmp(argv[0], "exec") == 0) {
        if (argc != 4) {
            console_write_line("Usage: exec <name> <burst> <priority>");
            return;
        }
        process_create(argv[1], atoi(argv[2]), atoi(argv[3]));
        return;
    }

    if (strcmp(argv[0], "kill") == 0) {
        if (argc != 2) {
            console_write_line("Usage: kill <pid>");
            return;
        }
        process_kill(atoi(argv[1]));
        return;
    }

    if (strcmp(argv[0], "block") == 0) {
        if (argc != 2) {
            console_write_line("Usage: block <pid>");
            return;
        }
        process_block(atoi(argv[1]));
        return;
    }

    if (strcmp(argv[0], "wakeup") == 0) {
        if (argc != 2) {
            console_write_line("Usage: wakeup <pid>");
            return;
        }
        process_wakeup(atoi(argv[1]));
        return;
    }

    if (strcmp(argv[0], "sched") == 0) {
        if (argc != 2) {
            console_write_line("Usage: sched fcfs|sjf|rr|prio");
            return;
        }

        if (strcmp(argv[1], "fcfs") == 0) {
            process_set_scheduler(SCHED_FCFS);
            console_write_line("Scheduler set to FCFS.");
            return;
        }

        if (strcmp(argv[1], "sjf") == 0) {
            process_set_scheduler(SCHED_SJF);
            console_write_line("Scheduler set to SJF.");
            return;
        }

        if (strcmp(argv[1], "rr") == 0) {
            process_set_scheduler(SCHED_RR);
            console_write_line("Scheduler set to RR.");
            return;
        }

        if (strcmp(argv[1], "prio") == 0 || strcmp(argv[1], "priority") == 0) {
            process_set_scheduler(SCHED_PRIORITY);
            console_write_line("Scheduler set to PRIORITY.");
            return;
        }

        console_write_line("Unknown scheduler. Use fcfs|sjf|rr|prio.");
        return;
    }

    if (strcmp(argv[0], "quantum") == 0) {
        if (argc != 2) {
            console_write_line("Usage: quantum <n>");
            return;
        }

        int q = atoi(argv[1]);
        if (q <= 0) {
            console_write_line("Error: quantum must be > 0.");
            return;
        }

        process_set_rr_quantum(q);
        console_write("RR quantum set to ");
        console_write_dec(process_get_rr_quantum());
        console_put_char('\n');
        return;
    }

    if (strcmp(argv[0], "run") == 0) {
        process_run();
        return;
    }

    console_write("Unknown command: ");
    console_write_line(argv[0]);
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