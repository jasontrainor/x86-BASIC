#include "string.h"

int strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

void strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n--) {
        if (*s1 != *s2) return *(const unsigned char*)s1 - *(const unsigned char*)s2;
        if (!*s1) break;
        s1++;
        s2++;
    }
    return 0;
}

int isdigit(char c) {
    return (c >= '0' && c <= '9');
}

int isspace(char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

int isalpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(char c) {
    return isalpha(c) || isdigit(c);
}

char toupper(char c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) return *x - *y;
    }
    return 0;
}

/*
 * GCC may emit calls to memcpy, memmove and memset itself (for struct
 * copies, for example), so a freestanding kernel must provide them. The
 * host test build uses the C library's, and the 16-bit BIOS kernel has
 * its own in bios_string.asm.
 */
#if !defined(HOST_TEST) && !defined(BIOS_KERNEL)
void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    unsigned char *d = dest;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}
#endif

int atoi(const char* str) {
    int res = 0;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    }
    while (isdigit(*str)) {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}

void itoa(int val, char* buf) {
    char temp[16];
    int i = 0;
    int sign = 0;
    // Work in unsigned so that -2147483648 (which has no positive int
    // counterpart) converts correctly.
    unsigned int u = (unsigned int)val;

    if (val < 0) {
        sign = 1;
        u = 0u - u;
    }
    do {
        temp[i++] = (u % 10) + '0';
        u /= 10;
    } while (u > 0);
    if (sign) temp[i++] = '-';
    
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}
