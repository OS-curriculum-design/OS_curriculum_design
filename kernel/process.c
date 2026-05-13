/*
 * 最小用户进程与调度器实现
 * ========================
 * 这份代码把“用户态程序”抽象成：
 * - 一张私有页目录
 * - 一页代码
 * - 一页用户栈
 * - 一份可恢复的用户寄存器上下文
 *
 * 调度策略采用“高优先级优先 + 同优先级轮转”，
 * 并通过 SYS_YIELD / 时钟抢占切回内核。
 */
#include "process.h"

#include "../console/console.h"
#include "../include/string.h"
#include "../mm/pager.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../timer/timer.h"
#include "memdemo.h"
#include "usermode.h"

/* 当前系统最多同时容纳 8 个进程，便于教学和调试。 */
#define PROCESS_MAX 8
#define PROCESS_IMAGE_MAX PAGE_SIZE
#define PROCESS_USER_CODE_BASE  0x00800000U
#define PROCESS_USER_STACK_TOP  0xBFFF0000U
#define PROCESS_USER_STACK_PAGE (PROCESS_USER_STACK_TOP - PAGE_SIZE)
#define PROCESS_USER_VM_BASE    0x03000000U
#define PROCESS_USER_VM_PAGE_COUNT 24U
#define PROCESS_PRIORITY_DEFAULT 5
#define PROCESS_PRIORITY_MIN 0
#define PROCESS_PRIORITY_MAX 10

typedef struct {
    int used;
    int pid;
    /* 父进程 PID；0 表示由内核/Shell 托管，或父进程已退出后被收养。 */
    int parent_pid;
    ProcessState state;
    int priority;
    const char* name;
    UserContext context;
    uint32_t page_directory_phys;
    uint32_t vm_page_bitmap;
    uint32_t exit_code;
    uint32_t image_size;
} Process;

/* 固定长度进程表，避免在内核里再引入动态链表管理复杂度。 */
static Process processes[PROCESS_MAX];
static int next_pid = 1;
/* current_pid=0 表示当前不在执行任何用户进程。 */
static int current_pid = 0;
/* 轮转调度从上次选中位置的下一个槽位继续找。 */
static int schedule_cursor = 0;
static int auto_schedule_enabled = 1;

/* 下面几组 emit_* 辅助函数用于动态拼装最小用户态机器码镜像。 */
static void emit_u8(uint8_t* image, uint32_t* offset, uint8_t value) {
    image[*offset] = value;
    (*offset)++;
}

static void emit_u32(uint8_t* image, uint32_t* offset, uint32_t value) {
    emit_u8(image, offset, (uint8_t)(value & 0xFF));
    emit_u8(image, offset, (uint8_t)((value >> 8) & 0xFF));
    emit_u8(image, offset, (uint8_t)((value >> 16) & 0xFF));
    emit_u8(image, offset, (uint8_t)((value >> 24) & 0xFF));
}

static void patch_message_address(uint8_t* image, uint32_t instruction_offset, uint32_t msg_offset);
static void emit_write_string(uint8_t* image, uint32_t* offset, const char* message, uint32_t* patch_offset_out, uint32_t* msg_len_out);
static void emit_exit(uint8_t* image, uint32_t* offset, uint32_t exit_code);

static Process* find_process_by_pid(int pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].used && processes[i].pid == pid) {
            return &processes[i];
        }
    }

    return (Process*)0;
}

static Process* allocate_process(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].used) {
            /* 把一项空槽初始化成 READY 状态的最小进程骨架。 */
            processes[i].used = 1;
            processes[i].pid = next_pid++;
            processes[i].parent_pid = 0;
            processes[i].state = PROCESS_READY;
            processes[i].priority = PROCESS_PRIORITY_DEFAULT;
            processes[i].name = "";
            memset(&processes[i].context, 0, sizeof(UserContext));
            processes[i].context.eip = PROCESS_USER_CODE_BASE;
            processes[i].context.user_esp = PROCESS_USER_STACK_TOP;
            processes[i].context.eflags = 0x202U;
            processes[i].page_directory_phys = 0;
            processes[i].vm_page_bitmap = 0;
            processes[i].exit_code = 0;
            processes[i].image_size = 0;
            return &processes[i];
        }
    }

    return (Process*)0;
}

static int parent_pid_is_valid(int parent_pid) {
    Process* parent;

    /* parent_pid=0 是保留的“根父进程”，用于 Shell 直接创建和孤儿进程收养。 */
    if (parent_pid == 0) {
        return 1;
    }

    parent = find_process_by_pid(parent_pid);
    return parent != (Process*)0 && parent->state != PROCESS_ZOMBIE;
}

