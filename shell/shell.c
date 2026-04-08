#include "shell.h"
#include "../console/console.h"
#include "../drivers/ata.h"
#include "../fs/simplefs.h"
#include "../include/string.h"
#include "../kernel/process.h"
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
static int prompt_visible = 0;
static int async_output_active = 0;
static int async_output_dirty = 0;
static uint8_t fs_command_buffer[SIMPLEFS_MAX_FILE_SIZE + 1U];

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

static const char* skip_spaces(const char* str) {
    while (*str == ' ') {
        str++;
    }

    return str;
}

static int parse_leading_uint(const char** str_io, uint32_t* out) {
    const char* str = skip_spaces(*str_io);
    uint32_t value = 0;

    if (*str < '0' || *str > '9') {
        return 0;
    }

    while (*str >= '0' && *str <= '9') {
        value = value * 10U + (uint32_t)(*str - '0');
        str++;
    }

    *out = value;
    *str_io = str;
    return 1;
}

static int split_name_and_text(const char* args, char* name_out, uint32_t name_size, const char** text_out) {
    uint32_t i = 0;

    args = skip_spaces(args);
    if (*args == '\0') {
        return 0;
    }

    while (args[i] && args[i] != ' ') {
        if (i + 1U >= name_size) {
            return 0;
        }
        name_out[i] = args[i];
        i++;
    }
    name_out[i] = '\0';

    *text_out = skip_spaces(args + i);
    return 1;
}

static void fs_print_mount_hint(void) {
    if (!simplefs_is_mounted()) {
        console_write_line("SimpleFS is not mounted. Run mkfs first.");
    }
}

