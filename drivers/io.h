#ifndef IO_H
#define IO_H

#include "../include/types.h"

/*
 * 下面这几个函数是最基础的 x86 端口 I/O 操作。
 *
 * 这里访问的不是内存地址，而是 I/O 端口号：
 * - outb(port, value): 往某个硬件端口写 1 字节
 * - inb(port): 从某个硬件端口读 1 字节
 *
 * 在这个项目里常见的端口有：
 * - 0x20 / 0x21: 主 PIC（8259A）命令/数据端口
 * - 0xA0 / 0xA1: 从 PIC 命令/数据端口
 * - 0x40 / 0x43: PIT（定时器）通道 0 / 命令端口
 * - 0x60 / 0x64: 键盘/鼠标所在的 PS/2 控制器数据/状态端口
 */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    /*
     * 向 0x80 端口写一个无意义的字节，用来制造一个很短的 I/O 延时。
     * 这是老式 x86 内核里常见的写法，常用于 PIC 重编程时确保前一条 outb
     * 已经被硬件“吃掉”。
     *
     * 0x80 本身通常被当作调试/延时用途，不依赖它携带什么业务语义。
     */
    __asm__ __volatile__("outb %%al, $0x80" : : "a"(0));
}

#endif
