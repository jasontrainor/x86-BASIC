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

/*
 * The Spectrum's numbers: an exponent byte and a 32-bit mantissa (the
 * top bit, always 1, standing for the sign). So the significand of a
 * double (52 bits after the leading 1) is rounded to 31 bits after it,
 * to nearest, and the exponent kept in the Spectrum's range: values are
 * 0.1xxx (binary) * 2^(e - 128) for e from 1 to 255.
 */
int num_spectrum(double *x) {
    union { double d; unsigned long long u; } v;
    v.d = *x;
    unsigned long long sign = v.u & (1ULL << 63), mag = v.u & ~(1ULL << 63);
    if ((int)(mag >> 52) == 0x7FF) return 0;              // infinity, NaN
    mag = (mag + (1ULL << 20)) & ~((1ULL << 21) - 1);    // (a carry goes into the exponent)
    int e = (int)(mag >> 52) - 1023;                      // x = 1.f * 2^e
    if (e > 126) return 0;
    if (e < -128) {
        *x = 0;                                          // (no -0 either)
        return 1;
    }
    v.u = sign | mag;
    *x = v.d;
    return 1;
}

// Infinity and NaN are the only values for which x - x isn't 0.
int num_is_finite(double x) { return x - x == 0; }

/*
 * Print a number the way the Spectrum does: rounded to 8 significant
 * digits, without trailing zeros. With b digits before the decimal point,
 * it's written out in full for b from -4 to 8, and in E-format
 * otherwise (1E+8, 1.5E-7). As on the Spectrum, a single 0 is written
 * before the point only when b is 0: 0.5, but .05 and .00123.
 */
void num_format(double x, char *buf) {
    char *o = buf;
    if (x == 0) { strcpy(buf, "0"); return; }   // (also -0)
    if (x < 0) { *o++ = '-'; x = -x; }

    // x = 0.d1d2...d8 * 10^b
    int b = (int)num_floor(num_log10(x)) + 1;
    double m = x / num_pow10(b - 1);
    if (m >= 10) { m /= 10; b++; }        // log10 can be off by one ulp
    if (m < 1)   { m *= 10; b--; }
    double r = num_round(m * 1e7);
    if (r >= 1e8) { r = 1e7; b++; }       // 9.99999999 -> 10
    int d = (int)r;
    char dig[8];
    for (int i = 7; i >= 0; i--) { dig[i] = '0' + d % 10; d /= 10; }
    int nd = 8;
    while (nd > 1 && dig[nd - 1] == '0') nd--;

    if (b >= -4 && b <= 8) {
        if (b == 0) *o++ = '0';
        for (int i = 0; i < b; i++) *o++ = i < nd ? dig[i] : '0';
        if (nd > b) {
            *o++ = '.';
            for (int i = b; i < 0; i++) *o++ = '0';
            for (int i = b > 0 ? b : 0; i < nd; i++) *o++ = dig[i];
        }
    } else {
        *o++ = dig[0];
        if (nd > 1) {
            *o++ = '.';
            for (int i = 1; i < nd; i++) *o++ = dig[i];
        }
        int e = b - 1;
        *o++ = 'E';
        *o++ = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        char tmp[6];
        int n = 0;
        do { tmp[n++] = '0' + e % 10; e /= 10; } while (e);
        while (n) *o++ = tmp[--n];
    }
    *o = '\0';
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
