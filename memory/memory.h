#ifndef MEMORY_H
#define MEMORY_H

#include "../include/types.h"

void memory_manager_init(void);
void memory_manager_tick(void);

void memory_manager_start(void);
void memory_manager_stop(void);
int memory_manager_is_running(void);

int memory_manager_set_mode(const char* name);
const char* memory_manager_mode_name(void);

int memory_manager_set_user_pages(uint32_t pages);
int memory_manager_set_physical_frames(uint32_t frames);

void memory_manager_reset(void);
void memory_manager_step(uint32_t cycles);
void memory_manager_print_help(void);
void memory_manager_print_status(void);
void memory_manager_print_log(void);
void memory_manager_run_compare(void);

#endif