static void adopt_children(int parent_pid) {
    /*
     * 父进程退出、被 wait 回收或被 kill 时，不能让孩子继续指向失效 PID。
     * 这里采用最小实现：所有孩子转交给 parent_pid=0，由 reap 负责后续清理。
     */
    if (parent_pid == 0) {
        return;
    }

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].used && processes[i].parent_pid == parent_pid) {
            processes[i].parent_pid = 0;
        }
    }
}

int process_spawn_child_from_buffer_with_priority(int parent_pid, const char* name, const uint8_t* image, uint32_t image_size, int priority) {
    Process* process;

    /* 创建子进程时先确认父进程存在；普通 exec 会走 parent_pid=0 的兼容路径。 */
    if (image == (const uint8_t*)0 ||
        image_size == 0 ||
        image_size > PROCESS_IMAGE_MAX ||
        !parent_pid_is_valid(parent_pid) ||
        priority < PROCESS_PRIORITY_MIN ||
        priority > PROCESS_PRIORITY_MAX) {
        return 0;
    }

    process = allocate_process();
    if (process == (Process*)0) {
        return 0;
    }

    process->name = name;
    process->parent_pid = parent_pid;
    process->priority = priority;
    process->image_size = image_size;
    if (!vmm_create_address_space(&process->page_directory_phys)) {
        process->used = 0;
        process->state = PROCESS_UNUSED;
        return 0;
    }

    /*
     * 进程代码页只写入交换区并登记为 not-present；
     * 第一次从 PROCESS_USER_CODE_BASE 取指时，#PF 会把它换入物理内存。
     */
    if (!pager_register_page_data(process->page_directory_phys,
                                  PROCESS_USER_CODE_BASE,
                                  VMM_PAGE_USER,
                                  image,
                                  image_size)) {
        vmm_destroy_address_space(process->page_directory_phys);
        process->used = 0;
        process->state = PROCESS_UNUSED;
        return 0;
    }
    pager_pin_page_in_directory(process->page_directory_phys, PROCESS_USER_CODE_BASE, 1);

    /*
     * 栈页同样延迟分配。iret 只加载 ESP，真正读写栈时才会触发 #PF。
     */
    if (!pager_register_page_in_directory(process->page_directory_phys,
                                          PROCESS_USER_STACK_PAGE,
                                          VMM_PAGE_WRITABLE | VMM_PAGE_USER)) {
        memdemo_release_for_directory(process->page_directory_phys);
        pager_unregister_page(process->page_directory_phys, PROCESS_USER_CODE_BASE);
        vmm_destroy_address_space(process->page_directory_phys);
        process->used = 0;
        process->state = PROCESS_UNUSED;
        return 0;
    }
    pager_pin_page_in_directory(process->page_directory_phys, PROCESS_USER_STACK_PAGE, 1);

    return process->pid;
}

int process_spawn_from_buffer_with_priority(const char* name, const uint8_t* image, uint32_t image_size, int priority) {
    /* 旧接口保持不变：Shell 直接 exec 出来的进程统一挂在 parent_pid=0 下。 */
    return process_spawn_child_from_buffer_with_priority(0, name, image, image_size, priority);
}

int process_spawn_child_from_buffer(int parent_pid, const char* name, const uint8_t* image, uint32_t image_size) {
    return process_spawn_child_from_buffer_with_priority(parent_pid, name, image, image_size, PROCESS_PRIORITY_DEFAULT);
}

int process_spawn_from_buffer(const char* name, const uint8_t* image, uint32_t image_size) {
    return process_spawn_from_buffer_with_priority(name, image, image_size, PROCESS_PRIORITY_DEFAULT);
}

uint32_t process_vm_alloc_page(uint32_t page_index) {
    Process* process;
    uint32_t virt_addr;
    uint32_t bit;

    if (current_pid == 0 || page_index >= PROCESS_USER_VM_PAGE_COUNT) {
        return 0;
    }

    process = find_process_by_pid(current_pid);
    if (process == (Process*)0 || process->state != PROCESS_RUNNING) {
        return 0;
    }

    virt_addr = PROCESS_USER_VM_BASE + page_index * PAGE_SIZE;
    bit = 1U << page_index;
    if ((process->vm_page_bitmap & bit) != 0) {
        return virt_addr;
    }

    if (!pager_register_page_in_directory(process->page_directory_phys,
                                          virt_addr,
                                          VMM_PAGE_WRITABLE | VMM_PAGE_USER)) {
        return 0;
    }

    process->vm_page_bitmap |= bit;
    return virt_addr;
}

