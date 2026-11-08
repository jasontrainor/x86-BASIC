#ifndef MACHINE_H
#define MACHINE_H

/*
 * What each machine (machines/NAME/) provides to the shared kernel code
 * in core/: the computer it is - its screen, keyboard handling and BASIC.
 * core/kernel.c calls machine_start() once the platform is ready; it
 * doesn't return.
 */
void machine_start(void);

#endif
