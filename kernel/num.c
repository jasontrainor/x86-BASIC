#include "num.h"
#include "string.h"

/*
 * x87 helpers. The "t" constraint is the top of the FPU stack (st(0)) and
 * "u" the one below it (st(1)); GCC moves values in and out as needed.
 */

// frndint using the given rounding mode (FPU control word bits 10-11).
static double round_with(double x, unsigned short mode) {
    unsigned short saved, cw;
    __asm__ __volatile__ ("fnstcw %0" : "=m" (saved));
    cw = (saved & ~0x0C00) | mode;
    __asm__ __volatile__ ("fldcw %0" : : "m" (cw));
    __asm__ __volatile__ ("frndint" : "=t" (x) : "0" (x));
    __asm__ __volatile__ ("fldcw %0" : : "m" (saved));
    return x;
}

double num_floor(double x) { return round_with(x, 0x0400); }  // towards -inf
double num_round(double x) { return round_with(x, 0x0000); }  // to nearest
double num_abs(double x)   { return x < 0 ? -x : x; }

double num_sqrt(double x) {
    __asm__ ("fsqrt" : "=t" (x) : "0" (x));
    return x;
}

double num_sin(double x) {
    __asm__ ("fsin" : "=t" (x) : "0" (x));
    return x;
}

double num_cos(double x) {
    __asm__ ("fcos" : "=t" (x) : "0" (x));
    return x;
}

double num_tan(double x) {
    // fptan pushes 1.0 after the result; drop it.
    __asm__ ("fptan\n\tfstp %%st(0)" : "=t" (x) : "0" (x));
    return x;
}

double num_atan2(double y, double x) {
    double r;
    // fpatan: st(1) = atan(st(1) / st(0)), correct quadrant; pops.
    __asm__ ("fpatan" : "=t" (r) : "0" (x), "u" (y) : "st(1)");
    return r;
}

// fyl2x: st(1) * log2(st(0)), pops.
static double y_log2_x(double y, double x) {
    double r;
    __asm__ ("fyl2x" : "=t" (r) : "0" (x), "u" (y) : "st(1)");
    return r;
}

double num_ln(double x)    { return y_log2_x(0.69314718055994530942, x); }
double num_log10(double x) { return y_log2_x(0.30102999566398119521, x); }

double num_exp(double x) {
    // e^x = 2^t with t = x*log2(e), split into integer n and fraction f:
    // 2^f from f2xm1 (valid for |f| <= 1), then scaled by 2^n.
    double t = x * 1.44269504088896340736;
    if (!num_is_finite(t)) return t > 0 ? t : 0;
    double n = num_round(t), f = t - n, r;
    __asm__ ("f2xm1" : "=t" (r) : "0" (f));
    r += 1;
    __asm__ ("fscale" : "=t" (r) : "0" (r), "u" (n));
    return r;
}

double num_pow10(int n) {
    if (n < 0) return 1 / num_pow10(-n);
    double r = 1, b = 10;
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return r;
}

// Infinity and NaN are the only values for which x - x isn't 0.
int num_is_finite(double x) { return x - x == 0; }

void num_format(double x, char *buf) {
    char *b = buf;
    if (x == 0) { strcpy(buf, "0"); return; }   // (also -0)
    if (x < 0) { *b++ = '-'; x = -x; }

    if (x < 1e10 && num_floor(x) == x) {
        // Whole numbers: every digit (exact; doubles hold them exactly).
        char tmp[12];
        int n = 0;
        while (x >= 1) {
            double q = num_floor(x / 10);
            tmp[n++] = '0' + (int)(x - q * 10);
            x = q;
        }
        while (n) *b++ = tmp[--n];
        *b = '\0';
        return;
    }

    // Otherwise 8 significant digits: x = 0.ddddddd * 10^(e+1)
    int e = (int)num_floor(num_log10(x));
    double m = x / num_pow10(e);
    if (m >= 10) { m /= 10; e++; }        // log10 can be off by one ulp
    if (m < 1)   { m *= 10; e--; }
    int d = (int)num_round(m * 1e7);
    if (d >= 100000000) { d = 10000000; e++; }   // 9.99999999 -> 10
    char dig[9];
    for (int i = 7; i >= 0; i--) { dig[i] = '0' + d % 10; d /= 10; }
    int nd = 8;
    while (nd > 1 && dig[nd - 1] == '0') nd--;

    if (e >= 0 && e < 10) {                 // 123.456
        for (int i = 0; i <= e; i++) *b++ = i < nd ? dig[i] : '0';
        if (nd > e + 1) {
            *b++ = '.';
            for (int i = e + 1; i < nd; i++) *b++ = dig[i];
        }
    } else if (e < 0 && e >= -5) {          // 0.00123
        *b++ = '0';
        *b++ = '.';
        for (int i = -1; i > e; i--) *b++ = '0';
        for (int i = 0; i < nd; i++) *b++ = dig[i];
    } else {                                // 1.23E+15, 4E-7
        *b++ = dig[0];
        if (nd > 1) {
            *b++ = '.';
            for (int i = 1; i < nd; i++) *b++ = dig[i];
        }
        *b++ = 'E';
        *b++ = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        char tmp[6];
        int n = 0;
        do { tmp[n++] = '0' + e % 10; e /= 10; } while (e);
        while (n) *b++ = tmp[--n];
    }
    *b = '\0';
}

int num_parse(const char **sp, double *out) {
    const char *s = *sp;
    double v = 0;
    int digits = 0, exp10 = 0;
    while (isdigit(*s)) { v = v * 10 + (*s++ - '0'); digits++; }
    if (*s == '.') {
        s++;
        while (isdigit(*s)) { v = v * 10 + (*s++ - '0'); digits++; exp10--; }
    }
    if (!digits) return 0;
    // Exponent, only if digits follow (so "2E" is 2 followed by E).
    if ((*s == 'E' || *s == 'e') &&
        (isdigit(s[1]) || ((s[1] == '+' || s[1] == '-') && isdigit(s[2])))) {
        s++;
        int neg = 0, e = 0;
        if (*s == '+') s++;
        else if (*s == '-') { neg = 1; s++; }
        while (isdigit(*s)) {
            if (e < 10000) e = e * 10 + (*s - '0');
            s++;
        }
        exp10 += neg ? -e : e;
    }
    // Dividing by an exact power of ten is more accurate than
    // multiplying by an inexact 0.1^n.
    *out = exp10 < 0 ? v / num_pow10(-exp10) : v * num_pow10(exp10);
    *sp = s;
    return 1;
}