static uint32_t build_hello_image(uint8_t* image) {
    static const char message[] = "hello from user process\n";
    uint32_t offset = 0;
    uint32_t msg_offset;
    uint32_t msg_len = (uint32_t)strlen(message);

    memset(image, 0, PROCESS_IMAGE_MAX);

    /* hello 程序只做一次 write，再 exit。 */
    emit_u8(image, &offset, 0xB8); /* mov eax, SYS_WRITE */
    emit_u32(image, &offset, SYS_WRITE);
    emit_u8(image, &offset, 0xBB); /* mov ebx, message */
    emit_u32(image, &offset, 0);
    emit_u8(image, &offset, 0xB9); /* mov ecx, length */
    emit_u32(image, &offset, msg_len);
    emit_u8(image, &offset, 0xCD); /* int 0x80 */
    emit_u8(image, &offset, 0x80);

    emit_u8(image, &offset, 0xB8); /* mov eax, SYS_EXIT */
    emit_u32(image, &offset, SYS_EXIT);
    emit_u8(image, &offset, 0xBB); /* mov ebx, 0 */
    emit_u32(image, &offset, 0);
    emit_u8(image, &offset, 0xCD); /* int 0x80 */
    emit_u8(image, &offset, 0x80);
    emit_u8(image, &offset, 0xEB); /* jmp . */
    emit_u8(image, &offset, 0xFE);

    msg_offset = offset;
    for (uint32_t i = 0; i < msg_len; i++) {
        image[offset++] = (uint8_t)message[i];
    }

    /* 早期版本直接手动回填字符串地址，因此这里写死了 mov ebx, imm32 的位置。 */
    image[6] = (uint8_t)((PROCESS_USER_CODE_BASE + msg_offset) & 0xFF);
    image[7] = (uint8_t)(((PROCESS_USER_CODE_BASE + msg_offset) >> 8) & 0xFF);
    image[8] = (uint8_t)(((PROCESS_USER_CODE_BASE + msg_offset) >> 16) & 0xFF);
    image[9] = (uint8_t)(((PROCESS_USER_CODE_BASE + msg_offset) >> 24) & 0xFF);

    return offset;
}

static uint32_t build_busy_image(uint8_t* image) {
    static const char msg1[] = "busy step 1\n";
    static const char msg2[] = "busy step 2\n";
    static const char msg3[] = "busy step 3\n";
    uint32_t offset = 0;
    uint32_t patch1;
    uint32_t patch2;
    uint32_t patch3;
    uint32_t len1;
    uint32_t len2;
    uint32_t len3;
    uint32_t msg1_offset;
    uint32_t msg2_offset;
    uint32_t msg3_offset;

    memset(image, 0, PROCESS_IMAGE_MAX);

    /*
     * busy 程序不会主动 yield，而是在用户态做忙等待，
     * 适合观察抢占式时间片调度是否生效。
     */
    emit_write_string(image, &offset, msg1, &patch1, &len1);
    emit_u8(image, &offset, 0xB9); /* mov ecx, imm32 */
    emit_u32(image, &offset, 0x02000000U);
    emit_u8(image, &offset, 0x49); /* dec ecx */
    emit_u8(image, &offset, 0x75); /* jnz -3 */
    emit_u8(image, &offset, 0xFD);

    emit_write_string(image, &offset, msg2, &patch2, &len2);
    emit_u8(image, &offset, 0xB9); /* mov ecx, imm32 */
    emit_u32(image, &offset, 0x02000000U);
    emit_u8(image, &offset, 0x49); /* dec ecx */
    emit_u8(image, &offset, 0x75); /* jnz -3 */
    emit_u8(image, &offset, 0xFD);

    emit_write_string(image, &offset, msg3, &patch3, &len3);
    emit_exit(image, &offset, 0);

    msg1_offset = offset;
    for (uint32_t i = 0; i < len1; i++) {
        image[offset++] = (uint8_t)msg1[i];
    }

    msg2_offset = offset;
    for (uint32_t i = 0; i < len2; i++) {
        image[offset++] = (uint8_t)msg2[i];
    }

    msg3_offset = offset;
    for (uint32_t i = 0; i < len3; i++) {
        image[offset++] = (uint8_t)msg3[i];
    }

    patch_message_address(image, patch1, msg1_offset);
    patch_message_address(image, patch2, msg2_offset);
    patch_message_address(image, patch3, msg3_offset);
    return offset;
}

