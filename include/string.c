#include "string.h"

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    size_t i = 0;
    while (i < n && a[i] && b[i] && a[i] == b[i]) {
        i++;
    }
    if (i == n) return 0;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

void strcpy(char* dst, const char* src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

void strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
}

void memset(void* dst, uint8_t value, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = value;
    }
}

int atoi(const char* s) {
    int sign = 1;
    int value = 0;

    if (*s == '-') {
        sign = -1;
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return sign * value;
}