/*
 * VGA 文本控制台实现
 * ===================
 * 这个模块直接操作 0xB8000 文本显存，并额外维护一份历史缓冲，
 * 让用户可以在文本模式下回看已经滚出屏幕的内容。
 */
#include "console.h"

#define VGA_MEMORY ((uint16_t*)0xB8000)

/* 当前命令行输出光标位置，按“历史缓冲中的绝对行号”记录。 */
static size_t cursor_row = 0;
static size_t cursor_col = 0;
/* 视图顶部所在的绝对行号。为 0 表示正从最上面的历史行开始显示。 */
static size_t viewport_top = 0;
/* 历史缓冲中当前保留的最老一行的绝对行号。 */
static size_t history_first_row = 0;
/* 历史缓冲中当前有效的行数，范围是 1 ~ CONSOLE_HISTORY_LINES。 */
static size_t history_row_count = 1;
/* VGA 文本属性：低 4 位是前景色，高 4 位是背景色。 */
static uint8_t current_color = 0x07;
/* 每一行都保存完整的 80 列字符和颜色，便于整屏重绘。 */
static uint16_t history[CONSOLE_HISTORY_LINES][VGA_WIDTH];

static uint16_t vga_entry(unsigned char ch, uint8_t color) {
    /* VGA 文本模式每个单元占 2 字节：低字节字符，高字节颜色。 */
    return (uint16_t)ch | ((uint16_t)color << 8);
}

static size_t history_slot(size_t absolute_row) {
    return absolute_row % CONSOLE_HISTORY_LINES;
}

static void clear_history_row(size_t absolute_row, uint8_t color) {
    size_t slot = history_slot(absolute_row);

    for (size_t col = 0; col < VGA_WIDTH; col++) {
        history[slot][col] = vga_entry(' ', color);
    }
}

static size_t bottom_viewport_top(void) {
    if (cursor_row + 1U <= VGA_HEIGHT) {
        return 0;
    }

    return cursor_row - (VGA_HEIGHT - 1U);
}

static void render_viewport(void) {
    size_t history_last_row = history_first_row + history_row_count;

    for (size_t screen_row = 0; screen_row < VGA_HEIGHT; screen_row++) {
        size_t absolute_row = viewport_top + screen_row;

        for (size_t col = 0; col < VGA_WIDTH; col++) {
            if (absolute_row >= history_first_row && absolute_row < history_last_row) {
                VGA_MEMORY[screen_row * VGA_WIDTH + col] = history[history_slot(absolute_row)][col];
            } else {
                VGA_MEMORY[screen_row * VGA_WIDTH + col] = vga_entry(' ', current_color);
            }
        }
    }
}

static void clamp_viewport(void) {
    size_t bottom = bottom_viewport_top();

    if (viewport_top < history_first_row) {
        viewport_top = history_first_row;
    }

    if (viewport_top > bottom) {
        viewport_top = bottom;
    }
}

static void advance_to_next_line(void) {
    cursor_row++;

    if (history_row_count < CONSOLE_HISTORY_LINES) {
        history_row_count++;
    } else {
        history_first_row++;
    }

    clear_history_row(cursor_row, current_color);
    clamp_viewport();
}

static void write_visible_cell(char c, size_t row, size_t col, uint8_t color) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return;
    }

    VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry((unsigned char)c, color);

    {
        size_t absolute_row = viewport_top + row;
        size_t history_last_row = history_first_row + history_row_count;

        if (absolute_row >= history_first_row && absolute_row < history_last_row) {
            history[history_slot(absolute_row)][col] = vga_entry((unsigned char)c, color);
        }
    }
}

void console_set_color(uint8_t fg, uint8_t bg) {
    current_color = fg | (bg << 4);
}

void console_clear_line(size_t row, uint8_t color) {
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        write_visible_cell(' ', row, col, color);
    }
}

void console_clear(void) {
    cursor_row = 0;
    cursor_col = 0;
    viewport_top = 0;
    history_first_row = 0;
    history_row_count = 1;
    clear_history_row(0, current_color);
    render_viewport();
}

void console_put_char_at(char c, size_t row, size_t col, uint8_t color) {
    write_visible_cell(c, row, col, color);
}

void console_write_at(const char* str, size_t row, size_t col, uint8_t color) {
    size_t i = 0;
    while (str[i] && col + i < VGA_WIDTH) {
        console_put_char_at(str[i], row, col + i, color);
        i++;
    }
}

void console_put_char(char c) {
    int follow_bottom = !console_is_scrolled();

    if (c == '\n') {
        /* 换行只移动光标，不主动写入额外字符。 */
        cursor_col = 0;
        advance_to_next_line();
    } else if (c == '\b') {
        if (cursor_col > 0) {
            /* 退格目前只支持行内回退，不跨行。 */
            cursor_col--;
            history[history_slot(cursor_row)][cursor_col] = vga_entry(' ', current_color);
        }
    } else {
        history[history_slot(cursor_row)][cursor_col] = vga_entry((unsigned char)c, current_color);
        cursor_col++;

        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            advance_to_next_line();
        }
    }

    if (follow_bottom) {
        viewport_top = bottom_viewport_top();
    } else {
        clamp_viewport();
    }

    render_viewport();
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

    if (neg) {
        buf[i++] = '-';
    }

    while (i--) {
        console_put_char(buf[i]);
    }
}

/* 固定输出 8 个十六进制半字节，适合打印地址和寄存器。 */
void console_write_hex(uint32_t value) {
    static const char hex_digits[] = "0123456789ABCDEF";

    console_write("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        console_put_char(hex_digits[(value >> shift) & 0x0F]);
    }
}

void console_scroll_up(size_t lines) {
    size_t old_top = viewport_top;

    if (lines > viewport_top - history_first_row) {
        viewport_top = history_first_row;
    } else {
        viewport_top -= lines;
    }

    if (viewport_top != old_top) {
        render_viewport();
    }
}

void console_scroll_down(size_t lines) {
    size_t old_top = viewport_top;
    size_t bottom = bottom_viewport_top();

    if (viewport_top + lines > bottom) {
        viewport_top = bottom;
    } else {
        viewport_top += lines;
    }

    if (viewport_top != old_top) {
        render_viewport();
    }
}

void console_scroll_to_bottom(void) {
    size_t bottom = bottom_viewport_top();

    if (viewport_top != bottom) {
        viewport_top = bottom;
        render_viewport();
    }
}

int console_is_scrolled(void) {
    return viewport_top != bottom_viewport_top();
}
