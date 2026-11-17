// Chapter 40: single-precision floating point with the F extension.
//
// The same source is also compiled for plain RV32I, where every operation is
// a soft-float libcall.  That keeps the e2e corpus honest: enabling F must
// change the instruction selection, not the source-level behaviour.

float fadd(float a, float b) { return a + b; }
float fsub(float a, float b) { return a - b; }
float fmul(float a, float b) { return a * b; }
float fdiv(float a, float b) { return a / b; }

float fabs_neg(float x) { return __builtin_fabsf(-x); }

int flt(float a, float b) { return a < b; }
int feq(float a, float b) { return a == b; }
int fne(float a, float b) { return a != b; }

int ftoi(float x) { return (int)x; }
unsigned ftou(float x) { return (unsigned)x; }
float itof(int x) { return (float)x; }
float utof(unsigned x) { return (float)x; }

float fsum(const float *p, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

void fstore(float *p, float v) {
  p[0] = v;
  p[1] = v * 2.0f;
  p[2] = v - 1.5f;
}

float fselect(float a, float b, int c) { return c ? a : b; }
