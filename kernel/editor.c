/*
 * The line editor, used for the command line and INPUT. It edits in the
 * lower screen, from its print position, which must be in use
 * (use_lower(1)).
 *
 *   Left/Right, Home/End   move the cursor
 *   Backspace, Delete      delete before / at the cursor
 *   Up/Down                recall earlier lines (history)
 *   Enter                  finish;  Esc: give up (BREAK)
 *
 * Characters are inserted at the cursor. The line wraps across rows, and
 * the lower screen grows upwards to hold it.
 */
#include "editor.h"
#include "screen.h"
#include "platform.h"
#include "string.h"
#include "mem.h"
#include "memory.h"

#define HISTORY 16
#define HIST_LEN 1024
HIGH_MEM static char history[HISTORY][HIST_LEN];
static int hist_count;            // entries used
static int hist_newest;           // index of the most recent entry
HIGH_MEM static char draft[HIST_LEN];     // the line being typed, while browsing

static void remember(const char *line) {
    if (!*line) return;
    if (hist_count && strcmp(history[hist_newest], line) == 0) return;
    hist_newest = (hist_newest + 1) % HISTORY;
    strncpy_safe(history[hist_newest], line, HIST_LEN);
    if (hist_count < HISTORY) hist_count++;
}

// Editing state: the text, and where in the lower screen it starts (a
// cell number, row * SCREEN_COLS + col).
static char *text;
static int len, pos, start;

static void place(int i, int *col, int *row) {
    *col = (start + i) % SCREEN_COLS;
    *row = (start + i) / SCREEN_COLS;
}

// Make the lower screen big enough for character i (or the cell after
// the text).
static void make_room(int i) {
    lower_need_rows((start + i) / SCREEN_COLS + 1);
}

// Redraw from character i to the end, blanking up to old_len.
static void redraw(int i, int old_len) {
    int end = len > old_len ? len : old_len;
    make_room(end);
    for (; i < end; i++) {
        int col, row;
        place(i, &col, &row);
        lower_put(col, row, i < len ? text[i] : ' ');
    }
}

static void move_cursor(void) {
    int col, row;
    make_room(pos);
    place(pos, &col, &row);
    set_cursor(col, row);
}

// Replace the whole text (for history), redrawing it.
static void replace_text(const char *s, int cap) {
    int old_len = len;
    strncpy_safe(text, s, cap);
    len = strlen(text);
    pos = len;
    redraw(0, old_len);
}

static int error_mark;

void edit_show_error(void) { error_mark = 1; }

// Show a ? at the cursor, the text after it moved along to make room.
static void show_mark(void) {
    make_room(len + 1);
    for (int i = pos; i < len; i++) {
        int col, row;
        place(i + 1, &col, &row);
        lower_put(col, row, text[i]);
    }
    int col, row;
    place(pos, &col, &row);
    lower_put(col, row, '?');
}

int edit_line(char *buf, int cap, int use_history, int cursor) {
    text = buf;
    start = cursor_row() * SCREEN_COLS + cursor_col();
    // It must fit on the screen, with room for the cursor after it.
    int room = SCREEN_ROWS * SCREEN_COLS - start - 1;
    if (cap > room + 1) cap = room + 1;
    buf[cap - 1] = '\0';
    len = strlen(buf);
    pos = cursor >= 0 && cursor < len ? cursor : len;
    redraw(0, 0);
    int marked = error_mark;
    error_mark = 0;
    if (marked) show_mark();

    int browsing = -1;                    // history entries back, -1: none
    int result = 0;
    for (;;) {
        move_cursor();
        int k = get_key();
        if (marked) {                     // the first key takes the ? away
            marked = 0;
            redraw(pos, len + 1);
        }
        if (k == '\r' || k == '\n') {
            break;
        } else if (k == 27) {
            result = -1;
            break;
        } else if (k >= 32 && k < 127) {
            if ((mem_peek(SYS_FLAGS2) & 8) && k >= 'a' && k <= 'z')
                k -= 'a' - 'A';               // CAPS LOCK (POKE 23658,8)
            if (len < cap - 1) {
                memmove(text + pos + 1, text + pos, len - pos + 1);
                text[pos] = k;
                len++;
                redraw(pos, len);
                pos++;
            }
        } else if (k == 8) {
            if (pos > 0) {
                memmove(text + pos - 1, text + pos, len - pos + 1);
                pos--;
                len--;
                redraw(pos, len + 1);
            }
        } else if (k == KEY_DELETE) {
            if (pos < len) {
                memmove(text + pos, text + pos + 1, len - pos);
                len--;
                redraw(pos, len + 1);
            }
        } else if (k == KEY_LEFT) {
            if (pos > 0) pos--;
        } else if (k == KEY_RIGHT) {
            if (pos < len) pos++;
        } else if (k == KEY_HOME) {
            pos = 0;
        } else if (k == KEY_END) {
            pos = len;
        } else if (k == KEY_UP && use_history && browsing + 1 < hist_count) {
            if (browsing < 0) strncpy_safe(draft, text, HIST_LEN);
            browsing++;
            replace_text(history[(hist_newest - browsing + HISTORY) % HISTORY], cap);
        } else if (k == KEY_DOWN && use_history && browsing >= 0) {
            browsing--;
            replace_text(browsing < 0 ? draft
                         : history[(hist_newest - browsing + HISTORY) % HISTORY], cap);
        }
    }

    // Leave the print position just after the text.
    pos = len;
    move_cursor();
    transcript(text);
    transcript("\n");
    if (result == 0 && use_history) remember(text);
    return result;
}
