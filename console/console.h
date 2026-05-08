#ifndef CONSOLE_H
#define CONSOLE_H

#include "../include/types.h"

/*
 * 控制台模块
 * ==========
 * 这个模块直接操作 VGA 文本模式显存，提供最基础的字符输出能力。
 * 内核启动早期还没有串口、图形界面和标准库时，调试信息主要靠它输出。
 */

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

/* 清空整个屏幕，并把光标重置到左上角。 */
void console_clear(void);
/* 设置后续输出使用的前景色/背景色。 */
void console_set_color(uint8_t fg, uint8_t bg);
/* 在当前光标位置输出单个字符。 */
void console_put_char(char c);
/* 输出以 '\0' 结尾的字符串。 */
void console_write(const char* str);
/* 输出一行文本，并自动补一个换行。 */
void console_write_line(const char* str);
/* 以十进制格式输出整数。 */
void console_write_dec(int value);
/* 以 0xXXXXXXXX 形式输出 32 位十六进制值。 */
void console_write_hex(uint32_t value);

/* 在指定行列输出字符，不影响全局光标。 */
void console_put_char_at(char c, size_t row, size_t col, uint8_t color);
/* 在指定位置输出字符串，不影响全局光标。 */
void console_write_at(const char* str, size_t row, size_t col, uint8_t color);
/* 用指定颜色把某一整行清空。 */
void console_clear_line(size_t row, uint8_t color);

#endif
