/*
 * Integer BASIC on the 1977 colour machine: low-resolution graphics,
 * the text window, paddles (machines/intbasic/, machines/apple/).
 */
#include "machine.h"
#include "intbasic.h"

void machine_start(void) {
    intbasic_run(INT_1977);
}
