#ifndef EDITOR_H
#define EDITOR_H

// Let the user edit the text in buf (which may start non-empty) at the
// cursor, up to cap-1 characters. Returns 0 when Enter is pressed, or
// -1 for Esc. With use_history, Up/Down recall earlier lines and the
// line is added to the history.
int edit_line(char *buf, int cap, int use_history);

#endif
