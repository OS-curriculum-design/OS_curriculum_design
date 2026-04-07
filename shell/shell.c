#include "shell.h"
#include "../console/console.h"
#include "../drivers/ata.h"
#include "../include/string.h"
#include "../kernel/usermode.h"
#include "../mm/pager.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../timer/timer.h"

// Shell 当前支持的单行输入最大长度。
// 这里预留 1 个字节给字符串结束符 '\0'，
// 因此用户实际最多只能输入 127 个可见字符。
#define INPUT_MAX 128
#define PAGER_TEST_BASE 0x40000000U
#define PAGER_TEST_PAGE_COUNT 20U

// 保存用户当前正在输入的一整行命令。
// 这是一个静态全局缓冲区，Shell 按“整行输入、回车执行”的方式工作。
static char input_buffer[INPUT_MAX];

// 记录当前输入缓冲区中已经存放了多少个字符。
// 它始终指向下一次写入的位置。
static int input_len = 0;

static int demo_user_space_ready = 0;
static VmmUserSpaceInfo demo_user_space;

static void run_pager_fault_test(void) {
    if (!pager_is_ready()) {
        console_write_line("pager is not ready");
        return;
    }

    console_write_line("Pager fault test using real #PF");
    for (uint32_t i = 0; i < PAGER_TEST_PAGE_COUNT; i++) {
        if (!pager_register_page(PAGER_TEST_BASE + i * PAGE_SIZE, VMM_PAGE_WRITABLE)) {
            console_write_line("pager test: register failed");
            return;
        }
    }

    console_write_line("access pattern: pages 0..19, then 0..3");

    for (uint32_t i = 0; i < PAGER_TEST_PAGE_COUNT; i++) {
        uint32_t* ptr = (uint32_t*)(PAGER_TEST_BASE + i * PAGE_SIZE);
        *ptr = 0xC1000000U | i;
    }

    for (uint32_t i = 0; i < 4U; i++) {
        uint32_t* ptr = (uint32_t*)(PAGER_TEST_BASE + i * PAGE_SIZE);
        *ptr = 0xC1001000U | i;
    }

    pager_print_stats();
}

static void print_memory_stats(void) {
    console_write("Physical memory: ");
    console_write_dec((int)(pmm_get_total_memory_bytes() / (1024U * 1024U)));
    console_write_line(" MiB");

    console_write("Page size: ");
    console_write_dec((int)PAGE_SIZE);
    console_write_line(" bytes");

    console_write("Total pages: ");
    console_write_dec((int)pmm_get_total_pages());
    console_put_char('\n');

    console_write("Used pages: ");
    console_write_dec((int)pmm_get_used_pages());
    console_put_char('\n');

    console_write("Free pages: ");
    console_write_dec((int)pmm_get_free_pages());
    console_put_char('\n');

    console_write("Bitmap base: ");
    console_write_hex(pmm_get_bitmap_base());
    console_put_char('\n');

    console_write("Bitmap size: ");
    console_write_dec((int)pmm_get_bitmap_size_bytes());
    console_write_line(" bytes");

    console_write("Paging: ");
    console_write_line(vmm_is_paging_enabled() ? "enabled" : "disabled");

    console_write("Page directory: ");
    console_write_hex(vmm_get_page_directory());
    console_put_char('\n');

    console_write("Kernel base: ");
    console_write_hex(VMM_KERNEL_BASE);
    console_put_char('\n');

    console_write("Identity mapped: ");
    console_write_dec((int)(vmm_get_identity_mapped_bytes() / (1024U * 1024U)));
    console_write_line(" MiB");

    console_write("Kernel mapped: ");
    console_write_dec((int)(vmm_get_kernel_mapped_bytes() / (1024U * 1024U)));
    console_write_line(" MiB");

    console_write("Mapped pages: ");
    console_write_dec((int)vmm_get_mapped_pages());
    console_put_char('\n');

    console_write("ATA disk: ");
    console_write_line(ata_is_ready() ? "ready" : "not ready");
}

