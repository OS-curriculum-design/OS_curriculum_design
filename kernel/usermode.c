#include "usermode.h"

#include "../mm/vmm.h"

#define USERMODE_DEMO_SYSCALL_MAGIC 0x2BADCAFEU

extern uint32_t usermode_enter(uint32_t entry, uint32_t user_esp);

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
    uint32_t value = 0xBAD08080U;

    if (frame != (InterruptFrame*)0) {
        value = frame->eax;
    }

    /*
     * 演示阶段：只要用户态能执行 int 0x80 进入这里，就直接返回 shell。
     * 不在中断栈上打印，避免把“系统调用是否成功”和“控制台输出是否安全”
     * 混在一起。
     */
    usermode_return_to_kernel(value);
}
