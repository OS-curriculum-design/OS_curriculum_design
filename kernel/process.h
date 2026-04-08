#ifndef PROCESS_H
#define PROCESS_H

#include "../include/types.h"
#include "../interrupt/interrupts.h"

typedef enum {
    PROCESS_UNUSED = 0,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_ZOMBIE
} ProcessState;

void process_init(void);
int process_spawn_builtin(const char* name);
int process_schedule(void);
int process_schedule_auto(void);
int process_run_builtin(const char* name);
int process_has_ready(void);
int process_auto_schedule_enabled(void);
void process_set_auto_schedule(int enabled);
int process_reap_zombies(void);
void process_print_table(void);
void process_save_yield_frame(InterruptFrame* frame);
int process_preempt_if_needed(InterruptFrame* frame);

#endif
