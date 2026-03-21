#ifndef CONSOLE_H
#define CONSOLE_H

#include "../include/types.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

void console_clear(void);
void console_set_color(uint8_t fg, uint8_t bg);
void console_put_char(char c);
void console_write(const char* str);
void console_write_line(const char* str);
void console_write_dec(int value);
void console_write_hex(uint32_t value);

void console_put_char_at(char c, size_t row, size_t col, uint8_t color);
void console_write_at(const char* str, size_t row, size_t col, uint8_t color);
void console_clear_line(size_t row, uint8_t color);

#endif