static void patch_message_address(uint8_t* image, uint32_t instruction_offset, uint32_t msg_offset) {
    uint32_t address = PROCESS_USER_CODE_BASE + msg_offset;

    /* 把 mov ebx, imm32 指令中的立即数回填成字符串在用户空间中的虚拟地址。 */
    image[instruction_offset + 1U] = (uint8_t)(address & 0xFF);
    image[instruction_offset + 2U] = (uint8_t)((address >> 8) & 0xFF);
    image[instruction_offset + 3U] = (uint8_t)((address >> 16) & 0xFF);
    image[instruction_offset + 4U] = (uint8_t)((address >> 24) & 0xFF);
}

static void emit_write_string(uint8_t* image, uint32_t* offset, const char* message, uint32_t* patch_offset_out, uint32_t* msg_len_out) {
    uint32_t msg_len = (uint32_t)strlen(message);

    /* 约定 write(ebx=text, ecx=len)。 */
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_WRITE */
    emit_u32(image, offset, SYS_WRITE);
    *patch_offset_out = *offset;
    emit_u8(image, offset, 0xBB); /* mov ebx, message */
    emit_u32(image, offset, 0);
    emit_u8(image, offset, 0xB9); /* mov ecx, length */
    emit_u32(image, offset, msg_len);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);

    *msg_len_out = msg_len;
}

static void emit_yield(uint8_t* image, uint32_t* offset) {
    /* yield 通过 int 0x80 主动把控制权还给内核调度器。 */
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_YIELD */
    emit_u32(image, offset, SYS_YIELD);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
}

static void emit_memdemo_op(uint8_t* image, uint32_t* offset, uint32_t op, uint32_t page) {
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_MEMDEMO_OP */
    emit_u32(image, offset, SYS_MEMDEMO_OP);
    emit_u8(image, offset, 0xBB); /* mov ebx, op */
    emit_u32(image, offset, op);
    emit_u8(image, offset, 0xB9); /* mov ecx, page */
    emit_u32(image, offset, page);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
}

static void emit_user_write_eax(uint8_t* image, uint32_t* offset, uint32_t value) {
    emit_u8(image, offset, 0xC7); /* mov dword ptr [eax], imm32 */
    emit_u8(image, offset, 0x00);
    emit_u32(image, offset, value);
}

static void emit_memdemo_reset(uint8_t* image, uint32_t* offset) {
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_MEMDEMO_RESET */
    emit_u32(image, offset, SYS_MEMDEMO_RESET);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
}

static void emit_vm_alloc(uint8_t* image, uint32_t* offset, uint32_t page) {
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_VM_ALLOC */
    emit_u32(image, offset, SYS_VM_ALLOC);
    emit_u8(image, offset, 0xBB); /* mov ebx, page */
    emit_u32(image, offset, page);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
}

static void emit_vm_sample(uint8_t* image, uint32_t* offset) {
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_VM_SAMPLE */
    emit_u32(image, offset, SYS_VM_SAMPLE);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
}

static void emit_pager_trace_reset(uint8_t* image, uint32_t* offset) {
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_PAGER_TRACE_RESET */
    emit_u32(image, offset, SYS_PAGER_TRACE_RESET);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
}

static void emit_rel32(uint8_t* image, uint32_t patch_offset, int32_t rel) {
    image[patch_offset] = (uint8_t)(rel & 0xFF);
    image[patch_offset + 1U] = (uint8_t)((rel >> 8) & 0xFF);
    image[patch_offset + 2U] = (uint8_t)((rel >> 16) & 0xFF);
    image[patch_offset + 3U] = (uint8_t)((rel >> 24) & 0xFF);
}

static void emit_wait_memdemo_report(uint8_t* image, uint32_t* offset, uint32_t sequence) {
    uint32_t loop_start = *offset;
    uint32_t je_patch;
    uint32_t jmp_patch;
    uint32_t ready_offset;
    int32_t rel;

    emit_u8(image, offset, 0xB8); /* mov eax, SYS_MEMDEMO_REPORT */
    emit_u32(image, offset, SYS_MEMDEMO_REPORT);
    emit_u8(image, offset, 0xBB); /* mov ebx, sequence */
    emit_u32(image, offset, sequence);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);

    emit_u8(image, offset, 0x83); /* cmp eax, 1 */
    emit_u8(image, offset, 0xF8);
    emit_u8(image, offset, 0x01);
    emit_u8(image, offset, 0x74); /* je ready */
    je_patch = *offset;
    emit_u8(image, offset, 0x00);

    emit_yield(image, offset);
    emit_u8(image, offset, 0xE9); /* jmp loop_start */
    jmp_patch = *offset;
    emit_u32(image, offset, 0);

    ready_offset = *offset;
    image[je_patch] = (uint8_t)(ready_offset - (je_patch + 1U));

    rel = (int32_t)loop_start - (int32_t)(jmp_patch + 4U);
    emit_rel32(image, jmp_patch, rel);
}

