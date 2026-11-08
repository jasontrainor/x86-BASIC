/*
 * Integer BASIC on the 1976 terminal computer: 40x24 upper case, at 60
 * characters a second (Tab: as fast as it goes), no graphics
 * (machines/intbasic/, machines/apple/).
 */
#include "machine.h"
#include "intbasic.h"

void machine_start(void) {
    intbasic_run(INT_1976);
}
