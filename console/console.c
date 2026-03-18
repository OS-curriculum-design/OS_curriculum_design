#include "console.h"

#define VGA_MEMORY ((uint16_t *)0xB8000)

static size_t cursor_row = 0;
static size_t cursor_col = 0;
static uint8_t current_color = 0x07;

static uint16_t vga_entry(unsigned char ch, uint8_t color)
{
    return (uint16_t)ch | ((uint16_t)color << 8); // 将字符拼成VGA格式，注意使用小端法，高位是color，低位是字符
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    current_color = fg | (bg << 4); // fg=front ground bg=back ground 即设置背景色与字体色
}

static void scroll_if_needed(void) // 滚屏逻辑
{
    if (cursor_row < VGA_HEIGHT) // 没超出最后一行 不用滚屏
        return;

    for (size_t row = 1; row < VGA_HEIGHT; row++)
    {
        for (size_t col = 0; col < VGA_WIDTH; col++)
        {
            VGA_MEMORY[(row - 1) * VGA_WIDTH + col] =
                VGA_MEMORY[row * VGA_WIDTH + col]; // 将下一行拷贝到上一行
        }
    }

    for (size_t col = 0; col < VGA_WIDTH; col++)
    {
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = vga_entry(' ', current_color); // 拷贝后最下面一行留空 此处为何不调用console_clear_line？
    }

    cursor_row = VGA_HEIGHT - 1; // 光标为最后一行
}

void console_clear_line(size_t row, uint8_t color) // 清空一行
{
    for (size_t col = 0; col < VGA_WIDTH; col++)
    {
        VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(' ', color);
    }
}

void console_clear(void) // 清屏
{
    for (size_t row = 0; row < VGA_HEIGHT; row++)
    {
        for (size_t col = 0; col < VGA_WIDTH; col++)
        {
            VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(' ', current_color);
        }
    }
    cursor_row = 0;
    cursor_col = 0;
}

void console_put_char_at(char c, size_t row, size_t col, uint8_t color) // 在指定位置写一个字符
{
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH)
        return;
    VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry(c, color);
}

void console_write_at(const char *str, size_t row, size_t col, uint8_t color) // 在指定位置写一个字符串
{
    size_t i = 0;
    while (str[i] && col + i < VGA_WIDTH)
    {
        console_put_char_at(str[i], row, col + i, color);
        i++;
    }
}

void console_put_char(char c) // 打印一个字符
{
    if (c == '\n')
    {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        return;
    }

    if (c == '\b')
    {
        if (cursor_col > 0)
        {
            cursor_col--;
            console_put_char_at(' ', cursor_row, cursor_col, current_color);
        }
        return;
    }

    console_put_char_at(c, cursor_row, cursor_col, current_color);
    cursor_col++;

    if (cursor_col >= VGA_WIDTH)
    {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }
}

void console_write(const char *str) // 打印字符串
{
    while (*str)
    {
        console_put_char(*str++);
    }
}

void console_write_line(const char *str) // 打印一行，注意最后会添加回车
{
    console_write(str);
    console_put_char('\n');
}

void console_write_dec(int value) // 写数字
{
    char buf[16]; // 用于存储数字转换后的每一位
    int i = 0;
    int neg = 0; // 是否为负数

    if (value == 0)
    {
        console_put_char('0');
        return;
    }

    if (value < 0) // 转为正数打印
    {
        neg = 1;
        value = -value;
    }

    while (value > 0) // 倒序插入数组
    {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    if (neg)
        buf[i++] = '-';

    while (i--) // 倒序打印
    {
        console_put_char(buf[i]);
    }
}
