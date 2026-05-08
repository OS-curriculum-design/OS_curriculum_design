#ifndef TYPES_H
#define TYPES_H

/*
 * 基础类型定义
 * ============
 * freestanding 内核不依赖标准头文件，这里自己定义一组最常用的定长整数类型。
 */

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef signed char    int8_t;
typedef signed short   int16_t;
typedef signed int     int32_t;
/* 32 位环境下 size_t 也按 unsigned int 处理。 */
typedef unsigned int   size_t;

#endif
