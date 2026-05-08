/*
 * 最小用户进程与调度器实现
 * ========================
 * 这份代码把“用户态程序”抽象成：
 * - 一张私有页目录
 * - 一页代码
 * - 一页用户栈
 * - 一份可恢复的用户寄存器上下文
 *
 * 调度策略采用简单轮转，并通过 SYS_YIELD / 时钟抢占切回内核。
 */
#include "process.h"

#include "../console/console.h"
#include "../include/string.h"
#include "../mm/pager.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../timer/timer.h"
#include "usermode.h"

/* 当前系统最多同时容纳 8 个进程，便于教学和调试。 */
#define PROCESS_MAX 8
#define PROCESS_IMAGE_MAX PAGE_SIZE
#define PROCESS_USER_CODE_BASE  0x00800000U
#define PROCESS_USER_STACK_TOP  0xBFFF0000U
#define PROCESS_USER_STACK_PAGE (PROCESS_USER_STACK_TOP - PAGE_SIZE)

typedef struct {
    int used;
    int pid;
    ProcessState state;
    const char* name;
    UserContext context;
    uint32_t page_directory_phys;
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
            processes[i].state = PROCESS_READY;
            processes[i].name = "";
            memset(&processes[i].context, 0, sizeof(UserContext));
            processes[i].context.eip = PROCESS_USER_CODE_BASE;
            processes[i].context.user_esp = PROCESS_USER_STACK_TOP;
            processes[i].context.eflags = 0x202U;
            processes[i].page_directory_phys = 0;
            processes[i].exit_code = 0;
            processes[i].image_size = 0;
            return &processes[i];
        }
    }

    return (Process*)0;
}

int process_spawn_from_buffer(const char* name, const uint8_t* image, uint32_t image_size) {
    Process* process;

    if (image == (const uint8_t*)0 || image_size == 0 || image_size > PROCESS_IMAGE_MAX) {
        return 0;
    }

    process = allocate_process();
    if (process == (Process*)0) {
        return 0;
    }

    process->name = name;
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

    /*
     * 栈页同样延迟分配。iret 只加载 ESP，真正读写栈时才会触发 #PF。
     */
    if (!pager_register_page_in_directory(process->page_directory_phys,
                                          PROCESS_USER_STACK_PAGE,
                                          VMM_PAGE_WRITABLE | VMM_PAGE_USER)) {
        pager_unregister_page(process->page_directory_phys, PROCESS_USER_CODE_BASE);
        vmm_destroy_address_space(process->page_directory_phys);
        process->used = 0;
        process->state = PROCESS_UNUSED;
        return 0;
    }

    return process->pid;
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
    }

    current_pid = 0;
    return 1;
}

static void release_process(Process* process) {
    if (process == (Process*)0 || !process->used || process->state == PROCESS_RUNNING) {
        return;
    }

    if (process->page_directory_phys != 0) {
        /* Pager 会处理“已换入”和“仍在交换区”的两种页状态。 */
        pager_unregister_page(process->page_directory_phys, PROCESS_USER_CODE_BASE);
        pager_unregister_page(process->page_directory_phys, PROCESS_USER_STACK_PAGE);
        vmm_destroy_address_space(process->page_directory_phys);
    }

    process->used = 0;
    process->pid = 0;
    process->state = PROCESS_UNUSED;
    process->name = "";
    memset(&process->context, 0, sizeof(UserContext));
    process->page_directory_phys = 0;
    process->exit_code = 0;
    process->image_size = 0;
}

void process_init(void) {
    /* 开机时先把整张进程表置为空。 */
    for (int i = 0; i < PROCESS_MAX; i++) {
        processes[i].used = 0;
        processes[i].pid = 0;
        processes[i].state = PROCESS_UNUSED;
    }

    next_pid = 1;
    current_pid = 0;
    schedule_cursor = 0;
    auto_schedule_enabled = 1;
}

int process_spawn_builtin(const char* name) {
    static uint8_t image[PROCESS_IMAGE_MAX];
    uint32_t image_size;
    int pid;

    /* 先把内建程序翻译成一页机器码镜像，再走通用建进程流程。 */
    if (!process_build_builtin_image(name, image, sizeof(image), &image_size)) {
        return 0;
    }

    pid = process_spawn_from_buffer(name, image, image_size);
    if (pid == 0) {
        return 0;
    }

    console_write("created process pid=");
    console_write_dec(pid);
    console_put_char('\n');
    return pid;
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
    } else {
        return 0;
    }

    *image_size_out = image_size;
    return 1;
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
    /* 从 schedule_cursor 开始做一圈线性扫描，实现最简单的 round-robin。 */
    for (int offset = 0; offset < PROCESS_MAX; offset++) {
        int i = (schedule_cursor + offset) % PROCESS_MAX;

        if (processes[i].used && processes[i].state == PROCESS_READY) {
            int pid = processes[i].pid;

            if (!process_run(pid)) {
                console_write_line("process run failed");
                return 0;
            }

            /* 下次从当前成功运行项的下一个槽位开始找。 */
            schedule_cursor = (i + 1) % PROCESS_MAX;
            Process* process = find_process_by_pid(pid);
            if (process != (Process*)0 && process->state == PROCESS_ZOMBIE) {
                console_write("process exited: pid=");
                console_write_dec(pid);
                console_write(" code=");
                console_write_dec((int)process->exit_code);
                console_put_char('\n');
            }
            return pid;
        }
    }

    return 0;
}

int process_run_builtin(const char* name) {
    int pid = process_spawn_builtin(name);
    Process* process;

    if (pid == 0) {
        return 0;
    }

    if (!process_run(pid)) {
        console_write_line("process run failed");
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

int process_auto_schedule_enabled(void) {
    return auto_schedule_enabled;
}

void process_set_auto_schedule(int enabled) {
    auto_schedule_enabled = enabled ? 1 : 0;
}

int process_reap_zombies(void) {
    int reaped = 0;

    /* 把所有已经 exit 的进程统一回收掉。 */
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].used && processes[i].state == PROCESS_ZOMBIE) {
            release_process(&processes[i]);
            reaped++;
        }
    }

    return reaped;
}

void process_print_table(void) {
    console_write_line("PID  STATE    NAME");

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].used) {
            continue;
        }

        console_write_dec(processes[i].pid);
        console_write("    ");

        if (processes[i].state == PROCESS_READY) {
            console_write("READY    ");
        } else if (processes[i].state == PROCESS_RUNNING) {
            console_write("RUNNING  ");
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
