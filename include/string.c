/*
 * 极简字符串/内存工具实现
 * =======================
 * 在 freestanding 内核环境里没有 libc，因此这里自己提供最常用的几个函数。
 */
#include "string.h"

size_t strlen(const char* s) {
    size_t n = 0;
    /* 一直数到遇到字符串结束符为止。 */
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    /* 找到第一个不同字符，或某一方先结束。 */
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    size_t i = 0;
    /* 最多比较前 n 个字符。 */
    while (i < n && a[i] && b[i] && a[i] == b[i]) {
        i++;
    }
    if (i == n) return 0;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

void strcpy(char* dst, const char* src) {
    /* 复制正文后，别忘了把结尾的 '\0' 也补上。 */
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

void memset(void* dst, uint8_t value, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    /* 最朴素的逐字节填充版本，简单可靠。 */
    for (size_t i = 0; i < n; i++) {
        p[i] = value;
    }
}
