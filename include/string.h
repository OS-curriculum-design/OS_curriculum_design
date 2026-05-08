#ifndef STRING_H
#define STRING_H

#include "types.h"

/*
 * 极简字符串/内存工具
 * ====================
 * freestanding 内核环境没有 libc，这里补上项目里真正会用到的几个基础函数。
 */

/* 计算以 '\0' 结尾字符串的长度。 */
size_t strlen(const char* s);
/* 按 C 字符串语义比较两个字符串。 */
int strcmp(const char* a, const char* b);
/* 最多比较前 n 个字符。 */
int strncmp(const char* a, const char* b, size_t n);
/* 把 src 完整复制到 dst，包含结尾 '\0'。 */
void strcpy(char* dst, const char* src);
/* 用固定字节值填充一段连续内存。 */
void memset(void* dst, uint8_t value, size_t n);

#endif
