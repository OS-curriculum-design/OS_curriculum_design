#ifndef SHELL_H
#define SHELL_H

#include "../include/types.h"

/*
 * Shell 接口
 * ==========
 * Shell 负责接收键盘输入、维护命令行编辑状态，并把内建命令分发给各子模块。
 */

/* 初始化输入缓冲与提示符状态。 */
void shell_init(void);
/* 打印命令提示符。 */
void shell_prompt(void);
/* 处理一个来自键盘驱动的按键。 */
void shell_handle_key(uint16_t key);
/* 告诉 Shell 接下来会有异步输出，避免打乱当前输入行。 */
void shell_begin_async_output(void);
/* 记录一次异步输出已经发生。 */
void shell_note_async_output(void);
/* 异步输出结束后恢复提示符和用户正在输入的内容。 */
void shell_end_async_output(void);

#endif
