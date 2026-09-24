#ifndef BIOS_H
#define BIOS_H

void print_char(char c);
void print_string(const char* str);
char get_char();
void clear_screen();
void set_ink(int c);
void set_paper(int c);
void set_border(int c);
void beep();
void update_cursor();
void init_video();

#endif
