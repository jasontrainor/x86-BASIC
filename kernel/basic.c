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

int expr();
int term();
int factor();
void execute_statement();

void skip_space() {
    while (isspace(*p)) p++;
}

int factor() {
    skip_space();
    if (isdigit(*p) || (*p == '-' && isdigit(p[1]))) {
        int sign = 1;
        if (*p == '-') { sign = -1; p++; }
        int val = 0;
        while (isdigit(*p)) {
            val = val * 10 + (*p - '0');
            p++;
        }
        return val * sign;
    } else if (isalpha(*p)) {
        int var_idx = (*p & ~32) - 'A';
        p++;
        return vars[var_idx];
    } else if (*p == '(') {
        p++;
        int val = expr();
        skip_space();
        if (*p == ')') p++;
        return val;
    }
    return 0;
}

int term() {
    int val = factor();
    skip_space();
    while (*p == '*' || *p == '/') {
        char op = *p++;
        int next_val = factor();
        if (op == '*') val *= next_val;
        else if (op == '/' && next_val != 0) val /= next_val;
        skip_space();
    }
    return val;
}

int expr() {
    int val = term();
    skip_space();
    while (*p == '+' || *p == '-') {
        char op = *p++;
        int next_val = term();
        if (op == '+') val += next_val;
        else if (op == '-') val -= next_val;
        skip_space();
    }
    return val;
}

int condition() {
    int v1 = expr();
    skip_space();
    char op = *p++;
    int has_equal = 0;
    if (*p == '=') { has_equal = 1; p++; }
    else if (*p == '>' || *p == '<') {
        if (p[0] == '=') { has_equal = 1; p++; }
    }
    
    int v2 = expr();
    
    if (op == '=') return v1 == v2;
    if (op == '>') return has_equal ? v1 >= v2 : v1 > v2;
    if (op == '<') return has_equal ? v1 <= v2 : v1 < v2;
    return 0;
}