static void emit_exit(uint8_t* image, uint32_t* offset, uint32_t exit_code) {
    /* exit 返回后理论上不会继续执行，但这里仍补一个死循环做保险。 */
    emit_u8(image, offset, 0xB8); /* mov eax, SYS_EXIT */
    emit_u32(image, offset, SYS_EXIT);
    emit_u8(image, offset, 0xBB); /* mov ebx, exit_code */
    emit_u32(image, offset, exit_code);
    emit_u8(image, offset, 0xCD); /* int 0x80 */
    emit_u8(image, offset, 0x80);
    emit_u8(image, offset, 0xEB); /* jmp . */
    emit_u8(image, offset, 0xFE);
}

static uint32_t build_counter_image(uint8_t* image) {
    static const char msg1[] = "counter step 1\n";
    static const char msg2[] = "counter step 2\n";
    static const char msg3[] = "counter step 3\n";
    uint32_t offset = 0;
    uint32_t patch1;
    uint32_t patch2;
    uint32_t patch3;
    uint32_t len1;
    uint32_t len2;
    uint32_t len3;
    uint32_t msg1_offset;
    uint32_t msg2_offset;
    uint32_t msg3_offset;

    memset(image, 0, PROCESS_IMAGE_MAX);

    /*
     * counter 在每一步输出之后主动 yield，
     * 便于观察协作式切换与轮转调度顺序。
     */
    emit_write_string(image, &offset, msg1, &patch1, &len1);
    emit_yield(image, &offset);
    emit_write_string(image, &offset, msg2, &patch2, &len2);
    emit_yield(image, &offset);
    emit_write_string(image, &offset, msg3, &patch3, &len3);
    emit_exit(image, &offset, 0);

    msg1_offset = offset;
    for (uint32_t i = 0; i < len1; i++) {
        image[offset++] = (uint8_t)msg1[i];
    }

    msg2_offset = offset;
    for (uint32_t i = 0; i < len2; i++) {
        image[offset++] = (uint8_t)msg2[i];
    }

    msg3_offset = offset;
    for (uint32_t i = 0; i < len3; i++) {
        image[offset++] = (uint8_t)msg3[i];
    }

    patch_message_address(image, patch1, msg1_offset);
    patch_message_address(image, patch2, msg2_offset);
    patch_message_address(image, patch3, msg3_offset);
    return offset;
}

static uint32_t build_memalloc_image(uint8_t* image) {
    static const uint32_t ops[MEMDEMO_EVENT_COUNT] = {
        MEMDEMO_OP_ALLOC,
        MEMDEMO_OP_ALLOC,
        MEMDEMO_OP_TOUCH,
        MEMDEMO_OP_ALLOC,
        MEMDEMO_OP_FREE,
        MEMDEMO_OP_ALLOC,
        MEMDEMO_OP_TOUCH,
        MEMDEMO_OP_FREE
    };
    static const uint32_t pages[MEMDEMO_EVENT_COUNT] = {
        0U, 1U, 0U, 3U, 1U, 2U, 3U, 0U
    };
    uint32_t offset = 0;

    memset(image, 0, PROCESS_IMAGE_MAX);
    emit_memdemo_reset(image, &offset);
    for (uint32_t i = 0; i < MEMDEMO_EVENT_COUNT; i++) {
        emit_memdemo_op(image, &offset, ops[i], pages[i]);
        if (ops[i] != MEMDEMO_OP_FREE) {
            emit_user_write_eax(image, &offset, 0xA1100000U | (i << 8) | pages[i]);
        }
        emit_yield(image, &offset);
    }
    emit_exit(image, &offset, 0);
    return offset;
}

static uint32_t build_memtrack_image(uint8_t* image) {
    uint32_t offset = 0;

    memset(image, 0, PROCESS_IMAGE_MAX);
    for (uint32_t i = 1; i <= MEMDEMO_EVENT_COUNT; i++) {
        emit_wait_memdemo_report(image, &offset, i);
        emit_yield(image, &offset);
    }
    emit_exit(image, &offset, 0);
    return offset;
}

