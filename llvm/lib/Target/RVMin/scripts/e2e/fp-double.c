// Chapter 41: double-precision floating point with the D extension.
//
// The same source is compiled for plain RV32I/F, where double operations are
// soft-float libcalls.  Enabling D must change instruction selection, not the
// source-level behaviour.

double dadd(double a, double b) { return a + b; }
double dsub(double a, double b) { return a - b; }
double dmul(double a, double b) { return a * b; }
double ddiv(double a, double b) { return a / b; }

double dabs_neg(double x) { return __builtin_fabs(-x); }
double dsqrt(double x) { return __builtin_sqrt(x); }

int dlt(double a, double b) { return a < b; }
int deq(double a, double b) { return a == b; }
int dne(double a, double b) { return a != b; }

int dtoi(double x) { return (int)x; }
unsigned dtou(double x) { return (unsigned)x; }
double itod(int x) { return (double)x; }
double utod(unsigned x) { return (double)x; }

float dtof(double x) { return (float)x; }
double ftod(float x) { return (double)x; }

double dsum(const double *p, int n) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

void dstore(double *p, double v) {
  p[0] = v;
  p[1] = v * 2.0;
  p[2] = v - 1.5;
}

double dselect(double a, double b, int c) { return c ? a : b; }