static void print_help(void) {
    console_write_line("Core:");
    console_write_line("  help clear ticks mem uservm ring3 pager pagertest");
    console_write_line("Process:");
    console_write_line("  ps run <app> spawn <app> sched autosched [on|off] reap slice [ticks]");
    console_write_line("  apps: hello counter busy");
    console_write_line("Files:");
    console_write_line("  mkfs fsstat pwd ls mkdir <dir> cd <dir|..|/> rmdir <dir>");
    console_write_line("  touch <file> rm <file> cat <file> write <file> <text>");
    console_write_line("  append <file> <text> edit <file> <text>");
    console_write_line("FD:");
    console_write_line("  open <file> close <fd> fds read <fd> writefd <fd> <text> seek <fd> <offset>");
    console_write_line("Apps on FS:");
    console_write_line("  installapps exec <file.app>");
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

    // help：显示当前 shell 支持的主要命令。
    if (strcmp(cmd, "help") == 0) {
        print_help();
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

    // mkfs：在 disk.img 的 SimpleFS 区域上格式化根文件系统。
    if (strcmp(cmd, "mkfs") == 0) {
        console_write_line(simplefs_format() ? "SimpleFS formatted." : "mkfs failed.");
        return;
    }

    // ls：列出根目录文件。
    if (strcmp(cmd, "ls") == 0) {
        simplefs_list();
        return;
    }

    // fsstat：显示 SimpleFS 状态。
    if (strcmp(cmd, "fsstat") == 0) {
        simplefs_print_stats();
        return;
    }

    // pwd：显示当前目录。
    if (strcmp(cmd, "pwd") == 0) {
        simplefs_print_working_directory();
        return;
    }

    // mkdir <name>：创建当前目录下的子目录。
    if (strncmp(cmd, "mkdir ", 6) == 0) {
        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            console_write_line(simplefs_make_dir(skip_spaces(cmd + 6)) ? "directory created." : "mkdir failed.");
        }
        return;
    }

    // rmdir <name>：删除当前目录下的空目录。
    if (strncmp(cmd, "rmdir ", 6) == 0) {
        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            console_write_line(simplefs_remove_dir(skip_spaces(cmd + 6)) ? "directory removed." : "rmdir failed.");
        }
        return;
    }

    // cd <name|..|/>：切换当前目录。
    if (strncmp(cmd, "cd ", 3) == 0) {
        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            console_write_line(simplefs_change_dir(skip_spaces(cmd + 3)) ? "directory changed." : "cd failed.");
        }
        return;
    }

    // touch <name>：创建空文本文件。
    if (strncmp(cmd, "touch ", 6) == 0) {
        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            console_write_line(simplefs_create(skip_spaces(cmd + 6)) ? "file created." : "touch failed.");
        }
        return;
    }

    // rm <name>：删除文件。
    if (strncmp(cmd, "rm ", 3) == 0) {
        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            console_write_line(simplefs_delete(skip_spaces(cmd + 3)) ? "file removed." : "rm failed.");
        }
        return;
    }

    // cat <name>：打印文本文件内容。
    if (strncmp(cmd, "cat ", 4) == 0) {
        uint32_t bytes_read = 0;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (simplefs_read_file(skip_spaces(cmd + 4), fs_command_buffer, SIMPLEFS_MAX_FILE_SIZE, &bytes_read)) {
                fs_command_buffer[bytes_read] = '\0';
                console_write((const char*)fs_command_buffer);
                if (bytes_read == 0 || fs_command_buffer[bytes_read - 1U] != '\n') {
                    console_put_char('\n');
                }
            } else {
                console_write_line("cat failed.");
            }
        }
        return;
    }

    // write <name> <text>：覆盖写入文本文件。
    if (strncmp(cmd, "write ", 6) == 0) {
        char name[28];
        const char* text;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (split_name_and_text(cmd + 6, name, sizeof(name), &text) &&
                simplefs_write_file(name, (const uint8_t*)text, (uint32_t)strlen(text))) {
                console_write_line("file written.");
            } else {
                console_write_line("Usage: write <name> <text>");
            }
        }
        return;
    }

    // append <name> <text>：追加文本。
    if (strncmp(cmd, "append ", 7) == 0) {
        char name[28];
        const char* text;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (split_name_and_text(cmd + 7, name, sizeof(name), &text) &&
                simplefs_append_file(name, (const uint8_t*)text, (uint32_t)strlen(text))) {
                console_write_line("file appended.");
            } else {
                console_write_line("Usage: append <name> <text>");
            }
        }
        return;
    }

    // edit <name> <text>：第一版简化编辑器，本质是覆盖写入文本。
    if (strncmp(cmd, "edit ", 5) == 0) {
        char name[28];
        const char* text;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (split_name_and_text(cmd + 5, name, sizeof(name), &text) &&
                simplefs_write_file(name, (const uint8_t*)text, (uint32_t)strlen(text))) {
                console_write_line("file edited.");
            } else {
                console_write_line("Usage: edit <name> <text>");
            }
        }
        return;
    }

    // installapps：把当前内置用户程序镜像写入 SimpleFS。
    if (strcmp(cmd, "installapps") == 0) {
        uint32_t image_size = 0;
        int ok = 1;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (!process_build_builtin_image("hello", fs_command_buffer, SIMPLEFS_MAX_FILE_SIZE, &image_size) ||
                !simplefs_write_file("hello.app", fs_command_buffer, image_size)) {
                ok = 0;
            }
            if (!process_build_builtin_image("counter", fs_command_buffer, SIMPLEFS_MAX_FILE_SIZE, &image_size) ||
                !simplefs_write_file("counter.app", fs_command_buffer, image_size)) {
                ok = 0;
            }
            if (!process_build_builtin_image("busy", fs_command_buffer, SIMPLEFS_MAX_FILE_SIZE, &image_size) ||
                !simplefs_write_file("busy.app", fs_command_buffer, image_size)) {
                ok = 0;
            }

            console_write_line(ok ? "apps installed." : "installapps failed.");
        }
        return;
    }

    // exec <file.app>：从 SimpleFS 读取程序镜像并创建进程。
    if (strncmp(cmd, "exec ", 5) == 0) {
        uint32_t image_size = 0;
        int pid;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (!simplefs_read_file(skip_spaces(cmd + 5), fs_command_buffer, SIMPLEFS_MAX_FILE_SIZE, &image_size) || image_size == 0) {
                console_write_line("exec failed: cannot read app.");
                return;
            }

            pid = process_spawn_from_buffer(skip_spaces(cmd + 5), fs_command_buffer, image_size);
            if (pid == 0) {
                console_write_line("exec failed: cannot create process.");
                return;
            }

            console_write("created process pid=");
            console_write_dec(pid);
            console_put_char('\n');
        }
        return;
    }

    // open <name> / close <fd> / fds：演示打开文件表。
    if (strncmp(cmd, "open ", 5) == 0) {
        int fd;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            fd = simplefs_open(skip_spaces(cmd + 5));
            if (fd >= 0) {
                console_write("fd=");
                console_write_dec(fd);
                console_put_char('\n');
            } else {
                console_write_line("open failed.");
            }
        }
        return;
    }

    if (strncmp(cmd, "close ", 6) == 0) {
        uint32_t fd = 0;

        if (!parse_uint(skip_spaces(cmd + 6), &fd)) {
            console_write_line("Usage: close <fd>");
            return;
        }

        console_write_line(simplefs_close((int)fd) ? "file closed." : "close failed.");
        return;
    }

    if (strcmp(cmd, "fds") == 0) {
        simplefs_print_open_files();
        return;
    }

    // read <fd>：从当前 fd 偏移读到文件末尾，并推进 fd offset。
    if (strncmp(cmd, "read ", 5) == 0) {
        uint32_t fd = 0;
        uint32_t bytes_read = 0;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (!parse_uint(skip_spaces(cmd + 5), &fd)) {
                console_write_line("Usage: read <fd>");
                return;
            }

            if (simplefs_read_fd((int)fd, fs_command_buffer, SIMPLEFS_MAX_FILE_SIZE, &bytes_read)) {
                fs_command_buffer[bytes_read] = '\0';
                console_write((const char*)fs_command_buffer);
                if (bytes_read == 0 || fs_command_buffer[bytes_read - 1U] != '\n') {
                    console_put_char('\n');
                }
            } else {
                console_write_line("read failed.");
            }
        }
        return;
    }

    // writefd <fd> <text>：从当前 fd 偏移写入文本，并推进 fd offset。
    if (strncmp(cmd, "writefd ", 8) == 0) {
        const char* args = cmd + 8;
        const char* text;
        uint32_t fd = 0;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (!parse_leading_uint(&args, &fd)) {
                console_write_line("Usage: writefd <fd> <text>");
                return;
            }

            text = skip_spaces(args);
            if (simplefs_write_fd((int)fd, (const uint8_t*)text, (uint32_t)strlen(text))) {
                console_write_line("fd written.");
            } else {
                console_write_line("writefd failed.");
            }
        }
        return;
    }

    // seek <fd> <offset>：调整 fd 的当前读写偏移。
    if (strncmp(cmd, "seek ", 5) == 0) {
        const char* args = cmd + 5;
        uint32_t fd = 0;
        uint32_t offset = 0;

        fs_print_mount_hint();
        if (simplefs_is_mounted()) {
            if (!parse_leading_uint(&args, &fd) || !parse_leading_uint(&args, &offset)) {
                console_write_line("Usage: seek <fd> <offset>");
                return;
            }

            console_write_line(simplefs_seek((int)fd, offset) ? "fd offset updated." : "seek failed.");
        }
        return;
    }

    // ps：显示当前进程表。
    if (strcmp(cmd, "ps") == 0) {
        process_print_table();
        return;
    }

    // run hello：创建并运行一个内置用户进程。
    if (strncmp(cmd, "run ", 4) == 0) {
        if (!process_run_builtin(cmd + 4)) {
            console_write_line("Usage: run hello|counter|busy");
        }
        return;
    }

    // spawn hello：只创建进程，放入 READY 队列，等待 sched 调度。
    if (strncmp(cmd, "spawn ", 6) == 0) {
        if (!process_spawn_builtin(cmd + 6)) {
            console_write_line("Usage: spawn hello|counter|busy");
        }
        return;
    }

    // sched：运行下一个 READY 进程。
    if (strcmp(cmd, "sched") == 0) {
        process_schedule();
        return;
    }

    // autosched：查看/控制时钟驱动的自动进程调度。
    if (strcmp(cmd, "autosched") == 0) {
        console_write("auto scheduling: ");
        console_write_line(process_auto_schedule_enabled() ? "on" : "off");
        return;
    }

    if (strcmp(cmd, "autosched on") == 0) {
        process_set_auto_schedule(1);
        console_write_line("auto scheduling: on");
        return;
    }

    if (strcmp(cmd, "autosched off") == 0) {
        process_set_auto_schedule(0);
        console_write_line("auto scheduling: off");
        return;
    }

    // reap：回收已经退出的 ZOMBIE 进程，释放它们的页。
    if (strcmp(cmd, "reap") == 0) {
        int count = process_reap_zombies();
        console_write("reaped zombies: ");
        console_write_dec(count);
        console_put_char('\n');
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
    prompt_visible = 0;
    async_output_active = 0;
    async_output_dirty = 0;
    memset(input_buffer, 0, INPUT_MAX);
}

// 在控制台上显示命令提示符。
void shell_prompt(void) {
    console_write("MyOS> ");
    prompt_visible = 1;
}

void shell_begin_async_output(void) {
    async_output_active = 1;
    async_output_dirty = 0;
}

void shell_note_async_output(void) {
    if (async_output_active && !async_output_dirty && prompt_visible) {
        console_put_char('\n');
        prompt_visible = 0;
    }

    async_output_dirty = 1;
}

void shell_end_async_output(void) {
    if (async_output_active && async_output_dirty && !prompt_visible) {
        shell_prompt();
        for (int i = 0; i < input_len; i++) {
            console_put_char(input_buffer[i]);
        }
    }

    async_output_active = 0;
    async_output_dirty = 0;
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
        prompt_visible = 0;

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
