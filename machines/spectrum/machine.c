/*
 * The Spectrum-style machine: its screen, editor and BASIC.
 */
#include "machine.h"
#include "screen.h"
#include "basic.h"
#include "fat.h"

void machine_start(void) {
    fat_folder("SPECTRUM");   // its listings and saved files
    screen_init();   // Spectrum defaults: black ink on white paper, white border
    basic_run();     // (which shows the banner at the bottom, as on the Spectrum)
}
