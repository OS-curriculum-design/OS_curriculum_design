#include "console.h"

#define VGA_MEMORY ((uint16_t*)0xB8000)

static size_t cursor_row = 0;
static size_t cursor_col = 0;
static uint8_t current_color = 0x07;

static uint16_t vga_entry(unsigned char ch, uint8_t color) {
    return (uint16_t)ch | ((uint16_t)color << 8);
}

void console_set_color(uint8_t fg, uint8_t bg) {
    current_color = fg | (bg << 4);
}

static void scroll_if_needed(void) {
    if (cursor_row < VGA_HEIGHT) return;

    for (size_t row = 1; row < VGA_HEIGHT; row++) {
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            VGA_MEMORY[(row - 1) * VGA_WIDTH + col] =
                VGA_MEMORY[row * VGA_WIDTH + col];
        }
    }

    for (size_t col = 0; col < VGA_WIDTH; col++) {
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = vga_entry(' ', current_color);
    }

    cursor_row = VGA_HEIGHT - 1;
}

void console_clear_line(size_t row, uint8_t color) {
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(' ', color);
    }
}

void console_clear(void) {
    for (size_t row = 0; row < VGA_HEIGHT; row++) {
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(' ', current_color);
        }
    }
    cursor_row = 0;
    cursor_col = 0;
}

void console_put_char_at(char c, size_t row, size_t col, uint8_t color) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return;
    VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(c, color);
}

void console_write_at(const char* str, size_t row, size_t col, uint8_t color) {
    size_t i = 0;
    while (str[i] && col + i < VGA_WIDTH) {
        console_put_char_at(str[i], row, col + i, color);
        i++;
    }
}

void console_put_char(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        return;
    }

    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            console_put_char_at(' ', cursor_row, cursor_col, current_color);
        }
        return;
    }

    console_put_char_at(c, cursor_row, cursor_col, current_color);
    cursor_col++;

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }
}

void console_write(const char* str) {
    while (*str) {
        console_put_char(*str++);
    }
}

void console_write_line(const char* str) {
    console_write(str);
    console_put_char('\n');
}

void console_write_dec(int value) {
    char buf[16];
    int i = 0;
    int neg = 0;

    if (value == 0) {
        console_put_char('0');
        return;
    }

    if (value < 0) {
        neg = 1;
        value = -value;
    }

    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    if (neg) buf[i++] = '-';

    while (i--) {
        console_put_char(buf[i]);
    }
}
