#ifndef MSBASIC_H
#define MSBASIC_H

/*
 * The language core shared by the 6502 BASICs of the 1977-82 home
 * computers, which are one family (msbasic.c): FP BASIC (machines/a2fp/)
 * and BASIC V2 (machines/c64/). Written for this project from how they
 * behave; none of it comes from their code.
 *
 * The core keeps the program, variables, arrays and strings, reads
 * expressions, and runs the statements the family shares (PRINT, INPUT,
 * FOR, GOSUB, DATA ...). A dialect - the BASIC of one machine - gives its
 * keywords, its own statements and functions, and the hooks below: how
 * it prints, reads keys and lines, reports errors, and what PEEK and
 * POKE reach. Each machine is built with one dialect, so the hooks are
 * plain functions the dialect defines.
 *
 * What all of them do, and the core keeps:
 *   - Numbers have 32 significant bits (about 9.6 digits), from about
 *     2.9E-39 to 1.7E+38, and print with up to 9 digits: 1/3 is
 *     .333333333, 1E9 is 1E+09, .001 is 1E-03.
 *   - Only the first two characters of a name count (SCORE is SC), and
 *     keywords are found anywhere in a line, even inside names.
 *   - Lines aren't checked as they're typed: mistakes show when they're
 *     run (?SYNTAX ERROR IN 10).
 *   - A% names are integers, A$ strings (up to 255 characters). Arrays
 *     not DIMmed have 11 elements (0-10) a subscript.
 *   - IF ... THEN: when false, the rest of the line is skipped.
 */

/* ------------------------------------------------------------------------
 * Tokens: keywords are kept in the program as these bytes. The core's
 * own come first; a dialect numbers its others from T_DIALECT. (They
 * are this project's numbers; a dialect whose files keep the original's
 * converts them.)
 * --------------------------------------------------------------------- */

enum {
    T_END = 0x80, T_FOR, T_NEXT, T_DATA, T_INPUT, T_DIM, T_READ, T_LET, T_GOTO, T_RUN,
    T_IF, T_RESTORE, T_GOSUB, T_RETURN, T_REM, T_STOP, T_ON, T_WAIT, T_LOAD, T_SAVE,
    T_DEF, T_POKE, T_PRINT, T_CONT, T_LIST, T_CLEAR, T_GET, T_NEW, T_TAB, T_TO, T_FN,
    T_SPC, T_THEN, T_NOT, T_STEP, T_AND, T_OR, T_SGN, T_INT, T_ABS, T_USR, T_FRE, T_POS,
    T_SQR, T_RND, T_LOG, T_EXP, T_COS, T_SIN, T_TAN, T_ATN, T_PEEK, T_LEN, T_STR, T_VAL,
    T_ASC, T_CHR, T_LEFT, T_RIGHT, T_MID,
    // (the core does these for the dialects that have them)
    T_ONERR, T_RESUME, T_POP, T_TRACE, T_NOTRACE, T_DEL, T_GO,
    T_DIALECT,
};

// A dialect's keywords, in the order they're looked for (the first that
// matches is used: ONERR before ON). A keyword whose token is an
// operator character ('+') is that character in the program.
struct ms_keyword {
    const char *name;
    unsigned char token;
};

// How the dialect's BASIC behaves where they differ.
struct ms_dialect {
    const struct ms_keyword *keywords;
    int n_keywords;
    char keep_spaces;        // lines keep their spaces (else they're taken out,
                             // and spaces inside keywords skipped: G O TO)
    char list_spaces;        // LIST puts spaces around keywords and operators
    char sign_space;         // positive numbers have a space for the sign (STR$ too)
    char get_waits;          // GET waits for a key (else "" or 0 if none)
    int int_min;             // the smallest integer A% holds (-32767, -32768)
    int at_token;            // (the token AT: A TO is A TO, not AT O), or 0
    char onerr;              // has ONERR GOTO (RUN clears its flag at 216)
    char abbrev;             // a keyword's letters then the next one shifted (bit 7
                             // set: pO) are the keyword
    char empty_input_ends;   // an empty line for INPUT leaves the rest unchanged
    char bitwise;            // true is -1, and AND, OR and NOT work on the bits of
                             // 16-bit integers (else true is 1 and they're logical)
    const char *redo;        // what bad INPUT says: "?REENTER"
};
extern const struct ms_dialect ms_dialect;

/* ------------------------------------------------------------------------
 * Errors: by the numbers FP BASIC's ONERR programs see (a dialect may add
 * its own, from 256)
 * --------------------------------------------------------------------- */

enum {
    E_NONE = -1, E_NEXT = 0, E_SYNTAX = 16, E_RETURN = 22, E_DATA = 42, E_QUANTITY = 53,
    E_OVERFLOW = 69, E_MEMORY = 77, E_UNDEF = 90, E_SUBSCRIPT = 107, E_REDIM = 120,
    E_DIV0 = 133, E_DIRECT = 149, E_TYPE = 163, E_LONG = 176, E_COMPLEX = 191,
    E_CONT = 214, E_FN = 224, E_REENTER = 254, E_BREAK = 255,
};

extern int ms_err;                        // the error, once there is one
// While INPUT reads from a file (INPUT#): the error a bad item is (no
// ?REENTER) and no ?EXTRA IGNORED; 0 otherwise.
extern int ms_input_quiet;
extern int ms_dos_err;                    // the dialect's own error code, if not 0
void ms_error(int e);                     // (the first error counts)

