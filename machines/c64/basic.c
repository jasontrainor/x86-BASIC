/*
 * BASIC V2: the machine's start-up screen and its prompt.
 */
#include "c64basic.h"
#include "c64.h"
#include "mem.h"

#ifdef HOST_TEST
void screen_host_hook(void);
#endif

HIGH_MEM char c64_line[96];

void c64basic_run(void) {
    c64_init();
#ifdef HOST_TEST
    screen_host_hook();
#endif
    c64_print("\r    **** X86 BASIC V2 ****\r\r 64K RAM SYSTEM  38911 BASIC BYTES FREE\r");
    c64_print("\rREADY.\r");
    for (;;) c64_input_line(c64_line, 89);
}