static void print_user_vm_demo(void) {
    uint32_t resolved_phys = 0;

    if (!vmm_is_ready()) {
        console_write_line("Virtual memory manager is not ready.");
        return;
    }

    if (!demo_user_space_ready) {
        if (!vmm_create_user_demo_space(&demo_user_space)) {
            console_write_line("Failed to create user address space.");
            return;
        }
        demo_user_space_ready = 1;
    }

    console_write_line("User address space demo:");

    console_write("  page directory: ");
    console_write_hex(demo_user_space.page_directory_phys);
    console_put_char('\n');

    console_write("  user range: 0x00000000 - ");
    console_write_hex(VMM_USER_TOP - 1U);
    console_put_char('\n');

    console_write("  kernel range: ");
    console_write_hex(VMM_KERNEL_BASE);
    console_write_line(" - 0xFFFFFFFF");

    console_write("  code: ");
    console_write_hex(demo_user_space.code_virt);
    console_write(" -> ");
    if (vmm_get_mapping_in_directory(demo_user_space.page_directory_phys,
                                     demo_user_space.code_virt,
                                     &resolved_phys)) {
        console_write_hex(resolved_phys);
    } else {
        console_write("not mapped");
    }
    console_put_char('\n');

    console_write("  stack page: ");
    console_write_hex(demo_user_space.stack_page_virt);
    console_write(" -> ");
    if (vmm_get_mapping_in_directory(demo_user_space.page_directory_phys,
                                     demo_user_space.stack_page_virt,
                                     &resolved_phys)) {
        console_write_hex(resolved_phys);
    } else {
        console_write("not mapped");
    }
    console_put_char('\n');

    console_write("  initial user esp: ");
    console_write_hex(demo_user_space.stack_top);
    console_put_char('\n');
}

// 将一个十进制数字字符串解析为无符号 32 位整数。
//
// 参数：
// - str: 待解析的 C 字符串，要求内容必须全部是 '0' ~ '9'
// - out: 成功时用于接收解析结果
//
// 返回值：
// - 1: 解析成功
// - 0: 解析失败（空串，或包含非数字字符）
//
// 说明：
// - 当前实现只支持纯十进制正整数
// - 不支持前导空格、正负号、十六进制等格式
static int parse_uint(const char* str, uint32_t* out) {
    uint32_t value = 0;

    // 空字符串不认为是合法数字。
    if (!str[0]) return 0;

    // 逐字符检查并累积数值：
    // 例如 "123" => ((1 * 10 + 2) * 10 + 3)
    for (size_t i = 0; str[i]; i++) {
        // 只要出现一个非数字字符，立即判定失败。
        if (str[i] < '0' || str[i] > '9') return 0;
        value = value * 10 + (uint32_t)(str[i] - '0');
    }

    // 所有字符都合法时，把结果写回调用方。
    *out = value;
    return 1;
}

// 将 tick 数换算为毫秒数。
//
// 计算公式：
//   ms = ticks * 1000 / frequency
//
// 其中 frequency 表示每秒多少个 tick。
// 例如 frequency = 100 时，1 tick = 10 ms。
static uint32_t ticks_to_ms(uint32_t ticks) {
    uint32_t frequency = timer_get_frequency();

    // 防止除以 0。
    // 如果定时器频率尚未初始化，则返回 0。
    if (frequency == 0) {
        return 0;
    }

    return (ticks * 1000U) / frequency;
}