/* ------------------------------------------------------------------------
 * Values, and reading the program
 * --------------------------------------------------------------------- */

enum { MS_REAL, MS_INT, MS_STR };

struct ms_val {
    char str;
    double n;
    const unsigned char *s;
    int len;
};

// A variable or array element, to read or assign: where its value is.
struct ms_place {
    int type;
    struct ms_var *var;                   // a simple variable, or
    unsigned char *elem;                  // an array element, or
    int special;                          // one the dialect keeps (ms_special)
};

extern const unsigned char *ms_ip;        // where the statement is being read
extern int ms_cur_line;                   // the line running (offset), -1: typed
extern int ms_jumped, ms_ended;
extern int ms_tracing;

int ms_cur(void);                         // the next character, spaces skipped
int ms_expect(int c);                     // skip c, or SYNTAX ERROR (0)
int ms_at_end(void);                      // the statement's end (0 or ':')
void ms_expr(struct ms_val *v);
double ms_num_expr(void);
int ms_quantity(long max);                // a number 0-max (ILLEGAL QUANTITY)
unsigned int ms_address(void);            // -65535 to 65535, as 0-65535
long ms_line_number(void);                // a line number written there, or -1
void ms_str_arg(struct ms_val *v);        // a string expression
double ms_num_arg(void);                  // a number in brackets
int ms_place(struct ms_place *p);
void ms_get_place(const struct ms_place *p, struct ms_val *v);
void ms_set_place(const struct ms_place *p, const struct ms_val *v);
void ms_make_str(struct ms_val *v, const unsigned char *s, int len);
void ms_make_num(struct ms_val *v, double n);
unsigned char *ms_scratch(int len);       // room for a string being made
double ms_fit(double x);                  // rounded to 5 bytes (OVERFLOW if too big)
void ms_format(double x, char *buf);      // as PRINT and STR$ show it (24 bytes)
void ms_skip_statement(void);
void ms_skip_line(void);
void ms_goto_line(long num);
void ms_run_from(long num);               // RUN (num < 0: from the start)
void ms_clear_vars(void);
// INPUT's and GET's variables (ip at them), as the statements read
// them after their prompt: for INPUT# and GET#, say.
void ms_input_vars(void);
void ms_get_vars(void);
// What the program and variables take in the original's memory (FRE).
long ms_memory_used(void);

/* ------------------------------------------------------------------------
 * The program: lines in order, each the line number (2 bytes), the length
 * of what follows (2 bytes) and the crunched text, ending in 0.
 * --------------------------------------------------------------------- */

#define MS_PROG_SIZE 32768
extern unsigned char ms_prog[];
extern int ms_prog_len;
int ms_line_num(int off);
int ms_line_next(int off);
int ms_line_find(int num);                // the first line >= num
void ms_line_delete(int num);
// The line as LIST shows it, into buf (8 times the longest line): its length.
int ms_detokenize(int off, char *buf);
const char *ms_keyword_name(int token);   // 0 if it isn't a keyword

// A line typed or read: a program line (stored), or a command (run).
void ms_enter_line(const char *text);
// Run the program as it is (after RUN from the prompt), to its end.
void ms_execute(void);
// A line of the program stored as it's kept (crunched, ending in 0; len
// counts the 0); 0 if there's no room.
int ms_store_line(int num, const unsigned char *text, int len);
// Go on from the program's first line, keeping the variables (a program
// LOADing another, as BASIC V2 does).
void ms_chain(void);
// PRINT's items (ip at them), as the statement prints them.
void ms_print_items(void);
// Run lines given as text (LOAD of a listing): NEW, then each line.
void ms_enter_text(const char *text, int len);

/* ------------------------------------------------------------------------
 * What the dialect gives (its hooks)
 * --------------------------------------------------------------------- */

void ms_out(int c);                       // print a character
int ms_column(void);                      // where the cursor is (POS)
void ms_print_number(double x);           // PRINT of a number
void ms_print_comma(void);                // PRINT's ","
void ms_print_tab(int n, int spc);        // TAB(n) or SPC(n)
int ms_peek(unsigned int addr);
void ms_poke(unsigned int addr, int value);
int ms_break(void);                       // the break key was pressed (taken)
// A line for INPUT into buf: after the prompt (prompt, plen; more: the
// line before wasn't enough). 0 if there's an error (it's set).
int ms_input_line(const unsigned char *prompt, int plen, int more, char *buf, int cap);
// GET's key: a character, -1 if none, or -2 if there's an error (set).
int ms_get_key(void);
// A statement or function of the dialect's own (ip after its token):
// 0 if it isn't one.
int ms_statement(int token);
int ms_function(int token, struct ms_val *v);
double ms_rnd(double x);
double ms_fre(void);
// The error (ms_err, or BREAK) at line (or -1), printed.
void ms_report(int line);
void ms_program_ended(void);              // after a program or command stops
// A variable the dialect keeps itself (TI, say): an id for it, or 0; and
// reading and assigning it.
int ms_special(const char name[2], int type);
void ms_special_get(int id, struct ms_val *v);
void ms_special_set(int id, const struct ms_val *v);
void ms_list_line(const char *text);      // a line LIST shows
void ms_cleared(void);                    // the variables were cleared (CLR, RUN ...)
void ms_wait_idle(void);                  // WAIT is waiting
// A character of a line being crunched, as the program keeps it
// (in_string: in quotes, REM or DATA).
int ms_line_char(int c, int in_string);

#endif
