/*
 * 用户态与系统调用实现
 * ====================
 * 这个模块提供一个最小的 ring3 演示环境，并处理 int 0x80 系统调用。
 */
#include "usermode.h"

#include "../console/console.h"
#include "../mm/vmm.h"
#include "../shell/shell.h"
#include "memdemo.h"
#include "process.h"

/* 只在第一次使用时构造演示地址空间，后续复用。 */
static int ring3_demo_ready = 0;
static VmmUserSpaceInfo ring3_demo_space;

/*
 * ring3 演示程序机器码：
 *   mov $0x2BADCAFE, %eax
 *   int $0x80
 *   jmp .
 *
 * syscall 处理函数会在 int 0x80 时把控制权切回内核，
 * 所以正常情况下不会执行到最后的死循环。
 */
static const uint8_t ring3_demo_code[] = {
    0xB8, 0xFE, 0xCA, 0xAD, 0x2B,
    0xCD, 0x80,
    0xEB, 0xFE
};

uint32_t usermode_run_demo(void) {
    if (!ring3_demo_ready) {
        /* 把内嵌机器码复制到用户空间代码页中。 */
        if (!vmm_create_current_user_demo_space(&ring3_demo_space,
                                                ring3_demo_code,
                                                sizeof(ring3_demo_code))) {
            return 0;
        }

        ring3_demo_ready = 1;
    }

    return usermode_enter(ring3_demo_space.code_virt, ring3_demo_space.stack_top);
}

void usermode_handle_syscall(InterruptFrame* frame) {
    if (frame == (InterruptFrame*)0) {
        usermode_return_to_kernel(0xBAD08080U);
    }

    if (frame->eax == SYS_WRITE) {
        const char* text = (const char*)frame->ebx;
        uint32_t length = frame->ecx;

        /* 防御性限制输出长度，避免用户态一次打印过长数据。 */
        if (length > 512U) {
            length = 512U;
        }

        /* 告诉 Shell 这段输出来自异步上下文，必要时恢复命令行。 */
        shell_note_async_output();
        for (uint32_t i = 0; i < length; i++) {
            console_put_char(text[i]);
        }

        frame->eax = length;
        return;
    }

    if (frame->eax == SYS_EXIT) {
        /* exit 直接结束当前用户态执行，不再返回原用户指令。 */
        usermode_return_to_kernel(frame->ebx);
    }

    if (frame->eax == SYS_YIELD) {
        /* yield 先保存寄存器现场，再回到内核让调度器决定后续运行者。 */
        process_save_yield_frame(frame);
        usermode_return_to_kernel(USERMODE_RETURN_YIELD);
    }

    /* 未识别系统调用：把 eax 原样作为返回值带回去，便于调试。 */
    if (frame->eax == SYS_MEMDEMO_OP) {
        frame->eax = (uint32_t)memdemo_apply_op(frame->ebx, frame->ecx);
        return;
    }

    if (frame->eax == SYS_MEMDEMO_RESET) {
        memdemo_reset();
        frame->eax = 1U;
        return;
    }

    if (frame->eax == SYS_MEMDEMO_REPORT) {
        shell_note_async_output();
        frame->eax = (uint32_t)memdemo_report_event(frame->ebx);
        return;
    }

    usermode_return_to_kernel(frame->eax);
}
