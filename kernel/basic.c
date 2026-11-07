#include "basic.h"
#include "bios.h"
#include "string.h"

#define MAX_LINES 256
#define MAX_LINE_LEN 64

struct line {
    int num;
    char text[MAX_LINE_LEN];
};

struct line program[MAX_LINES];
int num_lines = 0;

int vars[26];
char strs[26][64];

struct for_loop {
    int var_idx;
    int limit;
    int step;
    int line_index;
};
struct for_loop for_stack[16];
int for_sp = 0;

char input_buf[128];

void read_line() {
    int pos = 0;
    while (1) {
        char c = get_char();
        if (c == '\r' || c == '\n') {
            print_string("\n");
            input_buf[pos] = '\0';
            break;
        } else if (c == 8) {
            if (pos > 0) {
                pos--;
                print_char(8);
                print_char(' ');
                print_char(8);
            }
        } else if (pos < sizeof(input_buf) - 1 && c >= 32 && c < 127) {
            input_buf[pos++] = c;
            print_char(c);
        }
    }
}

int current_line = -1;
char* p;

// First error raised while executing the current statement, or 0.
// Once set, the parser stops consuming input and the statement is abandoned.
const char* error_msg = 0;

void error(const char* msg) {
    if (!error_msg) error_msg = msg;
}

void skip_space();

// If the text at p is the keyword kw (any letter case) and is not just
// the start of a longer word, consume it and return 1. Variables are a
// single letter, so a keyword followed by another letter (e.g. "RUNX")
// is never a match. Digits and symbols may follow directly ("GOTO10").
int keyword(const char* kw) {
    skip_space();
    int i = 0;
    while (kw[i]) {
        if ((p[i] & ~32) != kw[i] || !isalpha(p[i])) return 0;
        i++;
    }
    if (isalpha(p[i])) return 0;
    p += i;
    return 1;
}

int expr();
int term();
int factor();
void execute_statement();

void skip_space() {
    while (isspace(*p)) p++;
}

int factor() {
    if (error_msg) return 0;
    skip_space();
    if (isdigit(*p)) {
        int val = 0;
        while (isdigit(*p)) {
            val = val * 10 + (*p - '0');
            p++;
        }
        return val;
    } else if (*p == '-') {
        p++;
        return -factor();
    } else if (*p == '+') {
        p++;
        return factor();
    } else if (isalpha(*p)) {
        int var_idx = (*p & ~32) - 'A';
        p++;
        return vars[var_idx];
    } else if (*p == '(') {
        p++;
        int val = expr();
        skip_space();
        if (*p == ')') p++;
        else error("MISSING )");
        return val;
    }
    error("SYNTAX ERROR");
    return 0;
}

int term() {
    int val = factor();
    skip_space();
    while (!error_msg && (*p == '*' || *p == '/')) {
        char op = *p++;
        int next_val = factor();
        if (op == '*') val *= next_val;
        else if (next_val == 0) error("DIVISION BY ZERO");
        else val /= next_val;
        skip_space();
    }
    return val;
}

int expr() {
    int val = term();
    skip_space();
    while (!error_msg && (*p == '+' || *p == '-')) {
        char op = *p++;
        int next_val = term();
        if (op == '+') val += next_val;
        else if (op == '-') val -= next_val;
        skip_space();
    }
    return val;
}

// expr ( = | <> | < | <= | > | >= ) expr
int condition() {
    int v1 = expr();
    if (error_msg) return 0;
    skip_space();

    int lt = 0, eq = 0, gt = 0;   // which outcomes make the condition true
    if (*p == '=') {
        eq = 1; p++;
    } else if (*p == '<') {
        lt = 1; p++;
        if (*p == '=') { eq = 1; p++; }
        else if (*p == '>') { gt = 1; p++; }
    } else if (*p == '>') {
        gt = 1; p++;
        if (*p == '=') { eq = 1; p++; }
    } else {
        // Don't consume anything: *p may be the string terminator.
        error("COMPARISON EXPECTED");
        return 0;
    }

    int v2 = expr();
    if (v1 < v2) return lt;
    if (v1 > v2) return gt;
    return eq;
}

