/*
 * The line editor, used for the command line and INPUT.
 *
 *   Left/Right, Home/End   move the cursor
 *   Backspace, Delete      delete before / at the cursor
 *   Up/Down                recall earlier lines (history)
 *   Enter                  finish;  Esc: give up (BREAK)
 *
 * Characters are inserted at the cursor. The line wraps across screen
 * rows and scrolls the screen if it runs off the bottom.
 */
#include "editor.h"
#include "screen.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

#define HISTORY 16
#define HIST_LEN 256
HIGH_MEM static char history[HISTORY][HIST_LEN];
static int hist_count;            // entries used
static int hist_newest;           // index of the most recent entry

static void remember(const char *line) {
    if (!*line) return;
    if (hist_count && strcmp(history[hist_newest], line) == 0) return;
    hist_newest = (hist_newest + 1) % HISTORY;
    strncpy_safe(history[hist_newest], line, HIST_LEN);
    if (hist_count < HISTORY) hist_count++;
}

// Editing state: the text, and where on screen it starts (a cell number,
// row * SCREEN_COLS + col; it moves up when the screen scrolls).
static char *text;
static int len, pos, start;

static void place(int i, int *col, int *row) {
    *col = (start + i) % SCREEN_COLS;
    *row = (start + i) / SCREEN_COLS;
}

// Scroll until character i (or the cell after the text) is on screen.
static void make_visible(int i) {
    while ((start + i) / SCREEN_COLS >= SCREEN_ROWS) {
        scroll_up();
        start -= SCREEN_COLS;
    }
}

// Redraw from character i to the end, blanking up to old_len.
static void redraw(int i, int old_len) {
    int end = len > old_len ? len : old_len;
    make_visible(end);
    for (; i < end; i++) {
        int col, row;
        place(i, &col, &row);
        put_char_at(col, row, i < len ? text[i] : ' ');
    }
}

static void move_cursor(void) {
    int col, row;
    make_visible(pos);
    place(pos, &col, &row);
    place_cursor(col, row);
}

// Replace the whole text (for history), redrawing it.
static void replace_text(const char *s, int cap) {
    int old_len = len;
    strncpy_safe(text, s, cap);
    len = strlen(text);
    pos = len;
    redraw(0, old_len);
}

int edit_line(char *buf, int cap, int use_history) {
    text = buf;
    len = strlen(buf);
    pos = len;
    // (A cursor just past the last column means the next row.)
    start = cursor_row() * SCREEN_COLS + cursor_col();
    redraw(0, 0);

    int browsing = -1;                    // history entries back, -1: none
    char draft[HIST_LEN];                 // the line being typed, while browsing
    int result = 0;
    for (;;) {
        move_cursor();
        int k = get_key();
        if (k == '\r' || k == '\n') {
            break;
        } else if (k == 27) {
            result = -1;
            break;
        } else if (k >= 32 && k < 127) {
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

    // Leave the cursor on the line after the text.
    pos = len;
    move_cursor();
    transcript(text);
    print_char('\n');
    if (result == 0 && use_history) remember(text);
    return result;
}
