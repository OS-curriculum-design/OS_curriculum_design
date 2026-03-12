#include "process.h"
#include "../console/console.h"
#include "../include/string.h"

static PCB pcb_table[MAX_PROCS];
static int next_pid = 1;
static int next_arrival = 1;
static SchedulerType current_scheduler = SCHED_FCFS;
static int rr_quantum = 2;
static int rr_last_index = -1;

static const char* state_to_string(ProcessState state) {
    switch (state) {
        case PROC_READY: return "READY";
        case PROC_RUNNING: return "RUNNING";
        case PROC_BLOCKED: return "BLOCKED";
        case PROC_TERMINATED: return "TERMINATED";
        default: return "UNUSED";
    }
}

static int find_pcb_index_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (pcb_table[i].used && pcb_table[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int has_ready_process(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (pcb_table[i].used && pcb_table[i].state == PROC_READY) {
            return 1;
        }
    }
    return 0;
}

static int has_blocked_process(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (pcb_table[i].used && pcb_table[i].state == PROC_BLOCKED) {
            return 1;
        }
    }
    return 0;
}

static int find_next_fcfs(void) {
    int best = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!pcb_table[i].used || pcb_table[i].state != PROC_READY) continue;
        if (best == -1 || pcb_table[i].arrival_order < pcb_table[best].arrival_order) {
            best = i;
        }
    }
    return best;
}

static int find_next_sjf(void) {
    int best = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!pcb_table[i].used || pcb_table[i].state != PROC_READY) continue;

        if (best == -1) {
            best = i;
            continue;
        }

        if (pcb_table[i].remaining_time < pcb_table[best].remaining_time) {
            best = i;
        } else if (pcb_table[i].remaining_time == pcb_table[best].remaining_time &&
                   pcb_table[i].arrival_order < pcb_table[best].arrival_order) {
            best = i;
        }
    }
    return best;
}

static int find_next_priority(void) {
    int best = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!pcb_table[i].used || pcb_table[i].state != PROC_READY) continue;

        if (best == -1) {
            best = i;
            continue;
        }

        /* 数值越大，优先级越高 */
        if (pcb_table[i].priority > pcb_table[best].priority) {
            best = i;
        } else if (pcb_table[i].priority == pcb_table[best].priority &&
                   pcb_table[i].arrival_order < pcb_table[best].arrival_order) {
            best = i;
        }
    }
    return best;
}

static int find_next_rr(void) {
    for (int step = 1; step <= MAX_PROCS; step++) {
        int idx = (rr_last_index + step) % MAX_PROCS;
        if (pcb_table[idx].used && pcb_table[idx].state == PROC_READY) {
            rr_last_index = idx;
            return idx;
        }
    }
    return -1;
}

static void print_proc_line(const PCB* p) {
    console_write("PID=");
    console_write_dec(p->pid);
    console_write(" Name=");
    console_write(p->name);
    console_write(" State=");
    console_write(state_to_string(p->state));
    console_write(" Burst=");
    console_write_dec(p->burst_time);
    console_write(" Rem=");
    console_write_dec(p->remaining_time);
    console_write(" Prio=");
    console_write_dec(p->priority);
    console_write(" Arr=");
    console_write_dec(p->arrival_order);
    console_put_char('\n');
}

void process_init(void) {
    memset(pcb_table, 0, sizeof(pcb_table));
    next_pid = 1;
    next_arrival = 1;
    current_scheduler = SCHED_FCFS;
    rr_quantum = 2;
    rr_last_index = -1;
}

