#ifndef NUM_H
#define NUM_H

/*
 * BASIC numbers are doubles, worked on by the x87 FPU, which every PC
 * CPU since the 486 has. The maths functions use x87 instructions
 * directly, so the same code works in the 16-bit BIOS kernel, the 64-bit
 * UEFI kernel and the host test build, without a C library.
 */

double num_floor(double x);
double num_round(double x);          // to nearest (ties to even)
double num_abs(double x);
double num_sqrt(double x);
double num_sin(double x);
double num_cos(double x);
double num_tan(double x);
double num_atan2(double y, double x);
double num_ln(double x);             // x > 0
double num_log10(double x);          // x > 0
double num_exp(double x);
double num_pow10(int n);
int    num_is_finite(double x);

// Format x the way BASIC prints it (at most 8 significant digits,
// exponent form for very large or small numbers). buf: 24 bytes.
void num_format(double x, char *buf);

// Parse a decimal number ("12", "3.5", ".5", "1E-3") at *s, advancing
// *s past it. Returns 0 if there is no number there.
int num_parse(const char **s, double *out);

#endif
