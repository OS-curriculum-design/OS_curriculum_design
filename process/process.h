#ifndef PROCESS_H
#define PROCESS_H

#include "../include/types.h"

#define MAX_PROCS 16
#define PROC_NAME_LEN 16

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_TERMINATED
} ProcessState;

typedef enum {
    SCHED_FCFS = 0,
    SCHED_SJF,
    SCHED_RR,
    SCHED_PRIORITY
} SchedulerType;

typedef struct {
    int used;
    int pid;
    int arrival_order;
    char name[PROC_NAME_LEN];
    int burst_time;
    int remaining_time;
    int priority;
    ProcessState state;
} PCB;

void process_init(void);

int process_create(const char* name, int burst_time, int priority);
int process_kill(int pid);
int process_block(int pid);
int process_wakeup(int pid);

void process_list(void);
void process_run(void);

void process_set_scheduler(SchedulerType type);
SchedulerType process_get_scheduler(void);
const char* process_get_scheduler_name(void);

void process_set_rr_quantum(int q);
int process_get_rr_quantum(void);

#endif