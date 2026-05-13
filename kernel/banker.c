#include "banker.h"

#include "../console/console.h"

typedef struct {
    int used;
    int pid;
    uint32_t max[BANKER_RESOURCE_COUNT];
    uint32_t allocation[BANKER_RESOURCE_COUNT];
    uint32_t need[BANKER_RESOURCE_COUNT];
} BankerProcess;

static int banker_ready = 0;
static uint32_t available[BANKER_RESOURCE_COUNT];
static BankerProcess banker_processes[BANKER_MAX_PROCESSES];

static int find_slot_by_pid(int pid) {
    for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
        if (banker_processes[i].used && banker_processes[i].pid == pid) {
            return i;
        }
    }

    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
        if (!banker_processes[i].used) {
            return i;
        }
    }

    return -1;
}

static int vector_leq(const uint32_t* left, const uint32_t* right) {
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        if (left[r] > right[r]) {
            return 0;
        }
    }

    return 1;
}

static int all_finished(const int* finish) {
    for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
        if (banker_processes[i].used && !finish[i]) {
            return 0;
        }
    }

    return 1;
}

static int banker_state_is_safe(void) {
    uint32_t work[BANKER_RESOURCE_COUNT];
    int finish[BANKER_MAX_PROCESSES];
    int progress;

    /*
     * 安全性检测只在银行家模块的表副本上运行：
     * 如果能找到一个完成序列，让所有登记进程最终都 finish，则当前状态安全。
     */
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        work[r] = available[r];
    }

    for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
        finish[i] = banker_processes[i].used ? 0 : 1;
    }

    do {
        progress = 0;
        for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
            if (banker_processes[i].used &&
                !finish[i] &&
                vector_leq(banker_processes[i].need, work)) {
                for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
                    work[r] += banker_processes[i].allocation[r];
                }
                finish[i] = 1;
                progress = 1;
            }
        }
    } while (progress);

    return all_finished(finish);
}

void banker_init(const uint32_t* initial_available) {
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        available[r] = initial_available[r];
    }

    for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
        banker_processes[i].used = 0;
        banker_processes[i].pid = 0;
        for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
            banker_processes[i].max[r] = 0;
            banker_processes[i].allocation[r] = 0;
            banker_processes[i].need[r] = 0;
        }
    }

    banker_ready = 1;
}

int banker_register_process(int pid, const uint32_t* max) {
    int slot;

    if (!banker_ready) {
        return BANKER_ERR_NOT_READY;
    }

    slot = find_slot_by_pid(pid);
    if (slot < 0) {
        slot = find_free_slot();
    }

    if (slot < 0) {
        return BANKER_ERR_NO_SLOT;
    }

    banker_processes[slot].used = 1;
    banker_processes[slot].pid = pid;
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        banker_processes[slot].max[r] = max[r];
        banker_processes[slot].allocation[r] = 0;
        banker_processes[slot].need[r] = max[r];
    }

    return BANKER_OK;
}

void banker_unregister_process(int pid) {
    int slot;

    if (!banker_ready) {
        return;
    }

    slot = find_slot_by_pid(pid);
    if (slot < 0) {
        return;
    }

    /* 注销进程时自动归还 Allocation，配合 wait/reap/kill 的生命周期回收。 */
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        available[r] += banker_processes[slot].allocation[r];
        banker_processes[slot].max[r] = 0;
        banker_processes[slot].allocation[r] = 0;
        banker_processes[slot].need[r] = 0;
    }

    banker_processes[slot].used = 0;
    banker_processes[slot].pid = 0;
}

int banker_request(int pid, const uint32_t* request) {
    int slot;

    if (!banker_ready) {
        return BANKER_ERR_NOT_READY;
    }

    slot = find_slot_by_pid(pid);
    if (slot < 0) {
        return BANKER_ERR_NO_PROCESS;
    }

    if (!vector_leq(request, banker_processes[slot].need)) {
        return BANKER_ERR_OVER_NEED;
    }

    if (!vector_leq(request, available)) {
        return BANKER_ERR_OVER_AVAILABLE;
    }

    /* 先试分配，再做安全性检测；不安全就完整回滚。 */
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        available[r] -= request[r];
        banker_processes[slot].allocation[r] += request[r];
        banker_processes[slot].need[r] -= request[r];
    }

    if (!banker_state_is_safe()) {
        for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
            available[r] += request[r];
            banker_processes[slot].allocation[r] -= request[r];
            banker_processes[slot].need[r] += request[r];
        }
        return BANKER_ERR_UNSAFE;
    }

    return BANKER_OK;
}

int banker_release(int pid, const uint32_t* release) {
    int slot;

    if (!banker_ready) {
        return BANKER_ERR_NOT_READY;
    }

    slot = find_slot_by_pid(pid);
    if (slot < 0) {
        return BANKER_ERR_NO_PROCESS;
    }

    if (!vector_leq(release, banker_processes[slot].allocation)) {
        return BANKER_ERR_OVER_ALLOCATED;
    }

    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        banker_processes[slot].allocation[r] -= release[r];
        banker_processes[slot].need[r] += release[r];
        available[r] += release[r];
    }

    return BANKER_OK;
}

static void print_vector(const uint32_t* values) {
    console_write("[");
    for (uint32_t r = 0; r < BANKER_RESOURCE_COUNT; r++) {
        if (r > 0) {
            console_write(" ");
        }
        console_write_dec((int)values[r]);
    }
    console_write("]");
}

void banker_print_state(void) {
    if (!banker_ready) {
        console_write_line("Banker is not initialized. Use: banker init <r0> <r1> <r2>");
        return;
    }

    console_write("Available ");
    print_vector(available);
    console_put_char('\n');

    console_write_line("PID  MAX      ALLOC    NEED");
    for (int i = 0; i < BANKER_MAX_PROCESSES; i++) {
        if (!banker_processes[i].used) {
            continue;
        }

        console_write_dec(banker_processes[i].pid);
        console_write("    ");
        print_vector(banker_processes[i].max);
        console_write("  ");
        print_vector(banker_processes[i].allocation);
        console_write("  ");
        print_vector(banker_processes[i].need);
        console_put_char('\n');
    }
}
