#ifndef INTBASIC_H
#define INTBASIC_H

/*
 * Integer BASIC (intbasic.c): the 16-bit integer BASIC of the 1976
 * terminal computer and of the 1977 colour one that followed it,
 * written for this project from how they behave. Programs written for
 * them can be typed in (or LOADed as listings) and give the same
 * results.
 */
enum {
    INT_1976,          // the terminal: no graphics, no cursor moves
    INT_1977,          // colour graphics, the text window, paddles
};

void intbasic_run(int setting);   // doesn't return

#endif
