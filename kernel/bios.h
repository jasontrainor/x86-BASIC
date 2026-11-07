#ifndef BIOS_H
#define BIOS_H

void print_char(char c);
void print_string(const char* str);
char get_char();            // waits for a key
int poll_key();             // next key, or -1 if none (doesn't wait)
int key_waiting();          // 1 if a key has been pressed (doesn't take it)
int check_break();          // 1 if Esc or Ctrl+C has been pressed
unsigned long timer_ms();   // milliseconds since some point (coarse)
void tone(int freq, int ms);
void set_cursor(int col, int row);
int cursor_col();
int screen_cols();
int screen_rows();
void clear_screen();
void set_ink(int c);
void set_paper(int c);
void set_border(int c);
void update_cursor();
void init_video();

#endif
