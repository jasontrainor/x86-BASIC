/*
 * The BASIC V2 machine (the 1982 64KB home computer): its BASIC
 * (machines/c64/basic.c) on its hardware (machines/c64hw/).
 */
#include "machine.h"
#include "c64basic.h"

void machine_start(void) {
    c64basic_run();
}
