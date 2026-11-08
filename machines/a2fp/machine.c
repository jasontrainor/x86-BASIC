/*
 * FP BASIC on the later colour machine (lower case, 80 columns, double
 * resolution): machines/a2fp/fpbasic.c, on machines/apple/.
 */
#include "machine.h"
#include "fpbasic.h"

void machine_start(void) {
    fpbasic_run();
}