// Parse a variable name (a single letter A-Z, either case) and return
// its index 0-25. Anything else is an error, so callers can safely use
// the result to index vars[] and strs[].
int parse_var() {
    skip_space();
    if (!isalpha(*p)) {
        error("BAD VARIABLE NAME");
        return 0;
    }
    return (*p++ & ~32) - 'A';
}

// [LET] var = expr   or   [LET] var$ = "text"   (the LET keyword already consumed)
void assignment() {
    int var_idx = parse_var();
    if (error_msg) return;
    int is_str = 0;
    if (*p == '$') { is_str = 1; p++; }
    skip_space();
    if (*p != '=') {
        error("SYNTAX ERROR");
        return;
    }
    p++;
    skip_space();
    if (is_str) {
        if (*p != '"') {
            error("STRING EXPECTED");
            return;
        }
        p++;
        int i = 0;
        while (*p && *p != '"' && i < 63) strs[var_idx][i++] = *p++;
        strs[var_idx][i] = '\0';
        if (*p && *p != '"') { error("STRING TOO LONG"); return; }
        if (*p == '"') p++;
    } else {
        int val = expr();
        if (!error_msg) vars[var_idx] = val;
    }
}

void execute_statement() {
    skip_space();
    if (keyword("PRINT")) {
        skip_space();
        while (*p) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    print_char(*p++);
                }
                if (*p == '"') p++;
            } else if (isalpha(*p) && p[1] == '$') {
                int var_idx = (*p & ~32) - 'A';
                print_string(strs[var_idx]);
                p += 2;
            } else {
                int val = expr();
                if (error_msg) return;
                char buf[16];
                itoa(val, buf);
                print_string(buf);
            }
            skip_space();
            if (*p == ',' || *p == ';') p++; else break;
            skip_space();
        }
        print_string("\n");
    } else if (keyword("LET")) {
        assignment();
    } else if (keyword("GOTO")) {
        int target = expr();
        if (error_msg) return;
        for (int i = 0; i < num_lines; i++) {
            if (program[i].num == target) {
                current_line = i - 1;
                return;
            }
        }
        error("LINE NOT FOUND");
    } else if (keyword("IF")) {
        int cond = condition();
        if (error_msg) return;
        skip_space();
        if (keyword("THEN")) {
            if (cond) {
                execute_statement();
            } else {
                while (*p) p++;   // skip the rest of the line
            }
        } else {
            error("THEN EXPECTED");
        }
    } else if (keyword("FOR")) {
        int var_idx = parse_var();
        if (error_msg) return;
        skip_space();
        if (*p != '=') { error("SYNTAX ERROR"); return; }
        p++;
        int start_val = expr();
        skip_space();
        if (error_msg) return;
        if (!keyword("TO")) {
            error("TO EXPECTED");
            return;
        }
        int limit = expr();
        int step = 1;
        skip_space();
        if (keyword("STEP")) {
            step = expr();
        }
        if (error_msg) return;
        // Re-entering a FOR for a variable that is already looping (e.g.
        // via GOTO) restarts that loop instead of stacking a duplicate.
        for (int i = 0; i < for_sp; i++) {
            if (for_stack[i].var_idx == var_idx) { for_sp = i; break; }
        }
        if (for_sp >= 16) {
            error("TOO MANY NESTED FOR LOOPS");
            return;
        }
        vars[var_idx] = start_val;
        for_stack[for_sp].var_idx = var_idx;
        for_stack[for_sp].limit = limit;
        for_stack[for_sp].step = step;
        for_stack[for_sp].line_index = current_line;
        for_sp++;
    } else if (keyword("NEXT")) {
        int var_idx = parse_var();
        if (error_msg) return;
        // Find the matching loop; any inner loops left unfinished are dropped.
        int i = for_sp - 1;
        while (i >= 0 && for_stack[i].var_idx != var_idx) i--;
        if (i < 0) {
            error("NEXT WITHOUT FOR");
            return;
        }
        for_sp = i + 1;
        vars[var_idx] += for_stack[i].step;
        int limit = for_stack[i].limit;
        int step = for_stack[i].step;
        if ((step > 0 && vars[var_idx] <= limit) || (step < 0 && vars[var_idx] >= limit)) {
            current_line = for_stack[i].line_index;
        } else {
            for_sp--;
        }
    } else if (keyword("CLS")) {
        clear_screen();
    } else if (keyword("BEEP")) {
        beep();
        // Arguments (duration, pitch) are accepted but not used yet.
        while (*p) p++;
    } else if (keyword("INK")) {
        int c = expr();
        set_ink(c);
    } else if (keyword("PAPER")) {
        int c = expr();
        set_paper(c);
    } else if (keyword("BORDER")) {
        int c = expr();
        set_border(c);
    } else if (keyword("END")) {
        current_line = num_lines;
    } else if (isalpha(*p)) {
        // Must come last: every keyword above also starts with a letter.
        assignment();
    } else {
        error("SYNTAX ERROR");
    }
}

