#ifndef C64BASIC_H
#define C64BASIC_H

#include "msbasic.h"

// BASIC V2 (basic.c), on the machine's hardware. Doesn't return.
void c64basic_run(void);

// Its own keywords' tokens (after the language core's).
enum {
    T_INPUTN = T_DIALECT, T_VERIFY, T_PRINTN, T_CMD, T_SYS, T_OPEN, T_CLOSE, T_GET64,
    T_PI = 0xFF,                          // (pi: a constant, its own character)
};

// The token this BASIC keeps for byte c (128-255) of one of the
// original's lines, and the original's byte for one of its tokens.
int c64_token(int c);
int c64_original_token(int t);

#endif
