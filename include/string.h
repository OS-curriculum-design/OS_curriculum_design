#ifndef STRING_H
#define STRING_H

#include "types.h"

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
void strcpy(char* dst, const char* src);
void strncpy(char* dst, const char* src, size_t n);
void memset(void* dst, uint8_t value, size_t n);
int atoi(const char* s);

#endif