// Print "ERROR: <msg>", plus " IN LINE <n>" when line_num >= 0.
void print_error(const char* msg, int line_num) {
    print_string("ERROR: ");
    print_string(msg);
    if (line_num >= 0) {
        char buf[16];
        itoa(line_num, buf);
        print_string(" IN LINE ");
        print_string(buf);
    }
    print_string("\n");
}

// Run one complete statement, flagging anything left over on the line.
// Returns 1 on success; on failure prints the error and returns 0.
int run_statement() {
    error_msg = 0;
    int line = current_line;
    execute_statement();
    skip_space();
    if (!error_msg && *p) error("SYNTAX ERROR");
    if (!error_msg) return 1;

    print_error(error_msg, line >= 0 && line < num_lines ? program[line].num : -1);
    return 0;
}

void run_program() {
    for_sp = 0;
    current_line = 0;
    while (current_line >= 0 && current_line < num_lines) {
        p = program[current_line].text;
        if (!run_statement()) break;
        current_line++;
    }
}

void list_program() {
    for (int i = 0; i < num_lines; i++) {
        char buf[16];
        itoa(program[i].num, buf);
        print_string(buf);
        print_char(' ');
        print_string(program[i].text);
        print_string("\n");
    }
}

// Store or replace a program line. Returns an error message, or 0 on success.
const char* add_line(int num, const char* text) {
    if (strlen(text) > MAX_LINE_LEN - 1) {
        return "LINE TOO LONG";   // would overflow program[].text
    }
    for (int i = 0; i < num_lines; i++) {
        if (program[i].num == num) {
            strcpy(program[i].text, text);
            return 0;
        }
    }
    // Not a replacement, so a new slot is needed.
    if (num_lines >= MAX_LINES) {
        return "PROGRAM FULL";
    }
    for (int i = 0; i < num_lines; i++) {
        if (program[i].num > num) {
            for (int j = num_lines; j > i; j--) {
                program[j].num = program[j-1].num;
                strcpy(program[j].text, program[j-1].text);
            }
            program[i].num = num;
            strcpy(program[i].text, text);
            num_lines++;
            return 0;
        }
    }
    program[num_lines].num = num;
    strcpy(program[num_lines].text, text);
    num_lines++;
    return 0;
}

void basic_run() {
    while (1) {
        read_line();
        p = input_buf;
        skip_space();
        if (*p == '\0') continue;
        
        if (isdigit(*p)) {
            int num = 0;
            while (isdigit(*p) && num <= 9999) {
                num = num * 10 + (*p - '0');
                p++;
            }
            if (num < 1 || num > 9999) {
                print_error("LINE NUMBER MUST BE 1-9999", -1);
                continue;
            }
            skip_space();
            if (*p == '\0') {
                for (int i = 0; i < num_lines; i++) {
                    if (program[i].num == num) {
                        for (int j = i; j < num_lines - 1; j++) {
                            program[j].num = program[j+1].num;
                            strcpy(program[j].text, program[j+1].text);
                        }
                        num_lines--;
                        break;
                    }
                }
            } else {
                const char* err = add_line(num, p);
                if (err) print_error(err, num);
            }
        } else if (keyword("RUN")) {
            run_program();
        } else if (keyword("LIST")) {
            list_program();
        } else if (keyword("NEW")) {
            num_lines = 0;
            for_sp = 0;
            clear_screen();
        } else {
            current_line = -1;
            run_statement();
        }
    }
}
