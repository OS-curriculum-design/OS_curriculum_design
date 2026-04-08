#include "usermode.h"

#include "../console/console.h"
#include "../mm/vmm.h"
#include "../shell/shell.h"
#include "process.h"

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

        if (length > 512U) {
            length = 512U;
        }

        shell_note_async_output();
        for (uint32_t i = 0; i < length; i++) {
            console_put_char(text[i]);
        }

        frame->eax = length;
        return;
    }

    if (frame->eax == SYS_EXIT) {
        usermode_return_to_kernel(frame->ebx);
    }

    if (frame->eax == SYS_YIELD) {
        process_save_yield_frame(frame);
        usermode_return_to_kernel(USERMODE_RETURN_YIELD);
    }

    usermode_return_to_kernel(frame->eax);
}
