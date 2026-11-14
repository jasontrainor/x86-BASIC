#ifndef KEYMAP_H
#define KEYMAP_H

/*
 * Keyboard layouts. PC firmware, BIOS and UEFI alike, types as a US
 * keyboard does, whatever is printed on the keys. For a keyboard of
 * another layout, keymap_key() turns each key into the character on its
 * keycap. Which layout: the KEYBOARD= line of BOOT.CFG (US, UK, DE or
 * FR), read by keymap_init().
 *
 * Characters a machine can't show (ä, é, ¬ ...) come out as Latin-1
 * codes above 127, which the machines ignore, except 163, the pound
 * sign, which the Spectrum has.
 */
void keymap_init(void);                   // from BOOT.CFG (at start-up)
int keymap_set(const char *name);         // "UK": 1, or 0 if there's no such layout

/*
 * The character for a key: us is what a US keyboard types for it
 * (Shift and Caps Lock already counted), scan its scan code (set 1) if
 * the platform knows it, else 0, and altgr whether AltGr (right Alt) is
 * held; then us may be 0, the key being known by scan alone. Returns the
 * character, or -1 for a key that types nothing in this layout.
 */
int keymap_key(int us, int scan, int altgr);

// Whether a layout other than US is in use (for AltGr on the BIOS).
int keymap_active(void);

#endif