static uint32_t build_pagerdemo_image(uint8_t* image) {
    static const char start_msg[] = "pagerdemo: user process starts\n";
    static const char done_msg[] = "pagerdemo: done, run pager to inspect victim trace\n";
    uint32_t offset = 0;
    uint32_t start_patch;
    uint32_t done_patch;
    uint32_t start_len;
    uint32_t done_len;
    uint32_t start_msg_offset;
    uint32_t done_msg_offset;

    memset(image, 0, PROCESS_IMAGE_MAX);

    emit_write_string(image, &offset, start_msg, &start_patch, &start_len);
    emit_pager_trace_reset(image, &offset);

    for (uint32_t i = 0; i <= PAGER_FRAME_LIMIT; i++) {
        emit_vm_alloc(image, &offset, i);
    }

    for (uint32_t i = 0; i < PAGER_FRAME_LIMIT - 1U; i++) {
        emit_vm_alloc(image, &offset, i);
        emit_user_write_eax(image, &offset, 0xD0A00000U | i);
    }

    emit_vm_sample(image, &offset);

    emit_vm_alloc(image, &offset, 0);
    emit_user_write_eax(image, &offset, 0xD0A01000U);

    emit_vm_alloc(image, &offset, PAGER_FRAME_LIMIT - 1U);
    emit_user_write_eax(image, &offset, 0xD0A02000U | (PAGER_FRAME_LIMIT - 1U));

    emit_vm_alloc(image, &offset, PAGER_FRAME_LIMIT);
    emit_user_write_eax(image, &offset, 0xD0A03000U | PAGER_FRAME_LIMIT);

    emit_write_string(image, &offset, done_msg, &done_patch, &done_len);
    emit_exit(image, &offset, 0);

    start_msg_offset = offset;
    for (uint32_t i = 0; i < start_len; i++) {
        image[offset++] = (uint8_t)start_msg[i];
    }

    done_msg_offset = offset;
    for (uint32_t i = 0; i < done_len; i++) {
        image[offset++] = (uint8_t)done_msg[i];
    }

    patch_message_address(image, start_patch, start_msg_offset);
    patch_message_address(image, done_patch, done_msg_offset);
    return offset;
}

static int process_run(int pid) {
    Process* process = find_process_by_pid(pid);
    uint32_t result;
    uint32_t kernel_page_directory = vmm_get_kernel_page_directory();

    if (process == (Process*)0 || process->state != PROCESS_READY) {
        return 0;
    }

    /* 先切到目标进程页目录，再真正进入用户态。 */
    if (!vmm_switch_page_directory(process->page_directory_phys)) {
        return 0;
    }

    current_pid = process->pid;
    process->state = PROCESS_RUNNING;
    result = usermode_enter_context(&process->context);
    vmm_switch_page_directory(kernel_page_directory);

    /* 用户态返回只可能有两种语义：yield，或者 exit(返回码)。 */
    if (result == USERMODE_RETURN_YIELD) {
        process->state = PROCESS_READY;
    } else {
        process->exit_code = result;
        process->state = PROCESS_ZOMBIE;
        /* 当前没有阻塞式 wait；父进程结束时，先把仍存活的孩子交给 0 号父进程托管。 */
        adopt_children(process->pid);
    }

    current_pid = 0;
    return 1;
}

static void release_process(Process* process) {
    if (process == (Process*)0 || !process->used || process->state == PROCESS_RUNNING) {
        return;
    }

    /* 释放 PCB 前先处理父子关系，避免子进程留下悬空的 parent_pid。 */
    adopt_children(process->pid);

    if (process->page_directory_phys != 0) {
        memdemo_release_for_directory(process->page_directory_phys);
        /* Pager 会处理“已换入”和“仍在交换区”的两种页状态。 */
        pager_unregister_page(process->page_directory_phys, PROCESS_USER_CODE_BASE);
        pager_unregister_page(process->page_directory_phys, PROCESS_USER_STACK_PAGE);
        for (uint32_t i = 0; i < PROCESS_USER_VM_PAGE_COUNT; i++) {
            if ((process->vm_page_bitmap & (1U << i)) != 0) {
                pager_unregister_page(process->page_directory_phys,
                                      PROCESS_USER_VM_BASE + i * PAGE_SIZE);
            }
        }
        vmm_destroy_address_space(process->page_directory_phys);
    }

    process->used = 0;
    process->pid = 0;
    process->parent_pid = 0;
    process->state = PROCESS_UNUSED;
    process->priority = PROCESS_PRIORITY_DEFAULT;
    process->name = "";
    memset(&process->context, 0, sizeof(UserContext));
    process->page_directory_phys = 0;
    process->vm_page_bitmap = 0;
    process->exit_code = 0;
    process->image_size = 0;
}

void process_init(void) {
    /* 开机时先把整张进程表置为空。 */
    for (int i = 0; i < PROCESS_MAX; i++) {
        processes[i].used = 0;
        processes[i].pid = 0;
        processes[i].parent_pid = 0;
        processes[i].state = PROCESS_UNUSED;
        processes[i].priority = PROCESS_PRIORITY_DEFAULT;
        processes[i].vm_page_bitmap = 0;
    }

    next_pid = 1;
    current_pid = 0;
    schedule_cursor = 0;
    auto_schedule_enabled = 1;
}