void execute_statement() {
    skip_space();
    if (strncmp(p, "PRINT", 5) == 0 || strncmp(p, "print", 5) == 0) {
        p += 5;
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
                char buf[16];
                itoa(val, buf);
                print_string(buf);
            }
            skip_space();
            if (*p == ',' || *p == ';') p++; else break;
            skip_space();
        }
        print_string("\n");
    } else if (strncmp(p, "LET", 3) == 0 || strncmp(p, "let", 3) == 0 || isalpha(*p)) {
        if (strncmp(p, "LET", 3) == 0 || strncmp(p, "let", 3) == 0) {
            p += 3;
            skip_space();
        }
        int var_idx = (*p & ~32) - 'A';
        p++;
        int is_str = 0;
        if (*p == '$') { is_str = 1; p++; }
        skip_space();
        if (*p == '=') {
            p++;
            skip_space();
            if (is_str) {
                if (*p == '"') {
                    p++;
                    int i = 0;
                    while (*p && *p != '"' && i < 63) strs[var_idx][i++] = *p++;
                    strs[var_idx][i] = '\0';
                    if (*p == '"') p++;
                }
            } else {
                vars[var_idx] = expr();
            }
        }
    } else if (strncmp(p, "GOTO", 4) == 0 || strncmp(p, "goto", 4) == 0) {
        p += 4;
        int target = expr();
        for (int i = 0; i < num_lines; i++) {
            if (program[i].num == target) {
                current_line = i - 1; 
                return;
            }
        }
        print_string("ERROR: LINE NOT FOUND\n");
    } else if (strncmp(p, "IF", 2) == 0 || strncmp(p, "if", 2) == 0) {
        p += 2;
        int cond = condition();
        skip_space();
        if (strncmp(p, "THEN", 4) == 0 || strncmp(p, "then", 4) == 0) {
            p += 4;
            if (cond) {
                execute_statement();
            }
        }
    } else if (strncmp(p, "FOR", 3) == 0 || strncmp(p, "for", 3) == 0) {
        p += 3;
        skip_space();
        int var_idx = (*p & ~32) - 'A';
        p++;
        skip_space();
        if (*p == '=') p++;
        int start_val = expr();
        vars[var_idx] = start_val;
        skip_space();
        if (strncmp(p, "TO", 2) == 0 || strncmp(p, "to", 2) == 0) {
            p += 2;
            int limit = expr();
            int step = 1;
            skip_space();
            if (strncmp(p, "STEP", 4) == 0 || strncmp(p, "step", 4) == 0) {
                p += 4;
                step = expr();
            }
            if (for_sp < 16) {
                for_stack[for_sp].var_idx = var_idx;
                for_stack[for_sp].limit = limit;
                for_stack[for_sp].step = step;
                for_stack[for_sp].line_index = current_line;
                for_sp++;
            }
        }
    } else if (strncmp(p, "NEXT", 4) == 0 || strncmp(p, "next", 4) == 0) {
        p += 4;
        skip_space();
        int var_idx = (*p & ~32) - 'A';
        if (for_sp > 0 && for_stack[for_sp-1].var_idx == var_idx) {
            vars[var_idx] += for_stack[for_sp-1].step;
            int limit = for_stack[for_sp-1].limit;
            int step = for_stack[for_sp-1].step;
            if ((step > 0 && vars[var_idx] <= limit) || (step < 0 && vars[var_idx] >= limit)) {
                current_line = for_stack[for_sp-1].line_index;
            } else {
                for_sp--;
            }
        }
    } else if (strncmp(p, "CLS", 3) == 0 || strncmp(p, "cls", 3) == 0) {
        p += 3;
        clear_screen();
    } else if (strncmp(p, "BEEP", 4) == 0 || strncmp(p, "beep", 4) == 0) {
        p += 4;
        beep();
        // Skip args if user added them, as our beep is basic
        while (*p && *p != ':') p++;
    } else if (strncmp(p, "INK", 3) == 0 || strncmp(p, "ink", 3) == 0) {
        p += 3;
        int c = expr();
        set_ink(c);
    } else if (strncmp(p, "PAPER", 5) == 0 || strncmp(p, "paper", 5) == 0) {
        p += 5;
        int c = expr();
        set_paper(c);
    } else if (strncmp(p, "BORDER", 6) == 0 || strncmp(p, "border", 6) == 0) {
        p += 6;
        int c = expr();
        set_border(c);
    } else if (strncmp(p, "END", 3) == 0 || strncmp(p, "end", 3) == 0) {
        current_line = num_lines;
    }
}

void run_program() {
    for_sp = 0;
    current_line = 0;
    while (current_line >= 0 && current_line < num_lines) {
        p = program[current_line].text;
        execute_statement();
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

void add_line(int num, const char* text) {
    for (int i = 0; i < num_lines; i++) {
        if (program[i].num == num) {
            strcpy(program[i].text, text);
            return;
        } else if (program[i].num > num) {
            for (int j = num_lines; j > i; j--) {
                program[j].num = program[j-1].num;
                strcpy(program[j].text, program[j-1].text);
            }
            program[i].num = num;
            strcpy(program[i].text, text);
            num_lines++;
            return;
        }
    }
    program[num_lines].num = num;
    strcpy(program[num_lines].text, text);
    num_lines++;
}

void basic_run() {
    while (1) {
        read_line();
        p = input_buf;
        skip_space();
        if (*p == '\0') continue;
        
        if (isdigit(*p)) {
            int num = 0;
            while (isdigit(*p)) {
                num = num * 10 + (*p - '0');
                p++;
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
                add_line(num, p);
            }
        } else if (strncmp(p, "RUN", 3) == 0 || strncmp(p, "run", 3) == 0) {
            run_program();
        } else if (strncmp(p, "LIST", 4) == 0 || strncmp(p, "list", 4) == 0) {
            list_program();
        } else if (strncmp(p, "NEW", 3) == 0 || strncmp(p, "new", 3) == 0) {
            num_lines = 0;
            for_sp = 0;
            clear_screen();
        } else {
            current_line = -1;
            execute_statement();
        }
    }
}
