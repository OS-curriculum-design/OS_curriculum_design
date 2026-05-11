#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../include/types.h"

/*
 * 键盘驱动接口
 * ============
 * 键盘中断会把按键事件写入内部环形缓冲区，Shell 再从这里取字符或方向键。
 */

#define KEYBOARD_KEY_UP   0x0100U
#define KEYBOARD_KEY_DOWN 0x0101U

/* 注册 IRQ1 处理函数并清空内部状态。 */
void keyboard_init(void);
/* 尝试读出一个按键，成功返回 1。 */
int keyboard_read_key(uint16_t* out);
/* 判断缓冲区里是否还有待消费的按键。 */
int keyboard_has_key(void);

#endif