int process_build_builtin_image(const char* name, uint8_t* image, uint32_t image_capacity, uint32_t* image_size_out) {
    uint32_t image_size;

    if (image == (uint8_t*)0 || image_size_out == (uint32_t*)0 || image_capacity < PROCESS_IMAGE_MAX) {
        return 0;
    }

    if (strcmp(name, "hello") == 0) {
        image_size = build_hello_image(image);
    } else if (strcmp(name, "counter") == 0) {
        image_size = build_counter_image(image);
    } else if (strcmp(name, "busy") == 0) {
        image_size = build_busy_image(image);
    } else if (strcmp(name, "memalloc") == 0) {
        image_size = build_memalloc_image(image);
    } else if (strcmp(name, "memtrack") == 0) {
        image_size = build_memtrack_image(image);
    } else if (strcmp(name, "pagerdemo") == 0) {
        image_size = build_pagerdemo_image(image);
    } else {
        return 0;
    }

    *image_size_out = image_size;
    return 1;
}

static int select_next_ready_process(void) {
    int selected = -1;
    int best_priority = PROCESS_PRIORITY_MIN - 1;

    /*
     * 优先级调度策略：
     * - 优先选择 READY 队列中优先级最高的进程
     * - 对同优先级进程，仍然从 schedule_cursor 开始顺序找，
     *   因而保留轮转公平性
     */
    for (int offset = 0; offset < PROCESS_MAX; offset++) {
        int i = (schedule_cursor + offset) % PROCESS_MAX;

        if (processes[i].used &&
            processes[i].state == PROCESS_READY &&
            processes[i].priority > best_priority) {
            best_priority = processes[i].priority;
            selected = i;
        }
    }

    return selected;
}

int process_schedule(void) {
    /* 手工调度入口，本质上只是包装了自动调度逻辑。 */
    if (process_schedule_auto()) {
        return 1;
    }

    console_write_line("no ready process");
    return 0;
}

int process_schedule_auto(void) {
    int selected = select_next_ready_process();
    Process* process;
    int pid;

    if (selected < 0) {
        return 0;
    }

    pid = processes[selected].pid;
    if (!process_run(pid)) {
        console_write_line("process run failed");
        return 0;
    }

    /* 下次从当前成功运行项的下一个槽位开始找。 */
    schedule_cursor = (selected + 1) % PROCESS_MAX;
    process = find_process_by_pid(pid);
    if (process != (Process*)0 && process->state == PROCESS_ZOMBIE) {
        console_write("process exited: pid=");
        console_write_dec(pid);
        console_write(" code=");
        console_write_dec((int)process->exit_code);
        console_put_char('\n');
    }
    return pid;
}

int process_run_pid(int pid) {
    Process* process;

    if (!process_run(pid)) {
        return 0;
    }

    process = find_process_by_pid(pid);
    if (process != (Process*)0 && process->state == PROCESS_ZOMBIE) {
        console_write("process exited: pid=");
        console_write_dec(pid);
        console_write(" code=");
        console_write_dec((int)process->exit_code);
        console_put_char('\n');
    } else {
        console_write("process yielded: pid=");
        console_write_dec(pid);
        console_put_char('\n');
    }
    return pid;
}

void process_save_yield_frame(InterruptFrame* frame) {
    Process* process;

    if (frame == (InterruptFrame*)0 || current_pid == 0) {
        return;
    }

    process = find_process_by_pid(current_pid);
    if (process == (Process*)0) {
        return;
    }

    /* 把会影响用户态继续执行的关键寄存器全部抄回进程上下文。 */
    process->context.eip = frame->eip;
    process->context.user_esp = frame->useresp;
    process->context.eflags = frame->eflags;
    process->context.eax = frame->eax;
    process->context.ebx = frame->ebx;
    process->context.ecx = frame->ecx;
    process->context.edx = frame->edx;
    process->context.esi = frame->esi;
    process->context.edi = frame->edi;
    process->context.ebp = frame->ebp;
}

int process_preempt_if_needed(InterruptFrame* frame) {
    /*
     * 只有在“当前确实正在执行用户态进程”时才允许抢占：
     * - current_pid != 0
     * - CS 的 RPL 为 3，说明中断发生在用户态
     */
    if (frame == (InterruptFrame*)0 || current_pid == 0 || (frame->cs & 0x3U) != 0x3U) {
        return 0;
    }

    /* 取走一个时间片事件，避免主循环里再次重复消费。 */
    if (!timer_take_schedule_event()) {
        return 0;
    }

    if (!process_has_ready()) {
        return 0;
    }

    process_save_yield_frame(frame);
    usermode_return_to_kernel(USERMODE_RETURN_YIELD);
    return 1;
}

