#ifndef STRING_H
#define STRING_H

typedef __SIZE_TYPE__ size_t;

int strlen(const char* str);
void strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
int isdigit(char c);
int isspace(char c);
int isalpha(char c);
int isalnum(char c);
char toupper(char c);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int atoi(const char* str);
void itoa(int val, char* buf);

#endif
