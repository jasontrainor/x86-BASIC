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

    if (val < 0) {
        sign = 1;
        val = -val;
    }
    if (val == 0) {
        temp[i++] = '0';
    } else {
        while (val > 0) {
            temp[i++] = (val % 10) + '0';
            val /= 10;
        }
    }
    if (sign) temp[i++] = '-';
    
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}