int process_has_ready(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].used && processes[i].state == PROCESS_READY) {
            return 1;
        }
    }

    return 0;
}

int process_set_priority(int pid, int priority) {
    Process* process;

    if (priority < PROCESS_PRIORITY_MIN || priority > PROCESS_PRIORITY_MAX) {
        return 0;
    }

    process = find_process_by_pid(pid);
    if (process == (Process*)0 || process->state == PROCESS_ZOMBIE) {
        return 0;
    }

    process->priority = priority;
    return 1;
}

int process_get_priority(int pid, int* priority_out) {
    Process* process;

    if (priority_out == (int*)0) {
        return 0;
    }

    process = find_process_by_pid(pid);
    if (process == (Process*)0) {
        return 0;
    }

    *priority_out = process->priority;
    return 1;
}

int process_wait_child(int parent_pid, int child_pid, int* waited_pid_out, uint32_t* exit_code_out) {
    Process* selected = (Process*)0;
    int has_child = 0;

    /*
     * Shell 不是普通用户进程，因此这里实现成“非阻塞 wait”：
     * 只有匹配的子进程已经 ZOMBIE 时才回收，否则返回 NOT_EXITED。
     */
    if (parent_pid != 0 && find_process_by_pid(parent_pid) == (Process*)0) {
        return PROCESS_WAIT_BAD_PARENT;
    }

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].used || processes[i].parent_pid != parent_pid) {
            continue;
        }

        if (child_pid != 0 && processes[i].pid != child_pid) {
            continue;
        }

        has_child = 1;
        if (processes[i].state == PROCESS_ZOMBIE) {
            selected = &processes[i];
            break;
        }
    }

    if (selected == (Process*)0) {
        /* 有孩子但还没退出，和根本没有匹配孩子，要给 Shell 不同提示。 */
        return has_child ? PROCESS_WAIT_NOT_EXITED : PROCESS_WAIT_NO_CHILD;
    }

    if (waited_pid_out != (int*)0) {
        *waited_pid_out = selected->pid;
    }

    if (exit_code_out != (uint32_t*)0) {
        *exit_code_out = selected->exit_code;
    }

    release_process(selected);
    return PROCESS_WAIT_OK;
}

int process_kill(int pid) {
    Process* process = find_process_by_pid(pid);

    if (process == (Process*)0) {
        return PROCESS_KILL_NOT_FOUND;
    }

    if (process->state == PROCESS_RUNNING) {
        /* 当前调度器没有异步中止 RUNNING 上下文的机制，先只支持撤销非运行态进程。 */
        return PROCESS_KILL_RUNNING;
    }

    /* READY/ZOMBIE 都可以直接释放；若目标有孩子，release_process 会完成收养。 */
    release_process(process);
    return PROCESS_KILL_OK;
}

int process_auto_schedule_enabled(void) {
    return auto_schedule_enabled;
}

void process_set_auto_schedule(int enabled) {
    auto_schedule_enabled = enabled ? 1 : 0;
}

int process_reap_zombies(void) {
    int reaped = 0;

    /* 只回收 parent_pid=0 的僵尸；普通子进程需要由父进程通过 wait 领取退出码。 */
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].used && processes[i].state == PROCESS_ZOMBIE && processes[i].parent_pid == 0) {
            release_process(&processes[i]);
            reaped++;
        }
    }

    return reaped;
}

void process_print_table(void) {
    console_write_line("PID  PPID  PRI  STATE    NAME");

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].used) {
            continue;
        }

        console_write_dec(processes[i].pid);
        console_write("    ");
        console_write_dec(processes[i].parent_pid);
        console_write("     ");
        console_write_dec(processes[i].priority);
        console_write("    ");

        if (processes[i].state == PROCESS_READY) {
            console_write("READY    ");
        } else if (processes[i].state == PROCESS_RUNNING) {
            console_write("RUNNING  ");
        } else if (processes[i].state == PROCESS_WAITING) {
            console_write("WAITING  ");
        } else if (processes[i].state == PROCESS_ZOMBIE) {
            console_write("ZOMBIE   ");
        } else {
            console_write("UNUSED   ");
        }

        console_write_line(processes[i].name);
        console_write("     cr3=");
        console_write_hex(processes[i].page_directory_phys);
        console_put_char('\n');
    }
}
