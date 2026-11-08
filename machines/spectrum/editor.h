#ifndef EDITOR_H
#define EDITOR_H

// Let the user edit the text in buf (which may start non-empty), in the
// lower screen from its print position, up to cap-1 characters. The
// cursor starts at character cursor (-1: at the end). Returns 0 when
// Enter is pressed, or -1 for Esc. With use_history, Up/Down recall
// earlier lines and the line is added to the history.
int edit_line(char *buf, int cap, int use_history, int cursor);

// The next edit_line shows a flashing ? at the cursor (where the line
// stops making sense) until a key is pressed.
void edit_show_error(void);

#endif
