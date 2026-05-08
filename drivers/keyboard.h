#ifndef KEYBOARD_H
#define KEYBOARD_H

/*
 * 键盘驱动接口
 * ============
 * 键盘中断会把字符写入内部环形缓冲区，Shell 再从这里取字符。
 */

/* 注册 IRQ1 处理函数并清空内部状态。 */
void keyboard_init(void);
/* 尝试读出一个字符，成功返回 1。 */
int keyboard_read_char(char* out);
/* 判断缓冲区里是否还有待消费的字符。 */
int keyboard_has_char(void);

#endif