int process_create(const char* name, int burst_time, int priority) {
    if (burst_time <= 0) {
        console_write_line("Error: burst_time must be > 0.");
        return -1;
    }

    for (int i = 0; i < MAX_PROCS; i++) {
        if (!pcb_table[i].used) {
            pcb_table[i].used = 1;
            pcb_table[i].pid = next_pid++;
            pcb_table[i].arrival_order = next_arrival++;
            strncpy(pcb_table[i].name, name, PROC_NAME_LEN - 1);
            pcb_table[i].name[PROC_NAME_LEN - 1] = '\0';
            pcb_table[i].burst_time = burst_time;
            pcb_table[i].remaining_time = burst_time;
            pcb_table[i].priority = priority;
            pcb_table[i].state = PROC_READY;

            console_write("Process created: PID=");
            console_write_dec(pcb_table[i].pid);
            console_write(" Name=");
            console_write(pcb_table[i].name);
            console_write(" Burst=");
            console_write_dec(pcb_table[i].burst_time);
            console_write(" Priority=");
            console_write_dec(pcb_table[i].priority);
            console_put_char('\n');
            return pcb_table[i].pid;
        }
    }

    console_write_line("Error: process table full.");
    return -1;
}

int process_kill(int pid) {
    int idx = find_pcb_index_by_pid(pid);
    if (idx < 0) {
        console_write_line("Error: PID not found.");
        return -1;
    }

    if (pcb_table[idx].state == PROC_TERMINATED) {
        console_write_line("Error: process already terminated.");
        return -1;
    }

    pcb_table[idx].state = PROC_TERMINATED;
    pcb_table[idx].remaining_time = 0;

    console_write("Process killed: PID=");
    console_write_dec(pid);
    console_put_char('\n');
    return 0;
}

int process_block(int pid) {
    int idx = find_pcb_index_by_pid(pid);
    if (idx < 0) {
        console_write_line("Error: PID not found.");
        return -1;
    }

    if (pcb_table[idx].state == PROC_TERMINATED) {
        console_write_line("Error: terminated process cannot be blocked.");
        return -1;
    }

    if (pcb_table[idx].state == PROC_BLOCKED) {
        console_write_line("Error: process already blocked.");
        return -1;
    }

    pcb_table[idx].state = PROC_BLOCKED;
    console_write("Process blocked: PID=");
    console_write_dec(pid);
    console_put_char('\n');
    return 0;
}

int process_wakeup(int pid) {
    int idx = find_pcb_index_by_pid(pid);
    if (idx < 0) {
        console_write_line("Error: PID not found.");
        return -1;
    }

    if (pcb_table[idx].state != PROC_BLOCKED) {
        console_write_line("Error: process is not blocked.");
        return -1;
    }

    pcb_table[idx].state = PROC_READY;
    console_write("Process waked up: PID=");
    console_write_dec(pid);
    console_put_char('\n');
    return 0;
}

void process_list(void) {
    console_write("Scheduler: ");
    console_write(process_get_scheduler_name());
    if (current_scheduler == SCHED_RR) {
        console_write(" Quantum=");
        console_write_dec(rr_quantum);
    }
    console_put_char('\n');

    int found = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (pcb_table[i].used) {
            print_proc_line(&pcb_table[i]);
            found = 1;
        }
    }

    if (!found) {
        console_write_line("(no processes)");
    }
}

void process_set_scheduler(SchedulerType type) {
    current_scheduler = type;
    rr_last_index = -1;
}

SchedulerType process_get_scheduler(void) {
    return current_scheduler;
}

const char* process_get_scheduler_name(void) {
    switch (current_scheduler) {
        case SCHED_FCFS:     return "fcfs";
        case SCHED_SJF:      return "sjf";
        case SCHED_RR:       return "rr";
        case SCHED_PRIORITY: return "priority";
        default:             return "unknown";
    }
}

void process_set_rr_quantum(int q) {
    if (q > 0) rr_quantum = q;
}

int process_get_rr_quantum(void) {
    return rr_quantum;
}

static void run_fcfs(void) {
    console_write_line("[Scheduler] FCFS start");
    while (has_ready_process()) {
        int idx = find_next_fcfs();
        if (idx < 0) break;

        PCB* p = &pcb_table[idx];
        p->state = PROC_RUNNING;

        console_write("[FCFS] Dispatch PID=");
        console_write_dec(p->pid);
        console_write(" Name=");
        console_write(p->name);
        console_write(" Burst=");
        console_write_dec(p->remaining_time);
        console_put_char('\n');

        console_write("       RUNNING -> complete, execute ");
        console_write_dec(p->remaining_time);
        console_write_line(" time units");

        p->remaining_time = 0;
        p->state = PROC_TERMINATED;

        console_write("[FCFS] Finish PID=");
        console_write_dec(p->pid);
        console_put_char('\n');
    }
    console_write_line("[Scheduler] FCFS end");
}

