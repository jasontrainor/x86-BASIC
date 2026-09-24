#ifndef STRING_H
#define STRING_H

typedef unsigned int size_t;

int strlen(const char* str);
void strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
int isdigit(char c);
int isspace(char c);
int isalpha(char c);
int atoi(const char* str);
void itoa(int val, char* buf);

#endif
