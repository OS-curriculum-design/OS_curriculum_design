/*
 * PS/2 键盘驱动
 * ==============
 * 键盘 IRQ1 到来时，处理函数从 0x60 端口读取扫描码并翻译成 ASCII，
 * 然后塞进环形缓冲区，供 Shell 在主循环里异步读取。
 */
#include "keyboard.h"
#include "io.h"
#include "../interrupt/interrupts.h"

/* 这是一个很小的环形缓冲区，用来暂存中断里收到的字符。 */
#define KEYBOARD_BUFFER_SIZE 128

/* 当前仅跟踪 Shift 键状态，不处理 Caps Lock / Ctrl / Alt 等组合键。 */
static int shift_pressed = 0;
static volatile char key_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint8_t key_head = 0;
static volatile uint8_t key_tail = 0;

/*
 * 扫描码（scancode）到 ASCII 的映射表。
 *
 * 键盘中断给我们的不是字符，而是“按键编号”。
 * 例如：
 * - 主键盘区数字 1 的按下扫描码是 0x02
 * - Enter 的按下扫描码是 0x1C
 * - 左 Shift 的按下扫描码是 0x2A
 *
 * 这里的 normal_map / shift_map 就是把扫描码翻译成最终字符。
 */
static const char normal_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0
};

static const char shift_map[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0
};

static void keyboard_buffer_push(char c) {
    /* 环形缓冲：head 写入，tail 读取。满了就直接丢弃新字符。 */
    uint8_t next = (uint8_t)((key_head + 1) % KEYBOARD_BUFFER_SIZE);
    if (next == key_tail) return;

    key_buffer[key_head] = c;
    key_head = next;
}

static void keyboard_irq_handler(InterruptFrame* frame) {
    (void)frame;

    /*
     * 键盘数据端口是 0x60。
     * 当 IRQ1 到来时，通常表示这里有一个新的扫描码可以读。
     */
    uint8_t scancode = inb(0x60);

    /* 0x2A / 0x36：左/右 Shift 按下。 */
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }

    /* 0xAA / 0xB6：左/右 Shift 松开。高位 1 通常表示“释放码”。 */
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return;
    }

    /* bit7=1 表示这是按键释放事件，我们这里只处理按下事件。 */
    if (scancode & 0x80) return;

    /* 当前实现只处理最常见的一字节扫描码。 */
    char c = shift_pressed ? shift_map[scancode] : normal_map[scancode];
    if (!c) return;

    keyboard_buffer_push(c);
}

void keyboard_init(void) {
    shift_pressed = 0;
    key_head = 0;
    key_tail = 0;

    /* 键盘控制器挂在 IRQ1。 */
    irq_register_handler(1, keyboard_irq_handler);
}

int keyboard_has_char(void) {
    return key_head != key_tail;
}

int keyboard_read_char(char* out) {
    if (key_head == key_tail) {
        return 0;
    }

    *out = key_buffer[key_tail];
    key_tail = (uint8_t)((key_tail + 1) % KEYBOARD_BUFFER_SIZE);
    return 1;
}