static void run_sjf(void) {
    console_write_line("[Scheduler] SJF start");
    while (has_ready_process()) {
        int idx = find_next_sjf();
        if (idx < 0) break;

        PCB* p = &pcb_table[idx];
        p->state = PROC_RUNNING;

        console_write("[SJF ] Dispatch PID=");
        console_write_dec(p->pid);
        console_write(" Name=");
        console_write(p->name);
        console_write(" ShortestRem=");
        console_write_dec(p->remaining_time);
        console_put_char('\n');

        console_write("       RUNNING -> complete, execute ");
        console_write_dec(p->remaining_time);
        console_write_line(" time units");

        p->remaining_time = 0;
        p->state = PROC_TERMINATED;

        console_write("[SJF ] Finish PID=");
        console_write_dec(p->pid);
        console_put_char('\n');
    }
    console_write_line("[Scheduler] SJF end");
}

static void run_priority(void) {
    console_write_line("[Scheduler] PRIORITY start");
    console_write_line("Rule: larger Prio value means higher priority.");

    while (has_ready_process()) {
        int idx = find_next_priority();
        if (idx < 0) break;

        PCB* p = &pcb_table[idx];
        p->state = PROC_RUNNING;

        console_write("[PRIO] Dispatch PID=");
        console_write_dec(p->pid);
        console_write(" Name=");
        console_write(p->name);
        console_write(" Priority=");
        console_write_dec(p->priority);
        console_write(" Burst=");
        console_write_dec(p->remaining_time);
        console_put_char('\n');

        console_write("       RUNNING -> complete, execute ");
        console_write_dec(p->remaining_time);
        console_write_line(" time units");

        p->remaining_time = 0;
        p->state = PROC_TERMINATED;

        console_write("[PRIO] Finish PID=");
        console_write_dec(p->pid);
        console_put_char('\n');
    }

    console_write_line("[Scheduler] PRIORITY end");
}

static void run_rr(void) {
    console_write("[Scheduler] RR start, quantum=");
    console_write_dec(rr_quantum);
    console_put_char('\n');

    while (has_ready_process()) {
        int idx = find_next_rr();
        if (idx < 0) break;

        PCB* p = &pcb_table[idx];
        int slice = (p->remaining_time < rr_quantum) ? p->remaining_time : rr_quantum;

        p->state = PROC_RUNNING;

        console_write("[RR  ] Dispatch PID=");
        console_write_dec(p->pid);
        console_write(" Name=");
        console_write(p->name);
        console_write(" Rem=");
        console_write_dec(p->remaining_time);
        console_write(" Slice=");
        console_write_dec(slice);
        console_put_char('\n');

        p->remaining_time -= slice;

        if (p->remaining_time == 0) {
            p->state = PROC_TERMINATED;
            console_write("[RR  ] Finish PID=");
            console_write_dec(p->pid);
            console_put_char('\n');
        } else {
            p->state = PROC_READY;
            console_write("[RR  ] Time slice over, PID=");
            console_write_dec(p->pid);
            console_write(" Remaining=");
            console_write_dec(p->remaining_time);
            console_put_char('\n');
        }
    }

    console_write_line("[Scheduler] RR end");
}

void process_run(void) {
    if (!has_ready_process()) {
        if (has_blocked_process()) {
            console_write_line("No READY process. Some processes are BLOCKED.");
        } else {
            console_write_line("No READY process to schedule.");
        }
        return;
    }

    if (current_scheduler == SCHED_FCFS) {
        run_fcfs();
    } else if (current_scheduler == SCHED_SJF) {
        run_sjf();
    } else if (current_scheduler == SCHED_RR) {
        run_rr();
    } else {
        run_priority();
    }
}