// 执行一条已经输入完成的命令字符串。
//
// 这里采用最直接的字符串比较方式来分发命令，
// 属于一个最小可用 Shell 的实现：
// - 不支持管道、重定向、参数拆分
// - 只支持少量内建命令
static void run_command(const char* cmd) {
    // 空命令直接忽略。
    // 例如用户只按回车，不做任何操作。
    if (strcmp(cmd, "") == 0) return;

    // clear：清空整个屏幕。
    if (strcmp(cmd, "clear") == 0) {
        console_clear();
        return;
    }

    // ticks：显示当前定时器累计的 tick 数。
    if (strcmp(cmd, "ticks") == 0) {
        console_write("Timer ticks: ");
        console_write_dec((int)timer_get_ticks());
        console_put_char('\n');
        return;
    }

    // mem：显示物理页位图管理器的当前状态。
    if (strcmp(cmd, "mem") == 0) {
        if (!pmm_is_ready()) {
            console_write_line("Physical memory manager is not ready.");
            return;
        }

        print_memory_stats();
        return;
    }

    // uservm：创建并显示一个用户虚拟地址空间示例。
    // 这里只做页表布局验证，还不会真正切换到用户态执行。
    if (strcmp(cmd, "uservm") == 0) {
        print_user_vm_demo();
        return;
    }

    // ring3：真正通过 iret 进入用户态，并用 int 0x80 返回内核。
    if (strcmp(cmd, "ring3") == 0) {
        uint32_t result;

        console_write_line("Entering ring3 demo...");
        result = usermode_run_demo();
        if (result == 0) {
            console_write_line("ring3 demo failed.");
        } else {
            console_write("Back from ring3, eax=");
            console_write_hex(result);
            console_put_char('\n');
        }
        return;
    }

    // pager：显示通用换页器状态。
    if (strcmp(cmd, "pager") == 0) {
        pager_print_stats();
        return;
    }

    // pagertest：用真实 #PF 触发通用换页器。
    if (strcmp(cmd, "pagertest") == 0) {
        run_pager_fault_test();
        return;
    }

    // slice：显示当前时间片设置，同时输出 tick 和毫秒两种单位。
    if (strcmp(cmd, "slice") == 0) {
        uint32_t ticks = timer_get_timeslice();
        console_write("Round-robin time slice: ");
        console_write_dec((int)ticks);
        console_write(" ticks (");
        console_write_dec((int)ticks_to_ms(ticks));
        console_write_line(" ms)");
        return;
    }

    // slice <ticks>：修改时间片长度。
    // 这里只接受形如 "slice 10" 的纯数字参数。
    if (strncmp(cmd, "slice ", 6) == 0) {
        uint32_t ticks = 0;

        // 从命令第 7 个字符开始解析参数，即跳过 "slice " 前缀。
        if (!parse_uint(cmd + 6, &ticks)) {
            console_write_line("Usage: slice <ticks>");
            return;
        }

        // 写入新的时间片配置，并把最终生效值打印出来。
        timer_set_timeslice(ticks);
        console_write("Time slice updated to ");
        console_write_dec((int)timer_get_timeslice());
        console_write(" ticks (");
        console_write_dec((int)ticks_to_ms(timer_get_timeslice()));
        console_write_line(" ms)");
        return;
    }

    // 所有未识别的命令都会落到这里。
    console_write("Unknown command: ");
    console_write_line(cmd);
}

// 初始化 Shell 的内部状态。
// 一般在系统启动时由内核调用一次。
void shell_init(void) {
    input_len = 0;
    memset(input_buffer, 0, INPUT_MAX);
}

// 在控制台上显示命令提示符。
void shell_prompt(void) {
    console_write("MyOS> ");
}

// 处理从键盘传入的单个字符。
//
// 这是 Shell 的输入核心：
// - 普通字符：追加到缓冲区并回显
// - 退格：删除缓冲区末尾字符并擦除显示
// - 回车：结束当前命令，执行后重新显示提示符
void shell_handle_char(char c) {
    // 回车/换行：表示一条命令输入完成。
    if (c == '\n') {
        // 先在屏幕上换行，让执行结果显示在下一行。
        console_put_char('\n');

        // 把当前输入补成合法的 C 字符串后执行命令。
        input_buffer[input_len] = '\0';
        run_command(input_buffer);

        // 命令执行完成后，清空输入状态，准备接收下一行命令。
        input_len = 0;
        memset(input_buffer, 0, INPUT_MAX);

        // 输出新的提示符。
        shell_prompt();
        return;
    }

    // 退格：删除当前输入的最后一个字符。
    if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            input_buffer[input_len] = '\0';

            // 通知控制台删除屏幕上的一个字符。
            console_put_char('\b');
        }
        return;
    }

    // 只接受可打印 ASCII 字符。
    // 控制字符（例如 Tab、ESC）当前全部忽略。
    if (c < 32 || c > 126) return;

    // 缓冲区保留最后一个位置给 '\0'，因此这里不能写满整个数组。
    if (input_len >= INPUT_MAX - 1) return;

    // 把字符追加到输入缓冲，并立即回显到屏幕。
    input_buffer[input_len++] = c;
    console_put_char(c);
}
